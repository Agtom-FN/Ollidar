#include "scanengine/timesync/min_delay_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "scanengine/core/log.h"

namespace scanengine {
namespace {

// Nearest-rank quantile on an already-sorted buffer. No interpolation: with
// 16 samples an interpolated p95 invents a number between two observations,
// and this value is a safety budget, not a plot.
std::int64_t quantile_sorted(const std::vector<std::int64_t>& v, double q) {
  if (v.empty()) return 0;
  const double n = static_cast<double>(v.size());
  auto idx = static_cast<std::size_t>(std::ceil(q * n));
  if (idx == 0) idx = 1;
  if (idx > v.size()) idx = v.size();
  return v[idx - 1];
}

}  // namespace

const char* to_string(ResyncReason r) noexcept {
  switch (r) {
    case ResyncReason::kNone: return "none";
    case ResyncReason::kColdStart: return "cold-start";
    case ResyncReason::kDeviceReset: return "device-reset";
    case ResyncReason::kClockStep: return "clock-step";
    case ResyncReason::kStreamGap: return "stream-gap";
  }
  return "unknown";
}

MinDelayOffsetEstimator::MinDelayOffsetEstimator(const MinDelayConfig& cfg) : cfg_(cfg) {
  if (cfg_.window_ns <= 0) cfg_.window_ns = 1;
  if (cfg_.windows < 2) cfg_.windows = 2;
  if (cfg_.residual_window < 2) cfg_.residual_window = 2;
  if (cfg_.step_confirm < 1) cfg_.step_confirm = 1;
  hull_.resize(cfg_.windows);
  resid_.resize(cfg_.residual_window);
  scratch_.reserve(cfg_.residual_window);
  const std::size_t fit_max = static_cast<std::size_t>(cfg_.windows) + 1;
  fit_t_.reserve(fit_max);
  fit_d_.reserve(fit_max);
  fit_resid_.reserve(fit_max);
  fit_sorted_.reserve(fit_max);
  fit_keep_.reserve(fit_max);
}

MinDelayOffsetEstimator::~MinDelayOffsetEstimator() = default;

void MinDelayOffsetEstimator::set_stream(StreamId s) {
  std::lock_guard<std::mutex> lock(m_);
  stream_ = s;
}

void MinDelayOffsetEstimator::set_resync_observer(ClockResyncFn fn, void* user_data) {
  std::lock_guard<std::mutex> lock(m_);
  observer_ = fn;
  observer_user_ = user_data;
}

const MinDelayOffsetEstimator::Envelope& MinDelayOffsetEstimator::hull_at_(std::size_t i) const {
  return hull_[(hull_head_ + i) % hull_.size()];
}

std::int64_t MinDelayOffsetEstimator::line_at_(std::int64_t t_device_ns) const {
  if (!have_fit_) return 0;
  const double dt = static_cast<double>(t_device_ns - fit_anchor_ns_);
  return fit_base_ns_ + static_cast<std::int64_t>(std::llround(fit_corr_ns_ + fit_slope_ * dt));
}

void MinDelayOffsetEstimator::clear_model_() {
  hull_head_ = 0;
  hull_size_ = 0;
  have_window_ = false;
  win_start_device_ = 0;
  win_min_ = Envelope{};
  have_fit_ = false;
  fit_anchor_ns_ = 0;
  fit_base_ns_ = 0;
  fit_corr_ns_ = 0.0;
  fit_slope_ = 0.0;
  resid_head_ = 0;
  resid_size_ = 0;
  since_jitter_ = 0;
  jitter_ns_ = 0;
  pending_step_ = 0;
}

void MinDelayOffsetEstimator::close_window_(const Envelope& e) {
  if (hull_size_ < hull_.size()) {
    hull_[(hull_head_ + hull_size_) % hull_.size()] = e;
    ++hull_size_;
  } else {
    hull_[hull_head_] = e;
    hull_head_ = (hull_head_ + 1) % hull_.size();
  }
  ++diag_.closed_windows;
}

// The fit, in two halves, which is the whole design in ten lines:
//
//   slope     — ordinary least squares through the per-window minima. Delay
//               noise is common to every window, so it lands in the
//               intercept and leaves the drift unbiased.
//   intercept — NOT the least-squares one. The line is pushed down until it
//               touches the lowest envelope point: intercept =
//               min_i(delta_i − slope·t_i). That is the linear-programming
//               estimator's constraint (all residuals ≥ 0, i.e. no packet
//               ever arrived before it was sent) and it is what makes the
//               estimator immune to congestion. A least-squares intercept is
//               not: a one-second congestion episode at the end of the
//               window drags it by ~12 ms, which is most of the S6 budget,
//               and the whole point of a min filter is that it does not do
//               that. Measured in tests/test_timesync.cpp case 4.
void MinDelayOffsetEstimator::refit_() {
  const std::size_t n_hull = hull_size_;
  const std::size_t n = n_hull + (have_window_ ? 1u : 0u);
  if (n == 0) {
    have_fit_ = false;
    return;
  }

  const Envelope& anchor = have_window_ ? win_min_ : hull_at_(n_hull - 1);
  const std::int64_t base = anchor.delta_ns;
  const std::int64_t anchor_t = anchor.t_device_ns;

  auto point = [&](std::size_t i) -> Envelope {
    return (i < n_hull) ? hull_at_(i) : win_min_;
  };

  const std::int64_t span =
      point(n - 1).t_device_ns - point(0).t_device_ns;

  fit_t_.clear();
  fit_d_.clear();
  for (std::size_t i = 0; i < n; ++i) {
    const Envelope p = point(i);
    fit_t_.push_back(static_cast<double>(p.t_device_ns - anchor_t));
    fit_d_.push_back(static_cast<double>(p.delta_ns - base));
  }

  const double max_slope = cfg_.max_drift_ppm * 1e-6;
  // Below two windows of span, a drift slope cannot be separated from delay
  // noise. Report zero rather than extrapolate a number we cannot see.
  const bool fit_slope = n >= 3 && span >= cfg_.window_ns * 2;

  auto ols = [&](const std::vector<unsigned char>* keep) {
    double sum_t = 0.0, sum_d = 0.0, sum_tt = 0.0, sum_td = 0.0, cnt = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (keep != nullptr && !(*keep)[i]) continue;
      sum_t += fit_t_[i];
      sum_d += fit_d_[i];
      sum_tt += fit_t_[i] * fit_t_[i];
      sum_td += fit_t_[i] * fit_d_[i];
      cnt += 1.0;
    }
    if (cnt < 3.0) return 0.0;
    const double denom = cnt * sum_tt - sum_t * sum_t;
    if (!(denom > 0.0)) return 0.0;
    const double s = (cnt * sum_td - sum_t * sum_d) / denom;
    return std::max(-max_slope, std::min(max_slope, s));
  };
  // Largest downward shift that still leaves every envelope point on or
  // above the line — the LP constraint "no packet arrived before it was
  // sent". Always taken over ALL points, never over the trimmed subset.
  auto lp_intercept = [&](double s) {
    double c = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double v = fit_d_[i] - s * fit_t_[i];
      if (i == 0 || v < c) c = v;
    }
    return c;
  };

  double slope = fit_slope ? ols(nullptr) : 0.0;
  double corr = lp_intercept(slope);

  // Second pass. Least squares has no breakdown point, so a congestion
  // episode sitting at one end of the history tilts the slope even though
  // the intercept is pinned to the envelope (measured: a 1 s block of
  // 100 ms delays moved the mapping by 712 µs). Refit the slope over the
  // three quarters of the envelope points closest to the line — the block
  // is by construction in the discarded quarter — and re-pin the intercept.
  if (fit_slope && n >= 8) {
    fit_resid_.clear();
    for (std::size_t i = 0; i < n; ++i) {
      fit_resid_.push_back(fit_d_[i] - (corr + slope * fit_t_[i]));
    }
    fit_sorted_ = fit_resid_;
    std::sort(fit_sorted_.begin(), fit_sorted_.end());
    const double cut = fit_sorted_[(fit_sorted_.size() * 3) / 4];
    fit_keep_.assign(n, 0);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (fit_resid_[i] <= cut) {
        fit_keep_[i] = 1;
        ++kept;
      }
    }
    if (kept >= 3) {
      slope = ols(&fit_keep_);
      corr = lp_intercept(slope);
    }
  }

  have_fit_ = true;
  fit_anchor_ns_ = anchor_t;
  fit_base_ns_ = base;
  fit_corr_ns_ = corr;
  fit_slope_ = slope;
}

void MinDelayOffsetEstimator::push_residual_(std::int64_t r) {
  if (resid_size_ < resid_.size()) {
    resid_[(resid_head_ + resid_size_) % resid_.size()] = r;
    ++resid_size_;
  } else {
    resid_[resid_head_] = r;
    resid_head_ = (resid_head_ + 1) % resid_.size();
  }
  ++since_jitter_;
}

// jitter_ns = p95 − p5 of the residuals against the fitted clock line.
//
// Why this and not a standard deviation or a max: the residual distribution
// is one-sided and heavy-tailed (a 50 ms scheduler stall is one sample in a
// thousand). A max reports the worst stall as if it were the norm; an sd is
// inflated by the same tail; the 90 % inter-quantile range reports the width
// of the bulk of the delay distribution, which is the quantity S6's budget
// is written against, and it is exactly the interval within which the
// unobservable minimum one-way delay — i.e. our offset error — must lie.
void MinDelayOffsetEstimator::refresh_jitter_() {
  since_jitter_ = 0;
  if (resid_size_ == 0) {
    jitter_ns_ = 0;
    return;
  }
  scratch_.clear();
  for (std::size_t i = 0; i < resid_size_; ++i) {
    scratch_.push_back(resid_[(resid_head_ + i) % resid_.size()]);
  }
  std::sort(scratch_.begin(), scratch_.end());
  const std::int64_t hi = quantile_sorted(scratch_, 0.95);
  const std::int64_t lo = quantile_sorted(scratch_, 0.05);
  jitter_ns_ = hi >= lo ? hi - lo : 0;
}

void MinDelayOffsetEstimator::refresh_cache_() {
  cache_.valid = have_;
  cache_.samples = diag_.pairs_accepted;
  cache_.resyncs = diag_.resyncs;
  cache_.jitter_ns = jitter_ns_;
  cache_.drift_ppm = have_fit_ ? fit_slope_ * 1e6 : 0.0;
  cache_.t_ref_device_ns = last_device_ns_;
  cache_.t_updated_ns = last_arrival_ns_;
  cache_.offset_ns = have_fit_ ? line_at_(last_device_ns_) : 0;

  const std::int64_t span = have_window_ && hull_size_ > 0
                                ? win_min_.t_device_ns - hull_at_(0).t_device_ns
                                : 0;
  cache_.converged = have_ && have_fit_ && pending_step_ == 0 &&
                     hull_size_ >= cfg_.converged_min_windows &&
                     span >= cfg_.converged_span_ns &&
                     resid_size_ >= cfg_.converged_min_residuals;
  diag_.history_span_ns = span;
}

void MinDelayOffsetEstimator::seed_(std::int64_t t_device_ns, std::int64_t delta_ns,
                                    std::int64_t arrival_ns) {
  have_ = true;
  have_window_ = true;
  win_start_device_ = t_device_ns;
  win_min_ = Envelope{t_device_ns, delta_ns};
  last_device_ns_ = t_device_ns;
  last_arrival_ns_ = arrival_ns;
  refit_();
  push_residual_(0);
  refresh_jitter_();
  ++diag_.pairs_accepted;
  refresh_cache_();
}

void MinDelayOffsetEstimator::accept_(std::int64_t t_device_ns, std::int64_t delta_ns,
                                      std::int64_t residual_ns, std::int64_t arrival_ns) {
  bool window_closed = false;
  if (t_device_ns - win_start_device_ >= cfg_.window_ns) {
    close_window_(win_min_);
    win_start_device_ = t_device_ns;
    win_min_ = Envelope{t_device_ns, delta_ns};
    window_closed = true;
  } else if (delta_ns < win_min_.delta_ns) {
    win_min_ = Envelope{t_device_ns, delta_ns};
  }

  last_device_ns_ = t_device_ns;
  last_arrival_ns_ = arrival_ns;
  ++diag_.pairs_accepted;

  push_residual_(residual_ns);

  if (window_closed || delta_ns < line_at_(t_device_ns)) {
    refit_();
  }
  // Cheap while the ring is short (cold start, where every sample matters),
  // then amortized to once per 64 pairs.
  if (window_closed || resid_size_ < 64 || since_jitter_ >= 64) {
    refresh_jitter_();
  }
  refresh_cache_();
}

void MinDelayOffsetEstimator::resync_(ResyncReason reason, std::int64_t t_device_ns,
                                      std::int64_t delta_ns, std::int64_t arrival_ns,
                                      std::int64_t step_ns) {
  const std::int64_t prev_offset = cache_.valid ? cache_.offset_ns : 0;
  const std::uint64_t accepted = diag_.pairs_accepted;

  clear_model_();
  seed_(t_device_ns, delta_ns, arrival_ns);
  diag_.pairs_accepted = accepted + 1;  // seeding is not a new stream

  ++diag_.resyncs;
  cache_.resyncs = diag_.resyncs;
  cache_.samples = diag_.pairs_accepted;

  SCAN_LOG_WARN("timesync",
                "stream %s: %s, step %lld ms, offset %lld -> %lld ns; re-converging",
                to_string(stream_), to_string(reason),
                static_cast<long long>(step_ns / 1'000'000),
                static_cast<long long>(prev_offset), static_cast<long long>(delta_ns));

  if (observer_) {
    ClockResync ev{};
    ev.stream = stream_;
    ev.reason = reason;
    ev.t_engine_ns = arrival_ns;
    ev.step_ns = step_ns;
    ev.previous_offset_ns = prev_offset;
    ev.new_offset_ns = delta_ns;
    ev.index = diag_.resyncs;
    observer_(ev, observer_user_);
  }
}

void MinDelayOffsetEstimator::add_pair(std::int64_t t_device_ns, TimePoint t_arrival) {
  std::lock_guard<std::mutex> lock(m_);
  const std::int64_t arrival = t_arrival.nanos;
  const std::int64_t delta = arrival - t_device_ns;
  ++diag_.pairs_seen;

  if (!have_) {
    seed_(t_device_ns, delta, arrival);
    return;
  }

  // 1. Device timestamp went a long way backwards. Nothing but a device
  //    reboot (or a counter wrap) does that; one sample is enough evidence,
  //    and holding the old mapping for even one more packet would emit
  //    timestamps hours away from the truth.
  if (t_device_ns < last_device_ns_ - cfg_.backward_tolerance_ns) {
    resync_(ResyncReason::kDeviceReset, t_device_ns, delta, arrival,
            t_device_ns - last_device_ns_);
    return;
  }

  // 2. Small backwards step: UDP reordering. Do not fold it in (it would
  //    corrupt the window bookkeeping) but do not panic either.
  if (t_device_ns < last_device_ns_) {
    ++diag_.pairs_reordered;
    return;
  }

  // 3. The stream went quiet for longer than we are willing to extrapolate.
  //    Not an error — a USB re-enumeration or an app pause looks like this —
  //    but the delay floor may have moved, so rebuild rather than trust it.
  if (arrival - last_arrival_ns_ > cfg_.max_gap_ns) {
    resync_(ResyncReason::kStreamGap, t_device_ns, delta, arrival,
            arrival - last_arrival_ns_);
    return;
  }

  const std::int64_t residual = delta - line_at_(t_device_ns);

  // 4. A residual this far from the model is either a monstrous stall or a
  //    clock step. Withhold judgement for step_confirm pairs: a stall is
  //    transient, a step is not. The withheld pairs are NOT folded into the
  //    model, and `converged` is false while the count is pending, so a
  //    consumer sees "unsynchronised" rather than a wild mapping.
  if (residual > cfg_.step_threshold_ns || residual < -cfg_.step_threshold_ns) {
    ++pending_step_;
    ++diag_.pairs_rejected;
    last_arrival_ns_ = arrival;
    if (pending_step_ >= cfg_.step_confirm) {
      resync_(ResyncReason::kClockStep, t_device_ns, delta, arrival, residual);
    } else {
      last_device_ns_ = t_device_ns;
      refresh_cache_();
    }
    return;
  }

  pending_step_ = 0;
  accept_(t_device_ns, delta, residual, arrival);
}

OffsetEstimate MinDelayOffsetEstimator::estimate() const {
  std::lock_guard<std::mutex> lock(m_);
  return cache_;
}

TimeModel MinDelayOffsetEstimator::model() const {
  std::lock_guard<std::mutex> lock(m_);
  TimeModel m;
  m.valid = cache_.valid;
  m.offset_ns = cache_.offset_ns;
  m.drift_ppm = cache_.drift_ppm;
  m.t_ref_device_ns = cache_.t_ref_device_ns;
  m.converged = cache_.converged;
  m.uncertainty_ns =
      cache_.converged ? cache_.jitter_ns : cfg_.unconverged_uncertainty_ns;
  return m;
}

void MinDelayOffsetEstimator::reset() {
  std::lock_guard<std::mutex> lock(m_);
  clear_model_();
  have_ = false;
  last_device_ns_ = 0;
  last_arrival_ns_ = 0;
  diag_ = Diagnostics{};
  cache_ = OffsetEstimate{};
}

MinDelayOffsetEstimator::Diagnostics MinDelayOffsetEstimator::diagnostics() const {
  std::lock_guard<std::mutex> lock(m_);
  return diag_;
}

}  // namespace scanengine
