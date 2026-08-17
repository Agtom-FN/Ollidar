// types.h — small vocabulary types shared by every engine module.
//
// Everything here is trivially copyable and C-ABI mirrorable: these values
// appear in events, in device health, and in .lscan chunk headers, so their
// numeric values are STABLE and APPEND-ONLY exactly like ScanError.
//
// Owner: A1.
#ifndef SCANENGINE_CORE_TYPES_H
#define SCANENGINE_CORE_TYPES_H

#include <cstdint>

#include "scanengine/core/error.h"

namespace scanengine {

// Handle for a device registered with the engine. 0 is never valid.
using DeviceId = std::uint32_t;
inline constexpr DeviceId kInvalidDeviceId = 0;

// Handle for a point page in the cloud/PageStore. 0 is never valid.
using PageId = std::uint32_t;
inline constexpr PageId kInvalidPageId = 0;

enum class DeviceKind : std::uint8_t {
  kUnknown = 0,
  kD6 = 1,       // COIN-D6 2D lidar over UART (drivers/d6)   — A2
  kMid360 = 2,   // Livox Mid-360 over UDP     (drivers/mid360) — A3
  kRtkRover = 3, // NMEA/RTCM3 GNSS rover      (gnss/)          — A10
};

const char* to_string(DeviceKind k) noexcept;

// Logical data streams. These values are also the .lscan stream ids
// (record/lscan.h) — do not renumber.
enum class StreamId : std::uint8_t {
  kUnknown = 0,
  kLidarD6 = 1,      // raw D6 UART bytes
  kLidarMid360 = 2,  // Mid-360 point packets
  kImu = 3,
  kPoseAr = 4,       // ARCore VIO poses
  kGnss = 5,         // NMEA sentences + fix state
  kCameraFrames = 6, // keyframe index (JPEGs live beside it)
  kPoseFused = 7,    // output of the pose fusion layer
  kSlamMap = 8,      // registered world-frame map points (A6 output)
  kPoseLio = 9,      // LIO pose track
  // ROUND 9 (additive): the PHONE's own gyro/accel, at 200-400 Hz. Deliberately
  // NOT kImu, which is the Mid-360's SDK2 IMU datagram stream — d6_resolve.cpp
  // and post_pipeline.cpp both read a non-empty kImu summary as evidence that a
  // container is a Mid-360 project, so a phone sample landing there would
  // misroute the offline pipeline. Same argument record/lscan.h makes for
  // kSlamMap. Stamps are CLOCK_BOOTTIME, i.e. already engine time.
  kImuPhone = 10,
};

const char* to_string(StreamId s) noexcept;

// Device lifecycle as observed by the app. A driver's health() reports this
// and every transition raises an EventType::kDeviceState event.
//
//   kDisconnected --open--> kIdle --start--> kStarting --data--> kStreaming
//        ^                                                          |
//        +----------------- kStopping <-----------stop--------------+
//   any state --failure--> kFault (terminal until stop()/re-add)
//   kStreaming --stalled/lossy--> kDegraded --recovered--> kStreaming
enum class DeviceState : std::uint8_t {
  kDisconnected = 0,
  kIdle = 1,
  kStarting = 2,
  kStreaming = 3,
  kDegraded = 4,  // still producing, but below spec (loss, checksum, stall)
  kStopping = 5,
  kFault = 6,
};

const char* to_string(DeviceState s) noexcept;

// One snapshot of a driver's health. Cheap to copy; polled by app UIs
// (Android health panel, Qt capture window) and mirrored across the C ABI.
struct DeviceHealth {
  DeviceId id = kInvalidDeviceId;
  DeviceKind kind = DeviceKind::kUnknown;
  DeviceState state = DeviceState::kDisconnected;
  ScanError last_error = ScanError::kOk;

  std::uint64_t bytes_in = 0;
  std::uint64_t packets_ok = 0;
  std::uint64_t packets_bad = 0;   // checksum + malformed
  std::uint64_t points_out = 0;
  std::uint64_t drops = 0;         // points/pages dropped by backpressure

  double points_per_sec = 0.0;
  double rotation_hz = 0.0;        // D6 revolutions or Mid-360 frame rate
  double checksum_pass_rate = 0.0; // 0..1; S1 exit criterion is > 0.995

  std::int64_t t_last_data_ns = 0; // 0 = no data yet
};

// Engine lifecycle.
//
//   kIdle --start_session--> kStarting --> kRunning --stop_session-->
//   kStopping --> kIdle;  any --fatal--> kFaulted --stop_session--> kIdle
enum class EngineState : std::uint8_t {
  kIdle = 0,
  kStarting = 1,
  kRunning = 2,
  kStopping = 3,
  kFaulted = 4,
};

const char* to_string(EngineState s) noexcept;

}  // namespace scanengine

#endif  // SCANENGINE_CORE_TYPES_H
