# A2 — D6 driver hardening

**Scope:** `engine/src/drivers/d6/d6_driver.cpp`, `engine/include/scanengine/drivers/d6/d6_driver.h`,
`engine/tests/test_d6_driver.cpp`. The S1 parser (`d6_parser.h/.cpp`, `commands.h`) is
**untouched** — byte-identical to the spike, as required. Everything below lives in the
driver's integration layer around it.

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
