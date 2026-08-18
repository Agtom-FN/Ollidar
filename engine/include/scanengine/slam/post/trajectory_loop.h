// trajectory_loop.h — ROUND 11 item 41. Loop closure for a MEASURED
// trajectory (the phone's), not for an estimated one.
//
// --- WHY THIS IS NOT post/pose_graph.h -------------------------------------
//
// A7 already owns a full SE(3) pose-graph optimizer and a Scan Context loop
// detector, and neither of them is the right tool here. They exist because a
// Mid-360's trajectory is ESTIMATED from the lidar: every keyframe pose is a
// free variable, the odometry edges between them are noisy, and a loop
// constraint is one more measurement to be balanced against all of them. That
// is a least-squares problem and it deserves a solver.
//
// A COIN-D6 rig has no such problem. Its trajectory was MEASURED by ARCore,
// and — this is the load-bearing part — ARCore's error is not the pose-graph
// error model. VIO drift is a slow, smooth, low-frequency walk of the world
// frame: over one second the relative motion is excellent (that is what makes
// ROUND 9's gyro densification work), and over 200 seconds the accumulated
// yaw/position error is large. So the correct correction is not "re-balance
// 6,000 free poses"; it is "the world frame has rotated and slid by THIS much
// over the walk, spread that back along the path". One measurement, one
// smooth correction. A pose graph fed one loop edge and 6,000 stiff odometry
// edges computes exactly that answer, at a thousand times the cost, with a
// sparse-solve ordering to keep bit-identical.
//
// --- HOW THE CORRECTION IS APPLIED, AND WHY POINTS NEED NO RE-RESOLVE ------
//
// A D6 world point is, exactly,
//
//     p_world(t) = T_world_phone(t) * T_phone_lidar * p_lidar
//
// so LEFT-multiplying the pose by a world-frame correction C(t) gives
//
//     p_world'(t) = C(t) * T_world_phone(t) * T_phone_lidar * p_lidar
//                 = C(t) * p_world(t)
//
// — the corrected cloud is the old cloud pushed through the same C(t), with
// no decode, no re-interpolation and no second pass over the container. That
// identity is why this file takes a cloud plus per-point timestamps rather
// than re-running D6ResolvePipeline: the two are equal by construction, and
// only one of them can disagree with the live pass.
//
// C(t) is the SE(3) geodesic from identity to the measured closing transform
// T_fix, parameterised by ARC LENGTH along the trajectory rather than by
// time. Arc length, because VIO drift accumulates with distance travelled and
// not with seconds elapsed: an operator who stands still for 30 s in the
// middle of a walk accumulates almost nothing, and a time parameterisation
// would hand that pause a third of the correction.
//
//     C(s) = Exp(s * Log(T_fix)),   s = pathlen(a..t) / pathlen(a..b)
//
// clamped to 0 before the first visit and to 1 after the second, so the tail
// of the walk is carried rigidly and never bent. Exp(0) is exactly identity
// and Exp(1*Log(T)) is exactly T, so both ends are exact rather than nearly
// exact.
//
// --- THE GUARD ------------------------------------------------------------
//
// A FALSE loop closure is the worst thing in this file. Odometry drift is
// smooth and bounded and a person can look past it; a spurious closure drags
// a straight corridor into a circle and every downstream product inherits the
// fold. A7's loop_closure.h says the same thing about Scan Context candidates
// and its answer is the right one, so this file reuses A7's ICP and A7's
// acceptance gate verbatim rather than inventing a second opinion.
//
// Three independent things must all agree before anything moves:
//
//   1. SPATIAL REVISIT. Two poses far apart in time and in path length whose
//      positions are close. A walk that never comes back cannot produce a
//      candidate at all, which is the structural reason a straight walk is
//      safe — not a threshold, an absence.
//   2. A REAL EXCURSION. The path between them must have LEFT the
//      neighbourhood (`min_excursion_m`). Without this, standing still is a
//      revisit: every pose is within centimetres of every other one.
//   3. GEOMETRIC AGREEMENT. Point-to-plane ICP of the later submap against
//      the earlier one must converge, keep `min_inlier_ratio` of its points,
//      and land inside `max_rms_m` — A7's `loop_is_acceptable`, unmodified.
//      Then, separately, the correction it found must be SMALL
//      (`max_close_translation_m` / `max_close_rotation_deg`): a large
//      correction does not mean a big drift to fix, it means the two places
//      were not the same place.
//
// Every decision — including every rejection and which gate did the rejecting
// — comes back in LoopClosureReport as a stable string, because "it did not
// close" and "it closed wrongly" have to be distinguishable from a field log.
//
// --- ONE LOOP, DELIBERATELY -----------------------------------------------
//
// This closes the single best revisit and stops. Two or more simultaneous
// constraints genuinely do need a graph, and pretending otherwise by
// composing corrections would produce a result nobody could reason about.
// The single best loop removes the accumulated drift of the whole walk, which
// is the number the owner can see.
//
// Owner: ROUND 11 item 41.
#ifndef SCANENGINE_SLAM_POST_TRAJECTORY_LOOP_H
#define SCANENGINE_SLAM_POST_TRAJECTORY_LOOP_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/span.h"
#include "scanengine/slam/post/loop_closure.h"

namespace scanengine {
namespace post {

// One recorded pose, in the container's own units. Deliberately not `Pose`:
// this module needs only the rigid part, and taking the smaller struct keeps
// it usable from a test that has no pose source.
struct TrajPose {
  std::int64_t t_ns = 0;
  double q[4] = {0.0, 0.0, 0.0, 1.0};  // (x, y, z, w), world_from_phone
  double p[3] = {0.0, 0.0, 0.0};       // world_from_phone translation
};

struct TrajectoryLoopConfig {
  // --- gate 1: the spatial revisit ----------------------------------------
  // How close two visits must be to be a candidate. 3 m is generous on
  // purpose: this is a SHORTLIST bound, and the drift it exists to find is
  // itself part of the gap being measured. The sharp gates are geometric.
  double max_revisit_m = 3.0;
  // ... and how far apart in time and in walked distance, so that a return
  // two seconds later is not a "loop".
  double min_loop_seconds = 15.0;
  double min_loop_path_m = 8.0;

  // --- gate 2: the excursion ----------------------------------------------
  // The path between the two visits must reach at least this far from the
  // first one. Without it, a stationary rig is a perfect loop closure
  // candidate at every pair of poses it ever recorded.
  double min_excursion_m = 4.0;

  // --- the submaps --------------------------------------------------------
  // Points within +/- this of each visit's timestamp form the two clouds ICP
  // compares. Wide enough to hold real structure (a D6 sweeps 10 times a
  // second, so 3 s is 30 revolutions from a moving rig), narrow enough that
  // the trajectory inside the window has not itself drifted.
  // 6 s and not 3: on a D6 the submap's SHAPE is what decides whether the
  // closure is observable at all (see min_normal_coverage below), and a
  // longer window is the only thing that puts surface normals of more than
  // one orientation into it.
  double submap_half_window_s = 6.0;
  std::size_t min_submap_points = 600;
  // Deterministic stride subsample ceiling per submap. A stride and not a
  // random sample: identical input, identical points, on every platform.
  std::size_t max_submap_points = 40000;

  // --- gate 3a: is the closure OBSERVABLE at all? --------------------------
  //
  // THE GATE THIS ROUND EXISTS BECAUSE OF, and it is a property of the sensor
  // rather than of the software.
  //
  // A COIN-D6 sweeps a PLANE, and on this rig that plane is perpendicular to
  // the walk. So over a short window the returns land on the surfaces the
  // plane cuts — the walls ahead and behind, the floor and the ceiling — and
  // on NO surface whose normal points along the walk. Point-to-plane ICP
  // measures distance along normals, so translation along the walk costs it
  // exactly zero residual: the problem has a null space, and an ICP with a
  // null space does not fail, it WANDERS. Measured on this round's own
  // fixture: a 4 deg / 0.30 m injected drift came back as 3.77 deg (right)
  // and 2.74 m of translation (a metre and a half of pure fiction, all of it
  // along the walk).
  //
  // So the target submap's surface normals are collected and their scatter
  // matrix `sum(n n^T)` eigen-decomposed. `min_normal_coverage` is the floor
  // on the smallest eigenvalue as a fraction of the largest: below it, one
  // direction in space simply is not measured by this pair of submaps and no
  // amount of iteration will find it.
  //
  // A wider `submap_half_window_s` is the cure where a cure exists — walking
  // a curve turns the fan and fills the gap in — and where it does not (a
  // dead-straight out-and-back, which is what the owner's scan-020 is) the
  // honest answer is a refusal that names the reason.
  double min_normal_coverage = 0.05;
  // Neighbourhood for the local plane fits that produce those normals.
  double normal_radius_m = 0.25;
  std::size_t normal_samples = 4000;

  // --- gate 3: geometric agreement ----------------------------------------
  IcpConfig icp{};
  LoopAcceptConfig accept{};

  // --- gate 4: the correction has to be plausible -------------------------
  //
  // How much drift this is willing to believe, and these numbers are an
  // EMPIRICAL finding of ROUND 11 rather than a guess. Run against the owner's
  // scan-020 with a generous 1.5 m / 20 deg bound, ICP happily produced a
  // 0.97 m / 17.0 deg "closure" that passed every geometric gate — 77.8 %
  // inliers, 13 cm RMS, and the same-place mismatch genuinely falling from
  // 77 cm to 12 cm — and made the map measurably WORSE globally (+8.6 %
  // occupied 3 cm voxels). Locally right, globally a fold: the exact failure
  // this file's header calls the worst thing that can happen.
  //
  // ARCore's real indoor drift over a 200 s walk is tens of centimetres and a
  // couple of degrees. Seventeen degrees is not drift. So the bound is set
  // where the physics is, not where ICP is comfortable.
  double max_close_translation_m = 0.60;
  double max_close_rotation_deg = 6.0;

  // --- gate 5: where the map overlaps itself, it has to get crisper --------
  //
  // The one gate that does not depend on any threshold in this struct being
  // right. Every gate above asks ICP or the trajectory whether the closure is
  // plausible; this one applies the correction to the whole cloud and asks
  // the CLOUD, with ROUND 10's own metric — the number of occupied 3 cm
  // voxels, which falls when a surface painted twice becomes a surface
  // painted once, and rises when anything is smeared.
  //
  // AND IT IS ONLY ASKED WHERE THE QUESTION HAS AN ANSWER. Occupancy carries
  // information about a correction only where the walk PAINTED THE SAME PLACE
  // TWICE — that is the only geometry a correction can merge or split. A
  // single lap of a room paints almost every surface exactly once, and
  // re-warping a singly-painted cloud moves the voxel count by a percent or
  // two through resampling alone, in either direction, meaning nothing.
  // Measured both ways this round: on the synthetic single-lap fixture a
  // closure that cut the worst per-point error from 61 cm to 16 cm still
  // RAISED the voxel count 1.8 %, while on the owner's out-and-back scan-020
  // a false 17 deg closure raised it 8.6 %. A gate that fired on the first
  // would veto every good closure there is.
  //
  // So the overlap is measured first — the fraction of occupied voxels
  // holding returns more than `min_loop_seconds` apart — and the crispness
  // rule is enforced only above `min_overlap_for_crispness`. Below it the
  // gate abstains and says so, rather than voting on no evidence.
  bool require_global_crispness = true;
  double min_overlap_for_crispness = 0.05;
  // Fractional slack once the gate IS active. 0.0 means "must not get worse
  // at all": where the walk really did paint a place twice, a correct closure
  // can only merge.
  double crispness_tolerance = 0.0;
  double crispness_voxel_m = 0.03;

  // Pose decimation for the O(N^2) candidate search. 4 Hz over a 200 s walk
  // is 800 knots and 320k pairs, which is nothing; 30 Hz would be 36M.
  // Decimation is by TIME, so it is independent of the pose rate the phone
  // happened to deliver.
  double candidate_stride_s = 0.25;

  // Constructed with A7's defaults retuned for this problem. See the .cpp.
  TrajectoryLoopConfig();
};

enum class LoopDecision {
  kClosed = 0,
  kNoTrajectory,      // fewer than two poses, or no timestamps
  kNoRevisit,         // gate 1: the walk never came back
  kNoExcursion,       // gate 2: it never left
  kThinSubmap,        // not enough points at one or both ends
  kUnobservable,      // gate 3a: the submaps cannot measure one direction at all
  kIcpFailed,         // gate 3: ICP did not converge
  kGeometryRejected,  // gate 3: converged, but the gate said no
  kCorrectionTooBig,  // gate 4: the "drift" was implausibly large
  kMapGotWorse,       // gate 5: applying it blurred the map instead of sharpening it
};

const char* to_string(LoopDecision d);

struct LoopClosureReport {
  LoopDecision decision = LoopDecision::kNoTrajectory;
  // A stable, loggable sentence naming what happened and why. Never null.
  const char* reason = "";

  // The candidate that was examined (valid whenever decision is past
  // kNoRevisit). Indices are into the caller's pose vector.
  std::size_t idx_a = 0;
  std::size_t idx_b = 0;
  std::int64_t t_a_ns = 0;
  std::int64_t t_b_ns = 0;
  double revisit_gap_m = 0.0;    // |p_b - p_a| as the trajectory reported it
  double loop_path_m = 0.0;      // walked distance between them
  double loop_seconds = 0.0;
  double excursion_m = 0.0;
  std::size_t candidates_seen = 0;

  // The geometry (valid from kThinSubmap on).
  std::size_t submap_a_points = 0;
  std::size_t submap_b_points = 0;

  // Gate 3a's evidence. `normal_coverage` is lambda_min / lambda_max of the
  // target submap's normal scatter; `weak_axis` is the world direction that
  // is least constrained, which for a straight walk is the walk itself.
  double normal_coverage = 0.0;
  double weak_axis[3] = {0.0, 0.0, 0.0};
  std::size_t normals_fitted = 0;

  IcpResult icp{};

  // What the closure moved (valid when kClosed).
  double drift_translation_m = 0.0;  // |T_fix translation|
  double drift_rotation_deg = 0.0;
  std::size_t poses_corrected = 0;
  std::size_t points_corrected = 0;

  // THE HEADLINE NUMBER, and it is a property of the POINTS rather than of
  // the correction that was applied to them: the mean distance from a submap-B
  // point to its nearest submap-A point, over the pairs closer than the ICP
  // correspondence gate, measured with the correction OFF and again with it
  // ON. "The same wall, seen 180 s apart, was N cm apart; now it is M cm."
  //
  // It is reported for a REJECTED candidate too — that is the number that says
  // whether the refusal cost anything.
  double submap_mismatch_before_m = 0.0;
  double submap_mismatch_after_m = 0.0;
  std::size_t mismatch_pairs = 0;

  // Gate 5's evidence, filled whenever the candidate got that far — including
  // when it FAILED there, which is the case worth reading. `overlap_fraction`
  // is how much of the map was painted twice and therefore how much weight
  // the crispness comparison carries; `crispness_checked` says whether the
  // gate actually voted.
  std::uint64_t occupied_voxels_before = 0;
  std::uint64_t occupied_voxels_after = 0;
  double overlap_fraction = 0.0;
  bool crispness_checked = false;
};

// The correction C(t) from the note at the top of this file. Cheap to copy,
// safe to keep, and exactly identity when nothing closed.
class TrajectoryCorrection {
 public:
  TrajectoryCorrection() = default;

  // Built by close_trajectory_loop(). `knots` are (t_ns, s) in ascending t,
  // `xi` is the se(3) log of T_fix as (rotation[3], translation[3]).
  void build(std::vector<std::int64_t> knot_t_ns, std::vector<double> knot_s, const double xi[6]);

  bool active() const { return active_; }

  // C(t) as a row-major 4x4.
  void matrix_at(std::int64_t t_ns, double out[16]) const;

  // p <- C(t) * p. No-op when inactive.
  void apply_point(std::int64_t t_ns, float xyz[3]) const;
  void apply_pose(std::int64_t t_ns, double q[4], double p[3]) const;

  // s at t, clamped to [0, 1]. Exposed for tests and for the CLI's reporting.
  double fraction_at(std::int64_t t_ns) const;

 private:
  bool active_ = false;
  std::vector<std::int64_t> t_;
  std::vector<double> s_;
  double xi_[6] = {0, 0, 0, 0, 0, 0};
};

// Detect a revisit in `poses`, verify it against the cloud, and — when every
// gate agrees — build the correction.
//
// `cloud` and `point_times` must be the same length and in the same order:
// point k was resolved at pose-time point_times[k]. That pairing is what
// D6ResolveConfig::out_point_times produces.
//
// NOTHING IS MUTATED. The caller applies `*out` to whatever it owns, which is
// what lets the same decision be inspected, logged and refused.
LoopClosureReport close_trajectory_loop(const std::vector<TrajPose>& poses,
                                        Span<const PointVertex> cloud,
                                        Span<const std::int64_t> point_times,
                                        const TrajectoryLoopConfig& cfg,
                                        TrajectoryCorrection* out);

// --- the exact SE(3) exp/log this file needs --------------------------------
//
// se3.h carries so3_exp/so3_log (rotation only) because nothing before now
// needed the full six. These are the standard closed forms with the
// small-angle series taken below 1e-8 rad, and they round-trip exactly enough
// that Exp(Log(T)) == T to 1e-12 on every rigid T (asserted in the tests).
void se3_log(const double m[16], double xi[6]);
void se3_exp(const double xi[6], double m[16]);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_TRAJECTORY_LOOP_H
