# Field-test kit v2 — engineering notes

Dev-facing. The tester never reads this; they read `README_FIRST.txt` in
whichever kit they were sent.

This kit replaces `tools/tester-kit/` (v1, COIN-D6 only, Windows only) with a
three-sensor, two-platform kit. v1 is left in place and still works — anything
already sent out keeps functioning — but new sends should use this one.

```
tools/fieldtest-kit/
├── windows/                     ships as LidarScan-FieldTestKit-Windows.zip
│   ├── START_TEST.bat           the only thing the tester touches
│   ├── README_FIRST.txt
│   └── scripts/
│       ├── menu.ps1             [1] D6  [2] Mid-360  [3] UM982  [4] all  [5] summary  [Q]
│       ├── common.ps1           result folder, combined log, banners, serial probe, NMEA checksum
│       ├── test_d6.ps1          230400 8N1, DTR clear, AA55F00F / AA55F50A
│       ├── test_mid360.ps1      static-IP help + elevated netsh + UDP capture (C# fast path + PS fallback)
│       └── test_um982.ps1       port/baud auto-probe, live fix+satellites, NMEA stats
├── macos/                       ships as LidarScan-FieldTestKit-macOS.zip
│   ├── TEST_ALL.command  TEST_D6.command  TEST_MID360.command  TEST_GPS_UM982.command
│   ├── README_FIRST.txt
│   ├── lib/common.sh            all three test functions + result plumbing
│   └── bin/                     BUNDLED arm64 binaries — the whole point
│       ├── d6cli                built from spikes/s1-d6-parser via cmake
│       ├── mid360cap            built from ../src/mid360cap.c
│       └── um982cap             built from ../src/um982cap.c
├── src/                         dev-side C sources for the two bundled tools
├── tests/                       dev-side smoke suites + synthetic data generator
├── make_kits.sh                 build + syntax-check + (optionally) test + zip
└── NOTES.md                     this file
```

`tools/fieldtest-kit/android/` is a **different deliverable** (an APK dropped
there by another workstream). `make_kits.sh` stages `windows/` and `macos/`
explicitly and never sweeps the parent directory, so the 95 MB APK cannot end
up inside a tester zip by accident.

---

## What each test actually proves

| Test | Proves | Does **not** prove |
| --- | --- | --- |
| **1 · COIN-D6** | The 4-pin cable, the CH340 bridge, the 5 V rail, the driver, the start command and the packet framing all work; the unit spins and ranges. On macOS additionally: the S1 parser decodes it at >99.5 % checksum pass rate. | Ranging *accuracy*, noise sigma, the checksum-variant question. Those need `verify_capture.py` / `d6cli --replay` on the returned `.bin` at the dev machine. |
| **2 · Mid-360** | 12 V power, Ethernet link, host IP config, and the lidar actually streaming point data to this host at a healthy datagram rate. | Point-cloud *content*. Nothing is decoded on the tester's machine on purpose — the `.livoxdump` goes home for SDK2 decoding. It also does not prove the lidar was *configured* by this kit; the kit only listens (see below). |
| **3 · UM982** | The board enumerates, the USB-serial bridge works, the receiver is alive, the baud rate is right, NMEA framing and checksums are clean, and the antenna sees satellites. Proprietary dual-antenna logs are captured intact. | RTK accuracy, heading accuracy, or anything requiring corrections. Deliberately: this test runs **standalone, with no NTRIP and no base station**, so *Single* fix (GGA quality 1) is the PASS condition. |

---

## Expected numbers

| Quantity | Expected | Kit's PASS threshold | Where the number comes from |
| --- | --- | --- | --- |
| **D6** byte rate | ~11.5 KB/s (the wire runs 23 040 B/s at 230400 8N1; the device idles between packets) | 8 000–16 000 B/s | `verify_capture.py` `D6_EXPECTED_RATE_*`, matching v1 |
| **D6** header spacing | one `AA 55` per ~100–140 bytes (packet is `10 + 3·LSN`, LSN≈40) | > 30 headers/s of capture | `verify_capture.py` D6 density check |
| **D6** checksum pass rate | 100 % on clean hardware | > 99.5 % (macOS only — Windows has no parser) | S1 exit criterion; measured 100.0000 % replaying `desktop/evidence/synth-d6.bin` |
| **D6** points | ~7 000 pts/s, ~400 points/rotation, ~10 Hz | not thresholded, logged | measured on the S1 synthetic capture |
| **Mid-360** datagram rate | ~2 000/s on the point port (~200 k pts/s ÷ 96 pts per 1380-byte packet) | ≥ 1 500/s on the busiest port | measured **2 084/s** on the real fixture `spikes/s2-mid360-sim/fixtures/indoor_livox_5s.livoxdump` |
| **Mid-360** point-port throughput | ~2.8 MB/s | not thresholded, logged | same fixture: 2.88 MB/s |
| **Mid-360** IMU port | ~200 Hz, small packets | not thresholded, logged | A3 driver doc |
| **UM982** sentence rate | 5–7/s at 1 Hz output (GGA+RMC+GSA+GST+VTG+HDT), higher at 5 Hz | ≥ 3/s | NMEA burst shape per A10 §"receiver emits a burst per epoch" |
| **UM982** checksum validity | 100 % | ≥ 99 % good | — |
| **UM982** fix quality | `1` = Single, outdoors, within ~1–2 min of cold start | **any fix ≥ 1 is a PASS**; `0` is WARN, never FAIL | no corrections are used in this test |
| **UM982** satellites | 8–30 outdoors on a multi-constellation receiver; 0 indoors | not thresholded, logged | — |
| **UM982** proprietary lines | `#UNIHEADINGA` etc., one per epoch if heading logs are on | not thresholded, logged and CRC-checked separately | — |

---

## Design decisions worth knowing

### Device identification is by probe, not by name
A COIN-D6 and a UM982 eval board can *both* be CH340s. v1 matched on the
friendly name `USB-SERIAL CH340`, which would grab the wrong port the moment
both are plugged in. v2 opens every candidate port and looks at what comes out:
`AA 55` framing ⇒ lidar, `$Gx…`/`#UNI…` ⇒ GPS. The UM982 probe also sweeps
baud rates (115200 first, then 460800, 9600, 38400, 230400), which
simultaneously solves the "what baud is this board actually set to" problem.

### Mid-360: ten ports, both conventions
`engine/docs/A3-mid360-driver.md` documents device ports 56100/56200/…/56500
and **host** ports 56101/56201/…/56501 (Livox's own +1 convention).
`tools/remote-capture/capture_mid360.py` binds the 56x00 set. Which one a given
unit streams to depends on what was pushed into it, and the kit has no way to
ask. So it binds **all ten** and lets the counters decide; the verdict uses the
busiest port. `verify_capture.py`'s mid-360 path now aggregates the silent
ports into one WARN line instead of nine.

### The kit only listens; it never configures the Mid-360
A Mid-360 does not discover its host — it is *told* where to stream via the
SDK2 `0x0100` config push (A3 §3). If the unit has never been pointed at the
tester's machine, nothing will arrive no matter how correct the IP is. The
FAIL text says so in plain words: run Livox Viewer 2 once, quit it, re-run.
This is `capture_mid360.py`'s "mode A" made idiot-proof.

### Windows Mid-360 receiver: compiled fast path with a scripted fallback
2 000 datagrams/s is more than a PowerShell loop can be relied on to sustain,
and undercounting would produce a false FAIL. `test_mid360.ps1` compiles a
~120-line C# receiver with `Add-Type` (one thread per port, `Interlocked`
counters, one locked writer) and falls back to a pure `Socket.Select` loop if
`Add-Type` is unavailable — logging which path ran, because the fallback's
numbers may understate the truth. Both paths are covered by the smoke suite
(`LIDARSCAN_KIT_FORCE_FALLBACK=1` forces the slow one); both hit ~2 000/s on
this machine.

### macOS goes compiled, Windows goes scripted
Windows ships PowerShell 5.1 in the box, so scripts are the zero-install
answer there. macOS ships no equivalent that can do 230400-baud serial or
sustained UDP capture without dependencies, so the kit bundles three small
arm64 binaries instead. That also buys the macOS D6 test a *real* verdict:
`d6cli` is the S1 parser, so the Mac reports an actual checksum pass rate
rather than the byte-rate heuristic Windows uses.

### The `.livoxdump` container is byte-identical across all four writers
`capture_mid360.py`, `mid360cap.c` and both PowerShell paths write the same
header (`LX360CAP`, u16 version=1, u16 num_ports, N×u32 ports) and the same
14-byte record prefix (u64 LE t_ns, u16 LE port_idx, u32 LE len). The smoke
suites assert this by reading the bytes back and by round-tripping through
`verify_capture.py`.

---

## Wiring / power / driver notes

### UM982 — antennas are not optional
The UM982 is a **dual-antenna** receiver. Two separate facts matter:

* **ANT1 (a.k.a. ANT/MAIN) must be connected or nothing works at all.** No
  antenna ⇒ the receiver still talks, still emits clean NMEA, and reports
  fix quality `0` forever. That is exactly the WARN case the kit describes as
  "alive but has not found satellites" — which is also what an indoor antenna
  looks like, so the tester is told to check both.
* **Heading needs BOTH antennas AND a real baseline.** `#UNIHEADINGA` /
  `$GNHDT` only become meaningful with ANT2 connected and the two antennas
  physically separated. A10 §"Yaw is observable only from a baseline"
  quantifies why the separation matters: heading uncertainty is
  `atan(σ / baseline)` — 1.1° at 2 cm accuracy over a 1 m baseline, 63° over
  2 cm. A bench test with the two antennas sitting next to each other will
  produce heading numbers that are noise. **Do not read anything into the
  heading values from this test**; the kit only reports that heading
  sentences are *present*.
* Antennas are finger-tight on SMA/MMCX. The README says so because
  over-torquing an SMA on a dev board is a classic way to kill it.

### Mid-360 — 12 V
9–27 V DC, ~6.5 W, and **reverse polarity is the highest-consequence mistake
on this bench** (`docs/bench/BENCH_SETUP.md` §4). The kit's pre-flight text
asks the tester to confirm power is connected and the unit whirrs; it
deliberately does not walk them through wiring a battery, because that step
should not be done unsupervised on a bare-wire lead.

### D6 — 5 V over the same 4-pin connector
4.5–5.5 V, <100 mV ripple, ~800 mA peak. If the vendor pigtail carries USB
VBUS the host port covers it; if it exposes bare CVC/GND leads it needs a
clean external 5 V. Bench doc §1.2.

### USB-serial drivers per OS

| Chip | Windows 10 | Windows 11 | macOS 14+ | How to tell which one you have |
| --- | --- | --- | --- | --- |
| **CH340 / CH341** (`1a86:7523`) | Usually auto-installed over Windows Update — needs internet + a replug, give it a minute | In-box | In-box (`AppleUSBCHCOM`) | Chip marking next to the USB socket reads `CH340G` / `CH340C` / `CH340N`. Device Manager shows `USB-SERIAL CH340 (COMx)`; macOS shows `/dev/cu.wchusbserial*` |
| **CP2102 / CP2105** (`10c4:ea60`) | Needs the Silicon Labs VCP driver | Needs the Silicon Labs VCP driver | Needs the Silicon Labs VCP driver on some versions | Chip marking reads `CP2102` / `CP2105`. macOS shows `/dev/cu.SLAB_USBtoUART*` |

Both kits print exactly this "look at the chip marking" instruction in their
GPS FAIL path, because which bridge a given UM982 eval board uses is not
knowable in advance. If a tester reports CP210x, send them
<https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers> — the
kit deliberately does **not** ship or auto-download a driver.

The D6's own CH340 pigtail is documented in `docs/bench/BENCH_SETUP.md` §1.5,
including the vendor's Windows driver zip already on disk.

---

## Verification side (`tools/remote-capture/verify_capture.py`)

Extended, not rewritten:

* **NMEA path now handles UM982 captures.** Unicore proprietary `#…` ASCII
  logs are counted separately from NMEA 0183, CRC32-checked with the
  NovAtel-style routine (standard polynomial, init 0, **no** final inversion —
  not `zlib.crc32`), and never scored as corrupt NMEA. Heading-bearing
  sentences (`UNIHEADING*`, `HDT`, `HDG`, `HPR`, `THS`) are counted and their
  *absence* is a WARN with the "only ANT1 connected?" explanation.
* **Binary Unicore logs are flagged**, not decoded: an `AA 44 B5` / `AA 44 12`
  sync anywhere in the file raises a WARN telling us to check the receiver's
  output config.
* **Fix histogram semantics made explicit.** Any fix ≥ 1 is a PASS, with the
  note that single/DGPS-only is *correct* for a bench capture. An all-zero
  histogram is a WARN that says it is tolerated and why.
* **Sentence rate** is checked when the duration is known (`--seconds`, or a
  filename like `um982_90s.nmea`, which is what the kits write).
* **Satellite counts** from GGA field 7 are summarised.
* `--type um982` is accepted as an alias for `nmea`.
* **Mid-360 path**: silent ports aggregate into one WARN line, per-port
  datagram/s is reported, and the busiest port is checked against the same
  1 500/s threshold the kits use.

⚠ The Unicore CRC32 routine is currently self-consistent with
`tests/make_synthetic.py` (both implement the same documented algorithm) but
has **not** been validated against a byte from a real UM982. That is why a CRC
mismatch on `#` lines is a WARN, never a FAIL — a mismatch is far more likely
to mean our polynomial assumption is wrong than that the receiver is broken.
First real capture: check this and tighten or loosen accordingly.

---

## What was verified locally, and what waits for hardware

`tests/smoke_windows.ps1` — 39 checks, all passing on pwsh 7 / macOS, and
again with `LIDARSCAN_KIT_FORCE_FALLBACK=1`.
`tests/smoke_macos.sh` — 29 checks, all passing.

Verified for real, no hardware:

* every PowerShell script parses clean under the PS tokenizer; every shell
  script passes `bash -n` (both wired into `make_kits.sh`)
* NMEA checksum validation (valid / invalid / unchecksummable / non-sentence)
* the full UM982 stats parser and all three verdict outcomes, over synthetic
  90 s streams: Single-fix ⇒ PASS, no-fix ⇒ WARN, 5 % corrupted ⇒ WARN,
  silence ⇒ FAIL
* Unicore `#UNIHEADINGA` lines counted as proprietary, never as bad checksums
* the combined `TEST_RESULT.txt` writer: creation, appending a second block,
  the menu's running summary
* the Mid-360 UDP receiver on **all four** implementations (C# fast path, pure
  PowerShell fallback, `mid360cap.c`, and via the packaged `.command`) against
  a 2 000 datagram/s synthetic blast, hitting the 1 500/s threshold
* the `.livoxdump` header bytes, and the file round-tripping through
  `verify_capture.py`
* `d6cli` built arm64 by `make_kits.sh` and replaying
  `desktop/evidence/synth-d6.bin`: 100.0000 % checksum pass rate, 120 300
  points — and the exact `sed` expressions `run_d6_test` uses to scrape that
  summary
* `um982cap` against a pseudo-terminal fed synthetic NMEA: live per-second
  fix/satellite display, all verdict paths
* both zips unpacking with the exec bit intact, and the extracted macOS kit
  running a test end to end into a result folder

Deferred to hardware:

* real USB serial enumeration (`Win32_PnPEntity`, `/dev/cu.usbserial-*`) and
  the CH340/CP210x driver situation on a fresh tester machine
* the Windows elevated-`netsh` static-IP path (UAC prompt, adapter naming,
  multi-adapter selection) and the macOS `sudo ifconfig alias` path
* a real Mid-360's actual host-port choice, and whether the ping sweep finds it
* the UM982's real default baud, its real sentence mix, and the real
  `#UNIHEADINGA` CRC
* `Add-Type` availability on a locked-down corporate Windows machine

---

## Open risks

1. **UM982 default baud is not confirmed.** 115200 is the documented default
   and is the kit's first probe, 460800 the second. If the board turns out to
   be neither, the probe sweeps 9600/38400/230400 too and the chosen rate is
   recorded in `TEST_RESULT.txt` — so a wrong guess costs a few seconds, not a
   failed test. Both kits also accept an explicit override (`-Baud` on
   Windows, `UM982_BAUD=` / `UM982_PORT=` on macOS) for a support call.
2. **Mid-360 static IP is the friction point on Windows.** The auto-set path
   needs a UAC prompt a non-technical tester may refuse, and it *breaks
   internet on that adapter* until reverted. The kit prints the exact undo
   command and the manual Settings path, but expect this to be the step that
   generates support messages. Consider shipping a `RESTORE_NETWORK.bat` if
   the first field round confirms this.
3. **The kit cannot start a Mid-360 stream.** If the unit was never pointed at
   the tester's machine, test 2 FAILs through no fault of the tester. The FAIL
   text explains the Livox Viewer 2 workaround, but the real fix is a small
   configure-and-capture tool (SDK2 `0x0100` push) in a future kit version.
4. **`Add-Type` could be blocked** by a locked-down machine or aggressive AV.
   The fallback covers it, but its counts may understate the rate and produce
   a spurious WARN. The receiver path used is logged in `TEST_RESULT.txt`, so
   this is diagnosable from the returned folder.
5. **Unsigned macOS binaries.** The `.command` files clear
   `com.apple.quarantine` from `bin/` at startup (our own files, no admin
   needed), but the `.command` itself still needs the one-time
   right-click → Open. This is called out at the top of the macOS README. If
   testers get stuck here often, the answer is notarisation, not more docs.
6. **Intel Macs are not covered.** `make_kits.sh` asserts all three bundled
   binaries are arm64 and fails the build otherwise. An Intel kit is a
   `-DCMAKE_OSX_ARCHITECTURES=x86_64` / `-arch x86_64` variant, or a universal
   binary via `lipo` — deliberately deferred.
7. **A D6 probe leaves the lidar spinning.** The probe sends the start command
   and does not send stop if that port turns out not to be the D6 (it will not
   have been the D6, so nothing was started) or if the run is aborted midway.
   Harmless, but a tester may notice the unit keeps spinning after a failed
   run; unplugging stops it.
