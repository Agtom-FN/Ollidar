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
#include <string>
#include <utility>
#include <vector>

#include "scanengine/core/engine.h"
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
SCAN_CHECK_ENUM(SCAN_EVENT_JOB_PROGRESS, EventType::kJobProgress);
SCAN_CHECK_ENUM(SCAN_EVENT_ERROR, EventType::kError);

static_assert(sizeof(scan_point_vertex) == sizeof(PointVertex),
              "scan_point_vertex must match PointVertex (16 B, S3-proven layout)");
static_assert(sizeof(scan_point_vertex) == 16, "scan_point_vertex must be 16 bytes");
static_assert(SCAN_ABI_VERSION == kEngineAbiVersion, "ABI version mismatch");

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
    default:
      // Types whose payload the ABI does not mirror yet (gnss, job — A10/A15)
      // travel as raw bytes; the consumer must not interpret them until this
      // switch grows a case.
      std::memcpy(out->payload.raw, in.payload.raw, sizeof(out->payload.raw));
      break;
  }
}

void event_trampoline(const Event& ev, void* user) {
  auto* h = static_cast<EngineHandle*>(user);
  if (h->event_cb == nullptr) return;
  scan_event out;
  convert_event(ev, &out);
  h->event_cb(&out, h->event_user);
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
    case DeviceKind::kMid360:
      if (cfg->lidar_ip != nullptr) dc.mid360.udp.lidar_ip = cfg->lidar_ip;
      if (cfg->host_ip != nullptr) dc.mid360.udp.host_ip = cfg->host_ip;
      break;
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

}  // extern "C"
