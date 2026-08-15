#include "mid360_probe.h"

#include <unistd.h>

#include "scanengine/timesync/clock.h"

namespace lidarscan_jni {
namespace {

using scanengine::DeviceConfig;
using scanengine::DeviceKind;
using scanengine::EngineConfig;
using scanengine::Mid360Backend;
using scanengine::Mid360Config;
using scanengine::SessionConfig;

// A3 §5's watchdog thresholds, applied to the one observable the app can
// actually see (`t_last_data_ns`). These are the driver's own defaults —
// Mid360ReconnectConfig::data_timeout_ms / reinit_after_silence_ms /
// connect_timeout_ms — restated here because Mid360Stats::link is not
// reachable through Engine (see mid360_probe.h's limitation 1). Keeping the
// numbers identical is what makes the derived link state agree with the
// driver's internal one rather than tell the user a second story.
constexpr std::int64_t kDataTimeoutNs = 1000LL * 1000000LL;          // 1 s
constexpr std::int64_t kReinitAfterSilenceNs = 5000LL * 1000000LL;   // 5 s

// Only one SDK2 instance may exist per process (A3 §3). This is the app-side
// mirror of that rule, so the wizard can refuse a second probe (or a capture
// start) with a sentence instead of letting the engine return kBusy from
// somewhere the user cannot see.
std::atomic<bool> g_sdk2_active{false};

std::int64_t now_ns() { return scanengine::steady_now().nanos; }

// `Result`/`Status` carry only a ScanError enum; the human sentence lives in
// the thread-local `last_error_message()` the engine's set_last_error() fills
// in. Composing both is what desktop C2's addMid360 does, and it is what
// turns "kInvalidArgument" into "check that host_ip '192.168.1.5' is a local
// address and that ports 56101/56201/56301 are free".
std::string engine_error_text(scanengine::ScanError err) {
  std::string out = scanengine::error_str(err);
  const char* detail = scanengine::last_error_message();
  if (detail != nullptr && detail[0] != '\0') {
    out += ": ";
    out += detail;
  }
  return out;
}

}  // namespace

Mid360Probe::~Mid360Probe() { stop(); }

bool Mid360Probe::is_sdk2_active() { return g_sdk2_active.load(std::memory_order_acquire); }

void Mid360Probe::on_raw_datagram(const std::uint8_t* /*data*/, std::size_t len, bool is_imu,
                                  std::int64_t t_arrival_ns, void* user_data) {
  // Called on the receive thread, before any parsing, with no driver lock
  // held (Mid360RawSink's contract). Counting is all it does — this is the
  // "bytes are arriving but nothing parses" signal, which is the difference
  // between a cabling fault and a configuration fault, and it must stay
  // cheap enough not to perturb what it measures.
  auto* self = static_cast<Mid360Probe*>(user_data);
  if (self == nullptr) return;
  std::lock_guard<std::mutex> lock(self->m_);
  if (is_imu) {
    ++self->datagrams_imu_;
  } else {
    ++self->datagrams_point_;
  }
  self->datagram_bytes_ += static_cast<std::uint64_t>(len);
  if (self->t_first_datagram_ns_ == 0) self->t_first_datagram_ns_ = t_arrival_ns;
  self->t_last_datagram_ns_ = t_arrival_ns;
}

bool Mid360Probe::start(const Params& params) {
  if (running_.load(std::memory_order_acquire)) {
    last_error_ = "a Mid-360 probe is already running";
    return false;
  }
  if (params.lidar_ip.empty()) {
    // The driver would reject this too; saying it here keeps the message in
    // the wizard's own words rather than the engine's macOS-flavoured one.
    last_error_ = "lidar_ip is required — the Mid-360 is never discovered by broadcast on this path";
    return false;
  }
  if (params.host_ip.empty()) {
    last_error_ = "host_ip is required — the device is TOLD where to stream; it does not discover us";
    return false;
  }

  const bool wants_sdk2 = (params.backend == static_cast<int>(Mid360Backend::kSdk2));
  if (wants_sdk2) {
    bool expected = false;
    if (!g_sdk2_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      last_error_ =
          "the Livox SDK2 is a process-wide singleton and is already in use "
          "(stop the running probe or the active capture first)";
      return false;
    }
    holds_sdk2_ = true;
  }

  EngineConfig ecfg;
  ecfg.app_name = "lidarscan-android-mid360-probe";
  auto created = scanengine::Engine::create(ecfg);
  if (!created.ok()) {
    last_error_ = scanengine::error_str(created.error());
    if (holds_sdk2_) {
      g_sdk2_active.store(false, std::memory_order_release);
      holds_sdk2_ = false;
    }
    return false;
  }
  engine_ = std::move(created.value());

  // Preview, never record: this is a transport check, not a capture. An
  // empty lscan_dir with record = false is the engine's own "live preview"
  // mode, and it is exactly what desktop C2's self-test starts with
  // (`startSession(QString(), profile, /*record=*/false)`).
  SessionConfig scfg;
  scfg.record = false;
  scfg.profile = "quickscan";
  scfg.live_slam = false;
  auto started = engine_->start_session(scfg);
  if (!started.ok()) {
    last_error_ = engine_error_text(started.error());
    engine_.reset();
    if (holds_sdk2_) {
      g_sdk2_active.store(false, std::memory_order_release);
      holds_sdk2_ = false;
    }
    return false;
  }

  Mid360Config mcfg;
  mcfg.backend = wants_sdk2 ? Mid360Backend::kSdk2 : Mid360Backend::kRawUdp;
  mcfg.udp.lidar_ip = params.lidar_ip;
  mcfg.udp.host_ip = params.host_ip;
  mcfg.udp.point_port = params.device_point_port;
  mcfg.udp.imu_port = params.device_imu_port;
  mcfg.udp.cmd_port = params.device_cmd_port;
  mcfg.udp.host_point_port = params.host_point_port;
  mcfg.udp.host_imu_port = params.host_imu_port;
  mcfg.udp.host_cmd_port = params.host_cmd_port;
  mcfg.live_points_per_sec = params.live_points_per_sec;
  mcfg.raw_sink = &Mid360Probe::on_raw_datagram;
  mcfg.raw_sink_user_data = this;

  if (!wants_sdk2 && params.prebound_point_fd >= 0) {
    mcfg.udp.prebound_fd = params.prebound_point_fd;
    owned_fd_ = params.prebound_point_fd;
    // See mid360_probe.h's limitation 2: one `prebound_fd` in `UdpConfig`,
    // but RawUdpBackend hands the SAME UdpConfig to both its point and its
    // IMU UdpSource. Two receive threads on one socket would steal each
    // other's datagrams, so a pre-bound run is point-only. Not a silent
    // downgrade: the wizard says "IMU off (pre-bound socket)" on this path.
    mcfg.publish_imu = false;
  } else {
    mcfg.publish_imu = params.publish_imu;
  }

  DeviceConfig dcfg;
  dcfg.kind = DeviceKind::kMid360;
  dcfg.mid360 = mcfg;

  auto added = engine_->add_device(dcfg);
  if (!added.ok()) {
    // error_str() alone is a bare enum name; last_error_message() is where
    // the SDK2 backend puts the sentence that actually helps ("check that
    // host_ip '…' is a local address and that ports …/…/… are free"). Desktop
    // C2 composes the same pair for the same reason.
    last_error_ = engine_error_text(added.error());
    (void)engine_->stop_session();
    engine_.reset();
    owned_fd_ = -1;  // never reached the engine; Kotlin still owns it
    if (holds_sdk2_) {
      g_sdk2_active.store(false, std::memory_order_release);
      holds_sdk2_ = false;
    }
    return false;
  }
  device_ = added.value();
  backend_ = params.backend;
  t_start_ns_ = now_ns();
  {
    std::lock_guard<std::mutex> lock(m_);
    datagrams_point_ = 0;
    datagrams_imu_ = 0;
    datagram_bytes_ = 0;
    t_first_datagram_ns_ = 0;
    t_last_datagram_ns_ = 0;
  }
  last_error_.clear();
  running_.store(true, std::memory_order_release);
  return true;
}

void Mid360Probe::stop() {
  if (engine_) {
    if (device_ != scanengine::kInvalidDeviceId) {
      (void)engine_->remove_device(device_);
      device_ = scanengine::kInvalidDeviceId;
    }
    (void)engine_->stop_session();
    engine_.reset();
  }
  running_.store(false, std::memory_order_release);

  // UdpSource "does not close the descriptor on stop() — the app owns it"
  // (udp_source.h). The app, here, is this class: Kotlin handed down a dup
  // it had already detached, so if we do not close it the fd leaks for the
  // life of the process. Ordering matters — the engine is torn down first,
  // so no receive thread can still be in recvfrom() on it.
  if (owned_fd_ >= 0) {
    ::close(owned_fd_);
    owned_fd_ = -1;
  }
  if (holds_sdk2_) {
    g_sdk2_active.store(false, std::memory_order_release);
    holds_sdk2_ = false;
  }
}

Mid360ProbeSnapshot Mid360Probe::snapshot() const {
  Mid360ProbeSnapshot s;
  s.running = running_.load(std::memory_order_acquire);
  s.backend = backend_;
  if (!s.running || !engine_) return s;

  s.elapsed_since_start_ns = now_ns() - t_start_ns_;

  auto health = engine_->device_health(device_);
  if (health.ok()) {
    const auto& h = health.value();
    s.device_state = static_cast<std::int32_t>(h.state);
    s.last_error = static_cast<std::int32_t>(h.last_error);
    s.packets_ok = h.packets_ok;
    s.packets_bad = h.packets_bad;
    s.points_out = h.points_out;
    s.drops = h.drops;
    s.bytes_in = h.bytes_in;
    s.points_per_sec = h.points_per_sec;
    // A3 §5: a Mid-360 has no revolutions, so `rotation_hz` carries the IMU
    // rate and `checksum_pass_rate` carries 1 − loss ("so a D6 and a Mid-360
    // mean the same thing on the same dial"). Un-aliasing them here is what
    // lets the wizard label them honestly.
    s.imu_hz = h.rotation_hz;
    s.loss_pct = (1.0 - h.checksum_pass_rate) * 100.0;
    if (s.loss_pct < 0.0) s.loss_pct = 0.0;
    s.t_last_data_ns = h.t_last_data_ns;
  }

  std::int64_t t_last_datagram = 0;
  {
    std::lock_guard<std::mutex> lock(m_);
    s.datagrams_point = datagrams_point_;
    s.datagrams_imu = datagrams_imu_;
    s.datagram_bytes = datagram_bytes_;
    s.t_first_datagram_ns = t_first_datagram_ns_;
    s.t_last_datagram_ns = t_last_datagram_ns_;
    t_last_datagram = t_last_datagram_ns_;
  }

  // Link state, derived from wall-clock silence exactly as A3 §5 specifies —
  // and note WHY it is wall clock and not the packet counter: S2 measured 0
  // counted losses across three separate 15-second cable pulls, because the
  // device's counter keeps advancing while the wire is down. "0.0% loss" and
  // "the cable is out" are the same reading on `udp_cnt`.
  const std::int64_t reference = t_last_datagram != 0 ? t_last_datagram : s.t_last_data_ns;
  if (reference == 0) {
    s.link_state = 1;  // kWaiting — started, nothing has ever arrived
  } else {
    const std::int64_t silence = now_ns() - reference;
    if (silence < kDataTimeoutNs) {
      s.link_state = 2;  // kUp
    } else if (silence < kReinitAfterSilenceNs) {
      s.link_state = 3;  // kSilent
    } else {
      s.link_state = 4;  // kReinitializing (the driver is forcing a re-init)
    }
  }
  return s;
}

}  // namespace lidarscan_jni
