// section_stitch.h — ROUND 13. Put a multi-section capture back into ONE
// frame.
//
// --- WHAT A "SECTION BREAK" ACTUALLY IS ------------------------------------
//
// Measured on the owner's scan-029 and scan-030 (2026-08-18), against the
// recorded 400 Hz phone gyro:
//
//   break            ARCore pose jump        gyro over the SAME 33 ms
//   scan-030 #1      0.78 m / 13.53 deg      0.23 deg
//   scan-030 #2      0.97 m / 11.55 deg      1.16 deg
//   scan-030 #3      1.23 m / 11.58 deg      0.94 deg
//   scan-030 #4      1.06 m /  8.08 deg      0.49 deg
//
// The phone did not rotate. Between the breaks ARCore tracks the gyro to a
// median of 0.07 deg over any one-second window and there is NO ramp before a
// break — the disagreement is flat at 0.04-0.2 deg right up to the frame it
// happens. So a section break is not a tracking failure that the app failed
// to prevent. It is ARCore RE-ANCHORING: recognising a place it has seen
// before, deciding its own estimate had drifted, and snapping its world frame
// to the corrected one. In a small flat walked in loops it will keep doing
// this, and no amount of walking slowly will stop it.
//
// --- WHICH MAKES THE FIX ANALYTIC, NOT A SEARCH ----------------------------
//
// Everything recorded before the snap is expressed in the OLD world frame and
// everything after it in the NEW one. The transform between those two frames
// is not unknown: it is written down in the pose stream, as the jump itself.
//
//     T_k = pose_after * pose_before^-1
//
// (the operator's own motion during the 33 ms gap is inside T_k too, but the
// gyro above bounds it at ~1 deg and the trajectory bounds the translation at
// ~1 cm, so T_k is the frame change to within that.) Pushing every point of
// section k through T_k lands it in section k+1's frame, exactly the way
// trajectory_loop.h's note derives — a world-frame left-multiplication needs
// no re-resolve because
//
//     p_world'(t) = C * T_world_phone(t) * T_phone_lidar * p_lidar = C * p_world(t)
//
// So the correction is piecewise CONSTANT (one rigid transform per section),
// where trajectory_loop.h's is a geodesic in arc length. That difference is
// the physics: VIO drift accumulates smoothly with distance walked, and a
// re-anchor happens in one frame.
//
// THE FALSIFIABLE PROOF that the direction is right, and it comes from
// gravity rather than from any metric this file computes. The operator walks
// on a flat floor. Before correction scan-030's trajectory wanders 0.82 m
// VERTICALLY over a 40 s walk; applying T_k forward flattens it to 0.27 m,
// which is a phone moving in a hand. Applying the inverse instead takes it to
// 1.55 m. scan-029: 0.64 m -> 0.30 m forward, 1.20 m inverted. The sign is
// not a matter of opinion.
//
// --- AND WHY THE REFINEMENT SOLVES FOR TRANSLATION ONLY --------------------
//
// ROUND 12 left the loop closer unavailable for exactly one reason: on a
// pushbroom, point-to-plane ICP has a null space along the walk, so it does
// not fail — it WANDERS, and proposed 14-19 deg rotations against a gyro that
// says <= 1.1 deg over 16 s. The rotation was never the unknown. Here it is
// less unknown still: T_k already carries it, and the gyro says the residual
// rotation across a 33 ms seam is under 1.2 deg.
//
// So the refinement freezes rotation at T_k's and solves the 3-vector that is
// actually in doubt. Point-to-plane with the rotation fixed is a plain 3x3
// normal-equation solve,
//
//     (sum_i w_i n_i n_i^T) dt = -sum_i w_i n_i (n_i . (s_i - t_i))
//
// and the matrix on the left IS the observability: its smallest eigenvalue
// relative to its largest says, exactly and without a proxy, how well the
// available surfaces constrain a translation. ROUND 11 had to estimate that
// from a scatter of sampled normals because its unknown was six-dimensional;
// with three unknowns the system matrix answers the question itself. Below
// `min_translation_observability` the seam keeps its analytic value and says
// so by name.
//
// Deterministic, hand-rolled, no Eigen, no RNG, no clock: a fixed iteration
// count, a fixed Jacobi sweep count, correspondences visited in point order,
// and every accumulation in the same order on every run.
//
// Owner: ROUND 13.
#ifndef SCANENGINE_SLAM_POST_SECTION_STITCH_H
#define SCANENGINE_SLAM_POST_SECTION_STITCH_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/span.h"
#include "scanengine/poses/reanchor.h"
#include "scanengine/slam/post/trajectory_loop.h"

namespace scanengine {
namespace post {

struct SectionStitchConfig {
  // --- detection -----------------------------------------------------------
  //
  // These three MIRROR android/core/.../capture/PoseSections.kt
  // (PoseSectionTracker.MAX_SPEED_MPS / MAX_TURN_RATE_DEG_PER_S /
  // MIN_DT_SECONDS). The engine had no notion of a section before ROUND 13,
  // and the app's `sectionBreaks` list is in project.json, which is the APP
  // manifest and not something engine/ reads. Rather than teach record/ a new
  // chunk type for a quantity that is a pure function of the pose stream the
  // container already holds, the seams are re-derived here from the same
  // rule. If the two ever disagree the container is the loser, so the
  // constants are duplicated deliberately and named on both sides.
  double max_speed_mps = 6.0;
  double max_turn_rate_deg_s = 400.0;
  double min_dt_s = 0.008;

  // --- ROUND 17 item 63: the LONG gap ---------------------------------------
  //
  // The three rules above are RATES, and a rate cannot see the failure that
  // broke the owner's scan-040. The tracker was blind for 6.065 s and came
  // back 0.678 m / 66.21 deg from where it let go — 0.11 m/s and 10.9 deg/s,
  // under every threshold, so this detector found ONE section in a capture
  // with a 66-degree fold in the middle of it. (Item 62 recorded exactly this
  // and called it "bridged rather than split", which was true and was not the
  // whole truth: nothing was bridged, the seam was simply invisible here.)
  //
  // So a run of poses the tracker DISOWNED, between two it did not, is a seam
  // candidate on its own — no rate involved. What happens to a candidate is
  // reanchor.h's decision, not this file's, and it needs the gyro to make it:
  // without `gyro` a long gap can only ever be refused, which is the correct
  // and useless answer, so a caller that has the recorded IMU should pass it.
  //
  // `bridge_long_gaps = false` restores the ROUND-13 detector exactly.
  bool bridge_long_gaps = true;
  const reanchor::GyroBridge* gyro = nullptr;
  reanchor::GapPolicy gap{};

  // --- refinement ----------------------------------------------------------
  bool refine = true;
  // Half-width of the submap taken either side of a seam. Six seconds is
  // ROUND 11's number and the reason is unchanged: it is long enough to hold
  // a wall and short enough that ARCore's own drift inside it is under the
  // centimetre ROUND 12 measured.
  double submap_half_window_s = 6.0;
  std::size_t min_submap_points = 600;
  std::size_t max_submap_points = 40000;

  double max_correspondence_m = 0.50;
  double huber_m = 0.10;
  // 40/1 mm rather than 25/0.1 mm. The correspondences are re-found every
  // iteration, so the step size floor is set by which points changed partner
  // and not by the solve; asking for 0.1 mm makes the last iterations a coin
  // toss and reports "not converged" on a seam that had already settled to
  // well inside a millimetre. A millimetre is two orders below the five
  // centimetres this is fighting.
  std::uint32_t max_iterations = 40;
  double converge_translation_m = 1e-3;
  std::size_t min_pairs = 200;
  // Neighbourhood for the target's local plane fits.
  double plane_radius_m = 0.25;
  double max_planarity_ratio = 0.10;

  // Gate: lambda_min / lambda_max of sum(n n^T), the translation system
  // matrix. 0.05 is ROUND 11's `min_normal_coverage`, kept so the two gates
  // are calibrated against each other.
  double min_translation_observability = 0.05;
  // Gate: how far the refinement may move a seam ON TOP of the analytic
  // transform. The analytic value is a measurement; a large correction to it
  // means the correspondences, not the pose stream, are wrong.
  double max_refine_translation_m = 0.30;
};

enum class SeamDecision {
  kAnalytic = 0,      // the jump transform, unrefined (refinement off, or it abstained)
  kRefined,           // analytic + a measured translation
  kNoTrajectory,      // no poses either side
  kThinSubmap,        // not enough points either side of the seam
  kUnobservable,      // the surfaces cannot measure a translation at all
  kNotConverged,      // the solve did not settle
  kRefinementTooBig,  // it settled somewhere implausible
  kMapGotWorse,       // the refinement made the two sides agree less
  // ROUND 17: a seam across a gap long enough for the operator to have moved
  // inside it. `kBridged` reached the correction through the gyro instead of
  // through the raw jump; the three refusals below produced NO correction and
  // NO section split, because inventing one is what item 63 exists to stop.
  kBridged,
  kGapRefusedNoGyro,
  kGapRefusedTooLong,
  kGapRefusedDisagree,
  // The gyro and the tracker agreed: the jump was the operator walking, not
  // the frame moving. Nothing to correct, and nothing wrong.
  kGapNegligible,
};

const char* to_string(SeamDecision d);

struct SectionSeam {
  std::size_t index = 0;  // 0-based seam; joins section `index` to `index + 1`
  std::size_t pose_before = 0;
  std::size_t pose_after = 0;
  std::int64_t t_ns = 0;
  double gap_s = 0.0;
  // What the pose stream jumped, i.e. what ARCore corrected.
  double jump_translation_m = 0.0;
  double jump_rotation_deg = 0.0;
  // T_k, row-major 4x4: section `index`'s frame -> section `index + 1`'s.
  double analytic[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // Translation the refinement added, in the final (last section's) frame.
  double refine_delta[3] = {0.0, 0.0, 0.0};

  // ROUND 17 item 63. What the recorded gyro says the OPERATOR turned across
  // the same span, and what is left of the jump once that is taken out. On a
  // 33 ms seam the two residuals are the jump itself (nobody moves in 33 ms);
  // on scan-040's 6.065 s gap the gyro says 144.94 deg against the tracker's
  // 66.21, and the 78.91 deg left over is why that seam is refused.
  double gyro_rotation_deg = 0.0;
  double residual_translation_m = 0.0;
  double residual_rotation_deg = 0.0;

  SeamDecision decision = SeamDecision::kAnalytic;
  const char* reason = "";
  double observability = 0.0;
  std::size_t pairs = 0;
  std::size_t submap_before_points = 0;
  std::size_t submap_after_points = 0;
  // Mean nearest-neighbour distance across the seam, analytic-only and then
  // refined. The number that says whether the refinement earned its place.
  double mismatch_analytic_m = 0.0;
  double mismatch_refined_m = 0.0;
};

// The correction: one rigid transform per section, bringing everything into
// the LAST section's frame. Exactly identity when there is one section, which
// is why running this on a clean capture is a provable no-op.
class SectionCorrection {
 public:
  SectionCorrection() = default;

  // `seam_t_ns` ascending, length N-1 for N sections; `per_section` length N,
  // row-major 4x4 each.
  void build(std::vector<std::int64_t> seam_t_ns, std::vector<double> per_section_4x4);

  bool active() const { return active_; }
  std::size_t sections() const { return m_.size() / 16; }

  // Which section a timestamp belongs to. A point stamped exactly at a seam
  // belongs to the LATER section, matching the detector: the jump is measured
  // between pose_before and pose_after, and t_ns is pose_after's stamp.
  std::size_t section_of(std::int64_t t_ns) const;

  void matrix_at(std::int64_t t_ns, double out[16]) const;
  void apply_point(std::int64_t t_ns, float xyz[3]) const;
  void apply_pose(std::int64_t t_ns, double q[4], double p[3]) const;

 private:
  bool active_ = false;
  std::vector<std::int64_t> seam_t_;
  std::vector<double> m_;  // 16 doubles per section
};

struct SectionStitchReport {
  std::size_t sections = 1;
  std::vector<SectionSeam> seams;
  // ROUND 17 item 63: every long gap this pass LOOKED at, seam or not, with
  // the numbers it judged on. A gap that resolved to kGapNegligible or to one
  // of the refusals is not in `seams` — it moved nothing — and would otherwise
  // vanish without trace, which is exactly how scan-040's six blind seconds
  // came to be reported as "1 section".
  std::vector<SectionSeam> gaps_examined;
  std::size_t gaps_refused = 0;
  // The longest stretch the tracker was blind for, seconds. The one number
  // that says "this capture has a hole in it" whatever was decided about it.
  double longest_gap_s = 0.0;
  std::size_t poses = 0;
  std::size_t points = 0;
  // How far the correction moves the FIRST section's origin — the headline
  // "the map was this far apart" number.
  double total_translation_m = 0.0;
  double total_rotation_deg = 0.0;
  // Vertical (gravity-axis) extent of the trajectory before and after. The
  // operator walks on a flat floor, so this is the one check on the RESULT
  // that does not come from the same measurement that produced it. `up_axis`
  // says which component was read (1 = +Y, ARCore's world).
  double trajectory_vertical_extent_before_m = 0.0;
  double trajectory_vertical_extent_after_m = 0.0;
  // Start-to-end gap, before and after. Before stitching this number compares
  // two points in DIFFERENT world frames and is therefore meaningless; after
  // it is real, and it is ARCore's own drift rather than anything this module
  // can fix. Reported so the card can say so instead of implying otherwise.
  double trajectory_end_gap_before_m = 0.0;
  double trajectory_end_gap_after_m = 0.0;
  // Poses the tracker disowned, i.e. recorded with no world frame at all.
  std::size_t poses_untracked = 0;
  int up_axis = 1;
  const char* summary = "";
};

// Derive the seams from `poses`, build the correction, and — when `refine` is
// on and the geometry allows — measure the residual translation at each seam.
//
// `cloud` and `point_times` must be the same length and in the same order
// (D6ResolveConfig::out_point_times). They may both be empty, in which case
// the analytic correction is still produced: the seams are a property of the
// pose stream alone.
//
// NOTHING IS MUTATED. The caller applies `*out`.
SectionStitchReport stitch_sections(const std::vector<TrajPose>& poses,
                                    Span<const PointVertex> cloud,
                                    Span<const std::int64_t> point_times,
                                    const SectionStitchConfig& cfg, SectionCorrection* out);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_SECTION_STITCH_H
