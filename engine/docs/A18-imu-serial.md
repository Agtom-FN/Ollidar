# A18 — Serial IMU module driver (JuxiTech ICM-42670-P)

Status (2026-09-07): **code-complete and unit-tested; not yet run against the
module through the engine.** The wire protocol comes from the vendor's Arduino
reference (`imu_uart_driver.cpp`), the timing model is a port of the ROS
driver that ran this module live for the FAST-LIO work, and the one hardware
fact that shapes the whole design — the module **stops transmitting for
~2.9 s roughly every ~35 s** — was measured there. §7 lists what the engine
still owes to hardware.

---

## 1. Why this exists, and the rule it follows

A Livox Mid-70 has no IMU (`docs/A17-mid70-driver.md`). The first-cut IMU
source for a Mid-70 session is this module over USB serial. It is a
**push-mode serial driver in the shape of `D6Driver`**: the app owns the port
(`transport/byte_source.h` — Android cannot open `/dev/ttyUSB*`, the Qt
desktop already owns a `QSerialPort`) and hands chunks to `push_bytes()`; the
one thing the driver sends back is the report-rate command, through
`UsbSerialConfig::write_fn`.

Three things make it unlike every other driver here:

1. **No PageStore.** IMU is not geometry. Samples go to a bounded ring
   (`drain()`) and a sink; the Engine's shim feeds the sink to `ImuIngest` →
   `LioOdometry::push_imu`.
2. **No device clock, no sample counter on the wire.** Arrival time is the
   only clock and arrivals are bursty (USB serial hands the host 3–10 frames
   at once), so every sample carries **both** `t_arrival_ns` (unmodified) and
   `t_stamped_ns` (de-bursted, §3). Consumers use the stamped time; the raw
   arrival is kept so a recording can be re-stamped later with a better
   model without re-capturing.
3. **The blackout is surfaced, never smoothed.** The ~2.9 s stall is module
   firmware — it is not USB, not the host, not load (all three were ruled out
   by measurement in the ROS work: data resumes at the normal rate with no
   backlog). The driver counts blackouts, reports `kDegraded` while one is in
   progress, and records the worst. A driver that filled the hole would hand
   the LIO three seconds of invented motion.

## 2. The wire

115200 8N1. Frame: `7E 23 <len> <func> <payload…> <sum8>`, where `<len>`
counts the whole frame and `sum8` is the low byte of the sum of every
preceding byte. The module emits four frame types unprompted and cannot be
told not to:

| func | Payload | Used |
| --- | --- | --- |
| `0x04` raw IMU | 9 × int16 LE: accel ×3 (×16/32767 g), gyro ×3 (×2000/32767 °/s), mag ×3 (×800/32767 µT) | **yes** — the only one the LIO wants |
| `0x16` quaternion | 16 B | counted, ignored |
| `0x26` Euler | 12 B | counted, ignored |
| `0x32` barometer | 16 B | counted, ignored |

Rate command: `7E 23 07 60 <hz> 5F <sum8>`, 10–100 Hz; the module persists
it, so sending 100 at every `start()` is idempotent. Default from power-on is
25 Hz.

`juxi::FrameParser` is a byte-stream state machine (the vendor's
`IMU_UART_Process`, re-done to reassemble across arbitrary chunk boundaries),
with per-func counters, `checksum_failures` and `resyncs`. `decode_raw_imu`
returns gyro in **rad/s** and accel in **g** — g exactly as the device
reports it, because the g → m/s² conversion is `ImuIngest::add_g`'s business,
the same rule the Mid-360 IMU follows.

## 3. Timing: `ImuStamper`

A line-for-line port of the ROS driver's `_stamp()` /
`_restart_period_estimate()` / `_update_period_estimate()`:

* the output stamp advances by a **measured** period from the last stamp
  (phase-locked to arrivals with gain 0.02), not by the nominal 10 ms — the
  module's real rate was 2500 ppm off nominal and a nominal-period model
  drifts by that much;
* a gap longer than `max_gap_ms` *snaps* to the arrival (an outage is an
  outage);
* a backwards host arrival (clock step) is counted and the stamper
  free-runs until the host catches up — output stays strictly monotonic;
* the period estimate re-anchors after more than 1 s of silence;
* a sample that would lead its arrival by more than `max_lead_ms` (50) is
  clamped and counted as a *deep burst*.

`nominal_hz` is overwritten from `ImuSerialConfig::report_rate_hz` in the
driver's constructor so the seed and the rate we asked the module for can
never disagree.

Measured in `tests/test_imu_serial_driver.cpp` — the same three scenarios the
ROS simulation ran, with the same numbers:

| Scenario | Result |
| --- | --- |
| (a) 10-deep bursts, true 108 Hz, nominal 100 Hz | mean dt −0.026 % from truth (limit 1 %); learned period +1.547 % (limit 2 %); 0 clamps; 85 gap snaps; 1330 deep bursts. Output is 7.4 % away from the nominal 10 ms — the drift the measured-period model removes. |
| (b) 3 s outage | learned period unchanged before and after (+0.000 %); exactly 1 gap snap, worst 3.000 s; 0 clamps. |
| (c) 1.0 s host clock step back | 1 `clock_step_back` (worst 0.990 s); 0 clamps; minimum dt 10.0000 ms (no sub-millisecond sample); output strictly monotonic. |

## 4. The driver

`ImuSerialConfig`: `serial` (`UsbSerialConfig`), `report_rate_hz` 100,
`send_rate_command` true, `blackout_threshold_s` 0.5 (under the module's own
2.9 s so the stall is always seen, over any scheduling hiccup),
`min_checksum_pass_rate` 0.99 over `health_min_frames` 200, `ring_capacity`
2048 (~20 s), `stamper`, `internal_supervisor_thread`.

`start()` encodes the rate command through `write_fn` when present
(out-of-range Hz is refused with a warning rather than clamped, because the
stamper is seeded from the rate we believe we asked for). `push_bytes()` →
parser → for each `0x04` frame: stamper → `ImuSerialSample` → ring + sink,
all on the caller's thread with no driver lock held during the sink call.
`tick()` (supervisor thread every 50 ms, or a test's scripted clock) runs the
blackout watchdog and the per-window health snapshot.

Blackout counting is dual-path: `tick()` counts one as soon as silence passes
the threshold (a module that never returns is still visible), and the resume
records the true duration without double-counting; a driver that is only
ever pushed bytes still counts exactly one per blackout. Both paths are
tested.

Health: `kStreaming` on the first sample; `kDegraded` while a blackout is in
progress or when the checksum pass rate drops under 0.99 (on a 23-byte frame
over a short 115200 link that means the wire or the baud is wrong, not bad
luck). `DeviceHealth` carries no free text, so rate, blackouts, worst blackout
and the parser counters are read through `Engine::imu_serial_stats(id)`.

## 5. Engine wiring

* `DeviceKind::kImuSerial`, `StreamId::kImuSerial` — **not** `kImu`, which
  the offline pipelines read as "this is a Mid-360 project".
* `Engine::Impl::imu_serial` is a **second** `ImuIngest`, on `kImuSerial`.
  `stream_has_device_clock(kImuSerial)` is false, so A4 gives it the
  passthrough estimator: `add_g(t_stamped, t_stamped, gyro, acc)` returns the
  stamped time unchanged, converts g → m/s², and `on_imu_serial` pushes the
  result to the live `LioOdometry`. It is a separate `ImuIngest` because an
  `ImuIngest` maps through one stream's offset and the Mid-70's clock has
  nothing to do with this module's.
* Record-always: `Engine::push_serial_bytes()` records the pushed buffer as
  `ChunkType::kImuSerialRaw` in **`streams/imu_serial.bin`** (its own file for
  the `kImuPhone` reason — `imu.bin` means Mid-360 — and because these are
  raw UART bytes no reader of SDK2 datagrams could parse). Replay is
  `push_serial_bytes()` again; the parser reassembles frames across whatever
  chunking the recording had.
* C ABI 13: `SCAN_DEVICE_IMU_SERIAL = 6`, `SCAN_STREAM_IMU_SERIAL = 13`; the
  device reuses the D6 serial fields verbatim (`serial_port_name`,
  `serial_baud` 0 ⇒ 115200, `serial_write` for the rate command,
  `send_start_stop_commands` 0 ⇒ do not send it) plus
  `imu_serial_report_rate_hz` (0 ⇒ 100).
* Discovery: `discovery::ProbeSerialJuxiImu` sniffs `7E 23` frames at 115200
  and **never writes** (the UM982 pattern); it runs after the D6/STL-27L
  probes and before the UM982's.

## 6. Verification (2026-09-07)

`tests/test_imu_serial_driver.cpp`: 18 cases, 42 112 assertions, `imu_serial/*`,
built on a **second encoder written from the vendor reference's frame layout**
(`juxi_ref::frame()` never calls the driver's own `encode_*`/`checksum8`) —
the `packet_builder.h` ethos. Covers: frame encode/decode cross-check, sum8,
chunk-boundary splits at every offset, checksum rejection and resync, the
other three funcs counted-not-decoded, the rate command bytes, the three
stamper scenarios in §3, ring overflow accounting, blackout counting on both
paths, health transitions. Full `ctest -LE "sim|sim-rtk"`: 6/6.

`engine_cli --replay <capture.imudump> --sensor imu-serial` replays the
`IMUSRCAP` container from `tools/remote-capture/capture_serial_imu.py` record
by record with the recorded arrival stamps, and prints frames/s and an
arrival-gap census (blackouts > 0.5 s, worst, and the stall-to-stall period);
`verify_capture.py --type imuserial` does the same offline and expects at
least one blackout on any capture ≥ 40 s. A plain byte file is accepted too
(chunked, no stamps).

## 7. What is still hardware-only

1. `tests/integration/data/juxi_imu_60s.bin` — a real 60 s container
   spanning at least one blackout, for the replay test. The tool exists; the
   module is unplugged today.
2. The rate command actually taking effect through the desktop's
   `QSerialPort` write bridge (the bytes are tested; the module's reaction is
   not).
3. Whether the module's stall is also present when it is read over I²C by a
   bridge MCU (the P4 design) — unknown, and the reason that design keeps a
   bench test before any commitment. It changes nothing here: the driver
   surfaces whatever the wire does.
4. Mounting/axis convention relative to the Mid-70 and the LIO's extrinsic
   — a rig-level calibration, not a driver property, and not yet done.
