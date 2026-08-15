// align.h — the three COARSE alignment paths of Tech Spec §3.10.
//
//   "Coarse: automatic when sessions are georeferenced (shared CRS);
//    otherwise manual 3-point / drag alignment."
//
// Coarse alignment is the part that has to be right *enough* for ICP to
// converge to the right basin. ICP itself cannot recover from a wrong basin —
// it will happily and confidently register a corridor onto the wrong corridor
// — so each of these three returns not just a transform but the evidence for
// it, and the two that can be wrong say how confident they are:
//
//   1. GEOREFERENCED (free, and the only one that is not a guess). Two
//      sessions in a shared CRS are already in a common frame; the merge
//      transform is a composition through ECEF, exact to ~1e-9 m. Its
//      ACCURACY is not this module's: it is A10's transform accuracy, which
//      `GeorefSolution` already quantifies honestly (yaw sigma x lever arm
//      dominates). §3 of docs/A13-merge.md tabulates what that means in mm.
//   2. MANUAL CORRESPONDENCES (the operator picks matching points). A rigid
//      (optionally similarity) fit by Horn's quaternion method, with the
//      residual per pick, the implied scale, and a COLLINEARITY gate —
//      because three points on a line define a transform only up to a
//      rotation about that line, and a UI that lets an operator pick three
//      points along one wall must be told so rather than handed a plausible
//      matrix.
//   3. YAW + TRANSLATION SEARCH (the fallback nobody should trust blindly).
//      Documented at length in yaw_translation_search() below and in
//      docs/A13-merge.md §5, including the two ways it is known to fail.
//
// Owner: A13.
#ifndef SCANENGINE_MERGE_ALIGN_H
#define SCANENGINE_MERGE_ALIGN_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/merge/session.h"

namespace scanengine {
namespace merge {

// --- 1. the georeferenced composition ---------------------------------------

// `to_from` = the rigid transform carrying a point in ENU frame `from` into
// ENU frame `to`, via ECEF:
//
//     X_ecef  = R_from * X_from + o_from
//     X_to    = R_to^T (X_ecef - o_to)
//
// so R = R_to^T R_from and t = R_to^T (o_from - o_to). Exact — no projection,
// no series, no small-angle assumption — and it is NOT the identity for two
// frames a kilometre apart: the ENU basis rotates with the ellipsoid normal,
// ~0.009 deg per km, which is 16 cm of vertical error at 1 km if you skip it.
// Returns kInvalidArgument if either frame is invalid.
Status enu_from_enu(const crs::EnuFrame& to, const crs::EnuFrame& from, double to_from[16]);

// --- 2. manual correspondences ----------------------------------------------

// One operator pick: the same physical feature seen in two frames.
struct PointCorrespondence {
  double a[3] = {0.0, 0.0, 0.0};  // in the source frame (the session being placed)
  double b[3] = {0.0, 0.0, 0.0};  // in the target frame (the merged world)
  double weight = 1.0;            // 0 or negative drops the pick
};

struct CorrespondenceOptions {
  // Solve for a scale as well. Default OFF: both clouds come from metric
  // SLAM, and a free scale silently absorbs a bad pick into a stretch of the
  // whole cloud — the same argument A10 makes for `lock_scale`
  // (gnss/georef.h). `implied_scale` below reports what the scale WOULD have
  // been either way, which is the diagnostic an operator actually wants.
  bool allow_scale = false;
  // Collinearity gate on the SOURCE picks. Three points define a rigid
  // transform only if they are not on one line; the middle singular value of
  // the pick spread is how far they are from being one. A pick set is
  // rejected when sqrt(lambda_1) is below `min_spread_m` OR below
  // `min_spread_ratio` x sqrt(lambda_2).
  double min_spread_m = 0.05;
  double min_spread_ratio = 0.02;
  std::size_t min_pairs = 3;
};

struct CorrespondenceSolution {
  bool ok = false;
  double b_from_a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  double scale = 1.0;          // 1.0 unless allow_scale
  double implied_scale = 1.0;  // what a free scale would have been
  double rms_m = 0.0;
  double max_residual_m = 0.0;
  std::vector<double> residuals_m;  // parallel to the input picks
  std::size_t pairs = 0;
  // Spread of the source picks, as the square roots of the eigenvalues of
  // their weighted scatter matrix, ASCENDING (metres). For three picks
  // spread[0] is always ~0 (three points are coplanar), so it is spread[1]
  // that says "not collinear" — see CorrespondenceOptions.
  double spread_m[3] = {0.0, 0.0, 0.0};
  const char* blocker = "";  // stable, loggable; empty when ok
};

// Weighted Horn (1987) absolute orientation: the rotation is the eigenvector
// of the largest eigenvalue of a 4x4 symmetric matrix built from the
// cross-covariance, found by cyclic Jacobi with a fixed sweep order.
//
// Why not SVD/Kabsch: the quaternion form cannot return a reflection (a
// unit quaternion is a rotation by construction), so the det<0 correction
// that a 3x3-SVD Kabsch needs — and that is easy to get subtly wrong for a
// degenerate, i.e. exactly-three-point, pick set — does not exist here. Why
// not Ceres/Eigen: docs/A8-pushbroom.md §2. The whole solver is ~120 lines
// and deterministic on all five CI legs.
CorrespondenceSolution solve_correspondences(Span<const PointCorrespondence> picks,
                                             const CorrespondenceOptions& opts = {});

// --- 3. the yaw + translation fallback --------------------------------------

struct YawSearchConfig {
  // Both clouds are voxel-downsampled to this before anything else happens.
  double work_voxel_m = 0.25;
  // Yaw sweep, degrees. The search is over rotation about +Z ONLY: both local
  // frames are gravity-aligned (A6's ESKF static init, ARCore, and A10's
  // `local_is_gravity_aligned` precondition all guarantee it), so roll and
  // pitch are already common and searching them would only add two
  // unobservable dimensions.
  double yaw_min_deg = -180.0;
  double yaw_max_deg = 180.0;
  double yaw_step_deg = 2.0;
  // Translation search bound and the histogram bin the correlation runs on.
  double max_translation_m = 40.0;
  double hist_bin_m = 0.25;
  // Scoring: occupancy overlap at this voxel size, over at most this many
  // sampled source points (strided, never truncated).
  double score_voxel_m = 0.30;
  std::uint32_t score_samples = 20000;
  // A yaw is a "distinct" runner-up only this far from the winner. The margin
  // between the winner and the best distinct runner-up is the honest
  // confidence number — see YawSearchResult::margin.
  double distinct_yaw_deg = 20.0;
  // Gates. Below `min_overlap` the search reports failure instead of a
  // transform; below `min_margin` it reports the transform AND says it is
  // ambiguous, because a symmetric building genuinely has two answers.
  double min_overlap = 0.30;
  double min_margin = 0.05;
};

struct YawSearchResult {
  bool ok = false;         // cleared both gates
  bool ambiguous = false;  // best overlap is fine, margin is not
  double b_from_a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  double yaw_deg = 0.0;
  double translation[3] = {0.0, 0.0, 0.0};
  double overlap = 0.0;           // occupancy overlap of the winner, 0..1
  double runner_up_overlap = 0.0; // best score at a distinct yaw
  double runner_up_yaw_deg = 0.0;
  double margin = 0.0;            // overlap - runner_up_overlap
  std::uint32_t yaws_scored = 0;
  std::uint64_t source_points = 0, target_points = 0;  // after downsampling
  const char* blocker = "";
};

// Coarse alignment for two overlapping, gravity-aligned, Manhattan-ish clouds
// with NO georeferencing and NO operator picks.
//
// HOW IT WORKS, so its failure modes are predictable rather than surprising:
// for each candidate yaw the rotated source's occupancy is projected onto the
// x, y and z axes as three 1-D histograms and each is cross-correlated
// against the target's. A building's walls, floor and ceiling put sharp peaks
// in those marginals, so the correlation peak is the translation — separably,
// three 1-D searches instead of one 3-D one, which is what makes a full
// 360-degree sweep cost milliseconds instead of minutes. The winner is then
// re-scored by actual occupancy overlap in 3-D, which is the number reported.
//
// WHERE IT IS UNRELIABLE — measured, not guessed (docs/A13-merge.md §5):
//   * A rotationally symmetric floor plan has two or four equally good
//     answers. The search finds them all and `margin` collapses; `ambiguous`
//     is the flag, and a UI must ask rather than merge.
//   * Partial overlap moves marginal mass that the other cloud never saw, and
//     the peak walks off. Below ~40% overlap this degrades quickly.
//   * A curved or diagonal building has no marginal peaks to correlate. It
//     does not crash — it returns a low overlap and `ok = false`.
// It is a fallback, in that order of preference: georeference the sessions,
// or pick three points. This is what you use when neither is available.
YawSearchResult yaw_translation_search(Span<const PointVertex> source,
                                       Span<const PointVertex> target,
                                       const YawSearchConfig& cfg = {});

}  // namespace merge
}  // namespace scanengine

#endif  // SCANENGINE_MERGE_ALIGN_H
