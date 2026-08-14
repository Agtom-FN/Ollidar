#include "scanengine/timesync/offset_estimator.h"

#include "scanengine/core/event.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/core/log.h"
#include "scanengine/timesync/min_delay_estimator.h"

namespace scanengine {

const char* to_string(SyncQuality q) noexcept {
  switch (q) {
    case SyncQuality::kUnknown: return "unknown";
    case SyncQuality::kGood: return "good";
    case SyncQuality::kGated: return "gated";
    case SyncQuality::kPoor: return "poor";
  }
  return "unknown";
}

TimeModel OffsetEstimator::model() const {
  const OffsetEstimate e = estimate();
  TimeModel m;
  m.valid = e.valid;
  m.offset_ns = e.offset_ns;
  m.drift_ppm = e.drift_ppm;
  m.t_ref_device_ns = e.t_ref_device_ns;
  m.converged = e.converged;
  m.uncertainty_ns = e.converged ? e.jitter_ns : kUnconvergedUncertaintyNs;
  return m;
}

// --- passthrough ----------------------------------------------------------

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
  est_.t_ref_device_ns = t_device_ns;
  est_.t_updated_ns = t_arrival.nanos;
  // A passthrough stream is by definition already in the engine's clock
  // domain, so there is nothing to converge on beyond having two samples to
  // measure a spread from.
  est_.converged = est_.samples >= 2;
}

OffsetEstimate PassthroughOffsetEstimator::estimate() const {
  std::lock_guard<std::mutex> lock(m_);
  return est_;
}

void PassthroughOffsetEstimator::reset() {
  std::lock_guard<std::mutex> lock(m_);
  est_ = OffsetEstimate{};
}

// --- registry -------------------------------------------------------------

// One per stream, so the C-style observer callback can carry both the
// registry and the stream without a lambda capture.
struct TimeSync::Hook {
  TimeSync* self = nullptr;
  StreamId stream = StreamId::kUnknown;
};

TimeSync::TimeSync() = default;
TimeSync::~TimeSync() = default;

bool TimeSync::stream_has_device_clock(StreamId stream) noexcept {
  switch (stream) {
    case StreamId::kLidarMid360:  // 200 kHz sensor clock, free-running
    case StreamId::kImu:          // same clock as the Mid-360 points
    case StreamId::kGnss:         // NMEA UTC time, arrival-correlated (§3.2)
      return true;
    case StreamId::kLidarD6:      // no device clock at all — arrival stamps
    case StreamId::kPoseAr:       // ARCore is already CLOCK_BOOTTIME
    case StreamId::kCameraFrames: // ARCore frame timestamps, same domain
    case StreamId::kSlamMap:      // engine-produced, already in engine time
    case StreamId::kPoseLio:      // engine-produced, already in engine time
    case StreamId::kPoseFused:    // produced in engine time
    case StreamId::kUnknown:
      return false;
  }
  return false;
}

std::unique_ptr<OffsetEstimator> TimeSync::make_default_estimator(StreamId stream) {
  if (!stream_has_device_clock(stream)) {
    return std::make_unique<PassthroughOffsetEstimator>();
  }
  auto est = std::make_unique<MinDelayOffsetEstimator>();
  est->set_stream(stream);
  return est;
}

void TimeSync::attach_hook_(StreamId stream, OffsetEstimator& est) {
  auto* md = dynamic_cast<MinDelayOffsetEstimator*>(&est);
  if (md == nullptr) return;
  auto hook = std::make_unique<Hook>();
  hook->self = this;
  hook->stream = stream;
  md->set_stream(stream);
  md->set_resync_observer(&TimeSync::on_resync_, hook.get());
  hooks_[stream] = std::move(hook);
}

void TimeSync::on_resync_(const ClockResync& ev, void* user_data) {
  auto* hook = static_cast<Hook*>(user_data);
  if (hook == nullptr || hook->self == nullptr) return;
  TimeSync* self = hook->self;

  EventBus* bus = nullptr;
  DeviceId device = kInvalidDeviceId;
  void (*observer)(const ClockResync&, void*) = nullptr;
  void* observer_user = nullptr;
  {
    std::lock_guard<std::mutex> lock(self->sink_m_);
    ++self->resyncs_;
    bus = self->bus_;
    device = self->device_;
    observer = self->observer_;
    observer_user = self->observer_user_;
  }

  ClockResync out = ev;
  out.stream = hook->stream;

  // A routine gap is not a data-integrity problem; a device whose clock
  // moved under us is, and the app (and A5's recorder) must see it.
  if (bus != nullptr && (out.reason == ResyncReason::kDeviceReset ||
                         out.reason == ResyncReason::kClockStep)) {
    Event e;
    e.type = EventType::kError;
    e.payload.error.error = ScanError::kProtocolError;
    e.payload.error.device = device;
    e.payload.error.stream = out.stream;
    bus->publish(e);
  }
  if (observer != nullptr) observer(out, observer_user);
}

OffsetEstimator& TimeSync::estimator(StreamId stream) {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) {
    it = estimators_.emplace(stream, make_default_estimator(stream)).first;
    attach_hook_(stream, *it->second);
  }
  return *it->second;
}

Status TimeSync::set_estimator(StreamId stream, std::unique_ptr<OffsetEstimator> est) {
  if (!est) return set_last_error(ScanError::kInvalidArgument, "timesync: null estimator");
  std::lock_guard<std::mutex> lock(m_);
  OffsetEstimator& ref = *est;
  estimators_[stream] = std::move(est);
  hooks_.erase(stream);
  attach_hook_(stream, ref);
  return kOkStatus;
}

OffsetEstimate TimeSync::estimate(StreamId stream) const {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) return OffsetEstimate{};
  return it->second->estimate();
}

void TimeSync::add_pair(StreamId stream, std::int64_t t_device_ns, TimePoint t_arrival) {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) {
    it = estimators_.emplace(stream, make_default_estimator(stream)).first;
    attach_hook_(stream, *it->second);
  }
  it->second->add_pair(t_device_ns, t_arrival);
}

std::int64_t TimeSync::to_engine_time(StreamId stream, std::int64_t t_device_ns) const {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) return t_device_ns;
  return it->second->to_engine_time(t_device_ns);
}

TimeModel TimeSync::model(StreamId stream) const {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) return TimeModel{};
  return it->second->model();
}

SyncQuality TimeSync::quality(StreamId stream) const { return sync_quality(estimate(stream)); }

Status TimeSync::reset(StreamId stream) {
  std::lock_guard<std::mutex> lock(m_);
  auto it = estimators_.find(stream);
  if (it == estimators_.end()) {
    return set_last_error(ScanError::kNotFound, "timesync: no estimator for stream %d",
                          static_cast<int>(stream));
  }
  it->second->reset();
  return kOkStatus;
}

std::vector<StreamId> TimeSync::streams() const {
  std::lock_guard<std::mutex> lock(m_);
  std::vector<StreamId> out;
  out.reserve(estimators_.size());
  for (const auto& kv : estimators_) out.push_back(kv.first);
  return out;
}

void TimeSync::reset_all() {
  std::lock_guard<std::mutex> lock(m_);
  for (auto& kv : estimators_) kv.second->reset();
}

void TimeSync::set_event_bus(EventBus* bus, DeviceId device) {
  std::lock_guard<std::mutex> lock(sink_m_);
  bus_ = bus;
  device_ = device;
}

void TimeSync::set_resync_observer(void (*fn)(const ClockResync&, void*), void* user_data) {
  std::lock_guard<std::mutex> lock(sink_m_);
  observer_ = fn;
  observer_user_ = user_data;
}

std::uint32_t TimeSync::resyncs() const {
  std::lock_guard<std::mutex> lock(sink_m_);
  return resyncs_;
}

}  // namespace scanengine
