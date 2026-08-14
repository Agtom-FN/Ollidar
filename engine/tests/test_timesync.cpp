// Time sync (task A4): the min-delay offset estimator, the TimeSync facade
// and the IMU ingestion path.
//
// These headers come first on purpose: it makes this translation unit the
// self-containment check for the three A4 headers (tests/test_headers.cpp is
// A1's file and does not know about them).
#include "scanengine/timesync/imu_ingest.h"
#include "scanengine/timesync/min_delay_estimator.h"
#include "scanengine/timesync/offset_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/core/event_bus.h"

using namespace scanengine;

namespace {

// --- deterministic simulation --------------------------------------------
//
// Nothing here uses <random>'s distributions: their output is not specified
// by the standard, so libstdc++, libc++ and MSVC would each assert against a
// different stream and the CI matrix would disagree about the numbers. A
// xorshift64 plus modulo is reproducible everywhere.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  // Uniform in [0, span] ns.
  std::int64_t upto(std::int64_t span) {
    if (span <= 0) return 0;
    return static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(span + 1));
  }
};

// Ground truth for one device stream:
//   engine_time(d) = engine0 + (d − device0)·(1 + drift_ppm·1e−6)
//   arrival(d)     = engine_time(d) + base_latency + U[0, jitter_span] (+ spikes)
//
// base_latency is the minimum one-way delay. It is NOT recoverable from
// one-way stamps by any estimator, so every accuracy assertion below is
// written against (truth + base_latency); see engine/docs/A4-timesync.md §3.
struct ClockSim {
  std::int64_t device0_ns = 1'000'000'000;               // device booted 1 s ago
  std::int64_t engine0_ns = 400'000'000'000'000'000LL;   // mach_absolute_time-like
  double drift_ppm = 0.0;
  std::int64_t base_latency_ns = 0;
  std::int64_t jitter_span_ns = 0;
  std::int64_t spike_ns = 0;
  std::uint32_t spike_every = 0;   // 0 = never
  std::uint32_t burst = 1;         // n samples share one arrival stamp

  std::int64_t true_engine(std::int64_t t_device_ns) const {
    const double dt = static_cast<double>(t_device_ns - device0_ns);
    return engine0_ns + (t_device_ns - device0_ns) +
           static_cast<std::int64_t>(std::llround(dt * drift_ppm * 1e-6));
  }
};

struct Feeder {
  ClockSim sim;
  Rng rng{0xC0FFEE123456789ull};
  std::int64_t period_ns = 5'000'000;  // 200 Hz
  std::uint64_t index = 0;
  std::int64_t device_ns = 0;
  std::int64_t burst_arrival = 0;

  explicit Feeder(const ClockSim& s, std::int64_t period = 5'000'000)
      : sim(s), period_ns(period), device_ns(s.device0_ns) {}

  // Next (device stamp, arrival stamp) pair.
  void step(std::int64_t* t_device, TimePoint* t_arrival) {
    const std::int64_t d = device_ns;
    std::int64_t a = sim.true_engine(d) + sim.base_latency_ns + rng.upto(sim.jitter_span_ns);
    if (sim.spike_every != 0 && (index % sim.spike_every) == sim.spike_every - 1) {
      a += sim.spike_ns;
    }
    // Chunked transports (CH340 64-byte reads, coalesced UDP) hand several
    // samples to the host at the same instant: the later ones look late.
    if (sim.burst > 1) {
      if (index % sim.burst == 0) burst_arrival = a + static_cast<std::int64_t>(sim.burst - 1) * period_ns;
      a = burst_arrival;
    }
    *t_device = d;
    *t_arrival = TimePoint{a};
    device_ns += period_ns;
    ++index;
  }
};

// Mapping error with the unobservable minimum one-way delay removed.
std::int64_t map_error_ns(const OffsetEstimator& est, const ClockSim& sim,
                          std::int64_t t_device_ns) {
  return est.to_engine_time(t_device_ns) - sim.true_engine(t_device_ns) - sim.base_latency_ns;
}

std::int64_t abs64(std::int64_t v) { return v < 0 ? -v : v; }

struct ResyncLog {
  std::vector<ClockResync> events;
  static void sink(const ClockResync& ev, void* user) {
    static_cast<ResyncLog*>(user)->events.push_back(ev);
  }
};

const std::int64_t kMs = 1'000'000;

}  // namespace

// ---------------------------------------------------------------------------
// 1. The headline case: 200 Hz stream, 20 ppm drift, 5 ms burst jitter,
//    occasional 50 ms delay spikes, one minute of data.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/recovers_offset_drift_and_jitter") {
  ClockSim sim;
  sim.drift_ppm = 20.0;
  sim.base_latency_ns = 3 * kMs;
  sim.jitter_span_ns = 5 * kMs;
  sim.spike_ns = 50 * kMs;
  sim.spike_every = 97;

  Feeder f(sim);
  MinDelayOffsetEstimator est;

  std::int64_t worst_after_convergence = 0;
  std::int64_t worst_at_ns = 0;
  std::int64_t converged_at_ns = -1;
  std::int64_t usable_at_ns = -1;
  const std::int64_t n = 60 * 200;  // 60 s at 200 Hz

  for (std::int64_t i = 0; i < n; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);

    const std::int64_t err = map_error_ns(est, sim, d);
    const std::int64_t elapsed = d - sim.device0_ns;
    if (usable_at_ns < 0 && abs64(err) < 1 * kMs) usable_at_ns = elapsed;
    if (converged_at_ns < 0 && est.estimate().converged) converged_at_ns = elapsed;
    if (converged_at_ns >= 0 && abs64(err) > worst_after_convergence) {
      worst_after_convergence = abs64(err);
      worst_at_ns = elapsed;
    }
  }

  const OffsetEstimate e = est.estimate();
  const MinDelayOffsetEstimator::Diagnostics d = est.diagnostics();
  const std::int64_t final_err = map_error_ns(est, sim, f.device_ns - f.period_ns);

  MESSAGE("A4 steady-state (200 Hz, 20 ppm, 5 ms jitter, 50 ms spikes every 97):");
  MESSAGE("  offset error      = " << final_err << " ns (worst after convergence "
                                   << worst_after_convergence << " ns at "
                                   << worst_at_ns / kMs << " ms)");
  MESSAGE("  drift             = " << e.drift_ppm << " ppm (truth 20.0)");
  MESSAGE("  jitter_ns         = " << e.jitter_ns << " (truth: 5 ms uniform span)");
  MESSAGE("  usable (<1 ms) at = " << usable_at_ns / kMs << " ms, converged at "
                                   << converged_at_ns / kMs << " ms");
  MESSAGE("  quality           = " << std::string(to_string(sync_quality(e)))
                                   << ", pairs accepted " << d.pairs_accepted << ", rejected "
                                   << d.pairs_rejected);

  CHECK(e.valid);
  CHECK(e.converged);
  CHECK(e.resyncs == 0);

  // Offset: recovered to well under a tenth of the jitter, and never below
  // the truth (a min filter cannot claim data arrived before it was sent).
  CHECK(final_err >= -50'000);
  CHECK(abs64(final_err) < 500'000);
  CHECK(worst_after_convergence < 1 * kMs);

  // Drift: 20 ppm recovered from a 32 s window of envelope points.
  CHECK(std::abs(e.drift_ppm - 20.0) < 3.0);

  // Jitter brackets the truth: the 90 % inter-quantile range of a U[0, 5 ms]
  // delay is 4.5 ms, and the 50 ms spikes must not be allowed to inflate it.
  CHECK(e.jitter_ns > 3 * kMs);
  CHECK(e.jitter_ns < 7 * kMs);
  CHECK(sync_quality(e) == SyncQuality::kGood);

  // Spikes were seen and refused, without ever being mistaken for a step.
  CHECK(d.pairs_accepted > static_cast<std::uint64_t>(n) * 9 / 10);
  CHECK(d.resyncs == 0);
}

// ---------------------------------------------------------------------------
// 2. jitter_ns is honest across the range S6 budgets against.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/jitter_brackets_truth_at_the_S6_thresholds") {
  struct Case {
    std::int64_t span_ns;
    SyncQuality expect;
  };
  const Case cases[] = {
      {400'000, SyncQuality::kGood},        // 0.4 ms — a clean USB stream
      {5 * kMs, SyncQuality::kGood},        // S6: 15.4 px at 3 m — colorize
      {15 * kMs, SyncQuality::kGated},      // S6: 22.0 px — motion-gate keyframes
      {30 * kMs, SyncQuality::kPoor},       // S6: 36.0 px — do not colorize
  };

  std::int64_t previous = -1;
  for (const Case& c : cases) {
    ClockSim sim;
    sim.drift_ppm = -12.0;
    sim.base_latency_ns = 2 * kMs;
    sim.jitter_span_ns = c.span_ns;
    Feeder f(sim);
    MinDelayOffsetEstimator est;
    for (int i = 0; i < 20 * 200; ++i) {
      std::int64_t d = 0;
      TimePoint a{};
      f.step(&d, &a);
      est.add_pair(d, a);
    }
    const OffsetEstimate e = est.estimate();
    MESSAGE("  jitter truth " << c.span_ns / 1000 << " us -> reported " << e.jitter_ns / 1000
                              << " us (" << std::string(to_string(sync_quality(e))) << ")");
    CHECK(e.converged);
    // Within [0.6, 1.3] of the injected span: honest, neither optimistic nor
    // alarmist. The 90 % inter-quantile range of a uniform span is 0.9 of it.
    CHECK(e.jitter_ns > c.span_ns * 6 / 10);
    CHECK(e.jitter_ns < c.span_ns * 13 / 10);
    CHECK(sync_quality(e) == c.expect);
    CHECK(e.jitter_ns > previous);  // monotone in the truth
    previous = e.jitter_ns;
  }
}

// ---------------------------------------------------------------------------
// 3. Bursty arrivals (CH340 chunking / UDP coalescing) must not bias offset.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/bursty_arrivals_do_not_bias_the_offset") {
  ClockSim sim;
  sim.drift_ppm = 8.0;
  sim.base_latency_ns = 1 * kMs;
  sim.jitter_span_ns = 1 * kMs;
  sim.burst = 16;  // 16 samples handed over at once — 80 ms of batching at 200 Hz

  Feeder f(sim);
  MinDelayOffsetEstimator min_est;
  PassthroughOffsetEstimator naive;  // what A1 shipped: last arrival − last stamp

  // The error has to be sampled at every pair, not just at the end of the
  // run: a last-arrival estimator happens to be exactly right on the last
  // sample of a burst and maximally wrong on the first.
  std::int64_t min_worst = 0, naive_worst = 0;
  for (int i = 0; i < 20 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    min_est.add_pair(d, a);
    naive.add_pair(d, a);
    if (i > 5 * 200) {
      min_worst = std::max(min_worst, abs64(map_error_ns(min_est, sim, d)));
      naive_worst = std::max(naive_worst, abs64(map_error_ns(naive, sim, d)));
    }
  }
  const std::int64_t last_device = f.device_ns - f.period_ns;
  const std::int64_t min_err = abs64(map_error_ns(min_est, sim, last_device));

  MESSAGE("  burst of 16 (80 ms of batching): min-delay worst " << min_worst / 1000
                                                                << " us vs passthrough "
                                                                << naive_worst / 1000 << " us");
  CHECK(min_err < 500'000);
  CHECK(min_worst < 500'000);      // and it stays there through every burst
  CHECK(naive_worst > 50 * kMs);   // the naive estimator eats the whole burst
}

// ---------------------------------------------------------------------------
// 4. A congestion episode must move jitter, not the offset, and must not be
//    mistaken for a clock step.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/delay_spikes_do_not_move_the_offset") {
  ClockSim sim;
  sim.base_latency_ns = 2 * kMs;
  sim.jitter_span_ns = 2 * kMs;
  Feeder f(sim);
  MinDelayOffsetEstimator est;

  for (int i = 0; i < 10 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);
  }
  const std::int64_t before = est.estimate().offset_ns;

  // 1 s of 100 ms congestion (below the 250 ms step threshold on purpose).
  for (int i = 0; i < 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, TimePoint{a.nanos + 100 * kMs});
  }
  const OffsetEstimate during = est.estimate();

  MESSAGE("  100 ms congestion: offset moved " << (during.offset_ns - before)
                                               << " ns, jitter now " << during.jitter_ns / 1000
                                               << " us");
  CHECK(abs64(during.offset_ns - before) < 200'000);  // 0.2 ms
  CHECK(during.resyncs == 0);
  CHECK(during.jitter_ns > 50 * kMs);                 // honest: it says it is bad now
  CHECK(sync_quality(during) == SyncQuality::kPoor);
}

// ---------------------------------------------------------------------------
// 5. Device reboot mid-session.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/device_reboot_is_detected_and_reconverges") {
  ClockSim sim;
  sim.drift_ppm = 20.0;
  sim.base_latency_ns = 3 * kMs;
  sim.jitter_span_ns = 5 * kMs;

  Feeder f(sim);
  MinDelayOffsetEstimator est;
  est.set_stream(StreamId::kLidarMid360);
  ResyncLog log;
  est.set_resync_observer(&ResyncLog::sink, &log);

  for (int i = 0; i < 30 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);
  }
  REQUIRE(est.estimate().converged);
  const std::int64_t reboot_engine = sim.true_engine(f.device_ns);

  // The device reboots: its timestamp counter restarts near zero while the
  // engine clock keeps going. Same physical stream, brand new clock.
  ClockSim after = sim;
  after.device0_ns = 2'000'000;  // 2 ms since the device's own boot
  after.engine0_ns = reboot_engine + 900 * kMs;  // 0.9 s of downtime
  Feeder f2(after);

  std::int64_t worst_first_100 = 0;
  std::int64_t reconverged_at = -1;
  for (int i = 0; i < 10 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f2.step(&d, &a);
    est.add_pair(d, a);
    const std::int64_t err = abs64(map_error_ns(est, after, d));
    const std::int64_t elapsed = d - after.device0_ns;
    if (i < 100) worst_first_100 = std::max(worst_first_100, err);
    if (reconverged_at < 0 && est.estimate().converged) reconverged_at = elapsed;
  }

  const OffsetEstimate e = est.estimate();
  MESSAGE("  reboot: " << log.events.size() << " resync event(s), reason "
                       << std::string(log.events.empty() ? "none" : to_string(log.events[0].reason))
                       << ", worst error in first 100 samples " << worst_first_100 / 1000
                       << " us, re-converged at " << reconverged_at / kMs << " ms");

  REQUIRE(log.events.size() == 1);
  CHECK(log.events[0].reason == ResyncReason::kDeviceReset);
  CHECK(log.events[0].stream == StreamId::kLidarMid360);
  CHECK(log.events[0].index == 1);
  CHECK(e.resyncs == 1);

  // The whole point: a rebooted device must never produce a mapping that is
  // wrong by the size of the device's old uptime (30 s here).
  CHECK(worst_first_100 < 10 * kMs);
  CHECK(reconverged_at >= 0);
  CHECK(reconverged_at < 3'000 * kMs);
  CHECK(abs64(map_error_ns(est, after, f2.device_ns - f2.period_ns)) < 1 * kMs);
  CHECK(e.converged);
}

// ---------------------------------------------------------------------------
// 6. A forward clock step is confirmed before it is believed, and nothing
//    wild is published in the meantime.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/forward_clock_step_is_confirmed_then_absorbed") {
  ClockSim sim;
  sim.base_latency_ns = 1 * kMs;
  sim.jitter_span_ns = 2 * kMs;
  Feeder f(sim);
  MinDelayOffsetEstimator est;
  ResyncLog log;
  est.set_resync_observer(&ResyncLog::sink, &log);

  for (int i = 0; i < 20 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);
  }
  REQUIRE(est.estimate().converged);

  // The device's clock jumps 1 s forward; arrivals do not.
  const std::int64_t step = 1'000 * kMs;
  ClockSim stepped = sim;
  stepped.device0_ns = sim.device0_ns - step;  // same engine times, later stamps
  Feeder f2(stepped);
  f2.device_ns = f.device_ns + step;
  f2.index = f.index;
  f2.rng = f.rng;

  bool converged_during_pending = true;
  for (int i = 0; i < 3; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f2.step(&d, &a);
    est.add_pair(d, a);
    if (i < 2) converged_during_pending = converged_during_pending && est.estimate().converged;
  }
  // Two unconfirmed pairs must not have been folded in, and must be flagged.
  CHECK_FALSE(converged_during_pending);
  REQUIRE(log.events.size() == 1);
  CHECK(log.events[0].reason == ResyncReason::kClockStep);
  CHECK(abs64(log.events[0].step_ns) > 900 * kMs);

  for (int i = 0; i < 10 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f2.step(&d, &a);
    est.add_pair(d, a);
  }
  CHECK(est.estimate().converged);
  CHECK(abs64(map_error_ns(est, stepped, f2.device_ns - f2.period_ns)) < 1 * kMs);
  CHECK(log.events.size() == 1);
}

// ---------------------------------------------------------------------------
// 7. Cold start: how fast is the mapping usable, and when may it be trusted?
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/cold_start_convergence_budget") {
  ClockSim sim;
  sim.drift_ppm = 20.0;
  sim.base_latency_ns = 3 * kMs;
  sim.jitter_span_ns = 5 * kMs;
  Feeder f(sim);
  MinDelayOffsetEstimator est;

  std::int64_t err_at_100ms = -1, err_at_500ms = -1;
  bool converged_at_1500ms = false;
  std::int64_t converged_at = -1;

  for (int i = 0; i < 5 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);
    const std::int64_t elapsed = d - sim.device0_ns;
    const std::int64_t err = abs64(map_error_ns(est, sim, d));
    if (err_at_100ms < 0 && elapsed >= 100 * kMs) err_at_100ms = err;
    if (err_at_500ms < 0 && elapsed >= 500 * kMs) err_at_500ms = err;
    if (elapsed <= 1500 * kMs && est.estimate().converged) converged_at_1500ms = true;
    if (converged_at < 0 && est.estimate().converged) converged_at = elapsed;
  }

  MESSAGE("  cold start: |err| at 100 ms = " << err_at_100ms / 1000 << " us, at 500 ms = "
                                             << err_at_500ms / 1000 << " us, converged at "
                                             << converged_at / kMs << " ms");

  // The first sample already gives a usable offset (it is the arrival stamp);
  // one window of minima brings it under 1 ms.
  CHECK(err_at_100ms < 2 * kMs);
  CHECK(err_at_500ms < 1 * kMs);
  // ...but it must not claim convergence before it has drift evidence.
  CHECK_FALSE(converged_at_1500ms);
  CHECK(converged_at > 0);
  CHECK(converged_at <= 3'000 * kMs);  // the documented budget
}

// ---------------------------------------------------------------------------
// 8. A long silence is a soft resync, not an error.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/stream_gap_is_a_soft_resync") {
  ClockSim sim;
  sim.base_latency_ns = 1 * kMs;
  sim.jitter_span_ns = 2 * kMs;
  Feeder f(sim);
  MinDelayOffsetEstimator est;
  ResyncLog log;
  est.set_resync_observer(&ResyncLog::sink, &log);

  for (int i = 0; i < 10 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);
  }
  REQUIRE(est.estimate().converged);

  f.device_ns += 30'000 * kMs;  // 30 s of silence (app paused, USB re-enumerated)
  for (int i = 0; i < 10 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);
  }
  REQUIRE(log.events.size() == 1);
  CHECK(log.events[0].reason == ResyncReason::kStreamGap);
  CHECK(est.estimate().converged);
  CHECK(abs64(map_error_ns(est, sim, f.device_ns - f.period_ns)) < 1 * kMs);
}

// ---------------------------------------------------------------------------
// 9. Reordering and duplicates must not corrupt the model.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/min_delay/reordered_pairs_are_counted_and_ignored") {
  ClockSim sim;
  sim.base_latency_ns = 1 * kMs;
  sim.jitter_span_ns = 1 * kMs;
  Feeder f(sim);
  MinDelayOffsetEstimator est;

  for (int i = 0; i < 10 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    est.add_pair(d, a);
    if (i > 100 && (i % 50) == 0) {
      // A datagram from 20 ms ago turns up late.
      est.add_pair(d - 20 * kMs, TimePoint{a.nanos + 1 * kMs});
    }
  }
  const OffsetEstimate e = est.estimate();
  CHECK(e.resyncs == 0);
  CHECK(est.diagnostics().pairs_reordered > 0);
  CHECK(abs64(map_error_ns(est, sim, f.device_ns - f.period_ns)) < 500'000);

  // 150 ms late is still inside the reordering tolerance, so it must not be
  // mistaken for a reboot...
  const std::int64_t late = f.device_ns - 150 * kMs;
  est.add_pair(late, TimePoint{sim.true_engine(late) + sim.base_latency_ns + 150 * kMs});
  CHECK(est.estimate().resyncs == 0);

  // ...but a stamp from 10 s ago on a live stream is a device that restarted
  // its clock, and one sample is enough to say so.
  est.add_pair(f.device_ns - 10'000 * kMs, TimePoint{sim.true_engine(f.device_ns)});
  CHECK(est.estimate().resyncs == 1);
}

// ---------------------------------------------------------------------------
// 10. The TimeSync facade.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/registry/default_estimator_per_stream") {
  CHECK(TimeSync::stream_has_device_clock(StreamId::kLidarMid360));
  CHECK(TimeSync::stream_has_device_clock(StreamId::kImu));
  CHECK(TimeSync::stream_has_device_clock(StreamId::kGnss));
  // The D6 has no device clock (DESIGN §6) and ARCore is already in the
  // engine's clock domain: both stay on arrival stamps.
  CHECK_FALSE(TimeSync::stream_has_device_clock(StreamId::kLidarD6));
  CHECK_FALSE(TimeSync::stream_has_device_clock(StreamId::kPoseAr));
  CHECK_FALSE(TimeSync::stream_has_device_clock(StreamId::kCameraFrames));

  TimeSync ts;
  CHECK(dynamic_cast<MinDelayOffsetEstimator*>(&ts.estimator(StreamId::kImu)) != nullptr);
  CHECK(dynamic_cast<PassthroughOffsetEstimator*>(&ts.estimator(StreamId::kLidarD6)) != nullptr);

  // Unknown stream: mapping is the identity, never a wild value.
  CHECK(ts.to_engine_time(StreamId::kPoseFused, 12345) == 12345);
  CHECK_FALSE(ts.model(StreamId::kPoseFused).valid);
  CHECK(ts.quality(StreamId::kPoseFused) == SyncQuality::kUnknown);

  CHECK(ts.reset(StreamId::kPoseFused).error() == ScanError::kNotFound);
  CHECK(ts.reset(StreamId::kImu).ok());
  CHECK(ts.streams().size() == 2);
}

TEST_CASE("timesync/registry/maps_time_and_snapshots_the_model") {
  ClockSim sim;
  sim.drift_ppm = 20.0;
  sim.base_latency_ns = 2 * kMs;
  sim.jitter_span_ns = 4 * kMs;
  Feeder f(sim);

  TimeSync ts;
  for (int i = 0; i < 20 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    ts.add_pair(StreamId::kLidarMid360, d, a);
  }

  const OffsetEstimate e = ts.estimate(StreamId::kLidarMid360);
  CHECK(e.converged);
  CHECK(ts.quality(StreamId::kLidarMid360) == SyncQuality::kGood);

  // The batch path (TimeModel::apply) and the single-sample path must agree
  // exactly — A3 maps a datagram of points with the former.
  const TimeModel m = ts.model(StreamId::kLidarMid360);
  CHECK(m.valid);
  CHECK(m.converged);
  CHECK(m.uncertainty_ns == e.jitter_ns);
  for (int k = 0; k < 5; ++k) {
    const std::int64_t t = f.device_ns - k * 1'000'000;
    CHECK(m.apply(t) == ts.to_engine_time(StreamId::kLidarMid360, t));
    CHECK(abs64(m.apply(t) - sim.true_engine(t) - sim.base_latency_ns) < 1 * kMs);
  }

  // Drift is actually applied when extrapolating: 20 ppm over 10 s = 200 µs.
  const std::int64_t far = f.device_ns + 10'000 * kMs;
  CHECK(abs64((m.apply(far) - m.apply(f.device_ns)) - 10'000 * kMs) > 150'000);

  // An unconverged estimator reports the pessimistic uncertainty, so an A11
  // gate reading it fails closed.
  TimeSync fresh;
  fresh.add_pair(StreamId::kImu, 1000, TimePoint{5000});
  const TimeModel cold = fresh.model(StreamId::kImu);
  CHECK(cold.valid);
  CHECK_FALSE(cold.converged);
  CHECK(cold.uncertainty_ns == kUnconvergedUncertaintyNs);
  CHECK(cold.apply(2000) == 6000);
}

TEST_CASE("timesync/registry/set_estimator_overrides_and_keeps_the_hook") {
  TimeSync ts;
  MinDelayConfig cfg;
  cfg.window_ns = 100 * kMs;
  cfg.windows = 8;
  auto custom = std::make_unique<MinDelayOffsetEstimator>(cfg);
  MinDelayOffsetEstimator* raw = custom.get();
  CHECK(ts.set_estimator(StreamId::kLidarMid360, std::move(custom)).ok());
  CHECK(&ts.estimator(StreamId::kLidarMid360) == raw);
  CHECK(raw->config().window_ns == 100 * kMs);
  CHECK_FALSE(ts.set_estimator(StreamId::kImu, nullptr).ok());
}

TEST_CASE("timesync/registry/publishes_a_clock_discontinuity_on_the_bus") {
  EventBus bus;
  SubscriptionOptions opts;
  const Result<SubscriptionId> sub = bus.subscribe(opts);
  REQUIRE(sub.ok());

  TimeSync ts;
  ts.set_event_bus(&bus, 7);
  ResyncLog log;
  ts.set_resync_observer(&ResyncLog::sink, &log);

  ClockSim sim;
  sim.base_latency_ns = 1 * kMs;
  sim.jitter_span_ns = 1 * kMs;
  Feeder f(sim);
  for (int i = 0; i < 5 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    ts.add_pair(StreamId::kImu, d, a);
  }
  // Device reboot on the IMU stream.
  ts.add_pair(StreamId::kImu, 1'000'000, TimePoint{sim.true_engine(f.device_ns) + 1 * kMs});

  Event ev{};
  bool found = false;
  while (bus.poll(sub.value(), &ev)) {
    if (ev.type == EventType::kError) {
      found = true;
      CHECK(ev.payload.error.error == ScanError::kProtocolError);
      CHECK(ev.payload.error.stream == StreamId::kImu);
      CHECK(ev.payload.error.device == 7);
    }
  }
  CHECK(found);
  CHECK(ts.resyncs() == 1);
  REQUIRE(log.events.size() == 1);
  CHECK(log.events[0].stream == StreamId::kImu);
  CHECK(log.events[0].reason == ResyncReason::kDeviceReset);
}

// ---------------------------------------------------------------------------
// 11. IMU ingestion.
// ---------------------------------------------------------------------------
TEST_CASE("timesync/imu/samples_are_mapped_and_queryable") {
  ClockSim sim;
  sim.drift_ppm = 20.0;
  sim.base_latency_ns = 2 * kMs;
  sim.jitter_span_ns = 5 * kMs;
  Feeder f(sim);  // 200 Hz, the Mid-360's IMU rate

  TimeSync ts;
  ImuIngest imu(ts, StreamId::kImu, 2048);  // 10 s at 200 Hz

  // Turn slowly for 5 s, then fast for 5 s: the motion gate must see it.
  const double slow = 10.0 * 3.14159265358979 / 180.0;  // 10 °/s — S6 pass
  const double fast = 45.0 * 3.14159265358979 / 180.0;  // 45 °/s — S6 fail
  std::int64_t slow_engine = 0, fast_engine = 0;
  std::int64_t worst_err = 0;
  ImuSample last{};

  for (int i = 0; i < 10 * 200; ++i) {
    std::int64_t d = 0;
    TimePoint a{};
    f.step(&d, &a);
    const bool is_fast = i >= 5 * 200;
    const float gyro[3] = {0.0f, 0.0f, static_cast<float>(is_fast ? fast : slow)};
    const float accel[3] = {0.0f, 0.0f, 9.81f};
    last = imu.add(d, a, gyro, accel);
    if (i == 4 * 200) slow_engine = last.t_engine_ns;
    if (i == 9 * 200) fast_engine = last.t_engine_ns;
    if (last.time_converged) {
      worst_err = std::max(worst_err, abs64(last.t_engine_ns - sim.true_engine(d) -
                                            sim.base_latency_ns));
    }
  }

  const ImuIngestStats st = imu.stats();
  MESSAGE("  IMU: " << st.samples << " samples, rate " << st.rate_hz
                    << " Hz, uncertainty " << st.uncertainty_ns / 1000
                    << " us, worst mapping error " << worst_err / 1000 << " us");

  CHECK(st.samples == 10 * 200);
  CHECK(st.converged);
  CHECK(st.rate_hz > 190.0);
  CHECK(st.rate_hz < 210.0);
  CHECK(worst_err < 1 * kMs);
  CHECK(last.time_converged);
  CHECK(last.uncertainty_ns > 3 * kMs);
  CHECK(last.uncertainty_ns < 7 * kMs);
  CHECK(last.accel_m_s2[2] == doctest::Approx(9.81f));

  // The estimator was fed by the ingest path, not separately.
  CHECK(ts.estimate(StreamId::kImu).samples == 10 * 200);

  // Motion gate (S6 §6.3): mean |gyro| over the 250 ms before a keyframe.
  double rate = 0.0;
  REQUIRE(imu.angular_rate_at(slow_engine, 250 * kMs, &rate));
  CHECK(rate * 180.0 / 3.14159265358979 == doctest::Approx(10.0).epsilon(0.01));
  REQUIRE(imu.angular_rate_at(fast_engine, 250 * kMs, &rate));
  CHECK(rate * 180.0 / 3.14159265358979 == doctest::Approx(45.0).epsilon(0.01));
  CHECK_FALSE(imu.angular_rate_at(sim.engine0_ns - 10'000 * kMs, 1 * kMs, &rate));

  // Ring semantics: bounded, newest-wins, oldest-first on read.
  std::vector<ImuSample> buf(64);
  const std::size_t n = imu.recent(buf.data(), buf.size());
  CHECK(n == 64);
  for (std::size_t i = 1; i < n; ++i) CHECK(buf[i].t_engine_ns >= buf[i - 1].t_engine_ns);
  CHECK(buf[n - 1].t_engine_ns == last.t_engine_ns);
  CHECK(st.overwritten == 0);

  ImuSample latest{};
  CHECK(imu.latest(&latest));
  CHECK(latest.t_engine_ns == last.t_engine_ns);

  // The Mid-360 reports acceleration in g; add_g() is the seam A3's
  // Mid360ImuSink plugs into unchanged.
  const float gyro_zero[3] = {0.f, 0.f, 0.f};
  const float accel_g[3] = {0.f, 0.f, 1.0f};
  const ImuSample in_g = imu.add_g(f.device_ns, TimePoint{f.sim.true_engine(f.device_ns)},
                                   gyro_zero, accel_g);
  CHECK(in_g.accel_m_s2[2] == doctest::Approx(9.80665f));

  imu.reset();
  CHECK(imu.stats().samples == 0);
  CHECK_FALSE(imu.latest(&latest));
}

TEST_CASE("timesync/imu/out_of_order_samples_are_dropped_from_the_ring") {
  TimeSync ts;
  ImuIngest imu(ts, StreamId::kImu, 16);
  const float g[3] = {0.1f, 0.0f, 0.0f};
  const float acc[3] = {0.0f, 0.0f, 9.81f};
  for (int i = 0; i < 8; ++i) {
    imu.add(1'000'000'000 + i * 5 * kMs, TimePoint{2'000'000'000 + i * 5 * kMs}, g, acc);
  }
  // A stale sample from 15 ms ago: mapped for the caller, refused by the ring.
  const ImuSample s = imu.add(1'000'000'000 + 2 * 5 * kMs,
                              TimePoint{2'000'000'000 + 8 * 5 * kMs}, g, acc);
  CHECK(s.t_engine_ns > 0);
  CHECK(imu.stats().dropped_out_of_order == 1);
  CHECK(imu.stats().samples == 8);

  // The ring is bounded and overwrites oldest-first; a consumer that falls
  // behind loses history but never blocks the driver thread.
  for (int i = 8; i < 40; ++i) {
    imu.add(1'000'000'000 + i * 5 * kMs, TimePoint{2'000'000'000 + i * 5 * kMs}, g, acc);
  }
  CHECK(imu.stats().samples == 40);
  CHECK(imu.stats().overwritten == 40 - 16);
  ImuSample buf[16];
  CHECK(imu.recent(buf, 16) == 16);
  CHECK(buf[0].t_device_ns == 1'000'000'000 + 24 * 5 * kMs);
}

// ---------------------------------------------------------------------------
// 12. The A1 passthrough contract is unchanged (test_core.cpp asserts it too).
// ---------------------------------------------------------------------------
TEST_CASE("timesync/passthrough/still_tracks_the_last_arrival") {
  PassthroughOffsetEstimator est;
  est.add_pair(1000, TimePoint{5000});
  CHECK(est.estimate().offset_ns == 4000);
  CHECK(est.to_engine_time(2000) == 6000);
  est.add_pair(2000, TimePoint{6100});
  CHECK(est.estimate().jitter_ns == 100);
  CHECK(est.estimate().converged);
  est.reset();
  CHECK_FALSE(est.estimate().valid);
}
