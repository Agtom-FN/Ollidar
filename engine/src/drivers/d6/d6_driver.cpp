#include "scanengine/drivers/d6/d6_driver.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/drivers/d6/commands.h"

namespace scanengine {

const char* to_string(D6Phase p) noexcept {
  switch (p) {
    case D6Phase::kIdle: return "idle";
    case D6Phase::kStarting: return "starting";
    case D6Phase::kStreaming: return "streaming";
    case D6Phase::kDegradedChecksum: return "degraded-checksum";
    case D6Phase::kStalled: return "stalled";
    case D6Phase::kRestarting: return "restarting";
    case D6Phase::kStopping: return "stopping";
    case D6Phase::kFault: return "fault";
  }
  return "?";
}

const char* to_string(D6StallKind k) noexcept {
  switch (k) {
    case D6StallKind::kNone: return "none";
    case D6StallKind::kSilent: return "silent";
    case D6StallKind::kGarbage: return "garbage";
  }
  return "?";
}

const char* to_string(D6ChecksumVerdict v) noexcept {
  switch (v) {
    case D6ChecksumVerdict::kUndetermined: return "undetermined";
    case D6ChecksumVerdict::kVendorConfirmed: return "vendor-confirmed";
    case D6ChecksumVerdict::kSpecConfirmed: return "spec-confirmed";
    case D6ChecksumVerdict::kAmbiguous: return "ambiguous";
  }
  return "?";
}

namespace {

constexpr const char* kMod = "d6";
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// Matches REPORT.md §2's confirmation bar (the S1 exit criterion).
constexpr double kChecksumVerdictThreshold = 0.995;

DeviceState to_device_state(D6Phase p) noexcept {
  switch (p) {
    case D6Phase::kIdle: return DeviceState::kIdle;
    case D6Phase::kStarting: return DeviceState::kStarting;
    case D6Phase::kRestarting: return DeviceState::kStarting;
    case D6Phase::kStreaming: return DeviceState::kStreaming;
    case D6Phase::kDegradedChecksum: return DeviceState::kDegraded;
    case D6Phase::kStalled: return DeviceState::kDegraded;
    case D6Phase::kStopping: return DeviceState::kStopping;
    case D6Phase::kFault: return DeviceState::kFault;
  }
  return DeviceState::kFault;
}

}  // namespace

D6Driver::D6Driver(DeviceId id, const D6Config& cfg, const DriverContext& ctx)
    : id_(id), cfg_(cfg), ctx_(ctx), parser_(cfg.parser) {
  serial_ = std::make_unique<UsbSerialSource>(cfg_.serial);
  serial_->set_sink([this](ByteSpan bytes, TimePoint t) { on_bytes(bytes, t); });
  parser_.set_point_callback([this](const d6::Point& p) { on_point(p); });
  batch_.reserve(cfg_.max_batch_points);
  state_ = DeviceState::kIdle;
  phase_ = D6Phase::kIdle;
  current_checksum_variant_.store(
      cfg.parser.checksum == d6::ChecksumVariant::kVendorSdk ? 0 : 1, std::memory_order_relaxed);
}

D6Driver::~D6Driver() {
  // Drop the parser callback before the members it captures die.
  parser_.set_point_callback(nullptr);
}

TimePoint D6Driver::current_time() const {
  return (ctx_.clock != nullptr) ? ctx_.clock() : SteadyClock::now();
}

D6Phase D6Driver::get_phase() const {
  std::lock_guard<std::mutex> lock(m_);
  return phase_;
}

Status D6Driver::start() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (phase_ == D6Phase::kStreaming || phase_ == D6Phase::kStarting ||
        phase_ == D6Phase::kDegradedChecksum || phase_ == D6Phase::kStalled ||
        phase_ == D6Phase::kRestarting) {
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
    restart_attempts_ = 0;
    t_next_restart_allowed_ns_ = 0;
    stall_ = D6StallKind::kNone;
    saw_start_ack_ = false;
  }
  set_phase(D6Phase::kStarting, ScanError::kOk);

  if (cfg_.send_start_stop_commands && cfg_.serial.write_fn != nullptr) {
    const Status s = serial_->write(ByteSpan(d6::kCmdStart, sizeof(d6::kCmdStart)));
    if (!s.ok()) {
      set_phase(D6Phase::kFault, s.error());
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
    if (phase_ == D6Phase::kIdle) return kOkStatus;
  }
  set_phase(D6Phase::kStopping, ScanError::kOk);
  if (cfg_.send_start_stop_commands && cfg_.serial.write_fn != nullptr) {
    const Status s = serial_->write(ByteSpan(d6::kCmdStop, sizeof(d6::kCmdStop)));
    if (!s.ok()) SCAN_LOG_WARN(kMod, "device %u: stop command failed (%s)", id_, s.message());
  }
  flush_batch(t_current_ns_);
  (void)serial_->stop();
  {
    std::lock_guard<std::mutex> lock(m_);
    restart_attempts_ = 0;
    stall_ = D6StallKind::kNone;
  }
  set_phase(D6Phase::kIdle, ScanError::kOk);
  return kOkStatus;
}

DeviceState D6Driver::state() const {
  const_cast<D6Driver*>(this)->check_watchdog(current_time());
  std::lock_guard<std::mutex> lock(m_);
  return state_;
}

Status D6Driver::push_bytes(ByteSpan bytes, TimePoint t_arrival) {
  return serial_->push(bytes, t_arrival);
}

void D6Driver::on_bytes(ByteSpan bytes, TimePoint t) {
  t_current_ns_ = t.nanos;
  {
    std::lock_guard<std::mutex> lock(m_);
    t_last_bytes_ns_ = t.nanos;
  }

  scan_for_acks(bytes);

  // Apply a pending checksum-variant switch here — this is the only thread
  // that ever calls parser_.feed()/parser_.set_config(), so this is the only
  // place that may touch it without racing the parser's internal state.
  const int desired = desired_checksum_variant_.exchange(-1, std::memory_order_relaxed);
  if (desired != -1) {
    const d6::Stats pre = parser_.stats();
    checksum_baseline_ok_ = pre.packets_ok;
    checksum_baseline_bad_ = pre.packets_bad_checksum;
    d6::Config c = parser_.config();
    c.checksum = (desired == 0) ? d6::ChecksumVariant::kVendorSdk : d6::ChecksumVariant::kSpecLiteral;
    parser_.set_config(c);
    current_checksum_variant_.store(desired, std::memory_order_relaxed);
    SCAN_LOG_INFO(kMod, "device %u: checksum acceptance switched to %s", id_,
                  desired == 0 ? "vendor" : "spec");
  }

  // ROUND 7: feed in byte slices, each with its own back-dated arrival time,
  // so a point's timestamp reflects when its bytes actually came off the wire
  // rather than when the last byte of a possibly-178 ms chunk did. See
  // D6Config::time_slice_bytes for why this is the whole of the "walls are not
  // straight" fix on the timing side.
  feed_time_sliced(bytes, t.nanos);
  flush_batch(t.nanos);

  const d6::Stats st = parser_.stats();
  bool progressed;
  {
    std::lock_guard<std::mutex> lock(m_);
    progressed = st.packets_ok > last_packets_ok_seen_;
    last_packets_ok_seen_ = st.packets_ok;
    if (progressed) t_last_valid_packet_ns_ = t.nanos;
  }

  // Promote/demote from what the parser saw.
  const D6Phase phase = get_phase();
  if (progressed && (phase == D6Phase::kStarting || phase == D6Phase::kIdle ||
                     phase == D6Phase::kRestarting || phase == D6Phase::kStalled)) {
    if (!cfg_.require_start_ack || saw_start_ack_) {
      {
        std::lock_guard<std::mutex> lock(m_);
        restart_attempts_ = 0;
        stall_ = D6StallKind::kNone;
      }
      set_phase(D6Phase::kStreaming, ScanError::kOk);
    }
  }

  // Rate this window since the last accepted-variant switch (see
  // checksum_baseline_ok_/_bad_'s doc comment), not the parser's lifetime
  // totals -- packets rejected under a since-corrected variant should not
  // permanently pin the device at kDegradedChecksum.
  const std::uint64_t win_ok = st.packets_ok - checksum_baseline_ok_;
  const std::uint64_t win_bad = st.packets_bad_checksum - checksum_baseline_bad_;
  const std::uint64_t win_total = win_ok + win_bad;
  const double win_rate = win_total > 0 ? static_cast<double>(win_ok) / static_cast<double>(win_total) : 0.0;

  const D6Phase phase2 = get_phase();
  if (phase2 == D6Phase::kStreaming && win_total >= cfg_.health_min_packets &&
      win_rate < cfg_.min_checksum_pass_rate) {
    set_phase(D6Phase::kDegradedChecksum, ScanError::kChecksumFailed);
  } else if (phase2 == D6Phase::kDegradedChecksum && win_rate >= cfg_.min_checksum_pass_rate) {
    set_phase(D6Phase::kStreaming, ScanError::kOk);
  }

  check_watchdog(t);
}

void D6Driver::scan_for_acks(ByteSpan bytes) {
  // Cheap: only while we are expecting one. The parser treats ACK bytes as
  // garbage (they are not 0xAA55-framed), which is correct — they just need
  // to be observed before they are discarded.
  const D6Phase p = get_phase();
  if (p != D6Phase::kStarting && p != D6Phase::kStopping && p != D6Phase::kRestarting) return;
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
      set_phase(D6Phase::kFault, ScanError::kDeviceFault);
      break;
    default:
      break;
  }
}

void D6Driver::check_watchdog(TimePoint now) {
  const D6Phase phase = get_phase();
  if (phase != D6Phase::kStarting && phase != D6Phase::kStreaming &&
      phase != D6Phase::kDegradedChecksum && phase != D6Phase::kStalled &&
      phase != D6Phase::kRestarting) {
    return;
  }

  std::int64_t t_start, t_last_bytes, t_last_valid, t_next_restart;
  {
    std::lock_guard<std::mutex> lock(m_);
    t_start = t_start_ns_;
    t_last_bytes = t_last_bytes_ns_;
    t_last_valid = t_last_valid_packet_ns_;
    t_next_restart = t_next_restart_allowed_ns_;
  }

  if (now.nanos - t_start < cfg_.startup_grace_ns) return;  // still settling

  const bool silent = (now.nanos - t_last_bytes) >= cfg_.silent_stall_timeout_ns;
  const bool garbage = !silent && (now.nanos - t_last_valid) >= cfg_.garbage_stall_timeout_ns;
  if (!silent && !garbage) return;  // nothing wrong (or already recovering via on_bytes)

  const D6StallKind kind = silent ? D6StallKind::kSilent : D6StallKind::kGarbage;
  const ScanError err = silent ? ScanError::kDeviceNotResponding : ScanError::kProtocolError;

  if (phase == D6Phase::kStarting || phase == D6Phase::kStreaming ||
      phase == D6Phase::kDegradedChecksum) {
    {
      std::lock_guard<std::mutex> lock(m_);
      stall_ = kind;
    }
    set_phase(D6Phase::kStalled, err);
    attempt_restart(now);
    return;
  }
  if (phase == D6Phase::kStalled) {
    {
      std::lock_guard<std::mutex> lock(m_);
      stall_ = kind;
    }
    attempt_restart(now);
    return;
  }
  if (phase == D6Phase::kRestarting && now.nanos >= t_next_restart) {
    attempt_restart(now);
  }
}

void D6Driver::attempt_restart(TimePoint now) {
  bool exhausted;
  {
    std::lock_guard<std::mutex> lock(m_);
    exhausted = restart_attempts_ >= cfg_.max_restart_attempts;
  }
  if (exhausted) {
    SCAN_LOG_ERROR(kMod, "device %u: restart budget exhausted (%u attempts)", id_,
                   cfg_.max_restart_attempts);
    set_phase(D6Phase::kFault, ScanError::kDeviceNotResponding);
    return;
  }

  if (!(cfg_.send_start_stop_commands && cfg_.serial.write_fn != nullptr)) {
    // No command channel: nothing we can actively do to reacquire the
    // device. Stay in kStalled (mapped to kDegraded) rather than pretending
    // to retry — the app owns this transport and may stop() it itself.
    return;
  }

  std::uint32_t attempt_no;
  std::int64_t backoff;
  {
    std::lock_guard<std::mutex> lock(m_);
    ++restart_attempts_;
    attempt_no = restart_attempts_;
    const std::uint32_t shift = std::min<std::uint32_t>(attempt_no - 1, 30);
    backoff = cfg_.restart_backoff_base_ns << shift;
    if (backoff <= 0 || backoff > cfg_.restart_backoff_max_ns) backoff = cfg_.restart_backoff_max_ns;
    t_next_restart_allowed_ns_ = now.nanos + backoff;
    saw_start_ack_ = false;
  }
  set_phase(D6Phase::kRestarting, ScanError::kDeviceNotResponding);

  // Best-effort stop, then start: resets any stuck framing state on the
  // device before asking it to stream again.
  (void)serial_->write(ByteSpan(d6::kCmdStop, sizeof(d6::kCmdStop)));
  const Status s = serial_->write(ByteSpan(d6::kCmdStart, sizeof(d6::kCmdStart)));
  SCAN_LOG_WARN(kMod, "device %u: restart attempt %u/%u after a %s stall%s", id_, attempt_no,
                cfg_.max_restart_attempts, to_string(stall_), s.ok() ? "" : " (write failed)");

  if (ctx_.bus != nullptr) {
    ErrorPayload e{};
    e.error = ScanError::kDeviceNotResponding;
    e.device = id_;
    e.stream = StreamId::kLidarD6;
    ctx_.bus->publish(EventType::kError, e, now.nanos);
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

  // A8 seam: the polar return, before any frame convention is applied. The
  // assembler needs the per-point time (a revolution spans 100 ms = 10 cm of
  // rig travel at walking pace), which is why this is a per-point callback
  // rather than a per-batch one.
  if (cfg_.profile_sink != nullptr) {
    // ROUND 7: the POINT's own stamp, not the driver's "time of the current
    // chunk". With D6Config::time_slice_bytes these differ by up to a whole
    // chunk (178 ms on the phone's 4 KB reads), and the difference is the
    // per-revolution shingling the assembler was blamed for.
    const std::int64_t t_point = static_cast<std::int64_t>(p.t_rx_ns);
    cfg_.profile_sink(p.angle_deg, static_cast<float>(p.distance_mm) * 0.001f, p.intensity,
                      p.high_reflectivity ? std::uint8_t{1} : std::uint8_t{0},
                      t_point != 0 ? t_point : t_current_ns_,
                      cfg_.profile_sink_user_data);
  }

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

std::int64_t D6Driver::byte_period_ns() const {
  const std::uint32_t baud = cfg_.serial.baud;
  const std::uint32_t bits = cfg_.wire_bits_per_byte;
  if (baud == 0 || bits == 0) return 0;
  return static_cast<std::int64_t>(bits) * 1'000'000'000LL / static_cast<std::int64_t>(baud);
}

void D6Driver::feed_time_sliced(ByteSpan bytes, std::int64_t t_chunk_end_ns) {
  const std::size_t n = bytes.size();
  const std::int64_t byte_ns = byte_period_ns();
  const std::size_t slice = cfg_.time_slice_bytes;

  // Legacy path: one stamp for the whole chunk. Kept reachable (slice == 0, or
  // an unknown baud) so a transport that is not a fixed-rate UART — a replay
  // that hands over a whole file, a test that pushes one packet — is not
  // back-dated by a byte rate that does not describe it.
  if (n == 0 || slice == 0 || byte_ns <= 0 || n <= slice) {
    t_current_ns_ = t_chunk_end_ns;
    parser_.feed(bytes.data(), n, static_cast<std::uint64_t>(t_chunk_end_ns));
    return;
  }

  for (std::size_t off = 0; off < n; off += slice) {
    const std::size_t end = (off + slice < n) ? off + slice : n;
    // The time byte `end - 1` arrived, given that byte `n - 1` arrived at
    // t_chunk_end_ns and the wire runs at a constant byte rate.
    const std::int64_t t_slice =
        t_chunk_end_ns - static_cast<std::int64_t>(n - end) * byte_ns;
    t_current_ns_ = t_slice;
    parser_.feed(bytes.data() + off, end - off, static_cast<std::uint64_t>(t_slice));
  }
  // Leave the driver's notion of "now" at the chunk's own arrival, so the
  // watchdog/health bookkeeping below is unchanged by the slicing.
  t_current_ns_ = t_chunk_end_ns;
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

void D6Driver::set_phase(D6Phase next, ScanError err) {
  D6Phase prev_phase;
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
    p.kind = DeviceKind::kD6;
    p.state = next_state;
    p.previous = prev_state;
    p.error = err;
    ctx_.bus->publish(EventType::kDeviceState, p, t_current_ns_);
  }
}

d6::Stats D6Driver::parser_stats() const { return parser_.stats(); }

DeviceHealth D6Driver::health() const {
  const_cast<D6Driver*>(this)->check_watchdog(current_time());
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

D6HealthSnapshot D6Driver::snapshot() const {
  const_cast<D6Driver*>(this)->check_watchdog(current_time());
  const d6::Stats st = parser_.stats();
  D6HealthSnapshot h{};
  {
    std::lock_guard<std::mutex> lock(m_);
    h.phase = phase_;
    h.stall = stall_;
    h.restart_attempts = restart_attempts_;
    h.t_last_bytes_ns = t_last_bytes_ns_;
    h.t_last_valid_packet_ns = t_last_valid_packet_ns_;
  }
  h.points_per_sec = st.points_per_sec;
  h.rotation_hz = st.rotation_hz;
  h.checksum_pass_rate = st.checksum_pass_rate();
  h.resyncs = st.resyncs;
  h.bytes_in = st.bytes_in;
  h.cs_ok_vendor = st.cs_ok_vendor;
  h.cs_ok_spec = st.cs_ok_spec;
  h.accepted_variant = checksum_variant();
  h.checksum_verdict = checksum_verdict();
  return h;
}

d6::ChecksumVariant D6Driver::checksum_variant() const {
  return current_checksum_variant_.load(std::memory_order_relaxed) == 0
             ? d6::ChecksumVariant::kVendorSdk
             : d6::ChecksumVariant::kSpecLiteral;
}

void D6Driver::set_checksum_variant(d6::ChecksumVariant v) {
  desired_checksum_variant_.store(v == d6::ChecksumVariant::kVendorSdk ? 0 : 1,
                                  std::memory_order_relaxed);
}

D6ChecksumVerdict D6Driver::checksum_verdict() const {
  const d6::Stats st = parser_.stats();
  const std::uint64_t total = st.packets_ok + st.packets_bad_checksum;
  if (total < cfg_.health_min_packets) return D6ChecksumVerdict::kUndetermined;
  const double vendor_rate = static_cast<double>(st.cs_ok_vendor) / static_cast<double>(total);
  const double spec_rate = static_cast<double>(st.cs_ok_spec) / static_cast<double>(total);
  const bool vendor_confirmed = vendor_rate >= kChecksumVerdictThreshold;
  const bool spec_confirmed = spec_rate >= kChecksumVerdictThreshold;
  if (vendor_confirmed && spec_confirmed) return D6ChecksumVerdict::kAmbiguous;
  if (vendor_confirmed) return D6ChecksumVerdict::kVendorConfirmed;
  if (spec_confirmed) return D6ChecksumVerdict::kSpecConfirmed;
  return D6ChecksumVerdict::kUndetermined;
}

}  // namespace scanengine
