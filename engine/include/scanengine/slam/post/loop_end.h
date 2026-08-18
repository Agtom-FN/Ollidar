// loop_end.h — ROUND 16 item 60. Close the gap at the END of a walk with the
// solver that actually applies to a pushbroom: rotation frozen at what the
// gyro already proved, translation solved.
//
// --- THE MEASUREMENT THIS EXISTS TO REMOVE ---------------------------------
//
// The owner's own 0.9.0 field session, every capture through the shipping
// app:
//
//     scan-033   28.0 m walked, 110.8 s   loop end gap 0.58 m
//     scan-036   11.7 m walked,  46.0 s   loop end gap 0.65 m
//     scan-038   16.8 m walked, 108.2 s   loop end gap 0.52 m
//
// and his verdict on the round-15 build was "scan look ok but not much
// improved". He is right, and this is the number that says so. ROUND 12
// measured the LOCAL geometry of these captures at 0.70 cm (crawl) to 5.3 cm
// (walking) of same-surface disagreement at 8 s separation, and ROUND 15's
// ruler puts scan-036 at 3.43 cm and scan-038 at 2.73 cm. Local geometry is
// excellent. The trajectory drifts half a metre. Half a metre is seventy
// times the local error, so nothing else in the pipeline is worth a round
// until this is claimed.
//
// --- WHY ROUND 11's CLOSER NEVER FIRED ON THESE ----------------------------
//
// It is in the tree, it works, and on the owner's captures it refuses. Run
// today (`engine_cli --d6-loopclose`) it says:
//
//     scan-033   geometry-rejected  (ICP rms 0.285 m > 0.25; it also proposed
//                                    5.72 deg of rotation)
//     scan-036   no-excursion       (furthest-from-start 3.58 m < 4.0 m)
//     scan-038   no-excursion       (furthest-from-start 3.68 m < 4.0 m)
//
// Two different failures, and neither is the closer being wrong about
// geometry:
//
//  1. **`min_excursion_m = 4.0` is a corridor's number and the owner scans a
//     flat.** The gate exists so that a rig shuffling on the spot cannot
//     count as a loop (ROUND 11 wrote it for exactly that), and that purpose
//     is served by *how far the walk went compared to how far it missed by*,
//     not by an absolute metre count. scan-036 walked 11.7 m around a room
//     3.58 m across and came back 0.65 m out; that is a loop by any reading,
//     and a fixed 4 m veto simply says "your flat is too small to have its
//     drift measured".
//
//  2. **Six-degree-of-freedom ICP is the wrong estimator here, and ROUND 12
//     proved it before ROUND 13 acted on it.** On a pushbroom, point-to-plane
//     ICP has a null space along the walk: it does not fail, it WANDERS, and
//     the rotations it proposes (14-19 deg in ROUND 12, 5.72 deg on scan-033
//     here) are contradicted by the recorded 400 Hz gyro, which tracks ARCore
//     to r = 0.9994 and disagrees by under 1.2 deg over any window that
//     matters. ROUND 13 drew the conclusion for section seams: **the rotation
//     was never the unknown.** This file draws it for loop ends.
//
// --- SO: GYRO-LOCKED, TRANSLATION-ONLY -------------------------------------
//
// The closing transform is constrained to a pure translation. Not "solved for
// six and hope the rotation comes out small" — *constrained*, so that the
// rotation is exactly, bit-identically, zero at every point of the
// correction. The solve is ROUND 13's, verbatim in method: rotation frozen,
// point-to-plane with three unknowns,
//
//     (sum_i w_i n_i n_i^T) dt = -sum_i w_i n_i (n_i . (s_i - t_i))
//
// and the 3x3 matrix on the left IS the observability — its smallest
// eigenvalue relative to its largest says exactly how well the surfaces at
// the two ends constrain a translation, with no sampled-normal proxy needed.
// Below `min_translation_observability` this refuses and names the reason,
// which is the same gate, at the same 0.05, that ROUND 11 and ROUND 13 use.
//
// The correction is then distributed along the walk by ARC LENGTH, exactly as
// trajectory_loop.h derives — VIO drift accumulates with distance travelled,
// not with seconds elapsed, so an operator who stands still for 30 s must not
// be handed a third of the correction. Because the transform is a pure
// translation, `C(s) = Exp(s * Log(T_fix))` degenerates to plain linear
// interpolation of the offset, and TrajectoryCorrection is reused unchanged:
// the rotation half of its se(3) vector is set to zero and therefore Exp
// cannot produce a rotation at any s. That is what "gyro-locked" means here —
// a property of the type, not a tolerance.
//
// And no point needs re-resolving, for the identity trajectory_loop.h states:
// left-multiplying the pose by a world-frame correction left-multiplies the
// point by the same correction.
//
// --- THE GATES, AND WHY EVERY ROUND-11 REFUSAL STILL STANDS -----------------
//
// A false closure remains the worst thing this file can do, so ROUND 11's
// gates are kept and only the two named above are changed:
//
//   1. SPATIAL REVISIT — unchanged (`max_revisit_m`, `min_loop_seconds`,
//      `min_loop_path_m`). A walk that never comes back produces no candidate
//      at all. This is why scan-034 (5.4 m of path) still refuses: absence,
//      not threshold.
//   2. EXCURSION — now scale-aware: the walk must have left by
//      `min_excursion_m` **and** by `min_excursion_over_gap` times the gap it
//      is closing. scan-035 (a sweep: 10.1 m of path inside a 1.55 m
//      neighbourhood) still refuses on the absolute floor, which is the exact
//      case ROUND 11 wrote the gate for.
//   3. OBSERVABILITY — the translation system matrix, gate at 0.05.
//   4. MAGNITUDE — `max_close_translation_m`. With rotation frozen a large
//      correction cannot FOLD the map, only slide it, so the bound is set
//      from measured VIO drift (0.45-0.68 m over these walks) with headroom
//      rather than from ICP's comfort.
//   5. THE SAME PLACE HAS TO AGREE BETTER — mean nearest-neighbour distance
//      between the two submaps, measured on the POINTS, before and after. A
//      correction that does not improve it is refused however plausible it
//      looked.
//   6. CRISPNESS WHERE THE MAP OVERLAPS — ROUND 11's gate 5, unchanged,
//      including its abstention below `min_overlap_for_crispness`.
//   7. AND THE RULER HAS THE LAST WORD. ROUND 12's map self-consistency,
//      measured over the WHOLE cloud before and after, must not get worse.
//      This gate is not decoration: on the owner's scan-036 gates 1-6 all
//      passed — the two submaps came 4.2 cm closer and the end gap fell
//      0.30 m — and the ruler went 3.43 -> 4.46 cm. See `require_self_
//      consistency` below for why every earlier gate is structurally
//      incapable of catching that.
//
// Every decision, including every refusal and which gate did the refusing,
// comes back as a stable string.
//
// --- WHAT IT DOES ON THE OWNER'S OWN CAPTURES (measured, this round) --------
//
//   scan-033  CLOSED              0.118 m; ruler 1.97 -> 1.66 cm (-15 %),
//                                 occupied 3 cm voxels -1.01 %, loop gap
//                                 0.581 -> 0.566 m, observability 0.358
//   scan-036  ruler-says-worse    0.336 m proposed, the two ends came 4.8 cm
//                                 together — and the ruler would have gone
//                                 3.43 -> 4.52 cm. Refused by gate 7.
//   scan-038  correction-too-big  1.389 m proposed, over the 1.00 m bound.
//   scan-034  no-revisit          5.4 m of path, under the 8 m floor.
//   scan-035  no-excursion        10.1 m walked inside a 1.55 m neighbourhood
//                                 — the sweep case ROUND 11 wrote gate 2 for.
//   scan-039  no-trajectory       no poses were recorded at all (item 58).
//
// One closure in six, and that is the honest state of it: this claims the
// lever, it does not yet claim the room. Two things are worth saying about
// the five refusals. First, they are the product working — each names a gate
// and a number, and scan-036 is the case where the last gate overruled the
// first six. Second, the loop GAP is not the target and must not be sold as
// one: on scan-033 the geometry says the walk genuinely ended 0.57 m from
// where it started, so most of that gap is where the operator stopped, not
// drift. What the closure removes is the part the map disagrees with itself
// about, and that is what the ruler measures.
//
// OFFLINE ONLY. This moves points the live pass could not have moved, so it
// runs inside `Process` and never behind a re-resolve's back — the same rule
// `close_loops` and `stitch_sections` live by.
//
// Owner: ROUND 16 item 60.
#ifndef SCANENGINE_SLAM_POST_LOOP_END_H
#define SCANENGINE_SLAM_POST_LOOP_END_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/span.h"
#include "scanengine/slam/post/map_consistency.h"
#include "scanengine/slam/post/trajectory_loop.h"

namespace scanengine {
namespace post {

struct LoopEndConfig {
  // --- gate 1: the spatial revisit (ROUND 11's numbers, unchanged) ---------
  double max_revisit_m = 3.0;
  double min_loop_seconds = 15.0;
  double min_loop_path_m = 8.0;

  // --- gate 2: the excursion, made scale-aware -----------------------------
  //
  // The absolute floor is what keeps a sweep out: the owner's scan-035 walked
  // 10.1 m without ever leaving a 1.55 m neighbourhood, and that is a rig
  // turning on the spot, not a loop. The ratio is what lets a small room in:
  // a walk that went 3.6 m out and came back 0.65 m wide has closed a loop
  // whatever the absolute size of the flat.
  double min_excursion_m = 3.0;
  double min_excursion_over_gap = 4.0;

  // --- the submaps (ROUND 11's numbers) ------------------------------------
  double submap_half_window_s = 6.0;
  std::size_t min_submap_points = 600;
  std::size_t max_submap_points = 40000;

  // --- the translation solve (ROUND 13's numbers) --------------------------
  double max_correspondence_m = 0.80;
  double huber_m = 0.10;
  std::uint32_t max_iterations = 40;
  double converge_translation_m = 1e-3;
  std::size_t min_pairs = 200;
  // THE ONE NUMBER THAT IS NOT ROUND 13's, and the reason is the difference
  // between the two problems.
  //
  // At a section seam the analytic transform has already removed the jump, so
  // what is left to solve is centimetres and a 25 cm neighbourhood is
  // generous. At a loop end there is no analytic transform: what is left to
  // solve is the WHOLE accumulated drift, which on these captures is 20-60 cm.
  // A source point is matched by finding target points within this radius of
  // it — so with a 25 cm radius, every surface whose normal points ALONG the
  // drift is displaced clean out of reach and contributes nothing, while
  // surfaces perpendicular to the drift (which carry no information about it)
  // match perfectly. The solver is then handed a system matrix that is
  // singular in exactly the direction the answer lies, and refuses.
  //
  // Measured on this round's fixture: with 0.25 m the weakest direction came
  // back as (1.000, -0.001, -0.002) — the X axis, which is where 0.30 m of the
  // 0.36 m injected drift lives — at an observability of 0.022. With 0.60 m it
  // is a well-conditioned problem. The radius has to be able to reach across
  // the error being measured; that is not a tuning parameter, it is the
  // geometry of the question.
  double plane_radius_m = 0.60;
  double max_planarity_ratio = 0.10;
  // gate 3, and it is the system matrix itself. Same 0.05 as ROUND 11's
  // min_normal_coverage and ROUND 13's min_translation_observability, kept
  // equal so the three gates stay calibrated against each other.
  double min_translation_observability = 0.05;

  // --- gate 4: magnitude ---------------------------------------------------
  //
  // ROUND 11 set 0.60 m because its six-DoF solver could pair a large
  // translation with a large rotation and fold the room. This solver cannot
  // rotate anything, so the failure mode a bound protects against is only
  // "the two places were not the same place". The owner's measured gaps are
  // 0.45-0.68 m; 1.0 m is those plus room, and it is still far below the
  // 3.0 m `max_revisit_m` shortlist bound.
  double max_close_translation_m = 1.00;

  // --- gate 5: the same place has to agree better --------------------------
  //
  // Measured on the points, so applying the correction cannot flatter it.
  // Strictly greater than zero improvement is required: a correction that
  // leaves the two ends exactly as far apart as it found them has measured
  // nothing.
  double min_mismatch_improvement_m = 0.0;

  // --- gate 6: crispness where the map overlaps (ROUND 11's gate 5) --------
  bool require_global_crispness = true;
  double min_overlap_for_crispness = 0.05;
  double crispness_tolerance = 0.0;
  double crispness_voxel_m = 0.03;

  // --- gate 7: THE RULER HAS THE LAST WORD ---------------------------------
  //
  // ROUND 16 added this after measuring the first three real captures, and it
  // is the gate that changed the answer on one of them.
  //
  // Every gate above asks the SOLVER, or the two submaps the solver used,
  // whether the closure looks right — and on the owner's scan-036 every one of
  // them said yes. The two submaps came 4.2 cm closer together (24.75 ->
  // 20.58 cm mean nearest-neighbour), the observability was a healthy 0.39, the
  // correction was a plausible 0.39 m, and the trajectory's end gap fell from
  // 1.16 m to 0.87 m. Then ROUND 12's ruler — same-surface disagreement at 8 s
  // repaint, measured over the WHOLE map and over every pair of windows, not
  // over the two the solver chose — went from 3.43 cm to 4.46 cm. The map got
  // worse. A translation that slides a cloud until it lands on SOME nearby
  // surface always reduces the mean nearest-neighbour distance between the two
  // clouds it was fitted to; that number is the solver's own residual wearing a
  // different hat, and it cannot referee itself.
  //
  // So the ruler votes last, and it votes on the metric the summary card
  // already prints — which means a closure can only ship if the number the
  // operator is shown improves. It abstains when the map was never measurable
  // (a single pass down a corridor paints nothing twice), because an
  // abstention is a legitimate answer and a fabricated one is not.
  bool require_self_consistency = true;
  double self_consistency_tolerance_m = 0.0;
  MapConsistencyConfig consistency{};

  // Pose decimation for the O(N^2) candidate search, by TIME.
  double candidate_stride_s = 0.25;
};

enum class LoopEndDecision {
  kClosed = 0,
  kNoTrajectory,      // fewer than two poses, or the cloud and times disagree
  kNoRevisit,         // gate 1: the walk never came back
  kNoExcursion,       // gate 2: it never left, or not far enough for the gap
  kThinSubmap,        // not enough points at one or both ends
  kUnobservable,      // gate 3: the surfaces cannot measure a translation
  kNotConverged,      // the solve did not settle
  kCorrectionTooBig,  // gate 4
  kNoImprovement,     // gate 5: the same place did not come together
  kMapGotWorse,       // gate 6: the overlap blurred instead of sharpening
  kRulerSaysWorse,    // gate 7: the map agreed with itself LESS afterwards
};

const char* to_string(LoopEndDecision d);

struct LoopEndReport {
  LoopEndDecision decision = LoopEndDecision::kNoTrajectory;
  const char* reason = "";

  // The candidate (valid from kNoExcursion on).
  std::size_t idx_a = 0;
  std::size_t idx_b = 0;
  std::int64_t t_a_ns = 0;
  std::int64_t t_b_ns = 0;
  double revisit_gap_m = 0.0;
  double loop_path_m = 0.0;
  double loop_seconds = 0.0;
  double excursion_m = 0.0;
  std::size_t candidates_seen = 0;

  // The geometry.
  std::size_t submap_a_points = 0;
  std::size_t submap_b_points = 0;
  double observability = 0.0;
  // The world direction the two ends constrain LEAST — the eigenvector of the
  // smallest eigenvalue of the translation system matrix. On a straight
  // corridor this is the corridor. Reported because "unobservable" is a much
  // more useful refusal when it can say in which direction.
  double weak_axis[3] = {0.0, 0.0, 0.0};
  std::size_t pairs = 0;
  std::uint32_t iterations = 0;

  // What was applied. `correction_rotation_deg` exists and is asserted zero:
  // it is the field that makes "gyro-locked" checkable from a field log
  // rather than a claim in a header.
  double correction_translation_m = 0.0;
  double correction_rotation_deg = 0.0;
  double correction[3] = {0.0, 0.0, 0.0};
  std::size_t poses_corrected = 0;
  std::size_t points_corrected = 0;

  // The headline pair: how far apart the same place was, and how far apart it
  // is now. Reported for a REFUSED candidate too — that is the number saying
  // whether the refusal cost anything.
  double submap_mismatch_before_m = 0.0;
  double submap_mismatch_after_m = 0.0;
  std::size_t mismatch_pairs = 0;

  // The trajectory's own start-to-end gap, before and after. This is the
  // number the summary card prints, so it is the number the operator can
  // check the claim with.
  double end_gap_before_m = 0.0;
  double end_gap_after_m = 0.0;

  // Gate 6's evidence.
  std::uint64_t occupied_voxels_before = 0;
  std::uint64_t occupied_voxels_after = 0;
  double overlap_fraction = 0.0;
  bool crispness_checked = false;

  // Gate 7's evidence — ROUND 12's ruler, before and after, in metres. This is
  // the same quantity the summary card prints as "surfaces repeat within X cm",
  // so a refusal here is a refusal the operator can check.
  bool self_check_checked = false;
  double self_check_before_m = 0.0;
  double self_check_after_m = 0.0;
};

// Detect the loop end, verify it against the cloud, and — when every gate
// agrees — build a PURE-TRANSLATION correction distributed along arc length.
//
// `cloud` and `point_times` must be the same length and in the same order
// (D6ResolveConfig::out_point_times).
//
// NOTHING IS MUTATED. The caller applies `*out`.
LoopEndReport close_loop_end(const std::vector<TrajPose>& poses, Span<const PointVertex> cloud,
                             Span<const std::int64_t> point_times, const LoopEndConfig& cfg,
                             TrajectoryCorrection* out);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_LOOP_END_H
