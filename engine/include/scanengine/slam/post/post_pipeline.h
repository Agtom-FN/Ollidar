// post_pipeline.h — "Finish scan" for a Mid-360 session (task A7).
//
// Tech Spec §3.3, "Mid-360 post", in one line:
//
//   full-density LIO re-run -> Scan Context loop candidates -> pose-graph
//   optimization -> re-integration -> voxel dedup / outlier filter
//
// and §3.8 puts it behind a foreground service or a background task:
// cancellable, progress-reported.
//
// --- it runs from the RECORDING, not from a live session -------------------
//
// The input is a `.lscan` directory, read with A5's FileRecordReader. That is
// the whole point of record-always (Tech Spec §3 key rule 2): the live pass
// decimated to 40k pts/s and let the map forget outside `map_radius_m`, and
// neither of those losses matters, because the raw datagrams are still on
// disk. So the post run is not a "refinement" of the live result — it is a
// second, better run from the same bytes, and it is the same code path the
// cloud worker executes (§3.8), which is what makes local / cloud / transfer
// one pipeline in three places.
//
// A7 decodes Mid-360 chunks itself rather than going through
// `lscan::ReplaySource`. ReplaySource pushes bytes into a live `Engine` via
// push_serial_bytes(), and no such entry point exists for Mid-360 (A5 §4 says
// so explicitly and A3 has not added one). Rather than invent an Engine API
// from inside slam/, the pipeline reads chunks and drives
// `mid360::parse_packet` / `point_passes` and `LioOdometry` directly — the
// engine's own production decode path, the same one tests/test_lio.cpp uses
// against the real capture. See engine/docs/A7-post.md §8 for the seam that
// would let ReplaySource take over.
//
// --- what "full density" means and what it costs ---------------------------
//
//   cfg.lio.live_points_per_sec = 0   (no decimation — the live 40k budget
//                                      exists for a phone's thermal envelope,
//                                      not for accuracy)
//   cfg.lio.map_radius_m        = 0   (never forget)
//   cfg.lio.map.max_points_per_voxel raised, max_voxels raised
//
// docs/A6-lio.md §8 measured the cost of exactly this on the real capture:
// 54.8 ms per scan against 14.7 ms decimated, and a trajectory that changes by
// 0.6% across a 6x point-count range. So full density is not what buys the
// accuracy — the loop closure is. Full density buys the DENSITY of the final
// cloud, which is the deliverable.
//
// --- memory ----------------------------------------------------------------
//
// Two things are resident: the keyframe clouds and the voxel accumulator.
//   keyframes  ~= (session length / keyframe_translation_m) *
//                 max_points_per_keyframe * 16 B
//                 -> 0.5 m / 4,000 points = 64 KB per keyframe; a 1.8 km,
//                    30-minute walk is 3,600 keyframes = 230 MB.
//   final cloud = bounded by the VOLUME scanned at dedup_voxel_m, not by the
//                 session length (the same argument that makes A6's IVox safe).
// Everything else streams. If a session ever needs to exceed that, the fix is
// to spill keyframe clouds to `processed/` and page them back for the ICP —
// noted, not implemented (docs/A7-post.md §9).
//
// Owner: A7.
#ifndef SCANENGINE_SLAM_POST_POST_PIPELINE_H
#define SCANENGINE_SLAM_POST_POST_PIPELINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/poses/pose_source.h"
#include "scanengine/slam/lio.h"
#include "scanengine/slam/post/cloud_filter.h"
#include "scanengine/slam/post/loop_closure.h"
#include "scanengine/slam/post/pose_graph.h"
#include "scanengine/slam/post/progress.h"
#include "scanengine/slam/post/scan_context.h"
#include "scanengine/slam/slam.h"

namespace scanengine {
namespace post {

// One keyframe: a pose and the cloud that was swept up on the way to it,
// expressed in that keyframe's own body frame.
struct Keyframe {
  std::int64_t t_ns = 0;
  // Odometry estimate (world_from_body), as the LIO re-run produced it.
  double q[4] = {0.0, 0.0, 0.0, 1.0};
  double p[3] = {0.0, 0.0, 0.0};
  // After optimization. Equal to the above until optimize runs.
  double q_opt[4] = {0.0, 0.0, 0.0, 1.0};
  double p_opt[3] = {0.0, 0.0, 0.0};
  // Body-frame, voxel-downsampled. This is what Scan Context describes and
  // what ICP registers.
  std::vector<PointVertex> points;
  std::uint32_t node = 0;  // index in the pose graph
};

struct LoopClosure {
  std::uint32_t from = 0;  // the earlier keyframe (the "match")
  std::uint32_t to = 0;    // the later keyframe (the "query")
  // Measured T_from^-1 * T_to, i.e. what the between-factor carries.
  double q[4] = {0.0, 0.0, 0.0, 1.0};
  double p[3] = {0.0, 0.0, 0.0};
  double sc_distance = 1.0;
  double sc_yaw_rad = 0.0;
  IcpResult icp;
  bool accepted = false;
  const char* reject_reason = "";  // stable string from loop_is_acceptable()
};

struct PostConfig {
  // Defaults are set to FULL DENSITY by the constructor below; a caller that
  // wants the live budget can put it back.
  LioConfig lio;

  // --- keyframing (Tech Spec §3.3's "every ~0.5 m / 10 deg") -------------
  double keyframe_translation_m = 0.5;
  double keyframe_rotation_deg = 10.0;
  // A stationary scanner emits no keyframes and would buffer points forever.
  // This is the ceiling that makes the buffer bounded rather than "bounded in
  // practice".
  double keyframe_max_interval_s = 2.0;
  // Downsampling of the keyframe cloud. 0.3 m is coarse on purpose: Scan
  // Context bins at (40 m / 20 rings) = 2 m and ICP fits planes over 5
  // neighbours, so finer costs memory (see the header's memory note) and buys
  // neither of them anything.
  double keyframe_voxel_m = 0.3;
  std::uint32_t max_points_per_keyframe = 4000;
  // Hard cap on the pending-point buffer, in points. Reached only if the
  // odometry stops producing poses entirely.
  std::uint32_t keyframe_buffer_cap = 400000;

  // --- loop closure -----------------------------------------------------
  bool detect_loops = true;
  ScanContextConfig scan_context;
  IcpConfig icp;
  LoopAcceptConfig loop_accept;
  // Keyframes either side of the match whose clouds are merged into the ICP
  // target. One Mid-360 keyframe is a fraction of a second of a
  // non-repetitive scan pattern; +-5 keyframes is a local map.
  std::uint32_t loop_submap_half_span = 5;
  double loop_submap_voxel_m = 0.2;

  // --- pose graph -------------------------------------------------------
  PoseGraphOptions graph;
  // Odometry edge sigmas, per keyframe interval. The LIO's own covariance is
  // available (Eskf::P) but is an ESKF covariance over a different state and
  // is not calibrated as an inter-keyframe uncertainty; a constant per-edge
  // sigma is the honest placeholder, and it is what makes the loop edge's
  // relative weight the only tuning knob that matters.
  double odom_sigma_rot_deg = 0.5;
  double odom_sigma_trans_m = 0.02;
  // Loop edge sigmas. The translation sigma is max(this, the ICP RMS), so a
  // loop that fit badly pulls proportionally less.
  double loop_sigma_rot_deg = 1.0;
  double loop_sigma_trans_m = 0.05;
  // Huber threshold on loop edges, in sigmas. Odometry edges get none — a
  // robust kernel on the chain hides real odometry failures.
  double loop_huber_sigmas = 2.0;

  // --- final cloud ------------------------------------------------------
  VoxelDedupConfig dedup;
  OutlierFilterConfig outlier;

  // Where the final cloud goes. Null ⇒ the pipeline owns a PageStore,
  // reachable through out_store().
  //
  // STREAM CHOICE: kSlamMap. core/types.h now has it (kSlamMap = 8,
  // "registered world-frame map points"), which is exactly what this is, and
  // no dedicated "final cloud" stream id exists. If integration adds one,
  // this is a one-line change — the value is a config field precisely so it
  // does not become a code change. See docs/A7-post.md §7.
  PageStore* store = nullptr;
  StreamId out_stream = StreamId::kSlamMap;
  bool publish_to_store = true;
  // Keep the final cloud in the pipeline's own vector as well (final_cloud()).
  // Off saves a copy of the deliverable for a caller that only wants pages.
  bool keep_final_cloud = true;

  // Mid-360 wire filter. A3's defaults drop no-returns and spatial-noise
  // tags; a post run deliberately keeps intensity-noise-tagged returns, whose
  // geometry is fine (see mid360_packets.h).
  mid360::PointFilterConfig point_filter;

  // How often a progress callback fires during the two streaming passes.
  std::uint32_t progress_chunk_interval = 2048;

  PostConfig();
};

struct PostStats {
  // --- input ------------------------------------------------------------
  std::uint64_t chunks_read = 0;
  std::uint64_t point_chunks = 0;
  std::uint64_t imu_chunks = 0;
  std::uint64_t malformed_chunks = 0;
  std::uint64_t points_decoded = 0;   // survived the wire filter
  std::uint64_t nonmonotonic_packets = 0;
  mid360::FilterStats filter;
  std::uint32_t truncated_tail_chunks = 0;
  std::uint32_t crc_mismatch_chunks = 0;

  // --- odometry ---------------------------------------------------------
  LioStats lio;
  std::uint64_t odom_poses = 0;
  double trajectory_length_m = 0.0;
  std::uint64_t keyframes = 0;
  std::uint64_t keyframe_points = 0;
  std::uint64_t buffer_overflow_points = 0;

  // --- loops ------------------------------------------------------------
  std::uint64_t loop_candidates = 0;
  std::uint64_t loops_accepted = 0;
  std::uint64_t loops_rejected = 0;

  // --- graph ------------------------------------------------------------
  PoseGraphSummary graph;
  // How far optimization moved the keyframes. With no ground truth this is
  // the only honest statement about the correction's size.
  double keyframe_shift_rms_m = 0.0;
  double keyframe_shift_max_m = 0.0;

  // --- final cloud ------------------------------------------------------
  std::uint64_t reintegrated_points = 0;  // transformed through the trajectory
  std::uint64_t unposed_points = 0;       // outside the trajectory's time span
  std::size_t dedup_points = 0;
  std::size_t final_points = 0;
  OutlierFilterStats outlier;
  std::uint32_t pages_appended = 0;
  std::uint64_t store_append_failures = 0;

  // --- timing (wall clock, ms) ------------------------------------------
  double ms_odometry = 0.0;
  double ms_loops = 0.0;
  double ms_optimize = 0.0;
  double ms_reintegrate = 0.0;
  double ms_filter = 0.0;
  double ms_total = 0.0;
};

// The A7 implementation of slam/slam.h's `PostPipeline` seam.
//
// Threading: run() is blocking and single-threaded (LioConfig::internal_thread
// is forced off — a post run has no live producer to unblock, and an internal
// thread would only reintroduce the scheduling dependencies A6 spent §4
// removing). cancel(), progress() and stats() are safe from another thread
// while run() is executing; everything else is not.
class PostSlamPipeline final : public PostPipeline {
 public:
  explicit PostSlamPipeline(const PostConfig& cfg = PostConfig());
  ~PostSlamPipeline() override;
  PostSlamPipeline(const PostSlamPipeline&) = delete;
  PostSlamPipeline& operator=(const PostSlamPipeline&) = delete;

  // --- slam.h seam ------------------------------------------------------
  Status run(const std::string& lscan_dir) override;
  float progress() const override;
  // Sets the pipeline's own token. STICKY: it is not cleared by run(), so a
  // pipeline that has been cancelled stays cancelled and a second run() fails
  // immediately with kCancelled. That is the safe direction — a cancel racing
  // the end of a run must not be silently lost — and a caller that wants to
  // re-run should construct a new pipeline (or drive an external token via
  // set_cancel_token() and reset() it).
  void cancel() override;

  // --- A7 additions -----------------------------------------------------
  void set_progress_callback(PostProgressFn cb);
  // A15 owns the token; the pipeline only reads it. Null restores the
  // pipeline's own internal token (the one cancel() sets).
  void set_cancel_token(CancelToken* token);

  PostStage stage() const;
  const PostStats& stats() const;

  const std::vector<Keyframe>& keyframes() const;
  const std::vector<LoopClosure>& loops() const;
  const PoseGraph& graph() const;
  // The optimized trajectory at full pose rate (not just keyframes), as a
  // PoseSource — so A9's exporters and A11's colorization consume it exactly
  // the way they consume the live LIO.
  const LioPoseSource& trajectory() const;
  const std::vector<PointVertex>& final_cloud() const;
  PageStore& out_store();
  const PostConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_POST_PIPELINE_H
