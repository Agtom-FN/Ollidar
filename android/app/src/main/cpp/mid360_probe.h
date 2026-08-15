// mid360_probe.h — B3's Mid-360 transport probe: a standalone
// scanengine::Engine wired to a Mid360Config this shim builds in full.
//
// WHY THIS EXISTS (the C-ABI gap, stated precisely).
// `scan_device_config`'s Mid-360 half is exactly two fields:
//
//     const char* lidar_ip;
//     const char* host_ip;
//
// and `scanengine_c.cpp` copies them into `dc.mid360.udp.{lidar_ip,host_ip}`
// and nothing else. So EVERYTHING else in `Mid360Config` is unreachable from
// the C ABI — in particular:
//
//   * `Mid360Config::backend` (kSdk2 / kRawUdp / kInject),
//   * every port in `UdpConfig` (the five device-side and five host-side),
//   * `UdpConfig::recv_buffer_bytes`,
//   * and **`UdpConfig::prebound_fd`** — the field
//     `engine/include/scanengine/transport/udp_source.h` documents, in its
//     own header comment, as "The Android seam: the app binds the socket to
//     the USB-Ethernet Network object (ConnectivityManager
//     TRANSPORT_ETHERNET + Network.bindSocket) and hands the bound
//     descriptor down, because the engine cannot reach ConnectivityManager."
//
// There is also no route from the C ABI's opaque `scan_engine*` to the C++
// `scanengine::Engine&` it wraps (`EngineHandle` is file-local to
// scanengine_c.cpp), so the pre-bound fd cannot be applied to the capture
// session's engine either. B4 hit the identical wall for
// `lscan::ReplaySource` and solved it the same way: a second, standalone
// Engine built by linking the engine's C++ API directly. This is that, for
// the Mid-360 transport.
//
// WHAT IT IS FOR — the connect wizard's checks, not capture:
//
//   * `kSdk2` — the full bring-up probe. Discovery + handshake + the 0x0100
//     host-IP configuration push, i.e. the only backend that can bring an
//     out-of-the-box device up (A3 §3). Used when there is no project yet.
//   * `kRawUdp` + `prebound_fd` — the bound-socket check. Listen-only on a
//     socket Kotlin already created, bound to the Ethernet `Network` via
//     `Network.bindSocket`, and dup'd down to us. Proves the whole
//     ConnectivityManager -> fd -> UdpSource chain carries real datagrams.
//     Works only against a device ALREADY configured to stream here.
//
// The CAPTURE path does NOT use this class: it goes through the C ABI
// (`scan_engine_add_device` with kind = SCAN_DEVICE_MID360), so Mid-360
// points land in the same engine, the same PageStore and the same `.lscan`
// recorder as everything else, and live SLAM sees them. See
// mid360_jni.cpp's `nativeAddMid360Device`.
//
// SDK2 IS A PROCESS-WIDE SINGLETON (A3 §3: "LivoxLidarSdkInit/Uninit and the
// callback registrations are global. A second kSdk2 driver gets kBusy"). A
// probe running the kSdk2 backend therefore MUST be destroyed before the
// capture engine adds its own Mid-360 device. `is_sdk2_active()` is what the
// Kotlin side gates on; the wizard also stops the probe itself before
// navigating to capture.
//
// TWO ENGINE-SIDE LIMITATIONS THIS FILE WORKS AROUND RATHER THAN HIDES:
//
//  1. `Engine` exposes no concrete-driver accessor (only
//     `device_health(DeviceId)` over the base `Driver*`), so `Mid360Stats` —
//     link state, watchdog trips, forced re-inits, window loss %, SN, device
//     IP — is not reachable even from C++. Desktop's NOTES §8.3 records the
//     same constraint. What IS reachable is `DeviceHealth`, whose Mid-360
//     mapping (A3 §5) carries the numbers that matter: `points_per_sec`,
//     `rotation_hz` = IMU Hz, `checksum_pass_rate` = 1 − lifetime loss. Link
//     state is re-derived here from wall-clock silence against A3's own
//     `data_timeout_ms` / `reinit_after_silence_ms` thresholds — the same
//     rule the driver applies internally, applied to the same observable
//     (`t_last_data_ns`), not a guess.
//  2. `UdpConfig::prebound_fd` is ONE fd, but `RawUdpBackend::open()` copies
//     the whole `UdpConfig` into both its point and its IMU `UdpSource`
//     (mid360_raw_udp.cpp), so both would `recvfrom()` the SAME socket and
//     steal each other's datagrams. The bound-socket check therefore runs
//     point-only (`publish_imu = false`) and says so in the UI. Carrying IMU
//     on a pre-bound socket needs a per-source fd in `UdpConfig`; that is an
//     engine change and is written up in android/NOTES.md rather than made.
#ifndef LIDARSCAN_JNI_MID360_PROBE_H
#define LIDARSCAN_JNI_MID360_PROBE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "scanengine/core/engine.h"
#include "scanengine/drivers/mid360/mid360_driver.h"

namespace lidarscan_jni {

// Everything the wizard's health readout needs, in one cheap copy.
// Deliberately a superset of `DeviceHealth`: the datagram counters come from
// this class's own `raw_sink`, which is the ONLY signal available when the
// device is streaming but nothing parses (wrong point port, or a device
// configured for a different host), and the difference between "no bytes at
// all" and "bytes but no points" is the difference between a cabling problem
// and a configuration problem.
struct Mid360ProbeSnapshot {
  // --- from Engine::device_health (A3 §5's mapping) ---
  std::int32_t device_state = 0;      // scanengine::DeviceState
  std::int32_t last_error = 0;        // scanengine::ScanError
  std::uint64_t packets_ok = 0;
  std::uint64_t packets_bad = 0;      // bad parses + inferred losses
  std::uint64_t points_out = 0;       // into the PageStore (post-filter/decimation)
  std::uint64_t drops = 0;
  std::uint64_t bytes_in = 0;
  double points_per_sec = 0.0;
  double imu_hz = 0.0;                // DeviceHealth::rotation_hz, per A3 §5
  double loss_pct = 0.0;              // 100 * (1 - checksum_pass_rate), lifetime
  std::int64_t t_last_data_ns = 0;

  // --- from this class's own raw_sink (pre-parse, per datagram) ---
  std::uint64_t datagrams_point = 0;
  std::uint64_t datagrams_imu = 0;
  std::uint64_t datagram_bytes = 0;
  std::int64_t t_first_datagram_ns = 0;
  std::int64_t t_last_datagram_ns = 0;

  // --- derived here (see the header note on Mid360Stats) ---
  // 0 down, 1 waiting, 2 up, 3 silent, 4 reinitializing-window.
  // Mirrors scanengine::Mid360LinkState's numbering exactly so the Kotlin
  // enum can be a straight ordinal map.
  std::int32_t link_state = 0;
  std::int64_t elapsed_since_start_ns = 0;

  bool running = false;
  std::int32_t backend = 0;  // scanengine::Mid360Backend
};

class Mid360Probe {
 public:
  Mid360Probe() = default;
  ~Mid360Probe();

  Mid360Probe(const Mid360Probe&) = delete;
  Mid360Probe& operator=(const Mid360Probe&) = delete;

  struct Params {
    std::string lidar_ip;
    std::string host_ip;
    // scanengine::Mid360Backend: 0 = kSdk2, 1 = kRawUdp. kInject (2) is not
    // offered — there is nothing on Android to inject from at this seam.
    int backend = 0;
    std::uint16_t device_point_port = 56300;
    std::uint16_t device_imu_port = 56400;
    std::uint16_t device_cmd_port = 56100;
    std::uint16_t host_point_port = 56301;
    std::uint16_t host_imu_port = 56401;
    std::uint16_t host_cmd_port = 56101;
    // Pre-bound, Network-bound socket for the kRawUdp backend, or -1.
    // OWNERSHIP: `UdpSource` never closes a pre-bound descriptor ("the app
    // owns it", udp_source.h), so this class closes it in stop(). The Kotlin
    // side hands down a `ParcelFileDescriptor.dup(...).detachFd()`, i.e. a
    // descriptor it has already given up, and keeps its own original open
    // until it tears the wizard down.
    int prebound_point_fd = -1;
    // Only meaningful with a pre-bound fd; see the header's limitation 2.
    bool publish_imu = true;
    // 0 disables decimation. The probe keeps A3's live budget so the numbers
    // it reports are the numbers a capture would see.
    std::uint32_t live_points_per_sec = 40000;
  };

  // Creates the standalone Engine, starts a non-recording session and adds
  // the Mid-360 device (which auto-starts, because the session is already
  // active — the same ordering desktop's CaptureWindow uses). Returns false
  // on failure; last_error() carries the engine's own message, which for
  // SDK2 includes the "check that host_ip is a local address and that ports
  // …/…/… are free" hint.
  bool start(const Params& params);

  // Stops the session, removes the device, destroys the Engine and closes
  // any pre-bound descriptor this probe took ownership of. Idempotent.
  void stop();

  bool running() const { return running_.load(std::memory_order_acquire); }

  // True while an SDK2-backed probe holds the process-wide SDK singleton.
  static bool is_sdk2_active();

  Mid360ProbeSnapshot snapshot() const;

  const std::string& last_error() const { return last_error_; }

 private:
  static void on_raw_datagram(const std::uint8_t* data, std::size_t len, bool is_imu,
                              std::int64_t t_arrival_ns, void* user_data);

  std::unique_ptr<scanengine::Engine> engine_;
  scanengine::DeviceId device_ = scanengine::kInvalidDeviceId;
  std::atomic<bool> running_{false};
  int owned_fd_ = -1;
  int backend_ = 0;
  bool holds_sdk2_ = false;
  std::int64_t t_start_ns_ = 0;

  mutable std::mutex m_;
  std::uint64_t datagrams_point_ = 0;
  std::uint64_t datagrams_imu_ = 0;
  std::uint64_t datagram_bytes_ = 0;
  std::int64_t t_first_datagram_ns_ = 0;
  std::int64_t t_last_datagram_ns_ = 0;

  std::string last_error_;
};

}  // namespace lidarscan_jni

#endif  // LIDARSCAN_JNI_MID360_PROBE_H
