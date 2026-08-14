#include "scanengine/drivers/mid360/mid360_driver.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "mid360_backend.h"
#include "scanengine/core/log.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "mid360";

// The supervisor wakes this often. It must be well under
// reconnect.data_timeout_ms so the watchdog fires promptly, and well under
// health_period_ms so per-second windows do not smear.
constexpr int kSupervisorTickMs = 50;

std::uint32_t decimation_stride(std::uint32_t live_pps) {
  if (live_pps == 0) return 1;  // no decimation: post-processing / replay
  const double stride = mid360::kNominalPointsPerSec / static_cast<double>(live_pps);
  if (stride <= 1.0) return 1;
  return static_cast<std::uint32_t>(stride + 0.5);
}

// Reflectivity → greyscale, matching the D6 driver's convention so the live
// view looks like one cloud when both sensors run. A14 owns real colour
// modes; this is the placeholder that keeps intensity visible until then.
inline void shade(PointVertex& v, std::uint8_t reflectivity) {
  v.r = reflectivity;
  v.g = reflectivity;
  v.b = reflectivity;
  v.a = 255;
}

}  // namespace

const char* to_string(Mid360Backend b) noexcept {
  switch (b) {
    case Mid360Backend::kSdk2: return "sdk2";
    case Mid360Backend::kRawUdp: return "raw-udp";
    case Mid360Backend::kInject: return "inject";
  }
  return "unknown";
}

const char* to_string(Mid360LinkState s) noexcept {
  switch (s) {
    case Mid360LinkState::kDown: return "down";
    case Mid360LinkState::kWaiting: return "waiting";
    case Mid360LinkState::kUp: return "up";
    case Mid360LinkState::kSilent: return "silent";
    case Mid360LinkState::kReinitializing: return "reinitializing";
  }
  return "unknown";
}

Mid360Driver::Mid360Driver(DeviceId id, const Mid360Config& cfg, const DriverContext& ctx)
    : id_(id), cfg_(cfg), ctx_(ctx) {
  batch_.reserve(cfg_.max_batch_points == 0 ? 1 : cfg_.max_batch_points);
  imu_ring_.resize(cfg_.imu_ring_capacity == 0 ? 1 : cfg_.imu_ring_capacity);
  decimate_stride_ = decimation_stride(cfg_.live_points_per_sec);
  state_ = DeviceState::kIdle;
  st_.state = state_;
}

Mid360Driver::~Mid360Driver() {
  (void)stop();
}

// --- lifecycle ------------------------------------------------------------

Status Mid360Driver::start() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == DeviceState::kStreaming || state_ == DeviceState::kStarting ||
        state_ == DeviceState::kDegraded) {
      return kOkStatus;
    }
  }

  // Explicit lidar IP is the default path on EVERY platform, and a hard
  // requirement on macOS: the patched SDK cannot bind the broadcast address
  // there, so an unknown-IP device is simply unreachable (S2 REPORT.md §3).
  // Failing here, by name, beats a silent 30-second wait for a discovery
  // that will never answer.
  if (cfg_.backend != Mid360Backend::kInject) {
    if (cfg_.udp.lidar_ip.empty()) {
      set_state(DeviceState::kFault, ScanError::kInvalidArgument);
      return set_last_error(ScanError::kInvalidArgument,
                            "mid360 device %u: udp.lidar_ip is required (there is no broadcast "
                            "discovery on macOS, and explicit addressing is the default "
                            "everywhere — set it from the connect wizard)",
                            id_);
    }
    if (cfg_.udp.host_ip.empty()) {
      set_state(DeviceState::kFault, ScanError::kInvalidArgument);
      return set_last_error(ScanError::kInvalidArgument,
                            "mid360 device %u: udp.host_ip is required (the device is TOLD "
                            "where to stream; it does not discover us)",
                            id_);
    }
  }

  switch (cfg_.backend) {
    case Mid360Backend::kSdk2: backend_ = make_sdk2_backend(*this, id_, cfg_); break;
    case Mid360Backend::kRawUdp: backend_ = make_raw_udp_backend(*this, id_, cfg_); break;
    case Mid360Backend::kInject: backend_ = make_inject_backend(id_); break;
  }
  if (backend_ == nullptr) {
    const ScanError e = last_error_code();
    set_state(DeviceState::kFault, e);
    return e;  // the factory already set a detailed message
  }

  const std::int64_t now = ctx_.clock().nanos;
  {
    std::lock_guard<std::mutex> lock(m_);
    loss_.reset();
    filter_stats_ = mid360::FilterStats{};
    batch_.clear();
    decimate_phase_ = 0;
    st_ = Mid360Stats{};
    window_ = Window{};
    window_.t_start_ns = now;
    t_last_point_ns_ = 0;
    t_last_imu_ns_ = 0;
    t_last_heartbeat_ns_ = 0;
    t_silent_since_ns_ = now;
    t_next_reinit_ns_ = 0;
    reinit_backoff_ms_ = cfg_.reconnect.reinit_backoff_initial_ms;
  }

  const Status s = open_backend();
  if (!s.ok()) {
    backend_.reset();
    set_state(DeviceState::kFault, s.error());
    return s;
  }

  set_state(DeviceState::kStarting, ScanError::kOk);
  set_link(Mid360LinkState::kWaiting, now);

  if (cfg_.internal_supervisor_thread) {
    supervisor_run_.store(true, std::memory_order_release);
    supervisor_ = std::thread(&Mid360Driver::supervisor_loop, this);
  }

  SCAN_LOG_INFO(kMod,
                "device %u: started, backend=%s lidar=%s host=%s point:%u imu:%u "
                "(decimation 1/%u, filter: no-return=%d tag-mask=0x%02X)",
                id_, to_string(cfg_.backend), cfg_.udp.lidar_ip.c_str(),
                cfg_.udp.host_ip.c_str(), static_cast<unsigned>(cfg_.udp.host_point_port),
                static_cast<unsigned>(cfg_.udp.host_imu_port), decimate_stride_,
                cfg_.filter.drop_no_return ? 1 : 0,
                static_cast<unsigned>(cfg_.filter.tag_reject_mask));
  return kOkStatus;
}

Status Mid360Driver::stop() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == DeviceState::kDisconnected || state_ == DeviceState::kIdle) {
      if (backend_ == nullptr && !supervisor_.joinable()) return kOkStatus;
    }
  }
  set_state(DeviceState::kStopping, ScanError::kOk);

  // Stop the supervisor BEFORE the backend: it is the only thing that can
  // decide to re-open a backend we are trying to close.
  if (supervisor_.joinable()) {
    supervisor_run_.store(false, std::memory_order_release);
    supervisor_cv_.notify_all();
    supervisor_.join();
  }

  close_backend();
  backend_.reset();

  const std::int64_t now = ctx_.clock().nanos;
  flush_points(now);  // whatever was mid-batch still belongs to the capture
  set_link(Mid360LinkState::kDown, now);
  set_state(DeviceState::kIdle, ScanError::kOk);
  return kOkStatus;
}

Status Mid360Driver::open_backend() {
  if (backend_ == nullptr) {
    return set_last_error(ScanError::kInvalidState, "mid360 device %u: no backend", id_);
  }
  return backend_->open();
}

void Mid360Driver::close_backend() {
  if (backend_ != nullptr) backend_->close();
}

DeviceState Mid360Driver::state() const {
  std::lock_guard<std::mutex> lock(m_);
  return state_;
}

Mid360LinkState Mid360Driver::link_state() const {
  std::lock_guard<std::mutex> lock(m_);
  return link_;
}

Status Mid360Driver::push_bytes(ByteSpan bytes, TimePoint t_arrival) {
  // In the normal case the Mid-360 owns its sockets (SDK2, or UdpSource in
  // raw mode) and bytes are never pushed from the app the way D6 serial
  // bytes are. Mid360Backend::kInject is the exception: there, ONE call is
  // ONE complete UDP datagram (never a partial read — datagram boundaries
  // are meaningful and are never reassembled), which is what makes a
  // recorded .lscan Mid-360 chunk replayable through the real driver.
  if (cfg_.backend != Mid360Backend::kInject) {
    return set_last_error(ScanError::kNotSupported,
                          "Mid-360 is a self-driven UDP source; push_bytes() does not apply "
                          "(replay through Mid360Backend::kInject instead)");
  }
  const mid360::PacketView v = mid360::parse_packet(bytes.data(), bytes.size());
  if (!v.valid()) {
    std::lock_guard<std::mutex> lock(m_);
    ++st_.bad_packets;
    return set_last_error(ScanError::kProtocolError,
                          "mid360 device %u: pushed %zu bytes are not a Mid-360 datagram", id_,
                          bytes.size());
  }
  if (v.header->data_type == mid360::kDataTypeImu) {
    on_imu_packet(bytes.data(), bytes.size(), t_arrival);
  } else {
    on_point_packet(bytes.data(), bytes.size(), t_arrival);
  }
  return kOkStatus;
}

// --- point ingest ---------------------------------------------------------

void Mid360Driver::on_point_packet(const std::uint8_t* data, std::size_t len,
                                   TimePoint t_arrival) {
  const mid360::PacketView v = mid360::parse_packet(data, len);
  const std::int64_t t_ns = t_arrival.nanos;

  if (!v.valid() || (cfg_.verify_crc && !mid360::crc32_ok(v))) {
    std::lock_guard<std::mutex> lock(m_);
    ++st_.bad_packets;
    return;
  }
  if (v.header->data_type == mid360::kDataTypeImu) {
    // Some paths (a single raw socket carrying both streams) deliver IMU
    // here; route it rather than mis-decoding it as geometry.
    on_imu_packet(data, len, t_arrival);
    return;
  }

  bool need_flush = false;
  {
    std::lock_guard<std::mutex> lock(m_);

    ++st_.point_packets;
    ++window_.packets;
    t_last_point_ns_ = t_ns;
    st_.t_last_point_ns = t_ns;

    // Loss accounting on the FREE-RUNNING udp_cnt. See mid360_packets.h for
    // why the documented per-frame-reset model is wrong, and for why this
    // cannot see a full-link outage (that is the watchdog's job).
    std::uint32_t lost = 0;
    (void)loss_.observe(v.header->udp_cnt, &lost);
    window_.lost += lost;
    st_.packets_lost = loss_.lost();
    st_.packets_duplicated = loss_.duplicates();
    st_.counter_resets = loss_.resets();

    const std::uint32_t n = v.point_count;
    st_.points_received += n;
    window_.points += n;

    if (v.header->data_type == mid360::kDataTypeCartesianHigh) {
      const auto* pts = reinterpret_cast<const mid360::CartesianHigh*>(v.payload);
      for (std::uint32_t i = 0; i < n; ++i) {
        mid360::CartesianHigh p;
        std::memcpy(&p, &pts[i], sizeof(p));  // datagram alignment is not ours to assume
        if (!mid360::point_passes(p, cfg_.filter, &filter_stats_)) continue;
        ++st_.points_kept;

        // Deterministic decimation AFTER filtering: a replay of the same
        // bytes produces the same cloud, which is what E2's golden datasets
        // need. Random sampling would not.
        // Explicit modulo on the counter itself, not on a free-running
        // one: a uint32 wrap mid-capture would otherwise silently shift the
        // phase and break replay determinism.
        if (decimate_stride_ > 1) {
          const bool keep = decimate_phase_ == 0;
          if (++decimate_phase_ >= decimate_stride_) decimate_phase_ = 0;
          if (!keep) continue;
        }

        PointVertex vert{};
        // mm → metres. The device reports in its own frame; A8 applies the
        // trajectory, A6 the extrinsic. The driver does no geometry.
        vert.x = static_cast<float>(p.x) * 0.001f;
        vert.y = static_cast<float>(p.y) * 0.001f;
        vert.z = static_cast<float>(p.z) * 0.001f;
        shade(vert, p.reflectivity);
        batch_.push_back(vert);
      }
    } else if (v.header->data_type == mid360::kDataTypeCartesianLow) {
      const auto* pts = reinterpret_cast<const mid360::CartesianLow*>(v.payload);
      for (std::uint32_t i = 0; i < n; ++i) {
        mid360::CartesianLow lo;
        std::memcpy(&lo, &pts[i], sizeof(lo));
        mid360::CartesianHigh p{};  // widen to mm so one filter serves both
        p.x = static_cast<std::int32_t>(lo.x) * 10;
        p.y = static_cast<std::int32_t>(lo.y) * 10;
        p.z = static_cast<std::int32_t>(lo.z) * 10;
        p.reflectivity = lo.reflectivity;
        p.tag = lo.tag;
        if (!mid360::point_passes(p, cfg_.filter, &filter_stats_)) continue;
        ++st_.points_kept;
        if (decimate_stride_ > 1) {
          const bool keep = decimate_phase_ == 0;
          if (++decimate_phase_ >= decimate_stride_) decimate_phase_ = 0;
          if (!keep) continue;
        }
        PointVertex vert{};
        vert.x = static_cast<float>(p.x) * 0.001f;
        vert.y = static_cast<float>(p.y) * 0.001f;
        vert.z = static_cast<float>(p.z) * 0.001f;
        shade(vert, p.reflectivity);
        batch_.push_back(vert);
      }
    }

    st_.filter = filter_stats_;
    need_flush = batch_.size() >= cfg_.max_batch_points;
  }

  if (need_flush) flush_points(t_ns);

  // First data promotes us out of kStarting; the watchdog in tick() handles
  // the end of a silent-link episode, so there is nothing to do here.
  if (state() == DeviceState::kStarting) set_state(DeviceState::kStreaming, ScanError::kOk);
}

void Mid360Driver::on_imu_packet(const std::uint8_t* data, std::size_t len,
                                 TimePoint t_arrival) {
  if (!cfg_.publish_imu) return;
  const mid360::PacketView v = mid360::parse_packet(data, len);
  if (!v.valid() || v.header->data_type != mid360::kDataTypeImu) {
    std::lock_guard<std::mutex> lock(m_);
    ++st_.bad_packets;
    return;
  }
  if (v.payload_bytes < sizeof(mid360::ImuRaw)) return;

  mid360::ImuRaw raw{};
  std::memcpy(&raw, v.payload, sizeof(raw));

  Mid360ImuSample s{};
  s.t_mono_ns = t_arrival.nanos;
  std::memcpy(&s.t_device_ns, &v.header->timestamp, sizeof(s.t_device_ns));
  s.gyro[0] = raw.gyro_x;
  s.gyro[1] = raw.gyro_y;
  s.gyro[2] = raw.gyro_z;
  s.acc[0] = raw.acc_x;
  s.acc[1] = raw.acc_y;
  s.acc[2] = raw.acc_z;

  {
    std::lock_guard<std::mutex> lock(m_);
    ++st_.imu_packets;
    ++window_.imu;
    t_last_imu_ns_ = s.t_mono_ns;
    st_.t_last_imu_ns = s.t_mono_ns;
  }

  // IMU NEVER ENTERS THE PageStore. It is not geometry, it does not belong in
  // a render buffer, and A6's ESKF wants it at full 200 Hz rate with its own
  // timestamps — so it goes to a bounded ring (pull) plus an optional sink
  // (push), both tagged StreamId::kImu by convention.
  {
    std::lock_guard<std::mutex> lock(imu_m_);
    if (!imu_ring_.empty()) {
      const std::size_t cap = imu_ring_.size();
      const std::size_t slot = (imu_head_ + imu_size_) % cap;
      imu_ring_[slot] = s;
      if (imu_size_ == cap) {
        imu_head_ = (imu_head_ + 1) % cap;  // overwrite the oldest
        std::lock_guard<std::mutex> lock2(m_);
        ++st_.imu_dropped;
      } else {
        ++imu_size_;
      }
    }
  }
  if (cfg_.imu_sink != nullptr) cfg_.imu_sink(&s, 1, cfg_.imu_sink_user_data);
}

std::size_t Mid360Driver::drain_imu(Mid360ImuSample* out, std::size_t max) {
  if (out == nullptr || max == 0) return 0;
  std::lock_guard<std::mutex> lock(imu_m_);
  const std::size_t cap = imu_ring_.size();
  const std::size_t n = std::min(max, imu_size_);
  for (std::size_t i = 0; i < n; ++i) out[i] = imu_ring_[(imu_head_ + i) % cap];
  imu_head_ = (imu_head_ + n) % cap;
  imu_size_ -= n;
  return n;
}

void Mid360Driver::on_device_connected(const char* sn, const char* ip) {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (sn != nullptr) st_.device_sn = sn;
    if (ip != nullptr) st_.device_ip = ip;
  }
  SCAN_LOG_INFO(kMod, "device %u: connected (sn=%s ip=%s)", id_, sn ? sn : "?", ip ? ip : "?");
}

void Mid360Driver::on_heartbeat(TimePoint t) {
  std::lock_guard<std::mutex> lock(m_);
  t_last_heartbeat_ns_ = t.nanos;
  st_.t_last_heartbeat_ns = t.nanos;
}

void Mid360Driver::flush_points(std::int64_t t_ns) {
  std::lock_guard<std::mutex> flush_lock(flush_m_);
  {
    std::lock_guard<std::mutex> lock(m_);
    if (batch_.empty()) return;
    flush_buf_.swap(batch_);  // flush_buf_ was cleared last time: capacity survives
    batch_.clear();
  }
  if (ctx_.points == nullptr) {
    flush_buf_.clear();
    return;
  }

  std::uint32_t appended = 0;
  const Status s = ctx_.points->append(
      StreamId::kLidarMid360, Span<const PointVertex>(flush_buf_.data(), flush_buf_.size()), t_ns,
      &appended);

  {
    std::lock_guard<std::mutex> lock(m_);
    st_.points_appended += appended;
    window_.points_appended += appended;
    if (!s.ok()) st_.points_dropped_store += flush_buf_.size() - appended;
  }
  if (!s.ok() && ctx_.bus != nullptr) {
    // Backpressure is not silent: A14 replaces max_pages with an eviction
    // policy, and until then the app must be able to say "the store is full"
    // rather than quietly rendering a truncated cloud.
    ErrorPayload e{};
    e.error = s.error();
    e.device = id_;
    e.stream = StreamId::kLidarMid360;
    ctx_.bus->publish(EventType::kError, e, t_ns);
  }
  flush_buf_.clear();
}

// --- supervisor: watchdog, reconnect, health ------------------------------

void Mid360Driver::supervisor_loop() {
  while (supervisor_run_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> lock(supervisor_m_);
      supervisor_cv_.wait_for(lock, std::chrono::milliseconds(kSupervisorTickMs),
                              [this] { return !supervisor_run_.load(std::memory_order_acquire); });
    }
    if (!supervisor_run_.load(std::memory_order_acquire)) break;
    tick(ctx_.clock());
  }
}

void Mid360Driver::tick(TimePoint now) {
  const std::int64_t t = now.nanos;
  const std::int64_t data_timeout_ns =
      static_cast<std::int64_t>(cfg_.reconnect.data_timeout_ms) * 1000000LL;
  const std::int64_t reinit_after_ns =
      static_cast<std::int64_t>(cfg_.reconnect.reinit_after_silence_ms) * 1000000LL;

  bool want_reinit = false;
  bool give_up = false;
  bool publish = false;
  bool stale_batch = false;
  Mid360Stats snapshot;

  {
    std::lock_guard<std::mutex> lock(m_);

    // A partial batch that has gone stale should still reach the renderer
    // rather than sit in the buffer until the stream picks up again.
    stale_batch = !batch_.empty() && (t - window_.t_start_ns) > data_timeout_ns;

    const bool ever_saw_data = t_last_point_ns_ != 0;
    const std::int64_t t_ref = ever_saw_data ? t_last_point_ns_ : t_silent_since_ns_;
    const std::int64_t silence_ns = t - t_ref;
    // Before the first packet the clock we are running against is the
    // handshake, not the stream, so the generous connect timeout applies.
    const std::int64_t timeout_ns =
        ever_saw_data ? data_timeout_ns
                      : static_cast<std::int64_t>(cfg_.reconnect.connect_timeout_ms) * 1000000LL;

    switch (link_) {
      case Mid360LinkState::kDown:
        break;

      case Mid360LinkState::kWaiting:
      case Mid360LinkState::kUp:
        if (ever_saw_data && silence_ns <= timeout_ns) {
          if (link_ != Mid360LinkState::kUp) {
            link_ = Mid360LinkState::kUp;
            t_silent_since_ns_ = 0;
            // A resume with no re-init is the cable-pull case S2 measured:
            // the SDK recovers a live link entirely on its own.
            if (st_.watchdog_trips > 0) {
              ++st_.clean_resumes;
              SCAN_LOG_INFO(kMod,
                            "device %u: link resumed with no re-init after %.2f s "
                            "(clean resume #%llu — cable-class fault; note udp_cnt counted "
                            "%llu lost, which is expected: the counter is blind to a full "
                            "outage)",
                            id_, static_cast<double>(silence_ns) * 1e-9,
                            static_cast<unsigned long long>(st_.clean_resumes),
                            static_cast<unsigned long long>(st_.packets_lost));
            }
          }
        } else if (cfg_.reconnect.enabled && silence_ns > timeout_ns) {
          link_ = Mid360LinkState::kSilent;
          t_silent_since_ns_ = t_ref;
          ++st_.watchdog_trips;
          t_next_reinit_ns_ = t_ref + reinit_after_ns;
          SCAN_LOG_WARN(kMod,
                        "device %u: DATA WATCHDOG tripped — no point packet for %.2f s "
                        "(trip #%llu). udp_cnt cannot see this; forcing a full SDK re-init "
                        "in %.1f s if it stays silent.",
                        id_, static_cast<double>(silence_ns) * 1e-9,
                        static_cast<unsigned long long>(st_.watchdog_trips),
                        static_cast<double>(cfg_.reconnect.reinit_after_silence_ms) / 1000.0);
        }
        break;

      case Mid360LinkState::kSilent:
      case Mid360LinkState::kReinitializing:
        if (ever_saw_data && silence_ns <= timeout_ns) {
          const bool after_reinit = (link_ == Mid360LinkState::kReinitializing);
          link_ = Mid360LinkState::kUp;
          t_silent_since_ns_ = 0;
          reinit_backoff_ms_ = cfg_.reconnect.reinit_backoff_initial_ms;
          if (!after_reinit) ++st_.clean_resumes;
          SCAN_LOG_INFO(kMod, "device %u: data flowing again (%s)", id_,
                        after_reinit ? "after a forced SDK re-init — power-cycle class"
                                     : "clean resume, no re-init — cable class");
        } else if (cfg_.reconnect.enabled && t >= t_next_reinit_ns_) {
          // Give up rather than churn sockets forever when a limit is set.
          if (cfg_.reconnect.max_reinits != 0 &&
              st_.forced_reinits >= cfg_.reconnect.max_reinits) {
            give_up = true;
          } else {
            want_reinit = true;
          }
        }
        break;
    }

    // Per-window health.
    const std::int64_t window_ns =
        static_cast<std::int64_t>(cfg_.health_period_ms) * 1000000LL;
    if (window_ns > 0 && t - window_.t_start_ns >= window_ns) {
      const double dt = static_cast<double>(t - window_.t_start_ns) * 1e-9;
      if (dt > 0.0) {
        st_.points_per_sec = static_cast<double>(window_.points) / dt;
        st_.points_appended_per_sec = static_cast<double>(window_.points_appended) / dt;
        st_.imu_hz = static_cast<double>(window_.imu) / dt;
        const std::uint64_t win_total = window_.packets + window_.lost;
        st_.loss_pct_window =
            win_total == 0 ? 0.0
                           : 100.0 * static_cast<double>(window_.lost) /
                                 static_cast<double>(win_total);
      }
      st_.loss_pct_total = 100.0 * loss_.loss_fraction();
      window_ = Window{};
      window_.t_start_ns = t;
      publish = true;
    }

    st_.link = link_;
    st_.state = state_;
    st_.t_silent_since_ns = t_silent_since_ns_;
    snapshot = st_;
  }

  if (stale_batch) flush_points(t);

  if (give_up) {
    set_state(DeviceState::kFault, ScanError::kDeviceNotResponding);
    if (publish) publish_health(snapshot, t);
    return;
  }

  // State demotion/promotion, outside the lock (set_state publishes).
  const Mid360LinkState link = snapshot.link;
  const DeviceState cur = state();
  if (link == Mid360LinkState::kSilent || link == Mid360LinkState::kReinitializing) {
    if (cur == DeviceState::kStreaming || cur == DeviceState::kStarting) {
      set_state(DeviceState::kDegraded, ScanError::kDeviceNotResponding);
    }
  } else if (link == Mid360LinkState::kUp) {
    const bool lossy = snapshot.loss_pct_window > cfg_.max_loss_pct &&
                       snapshot.point_packets >= cfg_.loss_min_packets;
    if (lossy && cur == DeviceState::kStreaming) {
      set_state(DeviceState::kDegraded, ScanError::kNetworkError);
    } else if (!lossy && (cur == DeviceState::kDegraded || cur == DeviceState::kStarting)) {
      set_state(DeviceState::kStreaming, ScanError::kOk);
    }
  }

  if (publish) publish_health(snapshot, t);

  if (!want_reinit) return;

  // --- forced re-init ---------------------------------------------------
  //
  // THE LOAD-BEARING S2 FINDING. A power-cycled Mid-360 keeps sending
  // discovery ACKs, and the SDK keeps receiving them — and never
  // re-configures the device, because GeneralCommandHandler::
  // HandleDetectionData() returns early for any handle it has already seen
  // and only re-sends the host-IP configuration if the device was never
  // configured at all. S2 watched a device stay dead for the remaining 35 s
  // of a run after a simulated power-cycle. Waiting for a self-heal is
  // therefore not a strategy; tearing the SDK down and building it again is.
  {
    std::lock_guard<std::mutex> lock(m_);
    link_ = Mid360LinkState::kReinitializing;
    ++st_.forced_reinits;
    st_.link = link_;
  }
  SCAN_LOG_WARN(kMod,
                "device %u: FORCING full SDK re-init (attempt #%llu) — the SDK never "
                "re-configures a device it has already seen, so a power-cycled unit will "
                "not come back on its own",
                id_, static_cast<unsigned long long>(snapshot.forced_reinits + 1));

  close_backend();
  const Status s = open_backend();

  {
    std::lock_guard<std::mutex> lock(m_);
    // Restart the silence clock from now, so the next attempt is measured
    // from this attempt and the backoff is honoured.
    t_last_point_ns_ = 0;
    t_silent_since_ns_ = t;
    if (s.ok()) {
      reinit_backoff_ms_ = cfg_.reconnect.reinit_backoff_initial_ms;
    } else {
      ++st_.reinit_failures;
      reinit_backoff_ms_ =
          std::min(cfg_.reconnect.reinit_backoff_max_ms,
                   reinit_backoff_ms_ == 0 ? cfg_.reconnect.reinit_backoff_initial_ms
                                           : reinit_backoff_ms_ * 2);
    }
    // Even a successful re-init needs the reconnect window before the next
    // attempt: the device has to be given time to answer the handshake.
    const std::int64_t wait_ns =
        std::max<std::int64_t>(reinit_after_ns,
                               static_cast<std::int64_t>(reinit_backoff_ms_) * 1000000LL);
    t_next_reinit_ns_ = t + wait_ns;
  }

  if (!s.ok()) {
    SCAN_LOG_ERROR(kMod, "device %u: re-init failed (%s); retrying in %u ms", id_,
                   error_str(s.error()), reinit_backoff_ms_);
    if (cfg_.reconnect.max_reinits != 0 &&
        snapshot.forced_reinits + 1 >= cfg_.reconnect.max_reinits) {
      set_state(DeviceState::kFault, s.error());
    }
  }
}

void Mid360Driver::publish_health(const Mid360Stats& s, std::int64_t t_ns) {
  SCAN_LOG_DEBUG(kMod,
                 "device %u: %.0f pts/s in, %.0f pts/s to store, IMU %.1f Hz, loss %.4f%% "
                 "(window) / %.4f%% (total), link=%s",
                 id_, s.points_per_sec, s.points_appended_per_sec, s.imu_hz, s.loss_pct_window,
                 s.loss_pct_total, to_string(s.link));
  if (ctx_.bus == nullptr) return;
  DeviceHealthPayload p{};
  p.device = id_;
  p.state = s.state;
  p.points_out = s.points_appended;
  p.points_per_sec = s.points_per_sec;
  // No checksums on this transport; the equivalent quality signal is the
  // fraction of packets that arrived, so the app's one health widget means
  // the same thing for a D6 and a Mid-360.
  p.checksum_pass_rate = 1.0 - (s.loss_pct_window / 100.0);
  ctx_.bus->publish(EventType::kDeviceHealth, p, t_ns);
}

void Mid360Driver::set_link(Mid360LinkState next, std::int64_t t_ns) {
  std::lock_guard<std::mutex> lock(m_);
  if (link_ == next) return;
  link_ = next;
  st_.link = next;
  if (next != Mid360LinkState::kUp) t_silent_since_ns_ = t_ns;
}

void Mid360Driver::set_state(DeviceState next, ScanError err) {
  DeviceState prev;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == next && last_error_ == err) return;
    prev = state_;
    state_ = next;
    last_error_ = err;
    st_.state = next;
  }
  SCAN_LOG_INFO(kMod, "device %u: %s -> %s%s%s", id_, to_string(prev), to_string(next),
                err == ScanError::kOk ? "" : " ", err == ScanError::kOk ? "" : error_str(err));
  if (ctx_.bus != nullptr) {
    DeviceStatePayload p{};
    p.device = id_;
    p.kind = DeviceKind::kMid360;
    p.state = next;
    p.previous = prev;
    p.error = err;
    ctx_.bus->publish(EventType::kDeviceState, p, ctx_.clock().nanos);
  }
}

Mid360Stats Mid360Driver::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  Mid360Stats s = st_;
  s.state = state_;
  s.link = link_;
  s.filter = filter_stats_;
  s.packets_lost = loss_.lost();
  s.packets_duplicated = loss_.duplicates();
  s.counter_resets = loss_.resets();
  s.loss_pct_total = 100.0 * loss_.loss_fraction();
  return s;
}

DeviceHealth Mid360Driver::health() const {
  DeviceHealth h{};
  h.id = id_;
  h.kind = DeviceKind::kMid360;
  std::lock_guard<std::mutex> lock(m_);
  h.state = state_;
  h.last_error = last_error_;
  h.packets_ok = st_.point_packets;
  h.packets_bad = st_.bad_packets + loss_.lost();
  h.points_out = st_.points_appended;
  h.drops = st_.points_dropped_store;
  h.bytes_in = st_.point_packets * mid360::kPointPacketBytes;
  h.points_per_sec = st_.points_per_sec;
  // The Mid-360 has no revolutions; the closest analogue the health panel can
  // render on the same dial is the IMU rate, which is the other thing that
  // must not sag.
  h.rotation_hz = st_.imu_hz;
  h.checksum_pass_rate = 1.0 - loss_.loss_fraction();
  h.t_last_data_ns = st_.t_last_point_ns;
  return h;
}

}  // namespace scanengine
