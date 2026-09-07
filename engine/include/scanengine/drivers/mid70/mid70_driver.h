// mid70_driver.h — Livox Mid-70 driver (task A17).
//
// Owner: A17. Implemented over a VENDORED, PATCHED Livox-SDK **v1**
// (engine/third_party/fetch_sdk1.sh + patches/sdk1-*.patch). Read
// engine/docs/A17-mid70-driver.md for the operational side; this header
// carries the contract and the reasons behind the defaults.
//
// This is the Mid-360 driver's shape (drivers/mid360/mid360_driver.h) with
// the four differences a Mid-70 forces, each learned the expensive way on a
// bench before this port and written down here so nobody learns it twice:
//
//  1. SDK v1, not SDK2. Discovery is a device BROADCAST to UDP 55000 (the
//     host listens; nothing needs to bind a broadcast address), then a
//     unicast handshake to the lidar's port 65000. That is why explicit
//     `udp.lidar_ip` is optional here where it is mandatory for the Mid-360:
//     the S2 Darwin bind() failure was on the SENDING side of SDK2's
//     discovery and does not exist in v1 (spikes/s8-mid70-sdk1/REPORT.md).
//     Give `broadcast_code` to pick one lidar on a shared switch.
//
//  2. No built-in IMU. This driver publishes points only. A Mid-70 session
//     pairs it with a DeviceKind::kImuSerial device, and the two streams
//     are mapped onto engine time independently by A4 — that is the whole
//     of the clock story, and it replaces the host-epoch anchor / self-sync
//     machinery the ROS driver needed.
//
//  3. No sequence counter. Loss is inferred from device-timestamp gaps
//     (mid70_packets.h GapTracker). Its blind spot is the same as the
//     Mid-360's: a full outage is the wall-clock watchdog's job.
//
//  4. The device reports its OWN clock status in every datagram
//     (err_code.time_sync_status / pps_ok). The driver surfaces it in
//     Mid70Stats and the health string; it does not try to infer it.
//
// SDK v1 keeps a handle per device and re-discovers a power-cycled unit on
// its own (it re-broadcasts; the SDK's broadcast callback fires again). The
// forced-re-init path is kept anyway, because a device that reboots into a
// different timestamp mode is only recovered cleanly by tearing down and
// re-handshaking — and because the state machine is already tested.
#ifndef SCANENGINE_DRIVERS_MID70_MID70_DRIVER_H
#define SCANENGINE_DRIVERS_MID70_MID70_DRIVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scanengine/drivers/driver.h"
#include "scanengine/drivers/mid70/mid70_packets.h"
#include "scanengine/transport/udp_source.h"

namespace scanengine {

enum class Mid70Backend : std::uint8_t {
  // The vendored SDK v1 runs discovery, the handshake and the heartbeat,
  // sets Cartesian output, starts sampling and delivers datagrams. The only
  // backend that can bring an out-of-the-box device up.
  kSdk1 = 0,
  // Listen-only: bind one host port and decode whatever arrives. Works only
  // against a device already told to stream here (or a replay harness).
  kRawUdp = 1,
  // No transport: the caller pushes complete datagrams through push_bytes().
  // .lscan kMid70Points chunks come back this way; the reconnect state
  // machine is tested against a scripted clock with no sockets.
  kInject = 2,
};

const char* to_string(Mid70Backend b) noexcept;

enum class Mid70LinkState : std::uint8_t {
  kDown = 0,
  kWaiting = 1,
  kUp = 2,
  kSilent = 3,
  kReinitializing = 4,
};

const char* to_string(Mid70LinkState s) noexcept;

// Same policy and the same reasons as Mid360ReconnectConfig. The Mid-70
// sends ~1,000 datagrams/s (100 points each at 100k pts/s), so 1 s of
// silence is ~1,000 missed packets.
struct Mid70ReconnectConfig {
  bool enabled = true;
  std::uint32_t data_timeout_ms = 1000;
  // SDK v1 discovery is a 1 Hz device broadcast plus a handshake: a cold
  // device answers inside ~2 s; a device still self-heating ([M] §4: up to
  // three minutes below 0 °C) does not, and that is a reason to keep
  // trying, not to fault.
  std::uint32_t connect_timeout_ms = 10000;
  std::uint32_t reinit_after_silence_ms = 5000;
  std::uint32_t reinit_backoff_initial_ms = 1000;
  std::uint32_t reinit_backoff_max_ms = 30000;
  std::uint32_t max_reinits = 0;
};

// Raw-datagram sink: every datagram, valid or not, before parsing — the
// record-always contract, replayable through Mid70Backend::kInject.
using Mid70RawSink = void (*)(const std::uint8_t* data, std::size_t len,
                              std::int64_t t_arrival_ns, void* user_data);

struct Mid70Config {
  UdpConfig udp{};

  Mid70Backend backend = Mid70Backend::kSdk1;

  // Which lidar to connect to when more than one broadcasts. Empty = the
  // first Mid-70 heard. This is the 15-character code printed on the
  // device (e.g. "3GGDJ5N00100101"); the SDK identifies devices by it, not
  // by IP.
  std::string broadcast_code;

  mid70::PointFilterConfig filter{};
  Mid70ReconnectConfig reconnect{};

  // Live decimation budget out of the sensor's 100k pts/s (Tech Spec §3.3).
  // 0 = none (post-processing / replay). Deterministic: every Nth survivor.
  std::uint32_t live_points_per_sec = 40000;

  Mid70RawSink raw_sink = nullptr;
  void* raw_sink_user_data = nullptr;

  std::uint32_t max_batch_points = 8192;
  std::uint32_t health_period_ms = 1000;
  double max_loss_pct = 1.0;
  std::uint64_t loss_min_packets = 200;

  // --- SDK v1 backend only ------------------------------------------------
  bool sdk_console_log = false;
  // Ask the device for dual-return mode (200k pts/s, kDualExtendCartesian).
  // Off: single return, which is what the FAST-LIO work ran and what the
  // odometry's decimation budget assumes.
  bool dual_return = false;

  bool internal_supervisor_thread = true;
};

struct Mid70Stats {
  Mid70LinkState link = Mid70LinkState::kDown;
  DeviceState state = DeviceState::kDisconnected;

  std::uint64_t point_packets = 0;
  std::uint64_t points_received = 0;
  std::uint64_t points_kept = 0;
  std::uint64_t points_appended = 0;
  std::uint64_t points_dropped_store = 0;
  std::uint64_t bad_packets = 0;
  std::uint64_t unexpected_imu_packets = 0;   // a Mid-70 never sends these

  std::uint64_t packets_lost = 0;      // timestamp-gap model
  std::uint64_t packets_duplicated = 0;
  std::uint64_t counter_resets = 0;    // unattributable gaps

  mid70::FilterStats filter{};

  double points_per_sec = 0.0;
  double points_appended_per_sec = 0.0;
  double loss_pct_window = 0.0;
  double loss_pct_total = 0.0;

  std::uint64_t watchdog_trips = 0;
  std::uint64_t clean_resumes = 0;
  std::uint64_t forced_reinits = 0;
  std::uint64_t reinit_failures = 0;

  std::int64_t t_last_point_ns = 0;
  std::int64_t t_silent_since_ns = 0;

  // What the device says about itself, from the last datagram.
  std::uint8_t data_type = 0xFF;
  std::uint8_t timestamp_type = mid70::kTimestampUnknown;
  std::uint32_t err_code_raw = 0;
  mid70::ErrorCode err{};
  std::int64_t t_device_last_ns = 0;   // last decoded device stamp
  bool device_stamp_decodable = false;

  std::string broadcast_code;
  std::string device_ip;
  std::string firmware;                // "a.b.c.d" once the SDK reports it
};

class Mid70BackendImpl;  // src/drivers/mid70/, one per Mid70Backend value

class Mid70Driver final : public Driver {
 public:
  Mid70Driver(DeviceId id, const Mid70Config& cfg, const DriverContext& ctx);
  ~Mid70Driver() override;

  const char* name() const override { return "mid70"; }
  DeviceKind kind() const override { return DeviceKind::kMid70; }
  DeviceId id() const override { return id_; }

  Status start() override;
  Status stop() override;
  DeviceState state() const override;
  DeviceHealth health() const override;

  // Inject backend only: ONE call is ONE complete datagram.
  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override;

  const Mid70Config& config() const { return cfg_; }
  Mid70Stats stats() const;
  Mid70LinkState link_state() const;

  // --- ingest seam ---------------------------------------------------------
  // Called from the SDK's receive thread, the raw-UDP source thread, or a
  // test. `data` is one complete UDP datagram (SDK v1 hands the driver the
  // LivoxEthPacket bytes exactly as received).
  void on_point_packet(const std::uint8_t* data, std::size_t len, TimePoint t_arrival);
  void on_device_connected(const char* broadcast_code, const char* ip, const char* firmware);
  void on_device_disconnected();

  // One supervisor step; tests drive it with a scripted clock.
  void tick(TimePoint now);

 private:
  struct Window {
    std::int64_t t_start_ns = 0;
    std::uint64_t points = 0;
    std::uint64_t points_appended = 0;
    std::uint64_t packets = 0;
    std::uint64_t lost = 0;
  };

  void set_state(DeviceState next, ScanError err);
  void set_link(Mid70LinkState next, std::int64_t t_ns);
  void flush_points(std::int64_t t_ns);
  void publish_health(const Mid70Stats& s, std::int64_t t_ns);
  Status open_backend();
  void close_backend();
  void supervisor_loop();
  void decode_into_batch(const mid70::PacketView& v);

  DeviceId id_;
  Mid70Config cfg_;
  DriverContext ctx_;

  mutable std::mutex m_;
  DeviceState state_ = DeviceState::kDisconnected;
  Mid70LinkState link_ = Mid70LinkState::kDown;
  ScanError last_error_ = ScanError::kOk;

  std::unique_ptr<Mid70BackendImpl> backend_;

  std::mutex flush_m_;
  std::vector<PointVertex> flush_buf_;

  mid70::GapTracker gaps_;
  mid70::FilterStats filter_stats_;
  std::vector<PointVertex> batch_;
  std::uint32_t decimate_stride_ = 1;
  std::uint32_t decimate_phase_ = 0;

  Mid70Stats st_{};
  Window window_{};

  // Measured wire bytes (DeviceHealth::bytes_in). Counted rather than
  // derived, because a Mid-70 datagram's size depends on the data_type the
  // firmware happens to be sending and a malformed datagram still arrived.
  std::uint64_t bytes_in_ = 0;

  std::int64_t t_last_point_ns_ = 0;
  std::int64_t t_silent_since_ns_ = 0;
  std::int64_t t_next_reinit_ns_ = 0;
  std::uint32_t reinit_backoff_ms_ = 0;

  std::thread supervisor_;
  std::atomic<bool> supervisor_run_{false};
  std::condition_variable supervisor_cv_;
  std::mutex supervisor_m_;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_MID70_MID70_DRIVER_H
