#include "scanengine/poses/imu_densified_pose.h"

#include <algorithm>
#include <cmath>

namespace scanengine {
namespace {

bool finite3(const float v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

}  // namespace

ImuDensifiedPoseSource::ImuDensifiedPoseSource(const PoseInterpolator* base,
                                               const ImuDensifyConfig& cfg)
    : base_(base), cfg_(cfg) {
  ring_.resize(cfg_.capacity > 0 ? cfg_.capacity : 1);
  se3::quat_normalize(cfg_.camera_from_imu);
}

bool ImuDensifiedPoseSource::push_imu(const PhoneImuSample& s) {
  std::lock_guard<std::mutex> lk(m_);
  ++stats_.samples_in;
  if (s.t_mono_ns <= 0 || !finite3(s.gyro_rad_s) || !finite3(s.accel_m_s2) ||
      (last_t_ns_ != 0 && s.t_mono_ns < last_t_ns_)) {
    ++stats_.samples_rejected;
    return false;
  }
  last_t_ns_ = s.t_mono_ns;
  ring_[head_] = s;
  head_ = (head_ + 1) % ring_.size();
  if (size_ < ring_.size()) ++size_;
  return true;
}

std::size_t ImuDensifiedPoseSource::buffered() const {
  std::lock_guard<std::mutex> lk(m_);
  return size_;
}

void ImuDensifiedPoseSource::reset() {
  std::lock_guard<std::mutex> lk(m_);
  head_ = 0;
  size_ = 0;
  last_t_ns_ = 0;
  bias_[0] = bias_[1] = bias_[2] = 0.0;
  stats_ = ImuDensifyStats{};
  closing_sum_deg_ = 0.0;
  closing_n_ = 0;
}

ImuDensifyStats ImuDensifiedPoseSource::stats() const {
  std::lock_guard<std::mutex> lk(m_);
  ImuDensifyStats out = stats_;
  for (int i = 0; i < 3; ++i) out.bias_rad_s[i] = bias_[i];
  out.mean_closing_deg = closing_n_ ? closing_sum_deg_ / static_cast<double>(closing_n_) : 0.0;
  return out;
}

bool ImuDensifiedPoseSource::time_span(std::int64_t* first_ns, std::int64_t* last_ns) const {
  return base_ != nullptr && base_->time_span(first_ns, last_ns);
}

// Caller holds m_.
bool ImuDensifiedPoseSource::integrate_(std::int64_t t0, std::int64_t t1, double q_rel[4],
                                        double* peak_rate, bool* saw_gap) const {
  se3::quat_identity(q_rel);
  *peak_rate = 0.0;
  *saw_gap = false;
  if (size_ == 0 || t1 <= t0) return t1 == t0;

  // The ring in chronological order.
  const std::size_t first = (head_ + ring_.size() - size_) % ring_.size();

  // The stream must actually straddle the interval; extrapolating a gyro past
  // the samples it has is how a stationary rig becomes a gyroscope.
  const PhoneImuSample& oldest = ring_[first];
  const PhoneImuSample& newest = ring_[(first + size_ - 1) % ring_.size()];
  if (oldest.t_mono_ns > t0 + cfg_.max_imu_gap_ns) return false;
  if (newest.t_mono_ns < t1 - cfg_.max_imu_gap_ns) return false;

  std::int64_t t_cur = t0;
  double prev_w[3] = {0.0, 0.0, 0.0};
  bool have_prev = false;

  for (std::size_t k = 0; k < size_; ++k) {
    const PhoneImuSample& s = ring_[(first + k) % ring_.size()];
    if (s.t_mono_ns <= t0) {
      // Hold the last sample at or before t0 as the interval's opening rate.
      for (int i = 0; i < 3; ++i) prev_w[i] = static_cast<double>(s.gyro_rad_s[i]) - bias_[i];
      have_prev = true;
      continue;
    }
    if (t_cur >= t1) break;

    const std::int64_t t_step = std::min(s.t_mono_ns, t1);
    const std::int64_t dt_ns = t_step - t_cur;
    if (dt_ns > 0) {
      if (dt_ns > cfg_.max_imu_gap_ns) *saw_gap = true;
      double w[3];
      for (int i = 0; i < 3; ++i) w[i] = static_cast<double>(s.gyro_rad_s[i]) - bias_[i];
      // Trapezoid: the midpoint rate over the step. At 400 Hz the difference
      // from a rectangle is far below the noise, but it is free and it makes
      // the integrator exact for a constant angular acceleration.
      double wm[3];
      for (int i = 0; i < 3; ++i) wm[i] = have_prev ? 0.5 * (prev_w[i] + w[i]) : w[i];

      const double dt = static_cast<double>(dt_ns) * 1e-9;
      double rot[3] = {wm[0] * dt, wm[1] * dt, wm[2] * dt};
      double dq[4];
      se3::quat_from_rotvec(rot, dq);
      double acc[4];
      se3::quat_mul(q_rel, dq, acc);
      for (int i = 0; i < 4; ++i) q_rel[i] = acc[i];

      const double rate = std::sqrt(wm[0] * wm[0] + wm[1] * wm[1] + wm[2] * wm[2]);
      *peak_rate = std::max(*peak_rate, rate);

      t_cur = t_step;
      for (int i = 0; i < 3; ++i) prev_w[i] = w[i];
      have_prev = true;
    } else {
      for (int i = 0; i < 3; ++i) prev_w[i] = static_cast<double>(s.gyro_rad_s[i]) - bias_[i];
      have_prev = true;
    }
  }

  if (t_cur < t1) {
    // Ran out of samples before the end of the interval.
    if (t1 - t_cur > cfg_.max_imu_gap_ns) return false;
    *saw_gap = true;
  }
  se3::quat_normalize(q_rel);

  // The gyro measures rotation in ITS frame; the pose lives in the camera
  // frame. Conjugate: q_cam = C * q_imu * C^-1.
  double c_conj[4];
  se3::quat_conj(cfg_.camera_from_imu, c_conj);
  double tmp[4], out[4];
  se3::quat_mul(cfg_.camera_from_imu, q_rel, tmp);
  se3::quat_mul(tmp, c_conj, out);
  for (int i = 0; i < 4; ++i) q_rel[i] = out[i];
  return true;
}

PoseSample ImuDensifiedPoseSource::sample_at(std::int64_t t_mono_ns) const {
  if (base_ == nullptr) return PoseSample{};
  PoseSample s = base_->sample_at(t_mono_ns);

  std::lock_guard<std::mutex> lk(m_);
  ++stats_.queries;

  // Only densify a usable, genuinely interpolated sample. A flagged or
  // unresolved one has bigger problems than its rotation path.
  if (!s.has_pose || s.gate != PoseGate::kOk) {
    ++stats_.fallbacks;
    // ROUND 14: this bucket had no counter, which is why the reasons never
    // summed to the total. Nothing here is the densifier's doing — it is the
    // trajectory underneath it — but that is exactly what the operator needs
    // told apart from a gyro problem. See ImuDensifyStats.
    if (!s.has_pose) {
      ++stats_.fallback_no_pose;
    } else {
      ++stats_.fallback_gate;
    }
    return s;
  }
  if (size_ == 0) {
    ++stats_.fallbacks;
    ++stats_.fallback_no_imu;
    return s;
  }

  // The exact bracket the wrapped source used. ROUND 9 added the knot stamps
  // to PoseSample precisely so this does not have to be guessed: integrating
  // over anything other than the interval the interpolator itself used would
  // lose the endpoint-pinning property that makes this safe.
  const std::int64_t ta = s.bracket_t0_ns;
  const std::int64_t tb = s.bracket_t1_ns;
  const std::int64_t span = tb - ta;
  if (ta <= 0 || tb <= 0 || span <= 0 || span > cfg_.max_bracket_ns ||
      t_mono_ns < ta || t_mono_ns > tb) {
    // No bracket (query landed on a knot, or the source is holding), or the
    // bracket is too wide to trust a linear error distribution over.
    ++stats_.fallbacks;
    ++stats_.fallback_bracket;
    return s;
  }

  const PoseSample a = base_->sample_at(ta);
  const PoseSample b = base_->sample_at(tb);
  if (!a.has_pose || !b.has_pose) {
    ++stats_.fallbacks;
    ++stats_.fallback_bracket;
    return s;
  }

  double q_to_t[4], q_to_b[4];
  double peak_t = 0.0, peak_b = 0.0;
  bool gap_t = false, gap_b = false;
  if (!integrate_(ta, t_mono_ns, q_to_t, &peak_t, &gap_t) ||
      !integrate_(ta, tb, q_to_b, &peak_b, &gap_b)) {
    ++stats_.fallbacks;
    ++stats_.fallback_no_imu;
    return s;
  }
  if (gap_t || gap_b) {
    ++stats_.fallbacks;
    ++stats_.fallback_gap;
    return s;
  }

  // q_int(tb) = q_a * q_to_b ; closing error e = q_int(tb)^-1 * q_b.
  double q_int_b[4];
  se3::quat_mul(a.pose.orientation, q_to_b, q_int_b);
  double q_int_b_conj[4], e[4];
  se3::quat_conj(q_int_b, q_int_b_conj);
  se3::quat_mul(q_int_b_conj, b.pose.orientation, e);
  se3::quat_normalize(e);

  double e_rv[3];
  se3::quat_to_rotvec(e, e_rv);
  const double closing_deg =
      std::sqrt(e_rv[0] * e_rv[0] + e_rv[1] * e_rv[1] + e_rv[2] * e_rv[2]) * kRadToDeg;
  stats_.worst_closing_deg = std::max(stats_.worst_closing_deg, closing_deg);
  closing_sum_deg_ += closing_deg;
  ++closing_n_;

  if (!(closing_deg <= cfg_.max_closing_deg)) {
    // The gyro and ARCore disagree by more than any real rig should. Do not
    // "correct" it — a wrong camera_from_imu, a mis-stamped stream or a
    // relocalisation jump all land here, and in every case the slerp is the
    // safer answer.
    ++stats_.fallbacks;
    ++stats_.fallback_closing;
    return s;
  }

  const double u = static_cast<double>(t_mono_ns - ta) / static_cast<double>(span);

  // q(t) = q_a * q_to_t * exp(u * log(e))
  double corr_rv[3] = {e_rv[0] * u, e_rv[1] * u, e_rv[2] * u};
  double corr[4];
  se3::quat_from_rotvec(corr_rv, corr);
  double q_at[4], q_out[4];
  se3::quat_mul(a.pose.orientation, q_to_t, q_at);
  se3::quat_mul(q_at, corr, q_out);
  se3::quat_normalize(q_out);

  for (int i = 0; i < 4; ++i) s.pose.orientation[i] = q_out[i];
  ++stats_.densified;

  // --- bias -------------------------------------------------------------
  //
  // A constant bias `b` produces exactly `b * T` of closing error over an
  // interval of length T, so the closing error IS the bias observation. Fold a
  // fraction in; take a bigger bite when the rig is still, because then ARCore
  // has nothing to say and the raw gyro is almost purely bias.
  if (cfg_.estimate_bias) {
    double arcore_rot[3];
    {
      double qa_conj[4], rel[4];
      se3::quat_conj(a.pose.orientation, qa_conj);
      se3::quat_mul(qa_conj, b.pose.orientation, rel);
      se3::quat_to_rotvec(rel, arcore_rot);
    }
    const double arcore_deg =
        std::sqrt(arcore_rot[0] * arcore_rot[0] + arcore_rot[1] * arcore_rot[1] +
                  arcore_rot[2] * arcore_rot[2]) * kRadToDeg;
    const bool still =
        peak_b < cfg_.stationary_rate_rad_s && arcore_deg < cfg_.stationary_arcore_deg;
    const double gain = still ? cfg_.bias_gain_stationary : cfg_.bias_gain;
    const double T = static_cast<double>(span) * 1e-9;
    if (T > 1e-6) {
      // The closing error is expressed in the camera frame; the bias lives in
      // the IMU frame, so rotate it back.
      double c_conj[4], tmp[4], e_imu[4];
      se3::quat_conj(cfg_.camera_from_imu, c_conj);
      se3::quat_mul(c_conj, e, tmp);
      se3::quat_mul(tmp, cfg_.camera_from_imu, e_imu);
      double e_imu_rv[3];
      se3::quat_to_rotvec(e_imu, e_imu_rv);
      for (int i = 0; i < 3; ++i) {
        // Integration fell SHORT by e, so the measured rate was too small by
        // e/T, so the subtracted bias must come DOWN by that much.
        bias_[i] -= gain * e_imu_rv[i] / T;
        bias_[i] = std::max(-cfg_.max_bias_rad_s, std::min(cfg_.max_bias_rad_s, bias_[i]));
      }
      ++stats_.bias_updates;
    }
  }
  return s;
}

}  // namespace scanengine
