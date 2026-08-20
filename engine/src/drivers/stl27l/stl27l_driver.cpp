#include "scanengine/drivers/stl27l/stl27l_driver.h"

#include <algorithm>

#include "scanengine/core/log.h"
// The ONE fan-frame definition in this engine. See stl27l_driver.h's header
// comment: this driver calls it, it does not restate it.
#include "scanengine/drivers/d6/d6_fan.h"

namespace scanengine {

const char* to_string(Stl27lPhase p) noexcept {
  switch (p) {
    case Stl27lPhase::kIdle: return "idle";
    case Stl27lPhase::kStarting: return "starting";
    case Stl27lPhase::kStreaming: return "streaming";
    case Stl27lPhase::kDegradedCrc: return "degraded-crc";
    case Stl27lPhase::kStalled: return "stalled";
    case Stl27lPhase::kStopping: return "stopping";
    case Stl27lPhase::kFault: return "fault";
  }
  return "?";
}

const char* to_string(Stl27lStallKind k) noexcept {
  switch (k) {
    case Stl27lStallKind::kNone: return "none";
    case Stl27lStallKind::kSilent: return "silent";
    case Stl27lStallKind::kGarbage: return "garbage";
  }
  return "?";
}

namespace {

constexpr const char* kMod = "stl27l";

DeviceState to_device_state(Stl27lPhase p) noexcept {
  switch (p) {
    case Stl27lPhase::kIdle: return DeviceState::kIdle;
    case Stl27lPhase::kStarting: return DeviceState::kStarting;
    case Stl27lPhase::kStreaming: return DeviceState::kStreaming;
    case Stl27lPhase::kDegradedCrc: return DeviceState::kDegraded;
    case Stl27lPhase::kStalled: return DeviceState::kDegraded;
    case Stl27lPhase::kStopping: return DeviceState::kStopping;
    case Stl27lPhase::kFault: return DeviceState::kFault;
  }
  return DeviceState::kFault;
}

}  // namespace

Stl27lDriver::Stl27lDriver(DeviceId id, const Stl27lConfig& cfg, const DriverContext& ctx)
    : id_(id), cfg_(cfg), ctx_(ctx), parser_(cfg.parser) {
  // A caller that left the transport's baud at the UsbSerialConfig default
  // (230400, which is the D6's) means "the default for THIS sensor". The
  // engine never opens the port, but the baud is what byte_period_ns() dates
  // every point with, so a wrong value here silently mis-times the whole
  // capture rather than failing loudly.
  if (cfg_.serial.baud == 0 || cfg_.serial.baud == 230400) {
    cfg_.serial.baud = stl27l::kDefaultBaud;
  }
  serial_ = std::make_unique<UsbSerialSource>(cfg_.serial);
  serial_->set_sink([this](ByteSpan bytes, TimePoint t) { on_bytes(bytes, t); });
  parser_.set_point_callback([this](const stl27l::Point& p) { on_point(p); });
  batch_.reserve(cfg_.max_batch_points);
  state_ = DeviceState::kIdle;
  phase_ = Stl27lPhase::kIdle;
}

Stl27lDriver::~Stl27lDriver() {
  // Drop the parser callback before the members it captures die.
  parser_.set_point_callback(nullptr);
}

TimePoint Stl27lDriver::current_time() const {
  return (ctx_.clock != nullptr) ? ctx_.clock() : SteadyClock::now();
}

Stl27lPhase Stl27lDriver::get_phase() const {
  std::lock_guard<std::mutex> lock(m_);
  return phase_;
}

Status Stl27lDriver::start() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (phase_ == Stl27lPhase::kStreaming || phase_ == Stl27lPhase::kStarting ||
        phase_ == Stl27lPhase::kDegradedCrc || phase_ == Stl27lPhase::kStalled) {
      return kOkStatus;
    }
  }
  SCAN_TRY(serial_->start());

  const TimePoint now = current_time();
  {
    std::lock_guard<std::mutex> lock(m_);
    t_start_ns_ = now.nanos;
    t_last_bytes_ns_ = now.nanos;
    t_last_valid_packet_ns_ = now.nanos;
    last_packets_ok_seen_ = parser_.stats().packets_ok;
    stall_ = Stl27lStallKind::kNone;
  }
  set_phase(Stl27lPhase::kStarting, ScanError::kOk);
  // No command to send: the LD-series streams from power-on. The first decoded
  // packet promotes to kStreaming.
  SCAN_LOG_DEBUG(kMod, "device %u: waiting for data (%u baud, free-running device)", id_,
                 cfg_.serial.baud);
  return kOkStatus;
}

Status Stl27lDriver::stop() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (phase_ == Stl27lPhase::kIdle) return kOkStatus;
  }
  set_phase(Stl27lPhase::kStopping, ScanError::kOk);
  flush_batch(t_current_ns_);
  (void)serial_->stop();
  {
    std::lock_guard<std::mutex> lock(m_);
    stall_ = Stl27lStallKind::kNone;
  }
  set_phase(Stl27lPhase::kIdle, ScanError::kOk);
  return kOkStatus;
}

DeviceState Stl27lDriver::state() const {
  const_cast<Stl27lDriver*>(this)->check_watchdog(current_time());
  std::lock_guard<std::mutex> lock(m_);
  return state_;
}

Status Stl27lDriver::push_bytes(ByteSpan bytes, TimePoint t_arrival) {
  return serial_->push(bytes, t_arrival);
}

void Stl27lDriver::on_bytes(ByteSpan bytes, TimePoint t) {
  t_current_ns_ = t.nanos;
  {
    std::lock_guard<std::mutex> lock(m_);
    t_last_bytes_ns_ = t.nanos;
  }

  feed_time_sliced(bytes, t.nanos);
  flush_batch(t.nanos);

  const stl27l::Stats st = parser_.stats();
  bool progressed;
  {
    std::lock_guard<std::mutex> lock(m_);
    progressed = st.packets_ok > last_packets_ok_seen_;
    last_packets_ok_seen_ = st.packets_ok;
    if (progressed) t_last_valid_packet_ns_ = t.nanos;
  }

  const Stl27lPhase phase = get_phase();
  if (progressed && (phase == Stl27lPhase::kStarting || phase == Stl27lPhase::kIdle ||
                     phase == Stl27lPhase::kStalled)) {
    {
      std::lock_guard<std::mutex> lock(m_);
      stall_ = Stl27lStallKind::kNone;
    }
    set_phase(Stl27lPhase::kStreaming, ScanError::kOk);
  }

  const std::uint64_t total = st.packets_ok + st.packets_bad_crc;
  const double rate = st.crc_pass_rate();
  const Stl27lPhase phase2 = get_phase();
  if (phase2 == Stl27lPhase::kStreaming && total >= cfg_.health_min_packets &&
      rate < cfg_.min_crc_pass_rate) {
    set_phase(Stl27lPhase::kDegradedCrc, ScanError::kChecksumFailed);
  } else if (phase2 == Stl27lPhase::kDegradedCrc && rate >= cfg_.min_crc_pass_rate) {
    set_phase(Stl27lPhase::kStreaming, ScanError::kOk);
  }

  check_watchdog(t);
}

void Stl27lDriver::check_watchdog(TimePoint now) {
  const Stl27lPhase phase = get_phase();
  if (phase != Stl27lPhase::kStarting && phase != Stl27lPhase::kStreaming &&
      phase != Stl27lPhase::kDegradedCrc && phase != Stl27lPhase::kStalled) {
    return;
  }

  std::int64_t t_start, t_last_bytes, t_last_valid;
  {
    std::lock_guard<std::mutex> lock(m_);
    t_start = t_start_ns_;
    t_last_bytes = t_last_bytes_ns_;
    t_last_valid = t_last_valid_packet_ns_;
  }

  if (now.nanos - t_start < cfg_.startup_grace_ns) return;  // motor still spinning up

  const bool silent = (now.nanos - t_last_bytes) >= cfg_.silent_stall_timeout_ns;
  const bool garbage = !silent && (now.nanos - t_last_valid) >= cfg_.garbage_stall_timeout_ns;
  if (!silent && !garbage) return;

  const Stl27lStallKind kind = silent ? Stl27lStallKind::kSilent : Stl27lStallKind::kGarbage;
  const ScanError err = silent ? ScanError::kDeviceNotResponding : ScanError::kProtocolError;
  {
    std::lock_guard<std::mutex> lock(m_);
    stall_ = kind;
  }
  if (phase != Stl27lPhase::kStalled) {
    set_phase(Stl27lPhase::kStalled, err);
    // No restart is attempted: there is no command channel to retry with (see
    // stl27l_driver.h). Publishing the error is what the app acts on — it owns
    // the transport and can re-open or power-cycle it.
    if (ctx_.bus != nullptr) {
      ErrorPayload e{};
      e.error = err;
      e.device = id_;
      e.stream = StreamId::kLidarStl27l;
      ctx_.bus->publish(EventType::kError, e, now.nanos);
    }
  }
}

void Stl27lDriver::on_point(const stl27l::Point& p) {
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

  const double angle_deg = cfg_.invert_angle ? (360.0 - static_cast<double>(p.angle_deg))
                                             : static_cast<double>(p.angle_deg);

  // A8 seam: the polar return, before any frame convention is applied, with
  // the POINT's own time (a revolution spans 100 ms = 10 cm of rig travel at
  // walking pace, so a per-chunk stamp is not good enough — this is the D6's
  // ROUND 7/ROUND 9 finding and it applies unchanged here).
  if (cfg_.profile_sink != nullptr) {
    const std::int64_t t_point = p.t_sample_ns != 0 ? static_cast<std::int64_t>(p.t_sample_ns)
                                                    : static_cast<std::int64_t>(p.t_rx_ns);
    cfg_.profile_sink(static_cast<float>(angle_deg),
                      static_cast<float>(p.distance_mm) * 0.001f, p.intensity,
                      /*high_reflectivity=*/0, t_point != 0 ? t_point : t_current_ns_,
                      cfg_.profile_sink_user_data);
  }

  double p_lidar[3];
  d6::fan_point(angle_deg, static_cast<double>(p.distance_mm) * 0.001, p_lidar);

  PointVertex v{};
  v.x = static_cast<float>(p_lidar[0]);
  v.y = static_cast<float>(p_lidar[1]);
  v.z = 0.0f;  // A8 replaces this with the trajectory-assembled 3-D position.
  // Intensity as greyscale. The LD protocol has no high-reflectivity flag, so
  // unlike the D6 there is nothing to tint.
  v.r = p.intensity;
  v.g = p.intensity;
  v.b = p.intensity;
  v.a = 255;

  batch_.push_back(v);
  ++points_in_rotation_;
  if (batch_.size() >= cfg_.max_batch_points) flush_batch(t_current_ns_);
}

std::int64_t Stl27lDriver::byte_period_ns() const {
  const std::uint32_t baud = cfg_.serial.baud;
  const std::uint32_t bits = cfg_.wire_bits_per_byte;
  if (baud == 0 || bits == 0) return 0;
  return static_cast<std::int64_t>(bits) * 1'000'000'000LL / static_cast<std::int64_t>(baud);
}

void Stl27lDriver::feed_time_sliced(ByteSpan bytes, std::int64_t t_chunk_end_ns) {
  const std::size_t n = bytes.size();
  const std::int64_t byte_ns = byte_period_ns();
  const std::size_t slice = cfg_.time_slice_bytes;

  // Legacy path: one stamp for the whole chunk. Kept reachable so a transport
  // that is not a fixed-rate UART (a replay handing over a whole file, a test
  // pushing one packet) is not back-dated by a byte rate that does not
  // describe it.
  if (n == 0 || slice == 0 || byte_ns <= 0 || n <= slice) {
    t_current_ns_ = t_chunk_end_ns;
    parser_.feed(bytes.data(), n, static_cast<std::uint64_t>(t_chunk_end_ns));
    return;
  }

  for (std::size_t off = 0; off < n; off += slice) {
    const std::size_t end = (off + slice < n) ? off + slice : n;
    const std::int64_t t_slice =
        t_chunk_end_ns - static_cast<std::int64_t>(n - end) * byte_ns;
    t_current_ns_ = t_slice;
    parser_.feed(bytes.data() + off, end - off, static_cast<std::uint64_t>(t_slice));
  }
  t_current_ns_ = t_chunk_end_ns;
}

void Stl27lDriver::flush_batch(std::int64_t t_ns) {
  if (batch_.empty() || ctx_.points == nullptr) return;

  std::uint32_t appended = 0;
  const Status s = ctx_.points->append(StreamId::kLidarStl27l,
                                       Span<const PointVertex>(batch_.data(), batch_.size()),
                                       t_ns, &appended);
  points_out_ += appended;
  if (!s.ok()) {
    drops_ += batch_.size() - appended;
    if (ctx_.bus != nullptr) {
      ErrorPayload e{};
      e.error = s.error();
      e.device = id_;
      e.stream = StreamId::kLidarStl27l;
      ctx_.bus->publish(EventType::kError, e, t_ns);
    }
  }
  batch_.clear();
}

void Stl27lDriver::set_phase(Stl27lPhase next, ScanError err) {
  Stl27lPhase prev_phase;
  DeviceState prev_state, next_state;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (phase_ == next && last_error_ == err) return;
    prev_phase = phase_;
    phase_ = next;
    last_error_ = err;
    prev_state = state_;
    next_state = to_device_state(next);
    state_ = next_state;
  }
  SCAN_LOG_INFO(kMod, "device %u: %s -> %s%s", id_, to_string(prev_phase), to_string(next),
                err == ScanError::kOk ? "" : error_str(err));
  if (ctx_.bus != nullptr) {
    DeviceStatePayload p{};
    p.device = id_;
    p.kind = DeviceKind::kStl27l;
    p.state = next_state;
    p.previous = prev_state;
    p.error = err;
    ctx_.bus->publish(EventType::kDeviceState, p, t_current_ns_);
  }
}

stl27l::Stats Stl27lDriver::parser_stats() const { return parser_.stats(); }

DeviceHealth Stl27lDriver::health() const {
  const_cast<Stl27lDriver*>(this)->check_watchdog(current_time());
  const stl27l::Stats st = parser_.stats();
  DeviceHealth h{};
  {
    std::lock_guard<std::mutex> lock(m_);
    h.state = state_;
    h.last_error = last_error_;
    h.points_out = points_out_;
    h.drops = drops_;
  }
  h.id = id_;
  h.kind = DeviceKind::kStl27l;
  h.bytes_in = st.bytes_in;
  h.packets_ok = st.packets_ok;
  h.packets_bad = st.packets_bad_crc + st.packets_malformed;
  h.points_per_sec = st.points_per_sec;
  h.rotation_hz = st.rotation_hz;
  h.checksum_pass_rate = st.crc_pass_rate();
  h.t_last_data_ns = serial_->stats().t_last_rx_ns;
  return h;
}

Stl27lHealthSnapshot Stl27lDriver::snapshot() const {
  const_cast<Stl27lDriver*>(this)->check_watchdog(current_time());
  const stl27l::Stats st = parser_.stats();
  Stl27lHealthSnapshot h{};
  {
    std::lock_guard<std::mutex> lock(m_);
    h.phase = phase_;
    h.stall = stall_;
    h.t_last_bytes_ns = t_last_bytes_ns_;
    h.t_last_valid_packet_ns = t_last_valid_packet_ns_;
  }
  h.points_per_sec = st.points_per_sec;
  h.rotation_hz = st.rotation_hz;
  h.crc_pass_rate = st.crc_pass_rate();
  h.sample_hz_est = st.sample_hz_est;
  h.speed_dps = st.speed_dps;
  h.resyncs = st.resyncs;
  h.bytes_in = st.bytes_in;
  h.packets_bad_crc = st.packets_bad_crc;
  h.packets_malformed = st.packets_malformed;
  return h;
}

}  // namespace scanengine
