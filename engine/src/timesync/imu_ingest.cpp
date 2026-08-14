#include "scanengine/timesync/imu_ingest.h"

#include <cmath>

namespace scanengine {

ImuIngest::ImuIngest(TimeSync& ts, StreamId stream, std::size_t capacity)
    : ts_(ts), stream_(stream) {
  if (capacity < 2) capacity = 2;
  ring_.resize(capacity);
}

ImuSample ImuIngest::add(std::int64_t t_device_ns, TimePoint t_arrival,
                         const float gyro_rad_s[3], const float accel_m_s2[3]) {
  // Feed the estimator first: the mapping this sample gets should include
  // the evidence this sample carries. At 200 Hz that is the difference
  // between a mapping that lags the stream by 5 ms and one that does not.
  ts_.add_pair(stream_, t_device_ns, t_arrival);
  const TimeModel model = ts_.model(stream_);

  ImuSample s;
  s.t_device_ns = t_device_ns;
  s.t_engine_ns = model.apply(t_device_ns);
  s.uncertainty_ns = model.uncertainty_ns;
  s.time_converged = model.converged;
  if (gyro_rad_s != nullptr) {
    for (int i = 0; i < 3; ++i) s.gyro_rad_s[i] = gyro_rad_s[i];
  }
  if (accel_m_s2 != nullptr) {
    for (int i = 0; i < 3; ++i) s.accel_m_s2[i] = accel_m_s2[i];
  }

  std::lock_guard<std::mutex> lock(m_);
  // An out-of-order IMU sample is a decoding bug or a reordered datagram;
  // either way, inserting it would make the ring non-monotonic and break the
  // window search in angular_rate_at(). Count it and drop it — but still
  // return the mapped sample, because the caller may want to log it.
  if (have_ && t_device_ns < last_device_ns_) {
    ++stats_.dropped_out_of_order;
    return s;
  }
  have_ = true;
  last_device_ns_ = t_device_ns;

  if (size_ < ring_.size()) {
    ring_[(head_ + size_) % ring_.size()] = s;
    ++size_;
  } else {
    ring_[head_] = s;
    head_ = (head_ + 1) % ring_.size();
    ++stats_.overwritten;
  }

  if (stats_.samples == 0) stats_.t_first_engine_ns = s.t_engine_ns;
  ++stats_.samples;
  stats_.t_last_engine_ns = s.t_engine_ns;
  stats_.uncertainty_ns = s.uncertainty_ns;
  stats_.converged = s.time_converged;

  const ImuSample& oldest = ring_[head_];
  const std::int64_t span = s.t_engine_ns - oldest.t_engine_ns;
  stats_.rate_hz = (size_ > 1 && span > 0)
                       ? static_cast<double>(size_ - 1) * 1e9 / static_cast<double>(span)
                       : 0.0;
  return s;
}

ImuSample ImuIngest::add_g(std::int64_t t_device_ns, TimePoint t_arrival,
                           const float gyro_rad_s[3], const float accel_g[3]) {
  float accel_m_s2[3] = {0.f, 0.f, 0.f};
  if (accel_g != nullptr) {
    for (int i = 0; i < 3; ++i) accel_m_s2[i] = accel_g[i] * kStandardGravity;
  }
  return add(t_device_ns, t_arrival, gyro_rad_s, accel_g != nullptr ? accel_m_s2 : nullptr);
}

bool ImuIngest::latest(ImuSample* out) const {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(m_);
  if (size_ == 0) return false;
  *out = ring_[(head_ + size_ - 1) % ring_.size()];
  return true;
}

std::size_t ImuIngest::recent(ImuSample* out, std::size_t max) const {
  if (out == nullptr || max == 0) return 0;
  std::lock_guard<std::mutex> lock(m_);
  const std::size_t n = size_ < max ? size_ : max;
  const std::size_t start = size_ - n;
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = ring_[(head_ + start + i) % ring_.size()];
  }
  return n;
}

bool ImuIngest::angular_rate_at(std::int64_t t_engine_ns, std::int64_t window_ns,
                                double* rad_per_s) const {
  if (rad_per_s == nullptr) return false;
  if (window_ns < 0) window_ns = -window_ns;
  std::lock_guard<std::mutex> lock(m_);
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < size_; ++i) {
    const ImuSample& s = ring_[(head_ + i) % ring_.size()];
    if (s.t_engine_ns < t_engine_ns - window_ns) continue;
    if (s.t_engine_ns > t_engine_ns) break;  // ring is time-ordered
    const double gx = s.gyro_rad_s[0];
    const double gy = s.gyro_rad_s[1];
    const double gz = s.gyro_rad_s[2];
    sum += std::sqrt(gx * gx + gy * gy + gz * gz);
    ++n;
  }
  if (n == 0) return false;
  *rad_per_s = sum / static_cast<double>(n);
  return true;
}

ImuIngestStats ImuIngest::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  return stats_;
}

void ImuIngest::reset() {
  std::lock_guard<std::mutex> lock(m_);
  head_ = 0;
  size_ = 0;
  have_ = false;
  last_device_ns_ = 0;
  stats_ = ImuIngestStats{};
}

}  // namespace scanengine
