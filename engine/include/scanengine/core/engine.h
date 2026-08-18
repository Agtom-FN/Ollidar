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
#include "scanengine/poses/imu_densified_pose.h"
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
// 5 (INT-FINAL): the Android capture seam and the CRS escape hatch —
//             scan_device_config's Mid-360 half grew the backend selector, the
//             two pre-bound descriptors, every port, the receive buffer, the
//             point filter, the decimation budget and the SDK config path
//             (android/NOTES.md §8 findings 1, 2 and 4), and
//             scan_engine_set_crs() landed (docs/INT29-wiring.md §7 item 5).
// 6 (A16): device auto-discovery (scan_discover_mid360, scan_host_check,
//             scan_probe_d6, scan_probe_um982, scan_enumerate_serial) and the
//             single-instance guard (scan_instance_acquire/release). All new
//             symbols and new structs — no ABI-5 layout changed, so an ABI-5
//             consumer relinks unmodified. docs/A16-discovery.md.
// 7 (live page eviction): scan_engine_set_live_page_eviction(),
//             scan_engine_page_stats() + scan_page_stats,
//             scan_engine_recycle_live_pages(), and SCAN_PAGE_UPDATE_EVICTED.
//             The fix for the 2026-08-17 field bug "live view not moving": a
//             full page store used to drop every subsequent point forever.
//             All new symbols / new struct / a new value of an existing enum
//             field — no ABI-6 layout changed, so an ABI-6 consumer relinks
//             unmodified and keeps the old hard-cap behaviour (eviction is
//             opt-in). page_store.h.
// 8 (ROUND 9 item 35): the phone IMU becomes an input to the geometry.
//             scan_engine_push_imu(), scan_engine_set_imu_extrinsics(),
//             scan_engine_imu_densify_stats() + scan_imu_densify_stats, and
//             SCAN_STREAM_IMU_PHONE. All new symbols / a new struct / a new
//             value of an existing mirrored enum — no ABI-7 layout changed, so
//             an ABI-7 consumer relinks unmodified and, pushing no IMU, gets
//             byte-for-byte the ABI-7 trajectory. poses/imu_densified_pose.h.
// 9 (ROUND 10 item 36): the lidar -> pose time offset.
//             scan_engine_set_pose_time_offset_ns() /
//             scan_engine_pose_time_offset_ns(). Two new symbols, nothing
//             else touched; zero is the ABI-8 behaviour exactly, so an ABI-8
//             consumer relinks unmodified and resolves the same cloud.
//             slam/pushbroom/pushbroom_assembler.h.
// ROUND 13: 9 -> 10, additive only. Two new C entry points
// (scan_lscan_reprocess_d6, scan_lscan_mount_check) and two new PODs;
// nothing existing changed size, order or meaning.
// ROUND 15: 10 -> 11, additive only. Three new C entry points for live
// re-anchor healing (scan_engine_heal_live_frame / clear_live_correction /
// live_heal_stats), one for the floor plan (scan_lscan_floor_plan) and one
// extended reprocess (scan_lscan_reprocess_d6_ex) that also returns the
// ROUND 12 self-consistency ruler. NOTHING existing changed size, order or
// meaning: scan_reprocess_result and scan_reprocess_options are untouched, so
// an ABI-10 consumer relinks unmodified and gets identical behaviour (the
// live correction starts at identity and stays there unless asked).
inline constexpr std::uint32_t kEngineAbiVersion = 11;
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

  // The Mid-360's OWN counters, which `DeviceHealth` cannot carry
  // (android/NOTES.md §8 finding 5, desktop/NOTES.md §8.3: "Engine exposes no
  // concrete-driver accessor", so Mid360Stats was unreachable from an app even
  // in C++). The generic health row answers "is data arriving"; this answers
  // the question a flaky bench session actually raises — how many watchdog
  // trips, clean resumes and FORCED SDK RE-INITS this capture has had, what
  // the link state is, and which device SN/IP is on the other end.
  //
  // kNotFound for an unknown id, kInvalidArgument for a device that is not a
  // Mid-360. Note the two loss figures mean different things: `loss_pct_window`
  // is this health window's, `loss_pct_total` the session's.
  Result<Mid360Stats> mid360_stats(DeviceId id) const;

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

  // --- ROUND 15 item 54: LIVE RE-ANCHOR HEALING ---------------------------
  //
  // ROUND 13 established what a section break is: ARCore recognising a place
  // and snapping its world frame, with the frame change written down in the
  // pose stream as the jump itself (T_k = pose_after * pose_before^-1). That
  // round applied it OFFLINE, after the seal. Everything needed to apply it
  // the instant it happens was already on the phone, and until now the live
  // map shattered anyway and the operator got a buzz telling them about it.
  //
  // THE DIRECTION MATTERS AND IT IS NOT THE OFFLINE ONE. Offline, sections
  // are brought into the LAST section's frame, because that is the frame
  // ARCore currently believes. Live, the map already on screen fills the
  // display and the operator's hands are steering by it, so the new frame is
  // mapped onto the OLD one instead: nothing that is already drawn moves, and
  // the points arriving after the snap land where the operator expects them.
  // Concretely the accumulated correction becomes C <- C * T^-1, and every
  // subsequent pose is left-multiplied by C on its way to the assembler.
  //
  // WHAT IS RECORDED DOES NOT CHANGE. `record_pose_()` writes the pose the
  // caller pushed, unmodified and before any of this is applied, so
  // `streams/poses_ar.bin` is byte-for-byte what an unhealed build would have
  // written and an offline re-resolve reproduces the capture exactly (Tech
  // Spec §3 key rule 2). The section bookkeeping is unaffected, so ROUND 13's
  // offline stitch — which has submaps, refinement and a flat-floor referee
  // that a live pass cannot afford — still runs and still improves on this.
  // The live correction is a VIEW transform with provenance, and the one
  // artifact it does change is `streams/map.bin`, which reprocess.h already
  // documents as a cache the Process path overwrites.
  //
  // `before` and `after` are the two poses that straddle the jump — the
  // caller's own detector found them, and the caller is the only thing that
  // knows which pair those were. Returns kInvalidArgument, WITHOUT changing
  // the correction, when the pair cannot produce a rigid transform (a pose
  // the tracker disowned, a non-finite or degenerate quaternion, a
  // non-increasing timestamp). That refusal is what the operator cue is for:
  // it fires only for a break that could NOT be healed.
  Status heal_live_frame(const Pose& before, const Pose& after);
  void clear_live_correction();

  struct LiveHealStats {
    std::uint32_t applied = 0;  // breaks folded into the correction
    std::uint32_t refused = 0;  // breaks with no usable pose bracket
    // What the accumulated correction moves a point by, i.e. how far apart
    // the live map WOULD have been without it.
    double translation_m = 0.0;
    double rotation_deg = 0.0;
    bool active = false;
    double matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  };
  LiveHealStats live_heal_stats() const;

  // --- ROUND 9 item 35: the phone's IMU ------------------------------------
  //
  // > "lidar data and the imu position data need sync the frequency"
  //
  // ARCore delivers ~30 poses/s; the D6 delivers 4000 returns/s, each with its
  // own instant. So one ARCore bracket spans ~133 returns whose orientation is
  // whatever a slerp between two endpoints says — and the 5-15 Hz band a
  // handheld rig actually lives in (tremor, heel strike) is not in that slerp.
  // The phone's gyro runs at 200-400 Hz and sees all of it.
  //
  // Pushing samples here does TWO things, and both matter:
  //   * they feed the `ImuDensifiedPoseSource` the pushbroom assembles
  //     against, so every return between two ARCore poses is placed on the
  //     gyro-integrated path rather than on the chord;
  //   * they are RECORDED as ChunkType::kPhoneImu chunks, so the offline
  //     re-resolve rebuilds the same geometry rather than a coarser one
  //     (record-always, Tech Spec §3 key rule 2).
  //
  // `t_mono_ns` is engine time. On Android that is `SensorEvent.timestamp`
  // verbatim — it is already CLOCK_BOOTTIME, the same domain as ARCore's pose
  // stamps, so nothing maps it (see TimeSync::stream_has_device_clock).
  //
  // Safe from any thread; the densifier's ring is internally synchronized, so
  // the SensorEventListener thread pushes here while the serial reader
  // resolves points. A non-finite or out-of-order sample is rejected with
  // kInvalidArgument rather than corrupting the ring.
  //
  // NEVER REQUIRED. With no IMU pushed, the densifier falls through to the
  // wrapped ExternalPoseSource's plain interpolation on every query, which is
  // exactly the pre-ROUND-9 answer (asserted in
  // tests/test_round9_phone_imu_record.cpp).
  Status push_phone_imu(std::int64_t t_mono_ns, const float gyro_rad_s[3],
                        const float accel_m_s2[3]);

  // Rotation taking a vector in the IMU's frame to the frame ARCore reports its
  // pose in, as a unit quaternion (x, y, z, w). NOT identity on a real phone:
  // Android's sensor frame is defined against the display in its natural
  // orientation and ARCore's against the camera image, and the camera is
  // usually mounted 90 degrees to the display
  // (CameraCharacteristics.SENSOR_ORIENTATION). Only the app knows it, so only
  // the app can supply it.
  //
  // Getting it wrong cannot corrupt the endpoints — those stay pinned to
  // ARCore — but it distorts the path between them, which is the whole value
  // being added. kInvalidArgument for a non-finite or zero-norm quaternion.
  //
  // COSTS THE BUFFERED SAMPLES. `ImuDensifyConfig` is fixed at construction
  // (poses/imu_densified_pose.h), so applying a new extrinsic rebuilds the
  // densifier, which drops the IMU ring and the estimated gyro bias. That is
  // right rather than merely convenient — a bias learned under one frame
  // convention is not a bias under another — but it means the first bracket or
  // two after the call fall back to plain interpolation while the ring refills.
  // Set it once, before start_session(), like the mount extrinsic.
  Status set_imu_extrinsics(const double quat_xyzw[4]);

  // Was the gyro path actually used, or did it fall back — and why? The
  // `fallback_*` counters are the diagnosis a field session needs: `no_imu`
  // means nothing is pushing, `gap` means the sensor is stuttering, `bracket`
  // means ARCore dropped poses, `closing` means the gyro and ARCore disagree
  // by more than any real rig should (usually a wrong camera_from_imu), and
  // ROUND 14's `no_pose`/`gate` mean the trajectory itself had nothing usable
  // there — not an IMU problem at all, and on a real capture the majority of
  // the total. They SUM to `fallbacks`; if they ever stop summing, a path was
  // added without a counter (tests/test_round14_session_reset.cpp asserts it).
  ImuDensifyStats imu_densify_stats() const;

  // The DENSIFIED interpolation, i.e. what the pushbroom actually resolves
  // against — pose_at() above deliberately stays the raw ARCore answer, because
  // that is what an app's overlay and the georef fusion mean by "the pose".
  // Identical to pose_at() whenever no IMU has been pushed.
  PoseSample densified_pose_at(std::int64_t t_mono_ns) const;

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

  // --- ROUND 10 item 36 ---------------------------------------------------
  //
  // The constant lidar-clock -> pose-clock offset, in nanoseconds, applied
  // when the assembler looks a pose up (PushbroomConfig::pose_time_offset_ns
  // carries the full derivation). Settable mid-session, because the app
  // exposes it as a live calibration slider and a reconnect to change a
  // number would lose the capture.
  //
  // Takes effect for every point resolved after the call, INCLUDING points
  // already pending — which is correct: they have not been placed yet, so
  // they should be placed with the operator's newest answer.
  Status set_pose_time_offset_ns(std::int64_t offset_ns);
  std::int64_t pose_time_offset_ns() const;

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

  // --- the survey profile's CRS escape hatch (INT-29 gap 5, INT-FINAL) -----
  //
  // §3.4's "EPSG picker (survey profile)". `crs::wkt1_for_epsg()` knows WGS 84
  // and the two UTM ranges and NOTHING else — deliberately, because the engine
  // ships no PROJ and no proj.db (gnss/crs.h's header explains the trade). A
  // national grid (HK1980 EPSG:2326, OSGB36 EPSG:27700, RD New EPSG:28992, …)
  // is therefore only expressible if the CALLER supplies the WKT its own
  // geodetic authority publishes. This is where it goes in.
  //
  //   set_crs("EPSG:2326", wkt)   label exports with the caller's CRS
  //   set_crs("EPSG:32650", "")   an EPSG the engine can render itself
  //   set_crs("", "")             clear the override; back to auto-UTM
  //
  // VALIDATED, not stored blind: the EPSG string must parse (`EPSG:<n>` or a
  // bare positive integer), and the WKT must be a plausible OGC WKT — a known
  // top-level keyword (PROJCS/GEOGCS/GEOCCS/COMPD_CS/VERT_CS/LOCAL_CS or their
  // WKT2 spellings), a quoted name, and balanced brackets outside quotes.
  // kInvalidArgument otherwise, with the reason in last_error_message(). An
  // EPSG the engine cannot render AND no WKT is also refused: that combination
  // silently produces an unlabelled export, which is the failure this method
  // exists to prevent. (`wkt` alone, with no `epsg`, is fine — some grids are
  // published as WKT with no EPSG code at all.)
  //
  // WHAT IT DOES NOT CHANGE: the convergence gate above. An override decides
  // WHAT label a georeferenced cloud gets, never WHETHER an ungeoreferenced
  // one may be labelled — a cloud still in the local frame must not be tagged
  // with a national grid any more than with a UTM zone. crs_wkt()/crs_epsg()
  // stay empty until the transform converges either way.
  //
  // Thread-safe; safe to call before or during a session.
  Status set_crs(const std::string& epsg, const std::string& wkt);
  // What was set, verbatim (both empty when nothing was). Unlike crs_wkt(),
  // these are NOT gated on convergence: they report configuration, not a
  // label a file may carry.
  std::string configured_crs_epsg() const;
  std::string configured_crs_wkt() const;

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

  // --- the live point window (2026-08-17 field bug) -----------------------
  //
  // OFF by default, because this Engine's PageStore is also what a
  // post-processing job, a merge preview and an export read: those must keep
  // the hard cap and SAY they overran, never silently throw the oldest half of
  // a cloud away (page_store.h "Backpressure").
  //
  // ON is what a LIVE CAPTURE wants, and only a live capture: the store then
  // recycles its oldest page instead of dead-ending, so the preview keeps
  // advancing for as long as the operator scans, with bounded memory, while
  // A5's recorder — a completely separate path — keeps every point on disk.
  //
  // Enabling it also makes start_session() reset the live window
  // (PageStore::recycle_all()) instead of stacking the next session's points
  // on top of the last one's: live SLAM restarts at the origin on every
  // session, so points from the previous session are in a STALE FRAME. An app
  // that has NOT opted in keeps the pre-existing behaviour exactly, including
  // the accumulation.
  Status set_live_page_eviction(bool enabled);
  bool live_page_eviction() const;

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
  // ROUND 8: writes an accepted pose into the active recording as a
  // ChunkType::kPoseAr chunk — record-always, finally applied to the
  // trajectory. See the long note above the definition in engine.cpp.
  void record_pose_(const Pose& pose);
  // ROUND 15: `pose` in the live-healed world frame. Identity until a break
  // is healed, so this is the ABI-10 value exactly on a clean capture.
  Pose live_pose_(const Pose& pose) const;
  // ROUND 9: the same thing for a phone IMU sample, as a kPhoneImu chunk.
  void record_phone_imu_(const PhoneImuSample& s);
  void on_gnss_fix_(const GnssFix& fix);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_CORE_ENGINE_H
