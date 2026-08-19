// auto_level.h — ROUND 20, item 80. The offline backstop for residual mount
// tilt: level the floor, pitch/roll only, ruler voting last.
//
// --- WHY THIS EXISTS --------------------------------------------------------
//
// ROUND 20's adjudication of the owner's scans 054/055/056 measured the trim
// procedure baking OPERATOR HOLD ATTITUDE into `phone_from_lidar`: two trims
// taken 3.5 minutes apart differed by 23.19 degrees, and swapping them moved
// scan-054's floor-tilt-vs-gravity from 3.61 to 0.96 degrees and its
// self-check from 1.97 to 1.22 cm. Items 78/79 fix what the trim measures
// going forward; THIS module is the backstop that retro-fixes every archived
// scan, and the safety net for the residual hold tilt that a start-of-scan
// hold still lets through (a human hand is not a jig).
//
// --- WHAT IS MEASURED, AND WHAT IS HONESTLY NOT ------------------------------
//
// ARCore's world frame is gravity-aligned (+Y up) by construction, so a
// dominant floor plane whose normal is NOT +Y is direct evidence of an
// extrinsics rotation error — the phone's own attitude cannot produce it,
// because the tracker's gravity estimate is what DEFINES +Y. The floor is
// therefore an extrinsics-error meter for exactly two degrees of freedom:
// pitch and roll. It cannot witness yaw (rotating a floor about gravity is a
// symmetry), so this module NEVER touches the yaw component: the correction
// is constructed as the rotation taking the measured floor normal to +Y about
// the horizontal axis `n x Y` — zero about-gravity twist by construction.
//
// --- HOW THE CORRECTION IS APPLIED -------------------------------------------
//
// The error being corrected lives in `phone_from_lidar`, so a world-frame
// cloud rotation would be the WRONG model: `p = W_R_P(t) * M * p_l + t(t)`
// varies per pose. Changing `M` to `dR_P * M` moves a resolved point to
//
//     p' = W_R_P(t) * dR_P * W_R_P(t)^T * (p - t(t)) + t(t)
//
// which needs only the point's own pose — no re-resolve, no re-read, exact up
// to the difference between the recorded 30 Hz attitude and the densified one
// the assembler used (fractions of a degree over one bracket). `dR_P` is the
// world-frame leveling rotation conjugated into the phone frame at the
// trajectory's mean attitude; because a walk changes the phone's yaw, one
// phone-frame rotation does not level every pose identically, so the solve
// ITERATES (fit, correct, re-fit) and the final tilt is re-MEASURED rather
// than assumed. The trajectory itself never moves: the poses are phone
// positions and a mount rotation does not move the phone.
//
// --- GATES, EACH BY NAME ------------------------------------------------------
//
//   no-floor        no plane with an upward normal found in the low band
//   thin-floor      a plane exists but with too few inliers / too little area
//                   to be trusted as "the floor" rather than a bed or a desk
//   already-level   tilt below `min_tilt_deg` — the honest no-op
//   tilt-too-big    past `max_tilt_deg` this is not a residual trim error and
//                   "leveling" would be fitting the wrong surface
//   no-improvement  the iteration could not actually reduce the tilt
//   ruler-says-worse ROUND 12's whole-map self-check votes LAST, exactly as it
//                   does for gap rescue: a leveling that makes the map agree
//                   with itself less is refused however level the floor looks
//
// A refusal mutates NOTHING: the caller's points are byte-identical to what
// it passed in, so a refused container reprocesses to the same map.
//
// Determinism: the plane search uses a fixed-seed xorshift index sequence and
// fixed iteration counts, percentiles are nth_element on copies, and the
// refinement is post_geom's fixed-sweep Jacobi — same points in, same verdict
// out, on every platform.
//
// Owner: ROUND 20.
#ifndef SCANENGINE_SLAM_POST_AUTO_LEVEL_H
#define SCANENGINE_SLAM_POST_AUTO_LEVEL_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/slam/post/map_consistency.h"
#include "scanengine/slam/post/trajectory_loop.h"

namespace scanengine {
namespace post {

struct AutoLevelConfig {
  // Tilt below this is a no-op ("already-level"). 1.5 degrees is the round-11
  // cost table's knee: 1.4 degrees paints an overhead feature 16 cm apart on
  // an out-and-back at 3 m, 0.8 degrees paints it 6.6 cm — so below ~1.5 the
  // correction would be smaller than the measurement's own confidence.
  double min_tilt_deg = 1.5;

  // Tilt above this is refused ("tilt-too-big"): the residual hold tilt this
  // module exists for is a few degrees, and a 20-degree "floor" is far more
  // likely a misidentified surface than a mount error items 78/79 let through.
  double max_tilt_deg = 15.0;

  // RANSAC inlier tolerance, metres — the same 2 cm the round-20 adjudication
  // measured the owner's floors with.
  double plane_tolerance_m = 0.02;

  // Fixed iteration count for the deterministic plane search.
  std::size_t plane_iterations = 500;

  // "Confident floor": at least this many inliers spread over at least this
  // much horizontal area (occupied 0.25 m XZ cells x cell area). A D6 at
  // walking pace paints a real room floor with tens of thousands of returns
  // over several square metres; a bed or a desk fails on area, a doorway
  // glimpse fails on count.
  std::size_t min_inliers = 1500;
  double min_coverage_m2 = 1.0;

  // The candidate band: points whose height is within this fraction of the
  // cloud's [p1, p99] vertical extent above the p1 floor line.
  double band_fraction = 0.25;

  // The fit-correct-refit loop: stop when the tilt is under `converge_deg`
  // or after `max_iterations` rounds.
  double converge_deg = 0.3;
  int max_iterations = 4;

  // Gate: the ruler votes last (see gap_rescue.h, verbatim doctrine).
  bool require_self_consistency = true;
  double self_consistency_tolerance_m = 0.0;
  MapConsistencyConfig consistency{};
};

enum class AutoLevelDecision {
  kApplied = 0,
  kNoFloor,
  kThinFloor,
  kAlreadyLevel,
  kTiltTooBig,
  kNoImprovement,
  kRulerSaysWorse,
  kNotEnoughData,  // no points or no poses at all
};

const char* to_string(AutoLevelDecision d);

struct AutoLevelReport {
  AutoLevelDecision decision = AutoLevelDecision::kNotEnoughData;
  const char* reason = "";

  // The floor, as measured on the INPUT cloud.
  bool floor_found = false;
  std::size_t floor_inliers = 0;
  double floor_coverage_m2 = 0.0;
  double floor_normal[3] = {0.0, 1.0, 0.0};

  // The headline numbers: floor tilt vs gravity before and after. `after` is
  // re-measured on the corrected cloud, never derived from the correction.
  double tilt_before_deg = 0.0;
  double tilt_after_deg = 0.0;

  // The total phone-frame rotation applied to phone_from_lidar's rotation
  // block, (x, y, z, w), and its magnitude. Identity unless kApplied.
  double correction_quat[4] = {0.0, 0.0, 0.0, 1.0};
  double correction_deg = 0.0;
  int iterations = 0;

  // The ruler, whole cloud, before and after (filled when checked — also on
  // a ruler refusal, because the refusal's numbers ARE the product).
  bool self_check_checked = false;
  double self_check_before_m = 0.0;
  double self_check_after_m = 0.0;
};

// Measure the dominant floor's tilt vs gravity and, when the gates allow,
// rotate `points` so the floor is level. `points` is mutated ONLY when the
// verdict is kApplied; every refusal leaves it untouched. `point_times` pairs
// with `points` exactly as D6ResolveConfig::out_point_times documents, and
// `poses` is the same trajectory the resolve produced (corrections applied).
AutoLevelReport auto_level_floor(std::vector<PointVertex>& points,
                                 const std::vector<std::int64_t>& point_times,
                                 const std::vector<TrajPose>& poses, const AutoLevelConfig& cfg);

// The floor-tilt measurement alone (deterministic, non-mutating): fills
// floor_* and tilt_before_deg / decision-if-refused into a report. Exposed so
// a test can pin the meter separately from the corrector.
AutoLevelReport measure_floor_tilt(const std::vector<PointVertex>& points,
                                   const AutoLevelConfig& cfg);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_AUTO_LEVEL_H
