// lio_pose_source.cpp — LioPoseSource, from include/scanengine/slam/lio.h.
#include <cmath>

#include "scanengine/slam/lio.h"

namespace scanengine {
namespace {

// Shortest-arc quaternion interpolation. Falls back to normalized lerp when
// the two poses are close, which at 10 Hz they always are: slerp's sin()
// pair costs more than it buys below a few degrees, and nlerp's angular
// error there is under 1e-6 rad.
void interp_quat(const double a[4], const double b[4], double t, double out[4]) {
  double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  double bb[4] = {b[0], b[1], b[2], b[3]};
  if (d < 0.0) {
    for (int i = 0; i < 4; ++i) bb[i] = -bb[i];
    d = -d;
  }
  if (d > 0.9995) {
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (bb[i] - a[i]) * t;
  } else {
    if (d > 1.0) d = 1.0;
    const double theta = std::acos(d);
    const double s = std::sin(theta);
    const double wa = std::sin((1.0 - t) * theta) / s;
    const double wb = std::sin(t * theta) / s;
    for (int i = 0; i < 4; ++i) out[i] = a[i] * wa + bb[i] * wb;
  }
  const double n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
  if (n > 0.0) {
    for (int i = 0; i < 4; ++i) out[i] /= n;
  } else {
    out[0] = out[1] = out[2] = 0.0;
    out[3] = 1.0;
  }
}

}  // namespace

LioPoseSource::LioPoseSource(std::size_t capacity, StreamId stream)
    : capacity_(capacity == 0 ? 1 : capacity), stream_(stream) {}

LioPoseSource::~LioPoseSource() = default;

const char* LioPoseSource::name() const { return "lio"; }
StreamId LioPoseSource::stream() const { return stream_; }

Status LioPoseSource::start() {
  std::lock_guard<std::mutex> lk(m_);
  running_ = true;
  return kOkStatus;
}

Status LioPoseSource::stop() {
  std::lock_guard<std::mutex> lk(m_);
  running_ = false;
  return kOkStatus;
}

bool LioPoseSource::running() const {
  std::lock_guard<std::mutex> lk(m_);
  return running_;
}

Status LioPoseSource::push_pose(const Pose& pose) {
  PoseCallback cb;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!poses_.empty() && pose.t_mono_ns < poses_.back().t_mono_ns) {
      return set_last_error(ScanError::kInvalidArgument,
                            "lio: pose at %lld is older than the newest pose at %lld",
                            static_cast<long long>(pose.t_mono_ns),
                            static_cast<long long>(poses_.back().t_mono_ns));
    }
    if (have_last_) {
      const double dx = pose.position[0] - last_p_[0];
      const double dy = pose.position[1] - last_p_[1];
      const double dz = pose.position[2] - last_p_[2];
      const double step = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (std::isfinite(step)) length_m_ += step;
    }
    last_p_[0] = pose.position[0];
    last_p_[1] = pose.position[1];
    last_p_[2] = pose.position[2];
    have_last_ = true;

    poses_.push_back(pose);
    while (poses_.size() > capacity_) poses_.pop_front();
    cb = cb_;
  }
  // Outside the lock: a subscriber must not be able to deadlock the odometry
  // by calling back into pose_at().
  if (cb) cb(pose);
  return kOkStatus;
}

void LioPoseSource::set_callback(PoseCallback cb) {
  std::lock_guard<std::mutex> lk(m_);
  cb_ = std::move(cb);
}

Status LioPoseSource::pose_at(std::int64_t t_mono_ns, Pose* out) const {
  if (out == nullptr) return ScanError::kInvalidArgument;
  std::lock_guard<std::mutex> lk(m_);
  if (poses_.empty()) return ScanError::kNotFound;
  if (t_mono_ns < poses_.front().t_mono_ns) return ScanError::kNotFound;
  if (t_mono_ns > poses_.back().t_mono_ns) return ScanError::kAgain;

  // Binary search for the first pose at or after t.
  std::size_t lo = 0, hi = poses_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (poses_[mid].t_mono_ns < t_mono_ns) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0 || poses_[lo].t_mono_ns == t_mono_ns) {
    *out = poses_[lo];
    return kOkStatus;
  }
  const Pose& a = poses_[lo - 1];
  const Pose& b = poses_[lo];
  const double span = static_cast<double>(b.t_mono_ns - a.t_mono_ns);
  const double u = span > 0.0 ? static_cast<double>(t_mono_ns - a.t_mono_ns) / span : 0.0;

  Pose r = a;
  r.t_mono_ns = t_mono_ns;
  for (int i = 0; i < 3; ++i) r.position[i] = a.position[i] + (b.position[i] - a.position[i]) * u;
  interp_quat(a.orientation, b.orientation, u, r.orientation);
  r.position_sigma_m = a.position_sigma_m + (b.position_sigma_m - a.position_sigma_m) *
                                                static_cast<float>(u);
  r.orientation_sigma_deg =
      a.orientation_sigma_deg + (b.orientation_sigma_deg - a.orientation_sigma_deg) *
                                    static_cast<float>(u);
  // The pessimistic end of the bracket: an interpolated pose is no better
  // than the worse of the two it came from.
  r.quality = a.quality < b.quality ? a.quality : b.quality;
  r.tracking_lost = static_cast<std::uint8_t>(a.tracking_lost | b.tracking_lost);
  *out = r;
  return kOkStatus;
}

bool LioPoseSource::latest(Pose* out) const {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lk(m_);
  if (poses_.empty()) return false;
  *out = poses_.back();
  return true;
}

std::size_t LioPoseSource::size() const {
  std::lock_guard<std::mutex> lk(m_);
  return poses_.size();
}

void LioPoseSource::clear() {
  std::lock_guard<std::mutex> lk(m_);
  poses_.clear();
  have_last_ = false;
  length_m_ = 0.0;
}

double LioPoseSource::trajectory_length_m() const {
  std::lock_guard<std::mutex> lk(m_);
  return length_m_;
}

}  // namespace scanengine
