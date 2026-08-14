// engine.h — the engine's C++ API. The Qt desktop app links this directly
// (same process, no FFI); Android reaches it through capi/scanengine_c.h.
//
// THREADING MODEL (the short version; DESIGN.md has the long one)
//   The Engine owns NO threads of its own. Work happens on the thread that
//   pushes data in (the app's serial reader) and on whatever threads later
//   modules introduce — A3's SDK2 receive thread, A6's odometry thread,
//   A15's job workers. Every such module must document its thread and reach
//   the rest of the engine only through the two thread-safe hubs: EventBus
//   and PageStore. The Engine's own methods are safe to call from any
//   thread; lifecycle calls (start_session/stop_session/add_device) are
//   serialized by one mutex and are expected from the app's control thread.
//   The one thread the Engine can cause to exist is A6's optional LIO
//   odometry thread (SessionConfig::live_slam + LioConfig::internal_thread);
//   it belongs to LioOdometry, not to the Engine. See DESIGN.md §2.
//
// LIFECYCLE
//   create() → kIdle → start_session() → kRunning → stop_session() → kIdle
//   Devices may be added in either kIdle or kRunning; a device added while
//   running is started immediately. Errors during a session put the offending
//   DEVICE into kFault; the engine only reaches kFaulted if a session-level
//   invariant breaks (e.g. the recorder cannot write).
//
// Owner: A1.
#ifndef SCANENGINE_CORE_ENGINE_H
#define SCANENGINE_CORE_ENGINE_H

#include <memory>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/core/log.h"
#include "scanengine/core/types.h"
#include "scanengine/drivers/d6/d6_driver.h"
#include "scanengine/drivers/mid360/mid360_driver.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/record/lscan.h"
#include "scanengine/slam/lio.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"
#include "scanengine/timesync/imu_ingest.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

// Bumped whenever the C ABI changes shape (see capi/scanengine_c.h).
// 2 (INT-24): poses + pushbroom + mount calibration + live-SLAM session knobs.
inline constexpr std::uint32_t kEngineAbiVersion = 2;
const char* engine_version_string();  // "scanengine 0.1.0 (<clock backend>)"

struct EngineConfig {
  std::string app_name = "scanengine";
  LogLevel log_level = LogLevel::kInfo;
  PageStoreConfig points{};
  std::uint32_t event_queue_capacity = 1024;  // for the built-in app subscription
};

// The Engine's pushbroom defaults, which differ from PushbroomConfig's own in
// exactly one field: assembled points are WORLD-frame and must not land in the
// same pages as D6Driver's sensor-frame live preview, so they go out on
// StreamId::kSlamMap (pages are single-stream, so this also keeps provenance
// intact for A9/A13). Everything else is A8's default.
inline PushbroomConfig engine_pushbroom_defaults() {
  PushbroomConfig c;
  c.out_stream = StreamId::kSlamMap;
  return c;
}

struct SessionConfig {
  // .lscan directory. Empty means "do not record" — legal, but it breaks
  // Tech Spec §3's record-always rule, so it is only for tests and live
  // previews. The Engine logs a warning when it is empty.
  std::string lscan_dir;
  std::string profile = "quickscan";  // survey | floorplan | research | quickscan
  bool record = true;

  // --- Tech Spec §3.1's "Live-SLAM vs Record-only" capture toggle ----------
  //
  // false (the default) is Record-only: the raw streams are decoded, recorded
  // and previewed, and nothing runs odometry. true starts one LioOdometry for
  // the session's Mid-360 stream; its registered map is published on
  // StreamId::kSlamMap through the Engine's own PageStore, so the app renders
  // it with no new code, and its trajectory is available through
  // Engine::live_slam()->poses().
  //
  // Record-always still holds either way: live SLAM is one more consumer of a
  // stream that has already hit the .lscan (DESIGN §5, spec §3 rule 2), and a
  // phone that thermally throttles can drop back to Record-only mid-project
  // without losing a byte.
  bool live_slam = false;

  // A6's knobs. `map_store` and `map_stream` are overwritten by the Engine
  // (they are the wiring, not a choice); everything else is the caller's.
  LioConfig lio{};

  // --- D6 pushbroom (§3.3) ------------------------------------------------
  //
  // Turn the D6's 2-D profiles into a 3-D cloud using the pushed pose stream
  // (ARCore, RTK, or a replayed trajectory) and the mount extrinsic. Off by
  // default: without a calibrated extrinsic there is nothing to assemble.
  // Also switchable mid-session with Engine::set_pushbroom_enabled().
  bool pushbroom = false;
  PushbroomConfig pushbroom_cfg = engine_pushbroom_defaults();
};

struct DeviceConfig {
  DeviceKind kind = DeviceKind::kUnknown;
  D6Config d6{};
  Mid360Config mid360{};
};

class Engine {
 public:
  static Result<std::unique_ptr<Engine>> create(const EngineConfig& cfg);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // --- lifecycle ---------------------------------------------------------
  EngineState state() const;
  Status start_session(const SessionConfig& cfg);
  Status stop_session();
  bool session_active() const;
  const SessionConfig& session_config() const;
  std::uint64_t session_id() const;

  // --- devices -----------------------------------------------------------
  Result<DeviceId> add_device(const DeviceConfig& cfg);
  Status remove_device(DeviceId id);
  std::vector<DeviceId> device_ids() const;
  Result<DeviceHealth> device_health(DeviceId id) const;

  // App → engine bytes for push-mode transports (D6 over USB serial).
  // t_arrival {0} means "stamp now".
  Status push_serial_bytes(DeviceId id, ByteSpan bytes, TimePoint t_arrival = TimePoint{0});

  // --- trajectory in (A8) -------------------------------------------------
  //
  // The app's pose stream: ARCore on Android, an RTK/replayed trajectory on
  // desktop. Safe from any thread — the pose ring is internally synchronized,
  // so poses arrive on the app's AR thread while points are decoded on the
  // serial reader. Every accepted pose raises EventType::kPoseUpdate.
  //
  // Confidence: the one-argument form derives it from quality/tracking_lost
  // (pose_confidence()); pass an explicit value in [0, 1] when the source has
  // a real scalar (RTK fix quality, a filter covariance).
  Status push_pose(const Pose& pose);
  Status push_pose(const Pose& pose, float confidence);

  // Interpolated lookup with the GATE, not just a Status: the five outcomes
  // in poses/pose_interpolator.h are what §3.3's "flagged and excluded by
  // default" is decided on. `sample.has_pose == false` means there is nothing
  // to interpolate yet.
  PoseSample pose_at(std::int64_t t_mono_ns) const;
  ExternalPoseSource& poses();
  const ExternalPoseSource& poses() const;

  // --- D6 pushbroom (A8) --------------------------------------------------
  //
  // `phone_from_lidar` is ROW-MAJOR 4x4 and must be rigid; a column-major
  // matrix handed across JNI is rejected (kInvalidArgument) rather than
  // silently producing a mirrored cloud. This is the same transform the
  // mount-calibration solver calls `camera_from_lidar`.
  Status set_mount_extrinsics(const double phone_from_lidar[16]);
  Status set_pushbroom_enabled(bool on);
  bool pushbroom_enabled() const;
  // Resolve what the poses allow and push the batch into the PageStore. Called
  // automatically by stop_session(); call it directly to force a partial batch
  // out during a live preview.
  Status pushbroom_flush();
  PushbroomStats pushbroom_stats() const;

  // --- live SLAM (A6) -----------------------------------------------------
  //
  // Non-null only between start_session(live_slam = true) and stop_session().
  // The trajectory is live_slam()->poses(); the map is this Engine's own
  // PageStore, on StreamId::kSlamMap.
  LioOdometry* live_slam();
  const LioOdometry* live_slam() const;

  // The single offset estimator the Mid-360's points AND its IMU are mapped
  // through (StreamId::kLidarMid360). One estimator rather than two because
  // the device stamps both from one clock, and a second estimator would inject
  // independent estimation noise BETWEEN the two streams — exactly the
  // quantity undistortion is sensitive to (docs/A6-lio.md §7.2).
  ImuIngest& imu();

  // --- shared services ---------------------------------------------------
  EventBus& events();
  PageStore& points();
  TimeSync& timesync();
  lscan::RecordWriter& recorder();
  // A5 seam: replace the recorder (e.g. NullRecordWriter in tests that must
  // not touch disk). Only valid while no session is running; a null pointer
  // is ignored.
  void set_recorder(std::unique_ptr<lscan::RecordWriter> w);

  // The built-in subscription the C ABI's poll_event()/set_event_callback()
  // drive. C++ consumers should make their own subscription instead.
  SubscriptionId app_subscription() const;
  Status set_app_event_callback(EventCallback cb, void* user_data);

 private:
  Engine();
  void publish_pose_(const Pose& pose);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_CORE_ENGINE_H
