// mid360_driver.h — Livox Mid-360 driver (task A3).
//
// Owner: A3. Implemented over a VENDORED, PATCHED Livox-SDK2
// (engine/third_party/fetch_sdk2.sh + patches/). Read
// engine/docs/A3-mid360-driver.md for the operational side; this header
// carries the contract and the reasons behind the defaults.
//
// FIVE FINDINGS FROM THE S2 SPIKE ARE BAKED INTO THIS FILE. Each was
// measured, not guessed (spikes/s2-mid360-sim/REPORT.md + FOLLOWUP_NOTES.md),
// and each would otherwise be re-discovered the expensive way on a bench:
//
//  1. Stock SDK2 cannot start on macOS — it bind()s 255.255.255.255, which
//     Darwin rejects. We vendor a patched SDK2. `ENGINE_WITH_LIVOX_SDK2=OFF`
//     (the default when third_party/Livox-SDK2 is absent) still builds this
//     driver: everything except the SDK backend is SDK-free, and start()
//     then fails loudly naming the fetch script.
//
//  2. There is no broadcast discovery on macOS, so `udp.lidar_ip` is
//     REQUIRED there — and explicit-IP is the default path everywhere, since
//     the connect wizard asks for it anyway and "which of the two lidars on
//     this switch did I just connect to" is not a question we want the
//     network to answer for us.
//
//  3. udp_cnt free-runs and frame_cnt stays 0 on real firmware, contrary to
//     the published table. Loss detection lives in mid360_packets.h's
//     LossTracker and uses the free-running model.
//
//  4. A link drop is INVISIBLE to udp_cnt: S2 measured 0 counted losses
//     across a 15-second cable pull, three times over, because the device's
//     counter keeps advancing while the wire is down. Hence `reconnect`
//     below: a wall-clock data watchdog is the primary outage signal.
//
//  5. A power-cycled device is NEVER re-configured by the SDK. Once a handle
//     is known, HandleDetectionData() returns early forever and re-sends the
//     host-IP configuration only if the device was never configured at all,
//     so a device that forgot its configuration keeps being discovered and
//     never streams again. The driver therefore FORCES a full SDK teardown
//     and re-init after `reinit_after_silence_ms` of silence rather than
//     waiting for a self-heal that will not come.
//
// IMU (StreamId::kImu) MUST NOT ENTER THE PageStore — it is not geometry.
// Samples land in a bounded ring plus an optional sink; see `imu_sink`.
#ifndef SCANENGINE_DRIVERS_MID360_MID360_DRIVER_H
#define SCANENGINE_DRIVERS_MID360_MID360_DRIVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scanengine/drivers/driver.h"
#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/transport/udp_source.h"

namespace scanengine {

// Which layer owns the sockets.
enum class Mid360Backend : std::uint8_t {
  // The vendored SDK2 runs discovery, the handshake, the host-IP
  // configuration push and the heartbeat, and delivers point/IMU callbacks.
  // This is the only backend that can bring an out-of-the-box device up,
  // and the only one that can re-configure one after a power-cycle.
  kSdk2 = 0,

  // Listen-only UDP: bind the point and IMU ports and decode whatever
  // arrives. No handshake, no heartbeat, no ability to configure anything —
  // so it works ONLY against a device already configured to stream at this
  // host (or against a replay/injection harness). Used by the unit tests,
  // and it is the path an .lscan/lvx2 replay ingests through.
  kRawUdp = 1,

  // No transport at all: the caller pushes complete datagrams in through
  // push_bytes() (or the ingest seam directly). This is how a recorded
  // capture is replayed through the real driver — A5's .lscan Mid-360 chunks
  // are stored as unmodified datagrams precisely so they can come back this
  // way — and it is what makes the reconnect state machine testable against
  // a scripted clock with no sockets and no SDK.
  kInject = 2,
};

const char* to_string(Mid360Backend b) noexcept;

// Where the driver believes the wire is. Orthogonal to DeviceState: a
// kSilent link shows up to the app as DeviceState::kDegraded, and a link
// that stays silent through a forced re-init shows up as kFault.
enum class Mid360LinkState : std::uint8_t {
  kDown = 0,        // not started, or stopped
  kWaiting = 1,     // started; no packet has ever arrived
  kUp = 2,          // data within the watchdog window
  kSilent = 3,      // watchdog fired; waiting to see whether it self-heals
  kReinitializing = 4,  // tearing the SDK down and bringing it back up
};

const char* to_string(Mid360LinkState s) noexcept;

// Reconnect / watchdog policy. The defaults encode S2's two measured failure
// modes: a cable pull, which the SDK recovers from on its own within a
// second or two of the wire coming back, and a power-cycle, which it never
// recovers from at all.
struct Mid360ReconnectConfig {
  bool enabled = true;

  // No point packet for this long ⇒ Mid360LinkState::kSilent and
  // DeviceState::kDegraded. 1 s is ~2,083 missed packets at the nominal
  // rate, far outside any jitter S2 saw (worst case 2 ms).
  std::uint32_t data_timeout_ms = 1000;

  // Grace for the FIRST packet after start(), i.e. discovery + handshake +
  // the configuration push. Measured end to end against the S2 simulator on
  // loopback: 1.4 s. 10 s leaves room for a real switch, a device still
  // booting, and a host that has just brought its interface up — without it,
  // data_timeout_ms would trip the watchdog on every single startup.
  // Exceeding it is treated exactly like a mid-capture outage, which is
  // correct: a device that never answers needs the same forced re-init as
  // one that stopped answering.
  std::uint32_t connect_timeout_ms = 10000;

  // Silence for this long ⇒ forced SDK teardown + re-init. Sized above the
  // cable-pull recovery time S2 measured (the stream resumed 0.0–0.4 s after
  // the wire came back, every cycle) so a clean resume is never interrupted
  // by a re-init, but well inside a user's patience for a power-cycled unit.
  std::uint32_t reinit_after_silence_ms = 5000;

  // Capped exponential backoff between successive forced re-inits, so a
  // device that is simply switched off does not spin the SDK's socket setup
  // once a second for the rest of the capture.
  std::uint32_t reinit_backoff_initial_ms = 1000;
  std::uint32_t reinit_backoff_max_ms = 30000;

  // 0 = keep trying for as long as the session runs. Any positive value
  // makes the driver give up into DeviceState::kFault after that many
  // consecutive failed re-inits.
  std::uint32_t max_reinits = 0;
};

// One IMU sample, already unpacked. 200 Hz. Gyro rad/s, accel in g exactly
// as the device reports it (S2 verified mean |acc| = 1.0000 g over 120,009
// packets) — converting to m/s² is A4/A6's business, not the driver's.
struct Mid360ImuSample {
  std::int64_t t_mono_ns = 0;    // engine steady clock at arrival
  std::uint64_t t_device_ns = 0; // the device's own clock, from the packet
  float gyro[3] = {0.f, 0.f, 0.f};
  float acc[3] = {0.f, 0.f, 0.f};
};

// Optional zero-copy IMU seam for A4 (offset estimation) and A5 (the .lscan
// imu.bin chunk). Called on the SDK's receive thread, with the driver's
// locks NOT held: it must be quick and must not re-enter the engine. If
// unset, samples are still available through drain_imu().
using Mid360ImuSink = void (*)(const Mid360ImuSample* samples, std::size_t count, void* user_data);

// Raw-datagram sink: called once per received UDP datagram, BEFORE any
// parsing, filtering or decimation, on the receive thread. `is_imu` is a
// cheap header peek (kDataTypeImu), not a validity judgment — invalid
// datagrams are delivered too, so a recording preserves exactly what the
// wire carried (Tech Spec §3 record-always; chunks are replayable through
// Mid360Backend::kInject, which takes one datagram per call).
using Mid360RawSink = void (*)(const std::uint8_t* data, std::size_t len, bool is_imu,
                               std::int64_t t_arrival_ns, void* user_data);

struct Mid360Config {
  UdpConfig udp{};

  Mid360Backend backend = Mid360Backend::kSdk2;

  // No-return / tag / range policy. Defaults come from the real Livox
  // recordings in the S2 fixtures, not from the simulator — see
  // mid360_packets.h for the histogram they were chosen against.
  mid360::PointFilterConfig filter{};

  Mid360ReconnectConfig reconnect{};

  // Live decimation budget (Tech Spec §3.3: ~40k pts/s into live LIO out of
  // the sensor's 200k). 0 = no decimation (post-processing / replay).
  // Decimation happens AFTER filtering, so the kept fraction is what gets
  // thinned, and it is deterministic (every Nth surviving point) rather than
  // random, so a replay reproduces exactly.
  std::uint32_t live_points_per_sec = 40000;

  bool publish_imu = true;
  Mid360ImuSink imu_sink = nullptr;
  void* imu_sink_user_data = nullptr;

  // INT-C2C3 finding: the Engine installs a shim here so raw datagrams reach
  // the .lscan recorder (kMid360Points / kMid360Imu chunks) — the D6's
  // record-before-parse guarantee, for a driver that owns its own sockets.
  Mid360RawSink raw_sink = nullptr;
  void* raw_sink_user_data = nullptr;
  std::uint32_t imu_ring_capacity = 2048;  // ~10 s at 200 Hz

  // PageStore batch size. S3 measured 200k pts/s ingest at 0.11 ms p95 with
  // ~1 M-point pages; one 96-point packet per append would be 2,083 locks a
  // second for no reason, so points accumulate to this many first.
  std::uint32_t max_batch_points = 8192;

  // Health: how often the per-second stats snapshot is recomputed and
  // published as EventType::kDeviceHealth.
  std::uint32_t health_period_ms = 1000;

  // Sustained loss above this (over the health window, and only once
  // `loss_min_packets` have been seen) demotes to DeviceState::kDegraded.
  // 1% is 20 dropped packets a second — well past anything attributable to
  // scheduling, and the level at which a voxel map starts thinning.
  double max_loss_pct = 1.0;
  std::uint64_t loss_min_packets = 200;

  // Verify the packet CRC32 in software. The SDK already did it, so this is
  // off by default; the raw-UDP backend has nobody else to trust.
  bool verify_crc = false;

  // --- SDK2 backend only ---------------------------------------------
  //
  // SDK2's entry point takes a config FILE. If this is empty the driver
  // writes one derived from `udp` into the temp directory and deletes it on
  // stop; set it to keep the generated file somewhere inspectable, or point
  // it at a hand-written config and set `generate_sdk_config = false` (which
  // is how the loopback tests reuse the S2 spike's config verbatim).
  std::string sdk_config_path;
  bool generate_sdk_config = true;
  bool sdk_console_log = false;  // SDK2 is chatty; off unless debugging

  // The supervisor thread (watchdog + health) is the driver's own. Tests
  // turn it off and drive tick() by hand with a deterministic clock.
  bool internal_supervisor_thread = true;
};

// A snapshot of everything the health panel and the docs' soak numbers need.
// Cheap to copy; taken under one lock.
struct Mid360Stats {
  Mid360LinkState link = Mid360LinkState::kDown;
  DeviceState state = DeviceState::kDisconnected;

  std::uint64_t point_packets = 0;
  std::uint64_t imu_packets = 0;
  std::uint64_t points_received = 0;   // before filtering
  std::uint64_t points_kept = 0;       // after filtering, before decimation
  std::uint64_t points_appended = 0;   // what actually reached the PageStore
  std::uint64_t points_dropped_store = 0;  // lost to PageStore backpressure
  std::uint64_t bad_packets = 0;       // failed parse_packet()/CRC
  std::uint64_t imu_dropped = 0;       // ring overflow (nobody draining)

  std::uint64_t packets_lost = 0;      // free-running udp_cnt model
  std::uint64_t packets_duplicated = 0;
  std::uint64_t counter_resets = 0;

  mid360::FilterStats filter{};

  // Per-health-window rates (default: per second).
  double points_per_sec = 0.0;
  double points_appended_per_sec = 0.0;
  double imu_hz = 0.0;
  double loss_pct_window = 0.0;   // this window only
  double loss_pct_total = 0.0;    // since start()

  // Reconnect accounting — the two failure modes S2 separated.
  std::uint64_t watchdog_trips = 0;   // times the data watchdog fired
  std::uint64_t clean_resumes = 0;    // data came back with no re-init (cable)
  std::uint64_t forced_reinits = 0;   // full SDK teardown+init (power-cycle)
  std::uint64_t reinit_failures = 0;

  std::int64_t t_last_point_ns = 0;
  std::int64_t t_last_imu_ns = 0;
  // The SDK's 1 Hz push-state heartbeat. Independent of the data streams:
  // S2 saw it stall 1.2 s after the point stream did, and a device that
  // streams but stops heartbeating is a real, distinct condition.
  std::int64_t t_last_heartbeat_ns = 0;
  std::int64_t t_silent_since_ns = 0;  // 0 unless link != kUp

  std::string device_sn;
  std::string device_ip;
};

class Mid360BackendImpl;  // src/drivers/mid360/, one per Mid360Backend value

class Mid360Driver final : public Driver {
 public:
  Mid360Driver(DeviceId id, const Mid360Config& cfg, const DriverContext& ctx);
  ~Mid360Driver() override;

  const char* name() const override { return "mid360"; }
  DeviceKind kind() const override { return DeviceKind::kMid360; }
  DeviceId id() const override { return id_; }

  Status start() override;
  Status stop() override;
  DeviceState state() const override;
  DeviceHealth health() const override;

  // The Mid-360 owns its own sockets; the app never pushes bytes.
  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override;

  const Mid360Config& config() const { return cfg_; }
  Mid360Stats stats() const;
  Mid360LinkState link_state() const;

  // Copy out up to `max` buffered IMU samples, oldest first. Returns how
  // many were written. This is A4/A5's pull seam; `imu_sink` is the push
  // one. Samples not drained are eventually overwritten (bounded ring) and
  // counted in Mid360Stats::imu_dropped.
  std::size_t drain_imu(Mid360ImuSample* out, std::size_t max);

  // --- ingest seam ---------------------------------------------------
  //
  // Public because two very different callers need it: the SDK2 backend
  // (from the SDK's receive thread) and the unit tests (from the test
  // thread, with synthetic packets and no SDK linked). Everything below is
  // internally synchronized. `data` is one complete UDP datagram.
  void on_point_packet(const std::uint8_t* data, std::size_t len, TimePoint t_arrival);
  void on_imu_packet(const std::uint8_t* data, std::size_t len, TimePoint t_arrival);
  void on_device_connected(const char* sn, const char* ip);
  void on_heartbeat(TimePoint t);

  // One supervisor step: data watchdog, reconnect decisions, per-window
  // health. Runs on the driver's own thread when
  // `internal_supervisor_thread` is set; tests call it directly with a
  // scripted clock so the reconnect state machine is deterministic.
  void tick(TimePoint now);

 private:
  struct Window {  // per-health-window deltas
    std::int64_t t_start_ns = 0;
    std::uint64_t points = 0;
    std::uint64_t points_appended = 0;
    std::uint64_t imu = 0;
    std::uint64_t packets = 0;
    std::uint64_t lost = 0;
  };

  void set_state(DeviceState next, ScanError err);
  void set_link(Mid360LinkState next, std::int64_t t_ns);
  // Move the accumulated batch into the PageStore. Swaps buffers under m_
  // (so neither side ever reallocates) and then appends with NO driver lock
  // held: PageStore subscribers — and therefore the Engine's event publish —
  // run inline on the calling thread, and holding a driver lock across app
  // code is exactly how S2's own simulator deadlocked its handshake.
  // Serialized by flush_m_, which the receive thread and the supervisor both
  // take BEFORE m_ (one lock order, no cycle).
  void flush_points(std::int64_t t_ns);
  void publish_health(const Mid360Stats& s, std::int64_t t_ns);
  Status open_backend();
  void close_backend();
  void supervisor_loop();

  DeviceId id_;
  Mid360Config cfg_;
  DriverContext ctx_;

  mutable std::mutex m_;
  DeviceState state_ = DeviceState::kDisconnected;
  Mid360LinkState link_ = Mid360LinkState::kDown;
  ScanError last_error_ = ScanError::kOk;

  std::unique_ptr<Mid360BackendImpl> backend_;

  // Taken before m_ by anything that appends to the PageStore.
  std::mutex flush_m_;
  std::vector<PointVertex> flush_buf_;  // only under flush_m_

  // Point path (guarded by m_).
  mid360::LossTracker loss_;
  mid360::FilterStats filter_stats_;
  std::vector<PointVertex> batch_;
  std::uint32_t decimate_stride_ = 1;
  std::uint32_t decimate_phase_ = 0;  // 0..decimate_stride_-1, never wraps wrong

  // IMU ring (guarded by imu_m_ — a separate lock so a 200 Hz IMU thread
  // never contends with the 2 kHz point path).
  mutable std::mutex imu_m_;
  std::vector<Mid360ImuSample> imu_ring_;
  std::size_t imu_head_ = 0;
  std::size_t imu_size_ = 0;

  Mid360Stats st_{};
  Window window_{};

  std::int64_t t_last_point_ns_ = 0;
  std::int64_t t_last_imu_ns_ = 0;
  std::int64_t t_last_heartbeat_ns_ = 0;  // SDK info-push; a second outage signal
  std::int64_t t_silent_since_ns_ = 0;
  std::int64_t t_next_reinit_ns_ = 0;
  std::uint32_t reinit_backoff_ms_ = 0;

  std::thread supervisor_;
  std::atomic<bool> supervisor_run_{false};
  std::condition_variable supervisor_cv_;
  std::mutex supervisor_m_;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_MID360_MID360_DRIVER_H
