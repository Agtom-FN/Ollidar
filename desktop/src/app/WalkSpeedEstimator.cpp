#include "app/WalkSpeedEstimator.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lidarscan {
namespace {

double dist(const double a[3], const double b[3]) {
  const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

MotionGate::Reading MotionGate::measure(const double* gyro, const double* accel, std::size_t n,
                                        const Config& cfg) {
  Reading r;
  r.samples = n;
  if (n < cfg.min_samples || gyro == nullptr || accel == nullptr) return r;

  double gsum = 0.0, amean = 0.0;
  std::vector<double> amag(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double* g = gyro + 3 * i;
    const double* a = accel + 3 * i;
    gsum += std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    amag[i] = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    amean += amag[i];
  }
  r.gyro_rms_rad_s = gsum / double(n);
  r.accel_mean_m_s2 = amean / double(n);
  // Deviation of the MAGNITUDE, not of the components: the magnitude is
  // orientation-free, so tilting the rig (which rotates gravity between the
  // axes) does not read as acceleration, while gait — which genuinely changes
  // the specific force — does.
  double var = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double d = amag[i] - r.accel_mean_m_s2;
    var += d * d;
  }
  r.accel_dev_m_s2 = std::sqrt(var / double(n));
  r.valid = true;
  r.still = r.accel_dev_m_s2 < cfg.accel_dev_still_m_s2 &&
            r.gyro_rms_rad_s < cfg.gyro_rms_still_rad_s;
  return r;
}

void WalkSpeedEstimator::reset() {
  win_.clear();
  speed_mps_ = 0.0;
  valid_ = false;
  have_last_ = false;
  last_t_ns_ = 0;
}

double WalkSpeedEstimator::windowSpanS() const {
  if (win_.size() < 2) return 0.0;
  return win_.back().t_s - win_.front().t_s;
}

bool WalkSpeedEstimator::update(std::int64_t t_mono_ns, const double p[3]) {
  // A pose with no stamp is a pose we cannot time, and timing is the whole
  // measurement. (LioPoseSource always stamps; a 0 here means something else
  // pushed into the source.)
  if (t_mono_ns <= 0) {
    ++stale_samples_;
    return false;
  }

  // Repeated identical poses. latest() answers every poll whether or not the
  // odometry produced anything new, so a stalled LIO would otherwise be
  // resampled ten times a second — and each resample would carry the previous
  // step's displacement over a fraction of the real dt.
  if (have_last_ && t_mono_ns <= last_t_ns_) {
    ++stale_samples_;
    return false;
  }

  const double t_s = double(t_mono_ns) / 1e9;

  if (!win_.empty()) {
    const Sample& prev = win_.back();
    const double dt = t_s - prev.t_s;
    // dt FLOOR. Two poses closer together than this are one observation as far
    // as a 10 Hz odometry is concerned; accepting them is how a millisecond gap
    // turns a centimetre of noise into a sprint.
    if (dt < cfg_.min_dt_s) {
      ++stale_samples_;
      return false;
    }
    const double step = dist(p, prev.p);
    // DISCONTINUITY. Either the pose frame restarted (every Start/Stop/Pause/
    // Resume restarts LioOdometry at the origin) or the odometry jumped. Both
    // mean the window's older samples are in a different frame; keeping them
    // would report the jump as motion.
    if (dt > cfg_.max_gap_s || step > cfg_.max_step_m) {
      ++discontinuities_;
      win_.clear();
      speed_mps_ = 0.0;
      valid_ = false;
      win_.push_back(Sample{t_s, {p[0], p[1], p[2]}});
      have_last_ = true;
      last_t_ns_ = t_mono_ns;
      return false;
    }
  }

  win_.push_back(Sample{t_s, {p[0], p[1], p[2]}});
  have_last_ = true;
  last_t_ns_ = t_mono_ns;

  // Keep just over one window of pose time: drop a leading sample only while
  // the one after it still leaves a full window behind.
  while (win_.size() > 2 && (win_.back().t_s - win_[1].t_s) >= cfg_.window_s) {
    win_.pop_front();
  }
  recompute();
  return true;
}

void WalkSpeedEstimator::recompute() {
  const double span = windowSpanS();
  if (win_.size() < 6 || span < cfg_.window_s) {
    // Not enough pose time yet. Deliberately NOT "keep the old number": a hint
    // that persists across a reset is exactly how the stale spike survived.
    valid_ = false;
    speed_mps_ = 0.0;
    return;
  }
  // MEDIAN-TO-MEDIAN DISPLACEMENT, not first-sample-to-last-sample.
  //
  // Split the window in half and take the componentwise MEDIAN position and the
  // median timestamp of each half. Two properties matter:
  //
  //   * For a rig standing still, both medians estimate the SAME point, and a
  //     median over ~6 samples suppresses per-pose jitter by roughly sqrt(n)
  //     while being immune to the single wild pose that a mean is not. The
  //     residual is then killed outright by the noise floor. This is what makes
  //     a stationary sensor read a hard 0.00 instead of "0.08 m/s".
  //   * For a rig moving at a steady speed, the median position of a half IS the
  //     position at that half's median time, so the quotient is the true speed —
  //     the robustness costs nothing in accuracy. (Measured: 1.402 m/s for a
  //     1.400 m/s walk carrying the same jitter.)
  const std::size_t n = win_.size();
  const std::size_t mid = n / 2;
  auto median_of = [this](std::size_t lo, std::size_t hi, double out_p[3], double* out_t) {
    const std::size_t k = hi - lo;
    std::vector<double> v(k);
    for (int axis = 0; axis < 3; ++axis) {
      for (std::size_t i = 0; i < k; ++i) v[i] = win_[lo + i].p[axis];
      std::nth_element(v.begin(), v.begin() + k / 2, v.end());
      out_p[axis] = v[k / 2];
    }
    for (std::size_t i = 0; i < k; ++i) v[i] = win_[lo + i].t_s;
    std::nth_element(v.begin(), v.begin() + k / 2, v.end());
    *out_t = v[k / 2];
  };
  double p_early[3], p_late[3], t_early = 0.0, t_late = 0.0;
  median_of(0, mid, p_early, &t_early);
  median_of(mid, n, p_late, &t_late);

  const double dt = t_late - t_early;
  if (dt < cfg_.min_dt_s) {  // cannot happen with a sane window; never divide by it anyway
    valid_ = false;
    speed_mps_ = 0.0;
    return;
  }
  const double net = dist(p_late, p_early);
  double v = (net <= cfg_.noise_floor_m) ? 0.0 : net / dt;
  if (v > cfg_.max_speed_mps) v = cfg_.max_speed_mps;
  speed_mps_ = v;
  valid_ = true;
}

}  // namespace lidarscan
