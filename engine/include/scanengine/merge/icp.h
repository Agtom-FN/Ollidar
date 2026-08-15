// icp.h — pairwise refinement (Tech Spec §3.10: "voxel-downsampled
// point-to-plane ICP per pair"), plus the overlap measure that decides
// whether a pair may be merged at all.
//
// ### It is A7's ICP kernel, with the outer loop unrolled
//
// `post::icp_point_to_plane()` (slam/post/loop_closure.h) is already the
// engine's point-to-plane ICP: FAST-LIO2 plane fits by covariance
// eigen-decomposition, a Huber kernel on the point-to-plane residual, the
// A6-derived Jacobian, a deterministic 6x6 LDL^T, and a Levenberg floor for
// the rank-deficient single-wall case. A7 measured it recovering a known
// transform to 0.00043 deg and 0.048 mm. Writing a second one here would
// mean a second set of those bugs.
//
// What it does not do is HAND BACK THE ITERATION HISTORY, and §3.10's residual
// report needs it: a merge workbench has to show an operator that a pair
// settled rather than wandered. So this module calls it with
// `max_iterations = 1` in its own outer loop and records the residual at every
// step. Two consequences, both deliberate:
//
//   * The target index is rebuilt once per iteration instead of once per ICP.
//     After the voxel downsample a target is tens of thousands of points and
//     the rebuild is a few milliseconds; a merge runs a handful of pairs, not
//     thousands of loop candidates. Measured cost is in docs/A13-merge.md §6.
//   * The outer loop owns convergence AND the rollback: a step whose residual
//     rose is undone and the loop stops. That makes `trace` MONOTONE
//     NON-INCREASING BY CONSTRUCTION, which is a property a UI can plot and a
//     test can assert, rather than a property that usually holds.
//
// ### Overlap is a gate, not a statistic
//
// Two sessions of two different wings of a building have no common geometry.
// Point-to-plane ICP does not know that: every source point still finds some
// nearest plane, the solve is still well-posed, and it will return a
// confident transform for a merge that must not happen. So overlap is
// measured independently of the fit — occupancy of one cloud's voxels in the
// other's — and it is checked BEFORE the residual is believed. A low-overlap
// pair is reported as low-overlap, which is a different answer from "aligned
// badly".
//
// Owner: A13.
#ifndef SCANENGINE_MERGE_ICP_H
#define SCANENGINE_MERGE_ICP_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/merge/session.h"
#include "scanengine/slam/post/loop_closure.h"
#include "scanengine/slam/post/progress.h"

namespace scanengine {
namespace merge {

// --- overlap ----------------------------------------------------------------

struct OverlapEstimate {
  // Fraction of sampled points of one cloud landing within `voxel_m` of an
  // occupied voxel of the other, after `b_from_a` is applied to a.
  double a_in_b = 0.0;
  double b_in_a = 0.0;
  // min(a_in_b, b_in_a). The conservative one, and the one the gate uses: a
  // small session fully inside a big one scores 1.0 one way and 0.1 the
  // other, and only the second number tells you the big session is mostly
  // unconstrained by this pair.
  double symmetric = 0.0;
  std::uint64_t samples_a = 0, samples_b = 0;
  double voxel_m = 0.0;
};

// Tolerance is one voxel: a point is "in" the other cloud when any of the
// 2x2x2 voxels around its position is occupied. Sampling is by STRIDE, never
// by truncation — the first N points of a lidar cloud are one side of the
// room (the same argument A7 makes for its keyframe cap).
OverlapEstimate estimate_overlap(Span<const PointVertex> a, Span<const PointVertex> b,
                                 const double b_from_a[16], double voxel_m = 0.30,
                                 std::uint32_t max_samples = 20000);

// --- refinement -------------------------------------------------------------

struct MergeIcpConfig {
  // §3.10's "voxel-downsampled". 0 disables the downsample for that side.
  // 0.15 m keeps enough surface detail for a 5-neighbour plane fit while
  // making a 20 M-point session a ~10^5-point ICP problem.
  double source_voxel_m = 0.15;
  double target_voxel_m = 0.15;
  // Stride-decimate the source after the downsample. 0 = no cap.
  std::uint32_t max_source_points = 80000;

  // Plane fit and robust kernel — A7's, unchanged. The CORRESPONDENCE GATE is
  // the one parameter A13 sets differently from A7, and it is a measured
  // decision, not a preference (docs/A13-merge.md §4): with A7's 1.0 m gate a
  // *correct* alignment of the test building reports 52 mm RMS, and with
  // 0.5 m the same alignment reports 9.6 mm. The difference is entirely
  // correspondences past a surface EDGE — a point in the corridor matched to
  // the extended plane of the partition it has just passed. A7 keeps the wide
  // gate because its RMS is an accept/reject gate and its initial guess can
  // be metres out; A13's RMS is a number shown to an operator next to a
  // "merge" button, and its initial guess is decimetres.
  post::IcpConfig icp{};
  // ...and because "decimetres" is not true of the yaw-search fallback, the
  // first `coarse_iterations` run with the gate multiplied by
  // `coarse_gate_scale`. Coarse-to-fine: pull it in from a metre out, then
  // measure the residual on correspondences that mean something. The gate in
  // force is recorded per iteration in the trace, because the step down at
  // the stage boundary is visible in the residual and must not look like a
  // bug.
  std::uint32_t coarse_iterations = 6;
  double coarse_gate_scale = 3.0;

  std::uint32_t max_iterations = 30;
  double converge_rot_rad = 1e-5;
  double converge_trans_m = 1e-4;
  // A step is rolled back and the loop stops when the residual rises by more
  // than this factor. 1.0 = any rise at all, which is what makes `trace`
  // monotone. Raising it lets ICP climb out of a plateau when the
  // correspondence set is still growing, at the cost of that guarantee.
  double max_rms_increase_ratio = 1.0;

  // Overlap measurement and its gate.
  double overlap_voxel_m = 0.30;
  std::uint32_t overlap_samples = 20000;
  // Below this, the pair is NOT refined and NOT merged. 0.15 is deliberately
  // low: a corridor joining two wings is a legitimate 20% overlap. What it
  // stops is 0.
  double min_overlap = 0.15;

  MergeIcpConfig();  // sets the icp sub-config's merge-specific defaults
};

struct IcpIteration {
  std::uint32_t index = 0;
  // Residual at the estimate ENTERING this iteration, over that iteration's
  // correspondences. The last entry is measured at the returned estimate, so
  // `trace.front()` and `trace.back()` are the before/after pair a report
  // quotes.
  double rms_m = 0.0;
  double fitness_m = 0.0;
  std::uint64_t inliers = 0;
  double inlier_ratio = 0.0;
  // Size of the step taken FROM this estimate. Zero on the last entry.
  double step_rot_deg = 0.0;
  double step_trans_m = 0.0;
  // The correspondence gate in force for this iteration (see
  // MergeIcpConfig::coarse_iterations).
  double gate_m = 0.0;
};

struct PairIcpResult {
  bool refined = false;  // ICP ran (i.e. the overlap gate passed)
  // Reached a stationary point: either the step fell below both thresholds,
  // or a step stopped reducing the residual and was rolled back after at
  // least one that did. Both leave the best iterate as the answer, and both
  // are "ICP is done" — as opposed to `false`, which means it ran out of
  // iterations, ran out of correspondences, or was cancelled.
  bool converged = false;
  bool low_overlap = false;

  double b_from_a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  double init_b_from_a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  double rms_before_m = 0.0;
  double rms_after_m = 0.0;
  std::uint64_t inliers = 0;
  double inlier_ratio = 0.0;
  OverlapEstimate overlap{};

  std::uint32_t iterations = 0;
  std::uint32_t rejected_steps = 0;  // steps undone because the residual rose
  std::vector<IcpIteration> trace;   // monotone non-increasing in rms_m

  std::uint64_t source_points = 0, target_points = 0;  // after downsampling
  double ms = 0.0;
  const char* blocker = "";
};

// Refine `b_from_a` (source points in a's frame, target points in b's frame).
// Neither cloud is transformed or copied: the initial guess is handed to the
// ICP, which is where it belongs.
PairIcpResult refine_pair(Span<const PointVertex> source, Span<const PointVertex> target,
                          const double init_b_from_a[16], const MergeIcpConfig& cfg = {},
                          post::CancelToken* cancel = nullptr);

// Same, from the paged/chunked session clouds. The downsample runs straight
// off the chunks (A7's VoxelAccumulator), so a multi-page session is never
// flattened into one big allocation first.
PairIcpResult refine_pair(const SessionCloud& source, const SessionCloud& target,
                          const double init_b_from_a[16], const MergeIcpConfig& cfg = {},
                          post::CancelToken* cancel = nullptr);

// The voxel downsample every stage of this module runs first, exposed because
// a caller that refines the same pair twice should not pay for it twice.
// Deterministic, insertion-ordered (A7's `VoxelAccumulator`).
void downsample(const SessionCloud& in, double voxel_m, std::uint32_t max_points,
                std::vector<PointVertex>* out);

}  // namespace merge
}  // namespace scanengine

#endif  // SCANENGINE_MERGE_ICP_H
