// d6_resolve.h — "Process" for a COIN-D6 session (ROUND 8, owner item 27b).
//
// --- WHY THIS EXISTS -------------------------------------------------------
//
// A7's PostSlamPipeline is the Mid-360 answer and is Mid-360-only BY
// CONSTRUCTION: its decode loop counts kLidarMid360/kImu chunks and returns
// kNotFound when there are none (post_pipeline.cpp). A D6 `.lscan` has
// neither, so a D6 project had no offline path at all — android/NOTES.md
// ROUND 7 §6 audited exactly this and made the button refuse honestly rather
// than fail with the string "not found".
//
// The refusal named its own blocker: a D6 capture stored the raw UART bytes
// and not the trajectory, so there was nothing on disk from which the 3D
// result could be rebuilt. ROUND 8 fixed that half (Engine::record_pose_()
// writes ChunkType::kPoseAr chunks; see the note over that function). This
// file is the other half — the thing that reads them back.
//
// --- WHY IT IS NOT "A7 WITH A D6 BRANCH" -----------------------------------
//
// The two pipelines share a name and nothing else. A7 re-runs an odometry:
// it ESTIMATES the trajectory from the lidar and IMU, so its expensive parts
// (loop detection, pose-graph optimization, re-integration) exist to make
// that estimate better. A D6 does not estimate anything. Its trajectory is
// the phone's, it was measured by ARCore during the walk, and it is already
// on disk — the only work left is the arithmetic A8 already owns:
//
//     world_from_lidar(t) = world_from_phone(t) * phone_from_lidar
//
// per point, with the pose interpolated to that point's own timestamp. There
// is no graph to optimize and no second pass, which is why this runs in
// roughly the time it takes to read the file rather than in minutes.
//
// --- WHY IT DRIVES THE DRIVER INSTEAD OF AN Engine -------------------------
//
// Same reasoning A7 states for itself, and it lands the same way: an Engine
// brings devices, a session, a transport and an internal PageStore this job
// cannot publish into, and a job needs to publish into the store the caller
// gave it so a chained Colorize/Export can reach it. So this drives the
// PRODUCTION classes directly — `D6Driver` (so the ROUND 7 per-byte time
// slicing that made walls straight applies identically), `ExternalPoseSource`
// (so the same LERP/SLERP and the same staleness/tracking-loss gates apply)
// and `D6PushbroomAssembler` (so it is literally the same assembler). Nothing
// here re-implements geometry; if it did, the offline result could drift away
// from the live one and nobody would notice.
//
// That is also what makes "replay == capture" (Tech Spec §3 key rule 2) true
// for a D6 cloud rather than only for its bytes: the same chunks through the
// same decode, resolved by the same assembler, produce the same points.
//
// --- WHAT IT DOES WITH A PROJECT THAT HAS NO POSES -------------------------
//
// Every `.lscan` recorded before 0.5.0 is such a project. It fails with
// ScanError::kNotFound and a message that says so in words a person can act
// on, and `D6ResolveStats::poses_read == 0` lets a caller distinguish "this
// recording predates trajectory storage" from "this recording is broken".
// The Android Review screen turns exactly that into its inline explanation.
//
// Owner: ROUND 8. Lives in slam/post/ because it is the D6 sibling of A7's
// pipeline and shares its progress/cancel vocabulary (post/progress.h).
#ifndef SCANENGINE_SLAM_POST_D6_RESOLVE_H
#define SCANENGINE_SLAM_POST_D6_RESOLVE_H

#include <cstdint>
#include <memory>
#include <string>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/slam/post/progress.h"
#include "scanengine/slam/post/section_stitch.h"
#include "scanengine/poses/imu_densified_pose.h"
#include "scanengine/slam/post/loop_end.h"
#include "scanengine/slam/post/trajectory_loop.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"

namespace scanengine {
namespace post {

struct D6ResolveConfig {
  // Where the resolved points go. Required — unlike A7 there is no internal
  // store to fall back on, because a job always owns one and a test always
  // wants to look at one.
  PageStore* store = nullptr;

  // Stream the resolved cloud is published under. kSlamMap is what a live
  // capture uses (engine.cpp's pushbroom defaults) and therefore what the
  // renderer's StreamFilter already expects.
  StreamId out_stream = StreamId::kSlamMap;

  // ROW-MAJOR phone_from_lidar. Three sources, in this precedence:
  //   1. `mount_phone_from_lidar` here, when `have_mount` is set — the caller
  //      knows better than the file (the Android app holds the operator's
  //      persisted re-zero, which is fresher than a manifest written before
  //      the trim was taken);
  //   2. `"mountCalibration"` in the container's manifest.json, which
  //      ROUND 8's FileRecordWriter::set_mount_calibration() now writes;
  //   3. nothing — and then this FAILS rather than silently resolving through
  //      identity. A D6 mounted at ~130 degrees to the phone (the owner's rig)
  //      resolved through identity produces a confidently wrong room, and a
  //      confidently wrong room is worse than a refusal.
  bool have_mount = false;
  double mount_phone_from_lidar[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  // Passed through to the assembler. The default is the LIVE config with one
  // change (see d6_resolve.cpp): `drain_on_push` stays on because the reader
  // hands chunks back in time order, so poses arrive before the returns that
  // need them and nothing has to be buffered to the end.
  PushbroomConfig pushbroom{};

  // Pose ring capacity. Bigger than the live default (8192 = ~4.5 min at
  // 30 Hz) because an offline pass has no memory pressure worth speaking of
  // and a ring that wraps mid-resolve silently drops trajectory.
  std::size_t pose_capacity = 262144;

  // --- ROUND 9 item 35: the recorded phone IMU ----------------------------
  //
  // ON, because "replay == capture" (Tech Spec §3 key rule 2) is the point of
  // this whole file and since ROUND 9 the live pass resolves through
  // `ImuDensifiedPoseSource`. An offline resolve that ignored the recorded
  // gyro would produce a DIFFERENT — and measurably worse — cloud than the one
  // the operator watched, with nobody able to say which was right.
  //
  // A container with no kPhoneImu chunks (i.e. everything recorded before
  // ROUND 9, and any capture on a device that never pushed one) is unaffected
  // either way: the densifier with an empty ring falls through to the plain
  // interpolation on every query, so the result is bit-identical to what this
  // pipeline produced before ROUND 9.
  //
  // OFF is the A/B: the same container resolved both ways, which is how the
  // gyro's contribution to a REAL capture is measured rather than asserted.
  bool densify_with_phone_imu = true;

  // IMU->camera rotation, (x, y, z, w). Only the app knows it (it comes from
  // CameraCharacteristics.SENSOR_ORIENTATION and the rig), and the container
  // does not carry it yet — `manifest.json` has no key for it, which is a real
  // seam and is named as such in the ROUND 9 notes. Supply it when the caller
  // has it; the identity default is correct for a synthetic stream and for a
  // device where the two frames coincide, and getting it wrong distorts the
  // path between two ARCore poses without ever moving the poses themselves.
  bool have_imu_extrinsics = false;
  double imu_camera_from_imu[4] = {0.0, 0.0, 0.0, 1.0};

  // IMU ring capacity, in samples. The default (3200 = 8 s at 400 Hz) is the
  // live one and is already generous — the densifier only ever integrates
  // BACKWARD over one ARCore bracket (~33 ms), so it needs only the recent
  // tail, and the chronological read guarantees that tail has arrived.
  std::size_t imu_capacity = 3200;

  // --- ROUND 11 item 41: loop closure -------------------------------------
  //
  // OFF by default, and that default is a decision rather than caution. This
  // pipeline's contract is "replay == capture" (Tech Spec §3 key rule 2): the
  // same container through the same assembler produces the same points as the
  // live pass did. Loop closure deliberately produces DIFFERENT points — it
  // is a correction the live pass could not have made — so it may never be
  // something a re-resolve does behind the caller's back. The offline
  // "Process" action asks for it explicitly; the Review fast path does not.
  //
  // When it fires, the resolved cloud in `store` is rewritten IN PLACE
  // (PageStore::page_data_mutable) rather than re-derived, because
  // `p' = C(t) * p` is exact — see slam/post/trajectory_loop.h. When it does
  // not fire, not one point is touched and `stats.loop.decision` says why.
  bool close_loops = false;
  TrajectoryLoopConfig loop{};

  // --- ROUND 13: section stitching -----------------------------------------
  //
  // OFF by default for exactly the reason `close_loops` is: it moves points
  // the live pass could not have moved. It runs BEFORE loop closure and it
  // corrects the TRAJECTORY as well as the cloud, so a closure that follows
  // sees one coherent walk instead of N pieces — which is the only way the
  // closer's revisit detector can mean anything on a broken capture.
  //
  // See slam/post/section_stitch.h: a section break is ARCore re-anchoring,
  // and the frame change it applied is recorded in the pose jump itself.
  bool stitch_sections = false;
  SectionStitchConfig sections{};

  // --- ROUND 16 item 60: loop-end closure ----------------------------------
  //
  // OFF by default for exactly the reason the two above are: it moves points
  // the live pass could not have moved. It runs LAST — after stitching (which
  // makes the trajectory one walk, so "the end" means something) and after
  // ROUND 11's closer (which, when it does fire, has already removed the
  // drift this would otherwise measure a second time).
  //
  // See slam/post/loop_end.h: rotation frozen at the tracker's own, which the
  // recorded 400 Hz gyro agrees with to under a degree, and the translation
  // solved against the surfaces at the two ends.
  bool close_loop_end = false;
  LoopEndConfig loop_end{};

  // Optional outputs, for a caller that wants to look at what was used. Both
  // are appended to (never cleared) so a caller can accumulate across runs if
  // it really wants to; `out_point_times` is the pairing described over
  // PushbroomConfig::out_point_times and is filled whenever it is set OR
  // `close_loops` is on (the pipeline needs it either way).
  std::vector<TrajPose>* out_trajectory = nullptr;
  std::vector<std::int64_t>* out_point_times = nullptr;
};

struct D6ResolveStats {
  std::uint64_t lidar_chunks = 0;   // kD6Raw chunks fed to the driver
  std::uint64_t lidar_bytes = 0;
  std::uint64_t poses_read = 0;     // kPoseAr chunks decoded
  std::uint64_t poses_accepted = 0; // ... and accepted by the interpolator
  std::uint64_t points_out = 0;     // resolved world points published
  PushbroomStats pushbroom{};
  bool mount_from_manifest = false; // the extrinsic came from the container
  // ROUND 9: the phone-IMU extrinsic came from the container's manifest
  // ("imuCalibration") rather than from the caller.
  bool imu_extrinsics_from_manifest = false;

  // --- ROUND 9: the phone IMU ---------------------------------------------
  // `imu_read` is kPhoneImu chunks decoded, `imu_accepted` how many the
  // densifier took (it rejects non-finite and out-of-order samples).
  // `imu_densified` / `imu_fallbacks` are the interpolator's own verdict: how
  // many returns were placed on the gyro path and how many fell back to the
  // slerp. Both zero on a pre-ROUND-9 container, and `imu_densified == 0` with
  // `imu_read > 0` is the signal that something is wrong (a stuttering sensor,
  // or an extrinsic so far off that every bracket trips the closing guard).
  std::uint64_t imu_read = 0;
  std::uint64_t imu_accepted = 0;
  std::uint64_t imu_densified = 0;
  std::uint64_t imu_fallbacks = 0;

  // --- ROUND 11 item 41 ---------------------------------------------------
  // What loop closure decided, ALWAYS — including the two answers that are
  // not "closed": `kNoRevisit` (the correct verdict for a one-way walk) and
  // every rejection, each naming the gate that refused. `loop_applied` is the
  // one bit that says whether the points in the store were moved.
  LoopClosureReport loop{};
  bool loop_applied = false;

  // --- ROUND 13 -----------------------------------------------------------
  // What section stitching found and did. `sections.sections == 1` on a clean
  // capture and `sections_stitched` is false, which is the provable no-op.
  SectionStitchReport sections{};
  bool sections_stitched = false;

  // --- ROUND 16 item 60 ----------------------------------------------------
  // What loop-end closure decided, ALWAYS — including every refusal, each
  // naming the gate. `loop_end_applied` is the one bit that says whether the
  // points in the store were moved.
  LoopEndReport loop_end{};
  bool loop_end_applied = false;

  // ROUND 10 (additive): the densifier's OWN verdict, verbatim, including the
  // per-reason fallback breakdown and the closing-error statistics.
  //
  // `imu_fallbacks` above answers "how often did the gyro path not run"; this
  // answers "why", which is the only version of the question a person can act
  // on. On the owner's scan-020 the first version of this field showed 43 % of
  // returns falling back with no way to tell a stuttering sensor from a
  // too-wide bracket from a rig whose gyro and ARCore genuinely disagree —
  // three completely different problems with three different fixes.
  ImuDensifyStats imu{};
};

class D6ResolvePipeline {
 public:
  explicit D6ResolvePipeline(const D6ResolveConfig& cfg);
  ~D6ResolvePipeline();

  D6ResolvePipeline(const D6ResolvePipeline&) = delete;
  D6ResolvePipeline& operator=(const D6ResolvePipeline&) = delete;

  void set_progress_callback(PostProgressFn fn);
  void set_cancel_token(CancelToken* token);

  // BLOCKING, on the calling thread. Reads `lscan_dir` start to finish once.
  Status run(const std::string& lscan_dir);

  const D6ResolveStats& stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// True when `lscan_dir` is a D6 project, i.e. it holds kD6Raw chunks and no
// Mid-360 ones. Cheap: reads the stream headers and A5's validation summary,
// not the chunks. Used by jobs/local_runner.cpp to route kPostProcess without
// making the caller state which sensor it recorded — the container knows.
//
// `*is_d6` is set false (and kOk returned) for a Mid-360 or empty project;
// a directory that cannot be opened at all returns the reader's error.
Status lscan_is_d6_project(const std::string& lscan_dir, bool* is_d6);

// Reads `"mountCalibration": {"phoneFromLidar": [...]}` out of a container's
// manifest.json. Returns false when the key is absent or unparseable, which
// is the normal case for every recording made before ROUND 8.
//
// Exposed because the Android side wants the same answer for its own reasons
// (it shows "recorded before trajectory storage" only when the project really
// is that old) and because a two-line JSON field read is not worth two
// implementations.
bool read_manifest_mount(const std::string& lscan_dir, double phone_from_lidar[16]);

// --- the Review fast path ---------------------------------------------------
//
// Loads the RESOLVED cloud a capture cached into the container
// (ChunkType::kPointsXyzRgba in streams/map.bin — see the note in
// Engine::Impl::on_page_update) straight into `store`, with no decoding and no
// pose interpolation. On a phone this is the difference between a saved scan
// opening instantly and opening after a full re-resolve pass.
//
// It is a CACHE and this function treats it as one: a container without a map
// stream is not an error, it returns kOk with `*out_points == 0`, and the
// caller falls through to D6ResolvePipeline. Nothing on this path can produce
// a wrong answer that a re-resolve would not also produce, because the cache
// was written by that same assembler during the capture.
//
// `out_stream` is what the points are appended under, independent of what they
// were recorded as — Review draws whatever the renderer's StreamFilter expects.
Status load_recorded_cloud(const std::string& lscan_dir, PageStore* store, StreamId out_stream,
                           std::uint64_t* out_points);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_D6_RESOLVE_H
