#include "scanengine/drivers/imu_serial/imu_serial_driver.h"

#include <algorithm>
#include <chrono>

#include "scanengine/core/log.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "imu-serial";
constexpr int kSupervisorTickMs = 50;

std::int64_t seconds_to_ns(double s) { return static_cast<std::int64_t>(s * 1e9); }

}  // namespace

ImuSerialDriver::ImuSerialDriver(DeviceId id, const ImuSerialConfig& cfg, const DriverContext& ctx)
    : id_(id), cfg_(cfg), ctx_(ctx) {
  // The stamper's seed and the rate we ask the module for must not be able to
  // disagree; everything else in ImuStamperConfig stays as the caller set it.
  ImuStamperConfig sc = cfg_.stamper;
  sc.nominal_hz = cfg_.report_rate_hz > 0 ? static_cast<double>(cfg_.report_rate_hz) : 100.0;
  cfg_.stamper = sc;
  stamper_ = ImuStamper(sc);

  serial_ = std::make_unique<UsbSerialSource>(cfg_.serial);
  serial_->set_sink([this](ByteSpan bytes, TimePoint t) { on_bytes(bytes, t); });
  ring_.resize(cfg_.ring_capacity == 0 ? 1 : cfg_.ring_capacity);
  state_ = DeviceState::kIdle;
}

ImuSerialDriver::~ImuSerialDriver() {
  if (supervisor_.joinable()) {
    supervisor_run_.store(false, std::memory_order_release);
    supervisor_cv_.notify_all();
    supervisor_.join();
  }
  // Drop the transport sink before the members its lambda captures die.
  serial_->set_sink(nullptr);
}

TimePoint ImuSerialDriver::current_time() const {
  return (ctx_.clock != nullptr) ? ctx_.clock() : SteadyClock::now();
}

Status ImuSerialDriver::start() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (started_) return kOkStatus;
  }
  SCAN_TRY(serial_->start());

  const TimePoint now = current_time();
  {
    std::lock_guard<std::mutex> lock(m_);
    started_ = true;
    saw_first_sample_ = false;
    degraded_blackout_ = false;
    degraded_checksum_ = false;
    t_start_ns_ = now.nanos;
    t_last_sample_ns_ = 0;
    window_start_ns_ = now.nanos;
    window_samples_ = 0;
    rate_hz_ = 0.0;
  }
  set_state(DeviceState::kStarting, ScanError::kOk);

  if (cfg_.send_rate_command) {
    if (cfg_.serial.write_fn != nullptr) {
      std::uint8_t cmd[juxi::kRateCommandBytes] = {0};
      const std::size_t n = juxi::encode_rate_command(cfg_.report_rate_hz, cmd);
      if (n == 0) {
        // Out of the module's 10..100 Hz range. Refusing to send is the honest
        // outcome: the module keeps whatever rate it has persisted, and the
        // stamper is seeded from the rate we THINK we are getting, so silently
        // sending something else would make every published dt a fiction.
        SCAN_LOG_WARN(kMod, "device %u: report_rate_hz=%u outside %u..%u; rate command not sent",
                      id_, static_cast<unsigned>(cfg_.report_rate_hz),
                      static_cast<unsigned>(juxi::kMinRateHz),
                      static_cast<unsigned>(juxi::kMaxRateHz));
      } else {
        const Status s = serial_->write(ByteSpan(cmd, n));
        if (!s.ok()) {
          set_state(DeviceState::kFault, s.error());
          return s;
        }
        SCAN_LOG_INFO(kMod, "device %u: sent report-rate command (%u Hz)", id_,
                      static_cast<unsigned>(cfg_.report_rate_hz));
      }
    } else {
      // The module free-runs at whatever rate it last persisted, so this is a
      // normal configuration (a read-only probe, a replay, an app that
      // configured the module itself) — not a fault.
      SCAN_LOG_INFO(kMod,
                    "device %u: no write function; assuming the module's persisted rate (%u Hz)",
                    id_, static_cast<unsigned>(cfg_.report_rate_hz));
    }
  }

  if (cfg_.internal_supervisor_thread) {
    supervisor_run_.store(true, std::memory_order_release);
    supervisor_ = std::thread(&ImuSerialDriver::supervisor_loop, this);
  }
  return kOkStatus;
}

Status ImuSerialDriver::stop() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!started_ && !supervisor_.joinable()) return kOkStatus;
  }
  set_state(DeviceState::kStopping, ScanError::kOk);

  if (supervisor_.joinable()) {
    supervisor_run_.store(false, std::memory_order_release);
    supervisor_cv_.notify_all();
    supervisor_.join();
  }
  (void)serial_->stop();
  {
    std::lock_guard<std::mutex> lock(m_);
    started_ = false;
    degraded_blackout_ = false;
    degraded_checksum_ = false;
  }
  set_state(DeviceState::kIdle, ScanError::kOk);
  return kOkStatus;
}

DeviceState ImuSerialDriver::state() const {
  std::lock_guard<std::mutex> lock(m_);
  return state_;
}

Status ImuSerialDriver::push_bytes(ByteSpan bytes, TimePoint t_arrival) {
  return serial_->push(bytes, t_arrival);
}

void ImuSerialDriver::on_bytes(ByteSpan bytes, TimePoint t) {
  {
    std::lock_guard<std::mutex> lock(m_);
    t_last_bytes_ns_ = t.nanos;
  }

  // Every frame decoded out of one chunk shares that chunk's arrival stamp:
  // that IS the decode burst, and un-bursting it is ImuStamper's whole job.
  parser_.push(bytes.data(), bytes.size(),
               [this, t](const juxi::RawImuFrame& f) { on_raw_frame(f, t.nanos); });

  const juxi::FrameStats fs = parser_.stats();
  {
    std::lock_guard<std::mutex> lock(m_);
    frames_ = fs;
    const std::uint64_t seen = fs.frames_seen();
    degraded_checksum_ =
        seen >= cfg_.health_min_frames && fs.checksum_pass_rate() < cfg_.min_checksum_pass_rate;
  }
  refresh_state();
}

void ImuSerialDriver::on_raw_frame(const juxi::RawImuFrame& f, std::int64_t t_arrival_ns) {
  ImuSerialSample s{};
  s.t_arrival_ns = t_arrival_ns;
  s.t_stamped_ns = stamper_.stamp(t_arrival_ns);
  for (int i = 0; i < 3; ++i) {
    s.gyro_rad_s[i] = f.gyro_rad_s[i];
    s.accel_g[i] = f.accel_g[i];
  }

  const std::int64_t blackout_ns = seconds_to_ns(cfg_.blackout_threshold_s);
  std::int64_t gap = 0;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (saw_first_sample_) {
      gap = t_arrival_ns - t_last_sample_ns_;
      if (gap > blackout_ns) {
        // The watchdog may already have counted this one (it fires as soon as
        // the silence passes the threshold, so that a module that never comes
        // back is still visible); either way the RESUME is where the true
        // duration becomes known.
        if (!degraded_blackout_) ++blackouts_;
        if (gap > worst_blackout_ns_) worst_blackout_ns_ = gap;
        SCAN_LOG_WARN(kMod, "device %u: blackout of %.3f s ended (%llu total)", id_,
                      static_cast<double>(gap) / 1e9,
                      static_cast<unsigned long long>(blackouts_));
      }
    }
    saw_first_sample_ = true;
    degraded_blackout_ = false;
    t_last_sample_ns_ = t_arrival_ns;
    ++samples_;
    ++window_samples_;
  }
  {
    std::lock_guard<std::mutex> lock(ring_m_);
    if (!ring_.empty()) {
      const std::size_t cap = ring_.size();
      ring_[(ring_head_ + ring_size_) % cap] = s;
      if (ring_size_ == cap) {
        ring_head_ = (ring_head_ + 1) % cap;  // overwrite the oldest
        std::lock_guard<std::mutex> lock2(m_);
        ++samples_dropped_;
      } else {
        ++ring_size_;
      }
    }
  }

  // No driver lock held: the sink runs app code (the Engine's ImuIngest shim).
  if (cfg_.sink != nullptr) cfg_.sink(&s, 1, cfg_.sink_user_data);
}

std::size_t ImuSerialDriver::drain(ImuSerialSample* out, std::size_t max) {
  if (out == nullptr || max == 0) return 0;
  std::lock_guard<std::mutex> lock(ring_m_);
  const std::size_t cap = ring_.size();
  if (cap == 0) return 0;
  const std::size_t n = std::min(max, ring_size_);
  for (std::size_t i = 0; i < n; ++i) out[i] = ring_[(ring_head_ + i) % cap];
  ring_head_ = (ring_head_ + n) % cap;
  ring_size_ -= n;
  return n;
}

void ImuSerialDriver::supervisor_loop() {
  while (supervisor_run_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> lock(supervisor_m_);
      supervisor_cv_.wait_for(lock, std::chrono::milliseconds(kSupervisorTickMs),
                              [this] { return !supervisor_run_.load(std::memory_order_acquire); });
    }
    if (!supervisor_run_.load(std::memory_order_acquire)) break;
    tick(current_time());
  }
}

void ImuSerialDriver::tick(TimePoint now) {
  const std::int64_t t = now.nanos;
  const std::int64_t blackout_ns = seconds_to_ns(cfg_.blackout_threshold_s);
  const std::int64_t health_ns = static_cast<std::int64_t>(cfg_.health_period_ms) * 1000000LL;

  bool publish = false;
  ImuSerialStats snapshot;
  bool entered_blackout = false;
  double blackout_s = 0.0;

  {
    std::lock_guard<std::mutex> lock(m_);
    if (!started_) return;

    // Blackout watchdog. Only meaningful once the stream has actually started:
    // before the first sample the relevant question is "did it ever start",
    // which is the kStarting state itself, not a blackout.
    if (saw_first_sample_) {
      const std::int64_t silence = t - t_last_sample_ns_;
      if (silence > blackout_ns) {
        if (!degraded_blackout_) {
          degraded_blackout_ = true;
          ++blackouts_;
          entered_blackout = true;
          blackout_s = static_cast<double>(silence) / 1e9;
        }
        if (silence > worst_blackout_ns_) worst_blackout_ns_ = silence;
      }
    }

    if (health_ns > 0 && t - window_start_ns_ >= health_ns) {
      const double elapsed_s = static_cast<double>(t - window_start_ns_) / 1e9;
      rate_hz_ = elapsed_s > 0.0 ? static_cast<double>(window_samples_) / elapsed_s : 0.0;
      window_start_ns_ = t;
      window_samples_ = 0;
      publish = true;

      snapshot.state = state_;
      snapshot.frames = frames_;
      snapshot.samples = samples_;
      snapshot.samples_dropped = samples_dropped_;
      snapshot.rate_hz = rate_hz_;
      snapshot.blackouts = blackouts_;
      snapshot.worst_blackout_ns = worst_blackout_ns_;
      snapshot.blackout_in_progress = degraded_blackout_;
      snapshot.t_last_sample_ns = t_last_sample_ns_;
      snapshot.t_last_bytes_ns = t_last_bytes_ns_;
    }
  }

  if (entered_blackout) {
    SCAN_LOG_WARN(kMod, "device %u: no IMU frame for %.3f s — module blackout", id_, blackout_s);
  }
  refresh_state();
  if (publish) {
    snapshot.stamper = stamper_.stats();
    snapshot.state = state();
    publish_health(snapshot, t);
  }
}

void ImuSerialDriver::refresh_state() {
  DeviceState next = DeviceState::kIdle;
  ScanError err = ScanError::kOk;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!started_) return;
    if (state_ == DeviceState::kStopping || state_ == DeviceState::kFault) return;
    if (!saw_first_sample_) {
      next = DeviceState::kStarting;
    } else if (degraded_blackout_) {
      next = DeviceState::kDegraded;
      err = ScanError::kDeviceNotResponding;
    } else if (degraded_checksum_) {
      next = DeviceState::kDegraded;
      err = ScanError::kChecksumFailed;
    } else {
      next = DeviceState::kStreaming;
    }
  }
  set_state(next, err);
}

void ImuSerialDriver::set_state(DeviceState next, ScanError err) {
  DeviceState prev;
  std::int64_t t_ns;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == next && last_error_ == err) return;
    prev = state_;
    state_ = next;
    last_error_ = err;
    t_ns = t_last_bytes_ns_ != 0 ? t_last_bytes_ns_ : t_start_ns_;
  }
  SCAN_LOG_INFO(kMod, "device %u: %s -> %s%s%s", id_, to_string(prev), to_string(next),
                err == ScanError::kOk ? "" : " ", err == ScanError::kOk ? "" : error_str(err));
  if (ctx_.bus != nullptr) {
    DeviceStatePayload p{};
    p.device = id_;
    p.kind = DeviceKind::kImuSerial;
    p.state = next;
    p.previous = prev;
    p.error = err;
    ctx_.bus->publish(EventType::kDeviceState, p, t_ns);
  }
}

void ImuSerialDriver::publish_health(const ImuSerialStats& s, std::int64_t t_ns) {
  SCAN_LOG_DEBUG(kMod,
                 "device %u: %.1f Hz, %llu samples, checksum %.4f, %llu blackouts "
                 "(worst %.3f s), learned period %.3f ms",
                 id_, s.rate_hz, static_cast<unsigned long long>(s.samples),
                 s.frames.checksum_pass_rate(), static_cast<unsigned long long>(s.blackouts),
                 static_cast<double>(s.worst_blackout_ns) / 1e9,
                 static_cast<double>(s.stamper.learned_period_ns) / 1e6);
  if (ctx_.bus == nullptr) return;
  DeviceHealthPayload p{};
  p.device = id_;
  p.state = s.state;
  // Not geometry: this device never produces points, and saying otherwise
  // would put a fake number on the app's one health widget.
  p.points_out = 0;
  p.points_per_sec = 0.0;
  p.checksum_pass_rate = s.frames.checksum_pass_rate();
  ctx_.bus->publish(EventType::kDeviceHealth, p, t_ns);
}

ImuSerialStats ImuSerialDriver::stats() const {
  ImuSerialStats s;
  {
    std::lock_guard<std::mutex> lock(m_);
    s.state = state_;
    s.frames = frames_;
    s.samples = samples_;
    s.samples_dropped = samples_dropped_;
    s.rate_hz = rate_hz_;
    s.blackouts = blackouts_;
    s.worst_blackout_ns = worst_blackout_ns_;
    s.blackout_in_progress = degraded_blackout_;
    s.t_last_sample_ns = t_last_sample_ns_;
    s.t_last_bytes_ns = t_last_bytes_ns_;
  }
  s.stamper = stamper_.stats();
  return s;
}

DeviceHealth ImuSerialDriver::health() const {
  DeviceHealth h{};
  h.id = id_;
  h.kind = DeviceKind::kImuSerial;
  {
    std::lock_guard<std::mutex> lock(m_);
    h.state = state_;
    h.last_error = last_error_;
    h.bytes_in = frames_.bytes_in;
    h.packets_ok = frames_.raw_frames;
    h.packets_bad = frames_.checksum_failures;
    h.checksum_pass_rate = frames_.checksum_pass_rate();
    // The IMU produces no geometry; `rotation_hz` is the health panel's one
    // "is this sensor keeping up" dial, so the measured SAMPLE rate goes
    // there — the same slot the D6 puts revolutions in and the Mid-360 puts
    // its IMU rate in.
    h.rotation_hz = rate_hz_;
    h.drops = samples_dropped_;
  }
  h.points_out = 0;
  h.points_per_sec = 0.0;
  h.t_last_data_ns = serial_->stats().t_last_rx_ns;
  return h;
}

}  // namespace scanengine
