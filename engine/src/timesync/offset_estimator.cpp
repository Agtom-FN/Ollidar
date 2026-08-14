#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

void PassthroughOffsetEstimator::add_pair(std::int64_t t_device_ns, TimePoint t_arrival) {
  std::lock_guard<std::mutex> lock(m_);
  const std::int64_t offset = t_arrival.nanos - t_device_ns;
  if (est_.valid) {
    const std::int64_t residual = offset - est_.offset_ns;
    est_.jitter_ns = residual >= 0 ? residual : -residual;
  }
  est_.offset_ns = offset;
  est_.valid = true;
  ++est_.samples;
}

OffsetEstimate PassthroughOffsetEstimator::estimate() const {
  std::lock_guard<std::mutex> lock(m_);
  return est_;
}

void PassthroughOffsetEstimator::reset() {
  std::lock_guard<std::mutex> lock(m_);
  est_ = OffsetEstimate{};
}

TimeSync::TimeSync() = default;
TimeSync::~TimeSync() = default;

OffsetEstimator& TimeSync::estimator(StreamId stream) {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) {
    it = estimators_.emplace(stream, std::make_unique<PassthroughOffsetEstimator>()).first;
  }
  return *it->second;
}

Status TimeSync::set_estimator(StreamId stream, std::unique_ptr<OffsetEstimator> est) {
  if (!est) return set_last_error(ScanError::kInvalidArgument, "timesync: null estimator");
  std::lock_guard<std::mutex> lock(m_);
  estimators_[stream] = std::move(est);
  return kOkStatus;
}

OffsetEstimate TimeSync::estimate(StreamId stream) const {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) return OffsetEstimate{};
  return it->second->estimate();
}

void TimeSync::reset_all() {
  std::lock_guard<std::mutex> lock(m_);
  for (auto& kv : estimators_) kv.second->reset();
}

}  // namespace scanengine
