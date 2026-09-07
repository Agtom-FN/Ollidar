// imu_serial_driver.h — JuxiTech ICM-42670-P over UART, behind Driver (A18).
//
// The Mid-70 has no built-in IMU, so a Mid-70 session pairs it with this
// device. It is a PUSH-mode serial driver in the shape of D6Driver: the app
// owns the port (transport/byte_source.h — Android cannot open /dev/ttyUSB*,
// and the Qt desktop already owns a QSerialPort) and hands chunks in through
// push_bytes(); the one thing this driver sends back is the report-rate
// command, through UsbSerialConfig::write_fn.
//
// What is different from every other driver here, and why:
//
//  1. NO PageStore. IMU is not geometry. Samples go to a bounded ring (pull,
//     drain()) and an optional sink (push), exactly like the Mid-360's IMU
//     path; the Engine's shim feeds them to ImuIngest → LIO.
//
//  2. NO device clock and NO sample counter on the wire. Arrival time is the
//     only clock, and arrivals are bursty, so every sample carries BOTH
//     `t_arrival_ns` (what the host observed, unmodified) and `t_stamped_ns`
//     (the de-bursted estimate from ImuStamper). Consumers use the stamped
//     time; the raw arrival is kept so a recording can be re-stamped later
//     with a better model without re-capturing.
//
//  3. The module STOPS TRANSMITTING for ~2.9 s roughly every 35 s. That is
//     measured firmware behaviour, not a link fault, and nothing in the
//     protocol turns it off. This driver COUNTS those blackouts, reports
//     kDegraded while one is in progress, and surfaces the worst one — it
//     never smooths them into the stream. A driver that hid this would hand
//     LIO three seconds of invented motion.
//
// Thread: when `internal_supervisor_thread` is set, start() spawns one
// supervisor thread that calls tick() every 50 ms (blackout watchdog + the
// per-window health snapshot). Tests turn it off and call tick() by hand with
// a scripted clock, which is the only way the blackout state machine is
// deterministic. The byte path itself runs entirely on the caller's thread.
//
// Owner: A18 (serial IMU).
#ifndef SCANENGINE_DRIVERS_IMU_SERIAL_IMU_SERIAL_DRIVER_H
#define SCANENGINE_DRIVERS_IMU_SERIAL_IMU_SERIAL_DRIVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "scanengine/drivers/driver.h"
#include "scanengine/drivers/imu_serial/imu_stamper.h"
#include "scanengine/drivers/imu_serial/juxi_frames.h"
#include "scanengine/transport/usb_serial_source.h"

namespace scanengine {

// One IMU sample. Gyro rad/s and accel in g, exactly as the module reports
// them (the g → m/s² conversion is ImuIngest's business — same rule as
// Mid360ImuSample). Magnetometer is decoded by the parser but not carried
// here: nothing downstream consumes a heading.
struct ImuSerialSample {
  std::int64_t t_arrival_ns = 0;  // host arrival, unmodified
  std::int64_t t_stamped_ns = 0;  // de-bursted (ImuStamper); use this one
  float gyro_rad_s[3] = {0.f, 0.f, 0.f};
  float accel_g[3] = {0.f, 0.f, 0.f};
};

// Push seam, the Mid360ImuSink shape. Called on the thread that pushed the
// bytes, with NO driver lock held: must be quick and must not re-enter the
// driver.
using ImuSerialSink = void (*)(const ImuSerialSample* samples, std::size_t count,
                               void* user_data);

struct ImuSerialConfig {
  UsbSerialConfig serial{};  // 115200 8N1; the app owns the port

  // Sent once at start() as 7E 23 07 60 <hz> 5F <sum8>. The module persists
  // it, so this is idempotent across runs. 10..100 Hz.
  std::uint8_t report_rate_hz = 100;
  bool send_rate_command = true;

  // Silence longer than this is a blackout: counted, published as kDegraded,
  // never smoothed. Sized under the module's own ~2.9 s stall so that stall
  // is always seen, and over any plausible scheduling hiccup.
  double blackout_threshold_s = 0.5;

  // Below this, over at least `health_min_frames` frames, the device is
  // kDegraded. Frames are 23 bytes with an 8-bit checksum on a short 115200
  // link: real failures are rare, and a rate under 0.99 means the wire or the
  // baud rate is wrong, not that a byte got unlucky.
  double min_checksum_pass_rate = 0.99;
  std::uint64_t health_min_frames = 200;

  ImuSerialSink sink = nullptr;
  void* sink_user_data = nullptr;

  std::uint32_t ring_capacity = 2048;   // ~20 s at 100 Hz
  std::uint32_t health_period_ms = 1000;

  // Timing model. `nominal_hz` is overwritten from report_rate_hz at
  // construction — the stamper's seed and the rate we asked the module for
  // must not be able to disagree — the other knobs are as the ROS driver
  // validated them.
  ImuStamperConfig stamper{};

  // See the thread note in the file header. Tests set this false and drive
  // tick() themselves.
  bool internal_supervisor_thread = true;
};

struct ImuSerialStats {
  DeviceState state = DeviceState::kDisconnected;

  juxi::FrameStats frames{};  // per-func counters, checksum failures, resyncs

  std::uint64_t samples = 0;       // published ImuSerialSamples (== raw frames)
  std::uint64_t samples_dropped = 0;  // ring overflow (nobody draining)
  double rate_hz = 0.0;            // measured over the last health window

  // The module's firmware stall. `blackout_in_progress` is true from the
  // moment the watchdog notices until data resumes.
  std::uint64_t blackouts = 0;
  std::int64_t worst_blackout_ns = 0;
  bool blackout_in_progress = false;

  std::int64_t t_last_sample_ns = 0;
  std::int64_t t_last_bytes_ns = 0;

  ImuStamperStats stamper{};
};

class ImuSerialDriver final : public Driver {
 public:
  ImuSerialDriver(DeviceId id, const ImuSerialConfig& cfg, const DriverContext& ctx);
  ~ImuSerialDriver() override;

  const char* name() const override { return "imu-serial"; }
  DeviceKind kind() const override { return DeviceKind::kImuSerial; }
  DeviceId id() const override { return id_; }

  Status start() override;
  Status stop() override;
  DeviceState state() const override;
  DeviceHealth health() const override;
  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override;

  const ImuSerialConfig& config() const { return cfg_; }
  ImuSerialStats stats() const;

  // Copy out up to `max` buffered samples, oldest first; returns how many.
  // Samples not drained are eventually overwritten (bounded ring) and counted
  // in ImuSerialStats::samples_dropped.
  std::size_t drain(ImuSerialSample* out, std::size_t max);

  // One supervisor step: blackout watchdog + per-window health. Runs on the
  // driver's own thread when `internal_supervisor_thread` is set; tests call
  // it directly with a scripted clock.
  void tick(TimePoint now);

 private:
  void on_bytes(ByteSpan bytes, TimePoint t);
  void on_raw_frame(const juxi::RawImuFrame& f, std::int64_t t_arrival_ns);
  void set_state(DeviceState next, ScanError err);
  void refresh_state();
  void publish_health(const ImuSerialStats& s, std::int64_t t_ns);
  void supervisor_loop();
  TimePoint current_time() const;

  DeviceId id_;
  ImuSerialConfig cfg_;
  DriverContext ctx_;

  std::unique_ptr<UsbSerialSource> serial_;
  juxi::FrameParser parser_;  // byte-path thread only
  ImuStamper stamper_;        // byte-path thread only

  mutable std::mutex m_;
  DeviceState state_ = DeviceState::kDisconnected;
  ScanError last_error_ = ScanError::kOk;
  bool started_ = false;
  bool saw_first_sample_ = false;
  bool degraded_blackout_ = false;
  bool degraded_checksum_ = false;
  juxi::FrameStats frames_{};  // last snapshot copied out of parser_

  std::uint64_t samples_ = 0;
  std::uint64_t samples_dropped_ = 0;
  std::uint64_t blackouts_ = 0;
  std::int64_t worst_blackout_ns_ = 0;
  std::int64_t t_last_sample_ns_ = 0;
  std::int64_t t_last_bytes_ns_ = 0;
  std::int64_t t_start_ns_ = 0;
  double rate_hz_ = 0.0;

  // Per-health-window deltas.
  std::int64_t window_start_ns_ = 0;
  std::uint64_t window_samples_ = 0;

  // Sample ring (its own lock: a 100 Hz producer must never contend with a
  // consumer polling stats).
  mutable std::mutex ring_m_;
  std::vector<ImuSerialSample> ring_;
  std::size_t ring_head_ = 0;
  std::size_t ring_size_ = 0;

  std::thread supervisor_;
  std::atomic<bool> supervisor_run_{false};
  std::condition_variable supervisor_cv_;
  std::mutex supervisor_m_;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_IMU_SERIAL_IMU_SERIAL_DRIVER_H
