#include "scanengine/drivers/imu_serial/imu_stamper.h"

#include <cmath>

namespace scanengine {
namespace {

constexpr double kNsPerSec = 1e9;

std::int64_t to_ns(double seconds) {
  return static_cast<std::int64_t>(std::llround(seconds * kNsPerSec));
}

}  // namespace

ImuStamper::ImuStamper(const ImuStamperConfig& cfg) : cfg_(cfg) {
  const double hz = cfg_.nominal_hz > 0.0 ? cfg_.nominal_hz : 1.0;
  nominal_dt_ = 1.0 / hz;
  max_gap_s_ = cfg_.max_gap_ms / 1000.0;
  max_lead_s_ = cfg_.max_lead_ms / 1000.0;
  period_ = nominal_dt_;
  st_.learned_period_ns = to_ns(period_);
}

void ImuStamper::reset() {
  have_origin_ = false;
  origin_ns_ = 0;
  started_ = false;
  resync_ = false;
  period_ = nominal_dt_;
  prev_stamp_ = 0.0;
  prev_arrival_ = 0.0;
  t_first_ = 0.0;
  n_since_ = 0;
  st_ = ImuStamperStats{};
  st_.learned_period_ns = to_ns(period_);
}

void ImuStamper::restart_period_estimate(double arrival_s) {
  // The estimator divides elapsed WALL time by the count of samples RECEIVED.
  // Those agree only while nothing is lost, so any gap left inside the window
  // inflates the estimate permanently (measured in ROS: one 3 s outage in a
  // 30 s, 100 Hz stream took 10.0 ms to 11.0 ms and never came back).
  t_first_ = arrival_s;
  n_since_ = 0;
}

void ImuStamper::update_period_estimate(double arrival_s) {
  ++n_since_;
  if (n_since_ < cfg_.period_min_n) return;
  const double span = arrival_s - t_first_;
  if (span <= 0.0) return;
  const double measured = span / static_cast<double>(n_since_);
  // Ignore absurd values (a stall the gap branch did not catch).
  if (measured > 0.2 * nominal_dt_ && measured < 5.0 * nominal_dt_) {
    period_ = measured;
    st_.learned_period_ns = to_ns(period_);
  }
}

std::int64_t ImuStamper::stamp(std::int64_t t_arrival_ns) {
  if (!have_origin_) {
    have_origin_ = true;
    origin_ns_ = t_arrival_ns;
  }
  const double arrival = static_cast<double>(t_arrival_ns - origin_ns_) / kNsPerSec;

  ++st_.samples;

  if (!started_) {
    started_ = true;
    prev_stamp_ = arrival;
    prev_arrival_ = arrival;
    restart_period_estimate(arrival);
    return t_arrival_ns;
  }

  double est = prev_stamp_ + period_;
  const double err = arrival - est;

  if (arrival < prev_arrival_) {
    // The host clock stepped backwards. `arrival` is behind stamps already
    // published and a consumer drops anything that goes backwards, so there is
    // no honest stamp: free-run at the measured period — which is at least the
    // right RATE — and re-anchor the period estimator.
    ++st_.clock_step_backs;
    const double back = prev_arrival_ - arrival;
    const std::int64_t back_ns = to_ns(back);
    if (back_ns > st_.worst_step_back_ns) st_.worst_step_back_ns = back_ns;
    restart_period_estimate(arrival);
    resync_ = true;
  } else if (resync_) {
    // Recovering from that step: stamps already published are ahead of where
    // the host clock now is, so there is nothing to lock onto until the
    // arrivals catch up. The phase pull stays suppressed rather than fighting
    // a known-stale offset, which also keeps the clamp counter meaningful.
    if (arrival >= prev_stamp_) {
      resync_ = false;
      est = arrival;
      restart_period_estimate(arrival);
    }
  } else if (err > max_gap_s_) {
    // A hole far larger than one period: samples were genuinely lost. Snap
    // fully rather than smearing the gap across the samples after it.
    ++st_.gap_snaps;
    const std::int64_t gap_ns = to_ns(err);
    if (gap_ns > st_.worst_gap_ns) st_.worst_gap_ns = gap_ns;
    est = arrival;
    // Only a LONG hole re-anchors the period estimate — see the header.
    if (err > cfg_.period_reanchor_s) restart_period_estimate(arrival);
  } else {
    if (err < -max_lead_s_) ++st_.deep_bursts;  // diagnostic only
    est = est + cfg_.phase_gain * err;
    update_period_estimate(arrival);
  }

  // Backstop only. Advance by one PERIOD, not by a microsecond: the 1 µs step
  // was self-perpetuating and published a nominal-rate stream of 1 µs dt.
  if (est <= prev_stamp_) {
    ++st_.clamps;
    est = prev_stamp_ + period_;
  }

  prev_stamp_ = est;
  prev_arrival_ = arrival;
  return origin_ns_ + to_ns(est);
}

ImuStamperStats ImuStamper::stats() const {
  ImuStamperStats s = st_;
  s.resyncing = resync_;
  return s;
}

}  // namespace scanengine
