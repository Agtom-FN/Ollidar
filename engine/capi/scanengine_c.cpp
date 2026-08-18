// scanengine_c.cpp — the C ABI implementation.
//
// Three jobs, and nothing else: (1) static_assert that the C mirror of every
// enum still matches the C++ one, (2) convert POD structs field by field —
// never by reinterpret_cast, so the two layouts are free to diverge, (3)
// stop every exception at the boundary.
#include "scanengine_c.h"

#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>

#include "scanengine/color/clock_sweep.h"
#include "scanengine/color/colorizer.h"
#include "scanengine/color/frames_idx.h"
#include "scanengine/core/engine.h"
#include "scanengine/core/instance_guard.h"
#include "scanengine/discovery/discovery.h"
#include "scanengine/jobs/job_types.h"
#include "scanengine/slam/post/d6_resolve.h"
#include "scanengine/slam/post/reprocess.h"
#include "scanengine/slam/pushbroom/mount_calibration.h"

using namespace scanengine;

// --- enum mirror checks ------------------------------------------------------
#define SCAN_CHECK_ENUM(c_value, cpp_value) \
  static_assert(static_cast<std::int32_t>(c_value) == static_cast<std::int32_t>(cpp_value), \
                "C ABI enum drifted from the C++ enum: " #c_value)

SCAN_CHECK_ENUM(SCAN_OK, ScanError::kOk);
SCAN_CHECK_ENUM(SCAN_ERR_UNKNOWN, ScanError::kUnknown);
SCAN_CHECK_ENUM(SCAN_ERR_INVALID_ARGUMENT, ScanError::kInvalidArgument);
SCAN_CHECK_ENUM(SCAN_ERR_INVALID_STATE, ScanError::kInvalidState);
SCAN_CHECK_ENUM(SCAN_ERR_NOT_FOUND, ScanError::kNotFound);
SCAN_CHECK_ENUM(SCAN_ERR_ALREADY_EXISTS, ScanError::kAlreadyExists);
SCAN_CHECK_ENUM(SCAN_ERR_NOT_SUPPORTED, ScanError::kNotSupported);
SCAN_CHECK_ENUM(SCAN_ERR_UNIMPLEMENTED, ScanError::kUnimplemented);
SCAN_CHECK_ENUM(SCAN_ERR_OUT_OF_MEMORY, ScanError::kOutOfMemory);
SCAN_CHECK_ENUM(SCAN_ERR_CANCELLED, ScanError::kCancelled);
SCAN_CHECK_ENUM(SCAN_ERR_TIMEOUT, ScanError::kTimeout);
SCAN_CHECK_ENUM(SCAN_ERR_BUSY, ScanError::kBusy);
SCAN_CHECK_ENUM(SCAN_ERR_AGAIN, ScanError::kAgain);
SCAN_CHECK_ENUM(SCAN_ERR_CAPACITY_EXCEEDED, ScanError::kCapacityExceeded);
SCAN_CHECK_ENUM(SCAN_ERR_IO, ScanError::kIoError);
SCAN_CHECK_ENUM(SCAN_ERR_DISCONNECTED, ScanError::kDisconnected);
SCAN_CHECK_ENUM(SCAN_ERR_PERMISSION_DENIED, ScanError::kPermissionDenied);
SCAN_CHECK_ENUM(SCAN_ERR_NETWORK, ScanError::kNetworkError);
SCAN_CHECK_ENUM(SCAN_ERR_DEVICE_NOT_RESPONDING, ScanError::kDeviceNotResponding);
SCAN_CHECK_ENUM(SCAN_ERR_PROTOCOL, ScanError::kProtocolError);
SCAN_CHECK_ENUM(SCAN_ERR_CHECKSUM, ScanError::kChecksumFailed);
SCAN_CHECK_ENUM(SCAN_ERR_DEVICE_FAULT, ScanError::kDeviceFault);
SCAN_CHECK_ENUM(SCAN_ERR_CORRUPT_DATA, ScanError::kCorruptData);
SCAN_CHECK_ENUM(SCAN_ERR_VERSION_MISMATCH, ScanError::kVersionMismatch);
SCAN_CHECK_ENUM(SCAN_ERR_FILE, ScanError::kFileError);

SCAN_CHECK_ENUM(SCAN_DEVICE_D6, DeviceKind::kD6);
SCAN_CHECK_ENUM(SCAN_DEVICE_MID360, DeviceKind::kMid360);
SCAN_CHECK_ENUM(SCAN_DEVICE_RTK_ROVER, DeviceKind::kRtkRover);

SCAN_CHECK_ENUM(SCAN_DEV_DISCONNECTED, DeviceState::kDisconnected);
SCAN_CHECK_ENUM(SCAN_DEV_STREAMING, DeviceState::kStreaming);
SCAN_CHECK_ENUM(SCAN_DEV_FAULT, DeviceState::kFault);

SCAN_CHECK_ENUM(SCAN_ENGINE_IDLE, EngineState::kIdle);
SCAN_CHECK_ENUM(SCAN_ENGINE_RUNNING, EngineState::kRunning);
SCAN_CHECK_ENUM(SCAN_ENGINE_FAULTED, EngineState::kFaulted);

SCAN_CHECK_ENUM(SCAN_STREAM_LIDAR_D6, StreamId::kLidarD6);
SCAN_CHECK_ENUM(SCAN_STREAM_LIDAR_MID360, StreamId::kLidarMid360);
SCAN_CHECK_ENUM(SCAN_STREAM_IMU, StreamId::kImu);
SCAN_CHECK_ENUM(SCAN_STREAM_POSE_AR, StreamId::kPoseAr);
SCAN_CHECK_ENUM(SCAN_STREAM_GNSS, StreamId::kGnss);
SCAN_CHECK_ENUM(SCAN_STREAM_CAMERA_FRAMES, StreamId::kCameraFrames);
SCAN_CHECK_ENUM(SCAN_STREAM_POSE_FUSED, StreamId::kPoseFused);
SCAN_CHECK_ENUM(SCAN_STREAM_SLAM_MAP, StreamId::kSlamMap);
SCAN_CHECK_ENUM(SCAN_STREAM_POSE_LIO, StreamId::kPoseLio);
SCAN_CHECK_ENUM(SCAN_STREAM_IMU_PHONE, StreamId::kImuPhone);

// A8's three new enums. The drift guard matters more here than usual: the
// gates are what Tech Spec §3.3's "flagged and excluded by default" and
// WIZARD.md's accept/reject bands are decided on, so a silent renumbering
// would turn a rejected calibration into a good one.
SCAN_CHECK_ENUM(SCAN_POSE_QUALITY_INVALID, PoseQuality::kInvalid);
SCAN_CHECK_ENUM(SCAN_POSE_QUALITY_POOR, PoseQuality::kPoor);
SCAN_CHECK_ENUM(SCAN_POSE_QUALITY_FAIR, PoseQuality::kFair);
SCAN_CHECK_ENUM(SCAN_POSE_QUALITY_GOOD, PoseQuality::kGood);

SCAN_CHECK_ENUM(SCAN_POSE_GATE_OK, PoseGate::kOk);
SCAN_CHECK_ENUM(SCAN_POSE_GATE_NO_DATA, PoseGate::kNoData);
SCAN_CHECK_ENUM(SCAN_POSE_GATE_BEFORE_FIRST, PoseGate::kBeforeFirst);
SCAN_CHECK_ENUM(SCAN_POSE_GATE_FUTURE, PoseGate::kFuture);
SCAN_CHECK_ENUM(SCAN_POSE_GATE_STALE, PoseGate::kStale);
SCAN_CHECK_ENUM(SCAN_POSE_GATE_TRACKING_LOST, PoseGate::kTrackingLost);
SCAN_CHECK_ENUM(SCAN_POSE_GATE_LOW_CONFIDENCE, PoseGate::kLowConfidence);

SCAN_CHECK_ENUM(SCAN_CALIB_GATE_UNKNOWN, CalibGate::kUnknown);
SCAN_CHECK_ENUM(SCAN_CALIB_GATE_GOOD, CalibGate::kGood);
SCAN_CHECK_ENUM(SCAN_CALIB_GATE_USABLE, CalibGate::kUsable);
SCAN_CHECK_ENUM(SCAN_CALIB_GATE_REJECT, CalibGate::kReject);

// A10's two new enums. FixType earns the guard as much as the pose gates do:
// §3.4's capture gate is an ORDERED comparison ("at or above RTK Float"), so
// inserting a state in the middle without moving this mirror would silently
// change which captures the app allows.
SCAN_CHECK_ENUM(SCAN_FIX_NONE, FixType::kNone);
SCAN_CHECK_ENUM(SCAN_FIX_SINGLE, FixType::kSingle);
SCAN_CHECK_ENUM(SCAN_FIX_DGPS, FixType::kDgps);
SCAN_CHECK_ENUM(SCAN_FIX_RTK_FLOAT, FixType::kRtkFloat);
SCAN_CHECK_ENUM(SCAN_FIX_RTK_FIXED, FixType::kRtkFixed);

SCAN_CHECK_ENUM(SCAN_NTRIP_IDLE, NtripState::kIdle);
SCAN_CHECK_ENUM(SCAN_NTRIP_CONNECTING, NtripState::kConnecting);
SCAN_CHECK_ENUM(SCAN_NTRIP_STREAMING, NtripState::kStreaming);
SCAN_CHECK_ENUM(SCAN_NTRIP_STALLED, NtripState::kStalled);
SCAN_CHECK_ENUM(SCAN_NTRIP_RECONNECTING, NtripState::kReconnecting);
SCAN_CHECK_ENUM(SCAN_NTRIP_FAILED, NtripState::kFailed);

SCAN_CHECK_ENUM(SCAN_TRAJECTORY_EXTERNAL, TrajectorySource::kExternal);
SCAN_CHECK_ENUM(SCAN_TRAJECTORY_GNSS, TrajectorySource::kGnss);

// ABI 5. The device config now CASTS an int32 straight to this enum, so a
// value inserted in the middle of Mid360Backend would silently select a
// different backend for every existing caller — exactly the class of drift
// this macro exists for.
SCAN_CHECK_ENUM(SCAN_MID360_BACKEND_SDK2, Mid360Backend::kSdk2);
SCAN_CHECK_ENUM(SCAN_MID360_BACKEND_RAW_UDP, Mid360Backend::kRawUdp);
SCAN_CHECK_ENUM(SCAN_MID360_BACKEND_INJECT, Mid360Backend::kInject);

SCAN_CHECK_ENUM(SCAN_MID360_LINK_DOWN, Mid360LinkState::kDown);
SCAN_CHECK_ENUM(SCAN_MID360_LINK_WAITING, Mid360LinkState::kWaiting);
SCAN_CHECK_ENUM(SCAN_MID360_LINK_UP, Mid360LinkState::kUp);
SCAN_CHECK_ENUM(SCAN_MID360_LINK_SILENT, Mid360LinkState::kSilent);
SCAN_CHECK_ENUM(SCAN_MID360_LINK_REINITIALIZING, Mid360LinkState::kReinitializing);

SCAN_CHECK_ENUM(SCAN_LOG_TRACE, LogLevel::kTrace);
SCAN_CHECK_ENUM(SCAN_LOG_ERROR, LogLevel::kError);
SCAN_CHECK_ENUM(SCAN_LOG_OFF, LogLevel::kOff);

SCAN_CHECK_ENUM(SCAN_EVENT_EVENTS_DROPPED, EventType::kEventsDropped);
SCAN_CHECK_ENUM(SCAN_EVENT_ENGINE_STATE, EventType::kEngineState);
SCAN_CHECK_ENUM(SCAN_EVENT_SESSION_STATE, EventType::kSessionState);
SCAN_CHECK_ENUM(SCAN_EVENT_DEVICE_STATE, EventType::kDeviceState);
SCAN_CHECK_ENUM(SCAN_EVENT_DEVICE_HEALTH, EventType::kDeviceHealth);
SCAN_CHECK_ENUM(SCAN_EVENT_POINTS_AVAILABLE, EventType::kPointsAvailable);
SCAN_CHECK_ENUM(SCAN_EVENT_ROTATION, EventType::kRotation);
SCAN_CHECK_ENUM(SCAN_EVENT_POSE_UPDATE, EventType::kPoseUpdate);
SCAN_CHECK_ENUM(SCAN_EVENT_GNSS_FIX, EventType::kGnssFix);
SCAN_CHECK_ENUM(SCAN_EVENT_NTRIP_STATE, EventType::kNtripState);
SCAN_CHECK_ENUM(SCAN_EVENT_GEOREF_CONVERGED, EventType::kGeorefConverged);
SCAN_CHECK_ENUM(SCAN_EVENT_JOB_PROGRESS, EventType::kJobProgress);
SCAN_CHECK_ENUM(SCAN_EVENT_ERROR, EventType::kError);

// --- ABI 4: A11 colorization + A15 jobs -------------------------------------
//
// SyncQuality earns its guard the way FixType does: policy_for() switches on
// it and the DEFAULT (kUnknown) is the one that REFUSES, so a value drifting
// by one would turn "not converged, do not colorize" into "colorize".
SCAN_CHECK_ENUM(SCAN_SYNC_UNKNOWN, SyncQuality::kUnknown);
SCAN_CHECK_ENUM(SCAN_SYNC_GOOD, SyncQuality::kGood);
SCAN_CHECK_ENUM(SCAN_SYNC_GATED, SyncQuality::kGated);
SCAN_CHECK_ENUM(SCAN_SYNC_POOR, SyncQuality::kPoor);

SCAN_CHECK_ENUM(SCAN_COVERAGE_NONE, color::ColorCoverage::kNone);
SCAN_CHECK_ENUM(SCAN_COVERAGE_COLORIZED, color::ColorCoverage::kColorized);
SCAN_CHECK_ENUM(SCAN_COVERAGE_LOW_CONFIDENCE, color::ColorCoverage::kLowConfidence);

SCAN_CHECK_ENUM(SCAN_KEYFRAME_POSE_CAMERA, color::KeyframePoseFrame::kCamera);
SCAN_CHECK_ENUM(SCAN_KEYFRAME_POSE_LIDAR_BODY, color::KeyframePoseFrame::kLidarBody);

SCAN_CHECK_ENUM(SCAN_KEYFRAME_MOTION_VALID, kKeyframeFlagMotionValid);
SCAN_CHECK_ENUM(SCAN_KEYFRAME_EXPOSURE_VALID, kKeyframeFlagExposureValid);
SCAN_CHECK_ENUM(SCAN_KEYFRAME_TRACKING_LOST, kKeyframeFlagTrackingLost);
SCAN_CHECK_ENUM(SCAN_KEYFRAME_AUTO_EXPOSURE_LOCKED, kKeyframeFlagAutoExposureLocked);

SCAN_CHECK_ENUM(SCAN_SWEEP_ACCEPTED, color::ClockSweepVerdict::kAccepted);
SCAN_CHECK_ENUM(SCAN_SWEEP_TOO_SHORT, color::ClockSweepVerdict::kTooShort);
SCAN_CHECK_ENUM(SCAN_SWEEP_TOO_FEW_SAMPLES, color::ClockSweepVerdict::kTooFewSamples);
SCAN_CHECK_ENUM(SCAN_SWEEP_NO_MOTION, color::ClockSweepVerdict::kNoMotion);
SCAN_CHECK_ENUM(SCAN_SWEEP_NO_SWEEP, color::ClockSweepVerdict::kNoSweep);
SCAN_CHECK_ENUM(SCAN_SWEEP_WEAK_CORRELATION, color::ClockSweepVerdict::kWeakCorrelation);
SCAN_CHECK_ENUM(SCAN_SWEEP_AMBIGUOUS, color::ClockSweepVerdict::kAmbiguous);
SCAN_CHECK_ENUM(SCAN_SWEEP_AT_SEARCH_EDGE, color::ClockSweepVerdict::kAtSearchEdge);

// jobs::JobState, NOT the top-level scanengine::JobState: the payload
// scan_event.payload.job.state carries is the one JobQueue publishes, which is
// A15's five-value encoding (docs/A15-jobs.md §3). The two enums coexist on
// purpose — see jobs/job_runner_adapter.h.
SCAN_CHECK_ENUM(SCAN_JOB_QUEUED, jobs::JobState::kQueued);
SCAN_CHECK_ENUM(SCAN_JOB_RUNNING, jobs::JobState::kRunning);
SCAN_CHECK_ENUM(SCAN_JOB_CANCELLING, jobs::JobState::kCancelling);
SCAN_CHECK_ENUM(SCAN_JOB_DONE, jobs::JobState::kDone);
SCAN_CHECK_ENUM(SCAN_JOB_FAILED, jobs::JobState::kFailed);

SCAN_CHECK_ENUM(SCAN_PAGE_UPDATE_APPENDED, PageUpdateKind::kAppended);
SCAN_CHECK_ENUM(SCAN_PAGE_UPDATE_RECOLOURED, PageUpdateKind::kRecoloured);
SCAN_CHECK_ENUM(SCAN_PAGE_UPDATE_EVICTED, PageUpdateKind::kEvicted);

static_assert(sizeof(scan_point_vertex) == sizeof(PointVertex),
              "scan_point_vertex must match PointVertex (16 B, S3-proven layout)");
static_assert(sizeof(scan_point_vertex) == 16, "scan_point_vertex must be 16 bytes");
static_assert(SCAN_ABI_VERSION == kEngineAbiVersion, "ABI version mismatch");

// --- ABI 6: A16 discovery ----------------------------------------------------
//
// The discovery mirrors carry no enums, so the drift that matters here is
// NUMERIC: a C consumer that binds SCAN_MID360_PUSH_PORT itself (an Android
// app with a pre-bound socket does exactly that — android/NOTES.md §8 finding
// 1) must not end up on a different port from the engine's own listener.
static_assert(SCAN_MID360_PUSH_PORT == discovery::kMid360PushPort,
              "C ABI drifted from discovery::kMid360PushPort");
static_assert(SCAN_MID360_PUSH_PORT_ALT == discovery::kMid360PushPortAlt,
              "C ABI drifted from discovery::kMid360PushPortAlt");
// The C mirrors are fixed-size buffers over std::strings. These bounds are
// the contract "a real value is never truncated": an IPv4 text is at most 15
// characters, a Livox SN 14, a MAC 17, and the note is built into a 512-byte
// buffer on the C++ side.
static_assert(sizeof(scan_mid360_beacon{}.lidar_ip) >= 16, "IPv4 text needs 16 bytes");
static_assert(sizeof(scan_mid360_beacon{}.sn) >= 24, "a Livox SN must fit untruncated");
static_assert(sizeof(scan_host_check_result{}.note) >= 512, "the note must fit untruncated");

namespace {

// The public handle is this wrapper, not scanengine::Engine, so the C ABI can
// carry ABI-only state (the pushed event callback) without touching the C++
// API's shape.
// Adapts the C write callback (returns int32_t) to the C++ one (returns
// ScanError). A trampoline rather than a reinterpret_cast of the function
// pointer: calling through an incompatible function-pointer type is UB even
// when the return types happen to be the same size.
struct SerialShim {
  scan_serial_write_cb cb = nullptr;
  void* user = nullptr;

  static ScanError write(const std::uint8_t* data, std::size_t len, void* user) {
    auto* s = static_cast<SerialShim*>(user);
    if (s == nullptr || s->cb == nullptr) return ScanError::kNotSupported;
    return static_cast<ScanError>(s->cb(data, len, s->user));
  }
};

struct EngineHandle {
  std::unique_ptr<Engine> engine;
  scan_event_cb event_cb = nullptr;
  void* event_user = nullptr;
  // Owned for the engine's whole lifetime: a driver keeps the raw pointer.
  std::vector<std::unique_ptr<SerialShim>> serial_shims;
};

EngineHandle* handle_of(scan_engine* e) { return reinterpret_cast<EngineHandle*>(e); }

// The colorizer handle is a wrapper, not a bare PointColorizer, because the C
// progress callback is (fn, user_data) and the C++ one is a std::function —
// the pair has to live somewhere with the same lifetime as the colorizer.
struct ColorizerHandle {
  explicit ColorizerHandle(const color::ColorizeConfig& cfg) : impl(cfg) {}
  color::PointColorizer impl;
  scan_colorize_progress_cb progress_cb = nullptr;
  void* progress_user = nullptr;
};

ColorizerHandle* colorizer_of(scan_colorizer* c) {
  return reinterpret_cast<ColorizerHandle*>(c);
}

scan_error_t to_c(ScanError e) { return static_cast<scan_error_t>(e); }
scan_error_t to_c(Status s) { return static_cast<scan_error_t>(s.error()); }

scan_error_t fail(ScanError e, const char* what) {
  return to_c(set_last_error(e, "%s", what));
}

// Field-by-field, never a memcpy of the whole struct: the C union and the
// C++ union are allowed to have different padding.
void convert_event(const Event& in, scan_event* out) {
  std::memset(out, 0, sizeof(*out));
  out->type = static_cast<std::uint16_t>(in.type);
  out->sequence = in.sequence;
  out->t_mono_ns = in.t_mono_ns;

  switch (in.type) {
    case EventType::kEventsDropped:
      out->payload.dropped.count = in.payload.dropped.count;
      out->payload.dropped.total = in.payload.dropped.total;
      break;
    case EventType::kEngineState:
      out->payload.engine_state.state = static_cast<std::uint8_t>(in.payload.engine_state.state);
      out->payload.engine_state.previous =
          static_cast<std::uint8_t>(in.payload.engine_state.previous);
      break;
    case EventType::kSessionState:
      out->payload.session.recording = in.payload.session.recording;
      out->payload.session.session_id = in.payload.session.session_id;
      out->payload.session.bytes_written = in.payload.session.bytes_written;
      break;
    case EventType::kDeviceState:
      out->payload.device.device = in.payload.device.device;
      out->payload.device.kind = static_cast<std::uint8_t>(in.payload.device.kind);
      out->payload.device.state = static_cast<std::uint8_t>(in.payload.device.state);
      out->payload.device.previous = static_cast<std::uint8_t>(in.payload.device.previous);
      out->payload.device.error = static_cast<std::int32_t>(in.payload.device.error);
      break;
    case EventType::kPointsAvailable:
      out->payload.points.page = in.payload.points.page;
      out->payload.points.first = in.payload.points.first;
      out->payload.points.count = in.payload.points.count;
      out->payload.points.stream = static_cast<std::uint8_t>(in.payload.points.stream);
      out->payload.points.page_created = in.payload.points.page_created;
      out->payload.points.update_kind = in.payload.points.update_kind;
      break;
    case EventType::kRotation:
      out->payload.rotation.device = in.payload.rotation.device;
      out->payload.rotation.rotation_index = in.payload.rotation.rotation_index;
      out->payload.rotation.points_in_rotation = in.payload.rotation.points_in_rotation;
      out->payload.rotation.rotation_hz = in.payload.rotation.rotation_hz;
      break;
    case EventType::kError:
      out->payload.error.error = static_cast<std::int32_t>(in.payload.error.error);
      out->payload.error.device = in.payload.error.device;
      out->payload.error.stream = static_cast<std::uint8_t>(in.payload.error.stream);
      break;
    case EventType::kPoseUpdate:
      out->payload.pose.source = static_cast<std::uint8_t>(in.payload.pose.source);
      for (int i = 0; i < 3; ++i) out->payload.pose.position[i] = in.payload.pose.position[i];
      for (int i = 0; i < 4; ++i) out->payload.pose.quaternion[i] = in.payload.pose.quaternion[i];
      out->payload.pose.quality = in.payload.pose.quality;
      break;
    // B2 gap: kDeviceHealth had a C++ payload and no case here, so the health
    // snapshot crossed the ABI as raw bytes with a C++ struct's padding.
    case EventType::kDeviceHealth:
      out->payload.health.device = in.payload.health.device;
      out->payload.health.state = static_cast<std::uint8_t>(in.payload.health.state);
      out->payload.health.points_out = in.payload.health.points_out;
      out->payload.health.points_per_sec = in.payload.health.points_per_sec;
      out->payload.health.checksum_pass_rate = in.payload.health.checksum_pass_rate;
      break;
    case EventType::kGnssFix:
      out->payload.gnss.fix_type = in.payload.gnss.fix_type;
      out->payload.gnss.satellites = in.payload.gnss.satellites;
      out->payload.gnss.hdop = in.payload.gnss.hdop;
      out->payload.gnss.correction_age_s = in.payload.gnss.correction_age_s;
      out->payload.gnss.sigma_h_m = in.payload.gnss.sigma_h_m;
      out->payload.gnss.lat_deg = in.payload.gnss.lat_deg;
      out->payload.gnss.lon_deg = in.payload.gnss.lon_deg;
      out->payload.gnss.alt_m = in.payload.gnss.alt_m;
      break;
    case EventType::kNtripState:
      out->payload.ntrip.state = in.payload.ntrip.state;
      out->payload.ntrip.error = static_cast<std::int32_t>(in.payload.ntrip.error);
      out->payload.ntrip.backoff_ms = in.payload.ntrip.backoff_ms;
      out->payload.ntrip.bytes_received = in.payload.ntrip.bytes_received;
      out->payload.ntrip.correction_age_s = in.payload.ntrip.correction_age_s;
      break;
    case EventType::kGeorefConverged:
      out->payload.georef.cep95_m = in.payload.georef.cep95_m;
      out->payload.georef.horizontal_sigma_m = in.payload.georef.horizontal_sigma_m;
      out->payload.georef.samples = in.payload.georef.samples;
      out->payload.georef.epsg = in.payload.georef.epsg;
      out->payload.georef.converged = in.payload.georef.converged;
      break;
    // A15 gap, closed at ABI 4 (docs/A15-jobs.md §7.1): the type constant and
    // its static_assert existed since A1, but there was no union member and no
    // case, so every job-progress event reached a C consumer zeroed. B6's
    // Android processing queue reads exactly this.
    case EventType::kJobProgress:
      out->payload.job.job_id = in.payload.job.job_id;
      out->payload.job.progress = in.payload.job.progress;
      out->payload.job.state = in.payload.job.state;
      break;
    default:
      // Types whose payload the ABI does not mirror travel as raw bytes; the
      // consumer must not interpret them until this switch grows a case.
      // Every EventType declared today has one.
      std::memcpy(out->payload.raw, in.payload.raw, sizeof(out->payload.raw));
      break;
  }
}

// --- A10 mirrors -------------------------------------------------------------

// Fixed-size string fields (scan_ntrip_source, scan_georef_solution::blocker):
// always NUL-terminated, truncated rather than overflowing. Convention 3 says
// no struct here holds a pointer the callee keeps, and a mountpoint name is
// short enough that copying is cheaper than an ownership rule.
void copy_str(char* dst, std::size_t cap, const std::string& src) {
  if (cap == 0) return;
  const std::size_t n = std::min(src.size(), cap - 1);
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

void to_c_fix(const GnssFix& s, scan_gnss_fix* out) {
  std::memset(out, 0, sizeof(*out));
  out->t_mono_ns = s.t_mono_ns;
  out->t_arrival_ns = s.t_arrival_ns;
  out->utc_unix_ns = s.utc_unix_ns;
  out->fix = static_cast<std::uint8_t>(s.fix);
  out->satellites = s.satellites;
  out->quality_raw = s.quality_raw;
  out->fix_dimension = s.fix_dimension;
  out->station_id = s.station_id;
  out->lat_deg = s.lat_deg;
  out->lon_deg = s.lon_deg;
  out->alt_m = s.alt_m;
  out->geoid_sep_m = s.geoid_sep_m;
  out->height_ellipsoid_m = s.height_ellipsoid_m;
  out->has_geoid_sep = s.has_geoid_sep ? 1 : 0;
  out->hdop = s.hdop;
  out->pdop = s.pdop;
  out->vdop = s.vdop;
  out->correction_age_s = s.correction_age_s;
  out->sigma_east_m = s.sigma_east_m;
  out->sigma_north_m = s.sigma_north_m;
  out->sigma_up_m = s.sigma_up_m;
  out->sigma_horizontal_m = s.sigma_horizontal_m;
  out->sigma_from_gst = s.sigma_from_gst ? 1 : 0;
  out->speed_mps = s.speed_mps;
  out->course_deg = s.course_deg;
  out->has_course = s.has_course ? 1 : 0;
}

void to_c_gnss_stats(const GnssStats& s, const GnssSource& src, scan_gnss_stats* out) {
  std::memset(out, 0, sizeof(*out));
  out->bytes_in = s.nmea.bytes_in;
  out->sentences_ok = s.nmea.sentences_ok;
  out->checksum_failed = s.nmea.checksum_failed;
  out->malformed = s.nmea.malformed;
  out->checksum_pass_rate = s.nmea.checksum_pass_rate();
  out->epochs = s.epochs;
  out->fixes_published = s.fixes_published;
  out->poses_published = s.poses_published;
  out->epochs_no_position = s.epochs_no_position;
  out->epochs_below_gate = s.epochs_below_gate;
  out->gst_epochs = s.gst_epochs;
  for (int i = 0; i < 5; ++i) out->by_fix[i] = s.by_fix[i];
  out->time_converged = s.time_converged ? 1 : 0;
  out->time_uncertainty_ns = s.time_uncertainty_ns;
  crs::Geodetic o;
  if (src.origin(&o)) {
    out->has_origin = 1;
    out->origin_lat_deg = o.lat_deg;
    out->origin_lon_deg = o.lon_deg;
    out->origin_height_m = o.height_m;
  }
}

void to_c_ntrip_stats(const TcpNtripClient& c, scan_ntrip_stats* out) {
  const NtripStats s = c.stats();
  std::memset(out, 0, sizeof(*out));
  out->connect_attempts = s.connect_attempts;
  out->connects_ok = s.connects_ok;
  out->disconnects = s.disconnects;
  out->reconnects = s.reconnects;
  out->bytes_received = s.bytes_received;
  out->gga_sent = s.gga_sent;
  out->stalls = s.stalls;
  out->handshake_failures = s.handshake_failures;
  out->frames_ok = s.rtcm.frames_ok;
  out->frames_crc_failed = s.rtcm.frames_crc_failed;
  out->rtcm_bytes = s.rtcm.bytes_in;
  out->t_connected_ns = s.t_connected_ns;
  out->t_last_rtcm_ns = s.t_last_rtcm_ns;
  out->backoff_ms = s.backoff_ms;
  out->http_status = s.http_status;
  out->ntrip_version_used = s.ntrip_version_used;
  out->last_error = static_cast<std::int32_t>(s.last_error);
  out->state = static_cast<std::uint8_t>(c.state());
  out->receiving = c.receiving() ? 1 : 0;
  out->correction_age_s = c.correction_age_s();
}

void to_c_georef(const GeorefSolution& s, int epsg, scan_georef_solution* out) {
  std::memset(out, 0, sizeof(*out));
  out->converged = s.converged ? 1 : 0;
  out->yaw_deg = s.yaw_deg;
  for (int i = 0; i < 3; ++i) out->translation[i] = s.translation[i];
  out->scale = s.scale;
  for (int i = 0; i < 16; ++i) out->global_from_local[i] = s.global_from_local[i];
  out->samples = static_cast<std::uint32_t>(s.samples);
  out->inliers = static_cast<std::uint32_t>(s.inliers);
  out->rejected = static_cast<std::uint32_t>(s.rejected);
  out->residual_rms_m = s.residual_rms_m;
  out->residual_rms_h_m = s.residual_rms_h_m;
  out->gravity_residual_m = s.gravity_residual_m;
  out->yaw_sigma_deg = s.yaw_sigma_deg;
  out->translation_sigma_h_m = s.translation_sigma_h_m;
  out->span_m = s.span_m;
  out->horizontal_sigma_m = s.horizontal_sigma_m;
  out->vertical_sigma_m = s.vertical_sigma_m;
  out->cep50_m = s.cep50_m;
  out->cep95_m = s.cep95_m;
  out->mean_fix_sigma_m = s.mean_fix_sigma_m;
  out->best_fix = static_cast<std::uint8_t>(s.best_fix);
  out->dominant_fix = static_cast<std::uint8_t>(s.dominant_fix);
  out->epsg = epsg;
  out->t_first_ns = s.t_first_ns;
  out->t_last_ns = s.t_last_ns;
  copy_str(out->blocker, sizeof(out->blocker), s.blocker != nullptr ? s.blocker : "");
}

NtripConfig from_c(const scan_ntrip_config& c) {
  NtripConfig n;
  if (c.host != nullptr) n.host = c.host;
  if (c.port != 0) n.port = c.port;
  if (c.mountpoint != nullptr) n.mountpoint = c.mountpoint;
  if (c.username != nullptr) n.username = c.username;
  if (c.password != nullptr) n.password = c.password;
  if (c.user_agent != nullptr && c.user_agent[0] != '\0') n.user_agent = c.user_agent;
  if (c.ntrip_version != 0) n.ntrip_version = c.ntrip_version;
  if (c.allow_v1_fallback_set != 0) n.allow_v1_fallback = c.allow_v1_fallback != 0;
  if (c.connect_timeout_ms != 0) n.connect_timeout_ms = c.connect_timeout_ms;
  if (c.stall_timeout_ms != 0) n.stall_timeout_ms = c.stall_timeout_ms;
  // 0 keeps the 10 s default; negative is the explicit "do not upload GGA".
  if (c.gga_interval_ms != 0) n.gga_interval_ms = c.gga_interval_ms < 0 ? 0 : c.gga_interval_ms;
  if (c.auto_reconnect_set != 0) n.auto_reconnect = c.auto_reconnect != 0;
  n.max_reconnect_attempts = c.max_reconnect_attempts;
  return n;
}

// The NTRIP handle. It either BORROWS the engine's client — the one wired to
// the rover's GGA and to the recorder — or owns a standalone one. `owned` is
// what decides, so destroy() is correct either way.
struct NtripHandle {
  TcpNtripClient* client = nullptr;
  std::unique_ptr<TcpNtripClient> owned;
  Engine* engine = nullptr;  // non-null iff borrowed
  scan_rtcm_cb cb = nullptr;
  void* user = nullptr;

  // ByteSpan -> (ptr, len). A trampoline rather than a function-pointer cast:
  // the two signatures are not compatible types (DESIGN §4).
  static void forward(ByteSpan rtcm, void* self) {
    auto* h = static_cast<NtripHandle*>(self);
    if (h->cb != nullptr) h->cb(rtcm.data(), rtcm.size(), h->user);
  }
};

NtripHandle* ntrip_of(scan_ntrip* c) { return reinterpret_cast<NtripHandle*>(c); }

// Convention 4: engine-owned, UTF-8, valid until the next engine call ON THIS
// THREAD. thread_local is what makes that promise true when two threads ask.
const char* thread_string(std::string s) {
  static thread_local std::string buf;
  buf = std::move(s);
  return buf.c_str();
}

void event_trampoline(const Event& ev, void* user) {
  auto* h = static_cast<EngineHandle*>(user);
  if (h->event_cb == nullptr) return;
  scan_event out;
  convert_event(ev, &out);
  h->event_cb(&out, h->event_user);
}

}  // namespace

// TEST SEAM (INT-34). Not declared in scanengine_c.h, not exported as part of
// the ABI, and not callable from C: convert_event() is static, and the only
// producer of a kJobProgress event is the engine's own bus — which nothing in
// the C surface can publish onto, because ABI 4 deliberately gives A15's job
// queue no C entry point (an app drives it through Engine::jobs()). Without
// this, the kJobProgress union case added at ABI 4 could only be verified by
// the first JNI consumer to hit it. tests/test_capi.cpp declares it.
namespace scanengine {
namespace capi_test {
void convert_event_for_test(const Event& in, scan_event* out) { convert_event(in, out); }
}  // namespace capi_test
}  // namespace scanengine

namespace {

// --- ABI 6 helpers (A16) ----------------------------------------------------

// The two serial probes take the same C-side argument shape, so the
// conversion of (char**, count) into a vector<string> lives once.
Status collect_ports(const char* const* ports, std::uint32_t n_ports,
                     std::vector<std::string>* out) {
  if (ports == nullptr && n_ports != 0) {
    return set_last_error(ScanError::kInvalidArgument, "null port array");
  }
  for (std::uint32_t i = 0; i < n_ports; ++i) {
    if (ports[i] == nullptr) {
      return set_last_error(ScanError::kInvalidArgument, "port %u is null", i);
    }
    out->emplace_back(ports[i]);
  }
  return kOkStatus;
}

// The C surface's single guard. Function-local static: constructed on first
// use, destroyed (and so released) at process exit even if the app forgets.
InstanceGuard& capi_instance_guard() {
  static InstanceGuard g;
  return g;
}

}  // namespace

// Every entry point goes through this: no exception may cross the ABI.
#define SCAN_GUARD_BEGIN try {
#define SCAN_GUARD_END                                                   \
  }                                                                      \
  catch (const std::bad_alloc&) {                                        \
    return fail(ScanError::kOutOfMemory, "out of memory");               \
  }                                                                      \
  catch (const std::exception& e) {                                      \
    return to_c(set_last_error(ScanError::kUnknown, "%s", e.what()));    \
  }                                                                      \
  catch (...) {                                                          \
    return fail(ScanError::kUnknown, "unknown C++ exception");           \
  }

extern "C" {

uint32_t scan_engine_abi_version(void) { return SCAN_ABI_VERSION; }

const char* scan_engine_version_string(void) { return engine_version_string(); }

const char* scan_error_str(scan_error_t err) {
  return error_str(static_cast<ScanError>(err));
}

const char* scan_engine_last_error(void) { return last_error_message(); }

scan_error_t scan_engine_create(const scan_engine_config* cfg, scan_engine** out) {
  SCAN_GUARD_BEGIN
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "out handle is null");
  *out = nullptr;

  EngineConfig ec;
  if (cfg != nullptr) {
    if (cfg->app_name != nullptr) ec.app_name = cfg->app_name;
    ec.log_level = static_cast<LogLevel>(cfg->log_level);
    if (cfg->page_capacity != 0) ec.points.page_capacity = cfg->page_capacity;
    if (cfg->max_pages != 0) ec.points.max_pages = cfg->max_pages;
    if (cfg->event_queue_capacity != 0) ec.event_queue_capacity = cfg->event_queue_capacity;
  }

  auto engine = Engine::create(ec);
  if (!engine.ok()) return to_c(engine.error());

  auto* h = new EngineHandle();
  h->engine = std::move(engine).value();
  *out = reinterpret_cast<scan_engine*>(h);
  return SCAN_OK;
  SCAN_GUARD_END
}

void scan_engine_destroy(scan_engine* engine) {
  if (engine == nullptr) return;
  delete handle_of(engine);
}

scan_error_t scan_engine_start(scan_engine* engine, const scan_session_config* cfg) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  SessionConfig sc;
  if (cfg != nullptr) {
    if (cfg->lscan_dir != nullptr) sc.lscan_dir = cfg->lscan_dir;
    if (cfg->profile != nullptr) sc.profile = cfg->profile;
    sc.record = cfg->record != 0;
    sc.live_slam = cfg->live_slam != 0;
    sc.pushbroom = cfg->pushbroom != 0;
    sc.trajectory = static_cast<TrajectorySource>(cfg->trajectory);
    // A live capture is what the odometry thread exists for: the SDK's receive
    // thread must never run scan-to-map inline (DESIGN §2).
    sc.lio.internal_thread = true;
  }
  return to_c(handle_of(engine)->engine->start_session(sc));
  SCAN_GUARD_END
}

scan_error_t scan_engine_stop(scan_engine* engine) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  return to_c(handle_of(engine)->engine->stop_session());
  SCAN_GUARD_END
}

scan_error_t scan_engine_state(scan_engine* engine, int32_t* out_state) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out_state == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out_state = static_cast<int32_t>(handle_of(engine)->engine->state());
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_add_device(scan_engine* engine, const scan_device_config* cfg,
                                    uint32_t* out_device_id) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || cfg == nullptr || out_device_id == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out_device_id = 0;

  DeviceConfig dc;
  dc.kind = static_cast<DeviceKind>(cfg->kind);
  switch (dc.kind) {
    case DeviceKind::kD6:
      dc.d6.serial.port_name = cfg->serial_port_name != nullptr ? cfg->serial_port_name : "";
      if (cfg->serial_baud != 0) dc.d6.serial.baud = cfg->serial_baud;
      if (cfg->serial_write != nullptr) {
        auto shim = std::make_unique<SerialShim>();
        shim->cb = cfg->serial_write;
        shim->user = cfg->serial_write_user_data;
        dc.d6.serial.write_fn = &SerialShim::write;
        dc.d6.serial.write_user_data = shim.get();
        handle_of(engine)->serial_shims.push_back(std::move(shim));
      }
      dc.d6.send_start_stop_commands = cfg->send_start_stop_commands != 0;
      break;
    case DeviceKind::kMid360: {
      if (cfg->lidar_ip != nullptr) dc.mid360.udp.lidar_ip = cfg->lidar_ip;
      if (cfg->host_ip != nullptr) dc.mid360.udp.host_ip = cfg->host_ip;

      // --- ABI 5 -----------------------------------------------------------
      //
      // Every field is "0 keeps the C++ default", except the three that carry
      // an explicit _set flag because 0 is a value a caller may mean.
      if (cfg->mid360_backend < 0 || cfg->mid360_backend > 2) {
        return to_c(set_last_error(
            ScanError::kInvalidArgument,
            "scan_device_config::mid360_backend %d is not a SCAN_MID360_BACKEND_* value",
            static_cast<int>(cfg->mid360_backend)));
      }
      dc.mid360.backend = static_cast<Mid360Backend>(cfg->mid360_backend);

      UdpConfig& u = dc.mid360.udp;
      // A 0 fd is never a descriptor an app hands over (it is its stdin), so
      // treating it as "unset" costs nothing and makes memset(0) mean ABI 4.
      if (cfg->mid360_prebound_fd > 0) u.prebound_fd = cfg->mid360_prebound_fd;
      if (cfg->mid360_prebound_imu_fd > 0) u.prebound_imu_fd = cfg->mid360_prebound_imu_fd;
      if (cfg->mid360_point_port != 0) u.point_port = cfg->mid360_point_port;
      if (cfg->mid360_imu_port != 0) u.imu_port = cfg->mid360_imu_port;
      if (cfg->mid360_cmd_port != 0) u.cmd_port = cfg->mid360_cmd_port;
      if (cfg->mid360_push_port != 0) u.push_port = cfg->mid360_push_port;
      if (cfg->mid360_log_port != 0) u.log_port = cfg->mid360_log_port;
      if (cfg->mid360_host_cmd_port != 0) u.host_cmd_port = cfg->mid360_host_cmd_port;
      if (cfg->mid360_host_push_port != 0) u.host_push_port = cfg->mid360_host_push_port;
      if (cfg->mid360_host_point_port != 0) u.host_point_port = cfg->mid360_host_point_port;
      if (cfg->mid360_host_imu_port != 0) u.host_imu_port = cfg->mid360_host_imu_port;
      if (cfg->mid360_host_log_port != 0) u.host_log_port = cfg->mid360_host_log_port;
      if (cfg->mid360_recv_buffer_bytes != 0) u.recv_buffer_bytes = cfg->mid360_recv_buffer_bytes;

      if (cfg->mid360_live_points_per_sec_set != 0) {
        dc.mid360.live_points_per_sec = cfg->mid360_live_points_per_sec;
      }
      if (cfg->mid360_publish_imu_set != 0) {
        dc.mid360.publish_imu = cfg->mid360_publish_imu != 0;
      }
      if (cfg->mid360_verify_crc_set != 0) {
        dc.mid360.verify_crc = cfg->mid360_verify_crc != 0;
      }
      if (cfg->mid360_filter_set != 0) {
        mid360::PointFilterConfig& f = dc.mid360.filter;
        f.drop_no_return = cfg->mid360_drop_no_return != 0;
        f.tag_reject_mask = cfg->mid360_tag_reject_mask;
        f.min_reflectivity = cfg->mid360_min_reflectivity;
        f.min_range_m = cfg->mid360_min_range_m;
        f.max_range_m = cfg->mid360_max_range_m;
      }
      if (cfg->mid360_sdk_config_path != nullptr && cfg->mid360_sdk_config_path[0] != '\0') {
        dc.mid360.sdk_config_path = cfg->mid360_sdk_config_path;
      }

      // Caught HERE rather than at start(): add_device() is where the app can
      // still do something about it, and the message names the field.
      if (u.prebound_fd >= 0 && dc.mid360.backend != Mid360Backend::kRawUdp) {
        return fail(ScanError::kInvalidArgument,
                    "scan_device_config: a pre-bound descriptor only reaches the raw-UDP backend "
                    "(SCAN_MID360_BACKEND_RAW_UDP); SDK2 creates its own sockets inside the "
                    "vendored SDK, so binding one to a Network here would have no effect");
      }
      break;
    }
    default:
      break;
  }

  auto id = handle_of(engine)->engine->add_device(dc);
  if (!id.ok()) return to_c(id.error());
  *out_device_id = id.value();
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_remove_device(scan_engine* engine, uint32_t device_id) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  return to_c(handle_of(engine)->engine->remove_device(device_id));
  SCAN_GUARD_END
}

scan_error_t scan_engine_device_health(scan_engine* engine, uint32_t device_id,
                                       scan_device_health* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  auto h = handle_of(engine)->engine->device_health(device_id);
  if (!h.ok()) return to_c(h.error());
  const DeviceHealth& src = h.value();
  std::memset(out, 0, sizeof(*out));
  out->id = src.id;
  out->kind = static_cast<std::uint8_t>(src.kind);
  out->state = static_cast<std::uint8_t>(src.state);
  out->last_error = static_cast<std::int32_t>(src.last_error);
  out->bytes_in = src.bytes_in;
  out->packets_ok = src.packets_ok;
  out->packets_bad = src.packets_bad;
  out->points_out = src.points_out;
  out->drops = src.drops;
  out->points_per_sec = src.points_per_sec;
  out->rotation_hz = src.rotation_hz;
  out->checksum_pass_rate = src.checksum_pass_rate;
  out->t_last_data_ns = src.t_last_data_ns;
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_mid360_stats(scan_engine* engine, uint32_t device_id,
                                      scan_mid360_stats* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  std::memset(out, 0, sizeof(*out));
  auto s = handle_of(engine)->engine->mid360_stats(device_id);
  if (!s.ok()) return to_c(s.error());
  const Mid360Stats& m = s.value();
  out->link = static_cast<std::uint8_t>(m.link);
  out->state = static_cast<std::uint8_t>(m.state);
  out->point_packets = m.point_packets;
  out->imu_packets = m.imu_packets;
  out->points_received = m.points_received;
  out->points_kept = m.points_kept;
  out->points_appended = m.points_appended;
  out->points_dropped_store = m.points_dropped_store;
  out->bad_packets = m.bad_packets;
  out->imu_dropped = m.imu_dropped;
  out->packets_lost = m.packets_lost;
  out->packets_duplicated = m.packets_duplicated;
  out->counter_resets = m.counter_resets;
  out->points_per_sec = m.points_per_sec;
  out->points_appended_per_sec = m.points_appended_per_sec;
  out->imu_hz = m.imu_hz;
  out->loss_pct_window = m.loss_pct_window;
  out->loss_pct_total = m.loss_pct_total;
  out->watchdog_trips = m.watchdog_trips;
  out->clean_resumes = m.clean_resumes;
  out->forced_reinits = m.forced_reinits;
  out->reinit_failures = m.reinit_failures;
  out->t_last_point_ns = m.t_last_point_ns;
  out->t_last_imu_ns = m.t_last_imu_ns;
  out->t_last_heartbeat_ns = m.t_last_heartbeat_ns;
  out->t_silent_since_ns = m.t_silent_since_ns;
  copy_str(out->device_sn, sizeof(out->device_sn), m.device_sn);
  copy_str(out->device_ip, sizeof(out->device_ip), m.device_ip);
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_push_serial_bytes(scan_engine* engine, uint32_t device_id,
                                           const uint8_t* data, size_t len,
                                           int64_t t_mono_ns) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  if (data == nullptr && len != 0) return fail(ScanError::kInvalidArgument, "null buffer");
  return to_c(handle_of(engine)->engine->push_serial_bytes(device_id, ByteSpan(data, len),
                                                           TimePoint{t_mono_ns}));
  SCAN_GUARD_END
}

scan_error_t scan_engine_poll_event(scan_engine* engine, scan_event* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  EngineHandle* h = handle_of(engine);
  Event ev;
  if (!h->engine->events().poll(h->engine->app_subscription(), &ev)) {
    return SCAN_ERR_AGAIN;  // not an error: "nothing right now"
  }
  convert_event(ev, out);
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_wait_event(scan_engine* engine, scan_event* out, int32_t timeout_ms) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  EngineHandle* h = handle_of(engine);
  Event ev;
  if (!h->engine->events().wait(h->engine->app_subscription(), &ev, timeout_ms)) {
    return SCAN_ERR_AGAIN;
  }
  convert_event(ev, out);
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_set_event_callback(scan_engine* engine, scan_event_cb cb,
                                            void* user_data) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  EngineHandle* h = handle_of(engine);
  h->event_cb = cb;
  h->event_user = user_data;
  if (cb == nullptr) {
    // Back to polling: re-subscribe in queued mode.
    return to_c(h->engine->set_app_event_callback(nullptr, nullptr));
  }
  return to_c(h->engine->set_app_event_callback(&event_trampoline, h));
  SCAN_GUARD_END
}

scan_error_t scan_engine_page_count(scan_engine* engine, uint32_t* out_count) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out_count == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out_count = static_cast<uint32_t>(handle_of(engine)->engine->points().page_count());
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_page_id_at(scan_engine* engine, uint32_t index, uint32_t* out_page_id) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out_page_id == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  const auto ids = handle_of(engine)->engine->points().page_ids();
  if (index >= ids.size()) {
    return to_c(set_last_error(ScanError::kNotFound, "page index %u of %zu", index, ids.size()));
  }
  *out_page_id = ids[index];
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_get_point_page(scan_engine* engine, uint32_t page_id,
                                        scan_point_page* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  const PageView v = handle_of(engine)->engine->points().page_view(page_id);
  if (!v.valid()) {
    return to_c(set_last_error(ScanError::kNotFound, "no point page %u", page_id));
  }
  std::memset(out, 0, sizeof(*out));
  out->id = v.id;
  out->stream = static_cast<std::uint8_t>(v.stream);
  out->data = reinterpret_cast<const scan_point_vertex*>(v.data);
  out->count = v.count;
  out->capacity = v.capacity;
  out->t_first_ns = v.t_first_ns;
  out->t_last_ns = v.t_last_ns;
  for (int i = 0; i < 3; ++i) {
    out->bounds_min[i] = v.bounds_min[i];
    out->bounds_max[i] = v.bounds_max[i];
  }
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_total_points(scan_engine* engine, uint64_t* out_points) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out_points == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out_points = handle_of(engine)->engine->points().total_points();
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_set_live_page_eviction(scan_engine* engine, uint8_t enabled) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "null engine");
  return to_c(handle_of(engine)->engine->set_live_page_eviction(enabled != 0));
  SCAN_GUARD_END
}

scan_error_t scan_engine_page_stats(scan_engine* engine, scan_page_stats* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  const PageStoreStats s = handle_of(engine)->engine->points().stats();
  std::memset(out, 0, sizeof(*out));
  out->pages = s.pages;
  out->max_pages = s.max_pages;
  out->resident_points = s.resident_points;
  out->total_points = s.total_points;
  out->dropped_points = s.dropped_points;
  out->evicted_pages = s.evicted_pages;
  out->evicted_points = s.evicted_points;
  out->evicting = s.evicting ? 1u : 0u;
  out->eviction_enabled = s.when_full == PageFullPolicy::kEvictOldest ? 1u : 0u;
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_recycle_live_pages(scan_engine* engine) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "null engine");
  handle_of(engine)->engine->points().recycle_all();
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- poses (A8) -------------------------------------------------------------

scan_error_t scan_engine_push_pose(scan_engine* engine, const scan_pose* pose, float confidence) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || pose == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  Pose p;
  p.t_mono_ns = pose->t_mono_ns;
  for (int i = 0; i < 3; ++i) p.position[i] = pose->position[i];
  for (int i = 0; i < 4; ++i) p.orientation[i] = pose->orientation[i];
  p.position_sigma_m = pose->position_sigma_m;
  p.orientation_sigma_deg = pose->orientation_sigma_deg;
  p.source = static_cast<StreamId>(pose->source);
  p.quality = static_cast<PoseQuality>(pose->quality);
  p.tracking_lost = pose->tracking_lost;
  Engine& e = *handle_of(engine)->engine;
  // Negative = "derive it", which is what an ARCore caller wants: it has a
  // TrackingState, not a scalar.
  return to_c(confidence < 0.0f ? e.push_pose(p) : e.push_pose(p, confidence));
  SCAN_GUARD_END
}

scan_error_t scan_engine_pose_at(scan_engine* engine, int64_t t_mono_ns, scan_pose* out,
                                 uint8_t* out_gate) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  const PoseSample s = handle_of(engine)->engine->pose_at(t_mono_ns);
  if (out_gate != nullptr) *out_gate = static_cast<std::uint8_t>(s.gate);
  if (out != nullptr) {
    std::memset(out, 0, sizeof(*out));
    out->t_mono_ns = s.pose.t_mono_ns;
    for (int i = 0; i < 3; ++i) out->position[i] = s.pose.position[i];
    for (int i = 0; i < 4; ++i) out->orientation[i] = s.pose.orientation[i];
    out->position_sigma_m = s.pose.position_sigma_m;
    out->orientation_sigma_deg = s.pose.orientation_sigma_deg;
    out->source = static_cast<std::uint8_t>(s.pose.source);
    out->quality = static_cast<std::uint8_t>(s.pose.quality);
    out->tracking_lost = s.pose.tracking_lost;
  }
  // has_pose, not gate == kOk: a flagged sample carries real geometry and the
  // caller decides what to do with it — that is the whole point of the gate.
  if (s.has_pose) return SCAN_OK;
  if (s.gate == PoseGate::kFuture) return SCAN_ERR_AGAIN;
  return to_c(set_last_error(ScanError::kNotFound, "no pose at %lld ns (%s)",
                             static_cast<long long>(t_mono_ns), to_string(s.gate)));
  SCAN_GUARD_END
}

// --- the phone IMU (ABI 8, ROUND 9 item 35) ---------------------------------

scan_error_t scan_engine_push_imu(scan_engine* engine, int64_t t_mono_ns,
                                  const float gyro_rad_s[3], const float accel_m_s2[3]) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || gyro_rad_s == nullptr || accel_m_s2 == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  return to_c(handle_of(engine)->engine->push_phone_imu(t_mono_ns, gyro_rad_s, accel_m_s2));
  SCAN_GUARD_END
}

scan_error_t scan_engine_set_imu_extrinsics(scan_engine* engine, const double quat_xyzw[4]) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || quat_xyzw == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  return to_c(handle_of(engine)->engine->set_imu_extrinsics(quat_xyzw));
  SCAN_GUARD_END
}

scan_error_t scan_engine_imu_densify_stats(scan_engine* engine, scan_imu_densify_stats* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  const ImuDensifyStats s = handle_of(engine)->engine->imu_densify_stats();
  std::memset(out, 0, sizeof(*out));
  out->samples_in = s.samples_in;
  out->samples_rejected = s.samples_rejected;
  out->queries = s.queries;
  out->densified = s.densified;
  out->fallbacks = s.fallbacks;
  out->fallback_no_imu = s.fallback_no_imu;
  out->fallback_gap = s.fallback_gap;
  out->fallback_bracket = s.fallback_bracket;
  out->fallback_closing = s.fallback_closing;
  out->bias_updates = s.bias_updates;
  for (int i = 0; i < 3; ++i) out->bias_rad_s[i] = s.bias_rad_s[i];
  out->worst_closing_deg = s.worst_closing_deg;
  out->mean_closing_deg = s.mean_closing_deg;
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- pushbroom (A8) ---------------------------------------------------------

scan_error_t scan_engine_set_mount_extrinsics(scan_engine* engine,
                                              const double phone_from_lidar[16]) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || phone_from_lidar == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  return to_c(handle_of(engine)->engine->set_mount_extrinsics(phone_from_lidar));
  SCAN_GUARD_END
}

scan_error_t scan_engine_pushbroom_enable(scan_engine* engine, int on) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  return to_c(handle_of(engine)->engine->set_pushbroom_enabled(on != 0));
  SCAN_GUARD_END
}

scan_error_t scan_engine_pushbroom_flush(scan_engine* engine) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  return to_c(handle_of(engine)->engine->pushbroom_flush());
  SCAN_GUARD_END
}

scan_error_t scan_engine_pushbroom_stats(scan_engine* engine, scan_pushbroom_stats* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  const PushbroomStats s = handle_of(engine)->engine->pushbroom_stats();
  std::memset(out, 0, sizeof(*out));
  out->points_in = s.points_in;
  out->points_out = s.points_out;
  out->points_pending = s.points_pending;
  out->dropped_range = s.dropped_range;
  out->dropped_no_pose = s.dropped_no_pose;
  out->dropped_overflow = s.dropped_overflow;
  out->dropped_page_full = s.dropped_page_full;
  out->flagged_tracking_lost = s.flagged_tracking_lost;
  out->flagged_stale_pose = s.flagged_stale_pose;
  out->flagged_low_confidence = s.flagged_low_confidence;
  out->flagged_emitted = s.flagged_emitted;
  out->t_first_ns = s.t_first_ns;
  out->t_last_ns = s.t_last_ns;
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- ROUND 10 item 36: the lidar -> pose time offset (ABI 9) ----------------

scan_error_t scan_engine_set_pose_time_offset_ns(scan_engine* engine, int64_t offset_ns) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "null engine");
  const Status s = handle_of(engine)->engine->set_pose_time_offset_ns(
      static_cast<std::int64_t>(offset_ns));
  return to_c(s);
  SCAN_GUARD_END
}

scan_error_t scan_engine_pose_time_offset_ns(scan_engine* engine, int64_t* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out = static_cast<int64_t>(handle_of(engine)->engine->pose_time_offset_ns());
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- mount calibration (A8) -------------------------------------------------

scan_error_t scan_mount_calib_create(scan_mount_calib** out) {
  SCAN_GUARD_BEGIN
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "out handle is null");
  *out = reinterpret_cast<scan_mount_calib*>(new MountCalibrationSolver());
  return SCAN_OK;
  SCAN_GUARD_END
}

void scan_mount_calib_destroy(scan_mount_calib* calib) {
  if (calib == nullptr) return;
  delete reinterpret_cast<MountCalibrationSolver*>(calib);
}

scan_error_t scan_mount_calib_add_observation(scan_mount_calib* calib, const double normal[3],
                                              double d, const scan_point_vertex* pts, uint32_t n,
                                              double sigma_m) {
  SCAN_GUARD_BEGIN
  if (calib == nullptr || normal == nullptr || (pts == nullptr && n != 0)) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  auto* solver = reinterpret_cast<MountCalibrationSolver*>(calib);
  // scan_point_vertex is 16-byte-identical to PointVertex (static_asserted
  // above) — this is the one place the ABI is allowed to alias rather than
  // convert, and it is why the assert exists.
  const auto* first = reinterpret_cast<const PointVertex*>(pts);
  return to_c(solver->add_observation(normal, d, Span<const PointVertex>(first, n), sigma_m));
  SCAN_GUARD_END
}

scan_error_t scan_mount_calib_solve(scan_mount_calib* calib, const double cad[16],
                                    scan_mount_calib_result* out) {
  SCAN_GUARD_BEGIN
  if (calib == nullptr || cad == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  auto* solver = reinterpret_cast<MountCalibrationSolver*>(calib);
  auto r = solver->solve(cad);
  if (!r.ok()) return to_c(r.error());
  const MountCalibResult& s = r.value();
  std::memset(out, 0, sizeof(*out));
  for (int i = 0; i < 16; ++i) out->camera_from_lidar[i] = s.camera_from_lidar[i];
  out->converged = s.converged ? 1 : 0;
  out->degenerate = s.degenerate ? 1 : 0;
  out->iterations_l2 = s.iterations_l2;
  out->iterations_robust = s.iterations_robust;
  out->observations = s.observations;
  out->residuals = s.residuals;
  out->rms_residual_m = s.rms_residual_m;
  out->final_cost = s.final_cost;
  out->split_half_px = s.split_half_px;
  out->gate_range_m = s.gate_range_m;
  out->gate = static_cast<std::uint8_t>(s.gate);
  out->sigma_rot_deg = s.sigma_rot_deg;
  out->sigma_trans_mm = s.sigma_trans_mm;
  out->condition_number = s.condition_number;
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- GNSS / RTK (A10) -------------------------------------------------------

scan_error_t scan_engine_push_nmea(scan_engine* engine, uint32_t device_id, const uint8_t* data,
                                   size_t len, int64_t t_mono_ns) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  if (data == nullptr && len != 0) return fail(ScanError::kInvalidArgument, "null buffer");
  Engine& e = *handle_of(engine)->engine;
  // Refuse a non-rover device rather than feeding NMEA to a D6 parser, which
  // would silently read as a stream of malformed packets.
  auto h = e.device_health(device_id);
  if (!h.ok()) return to_c(h.error());
  if (h.value().kind != DeviceKind::kRtkRover) {
    return to_c(set_last_error(ScanError::kInvalidArgument, "device %u is %s, not an RTK rover",
                               device_id, to_string(h.value().kind)));
  }
  return to_c(e.push_serial_bytes(device_id, ByteSpan(data, len), TimePoint{t_mono_ns}));
  SCAN_GUARD_END
}

scan_error_t scan_engine_last_fix(scan_engine* engine, scan_gnss_fix* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  to_c_fix(handle_of(engine)->engine->last_fix(), out);
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_gnss_stats(scan_engine* engine, scan_gnss_stats* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  Engine& e = *handle_of(engine)->engine;
  to_c_gnss_stats(e.gnss_stats(), e.gnss(), out);
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_engine_georef_solution(scan_engine* engine, scan_georef_solution* out) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  Engine& e = *handle_of(engine)->engine;
  to_c_georef(e.georef_solution(), e.georef().epsg(), out);
  return SCAN_OK;
  SCAN_GUARD_END
}

const char* scan_engine_crs_wkt(scan_engine* engine) {
  if (engine == nullptr) return "";
  try {
    return thread_string(handle_of(engine)->engine->crs_wkt());
  } catch (...) {
    return "";
  }
}

scan_error_t scan_engine_set_crs(scan_engine* engine, const char* epsg, const char* wkt) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  // NULL is "", which is how a caller clears the override.
  return to_c(handle_of(engine)->engine->set_crs(epsg != nullptr ? std::string(epsg) : std::string(),
                                                 wkt != nullptr ? std::string(wkt) : std::string()));
  SCAN_GUARD_END
}

const char* scan_engine_crs_epsg(scan_engine* engine) {
  if (engine == nullptr) return "";
  try {
    return thread_string(handle_of(engine)->engine->crs_epsg());
  } catch (...) {
    return "";
  }
}

scan_error_t scan_ntrip_create(scan_engine* engine, scan_ntrip** out) {
  SCAN_GUARD_BEGIN
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "out handle is null");
  *out = nullptr;
  auto h = std::make_unique<NtripHandle>();
  if (engine != nullptr) {
    h->engine = handle_of(engine)->engine.get();
    h->client = &h->engine->ntrip();
  } else {
    h->owned = std::make_unique<TcpNtripClient>();
    h->client = h->owned.get();
  }
  *out = reinterpret_cast<scan_ntrip*>(h.release());
  return SCAN_OK;
  SCAN_GUARD_END
}

void scan_ntrip_destroy(scan_ntrip* client) {
  if (client == nullptr) return;
  NtripHandle* h = ntrip_of(client);
  // A borrowed client outlives the handle, so leaving it holding a dangling
  // callback is exactly the crash this clears. A standalone one is joined by
  // its own destructor.
  if (h->owned == nullptr && h->engine != nullptr) h->engine->set_rtcm_sink(nullptr, nullptr);
  delete h;
}

scan_error_t scan_ntrip_connect(scan_ntrip* client, const scan_ntrip_config* cfg) {
  SCAN_GUARD_BEGIN
  if (client == nullptr || cfg == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  return to_c(ntrip_of(client)->client->connect(from_c(*cfg)));
  SCAN_GUARD_END
}

scan_error_t scan_ntrip_disconnect(scan_ntrip* client) {
  SCAN_GUARD_BEGIN
  if (client == nullptr) return fail(ScanError::kInvalidArgument, "client handle is null");
  return to_c(ntrip_of(client)->client->disconnect());
  SCAN_GUARD_END
}

scan_error_t scan_ntrip_get_state(scan_ntrip* client, int32_t* out_state) {
  SCAN_GUARD_BEGIN
  if (client == nullptr || out_state == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out_state = static_cast<std::int32_t>(ntrip_of(client)->client->state());
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_ntrip_get_stats(scan_ntrip* client, scan_ntrip_stats* out) {
  SCAN_GUARD_BEGIN
  if (client == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  to_c_ntrip_stats(*ntrip_of(client)->client, out);
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_ntrip_set_rtcm_callback(scan_ntrip* client, scan_rtcm_cb cb, void* user_data) {
  SCAN_GUARD_BEGIN
  if (client == nullptr) return fail(ScanError::kInvalidArgument, "client handle is null");
  NtripHandle* h = ntrip_of(client);
  h->cb = cb;
  h->user = user_data;
  if (h->engine != nullptr) {
    // Through the Engine, not the client: the Engine keeps its own handler
    // installed (that is what records the forwarded frames), and
    // NtripClient::set_rtcm_callback() would replace it.
    h->engine->set_rtcm_sink(cb != nullptr ? &NtripHandle::forward : nullptr, h);
  } else if (cb != nullptr) {
    h->client->set_rtcm_handler([h](ByteSpan rtcm) { NtripHandle::forward(rtcm, h); });
  } else {
    h->client->set_rtcm_handler(nullptr);
  }
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_ntrip_fetch_sourcetable(const scan_ntrip_config* cfg, scan_ntrip_source* out,
                                          uint32_t capacity, uint32_t* out_count) {
  SCAN_GUARD_BEGIN
  if (cfg == nullptr || out_count == nullptr || (out == nullptr && capacity != 0)) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out_count = 0;
  TcpNtripClient c;
  std::vector<NtripSource> sources;
  const Status s = c.fetch_sourcetable(from_c(*cfg), &sources);
  if (!s.ok()) return to_c(s);
  // The caster's TRUE total, always — a caller that passed a short array can
  // size a second one from it instead of guessing.
  *out_count = static_cast<std::uint32_t>(sources.size());
  const std::uint32_t n = std::min<std::uint32_t>(capacity, *out_count);
  for (std::uint32_t i = 0; i < n; ++i) {
    const NtripSource& src = sources[i];
    scan_ntrip_source& d = out[i];
    std::memset(&d, 0, sizeof(d));
    copy_str(d.mountpoint, sizeof(d.mountpoint), src.mountpoint);
    copy_str(d.identifier, sizeof(d.identifier), src.identifier);
    copy_str(d.format, sizeof(d.format), src.format);
    copy_str(d.nav_system, sizeof(d.nav_system), src.nav_system);
    copy_str(d.country, sizeof(d.country), src.country);
    d.lat_deg = src.lat_deg;
    d.lon_deg = src.lon_deg;
    d.needs_gga = src.needs_gga ? 1 : 0;
    d.fee = src.fee ? 1 : 0;
    d.carrier = src.carrier;
    d.solution = src.solution;
    d.bitrate = src.bitrate;
  }
  if (n < *out_count) {
    return to_c(set_last_error(ScanError::kCapacityExceeded, "%u of %u sources written", n,
                               *out_count));
  }
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- camera keyframes (A11 / B8), ABI 4 -------------------------------------

scan_error_t scan_engine_record_keyframe(scan_engine* engine, const scan_keyframe* kf) {
  SCAN_GUARD_BEGIN
  if (engine == nullptr) return fail(ScanError::kInvalidArgument, "engine handle is null");
  if (kf == nullptr) return fail(ScanError::kInvalidArgument, "keyframe is null");
  if (kf->image_name == nullptr) {
    return fail(ScanError::kInvalidArgument, "keyframe image_name is null");
  }

  // Field by field, never a reinterpret_cast: scan_keyframe is laid out for C
  // callers and Keyframe carries a std::string.
  Keyframe out;
  out.t_mono_ns = kf->t_engine_ns;
  // colorize.h defines Keyframe::image_path relative to the .lscan ROOT;
  // scan_keyframe::image_name is relative to streams/frames/, which is the
  // directory the app writes into and what the on-disk record stores. The
  // codec decomposes it again on the way out, so this composition is the
  // inverse of what read_frame_index() does.
  out.image_path = std::string("streams/frames/") + kf->image_name;
  out.pose.t_mono_ns = kf->t_engine_ns;
  for (int i = 0; i < 3; ++i) out.pose.position[i] = kf->position[i];
  for (int i = 0; i < 4; ++i) out.pose.orientation[i] = kf->orientation[i];
  out.pose.position_sigma_m = kf->position_sigma_m;
  out.pose.orientation_sigma_deg = kf->orientation_sigma_deg;
  out.pose.source = static_cast<StreamId>(kf->pose_source);
  out.pose.quality = static_cast<PoseQuality>(kf->pose_quality);
  out.pose.tracking_lost = kf->tracking_lost;
  out.intrinsics.fx = kf->fx;
  out.intrinsics.fy = kf->fy;
  out.intrinsics.cx = kf->cx;
  out.intrinsics.cy = kf->cy;
  for (int i = 0; i < 5; ++i) out.intrinsics.distortion[i] = kf->distortion[i];
  out.intrinsics.width = kf->width;
  out.intrinsics.height = kf->height;
  out.intrinsics.rolling_shutter_row_time_ns = kf->rolling_shutter_row_time_ns;
  out.flags = kf->flags;
  out.exposure_duration_ns = kf->exposure_ns;
  out.iso = kf->iso;
  out.angular_rate_rad_s = kf->angular_rate_rad_s;
  out.linear_speed_m_s = kf->linear_speed_m_s;
  out.image_bytes = kf->image_bytes;

  return to_c(handle_of(engine)->engine->record_keyframe(out));
  SCAN_GUARD_END
}

// --- colorization (A11), ABI 4 ----------------------------------------------

scan_error_t scan_colorizer_create(scan_colorizer** out, const scan_colorize_config* cfg) {
  SCAN_GUARD_BEGIN
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "out handle is null");
  color::ColorizeConfig c;  // S6-derived defaults for everything not exposed
  if (cfg != nullptr) {
    c.sync_quality = static_cast<SyncQuality>(cfg->sync_quality);
    c.allow_poor_sync = cfg->allow_poor_sync != 0;
    c.pose_frame = static_cast<color::KeyframePoseFrame>(cfg->pose_frame);
    c.motion_gate_deg_s = cfg->motion_gate_deg_s;
    c.motion_reject_deg_s = cfg->motion_reject_deg_s;
    c.camera_clock_offset_ns = cfg->camera_clock_offset_ns;
    // 0 means "keep the default" for the two ranges: a zero-initialized
    // struct must not silently become "min_range 0, max_range 0", which
    // colours nothing and looks like a bug in the colorizer.
    if (cfg->min_range_m > 0.f) c.min_range_m = cfg->min_range_m;
    if (cfg->max_range_m > 0.f) c.max_range_m = cfg->max_range_m;
    c.occlusion_test = cfg->occlusion_test != 0;
    c.rolling_shutter = cfg->rolling_shutter != 0;
    c.estimate_normals = cfg->estimate_normals != 0;
    c.low_confidence_alpha = cfg->low_confidence_alpha;
    c.uncovered_alpha = cfg->uncovered_alpha;
  }
  *out = reinterpret_cast<scan_colorizer*>(new ColorizerHandle(c));
  return SCAN_OK;
  SCAN_GUARD_END
}

void scan_colorizer_destroy(scan_colorizer* c) {
  if (c == nullptr) return;
  delete colorizer_of(c);
}

scan_error_t scan_colorizer_load_keyframes(scan_colorizer* c, const char* lscan_dir) {
  SCAN_GUARD_BEGIN
  if (c == nullptr || lscan_dir == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  return to_c(colorizer_of(c)->impl.load_keyframes(lscan_dir));
  SCAN_GUARD_END
}

scan_error_t scan_colorizer_set_extrinsics(scan_colorizer* c, const double camera_from_lidar[16]) {
  SCAN_GUARD_BEGIN
  if (c == nullptr || camera_from_lidar == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  return to_c(colorizer_of(c)->impl.set_extrinsics(camera_from_lidar));
  SCAN_GUARD_END
}

scan_error_t scan_colorizer_set_progress_callback(scan_colorizer* c, scan_colorize_progress_cb cb,
                                                  void* user_data) {
  SCAN_GUARD_BEGIN
  if (c == nullptr) return fail(ScanError::kInvalidArgument, "colorizer handle is null");
  ColorizerHandle* h = colorizer_of(c);
  h->progress_cb = cb;
  h->progress_user = user_data;
  if (cb == nullptr) {
    h->impl.set_progress_fn(nullptr);
  } else {
    // A trampoline, never a function-pointer cast (DESIGN.md §4): the C
    // callback takes (float, void*) and the C++ one a std::function<void(float)>.
    h->impl.set_progress_fn([h](float f) { h->progress_cb(f, h->progress_user); });
  }
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_colorizer_run(scan_colorizer* c, scan_engine* engine) {
  SCAN_GUARD_BEGIN
  if (c == nullptr || engine == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  return to_c(colorizer_of(c)->impl.colorize(&handle_of(engine)->engine->points()));
  SCAN_GUARD_END
}

scan_error_t scan_colorizer_cancel(scan_colorizer* c) {
  SCAN_GUARD_BEGIN
  if (c == nullptr) return fail(ScanError::kInvalidArgument, "colorizer handle is null");
  colorizer_of(c)->impl.cancel();
  return SCAN_OK;
  SCAN_GUARD_END
}

float scan_colorizer_progress(scan_colorizer* c) {
  if (c == nullptr) return 0.f;
  return colorizer_of(c)->impl.progress();
}

scan_error_t scan_colorizer_stats(scan_colorizer* c, scan_colorize_stats* out) {
  SCAN_GUARD_BEGIN
  if (c == nullptr || out == nullptr) return fail(ScanError::kInvalidArgument, "null argument");
  const color::ColorizeStats& s = colorizer_of(c)->impl.stats();
  std::memset(out, 0, sizeof(*out));
  out->points_total = s.points_total;
  out->points_colorized = s.points_colorized;
  out->points_low_confidence = s.points_low_confidence;
  out->points_uncovered = s.points_uncovered;
  out->keyframes_total = s.keyframes_total;
  out->keyframes_used = s.keyframes_used;
  out->keyframes_rejected_motion = s.keyframes_rejected_motion;
  out->keyframes_rejected_pose = s.keyframes_rejected_pose;
  out->keyframes_outside_cloud = s.keyframes_outside_cloud;
  out->keyframes_image_failed = s.keyframes_image_failed;
  out->ms_total = s.ms_total;
  out->coverage_fraction = s.coverage_fraction();
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- the wizard's clock sweep (A11, for B7), ABI 4 ---------------------------

scan_error_t scan_clock_sweep_estimate(const scan_rate_sample* camera, uint32_t n_camera,
                                       const scan_rate_sample* lidar, uint32_t n_lidar,
                                       scan_clock_sweep_result* out) {
  SCAN_GUARD_BEGIN
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "out is null");
  std::memset(out, 0, sizeof(*out));
  out->verdict = SCAN_SWEEP_TOO_FEW_SAMPLES;
  if ((camera == nullptr && n_camera != 0) || (lidar == nullptr && n_lidar != 0)) {
    return fail(ScanError::kInvalidArgument, "null rate track");
  }

  std::vector<color::RateSample> cam(n_camera), lid(n_lidar);
  for (uint32_t i = 0; i < n_camera; ++i) {
    cam[i].t_ns = camera[i].t_ns;
    cam[i].value = camera[i].value;
  }
  for (uint32_t i = 0; i < n_lidar; ++i) {
    lid[i].t_ns = lidar[i].t_ns;
    lid[i].value = lidar[i].value;
  }

  color::ClockSweepResult r;
  const Status st = color::estimate_clock_offset(
      Span<const color::RateSample>(cam.data(), cam.size()),
      Span<const color::RateSample>(lid.data(), lid.size()), color::ClockSweepConfig(), &r);
  // A REFUSED CAPTURE IS NOT AN ERROR: the verdict is what the wizard shows
  // the operator ("you did not sweep"), so it is written out either way and
  // only structurally bad input returns non-OK.
  out->offset_ns = r.offset_ns;
  out->correlation = r.correlation;
  out->rival_correlation = r.rival_correlation;
  out->sigma_ns = r.sigma_ns;
  out->grid_samples = r.grid_samples;
  out->zero_crossings = r.zero_crossings;
  out->overlap_ns = r.overlap_ns;
  out->accepted = r.accepted ? 1 : 0;
  out->verdict = static_cast<std::uint8_t>(r.verdict);
  return to_c(st);
  SCAN_GUARD_END
}

void scan_engine_set_log_callback(scan_log_cb cb, void* user_data, int32_t min_level) {
  set_log_min_level(static_cast<LogLevel>(min_level));
  if (cb == nullptr) {
    set_log_sink(nullptr, nullptr);
    return;
  }
  // The C sink signature takes int32_t where the C++ one takes LogLevel;
  // both are int32_t-sized, but we route through a trampoline rather than
  // casting the function pointer.
  struct Trampoline {
    static void call(LogLevel level, const char* module, const char* message, void* user) {
      auto* pair = static_cast<std::pair<scan_log_cb, void*>*>(user);
      pair->first(static_cast<std::int32_t>(level), module, message, pair->second);
    }
  };
  static std::pair<scan_log_cb, void*> s_sink;  // process-global, like the log facade
  s_sink = {cb, user_data};
  set_log_sink(&Trampoline::call, &s_sink);
}

// --- device auto-discovery (A16), ABI 6 --------------------------------------

scan_error_t scan_discover_mid360(uint32_t timeout_ms, scan_mid360_beacon* out,
                                  uint32_t capacity, uint32_t* out_count) {
  SCAN_GUARD_BEGIN
  if (out_count == nullptr || (out == nullptr && capacity != 0)) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  *out_count = 0;
  discovery::DiscoverOptions opt;
  opt.timeout_ms = static_cast<int>(timeout_ms > 600000u ? 600000u : timeout_ms);
  Result<std::vector<discovery::Mid360Beacon>> r = discovery::DiscoverMid360(opt);
  if (!r.ok()) return to_c(r.status());

  const std::vector<discovery::Mid360Beacon>& found = r.value();
  *out_count = static_cast<std::uint32_t>(found.size());
  const std::uint32_t n = std::min<std::uint32_t>(capacity, *out_count);
  for (std::uint32_t i = 0; i < n; ++i) {
    const discovery::Mid360Beacon& b = found[i];
    scan_mid360_beacon& d = out[i];
    std::memset(&d, 0, sizeof(d));
    copy_str(d.sn, sizeof(d.sn), b.sn);
    copy_str(d.dev_type, sizeof(d.dev_type), b.dev_type);
    copy_str(d.fw_version, sizeof(d.fw_version), b.fw_version);
    copy_str(d.fw_version_text, sizeof(d.fw_version_text), b.fw_version_text);
    copy_str(d.build_time, sizeof(d.build_time), b.build_time);
    copy_str(d.mac, sizeof(d.mac), b.mac);
    copy_str(d.lidar_ip, sizeof(d.lidar_ip), b.lidar_ip);
    copy_str(d.netmask, sizeof(d.netmask), b.netmask);
    copy_str(d.gateway, sizeof(d.gateway), b.gateway);
    copy_str(d.persisted_host_ip, sizeof(d.persisted_host_ip), b.persisted_host_ip);
    copy_str(d.source_ip, sizeof(d.source_ip), b.source_ip);
    d.persisted_point_port = b.persisted_point_port;
    d.persisted_imu_port = b.persisted_imu_port;
    d.push_port_seen = b.push_port_seen;
    d.key_count = static_cast<std::uint16_t>(b.key_count);
    d.t_last_seen_ns = b.t_last_seen_ns;
    d.beacons_seen = b.beacons_seen;
    d.crc_ok = b.crc_ok ? 1 : 0;
    d.heuristic = b.heuristic ? 1 : 0;
  }
  if (n < *out_count) {
    return to_c(set_last_error(ScanError::kCapacityExceeded, "%u of %u lidars written", n,
                               *out_count));
  }
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_host_check(const scan_mid360_beacon* beacon, scan_host_check_result* out) {
  SCAN_GUARD_BEGIN
  if (beacon == nullptr || out == nullptr) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  // Only the four fields the check reads cross back over. Rebuilding the C++
  // struct field by field rather than reinterpret_cast'ing keeps the two
  // layouts free to diverge (convention 6's spirit for structs).
  discovery::Mid360Beacon b;
  b.lidar_ip = beacon->lidar_ip;
  b.netmask = beacon->netmask;
  b.gateway = beacon->gateway;
  b.persisted_host_ip = beacon->persisted_host_ip;

  const discovery::HostCheck hc = discovery::CheckHostReachability(b);
  std::memset(out, 0, sizeof(*out));
  out->host_ip_is_local = hc.host_ip_is_local ? 1 : 0;
  out->on_lidar_subnet = hc.on_lidar_subnet ? 1 : 0;
  const std::uint32_t n = std::min<std::uint32_t>(
      SCAN_HOST_CHECK_MAX_CANDIDATES, static_cast<std::uint32_t>(hc.local_candidates.size()));
  for (std::uint32_t i = 0; i < n; ++i) {
    copy_str(out->candidates[i], sizeof(out->candidates[i]), hc.local_candidates[i]);
  }
  out->candidate_count = n;
  copy_str(out->suggested_host_ip, sizeof(out->suggested_host_ip), hc.suggested_host_ip);
  copy_str(out->suggested_interface, sizeof(out->suggested_interface), hc.suggested_interface);
  copy_str(out->note, sizeof(out->note), hc.note);
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_enumerate_serial(scan_serial_port* out, uint32_t capacity,
                                   uint32_t* out_count) {
  SCAN_GUARD_BEGIN
  if (out_count == nullptr || (out == nullptr && capacity != 0)) {
    return fail(ScanError::kInvalidArgument, "null argument");
  }
  const std::vector<std::string> ports = discovery::EnumerateSerialPorts();
  *out_count = static_cast<std::uint32_t>(ports.size());
  const std::uint32_t n = std::min<std::uint32_t>(capacity, *out_count);
  for (std::uint32_t i = 0; i < n; ++i) {
    std::memset(&out[i], 0, sizeof(out[i]));
    copy_str(out[i].path, sizeof(out[i].path), ports[i]);
  }
  if (n < *out_count) {
    return to_c(set_last_error(ScanError::kCapacityExceeded, "%u of %u ports written", n,
                               *out_count));
  }
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_probe_d6(const char* const* ports, uint32_t n_ports, uint32_t per_port_ms,
                           scan_d6_probe* out) {
  SCAN_GUARD_BEGIN
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "null argument");
  std::vector<std::string> paths;
  const Status s = collect_ports(ports, n_ports, &paths);
  if (!s.ok()) return to_c(s);

  std::optional<discovery::D6Probe> p =
      discovery::ProbeSerialD6(paths, static_cast<int>(per_port_ms));
  if (!p.has_value()) {
    std::memset(out, 0, sizeof(*out));
    return to_c(set_last_error(ScanError::kNotFound, "no COIN-D6 on %u port(s)", n_ports));
  }
  std::memset(out, 0, sizeof(*out));
  copy_str(out->port, sizeof(out->port), p->port);
  out->baud = p->baud;
  out->packets_ok = p->packets_ok;
  out->packets_bad_checksum = p->packets_bad_checksum;
  out->used_start_command = p->used_start_command ? 1 : 0;
  return SCAN_OK;
  SCAN_GUARD_END
}

scan_error_t scan_probe_um982(const char* const* ports, uint32_t n_ports, uint32_t per_port_ms,
                              scan_um982_probe* out) {
  SCAN_GUARD_BEGIN
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "null argument");
  std::vector<std::string> paths;
  const Status s = collect_ports(ports, n_ports, &paths);
  if (!s.ok()) return to_c(s);

  std::optional<discovery::Um982Probe> p =
      discovery::ProbeSerialUm982(paths, static_cast<int>(per_port_ms));
  if (!p.has_value()) {
    std::memset(out, 0, sizeof(*out));
    return to_c(set_last_error(ScanError::kNotFound, "no UM982 on %u port(s)", n_ports));
  }
  std::memset(out, 0, sizeof(*out));
  copy_str(out->port, sizeof(out->port), p->port);
  out->baud = p->baud;
  out->sentences_ok = p->sentences_ok;
  out->sentences_bad = p->sentences_bad;
  out->has_heading = p->has_heading ? 1 : 0;
  return SCAN_OK;
  SCAN_GUARD_END
}

// --- single-instance guard (A16), ABI 6 --------------------------------------
//
// The C surface is process-global where the C++ one is RAII, because there is
// no handle a JNI caller could keep alive across a JNI_OnLoad boundary without
// inventing an owner. The guard itself is a function-local static, so it is
// constructed on first use and released either by scan_instance_release() or
// by static destruction at process exit.
scan_error_t scan_instance_acquire(const char* app_id, const char* lock_path,
                                   int64_t* out_holder_pid) {
  SCAN_GUARD_BEGIN
  InstanceGuardOptions opt;
  if (app_id != nullptr && app_id[0] != '\0') opt.app_id = app_id;
  if (lock_path != nullptr && lock_path[0] != '\0') opt.lock_path = lock_path;
  InstanceGuard& g = capi_instance_guard();
  const Status s = g.Acquire(opt);
  if (out_holder_pid != nullptr) *out_holder_pid = g.holder_pid();
  return to_c(s);
  SCAN_GUARD_END
}

void scan_instance_release(void) {
  try {
    capi_instance_guard().Release();
  } catch (...) {
    // Release() cannot throw; the catch exists so this stays noexcept-shaped
    // like every other void entry point.
  }
}

int64_t scan_current_process_id(void) { return CurrentProcessId(); }

// ===== ROUND 13 (ABI 10) ===================================================

namespace {

int32_t mount_verdict_to_c(scanengine::post::MountWatchVerdict v) {
  switch (v) {
    case scanengine::post::MountWatchVerdict::kOk: return SCAN_MOUNT_OK;
    case scanengine::post::MountWatchVerdict::kNotMeasurable: return SCAN_MOUNT_NOT_MEASURABLE;
    case scanengine::post::MountWatchVerdict::kSuspect: return SCAN_MOUNT_SUSPECT;
    case scanengine::post::MountWatchVerdict::kMismatch: return SCAN_MOUNT_MISMATCH;
  }
  return SCAN_MOUNT_NOT_MEASURABLE;
}

}  // namespace

scan_error_t scan_lscan_reprocess_d6(const char* lscan_dir, const scan_reprocess_options* opts,
                                     scan_reprocess_result* out,
                                     scan_reprocess_progress_cb progress, void* user_data) {
  SCAN_GUARD_BEGIN
  if (lscan_dir == nullptr || lscan_dir[0] == '\0') {
    return fail(ScanError::kInvalidArgument, "lscan_dir is null or empty");
  }
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "out is null");
  std::memset(out, 0, sizeof(*out));

  scanengine::post::ReprocessOptions ro;
  if (opts != nullptr) {
    ro.stitch_sections = opts->stitch_sections != 0;
    ro.close_loops = opts->close_loops != 0;
    ro.densify_with_phone_imu = opts->densify_with_phone_imu != 0;
    ro.sections.refine = opts->refine_seams != 0;
    if (opts->max_refine_translation_m > 0.0) {
      ro.sections.max_refine_translation_m = opts->max_refine_translation_m;
    }
  }

  // The cancel token has to outlive the callback, and the callback is the
  // ONLY way a C caller can ask to stop: there is no handle to cancel through
  // because a reprocess owns nothing the caller can hold.
  scanengine::post::CancelToken token;
  scanengine::post::PostProgressFn fn;
  if (progress != nullptr) {
    fn = [progress, user_data, &token](const scanengine::post::PostProgress& p) {
      if (progress(p.fraction, user_data) == 0) token.cancel();
    };
  }

  scanengine::post::ReprocessReport rep;
  const Status st = scanengine::post::reprocess_d6_container(
      lscan_dir, ro, &rep, fn, progress != nullptr ? &token : nullptr);

  out->ran = rep.ran ? 1 : 0;
  out->map_written = rep.map_written ? 1 : 0;
  out->sections = static_cast<uint32_t>(rep.stitch.sections);
  out->seams = static_cast<uint32_t>(rep.stitch.seams.size());
  uint32_t refined = 0;
  for (const scanengine::post::SectionSeam& x : rep.stitch.seams) {
    if (x.decision == scanengine::post::SeamDecision::kRefined) ++refined;
  }
  out->seams_refined = refined;
  out->points = rep.points;
  out->poses = rep.poses;
  out->poses_untracked = rep.stitch.poses_untracked;
  out->first_section_moved_m = rep.stitch.total_translation_m;
  out->first_section_moved_deg = rep.stitch.total_rotation_deg;
  out->vertical_extent_before_m = rep.stitch.trajectory_vertical_extent_before_m;
  out->vertical_extent_after_m = rep.stitch.trajectory_vertical_extent_after_m;
  out->end_gap_before_m = rep.stitch.trajectory_end_gap_before_m;
  out->end_gap_after_m = rep.stitch.trajectory_end_gap_after_m;
  out->mount_verdict = mount_verdict_to_c(rep.mount.verdict);
  out->mount_impossible_fraction = rep.mount.impossible_fraction;
  out->mount_revolution_extent_m = rep.mount.median_revolution_extent_m;
  return to_c(st);
  SCAN_GUARD_END
}

int32_t scan_lscan_has_stitched_cloud(const char* lscan_dir) {
  try {
    if (lscan_dir == nullptr || lscan_dir[0] == '\0') return 0;
    return scanengine::post::has_stitched_cloud(lscan_dir) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

scan_error_t scan_lscan_mount_check(const char* lscan_dir, double window_seconds,
                                    scan_mount_check_result* out) {
  SCAN_GUARD_BEGIN
  if (lscan_dir == nullptr || lscan_dir[0] == '\0') {
    return fail(ScanError::kInvalidArgument, "lscan_dir is null or empty");
  }
  if (out == nullptr) return fail(ScanError::kInvalidArgument, "out is null");
  std::memset(out, 0, sizeof(*out));
  out->verdict = SCAN_MOUNT_NOT_MEASURABLE;

  PageStoreConfig psc;
  psc.page_capacity = 1u << 20;
  psc.max_pages = 4096;
  PageStore store(psc);
  std::vector<scanengine::post::TrajPose> traj;
  std::vector<std::int64_t> ptimes;
  scanengine::post::D6ResolveConfig cfg;
  cfg.store = &store;
  cfg.out_trajectory = &traj;
  cfg.out_point_times = &ptimes;
  scanengine::post::D6ResolvePipeline pipe(cfg);
  const Status st = pipe.run(lscan_dir);
  if (!st.ok()) return to_c(st);

  std::vector<PointVertex> pts;
  pts.reserve(static_cast<std::size_t>(store.total_points()));
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) pts.push_back(v.data[k]);
  }
  scanengine::post::MountWatchConfig mc;
  mc.window_seconds = window_seconds;
  const scanengine::post::MountWatchReport r = scanengine::post::check_mount_consistency(
      traj, Span<const PointVertex>(pts.data(), pts.size()),
      Span<const std::int64_t>(ptimes.data(), ptimes.size()), mc);
  out->verdict = mount_verdict_to_c(r.verdict);
  out->revolutions = static_cast<uint32_t>(r.revolutions);
  out->points = r.points;
  out->median_revolution_extent_m = r.median_revolution_extent_m;
  out->impossible_fraction = r.impossible_fraction;
  out->median_range_m = r.median_range_m;
  return to_c(kOkStatus);
  SCAN_GUARD_END
}

}  // extern "C"
