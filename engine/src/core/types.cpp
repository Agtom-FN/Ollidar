#include "scanengine/core/types.h"

namespace scanengine {

const char* to_string(DeviceKind k) noexcept {
  switch (k) {
    case DeviceKind::kUnknown: return "unknown";
    case DeviceKind::kD6: return "coin-d6";
    case DeviceKind::kMid360: return "livox-mid360";
    case DeviceKind::kRtkRover: return "rtk-rover";
  }
  return "?";
}

const char* to_string(StreamId s) noexcept {
  switch (s) {
    case StreamId::kUnknown: return "unknown";
    case StreamId::kLidarD6: return "lidar-d6";
    case StreamId::kLidarMid360: return "lidar-mid360";
    case StreamId::kImu: return "imu";
    case StreamId::kPoseAr: return "pose-ar";
    case StreamId::kGnss: return "gnss";
    case StreamId::kCameraFrames: return "camera-frames";
    case StreamId::kPoseFused: return "pose-fused";
  }
  return "?";
}

const char* to_string(DeviceState s) noexcept {
  switch (s) {
    case DeviceState::kDisconnected: return "disconnected";
    case DeviceState::kIdle: return "idle";
    case DeviceState::kStarting: return "starting";
    case DeviceState::kStreaming: return "streaming";
    case DeviceState::kDegraded: return "degraded";
    case DeviceState::kStopping: return "stopping";
    case DeviceState::kFault: return "fault";
  }
  return "?";
}

const char* to_string(EngineState s) noexcept {
  switch (s) {
    case EngineState::kIdle: return "idle";
    case EngineState::kStarting: return "starting";
    case EngineState::kRunning: return "running";
    case EngineState::kStopping: return "stopping";
    case EngineState::kFaulted: return "faulted";
  }
  return "?";
}

}  // namespace scanengine
