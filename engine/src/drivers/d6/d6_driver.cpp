#include "scanengine/drivers/d6/d6_driver.h"

#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/drivers/d6/commands.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "d6";
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

}  // namespace

D6Driver::D6Driver(DeviceId id, const D6Config& cfg, const DriverContext& ctx)
    : id_(id), cfg_(cfg), ctx_(ctx), parser_(cfg.parser) {
  serial_ = std::make_unique<UsbSerialSource>(cfg_.serial);
  serial_->set_sink([this](ByteSpan bytes, TimePoint t) { on_bytes(bytes, t); });
  parser_.set_point_callback([this](const d6::Point& p) { on_point(p); });
  batch_.reserve(cfg_.max_batch_points);
  state_ = DeviceState::kIdle;
}

D6Driver::~D6Driver() {
  // Drop the parser callback before the members it captures die.
  parser_.set_point_callback(nullptr);
}

Status D6Driver::start() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == DeviceState::kStreaming || state_ == DeviceState::kStarting) return kOkStatus;
  }
  SCAN_TRY(serial_->start());
  set_state(DeviceState::kStarting, ScanError::kOk);

  if (cfg_.send_start_stop_commands && cfg_.serial.write_fn != nullptr) {
    const Status s = serial_->write(ByteSpan(d6::kCmdStart, sizeof(d6::kCmdStart)));
    if (!s.ok()) {
      set_state(DeviceState::kFault, s.error());
      return s;
    }
    SCAN_LOG_INFO(kMod, "device %u: sent start command (AA 55 F0 0F)", id_);
  } else if (!cfg_.require_start_ack) {
    // No command channel: the app (or the device's power-on default) is
    // driving. First decoded packet promotes to kStreaming.
    SCAN_LOG_DEBUG(kMod, "device %u: no write function; waiting for data", id_);
  }
  return kOkStatus;
}

Status D6Driver::stop() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == DeviceState::kDisconnected || state_ == DeviceState::kIdle) {
      return kOkStatus;
    }
  }
  set_state(DeviceState::kStopping, ScanError::kOk);
  if (cfg_.send_start_stop_commands && cfg_.serial.write_fn != nullptr) {
    const Status s = serial_->write(ByteSpan(d6::kCmdStop, sizeof(d6::kCmdStop)));
    if (!s.ok()) SCAN_LOG_WARN(kMod, "device %u: stop command failed (%s)", id_, s.message());
  }
  flush_batch(t_current_ns_);
  (void)serial_->stop();
  set_state(DeviceState::kIdle, ScanError::kOk);
  return kOkStatus;
}

DeviceState D6Driver::state() const {
  std::lock_guard<std::mutex> lock(m_);
  return state_;
}

Status D6Driver::push_bytes(ByteSpan bytes, TimePoint t_arrival) {
  return serial_->push(bytes, t_arrival);
}

void D6Driver::on_bytes(ByteSpan bytes, TimePoint t) {
  t_current_ns_ = t.nanos;
  scan_for_acks(bytes);
  parser_.feed(bytes.data(), bytes.size(), static_cast<std::uint64_t>(t.nanos));
  flush_batch(t.nanos);

  // Promote/demote state from what the parser saw.
  const d6::Stats st = parser_.stats();
  DeviceState next = state();
  if (st.packets_ok > 0 && (next == DeviceState::kStarting || next == DeviceState::kIdle)) {
    if (!cfg_.require_start_ack || saw_start_ack_) next = DeviceState::kStreaming;
  }
  if (next == DeviceState::kStreaming && st.packets_ok + st.packets_bad_checksum >=
                                             cfg_.health_min_packets &&
      st.checksum_pass_rate() < cfg_.min_checksum_pass_rate) {
    next = DeviceState::kDegraded;
  } else if (next == DeviceState::kDegraded &&
             st.checksum_pass_rate() >= cfg_.min_checksum_pass_rate) {
    next = DeviceState::kStreaming;
  }
  if (next != state()) set_state(next, ScanError::kOk);
}

void D6Driver::scan_for_acks(ByteSpan bytes) {
  // Cheap: only while we are expecting one. The parser treats ACK bytes as
  // garbage (they are not 0xAA55-framed), which is correct — they just need
  // to be observed before they are discarded.
  const DeviceState s = state();
  if (s != DeviceState::kStarting && s != DeviceState::kStopping) return;
  std::size_t off = 0;
  const d6::Ack a = d6::find_ack(bytes.data(), bytes.size(), &off);
  switch (a) {
    case d6::Ack::kStartOk:
      saw_start_ack_ = true;
      SCAN_LOG_INFO(kMod, "device %u: start ACK", id_);
      break;
    case d6::Ack::kStopOk:
      SCAN_LOG_INFO(kMod, "device %u: stop ACK", id_);
      break;
    case d6::Ack::kError:
      SCAN_LOG_ERROR(kMod, "device %u: device reported an error ACK", id_);
      set_state(DeviceState::kFault, ScanError::kDeviceFault);
      break;
    default:
      break;
  }
}

void D6Driver::on_point(const d6::Point& p) {
  if (p.new_rotation) {
    if (points_in_rotation_ > 0 && ctx_.bus != nullptr) {
      RotationPayload r{};
      r.device = id_;
      r.rotation_index = rotations_seen_;
      r.points_in_rotation = points_in_rotation_;
      r.rotation_hz = parser_.stats().rotation_hz;
      ctx_.bus->publish(EventType::kRotation, r, t_current_ns_);
    }
    ++rotations_seen_;
    points_in_rotation_ = 0;
  }

  if (cfg_.drop_zero_range_points && p.distance_mm == 0) return;

  const double a = static_cast<double>(p.angle_deg) * kDegToRad;
  const double d = static_cast<double>(p.distance_mm) * 0.001;  // mm → m

  PointVertex v{};
  v.x = static_cast<float>(d * std::sin(a));
  v.y = static_cast<float>(d * std::cos(a));
  v.z = 0.0f;  // A8 replaces this with the trajectory-assembled 3-D position.
  // Intensity as greyscale; high-reflectivity returns tinted so the S1
  // reflective-post case is visible in the live view without a colour mode.
  v.r = p.intensity;
  v.g = p.high_reflectivity ? static_cast<std::uint8_t>(255) : p.intensity;
  v.b = p.intensity;
  v.a = 255;

  batch_.push_back(v);
  ++points_in_rotation_;
  if (batch_.size() >= cfg_.max_batch_points) flush_batch(t_current_ns_);
}

void D6Driver::flush_batch(std::int64_t t_ns) {
  if (batch_.empty() || ctx_.points == nullptr) return;

  std::uint32_t appended = 0;
  const Status s = ctx_.points->append(StreamId::kLidarD6,
                                       Span<const PointVertex>(batch_.data(), batch_.size()),
                                       t_ns, &appended);
  points_out_ += appended;
  if (!s.ok()) {
    drops_ += batch_.size() - appended;
    if (ctx_.bus != nullptr) {
      ErrorPayload e{};
      e.error = s.error();
      e.device = id_;
      e.stream = StreamId::kLidarD6;
      ctx_.bus->publish(EventType::kError, e, t_ns);
    }
  }
  batch_.clear();
  // The kPointsAvailable event is published by the Engine, which subscribes
  // to the PageStore — one place turns page updates into events, so every
  // producer (D6 now, Mid-360/SLAM later) gets identical semantics.
}

void D6Driver::set_state(DeviceState next, ScanError err) {
  DeviceState prev;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == next && last_error_ == err) return;
    prev = state_;
    state_ = next;
    last_error_ = err;
  }
  SCAN_LOG_INFO(kMod, "device %u: %s -> %s%s", id_, to_string(prev), to_string(next),
                err == ScanError::kOk ? "" : error_str(err));
  if (ctx_.bus != nullptr) {
    DeviceStatePayload p{};
    p.device = id_;
    p.kind = DeviceKind::kD6;
    p.state = next;
    p.previous = prev;
    p.error = err;
    ctx_.bus->publish(EventType::kDeviceState, p, t_current_ns_);
  }
}

d6::Stats D6Driver::parser_stats() const { return parser_.stats(); }

DeviceHealth D6Driver::health() const {
  const d6::Stats st = parser_.stats();
  DeviceHealth h{};
  {
    std::lock_guard<std::mutex> lock(m_);
    h.state = state_;
    h.last_error = last_error_;
    h.points_out = points_out_;
    h.drops = drops_;
  }
  h.id = id_;
  h.kind = DeviceKind::kD6;
  h.bytes_in = st.bytes_in;
  h.packets_ok = st.packets_ok;
  h.packets_bad = st.packets_bad_checksum + st.packets_malformed;
  h.points_per_sec = st.points_per_sec;
  h.rotation_hz = st.rotation_hz;
  h.checksum_pass_rate = st.checksum_pass_rate();
  h.t_last_data_ns = serial_->stats().t_last_rx_ns;
  return h;
}

}  // namespace scanengine
