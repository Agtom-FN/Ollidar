# Spike S1 — COIN-D6 packet parser + macOS serial bring-up tool

**Status:** code complete, fully unit-tested, **awaiting hardware for the live half of the exit criteria.**
No COIN-D6 was present on this machine (`/dev/tty.usbserial*` and `/dev/cu.usbserial*` are both empty), so the
"live polar plot / checksum pass > 99.5 % / start-stop ACKs verified" criteria are *ready to run*, not yet met.
Everything needed to close them is a single command once the device is plugged in (§6).

| S1 exit criterion | State |
| --- | --- |
| Live polar plot | Implemented + verified against a synthetic stream; needs the device |
| Checksum pass rate > 99.5 % | Checksum implemented against vendor ground truth; 100 % on synthetic data; needs the device |
| Start/stop ACKs verified | Frames implemented + unit-tested from the spec; needs the device |

---

## 1. What was built

```
spikes/s1-d6-parser/
  d6/d6_parser.h         portable parser API (C++17, stdlib only, no platform code)
  d6/d6_parser.cpp       resync + framing + checksum + angle interpolation + stats
  d6/commands.h          start/stop commands, ACK matcher, device-info frame (spec §1, §2.1)
  tests/microtest.h      ~90-line assert framework (no third-party dependency pulled in)
  tests/packet_builder.h synthetic packet builder; checksum computed by *replaying the
                         vendor state machine*, independently of d6/
  tests/test_d6.cpp      33 test cases / 177 checks
  tools/d6cli.cpp        macOS+Linux serial CLI: termios 230400 8N1, start/stop, live decode,
                         ASCII plots, --capture, --replay
  tools/d6synth.cpp      writes a synthetic raw capture (a 4x3 m room) so the whole
                         pipeline is exercisable with no hardware
  CMakeLists.txt         lib + tests + tools; make and ninja both verified
```

The `d6` target has **no platform code and no dependencies beyond the C++17 standard library** — it is meant to
be lifted verbatim into `engine/drivers/d6/` for task A2. All OS-specific code (termios, `/dev` scanning,
signals, TTY escape codes) is confined to `tools/`.

### API shape

```cpp
d6::Parser parser(cfg);
parser.set_point_callback([](const d6::Point& p) { ... });   // or take_points()
parser.feed(bytes, n, t_rx_ns);                              // any chunking
const d6::Stats& s = parser.stats();
```

`d6::Point` = `{angle_deg, distance_mm, intensity, high_reflectivity, new_rotation, from_start_packet, t_rx_ns}`.
`d6::Stats` = packets ok / bad-checksum / malformed, resyncs, bytes in / discarded / speed-adjust,
start packets, rotations, points, zero-range points, rolling `points_per_sec` and `rotation_hz`,
last reported scan-frequency field, and **both checksum variants' acceptance counters** (see §2).

---

## 2. The checksum question — where the vendor code diverges from the spec

This was the main unknown the spike had to close. The spec (§2.2, "Checksum") says the check code is a
two-byte XOR, that **the XOR order is not byte order**, and that because each sample `Si` is 3 bytes the third
byte "has to be zero-extended in its high 8 bits" — then points at a figure that does not exist in the
translated document (and is a bitmap in the original PDF).

XOR is commutative, so "order" is a red herring: **the only thing that matters is how bytes are grouped into
16-bit words.** Two readings survive the text, and they are genuinely different functions.

The vendor SDK (`sdk/lidar_data_processing.cpp`, `Lidar_Data_Processing::waitPackage`, the
`m_intensities == true` / `PackageSampleBytes == 3` path, which is the `M1CT_TOF` device profile:
230400 baud, 3-byte samples — i.e. **the D6**) accumulates, in wire order:

```c
case 1:  CheckSumCal  = PH;                       // 0x55AA as one word
case 5:  CheckSumCal ^= FirstSampleAngle;         // FSA, raw LE, check bit included
...      // sample bytes, recvPos counted from 0 over the body:
         recvPos%3==0 : Valu8Tou16 = byte; Valu8Tou16 += 0x00*0x100; CheckSumCal ^= byte;
         recvPos%3==1 : Valu8Tou16 = byte;                      // latch low
         recvPos%3==2 : Valu8Tou16 += byte*0x100; CheckSumCal ^= Valu8Tou16;
         CheckSumCal ^= SampleNumlAndCTCal;       // (LSN<<8)|M&T
         CheckSumCal ^= LastSampleAngleCal;       // LSA, raw LE
```

So the word set actually XORed is:

| Word | Content |
| --- | --- |
| 1 | `0x55AA` (the header as a word) |
| 2 | `(LSN << 8) \| M&T` |
| 3 | `FSA` raw little-endian, **before** the `>>1` that strips the check bit |
| 4 | `LSA` raw little-endian |
| per sample | `0x0000 \| Si_L` — **the *first* byte, zero-extended** |
| per sample | `(Si_H << 8) \| Si_2nd` |

**Divergence:** the spec text says the **third** byte of each sample is the zero-extended one; the vendor code
zero-extends the **first** byte and pairs the second with the third. These produce different check codes for
any sample where `Si_2nd != Si_H` (i.e. essentially always). Both readings are implemented:

* `ChecksumVariant::kVendorSdk` — **default**, the grouping above, matching the shipping driver.
* `ChecksumVariant::kSpecLiteral` — `(Si_2nd<<8)|Si_L` plus zero-extended `Si_H`.

The parser **always computes both** and counts them separately (`Stats::cs_ok_vendor`, `Stats::cs_ok_spec`),
so the very first live capture settles the question with data rather than argument: whichever counter tracks
`packets_ok` on real hardware is correct. `d6cli --checksum spec` switches the accepting variant.
This costs a handful of XORs per packet and can be dropped once the bench confirms the vendor reading.

Two further points where the vendor is authoritative over the text:

1. **`FSA`/`LSA` enter the checksum raw** — with the constant check bit still in bit 0, *before* the `>>1`.
   The spec's angle section describes `>>1` first, which would be the natural (wrong) order to implement.
2. **The header contributes `0x55AA` as a single word**, not as two zero-extended bytes (which would be
   `0x00AA ^ 0x0055 = 0x00FF`).

### Other divergences found between the vendor SDK and the spec

| # | Item | Spec | Vendor SDK | What the parser does |
| --- | --- | --- | --- | --- |
| 1 | Sample-byte grouping in the checksum | 3rd byte zero-extended | 1st byte zero-extended | vendor by default, spec variant available and always counted |
| 2 | Mechanical angle correction | "COIN-D6 already compensates for this angle mechanically, so this compensation value is ignored" (§2.1) | `waitPackage()` unconditionally adds `atan(19.16*(d-90.15)/(90.15*d)) * 64` in 1/64-degree units (approx +0.19 deg at 1 m, +0.21 deg asymptotically, 0 at 90.15 mm) | **off by default**, `Config::apply_mechanical_angle_correction` turns it on. Note the vendor multiplies an **atan result in radians** by 64 as if it were degrees — either a vendor bug or an empirical fudge; either way it is only ~0.2 deg, well under the 0.9 deg angular resolution. **Open question for the bench.** |
| 3 | Regressive angle packet (`LSA < FSA` not spanning 0 deg) | undefined | reuses the previous packet's angle step (`IntervalSampleAngle_LastPackage`) | same behaviour, unit-tested |
| 4 | Wraparound detection | `Angle(i) = FSA + (LSA-FSA)/(LSN-1)*(i-1)`, silent about wrap | treats it as a wrap only when `FSA > 270 deg` **and** `LSA < 90 deg` | same guard |
| 5 | Angle check bit | "`C` is a check bit whose value is fixed at 1" | rejects the packet and resyncs when bit 0 of the FSA/LSA low byte is 0 | same, `Config::require_angle_check_bit` (default on) — a cheap false-header filter |
| 6 | Error ACK frame | listed as a valid frame | not handled | `A5 5A 55 07 00x7 E9` has a *deliberately wrong* XOR trailer (correct would be `0xAD`) and is byte-identical to stop-OK otherwise; that is the only way to tell them apart. Encoded in `classify_ack()`. |
| 7 | Device-info frame checksum | "sum of all bytes except this field" | not used | additive sum, **not** XOR — verified against the worked example in §2.1 (sum = `0x02E3`). Unit-tested. |
| 8 | Start packet | "`LSN = 1`, contains 1 start point" | parses it like any other packet | same: its single sample is emitted as a real point, flagged `from_start_packet`, and marks `new_rotation` |
| 9 | Scan frequency `M` | "current scan frequency", units unstated | `scan_frequence = (M&T & 0xFE) >> 1`, and the health check treats a fixed-10 Hz device's value as needing to sit in `[10, 200]` | surfaced raw as `Stats::scan_freq_raw`; **units unresolved**, see open questions |

### Which vendor code path is the D6

`sdk/` is a multi-device SDK. `waitPackage_coin()` is **not** the D6 path — it decodes a completely different
`55 AA 83 08` frame with a rotate-and-add 15-bit checksum (the `M1CT_Coin_Plus` product). The D6 is the
`M1CT_TOF` profile: 230400 baud, `PackageSampleBytes = 3`, `m_intensities = true` -> `waitPackage()`, the
0x55AA protocol described in the D6 spec. Reading the wrong function would have produced a parser that never
syncs; this is worth flagging for whoever does A2.

---

## 3. Parser behaviour decisions

* **Header hunting.** Wire order is `AA 55` (the spec's `PH = 0x55AA` little-endian). While hunting, `0xFE`
  and `0xFF` bytes are counted as `speed_adjust_bytes` rather than garbage, so the pre-lock speed-adjustment
  traffic (spec §1) does not pollute the error stats. A contiguous run of dropped bytes counts as one resync.
* **Torn packets.** `feed()` buffers, so packets split across any number of calls reassemble. Tested with
  chunk sizes 1, 3, 7, 13, 64, 129 and 4096, plus strict byte-at-a-time feeding.
* **Bad checksum -> consume the whole packet** (vendor behaviour, `Config::consume_packet_on_bad_checksum`),
  because the header already passed the check-bit and LSN sanity tests, so its length is very likely right and
  consuming it preserves stream alignment. Set the flag to `false` to re-hunt 2 bytes in instead.
* **Malformed header -> drop one byte and resume hunting**, so a false `AA 55` inside a payload cannot eat a
  real packet.
* **Rotation detection** prefers start packets (`M&T` bit 0). If the device never emits one, it falls back to
  the vendor's angle-wrap heuristic (previous > 270 deg, current < 90 deg). Both paths tested.
* **Buffer bound.** A runaway stream that never yields a packet drops its oldest half at 64 KiB.
* **Zero-range points** are emitted by default (with a counter) rather than silently dropped —
  `Config::drop_zero_range` flips that; the `new_rotation` marker is carried forward to the next kept point.
* `t_rx_ns` is the host arrival time of the chunk that *completed* the packet, from `steady_clock`. Per-point
  time interpolation belongs in A4 (time-sync), not here.

---

## 4. Test results

`cmake -S . -B build && cmake --build build && ./build/d6_tests`

```
running 33 test cases
  ok   checksum_matches_vendor_state_machine
  ok   checksum_variants_differ_on_real_payloads
  ok   parser_counts_both_checksum_variants
  ok   bad_checksum_is_counted_and_drops_points
  ok   golden_sample_decode
  ok   sample_roundtrip_through_parser
  ok   drop_zero_range_option
  ok   angle_interpolation_is_linear
  ok   angle_wraparound_across_zero
  ok   angle_regression_reuses_previous_interval
  ok   single_sample_packet_uses_fsa_only
  ok   mechanical_angle_correction_is_opt_in
  ok   start_packet_marks_new_rotation
  ok   full_revolutions_are_counted
  ok   rotation_falls_back_to_angle_wrap_without_start_packets
  ok   resync_after_garbage
  ok   truncated_packet_then_resync
  ok   torn_across_feed_boundaries_bytewise
  ok   torn_across_awkward_chunk_sizes
  ok   speed_adjust_bytes_are_not_garbage
  ok   zero_lsn_is_malformed
  ok   angle_check_bit_rejects_false_headers
  ok   max_lsn_packet_is_handled
  ok   callback_mode_matches_queue_mode
  ok   stats_reset
  ok   rate_counters_advance
  ok   random_garbage_never_wedges_the_parser
  ok   feeding_only_garbage_bounds_the_buffer
  ok   command_bytes_match_spec
  ok   ack_frames_classified
  ok   ack_found_in_noisy_buffer
  ok   device_info_frame_from_spec_example
  ok   spec_info_example_checksum_is_the_documented_sum

177 checks, 33 cases, 0 failed cases, 0 failed checks
ALL TESTS PASSED
```

Also verified:

* `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` clean on the `d6` library (`-DD6_WARNINGS_AS_ERRORS=ON`).
* Clean under `-fsanitize=address,undefined` (tests **and** a full replay run).
* Both `Unix Makefiles` and `Ninja` generators build; `ctest` green.
* The golden sample case is hand-computed from the spec formulas: `Si = F1 8B 2A` ->
  distance `0x2A*64 + (0x8B>>2) = 2722 mm`, intensity `(0x8B&3)*64 + (0xF1>>2) = 252`, high-reflectivity `1`.

### End-to-end replay check (no hardware)

```
$ ./build/d6synth /tmp/d6_synth.bin 3 --noise
wrote 39426 bytes (30 revolutions, with noise) to /tmp/d6_synth.bin

$ ./build/d6cli --replay /tmp/d6_synth.bin --replay-duration 3 --plot none --quiet
---- summary (3.0 s) ----
bytes in            : 39426 (13142 B/s)
packets ok          : 330
packets bad cksum   : 0
packets malformed   : 0
checksum pass rate  : 100.0000 %   <-- S1 exit criterion: > 99.5 %
resyncs             : 4
garbage bytes       : 20
speed-adjust bytes  : 16
start packets       : 30 (scan freq field = 10)
rotations           : 30 (10.00 Hz avg)
points              : 12030 (4010 pts/s avg, 0 zero-range)
```

10.00 Hz and 4010 pts/s reproduce the datasheet figures through the full pipeline, and the injected garbage
and `0xFE/0xFF` runs are correctly separated (4 resyncs, 20 garbage bytes, 16 speed-adjust bytes) without
losing a single packet. The polar plot renders the synthetic 4 m x 3 m room with its reflective post.

Note: a raw capture carries no timestamps, so `--replay` defaults to a *wire clock* (23040 B/s at 230400 8N1).
The device idles between packets (~13 kB/s of a 23 kB/s link), so replayed rates read ~1.75x high unless you
pass `--replay-duration` with the real capture length. Live capture is unaffected.

---

## 5. The CLI

```
d6cli [--port DEV] [--replay FILE] [--capture FILE] [--seconds N]
      [--replay-duration S] [--plot polar|bars|none] [--max-range M]
      [--checksum vendor|spec] [--no-start] [--list] [--quiet]
```

* Auto-detects `/dev/cu.usbserial-*`, `/dev/tty.usbserial-*`, `cu.wchusbserial*`, `SLAB_USBtoUART*`,
  `ttyUSB*`, preferring the macOS callout (`cu.*`) node so it never blocks waiting for DCD.
* termios: `cfmakeraw`, `CS8`, no parity, 1 stop bit, `CLOCAL|CREAD`, no `CRTSCTS`, `VMIN=0 VTIME=0`,
  `B230400` (a standard rate on macOS — no `IOSSIOSPEED` needed). Clears DTR after open, as the vendor SDK
  does (some CH340 boards wire DTR to a reset line).
* Sends `AA 55 F0 0F` on start, watches the head of the stream for a start ACK **and** for the power-up
  device-info frame, then decodes continuously.
* Once a second: clears the screen, draws the plot, prints the stats line.
* Two plots, both 72x24 chars with one 5 deg sector per column: `--plot polar` is a top-down map (0 deg up,
  clockwise, glyph per sector at its nearest return) and `--plot bars` is a nearest-distance bar chart.
* `Ctrl-C` -> sends `AA 55 F5 0A`, drains for the stop ACK, prints the summary. `--seconds N` does the same
  on a timer, which is the bench-run mode.
* `--capture FILE` records the exact raw bytes while decoding (feeds A5's replay harness later);
  `--replay FILE` runs the identical decode path offline.

---

## 6. How to run the bench validation

The moment a D6 + CH340 is on the bench:

```bash
cd spikes/s1-d6-parser
cmake -S . -B build && cmake --build build -j8
./build/d6_tests                       # expect: ALL TESTS PASSED

./build/d6cli --list                   # confirm the CH340 enumerated
./build/d6cli --seconds 10 --capture bench10s.bin      # the 10 s exit-criteria run
```

What to record from that run:

1. **`checksum pass rate`** — the S1 gate is > 99.5 %.
2. **`accepted by vendor` vs `accepted by spec`** — settles §2. Expectation: vendor tracks `packets_ok`, spec
   stays at ~0. If it comes out the other way, flip the default in `Config::checksum` and update §2 here.
3. **`ACK: start-ok` and `stop ACK: stop-ok`** lines — the ACK half of the exit criteria.
4. **`rotations` ~ 10 Hz** and **`points` ~ 4000 pts/s** against the datasheet.
5. **`start packets`** — confirms the device emits `T=1` packets (if it is 0, rotation framing silently falls
   back to the angle-wrap heuristic and A8's pushbroom assembler needs to know).
6. **`scan freq field`** — see the open question below.
7. Whether the plot looks like the room. Point a corner and a doorway at it; check that 0 deg is where the
   datasheet says it is and that the angle increases clockwise.

Keep `bench10s.bin`: it becomes the first golden replay fixture for E2, and it makes every later parser change
regression-testable without hardware (`./build/d6cli --replay bench10s.bin --replay-duration 10`).

Also worth capturing for the fault-handling work in A2: a run started with `--no-start` (does the device
stream without the start command?), and a capture of the first two seconds after power-up (the info frame plus
the `0xFE/0xFF` speed-adjustment traffic before the rotation locks) — neither is reproducible later.

---

## 7. Open questions

1. **Checksum grouping** — vendor vs spec-literal. Resolved *in code* (vendor is the default, both are
   counted), needs one live capture to confirm. This is the only item that could invalidate the parser.
2. **Mechanical angle correction** (divergence #2). The vendor applies ~0.2 deg; the spec says the D6 needs
   none. Bench test: a flat wall at a known angle, correction on vs off, look at the residual. Below the 0.9
   deg resolution either way, so this is a refinement, not a blocker — but the pushbroom assembler (A8) will
   eventually care.
3. **`M` (scan frequency) units.** The vendor's health check accepts `[10, 200]` for a nominally 10 Hz device,
   which suggests either Hz or Hz*10 depending on the model. Read `scan_freq_raw` off a live device against
   the measured `rotation_hz` and pin it down.
4. **Does the D6 emit start packets at all, and how often?** Everything about rotation framing depends on it.
5. **Real LSN.** 40 samples/packet is the vendor's assumption (`INTENSITY_NORMAL_PACKAGE_SIZE = 130 = 10 + 40*3`);
   the parser handles 1..255, but the actual value affects packet cadence and the A4 time-sync model.
6. **False-header stall.** A garbage `AA 55` whose LSN field is large makes the parser wait for up to 775
   bytes before rejecting it on checksum. Self-correcting and bounded (~34 ms at 230400 baud, and only after a
   line error), but if bench data shows it happening often, cross-validate the length against the next header
   before committing.
7. **CH340 latency/chunking on macOS.** The current CLI polls with a 2 ms sleep; if the driver hands over
   large bursts, per-packet `t_rx` timestamps will be coarse. A4 needs to know the real granularity —
   worth logging inter-read gaps during the bench run.
8. **Vendor's `>= 0.15 m and intensity <= 65 -> drop`** filter in `send_lidar_data()` is a product-level
   noise filter, deliberately **not** implemented here (the engine records raw). Revisit in A2/A8.
