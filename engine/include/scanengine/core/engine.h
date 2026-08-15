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
//   The Engine can cause two threads to exist, and owns neither: A6's optional
//   LIO odometry thread (SessionConfig::live_slam +
//   LioConfig::internal_thread), which belongs to LioOdometry, and A10's NTRIP
//   receive thread, which belongs to TcpNtripClient and exists only between
//   ntrip().connect() and ntrip().disconnect(). See DESIGN.md §2.
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
#include "scanengine/color/colorize.h"
#include "scanengine/core/error.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/core/log.h"
#include "scanengine/core/types.h"
#include "scanengine/drivers/d6/d6_driver.h"
#include "scanengine/drivers/mid360/mid360_driver.h"
#include "scanengine/gnss/georef.h"
#include "scanengine/gnss/gnss_source.h"
#include "scanengine/gnss/ntrip_client.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/record/lscan.h"
#include "scanengine/slam/lio.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"
#include "scanengine/timesync/imu_ingest.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

// A15's job queue, reachable through Engine::jobs() below. Forward-declared
// rather than included: jobs/job_queue.h transitively pulls in the post
// pipeline, the exporter, the colorizer and the cloud client, and engine.h is
// included by every consumer of the engine.
namespace jobs {
class JobQueue;
}  // namespace jobs

// Bumped whenever the C ABI changes shape (see capi/scanengine_c.h).
// 2 (INT-24): poses + pushbroom + mount calibration + live-SLAM session knobs.
// 3 (INT-29): A10's GNSS/RTK stack — NMEA in, fix/NTRIP/georef events, the
//             NTRIP client handle, the georef solution and the session CRS.
// 4 (INT-34): A11's colorization (scan_engine_record_keyframe, the
//             scan_colorizer_* handle, scan_clock_sweep_estimate) and A15's
//             jobs (the kJobProgress union case + SCAN_JOB_* mirror), plus
//             `update_kind` on the points payload.
inline constexpr std::uint32_t kEngineAbiVersion = 4;
const char* engine_version_string();  // "scanengine 0.1.0 (<clock backend>)"

struct EngineConfig {
  std::string app_name = "scanengine";
  LogLevel log_level = LogLevel::kInfo;
  PageStoreConfig points{};
  std::uint32_t event_queue_capacity = 1024;  // for the built-in app subscription

  // --- A10: the GNSS/RTK stack -------------------------------------------
  //
  // Engine-lifetime, not per session, for the same reason the pose source is:
  // an operator pairs the rover and watches the fix go Fixed BEFORE pressing
  // record, and §3.4's capture gate is exactly that pre-session decision.
  // `timesync`, `stream` and the georef estimator's local source are wiring
  // and are overwritten by Engine::create(); everything else is the caller's.
  GnssSourceConfig gnss{};
  GeorefConfig georef{};
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

// Which trajectory the D6 pushbroom assembles against (Tech Spec §3.3).
//
//   kExternal  the pushed pose stream — ARCore on Android, a replayed track.
//   kGnss      the RTK rover's own trajectory. §3.3's "Desktop D6 capture: no
//              ARCore -> RTK-trajectory mode only". No new code path: GnssSource
//              implements the same PoseInterpolator the assembler already
//              consumes, so this is configuration (§3 key rule 3).
enum class TrajectorySource : std::uint8_t {
  kExternal = 0,
  kGnss = 1,
};

const char* to_string(TrajectorySource s) noexcept;

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

  // Which pose stream the assembler interpolates against. Applied at
  // start_session(); also switchable mid-session with
  // Engine::set_trajectory_source() for a rig that walks out of the building.
  TrajectorySource trajectory = TrajectorySource::kExternal;
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

  // Swap the assembler's trajectory between the pushed pose stream and the
  // RTK rover's. Takes effect for points assembled after the call; points
  // already pending resolve against the NEW source, which is what an indoor →
  // outdoor handover wants.
  Status set_trajectory_source(TrajectorySource src);
  TrajectorySource trajectory_source() const;

  // --- GNSS / RTK (A10) ---------------------------------------------------
  //
  // One GnssSource, one TcpNtripClient and one GeorefFusion per Engine, all
  // alive for the Engine's whole lifetime: an operator pairs the rover, joins
  // a caster and waits for RTK Fixed BEFORE pressing record, and §3.4's
  // capture gate is that pre-session decision.
  //
  // The three are wired to each other by Engine::create():
  //   * every closed epoch's fix raises EventType::kGnssFix and is offered to
  //     the georef fusion;
  //   * the fusion's ENU frame is the GnssSource's, anchored on the first fix
  //     at or above GnssSourceConfig::min_fix_for_origin;
  //   * the NTRIP client uploads the rover's OWN last GGA verbatim;
  //   * every NTRIP state change raises EventType::kNtripState, and the
  //     session's first convergence raises EventType::kGeorefConverged.
  //
  // NMEA reaches the source through push_serial_bytes() on a kRtkRover device
  // (record-always: the raw bytes hit the .lscan as kGnssNmea chunks before
  // they are parsed). RTCM3 leaves through the NTRIP client's rover callback,
  // which the app points at its Bluetooth socket; the engine records every
  // forwarded frame as a kGnssRtcm chunk on the way past.
  GnssSource& gnss();
  const GnssSource& gnss() const;
  TcpNtripClient& ntrip();
  const TcpNtripClient& ntrip() const;
  GeorefFusion& georef();
  const GeorefFusion& georef() const;

  // Where corrections go. The app supplies the one thing the engine cannot
  // have: the write function to the rover's Bluetooth/serial socket. Whole,
  // CRC-valid RTCM3 frames only, on the NTRIP receive thread, with no client
  // lock held — it must be quick and must not re-enter the client.
  //
  // Install it HERE rather than on ntrip() directly: the Engine keeps its own
  // handler on the client (that is what records the frames), and
  // NtripClient::set_rtcm_callback() would replace it.
  using RtcmSink = void (*)(ByteSpan rtcm, void* user_data);
  void set_rtcm_sink(RtcmSink cb, void* user_data);

  GnssFix last_fix() const;
  GnssStats gnss_stats() const;
  GeorefSolution georef_solution() const;

  // The A9 export seam: hand straight to `ExportOptions::crs_wkt` /
  // `crs_epsg`. Both are EMPTY until the georef transform converges — which is
  // a stricter gate than georef().crs_wkt(), and on purpose. The fusion's
  // version answers "what CRS is this site in" and needs only an origin; these
  // two answer "what CRS may I LABEL this cloud with", and a cloud still in the
  // local frame must not be labelled UTM. Empty is exactly the input A9
  // documents as "embed the local-frame placeholder".
  std::string crs_wkt() const;
  std::string crs_epsg() const;  // "EPSG:32650"

  // Where the georef fusion reads the LOCAL trajectory. Defaults to this
  // Engine's ExternalPoseSource. It must NOT be the GnssSource — pairing GNSS
  // against GNSS is degenerate by construction. Null restores the default.
  void set_georef_local_source(const PoseInterpolator* src);

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

  // --- camera keyframes in (A11's frames.idx; B8's write path, INT-34) ----
  //
  // Encodes `kf` as a `color::encode_keyframe_record()` payload and writes it
  // to the session recorder as a `kCameraFrameIndex` chunk on
  // `StreamId::kCameraFrames`, i.e. into `streams/frames/frames.idx`.
  //
  // WHY HERE AND NOT ON THE RECORDER. docs/A11-color.md §3.1 is explicit that
  // the capture side needs no new writer — it is two lines against the
  // `FileRecordWriter` the app already holds. What it cannot do for itself is
  // the LOCK: `FileRecordWriter` is not internally synchronized (record/ owns
  // no thread), and since the Mid-360 raw shim landed, the D6 serial thread,
  // the Mid-360 receive thread and the NTRIP thread can all be recording
  // concurrently. `Engine::recorder()` hands out the writer with no lock;
  // this method takes the same `record_m` every other producer takes. B8's
  // CameraX callback is a fourth thread, so it needs it.
  //
  // The record is VALIDATED before it is written (unit quaternion, principal
  // point inside the image, a relative image name with no ".." — the same
  // zip-slip class `zip_import()` defends against): an invalid record that
  // reaches the disk can only be skipped silently at read time.
  // kInvalidState when no session is recording, kInvalidArgument when the
  // keyframe is unusable.
  Status record_keyframe(const Keyframe& kf);

  // --- background processing (A15's queue, wired INT-34) ------------------
  //
  // ONE place an app drives every long job through: post-process, colorize,
  // export, extract-for-transfer, cloud submit (Tech Spec §3.8's three
  // processing modes). Before this, `JobQueue` was constructible but nothing
  // owned one, so the Android foreground service and the Qt processing panel
  // would each have invented their own — and each would have had to wire its
  // own EventBus for kJobProgress.
  //
  // OWNED LAZILY. The queue starts a worker thread in its constructor
  // (DESIGN.md §2, "(A15) job workers"), and the overwhelming majority of
  // Engine instances — every unit test, every live capture that never
  // processes anything — never submit a job. So the first call to jobs()
  // creates it and every later call returns the same one; an Engine that is
  // never asked owns no thread. The queue is constructed with THIS engine's
  // EventBus, which is what makes EventType::kJobProgress arrive on the same
  // subscription as every other event.
  //
  // LIFETIME. Destroyed (and its worker joined) at the very start of
  // ~Engine, before the recorder, the page store or the bus — a running job
  // may be publishing progress and appending points to both.
  //
  // Thread-safe: the lazy construction is serialized by the engine mutex, and
  // JobQueue's own methods are safe from any thread.
  jobs::JobQueue& jobs();

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
  void on_gnss_fix_(const GnssFix& fix);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_CORE_ENGINE_H
