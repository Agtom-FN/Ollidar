# A4 — time sync and IMU ingestion

**Scope:** `engine/include/scanengine/timesync/`, `engine/src/timesync/`,
`engine/tests/test_timesync.cpp`.
**Spec:** §3.2. **Contract:** `engine/DESIGN.md` §6 ("A4 (timesync)").
**Depends on:** S6 `spikes/s6-calibration/REPORT.md` §6 (the error budget this
module's `jitter_ns` is measured against).

A1 shipped the seam: `OffsetEstimator`, a `PassthroughOffsetEstimator`
placeholder, and `TimeSync::set_estimator()`. A4 fills it in. Nothing upstream
had to change — `Engine::Impl` still owns one `TimeSync`, `DriverContext` still
hands it to drivers — but the default estimator for a stream that has its own
clock is now real, `jitter_ns` is a measurement instead of a decoration, and a
device that reboots mid-session no longer poisons every timestamp after it.

---

## 1. Why this matters more than it looks

S6 measured the colorization error budget and found that **time sync, not
calibration, is what breaks it** (REPORT §6.1). At 3 m, walking 1 m/s, turning
30 °/s, against a 20.2 px budget:

| residual jitter | Mid-360 error | verdict |
| --- | --- | --- |
| 5 ms | 15.4 px | colorize |
| 15 ms | 22.0 px | only with motion-gated keyframes (→ 16.2 px) |
| 30 ms | 36.0 px | do not colorize |

The sync × turn-rate term alone is 16.7 px at 15 ms — **83 % of the whole
budget**. A11 has to make a go/no-go decision per session against those
thresholds, and it can only do that if this module reports an honest number.
A placeholder that reports "the last residual was 100 ns" is worse than
useless there: it reads as `kGood` on a stream that is 40 ms out.

---

## 2. The algorithm, and why this one

### 2.1 The model

For every packet we hold a pair `(t_device, t_arrival)`:

```
delta = t_arrival − t_device = offset + drift·t_device + q,     q ≥ 0
```

`q` is the transport delay: USB scheduling, CH340 chunking, UDP queueing, the
OS handing the app a buffer. **It is one-sided.** That single fact decides the
estimator.

### 2.2 Why not the median the spec names

Spec §3.2 says "drift-tracked median filter". A median is the right tool for
two-sided noise and the wrong one here: it tracks the *middle* of the delay
distribution, so it is biased by the whole distribution rather than by its
floor, and it moves whenever congestion changes the shape of that
distribution. Concretely, for the two transports the engine actually has:

* **CH340 USB serial** (COIN-D6 today, any UART GNSS later): the host delivers
  ~64-byte chunks, so a burst of samples shares one arrival stamp and the
  earlier ones look late by up to a full chunk time. The measured cost in
  `test_timesync.cpp` case 3, with 16-sample bursts (80 ms of batching):
  **min-delay filter 53 µs of error, last-arrival estimator 76 ms.**
* **Mid-360 UDP**: the sensor batches points per datagram and the OS coalesces
  datagrams; Wi-Fi/scheduler stalls add tens of milliseconds to single
  samples. A median downweights those; a min filter ignores them.

So the estimator tracks the **lower envelope** of `delta`, which is the
standard treatment for one-way timestamp streams (NTP's clock filter, Paxson's
and Moon–Skelly–Towsley's linear-programming line fit, Zhang–Liu–Xia's convex
hull). Spec §3.2's intent — "per-stream device-timestamp offset with drift
tracking" — is met; the statistic is a lower quantile rather than the median,
and that is a deliberate, measured departure.

### 2.3 What is implemented

`MinDelayOffsetEstimator` (`timesync/min_delay_estimator.h`):

1. **Windowed minimum.** One lower-envelope point per `window_ns` (500 ms),
   kept in a ring of `windows` (64) → 32 s of history, O(1) per pair.
   Bounded memory is the reason for windows: an exact convex hull cannot
   *forget*, and a crystal whose drift changes sign as the device warms up
   needs the estimator to forget.
2. **Drift.** Least squares through the envelope points → slope, clamped to
   ±500 ppm. The per-window minimum carries the same bias in every window, so
   it lands in the intercept and leaves the slope unbiased.
3. **Robust drift.** A second least-squares pass over the 75 % of envelope
   points closest to the line. Least squares has no breakdown point: a one
   second block of 100 ms delays at the end of the history tilted the slope
   enough to move the mapping by **712 µs** — with the trimmed pass it moves
   by **1.2 µs** (`test_timesync.cpp` case 4).
4. **Envelope intercept.** *Not* the least-squares intercept. The line is
   pushed down until it touches the lowest envelope point:
   `intercept = min_i(delta_i − slope·t_i)`. This is the LP estimator's
   constraint — no packet arrived before it was sent — and it is what makes
   the offset immune to congestion. With a least-squares intercept the same
   congestion episode moved the offset by **11.9 ms**, more than half the S6
   budget, which is precisely the failure a min filter exists to prevent.
5. **Discontinuity detection.** §5 below.

### 2.4 Configuration

Every number is a `MinDelayConfig` field with the reasoning in the header.

| field | default | why |
| --- | --- | --- |
| `window_ns` | 500 ms | one envelope point per window |
| `windows` | 64 | 32 s of history: 20 ppm ⇒ 640 µs of drift over the span, far above the ~50 µs envelope noise |
| `residual_window` | 512 pairs | jitter reflects *recent* conditions (2.5 s at 200 Hz), never the session average |
| `converged_span_ns` / `converged_min_windows` | 2 s / 4 | drift evidence before drift is believed |
| `step_threshold_ns` | 250 ms | ~5× the worst stall S2-sim saw, 8× the top of the spec's jitter range |
| `step_confirm` | 3 pairs | a stall is transient, a clock step is not |
| `backward_tolerance_ns` | 200 ms | below this a backwards stamp is UDP reordering |
| `max_gap_ns` | 5 s | longer silence ⇒ rebuild rather than extrapolate |
| `unconverged_uncertainty_ns` | 30 ms | the pessimistic end of the spec's 5–30 ms range, so an A11 gate reading it before convergence fails *closed* |

---

## 3. What is estimated — and what can never be

The min filter recovers the clock line **up to the minimum one-way delay**,
which is not observable from one-way timestamps by any method. The reported
offset is therefore `true offset + min transport latency`.

That residue is **systematic, not jitter**: it does not change within a
session, it is identical for every sample, and it is exactly what S6 §7.1
item 1 asks A11's 8-second wizard sweep (cross-correlating a target's bearing
in camera and lidar) to measure and remove. `jitter_ns` deliberately does not
hide it inside itself, and every accuracy figure in the tests is stated
against `truth + base_latency` for the same reason.

**Consequence for A11:** the wizard's constant-offset estimate and this
module's `jitter_ns` are complementary and must both be applied — one removes
the bias, the other bounds what is left.

---

## 4. `jitter_ns`: the definition A11 budgets against

```
jitter_ns = p95(residuals) − p5(residuals)   over the last 512 accepted pairs,
            residual_i = delta_i − fitted_line(t_device_i)
```

The 90 % inter-quantile range of the one-way-delay distribution. Rationale:

* **Not the maximum** — a 50 ms scheduler stall once in a thousand packets is
  not the sync quality of the session; a max reports the tail as the norm.
* **Not a standard deviation** — the distribution is one-sided and
  heavy-tailed, so an sd is inflated by the same tail and has no interpretable
  relationship to the S6 thresholds.
* **A quantile range is also the honest uncertainty of the offset itself.**
  The true offset lies within `[estimate − min_delay, estimate]` and the only
  evidence we have about the scale of `min_delay` is the observed spread of
  delays. So the number does double duty: spread of the delay distribution,
  and bound on our own offset error.

Measured honesty (`test_timesync.cpp` case 2 — injected delay span → reported
`jitter_ns`), on a 20 s stream with a −12 ppm drift:

| injected | reported | `sync_quality()` |
| --- | --- | --- |
| 0.4 ms | 0.358 ms | `kGood` |
| 5 ms | 4.49 ms | `kGood` (S6: 15.4 px — colorize) |
| 15 ms | 13.7 ms | `kGated` (S6: 22.0 px — motion-gate keyframes) |
| 30 ms | 26.8 ms | `kPoor` (S6: 36.0 px — do not colorize) |

The test asserts each is inside `[0.6, 1.3]×` the truth and monotone in it.
For a uniform delay span the exact expectation is 0.9× — the estimator is
neither optimistic nor alarmist.

**`jitter_ns` is only meaningful when `converged` is true.** An estimator with
two samples can report a tiny spread and be wildly wrong. Consumers gate on
`sync_quality()`, which returns `kUnknown` unless converged, or on
`TimeModel::uncertainty_ns`, which reports the pessimistic 30 ms until then.

---

## 5. Steppy clocks

Three discontinuities, three treatments (`ResyncReason`):

| reason | trigger | response |
| --- | --- | --- |
| `kDeviceReset` | device stamp goes backwards by more than 200 ms | **immediate** rebuild from the current pair. One sample is enough evidence: nothing but a reboot does that, and holding the old mapping for even one more packet emits a timestamp wrong by the device's entire previous uptime. |
| `kClockStep` | \|residual\| > 250 ms for 3 consecutive pairs | withheld for 3 pairs, then rebuild. The withheld pairs are **not** folded into the model and `converged` is false while a step is pending, so a consumer sees "unsynchronised" rather than a wild value. |
| `kStreamGap` | silence longer than 5 s | rebuild, but **not** an error: a USB re-enumeration or an app pause looks like this. Logged, not published. |

`kDeviceReset` and `kClockStep` are published as `EventType::kError` with
`ScanError::kProtocolError` and the offending `StreamId` — provided somebody
installed a bus (§7). They are always logged at WARN and always counted in
`OffsetEstimate::resyncs`.

A delay spike is not a step: a full second of 100 ms congestion produces
**zero** resyncs, moves the offset by 1.2 µs, and shows up honestly as
`jitter_ns` ≈ 101 ms and `sync_quality() == kPoor` (case 4).

Reordering below the tolerance is counted (`Diagnostics::pairs_reordered`) and
ignored — it must not corrupt the window bookkeeping, and it is not a reboot.

---

## 6. Convergence budget

| milestone | budget | measured (200 Hz, 20 ppm, 3 ms latency, 5 ms jitter) |
| --- | --- | --- |
| first usable offset | 1 pair | the arrival stamp itself |
| offset error < 1 ms | 250 ms | **70 ms** |
| offset error < 100 µs | 1 s | 42 µs at 100 ms |
| `converged == true` | **3 s** | **2.24 s** |
| re-converged after a device reboot | 3 s | 2.24 s, worst error in the first 100 post-reboot samples 1.2 ms |

Convergence needs 4 closed windows spanning ≥ 2 s **and** ≥ 16 residuals, so a
low-rate stream takes longer in wall-clock terms — a 1 Hz GNSS stream needs
~16 s. That is a property of the evidence available, not of the filter, and it
is why the API reports `converged` instead of pretending.

**A capture session must not colorize on the first three seconds of data.**
That is not a regression: before A4 those seconds were mapped with a
placeholder that was wrong and did not say so.

---

## 7. API for A3 / A5 / A11

Everything is reachable from `DriverContext::timesync`.

```cpp
// Per packet, on the driver thread. O(1) amortized.
ts.add_pair(StreamId::kLidarMid360, t_device_ns, t_arrival);
// or, on a hot path, hold the reference and skip the registry lock:
OffsetEstimator& est = ts.estimator(StreamId::kLidarMid360);

// One sample.
const std::int64_t t_engine = ts.to_engine_time(StreamId::kLidarMid360, t_device_ns);

// A whole datagram of points: snapshot once, apply with no lock at all.
const TimeModel m = ts.model(StreamId::kLidarMid360);
for (auto& p : points) p.t_engine_ns = m.apply(p.t_device_ns);
if (!m.converged) { /* flag the batch: uncertainty is m.uncertainty_ns */ }

// The A11 gate. Never gate on jitter_ns alone.
switch (ts.quality(StreamId::kLidarMid360)) {
  case SyncQuality::kGood:  /* colorize */ break;
  case SyncQuality::kGated: /* colorize, keyframes below 15 °/s only */ break;
  case SyncQuality::kPoor:
  case SyncQuality::kUnknown: /* do not colorize */ break;
}
```

**Defaults per stream** (`TimeSync::stream_has_device_clock`): `kLidarMid360`,
`kImu` and `kGnss` get the min-delay estimator; `kLidarD6` (no device clock at
all — DESIGN §6), `kPoseAr` and `kCameraFrames` (ARCore is already
`CLOCK_BOOTTIME`, the engine's own domain) stay on passthrough. Override per
stream with `set_estimator()`; the resync hook is re-attached automatically.

**IMU** (`timesync/imu_ingest.h`). `ImuIngest` is the landing place for
`StreamId::kImu` — DESIGN §6 forbids IMU in the PageStore, and A6/A11/A5 all
need the same mapped samples:

```cpp
ImuIngest imu(*ctx.timesync);                 // ring, 2048 samples ≈ 10 s
imu.add_g(s.t_device_ns, TimePoint{s.t_mono_ns}, s.gyro, s.acc);  // Mid360ImuSink
double rad_s;
if (imu.angular_rate_at(keyframe_t_engine_ns, 250'000'000, &rad_s)) { /* motion gate */ }
```

`add()` takes m/s², `add_g()` takes g and converts — `mid360_driver.h` states
that converting the Mid-360's g-units is A4's business, so it happens here.
Both feed the estimator *and* return the sample with `t_engine_ns`,
`uncertainty_ns` and `time_converged` filled in, so a driver that only wants a
timestamp can ignore the ring. `angular_rate_at()` is S6 §6.3's motion gate:
mean |gyro| over a window ending at a keyframe, which converts the 15 ms case
from 22.0 px (fail) to 16.2 px (pass) for the price of a keyframe preference.

### Threading

`TimeSync` is safe from any thread. `add_pair()`/`estimator()` serialize on one
registry mutex, so a hot path should hold the `OffsetEstimator&` rather than
going through the registry per packet; `TimeModel::apply()` takes no lock at
all and is the intended batch path. Resync events are published from *inside*
`add_pair()`, so a callback-mode bus subscriber must not re-enter `TimeSync`
from one — the general event-bus rule (DESIGN §2), restated because this is a
new place it applies. `MinDelayOffsetEstimator::refit_()` never allocates.

---

## 8. Seams this task did NOT take (owner action required)

These need a one-line change in a file A4 does not own:

1. **`Engine::Impl` should call `impl_->timesync.set_event_bus(&impl_->bus)`**
   once at construction (`src/core/engine.cpp`). Without it a device-clock
   reboot is logged and counted but never reaches the app. Everything else
   works; this is the only wiring A4 could not do itself.
2. **A3's Mid-360 driver** should feed the estimator from both data paths —
   `ts.add_pair(StreamId::kLidarMid360, header.timestamp, t_arrival)` for
   point packets and an `ImuIngest` behind `Mid360Config::imu_sink` for IMU —
   and map point timestamps with `TimeModel::apply()` per datagram. The
   driver's own `Mid360ImuSample` ring can stay; `ImuIngest` is the mapped,
   engine-time view of it.
3. **A5's recorder** may want a `.lscan` sync chunk per resync
   (`TimeSync::set_resync_observer`) plus a periodic `OffsetEstimate` snapshot,
   so a replay can reproduce the mapping instead of re-deriving it.
4. **A11** should read `sync_quality()` per session (and per keyframe, via
   `TimeModel::converged`) rather than assuming the spec's 5–30 ms range.

Nothing above blocks anyone: the defaults are installed on first use, and an
unwired `TimeSync` still maps time correctly.

---

## 9. Tests

`engine/tests/test_timesync.cpp` — 17 cases, 225 assertions, all simulation
based with a fixed-seed xorshift64 (no `<random>` distribution: their output is
not specified by the standard and the five CI legs would disagree).

| # | case | asserts |
| --- | --- | --- |
| 1 | 200 Hz, 20 ppm, 5 ms jitter, 50 ms spikes, 60 s | offset −4.0 µs, worst 346 µs after convergence, drift 19.80 ppm, jitter 4.56 ms, no false resyncs |
| 2 | jitter honesty at 0.4 / 5 / 15 / 30 ms | reported within [0.6, 1.3]× truth, monotone, `sync_quality()` matches the S6 verdicts |
| 3 | 16-sample bursts (CH340 / UDP batching) | min-delay 53 µs vs passthrough 76 ms |
| 4 | 1 s of 100 ms congestion | offset moves 1.2 µs, 0 resyncs, jitter → 101 ms, quality → `kPoor` |
| 5 | device reboot at 30 s | one `kDeviceReset`, worst error 1.2 ms over the next 100 samples, re-converged at 2.24 s |
| 6 | +1 s device clock step | confirmed after 3 pairs, `converged` false meanwhile, absorbed, exactly one event |
| 7 | cold start | < 1 ms at 70 ms, converged at 2.24 s, and *not* converged before 1.5 s |
| 8 | 30 s silence | `kStreamGap`, no error event, stays accurate |
| 9 | reordering / stale duplicates | counted, ignored, no resync; a 10 s-old stamp *is* a reset |
| 10–13 | `TimeSync` facade | per-stream defaults, identity mapping for unknown streams, `model()` ≡ `to_engine_time()`, drift actually applied on extrapolation, `kError` published on the bus with the right stream and device |
| 14–15 | `ImuIngest` | mapping accuracy 345 µs at 200 Hz, rate 200.02 Hz, ring/overwrite semantics, `angular_rate_at()` recovers 10 °/s and 45 °/s to 1 %, g→m/s² |
| 16–17 | passthrough unchanged | A1's contract, which `test_core.cpp` also asserts |

`test_core.cpp`'s `timesync/registry_creates_a_passthrough_estimator_per_stream`
passes **unchanged** against the new default estimator: on the first pair the
min filter reports the arrival delta exactly, and on the second its residual
spread is the same 100 ns the A1 test expects. The A1 contract was not
renegotiated to make A4 fit.
