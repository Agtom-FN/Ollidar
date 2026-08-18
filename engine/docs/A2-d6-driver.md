# A2 — D6 driver hardening

**Scope:** `engine/src/drivers/d6/d6_driver.cpp`, `engine/include/scanengine/drivers/d6/d6_driver.h`,
`engine/tests/test_d6_driver.cpp`. §1–§8 live entirely in the driver's integration layer:
the S1 parser (`d6_parser.h/.cpp`, `commands.h`) was **untouched** — byte-identical to the
spike, as required.

> **That is no longer true as of ROUND 9 (0.6.0), and §9 says why.** Timestamping a return
> at the instant it was *sampled* needs the packet's own byte length and sample count,
> which only the parser has. The decode path — framing, checksums, angle interpolation,
> every field of `d6::Point` that existed before — is unchanged; what was added is a new
> `t_sample_ns` field, the `Config`/`Stats` members that produce it, and nothing else. The
> original `t_rx_ns` keeps its exact old meaning, so a caller that ignores the new field
> sees the pre-ROUND-9 parser.

Reads: `engine/DESIGN.md` (A1 contracts — error model §3, event bus §2, "Notes for A2"),
`spikes/s1-d6-parser/REPORT.md` (§2 checksum variants, §7 open questions).

---

## 1. Why a second state machine

`core/types.h`'s `DeviceState` (A1, out of this task's ownership) has seven values and no
"stalled" or "restarting" concept. Rather than touch a shared, cross-workstream header,
`D6Driver` runs its own richer state machine, `D6Phase`, and maps it down onto
`DeviceState` for `state()`/`health()` and every event the driver publishes:

| `D6Phase` | → `DeviceState` | Meaning |
| --- | --- | --- |
| `kIdle` | `kIdle` | not started |
| `kStarting` | `kStarting` | `start()` called, nothing decoded yet |
| `kStreaming` | `kStreaming` | decoding packets at an acceptable checksum rate |
| `kDegradedChecksum` | `kDegraded` | streaming, but checksum pass rate < threshold |
| `kStalled` | `kDegraded` | watchdog fired: silent or garbage (see §3) |
| `kRestarting` | `kStarting` | actively retrying start/stop with backoff |
| `kStopping` | `kStopping` | `stop()` in progress |
| `kFault` | `kFault` | restart budget exhausted, or a device error ACK |

This keeps `DeviceHealth`/`EventType::kDeviceState` byte-compatible with every other
consumer (Engine, C ABI, Qt/JNI) while giving the driver, tests, and `engine_cli` the full
picture through `D6Driver::snapshot()` → `D6HealthSnapshot` (new, D6-specific, additive).

Consistent with `DESIGN.md` §3 item 5 ("checksum losses → `kDegraded`, never `kFault`"):
the only way this driver reaches `kFault` on its own is exhausting the restart budget
(§4) or a device error ACK. Checksum loss alone never escalates past `kDegradedChecksum`.

## 2. Health model

`D6HealthSnapshot` (returned by the new `D6Driver::snapshot()`) adds, beyond the core
`DeviceHealth`: the `D6Phase`, the `D6StallKind` the watchdog last observed, `resyncs`,
both checksum-variant counters (`cs_ok_vendor`/`cs_ok_spec`), the accepted variant and its
verdict (§5), the restart-attempt count, and the last-bytes / last-valid-packet
timestamps. `parser_.stats()` already refreshes `points_per_sec`/`rotation_hz` roughly
once a second (S1); nothing here changes that cadence, it just surfaces more of what was
already being counted.

## 3. Stall/fault detection without an engine thread

`DESIGN.md` §2 is explicit: **the engine owns no threads in A1**, and creating one is a
documented act (a new row in that table) that is not this task's to make. So the stall
watchdog cannot run on a timer. Instead, `check_watchdog(TimePoint now)` is re-evaluated
opportunistically, every time the driver does anything observable:

* `on_bytes()` — using the arriving chunk's own timestamp. This is what catches a
  "garbage stream" (bytes arriving, nothing decodes).
* `state()` / `health()` / `snapshot()` — using `DriverContext::clock()` (wall-clock,
  overridable in tests/replay). This is what catches a genuinely **silent** device, since
  by definition `on_bytes()` never runs for one.

An app that never polls health will not observe a silent-stall transition — this matches
`core/types.h`'s own note that `DeviceHealth` is "polled by app UIs" (the S1 CLI already
polls once a second). `check_watchdog()` is public specifically so tests and `engine_cli`
can drive it with synthetic time instead of sleeping in real time; see
`tests/test_d6_driver.cpp` for the pattern (a plain-function-pointer fake clock plugged
into `DriverContext::clock`, since `ClockFn` takes no captures).

Two kinds of stall are distinguished (`D6StallKind`), because they mean different things
operationally:

* **`kSilent`** — no bytes at all for `silent_stall_timeout_ns` (default 1.5 s). The
  device (or the OS handle to it) is gone.
* **`kGarbage`** — bytes are arriving but no valid packet decodes for
  `garbage_stall_timeout_ns` (default 3 s). REPORT.md §7 item 6 flags exactly this
  failure mode: a false `AA 55` with a large LSN can make the parser wait up to ~775
  bytes (~34 ms) before rejecting it — self-correcting on its own, but worth
  distinguishing from real silence when it isn't.

`ScanError` already had the right values for this without adding any (`kDeviceNotResponding`
for silent, `kProtocolError` for garbage) — both surface on the `kDeviceState` event and
in `health().last_error`.

## 4. Restart policy

On a stall, `attempt_restart()` sends `AA 55 F5 0A` (stop) then `AA 55 F0 0F` (start) —
best effort, resets any stuck framing state before asking the device to stream again —
and schedules the next attempt with exponential backoff (`restart_backoff_base_ns`,
doubling, capped at `restart_backoff_max_ns`). Exhausting `max_restart_attempts` (default
5) is terminal: `kFault`, matching `core/types.h`'s "any state --failure--> kFault
(terminal until stop()/re-add)". A plain `stop()` clears the restart budget, so an app can
retry a faulted device without recreating the driver.

Without a write function (`cfg.send_start_stop_commands == false`, or no
`serial.write_fn`), there is nothing the driver can actively do — it stays `kStalled`
(→ `kDegraded`) indefinitely rather than pretending to retry or manufacturing a fault out
of a condition it cannot act on. The app owns that transport and can `stop()` it itself.

Every phase transition publishes `EventType::kDeviceState` (mapped `DeviceState`,
previous, and the triggering `ScanError`); every restart attempt additionally publishes
`EventType::kError` (existing event types — no new payload was needed).

ACK scanning (`scan_for_acks()`) now also runs during `kRestarting`, not just
`kStarting`/`kStopping`, so a start ACK or the deliberately-wrong-XOR error ACK
(`commands.h`) is honoured mid-restart exactly as it is on the initial `start()`.

## 5. Checksum-variant resolution hook

Per REPORT.md §2, the parser always counts both `cs_ok_vendor`/`cs_ok_spec`; the *accepted*
variant (which one feeds `packets_ok`) is `kVendorSdk` by default. `D6Driver` adds:

* `checksum_variant()` / `set_checksum_variant()` — the switch is applied on the
  byte-processing thread only, on the next `on_bytes()`, never synchronously from the
  caller's thread. This is deliberate: `d6::Parser` has no internal synchronization (it is
  frozen S1 code), and the driver's own threading contract is that a `Driver` instance is
  pushed from one thread at a time. Routing the change through an atomic "desired variant"
  that `on_bytes()` applies to itself keeps every `parser_.set_config()`/`parser_.feed()`
  call on that one thread, so nothing new races the parser.
* `checksum_verdict()` — once `health_min_packets` packets have been observed, whichever
  counter tracks ≥ 99.5% (the same S1 exit-criterion bar) is `kVendorConfirmed` /
  `kSpecConfirmed`; otherwise `kUndetermined` (too few packets) or `kAmbiguous` (both
  track equally — should not happen on real hardware). This is the "first real capture
  settles it" mechanism REPORT.md asks for: read the verdict, and if it disagrees with
  `checksum_variant()`, call `set_checksum_variant()`.

**Baseline reset on switch.** Packets rejected under a since-corrected variant were an
artefact of the wrong acceptance, not real loss. If the checksum pass-rate used for the
`kDegradedChecksum`/`kStreaming` decision stayed cumulative across a switch, a device could
never climb back out of `kDegradedChecksum` in any reasonable number of packets (the old
rejects would forever outweigh new accepts). So `on_bytes()` snapshots
`packets_ok`/`packets_bad_checksum` at the moment a switch is applied
(`checksum_baseline_ok_`/`checksum_baseline_bad_`) and rates the window *since* that switch
for the phase decision. The externally-reported `checksum_pass_rate` (in `DeviceHealth` and
`D6HealthSnapshot`) is unaffected — it stays the parser's lifetime rate, matching what
`test_engine.cpp`'s existing degradation test already asserts.

## 6. Speed-adjust grace period

`startup_grace_ns` (default 2.5 s) is a window from `start()` during which neither
watchdog is evaluated at all — not "filler bytes don't count", but "nothing is judged
yet". The S1 spec's pre-lock `0xFE`/`0xFF` speed-adjustment traffic (and whatever silence
goes with the device settling on its rotation frequency) is exactly what this covers; it
is not extended by restart attempts, since a restart is not a fresh power-up. See
`tests/test_d6_driver.cpp`'s `speed_adjust_filler_tolerated_in_grace_then_garbage_stall_after`
for both halves: tolerated inside the window, a real garbage stall once it elapses with
still nothing decoded.

## 7. Tests

`tests/test_d6_driver.cpp`, 9 new cases (399 assertions across all D6 test files
combined — 33 S1 parser cases + these + the existing `test_engine.cpp` D6 end-to-end
cases): clean stream → `kStreaming`; corrupted checksums → `kDegradedChecksum` with
correct stats; a silent device with no write channel → `kStalled` forever, never faults;
a silent device with a write channel → `kRestarting` with backoff → `kFault` once the
budget is exhausted (checking the exact stop/start bytes written and the event counts);
start-ACK gating; the error-ACK → immediate fault; the grace-period boundary; the
checksum-variant verdict and switch (including the baseline-reset behaviour above); and
that `stop()` clears restart/fault state for a following `start()`.

Tests talk to `D6Driver` directly (not through `Engine`), constructing `EventBus`/
`PageStore`/`DriverContext` by hand, so the watchdog-timing tests can plug a
deterministic fake clock into `DriverContext::clock` — a plain function pointer reading a
file-local variable the test moves by hand — instead of sleeping in real time.
`packet_builder.h` is reused unmodified, per the ownership rules.

## 8. Nothing needed from the orchestrator

No out-of-ownership changes were required. `core/types.h`'s `DeviceState`, `core/error.h`'s
`ScanError`, and `core/event.h`'s existing event types already had everything this task
needed (see §1, §3, §4) — the richer state machine and the D6-specific health surface live
entirely in `D6Driver`'s own header, additively.

---

## 9. ROUND 9 — when a return was SAMPLED, not when its bytes arrived

The owner supplied the authoritative device numbers, and they change the
timestamping model.

| quantity | value |
| --- | ---: |
| rotation rate | 10 Hz |
| sampling rate | 4000 Hz |
| returns per revolution | 400 |
| sampling period | **250 µs** |
| angle per sample | 0.9° |
| link | 230400 8N1 = **23,040 B/s** capacity |
| **measured throughput** | **~13.7 KB/s** |

That last row is the one that matters. 13.7 / 23.04 = **~60 % wire duty**. The
D6 is not trickling bytes out in step with its mirror: it fills a packet, blasts
it at the full line rate — **~1.7x faster than real-time sampling** — and then
idles until the next one is ready.

The packet size falls straight out of those numbers and confirms them. With
`lsn` samples per packet the stream costs `4000·3 + (4000/lsn)·10` bytes/s; at
`lsn = 24` that is **13,667 B/s**, which is the measured 13.7 KB/s. A 24-sample
packet is 82 bytes, takes 3.56 ms to transmit and covers 6.0 ms of sampling —
59 % duty, exactly as measured.

### 9.1 What ROUND 7 got right, and the 1.7x it did not

`D6Config::time_slice_bytes` (§ROUND 7) back-dates bytes across a USB read at
the wire rate. At **packet** granularity that is correct and remains so: it is
how a packet is located inside a 4 KB read. Inside a packet it is wrong by the
duty cycle — it spreads ~24 samples of 250 µs spacing across only 3.56 ms of
wire time, compressing them 1.7x. The residual is 1–3 mm at walking pace,
small but systematic and in the same direction every packet.

### 9.2 The model

`d6::Config::per_sample_timestamps` (default on) implements three parts, and
`d6::Point` gained `t_sample_ns` alongside the existing `t_rx_ns` — the latter
keeps its old meaning exactly.

1. **Spacing is the SAMPLING period, not the wire period.** Derived per packet
   from the packet's own FSA/LSA angle span and the device's reported scan
   frequency (the start packet's field is in 0.1 Hz units — 100 means
   10.00 Hz), so a motor running at 9.7 Hz is honoured rather than assumed
   away. `nominal_sample_hz` (4000) is the fallback before the device has
   reported anything.

2. **The anchor is the packet's FIRST byte.** The device begins transmitting
   immediately after taking the packet's last sample, so
   `t_last_sample ≈ t_first_byte = t_last_byte − packet_bytes / 23040`, and
   the earlier samples are back-dated from there at the sample period.

3. **The wire-rate model keeps exactly one job**: locating packet boundaries
   inside a read burst.

**The cross-check the spec asks for** is in `Stats`: `sample_hz_est` is the
angle-derived rate, compared every packet against the datasheet's 4 kHz.
Outside `sample_rate_tolerance` (±25 %) the packet's angle span is treated as
corrupt rather than as a slow motor — the nominal period is used and
`sample_rate_warnings` increments. A steady climb there means the device is not
sampling where its spec says and every per-point stamp is drifting.

### 9.3 The min-delay sample clock

Part 2 above is **biased late**, and the bias is the duty cycle. Back-dating
`bytes_after` at the line rate assumes the link was busy throughout; at 60 %
duty it was idle for 40 % of it, so the true elapsed time is ~1.7x what the
model says and the anchor lands too late — by up to ~120 ms at the head of a
phone-sized 4 KB read, which is far worse than the 1–3 mm §9.1 is about. The
wire anchor is therefore an **upper bound** on the true sample time, never a
lower one.

The device's sampling rate, however, is an excellent clock: steady, known to
0.1 Hz, and indifferent to how the bytes are bunched. So `sample_clock_anchor`
(default on) runs the classic min-delay estimator over the two — propagate the
previous packet forward at the sampling period, take whichever of {propagated,
wire anchor} is **earlier**.

This converges without any tuning. At the **tail** of every read `bytes_after`
is ~0, so that packet's anchor is tight and the minimum takes it; the chain
carries that tight value through the **head** of the next read, where the anchor
is loose and the propagation wins. Every packet ends up dated from the nearest
tight anchor instead of from the line-rate fiction. `sample_clock_resync_ns`
(50 ms) re-seeds the chain when it breaks — dropped packets, a stall, a device
restart — and `Stats::sample_clock_resyncs` counts it.

It also subsumes what would otherwise be a separate monotonicity clamp. Two
invariants the assembler needs both fall out:

* **non-decreasing stamps** — the pushbroom's pending queue is documented as
  time-ordered ("if the OLDEST point is still in the future every point behind
  it is too");
* **never later than the transport** — a point claiming a time in the future of
  the bytes that carried it would make the assembler wait for a pose that has
  already been superseded.

Where a stream is saturated and the two conflict, the spacing is **compressed**
to fit rather than the window being slid forward: that holds both invariants and
degrades exactly to ROUND 7's wire-rate spacing, which is the honest fallback —
with no idle gap there is nothing better to infer.

The sampling period (250 µs) is *longer* than the per-sample wire period (3
bytes = 130 µs), which is why a truly saturated stream is not physical: the
60 % duty is precisely the slack that makes room for the sampling. It does
occur in synthetic fixtures with no idle gaps, which is what the clamp is for.

### 9.4 Tests

`tests/test_d6_driver.cpp`'s ROUND 7 cases are updated rather than replaced.
Two assertions had to change, and both changes are the point:

* the per-point span across a read may now slightly **exceed** the read's wire
  duration, because the first packet's earliest sample predates the read's
  first byte by up to `(lsn−1)` sampling periods. That allowance is stated in
  the test, not hidden in a tolerance.
* the ROUND 7 falsifiable control (`time_slice_bytes = 0` → "one stamp per
  chunk") now also sets `per_sample_timestamps = false`. Per-sample stamping
  works off each packet's byte *offset* inside the reassembly buffer, so it
  recovers per-point structure even with slicing off — a real improvement that
  would otherwise have quietly disarmed the control.

---

## 10. ROUND 10 — MEASURING the lidar↔pose offset instead of assuming it

ROUND 7 §4 named a "genuinely constant transport delay" on the D6 path, derived
a **2 ms** default from first principles (one USB bulk frame plus one reader
wake-up) and said honestly that no D6 existed here to measure against. ROUND 10
had one: the owner's `scan-020` — 202.1 s, 293,524 in-range returns, 5,966
ARCore poses and **80,661 phone-IMU samples at 399.1 Hz, all in the same
container and the same clock domain**.

### 10.1 The two crossings, measured separately

| crossing | method | result |
| --- | --- | ---: |
| phone IMU ↔ ARCore pose | cross-correlate \|gyro\| (integrated over each pose interval) against the pose-derived angular rate, sweeping the lag | **−1.5 ms**, r = 0.982 |
| D6 lidar ↔ ARCore pose | resolve the whole container at a sweep of `pose_time_offset_ns` and take the crispest map | **+4 ms**, curve flat to ±0.1 % over ±30 ms |

The first is the stronger measurement and the more useful one: it says
`Frame.getTimestamp()` and `SensorEvent.timestamp` agree to under two
milliseconds on a Pixel 8 Pro, i.e. the CLOCK_BOOTTIME claim in ROUND 7 §4 is
not just a documented intention, it is true on the hardware. It also removes
that crossing from suspicion permanently, which is worth more than the number.

The second is a **null result and should be read as one.** +4 ms is 4 mm at
1 m/s and 0.24° at a 60 °/s turn, against the 4.8 cm wall thickness the same
capture actually shows — two orders of magnitude down. The turn-around shift
the owner reported is not a clock offset.

### 10.2 The tool

`tools/engine_cli.cpp --d6-timesweep <lscan-dir> [--from MS] [--to MS]
[--step MS] [--no-densify] [--up X|Y|Z]` resolves the same container once per
offset through the production `post::D6ResolvePipeline` and prints three
crispness metrics per row:

* **`wall_rms_cm`** — 2-D PCA thickness at wall probes chosen ONCE from the
  reference (offset-0) pass and reused at every offset. Choosing them per
  offset would measure "which offset yields the most selectable walls", which
  is circular; the elongation filter is therefore applied only when the probes
  are picked, never when they are measured.
* **`occupied_vox`** — 3 cm voxels. Selection-free, so no fitting decision can
  influence it. This is the metric that actually resolved on scan-020.
* **`entropy_bits`** — Shannon entropy of the voxel histogram (the mean-map-
  entropy family). Same direction, sensitive to how mass redistributes rather
  than only to its support.

`points` is printed per row and must stay flat: a sweep is only a measurement
if the population being measured does not move with the knob.

The up axis is **not detected**, deliberately. The first version guessed it
from the tallest histogram peak, picked X on scan-020, silently emptied the
wall band and reported zero walls at every offset. A D6 project is by
construction an ARCore project (the D6 has no IMU, so only the phone can supply
its trajectory) and ARCore's world is gravity-aligned with **+Y up**. A
constant that can only be wrong loudly beats a heuristic that can be wrong
quietly.

### 10.3 What the sweep did find

`--d6-timesweep` also prints the pipeline's own accounting, and two rows of it
mattered more than the offset:

* **`fallbacks by reason`** — the raw `imu_fallbacks` counter looked alarming
  (196,823 of 455,402 queries, 43 %) until it was broken out: 162,236 of those
  are `sample_at()` calls that returned no pose at all, which are the
  assembler's *retries* on a point whose pose has not arrived yet, not
  densification failures. The real fallback rate among resolved returns is
  **11.8 %, essentially all `imu-gap`**.
* **`imuCalibration` was `null`** in scan-020's manifest even though the
  session log for that capture records `camera_from_imu = Rz(+90)`. That is
  ROUND 8's `mountCalibration` bug one sensor down — `start_session()` writes
  the manifest at `open()`, and the app applies the IMU extrinsic ~24 ms later
  — so every offline re-resolve integrated the gyro in the wrong frame. Fixed
  in `Engine::set_imu_extrinsics()` the same way ROUND 8 fixed the mount: push
  it at an already-open recorder.

### 10.4 The latency that WAS there

`PushbroomConfig::batch_points` (4096) is a throughput number written for a
Mid-360. At scan-020's measured **1,453 resolved points/s** it is **2.8 seconds**
of points held in a `std::vector` before the PageStore, the renderer or the map
cache sees one — which is most of "the scanning speed seems a bit slow and
delay", and it is invisible to every offline test because offline nobody is
watching. `max_batch_span_ns` (100 ms, one D6 revolution) bounds the batch in
**point time**, never wall time, so the batch boundaries stay a pure function
of the data and `assembles_identically_live_and_offline` still holds bit for
bit. Measured in `tests/test_round10_time_offset.cpp`: first point visible
after **2,825 ms → 100 ms**, 2,560 of 2,560 points bit-identical across the two
batchings.

## 11. ROUND 11 — the mount trim's cost, per metre of range

`--d6-timesweep` (§10) measures the lidar↔pose clock offset and found it
negligible. The error that is NOT negligible on this rig is the mount trim, and
it has a closed form worth having written down beside the timing one because the
two are confused easily and behave completely differently.

A mount trim error is a small rotation `d` of the lidar inside the phone frame.
It never changes a range, so it displaces every return **perpendicular to its own
ray** by `r·sin(d)` in a direction fixed in the PHONE. Turning around does not
change the trim — it changes which way the phone's axes point in the world — so
the world-frame displacement reverses and the same feature is painted twice:

    split = 2 · r · sin(d)

Measured through the production assembler in
`tests/test_round11_mount_trim.cpp` at r = 1.66 m: **13.1 cm at 2.4°**, 9.0 cm at
1.4°, 3.7 cm at 0.8°. Scale linearly with range.

Three properties distinguish it from a clock offset, which is why ROUND 10's
sweep could not see it and ROUND 11's fixture could:

* **rate-independent.** A clock offset costs `v·dt` and `ω·dt`; this costs the
  same whether the operator walks or crawls. scan-020 was walked at 5.3 cm/s and
  still shows the symptom.
* **it doubles a FEATURE, it does not thicken a WALL.** The displacement is
  perpendicular to the ray, so on a surface the fan looks at square-on the points
  slide along it and the plane-fit RMS does not move. Every geometry metric in
  this repository before ROUND 11 was a wall-flatness metric.
* **only part of it reverses.** About the phone's RIGHT axis (camera +X) it
  reverses in full and displaces overhead/underfoot returns along the walk;
  about the phone's UP axis it does not reverse at all (for a vertically-held
  phone that axis IS world up whichever way the operator faces) and simply
  displaces the room by `r·sin(d)`, consistently, which is an error nobody
  reports; about the forward axis it does nothing at all, because that is a
  rotation inside the fan's own plane and the fan is 360°.
