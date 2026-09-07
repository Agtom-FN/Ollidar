// Mid70Driver — the Mid-360 driver's state machine, minus an IMU and minus a
// sequence counter.
//
// Read src/drivers/mid360/mid360_driver.cpp alongside this file: the
// watchdog / forced-re-init machine, the batching rule, the deterministic
// decimation and the health model are deliberately identical, because they
// were paid for on a bench once and the two devices fail the same ways. What
// is genuinely different is only this:
//
//   • No IMU path at all. A Mid-70 has no IMU; the pairing device is a
//     DeviceKind::kImuSerial. A kDataTypeImu datagram here means somebody
//     plugged a Horizon/Avia into a Mid-70 slot, so it is counted as
//     unexpected and dropped rather than routed anywhere.
//
//   • Loss comes from device-timestamp gaps (mid70::GapTracker), not from a
//     counter, because SDK v1 datagrams carry no counter. The tracker's
//     interval is re-derived from every datagram, since a firmware that
//     changes data_type mid-stream changes the packet cadence with it.
//
//   • The device stamp has to be DECODED before it can be used: in PPS+GPS
//     mode it is a packed UTC date, not nanoseconds. An undecodable stamp is
//     not fed to the tracker or the offset estimator — the packet is still
//     counted, and the driver says so in Mid70Stats.
#include "scanengine/drivers/mid70/mid70_driver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "mid70_backend.h"
#include "scanengine/core/log.h"
#include "scanengine/timesync/offset_estimator.h"  // TimeSync: driver.h only forward-declares it

namespace scanengine {
namespace {

constexpr const char* kMod = "mid70";

// The supervisor wakes this often. It must be well under
// reconnect.data_timeout_ms so the watchdog fires promptly, and well under
// health_period_ms so per-second windows do not smear.
constexpr int kSupervisorTickMs = 50;

std::uint32_t decimation_stride(std::uint32_t live_pps) {
  if (live_pps == 0) return 1;  // no decimation: post-processing / replay
  const double stride = mid70::kNominalPointsPerSec / static_cast<double>(live_pps);
  if (stride <= 1.0) return 1;
  return static_cast<std::uint32_t>(stride + 0.5);
}

// Reflectivity → greyscale, matching the D6 and Mid-360 drivers' convention
// so the live view looks like one cloud when several sensors run. A14 owns
// real colour modes; this is the placeholder that keeps intensity visible.
inline void shade(PointVertex& v, std::uint8_t reflectivity) {
  v.r = reflectivity;
  v.g = reflectivity;
  v.b = reflectivity;
  v.a = 255;
}

// How many RETURNS a datagram of this shape carries. For every single-return
// type that is the wire point count; a dual-return sample carries two.
inline std::uint32_t returns_in_packet(const mid70::PacketView& v) {
  return v.header->data_type == mid70::kDataTypeDualExtendCartesian ? v.point_count * 2u
                                                                    : v.point_count;
}

// The nominal spacing between consecutive datagrams of this shape — the
// GapTracker's whole notion of "one packet".
//
// It is derived per datagram rather than fixed at start(), because the only
// thing that makes the number right is the type the device is ACTUALLY
// sending: 100 cartesian points at 100k pts/s is 1.000 ms, 96 extended
// points is 0.960 ms, and 48 dual samples (96 returns at the 200k dual rate)
// is 0.480 ms. Getting this wrong does not fail loudly — it quietly reports
// a steady stream as 50% lost — so it is computed from the packet in hand.
std::int64_t packet_interval_ns(const mid70::PacketView& v) {
  const bool dual = v.header->data_type == mid70::kDataTypeDualExtendCartesian;
  const double rate = dual ? mid70::kNominalPointsPerSecDual : mid70::kNominalPointsPerSec;
  const double returns = static_cast<double>(returns_in_packet(v));
  const std::int64_t ns = static_cast<std::int64_t>(std::llround(returns / rate * 1e9));
  return ns > 0 ? ns : 1;
}

}  // namespace

const char* to_string(Mid70Backend b) noexcept {
  switch (b) {
    case Mid70Backend::kSdk1: return "sdk1";
    case Mid70Backend::kRawUdp: return "raw-udp";
    case Mid70Backend::kInject: return "inject";
  }
  return "unknown";
}

const char* to_string(Mid70LinkState s) noexcept {
  switch (s) {
    case Mid70LinkState::kDown: return "down";
    case Mid70LinkState::kWaiting: return "waiting";
    case Mid70LinkState::kUp: return "up";
    case Mid70LinkState::kSilent: return "silent";
    case Mid70LinkState::kReinitializing: return "reinitializing";
  }
  return "unknown";
}

Mid70Driver::Mid70Driver(DeviceId id, const Mid70Config& cfg, const DriverContext& ctx)
    : id_(id), cfg_(cfg), ctx_(ctx) {
  batch_.reserve(cfg_.max_batch_points == 0 ? 1 : cfg_.max_batch_points);
  decimate_stride_ = decimation_stride(cfg_.live_points_per_sec);
  state_ = DeviceState::kIdle;
  st_.state = state_;
}

Mid70Driver::~Mid70Driver() {
  (void)stop();
}

// --- lifecycle ------------------------------------------------------------

Status Mid70Driver::start() {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (state_ == DeviceState::kStreaming || state_ == DeviceState::kStarting ||
        state_ == DeviceState::kDegraded) {
      return kOkStatus;
    }
  }

  // UNLIKE THE MID-360, an explicit lidar IP is NOT required for the SDK
  // backend. SDK v1 discovery is a device BROADCAST that the host merely
  // listens to on UDP 55000 — nothing has to bind a broadcast address, which
  // is the exact operation that fails on macOS in SDK2 (S2 REPORT.md §3).
  // `broadcast_code` picks one device out of several; empty means "the first
  // Mid-70 that speaks up".
  //
  // The raw-UDP backend is the one that binds a host socket, and it cannot
  // guess which one.
  if (cfg_.backend == Mid70Backend::kRawUdp) {
    if (cfg_.udp.host_ip.empty()) {
      set_state(DeviceState::kFault, ScanError::kInvalidArgument);
      return set_last_error(ScanError::kInvalidArgument,
                            "mid70 device %u: udp.host_ip is required for the raw-UDP backend "
                            "(it only listens; something else must already have told the device "
                            "to stream here — use Mid70Backend::kSdk1 to bring a device up)",
                            id_);
    }
    if (cfg_.udp.bind_port == 0 && cfg_.udp.host_point_port == 0) {
      set_state(DeviceState::kFault, ScanError::kInvalidArgument);
      return set_last_error(ScanError::kInvalidArgument,
                            "mid70 device %u: udp.bind_port (or udp.host_point_port) is required "
                            "for the raw-UDP backend — set it to the port the device was told "
                            "to send to",
                            id_);
    }
  }

  switch (cfg_.backend) {
    case Mid70Backend::kSdk1: backend_ = make_sdk1_backend(*this, id_, cfg_); break;
    case Mid70Backend::kRawUdp: backend_ = make_raw_udp_backend(*this, id_, cfg_); break;
    case Mid70Backend::kInject: backend_ = make_inject_backend(*this, id_); break;
  }
  if (backend_ == nullptr) {
    const ScanError e = last_error_code();
    set_state(DeviceState::kFault, e);
    return e;  // the factory already set a detailed message
  }

  const std::int64_t now = ctx_.clock().nanos;
  {
    std::lock_guard<std::mutex> lock(m_);
    gaps_.reset();
    filter_stats_ = mid70::FilterStats{};
    batch_.clear();
    decimate_phase_ = 0;
    st_ = Mid70Stats{};
    bytes_in_ = 0;
    window_ = Window{};
    window_.t_start_ns = now;
    t_last_point_ns_ = 0;
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
  set_link(Mid70LinkState::kWaiting, now);

  if (cfg_.internal_supervisor_thread) {
    supervisor_run_.store(true, std::memory_order_release);
    supervisor_ = std::thread(&Mid70Driver::supervisor_loop, this);
  }

  SCAN_LOG_INFO(kMod,
                "device %u: started, backend=%s code=%s lidar=%s host=%s point:%u "
                "(decimation 1/%u, filter: no-return=%d tag-mask=0x%02X min-range=%.2f m)",
                id_, to_string(cfg_.backend),
                cfg_.broadcast_code.empty() ? "(any)" : cfg_.broadcast_code.c_str(),
                cfg_.udp.lidar_ip.empty() ? "(discovered)" : cfg_.udp.lidar_ip.c_str(),
                cfg_.udp.host_ip.c_str(),
                static_cast<unsigned>(cfg_.udp.bind_port != 0 ? cfg_.udp.bind_port
                                                              : cfg_.udp.host_point_port),
                decimate_stride_, cfg_.filter.drop_no_return ? 1 : 0,
                static_cast<unsigned>(cfg_.filter.tag_reject_mask),
                static_cast<double>(cfg_.filter.min_range_m));
  return kOkStatus;
}

Status Mid70Driver::stop() {
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
  set_link(Mid70LinkState::kDown, now);
  set_state(DeviceState::kIdle, ScanError::kOk);
  return kOkStatus;
}

Status Mid70Driver::open_backend() {
  if (backend_ == nullptr) {
    return set_last_error(ScanError::kInvalidState, "mid70 device %u: no backend", id_);
  }
  return backend_->open();
}

void Mid70Driver::close_backend() {
  if (backend_ != nullptr) backend_->close();
}

DeviceState Mid70Driver::state() const {
  std::lock_guard<std::mutex> lock(m_);
  return state_;
}

Mid70LinkState Mid70Driver::link_state() const {
  std::lock_guard<std::mutex> lock(m_);
  return link_;
}

Status Mid70Driver::push_bytes(ByteSpan bytes, TimePoint t_arrival) {
  // In the normal case the Mid-70 owns its sockets (SDK v1, or UdpSource in
  // raw mode) and bytes are never pushed from the app the way D6 serial
  // bytes are. Mid70Backend::kInject is the exception: there, ONE call is
  // ONE complete UDP datagram (never a partial read — datagram boundaries
  // are meaningful and are never reassembled), which is what makes a
  // recorded .lscan kMid70Points chunk replayable through the real driver.
  if (cfg_.backend != Mid70Backend::kInject) {
    return set_last_error(ScanError::kNotSupported,
                          "Mid-70 is a self-driven UDP source; push_bytes() does not apply "
                          "(replay through Mid70Backend::kInject instead)");
  }
  const mid70::PacketView v = mid70::parse_packet(bytes.data(), bytes.size());
  if (!v.valid()) {
    std::lock_guard<std::mutex> lock(m_);
    ++st_.bad_packets;
    return set_last_error(ScanError::kProtocolError,
                          "mid70 device %u: pushed %zu bytes are not a Mid-70 datagram", id_,
                          bytes.size());
  }
  // One ingest seam: on_point_packet() is where an IMU-typed datagram is
  // counted as unexpected, so there is nothing to route here.
  on_point_packet(bytes.data(), bytes.size(), t_arrival);
  return kOkStatus;
}

// --- point ingest ---------------------------------------------------------

void Mid70Driver::on_point_packet(const std::uint8_t* data, std::size_t len,
                                  TimePoint t_arrival) {
  const mid70::PacketView v = mid70::parse_packet(data, len);
  const std::int64_t t_ns = t_arrival.nanos;

  // Record-always (Tech Spec §3 rule 2): every datagram reaches the raw sink
  // verbatim, valid or not, exactly once. There is no second sink here — the
  // Mid-70 has one stream — so this is unconditional.
  if (cfg_.raw_sink != nullptr) {
    cfg_.raw_sink(data, len, t_ns, cfg_.raw_sink_user_data);
  }
  {
    std::lock_guard<std::mutex> lock(m_);
    bytes_in_ += len;
  }

  if (!v.valid()) {
    std::lock_guard<std::mutex> lock(m_);
    ++st_.bad_packets;
    return;
  }
  if (v.header->data_type == mid70::kDataTypeImu) {
    // A Mid-70 has no IMU. This is a Horizon/Avia on the wire, or a replay of
    // the wrong capture: recorded above, counted here, decoded nowhere. It
    // does NOT touch the gap tracker either — its cadence is 200 Hz, not
    // 1 kHz, and mixing the two would manufacture loss out of nothing.
    std::lock_guard<std::mutex> lock(m_);
    ++st_.unexpected_imu_packets;
    return;
  }

  // --- A4: the device clock, mapped onto engine time ----------------------
  //
  // The stamp must be decoded first (mid70_packets.h note 2): in NoSync/PPS
  // mode it is nanoseconds since power-on, in PPS+GPS mode a packed UTC
  // date. Either is a monotonic device clock, which is all the A4 estimator
  // needs — it learns the offset rather than assuming an epoch, which is why
  // this driver does not need the host-epoch anchor the ROS work carried.
  //
  // Arrival time still drives the watchdog and the health window (those are
  // wall-clock questions about this host), but the stamp the points are
  // STORED under is device time mapped into the engine domain, which is what
  // A6/A8 interpolate against. A stamp that does not decode (reserved /
  // unknown timestamp_type, or a malformed UTC) falls back to arrival time
  // and is withheld from both the estimator and the gap tracker: a made-up
  // number in either place is worse than a missing one.
  std::int64_t t_dev = 0;
  const bool dev_ok = mid70::decode_timestamp_ns(*v.header, &t_dev);
  std::int64_t t_data_ns = t_ns;
  if (ctx_.timesync != nullptr && dev_ok && t_dev > 0) {
    ctx_.timesync->add_pair(StreamId::kLidarMid70, t_dev, t_arrival);
    t_data_ns = ctx_.timesync->to_engine_time(StreamId::kLidarMid70, t_dev);
  }

  bool need_flush = false;
  {
    std::lock_guard<std::mutex> lock(m_);

    ++st_.point_packets;
    ++window_.packets;
    t_last_point_ns_ = t_ns;
    st_.t_last_point_ns = t_ns;

    // What the device says about itself, every packet. err_code carries
    // pps_ok and time_sync_status; the FAST-LIO work spent days inferring
    // those from stamp arithmetic while the lidar was reporting them here.
    st_.data_type = v.header->data_type;
    st_.timestamp_type = v.header->timestamp_type;
    st_.err_code_raw = v.header->err_code;
    st_.err = mid70::decode_error_code(v.header->err_code);
    st_.device_stamp_decodable = dev_ok;
    if (dev_ok) st_.t_device_last_ns = t_dev;

    // Loss accounting on the DEVICE TIMESTAMP. See mid70_packets.h: there is
    // no counter to work from, and this model shares the Mid-360 counter's
    // blind spot — a full outage looks like one long gap, which lands in the
    // unattributable bucket and is the watchdog's problem, not loss.
    if (dev_ok) {
      gaps_.set_interval_ns(packet_interval_ns(v));
      std::uint32_t lost = 0;
      (void)gaps_.observe(t_dev, &lost);
      window_.lost += lost;
      st_.packets_lost = gaps_.lost();
      st_.packets_duplicated = gaps_.duplicates();
      st_.counter_resets = gaps_.resets();
    }

    const std::uint32_t returns = returns_in_packet(v);
    st_.points_received += returns;
    window_.points += returns;

    decode_into_batch(v);

    st_.filter = filter_stats_;
    need_flush = batch_.size() >= cfg_.max_batch_points;
  }

  if (need_flush) flush_points(t_data_ns);

  // First data promotes us out of kStarting; the watchdog in tick() handles
  // the end of a silent-link episode, so there is nothing to do here.
  if (state() == DeviceState::kStarting) set_state(DeviceState::kStreaming, ScanError::kOk);
}

// Called with m_ held. Every wire type is widened to one mid70::Point so the
// filter, the decimator and the metric conversion have a single code path.
void Mid70Driver::decode_into_batch(const mid70::PacketView& v) {
  const std::uint32_t n = v.point_count;
  const std::size_t stride = v.per_point_bytes;

  // The one thing every type does with a decoded return.
  const auto emit = [&](const mid70::Point& p) {
    if (!mid70::point_passes(p, cfg_.filter, &filter_stats_)) return;
    ++st_.points_kept;

    // Deterministic decimation AFTER filtering: a replay of the same bytes
    // produces the same cloud, which is what E2's golden datasets need.
    // Random sampling would not. Explicit modulo on the counter itself, not
    // on a free-running one, so nothing shifts phase mid-capture.
    if (decimate_stride_ > 1) {
      const bool keep = decimate_phase_ == 0;
      if (++decimate_phase_ >= decimate_stride_) decimate_phase_ = 0;
      if (!keep) return;
    }

    PointVertex vert{};
    // mm → metres. The device reports in its own frame; A8 applies the
    // trajectory, A6 the extrinsic. The driver does no geometry.
    vert.x = static_cast<float>(p.x) * 0.001f;
    vert.y = static_cast<float>(p.y) * 0.001f;
    vert.z = static_cast<float>(p.z) * 0.001f;
    shade(vert, p.reflectivity);
    batch_.push_back(vert);
  };

  switch (v.header->data_type) {
    case mid70::kDataTypeCartesian: {
      for (std::uint32_t i = 0; i < n; ++i) {
        mid70::RawPoint raw{};
        // Datagram alignment is not ours to assume — memcpy, never a cast.
        std::memcpy(&raw, v.payload + static_cast<std::size_t>(i) * stride, sizeof(raw));
        mid70::Point p;
        p.x = raw.x;
        p.y = raw.y;
        p.z = raw.z;
        p.reflectivity = raw.reflectivity;
        p.tag = 0;  // non-extended types carry no tag and are never tag-rejected
        emit(p);
      }
      break;
    }
    case mid70::kDataTypeExtendCartesian: {
      for (std::uint32_t i = 0; i < n; ++i) {
        mid70::ExtendRawPoint raw{};
        std::memcpy(&raw, v.payload + static_cast<std::size_t>(i) * stride, sizeof(raw));
        mid70::Point p;
        p.x = raw.x;
        p.y = raw.y;
        p.z = raw.z;
        p.reflectivity = raw.reflectivity;
        p.tag = raw.tag;
        emit(p);
      }
      break;
    }
    case mid70::kDataTypeSpherical: {
      for (std::uint32_t i = 0; i < n; ++i) {
        mid70::SpherPoint raw{};
        std::memcpy(&raw, v.payload + static_cast<std::size_t>(i) * stride, sizeof(raw));
        emit(mid70::from_spherical(raw.depth, raw.theta, raw.phi, raw.reflectivity, 0));
      }
      break;
    }
    case mid70::kDataTypeExtendSpherical: {
      for (std::uint32_t i = 0; i < n; ++i) {
        mid70::ExtendSpherPoint raw{};
        std::memcpy(&raw, v.payload + static_cast<std::size_t>(i) * stride, sizeof(raw));
        emit(mid70::from_spherical(raw.depth, raw.theta, raw.phi, raw.reflectivity, raw.tag));
      }
      break;
    }
    case mid70::kDataTypeDualExtendCartesian: {
      // Two returns per sample, and BOTH are real geometry — the second
      // return is what sees through a window frame or a chain-link fence.
      // The tag's return-number bits distinguish them; the filter keeps both
      // unless the tag says the return itself is unreliable.
      for (std::uint32_t i = 0; i < n; ++i) {
        mid70::DualExtendRawPoint raw{};
        std::memcpy(&raw, v.payload + static_cast<std::size_t>(i) * stride, sizeof(raw));
        mid70::Point a;
        a.x = raw.x1;
        a.y = raw.y1;
        a.z = raw.z1;
        a.reflectivity = raw.reflectivity1;
        a.tag = raw.tag1;
        emit(a);
        mid70::Point b;
        b.x = raw.x2;
        b.y = raw.y2;
        b.z = raw.z2;
        b.reflectivity = raw.reflectivity2;
        b.tag = raw.tag2;
        emit(b);
      }
      break;
    }
    default:
      // parse_packet() accepts nothing else; a type added there without a
      // decode here would silently produce an empty cloud, so say so.
      SCAN_LOG_WARN(kMod, "device %u: no decoder for data_type %u", id_,
                    static_cast<unsigned>(v.header->data_type));
      break;
  }
}

void Mid70Driver::on_device_connected(const char* broadcast_code, const char* ip,
                                      const char* firmware) {
  {
    std::lock_guard<std::mutex> lock(m_);
    if (broadcast_code != nullptr) st_.broadcast_code = broadcast_code;
    if (ip != nullptr) st_.device_ip = ip;
    if (firmware != nullptr) st_.firmware = firmware;
  }
  SCAN_LOG_INFO(kMod, "device %u: connected (code=%s ip=%s fw=%s)", id_,
                broadcast_code ? broadcast_code : "?", ip ? ip : "?",
                firmware ? firmware : "?");
}

void Mid70Driver::on_device_disconnected() {
  // Deliberately just a log line. The SDK reports a device "disconnected" on
  // a missed heartbeat, which is a weaker signal than the data watchdog's
  // "no points for data_timeout_ms" — demoting state from here would race
  // the watchdog and produce two different stories about the same second.
  SCAN_LOG_INFO(kMod, "device %u: SDK reports the device disconnected (the data watchdog "
                      "decides what that means for the stream)", id_);
}

void Mid70Driver::flush_points(std::int64_t t_ns) {
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
      StreamId::kLidarMid70, Span<const PointVertex>(flush_buf_.data(), flush_buf_.size()), t_ns,
      &appended);

  {
    std::lock_guard<std::mutex> lock(m_);
    st_.points_appended += appended;
    window_.points_appended += appended;
    if (!s.ok()) st_.points_dropped_store += flush_buf_.size() - appended;
  }
  if (!s.ok() && ctx_.bus != nullptr) {
    // Backpressure is not silent: the app must be able to say "the store is
    // full" rather than quietly rendering a truncated cloud.
    ErrorPayload e{};
    e.error = s.error();
    e.device = id_;
    e.stream = StreamId::kLidarMid70;
    ctx_.bus->publish(EventType::kError, e, t_ns);
  }
  flush_buf_.clear();
}

// --- supervisor: watchdog, reconnect, health ------------------------------

void Mid70Driver::supervisor_loop() {
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

void Mid70Driver::tick(TimePoint now) {
  const std::int64_t t = now.nanos;
  const std::int64_t data_timeout_ns =
      static_cast<std::int64_t>(cfg_.reconnect.data_timeout_ms) * 1000000LL;
  const std::int64_t reinit_after_ns =
      static_cast<std::int64_t>(cfg_.reconnect.reinit_after_silence_ms) * 1000000LL;

  bool want_reinit = false;
  bool give_up = false;
  bool publish = false;
  bool stale_batch = false;
  Mid70Stats snapshot;

  {
    std::lock_guard<std::mutex> lock(m_);

    // A partial batch that has gone stale should still reach the renderer
    // rather than sit in the buffer until the stream picks up again.
    stale_batch = !batch_.empty() && (t - window_.t_start_ns) > data_timeout_ns;

    const bool ever_saw_data = t_last_point_ns_ != 0;
    const std::int64_t t_ref = ever_saw_data ? t_last_point_ns_ : t_silent_since_ns_;
    const std::int64_t silence_ns = t - t_ref;
    // Before the first packet the clock we are running against is discovery
    // plus the handshake, not the stream, so the generous connect timeout
    // applies — a cold Mid-70 can self-heat for minutes ([M] §4).
    const std::int64_t timeout_ns =
        ever_saw_data ? data_timeout_ns
                      : static_cast<std::int64_t>(cfg_.reconnect.connect_timeout_ms) * 1000000LL;

    switch (link_) {
      case Mid70LinkState::kDown:
        break;

      case Mid70LinkState::kWaiting:
      case Mid70LinkState::kUp:
        if (ever_saw_data && silence_ns <= timeout_ns) {
          if (link_ != Mid70LinkState::kUp) {
            link_ = Mid70LinkState::kUp;
            t_silent_since_ns_ = 0;
            // A resume with no re-init is the cable-pull case: the link comes
            // back and the device was never confused about where to send.
            if (st_.watchdog_trips > 0) {
              ++st_.clean_resumes;
              SCAN_LOG_INFO(kMod,
                            "device %u: link resumed with no re-init after %.2f s "
                            "(clean resume #%llu — cable-class fault; note the timestamp-gap "
                            "model counted %llu unattributable gaps, which is expected: the "
                            "device keeps stamping while the wire is down)",
                            id_, static_cast<double>(silence_ns) * 1e-9,
                            static_cast<unsigned long long>(st_.clean_resumes),
                            static_cast<unsigned long long>(st_.counter_resets));
            }
          }
        } else if (cfg_.reconnect.enabled && silence_ns > timeout_ns) {
          link_ = Mid70LinkState::kSilent;
          t_silent_since_ns_ = t_ref;
          ++st_.watchdog_trips;
          t_next_reinit_ns_ = t_ref + reinit_after_ns;
          SCAN_LOG_WARN(kMod,
                        "device %u: DATA WATCHDOG tripped — no point packet for %.2f s "
                        "(trip #%llu). The timestamp-gap model cannot attribute this to loss; "
                        "forcing a full SDK re-init in %.1f s if it stays silent.",
                        id_, static_cast<double>(silence_ns) * 1e-9,
                        static_cast<unsigned long long>(st_.watchdog_trips),
                        static_cast<double>(cfg_.reconnect.reinit_after_silence_ms) / 1000.0);
        }
        break;

      case Mid70LinkState::kSilent:
      case Mid70LinkState::kReinitializing:
        if (ever_saw_data && silence_ns <= timeout_ns) {
          const bool after_reinit = (link_ == Mid70LinkState::kReinitializing);
          link_ = Mid70LinkState::kUp;
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
        const std::uint64_t win_total = window_.packets + window_.lost;
        st_.loss_pct_window =
            win_total == 0 ? 0.0
                           : 100.0 * static_cast<double>(window_.lost) /
                                 static_cast<double>(win_total);
      }
      st_.loss_pct_total = 100.0 * gaps_.loss_fraction();
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
  const Mid70LinkState link = snapshot.link;
  const DeviceState cur = state();
  if (link == Mid70LinkState::kSilent || link == Mid70LinkState::kReinitializing) {
    if (cur == DeviceState::kStreaming || cur == DeviceState::kStarting) {
      set_state(DeviceState::kDegraded, ScanError::kDeviceNotResponding);
    }
  } else if (link == Mid70LinkState::kUp) {
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
  // SDK v1 does re-discover a device that re-broadcasts, so this path is
  // less load-bearing than the Mid-360's (where S2 proved the SDK never
  // re-configures a handle it has already seen). It is kept because a device
  // that reboots into a DIFFERENT timestamp mode — NoSync after losing its
  // GPS fix, say — is only recovered cleanly by tearing down and
  // re-handshaking, and because a state machine that only runs on hardware
  // is a state machine nobody can test.
  {
    std::lock_guard<std::mutex> lock(m_);
    link_ = Mid70LinkState::kReinitializing;
    ++st_.forced_reinits;
    st_.link = link_;
  }
  SCAN_LOG_WARN(kMod,
                "device %u: FORCING full SDK re-init (attempt #%llu) — %.1f s of silence is "
                "past anything a live link explains",
                id_, static_cast<unsigned long long>(snapshot.forced_reinits + 1),
                static_cast<double>(cfg_.reconnect.reinit_after_silence_ms) / 1000.0);

  close_backend();
  const Status s = open_backend();

  {
    std::lock_guard<std::mutex> lock(m_);
    // Restart the silence clock from now, so the next attempt is measured
    // from this attempt and the backoff is honoured.
    t_last_point_ns_ = 0;
    t_silent_since_ns_ = t;
    // The device may come back with a different clock origin (a power-cycle
    // resets the NoSync counter to 0); a stale `prev` would then read as one
    // enormous duplicate. Forget it along with the socket.
    gaps_.reset();
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
    // attempt: the device has to be given time to broadcast and answer.
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

void Mid70Driver::publish_health(const Mid70Stats& s, std::int64_t t_ns) {
  SCAN_LOG_DEBUG(kMod,
                 "device %u: %.0f pts/s in, %.0f pts/s to store, loss %.4f%% (window) / "
                 "%.4f%% (total), link=%s, device clock=%s sync=%s pps=%d",
                 id_, s.points_per_sec, s.points_appended_per_sec, s.loss_pct_window,
                 s.loss_pct_total, to_string(s.link),
                 mid70::to_string_timestamp_type(s.timestamp_type),
                 mid70::to_string_time_sync_status(s.err.time_sync_status),
                 s.err.pps_ok ? 1 : 0);
  if (ctx_.bus == nullptr) return;
  DeviceHealthPayload p{};
  p.device = id_;
  p.state = s.state;
  p.points_out = s.points_appended;
  p.points_per_sec = s.points_per_sec;
  // No checksums on this transport; the equivalent quality signal is the
  // fraction of packets that arrived, so the app's one health widget means
  // the same thing for a D6, a Mid-360 and a Mid-70.
  p.checksum_pass_rate = 1.0 - (s.loss_pct_window / 100.0);
  ctx_.bus->publish(EventType::kDeviceHealth, p, t_ns);
}

void Mid70Driver::set_link(Mid70LinkState next, std::int64_t t_ns) {
  std::lock_guard<std::mutex> lock(m_);
  if (link_ == next) return;
  link_ = next;
  st_.link = next;
  if (next != Mid70LinkState::kUp) t_silent_since_ns_ = t_ns;
}

void Mid70Driver::set_state(DeviceState next, ScanError err) {
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
    p.kind = DeviceKind::kMid70;
    p.state = next;
    p.previous = prev;
    p.error = err;
    ctx_.bus->publish(EventType::kDeviceState, p, ctx_.clock().nanos);
  }
}

Mid70Stats Mid70Driver::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  Mid70Stats s = st_;
  s.state = state_;
  s.link = link_;
  s.filter = filter_stats_;
  s.packets_lost = gaps_.lost();
  s.packets_duplicated = gaps_.duplicates();
  s.counter_resets = gaps_.resets();
  s.loss_pct_total = 100.0 * gaps_.loss_fraction();
  return s;
}

DeviceHealth Mid70Driver::health() const {
  DeviceHealth h{};
  h.id = id_;
  h.kind = DeviceKind::kMid70;
  std::lock_guard<std::mutex> lock(m_);
  h.state = state_;
  h.last_error = last_error_;
  h.packets_ok = st_.point_packets;
  h.packets_bad = st_.bad_packets + gaps_.lost();
  h.points_out = st_.points_appended;
  h.drops = st_.points_dropped_store;
  // Measured, not multiplied: a Mid-70 datagram's size depends on the type
  // the firmware is sending (1318 B cartesian, 1362 B extended), and a
  // malformed datagram still consumed wire.
  h.bytes_in = bytes_in_;
  h.points_per_sec = st_.points_per_sec;
  // A Mid-70 is a non-repetitive Risley-prism scanner: there is no
  // revolution and, unlike the Mid-360, no IMU rate to put on the dial in
  // its place. Reporting 0 is honest; the IMU row belongs to the paired
  // kImuSerial device.
  h.rotation_hz = 0.0;
  h.checksum_pass_rate = 1.0 - gaps_.loss_fraction();
  h.t_last_data_ns = st_.t_last_point_ns;
  return h;
}

}  // namespace scanengine
