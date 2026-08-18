// loop_end.cpp — ROUND 16 item 60. See loop_end.h for the derivation and for
// why the rotation is frozen rather than solved.

#include "scanengine/slam/post/loop_end.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/slam/post/map_consistency.h"
#include "scanengine/poses/se3.h"

#include "point_grid.h"
#include "post_geom.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "loopend";

using detail::occupied_voxels;
using detail::plane_at;
using detail::revisit_overlap;
using detail::solve3_spd;
using detail::submap_at;
using detail::sym_eigen3;

double dist3(const double a[3], const double b[3]) {
  const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Mean nearest-neighbour distance from `source` shifted by `offset` to
// `target`, over the pairs closer than `gate_m`. Measured on the POINTS, so
// applying the correction cannot flatter it.
double mean_nn(const detail::PointIndex& index, const std::vector<PointVertex>& source,
               const double offset[3], double gate_m, std::size_t* out_pairs) {
  double sum = 0.0;
  std::size_t pairs = 0;
  std::uint32_t idx = 0;
  double d2 = 0.0;
  for (const PointVertex& v : source) {
    const double q[3] = {v.x + offset[0], v.y + offset[1], v.z + offset[2]};
    if (index.knn(q, 1, gate_m, &idx, &d2) == 1) {
      sum += std::sqrt(d2);
      ++pairs;
    }
  }
  if (out_pairs != nullptr) *out_pairs = pairs;
  return pairs > 0 ? sum / static_cast<double>(pairs) : 0.0;
}

// ROUND 13's refinement, with the seam replaced by a loop end: rotation
// frozen, translation solved, the 3x3 system matrix reporting its own
// observability. `source` is the LATER visit and `target` the earlier one, so
// `dt` is where the tail of the walk should have been.
enum class SolveOutcome { kOk, kThin, kUnobservable, kNotConverged };

SolveOutcome solve_translation(const std::vector<PointVertex>& source,
                               const std::vector<PointVertex>& target, const LoopEndConfig& cfg,
                               double dt[3], double* observability, double weak[3],
                               std::size_t* out_pairs, std::uint32_t* out_iterations) {
  dt[0] = dt[1] = dt[2] = 0.0;
  *observability = 0.0;
  weak[0] = weak[1] = weak[2] = 0.0;
  *out_pairs = 0;
  *out_iterations = 0;
  if (source.size() < cfg.min_submap_points || target.size() < cfg.min_submap_points) {
    return SolveOutcome::kThin;
  }

  detail::PointIndex index;
  index.build(&target[0].x, 4, target.size(), cfg.max_correspondence_m);

  bool converged = false;
  for (std::uint32_t it = 0; it < cfg.max_iterations; ++it) {
    *out_iterations = it + 1;
    double a[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    double b[3] = {0, 0, 0};
    std::size_t pairs = 0;
    for (const PointVertex& v : source) {
      const double s[3] = {v.x + dt[0], v.y + dt[1], v.z + dt[2]};
      double nrm[3], cen[3];
      if (!plane_at(index, target, s, cfg.plane_radius_m, cfg.max_planarity_ratio, nrm, cen)) {
        continue;
      }
      const double d =
          nrm[0] * (s[0] - cen[0]) + nrm[1] * (s[1] - cen[1]) + nrm[2] * (s[2] - cen[2]);
      if (std::fabs(d) > cfg.max_correspondence_m) continue;
      const double ad = std::fabs(d);
      const double w = ad <= cfg.huber_m ? 1.0 : cfg.huber_m / ad;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) a[r * 3 + c] += w * nrm[r] * nrm[c];
        b[r] -= w * nrm[r] * d;
      }
      ++pairs;
    }
    *out_pairs = pairs;
    if (pairs < cfg.min_pairs) return SolveOutcome::kThin;

    // The system matrix IS the observability. Normalised by the pair count so
    // the eigenvalues are a mean outer product and the ratio is scale free.
    double an[9];
    for (int i = 0; i < 9; ++i) an[i] = a[i] / static_cast<double>(pairs);
    double ev[3], vec[9];
    sym_eigen3(an, ev, vec);
    *observability = ev[2] > 1e-12 ? ev[0] / ev[2] : 0.0;
    weak[0] = vec[0];
    weak[1] = vec[3];
    weak[2] = vec[6];
    if (*observability < cfg.min_translation_observability) return SolveOutcome::kUnobservable;

    double step[3];
    if (!solve3_spd(a, b, step)) return SolveOutcome::kUnobservable;
    dt[0] += step[0];
    dt[1] += step[1];
    dt[2] += step[2];
    const double mag =
        std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
    if (mag < cfg.converge_translation_m) {
      converged = true;
      break;
    }
  }
  return converged ? SolveOutcome::kOk : SolveOutcome::kNotConverged;
}

}  // namespace

const char* to_string(LoopEndDecision d) {
  switch (d) {
    case LoopEndDecision::kClosed: return "closed";
    case LoopEndDecision::kNoTrajectory: return "no-trajectory";
    case LoopEndDecision::kNoRevisit: return "no-revisit";
    case LoopEndDecision::kNoExcursion: return "no-excursion";
    case LoopEndDecision::kThinSubmap: return "thin-submap";
    case LoopEndDecision::kUnobservable: return "unobservable";
    case LoopEndDecision::kNotConverged: return "not-converged";
    case LoopEndDecision::kCorrectionTooBig: return "correction-too-big";
    case LoopEndDecision::kNoImprovement: return "no-improvement";
    case LoopEndDecision::kMapGotWorse: return "map-got-worse";
    case LoopEndDecision::kRulerSaysWorse: return "ruler-says-worse";
  }
  return "unknown";
}

LoopEndReport close_loop_end(const std::vector<TrajPose>& poses, Span<const PointVertex> cloud,
                             Span<const std::int64_t> point_times, const LoopEndConfig& cfg,
                             TrajectoryCorrection* out) {
  LoopEndReport rep;
  if (out != nullptr) *out = TrajectoryCorrection{};

  if (poses.size() < 2 || cloud.size() != point_times.size() || cloud.empty()) {
    rep.decision = LoopEndDecision::kNoTrajectory;
    rep.reason = "loop end: no trajectory, or the cloud and its timestamps disagree in length";
    return rep;
  }

  // --- cumulative path length, in pose order -------------------------------
  std::vector<double> cum(poses.size(), 0.0);
  for (std::size_t i = 1; i < poses.size(); ++i) {
    cum[i] = cum[i - 1] + dist3(poses[i].p, poses[i - 1].p);
  }
  rep.end_gap_before_m = dist3(poses.back().p, poses.front().p);

  // --- decimate for the O(N^2) search --------------------------------------
  const std::int64_t stride_ns = static_cast<std::int64_t>(cfg.candidate_stride_s * 1e9);
  std::vector<std::size_t> knot;
  knot.push_back(0);
  for (std::size_t i = 1; i < poses.size(); ++i) {
    if (stride_ns <= 0 || poses[i].t_ns - poses[knot.back()].t_ns >= stride_ns) knot.push_back(i);
  }
  if (knot.back() != poses.size() - 1) knot.push_back(poses.size() - 1);

  // --- gates 1 and 2 -------------------------------------------------------
  //
  // Identical in shape to trajectory_loop.cpp's — `a` outer, `b` ascending
  // inner with a running maximum of |p_b - p_a|, which IS the excursion over
  // [a, b] because a maximum over a growing prefix is monotone — with gate 2
  // made scale-aware. `saw_spatial` distinguishes "never came back" from
  // "came back but never left", which are different verdicts for the operator
  // and different bugs for us.
  bool have = false;
  std::size_t best_a = 0, best_b = 0;
  double best_path = 0.0, best_gap = 0.0, best_exc = 0.0, best_dt = 0.0;
  bool saw_spatial = false;

  for (std::size_t ia = 0; ia + 1 < knot.size(); ++ia) {
    const std::size_t a = knot[ia];
    double run_max = 0.0;
    for (std::size_t ib = ia + 1; ib < knot.size(); ++ib) {
      const std::size_t b = knot[ib];
      const double gap = dist3(poses[b].p, poses[a].p);
      if (gap > run_max) run_max = gap;
      const double dt = static_cast<double>(poses[b].t_ns - poses[a].t_ns) * 1e-9;
      if (dt < cfg.min_loop_seconds) continue;
      const double path = cum[b] - cum[a];
      if (path < cfg.min_loop_path_m) continue;
      if (gap > cfg.max_revisit_m) continue;
      saw_spatial = true;
      ++rep.candidates_seen;
      if (run_max < cfg.min_excursion_m) continue;
      if (run_max < cfg.min_excursion_over_gap * gap) continue;
      // Prefer the loop that closes the most path; ties to the tighter gap,
      // then to the smaller indices. Same rule as ROUND 11's, so the two
      // closers pick the same candidate when both are offered one.
      const bool better = !have || path > best_path + 1e-12 ||
                          (std::fabs(path - best_path) <= 1e-12 &&
                           (gap < best_gap - 1e-12 ||
                            (std::fabs(gap - best_gap) <= 1e-12 &&
                             (a < best_a || (a == best_a && b < best_b)))));
      if (better) {
        have = true;
        best_a = a;
        best_b = b;
        best_path = path;
        best_gap = gap;
        best_exc = run_max;
        best_dt = dt;
      }
    }
  }

  if (!have) {
    rep.decision = saw_spatial ? LoopEndDecision::kNoExcursion : LoopEndDecision::kNoRevisit;
    rep.reason =
        saw_spatial
            ? "loop end: the walk came back to a place it had been but never went far enough "
              "away in between for the return to measure anything — a rig turning on the spot, "
              "not a loop"
            : "loop end: the walk never returned to a place it had already been, so there is "
              "nothing to close (the normal and correct answer for a one-way walk)";
    SCAN_LOG_INFO(kMod, "no closure: %s (%llu spatial candidates)", to_string(rep.decision),
                  static_cast<unsigned long long>(rep.candidates_seen));
    return rep;
  }

  rep.idx_a = best_a;
  rep.idx_b = best_b;
  rep.t_a_ns = poses[best_a].t_ns;
  rep.t_b_ns = poses[best_b].t_ns;
  rep.revisit_gap_m = best_gap;
  rep.loop_path_m = best_path;
  rep.loop_seconds = best_dt;
  rep.excursion_m = best_exc;

  // --- the submaps ---------------------------------------------------------
  const std::int64_t half_ns = static_cast<std::int64_t>(cfg.submap_half_window_s * 1e9);
  const std::vector<PointVertex> sa =
      submap_at(cloud, point_times, rep.t_a_ns, half_ns, cfg.max_submap_points);
  const std::vector<PointVertex> sb =
      submap_at(cloud, point_times, rep.t_b_ns, half_ns, cfg.max_submap_points);
  rep.submap_a_points = sa.size();
  rep.submap_b_points = sb.size();
  if (sa.size() < cfg.min_submap_points || sb.size() < cfg.min_submap_points) {
    rep.decision = LoopEndDecision::kThinSubmap;
    rep.reason = "loop end: one of the two visits has too few returns to compare — refusing to "
                 "close on evidence that thin";
    SCAN_LOG_INFO(kMod, "no closure: thin submap (%zu / %zu points)", sa.size(), sb.size());
    return rep;
  }

  // --- the solve: rotation frozen, translation measured --------------------
  double dt[3] = {0.0, 0.0, 0.0};
  const SolveOutcome outcome =
      solve_translation(sb, sa, cfg, dt, &rep.observability, rep.weak_axis, &rep.pairs,
                        &rep.iterations);

  // The before/after mismatch, built whatever the verdict — a refusal that
  // reports zeroes is a refusal nobody can audit.
  detail::PointIndex ta;
  ta.build(&sa[0].x, 4, sa.size(), cfg.max_correspondence_m);
  const double zero[3] = {0.0, 0.0, 0.0};
  rep.submap_mismatch_before_m =
      mean_nn(ta, sb, zero, cfg.max_correspondence_m, &rep.mismatch_pairs);
  rep.submap_mismatch_after_m = mean_nn(ta, sb, dt, cfg.max_correspondence_m, nullptr);

  rep.correction[0] = dt[0];
  rep.correction[1] = dt[1];
  rep.correction[2] = dt[2];
  rep.correction_translation_m = std::sqrt(dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2]);
  // Zero by construction, not by tolerance: the solver has no rotational
  // degree of freedom to spend. Reported so a field log can check the claim.
  rep.correction_rotation_deg = 0.0;

  switch (outcome) {
    case SolveOutcome::kThin:
      rep.decision = LoopEndDecision::kThinSubmap;
      rep.reason = "loop end: too few plane correspondences between the two visits to solve a "
                   "translation";
      SCAN_LOG_INFO(kMod, "no closure: %zu pairs", rep.pairs);
      return rep;
    case SolveOutcome::kUnobservable:
      rep.decision = LoopEndDecision::kUnobservable;
      rep.reason = "loop end: the surfaces at the two ends face too nearly the same way to "
                   "measure a translation in every direction — a pushbroom down a straight "
                   "corridor cannot close its own loop, and inventing the missing component is "
                   "exactly the fold this refuses to make";
      SCAN_LOG_INFO(kMod,
                    "no closure: observability %.4f < %.4f; weakest direction "
                    "(%.3f, %.3f, %.3f)",
                    rep.observability, cfg.min_translation_observability, rep.weak_axis[0],
                    rep.weak_axis[1], rep.weak_axis[2]);
      return rep;
    case SolveOutcome::kNotConverged:
      rep.decision = LoopEndDecision::kNotConverged;
      rep.reason = "loop end: the translation solve did not settle — the two visits are not the "
                   "same place, or one of them has no usable surface";
      SCAN_LOG_INFO(kMod, "no closure: not converged after %u iterations", rep.iterations);
      return rep;
    case SolveOutcome::kOk:
      break;
  }

  // --- gate 4: magnitude ---------------------------------------------------
  if (rep.correction_translation_m > cfg.max_close_translation_m) {
    rep.decision = LoopEndDecision::kCorrectionTooBig;
    rep.reason = "loop end: the closing translation is larger than VIO drift can plausibly be "
                 "over this walk — treating it as a mismatched place rather than a big "
                 "correction";
    SCAN_LOG_WARN(kMod, "no closure: correction %.3f m exceeds the plausible bound %.3f m",
                  rep.correction_translation_m, cfg.max_close_translation_m);
    return rep;
  }

  // --- gate 5: the same place has to come together -------------------------
  if (!(rep.submap_mismatch_before_m - rep.submap_mismatch_after_m >
        cfg.min_mismatch_improvement_m)) {
    rep.decision = LoopEndDecision::kNoImprovement;
    rep.reason = "loop end: the correction did not bring the two visits of the same place any "
                 "closer together, so there is no evidence it is the right correction";
    SCAN_LOG_INFO(kMod, "no closure: mismatch %.4f -> %.4f m", rep.submap_mismatch_before_m,
                  rep.submap_mismatch_after_m);
    return rep;
  }

  // --- build the correction ------------------------------------------------
  //
  // A PURE TRANSLATION: the rotation half of the se(3) vector is exactly
  // zero, so Exp(s * xi) is a pure translation at every s and the arc-length
  // geodesic degenerates to linear interpolation of the offset. This is the
  // whole of "gyro-locked" — nothing downstream can rotate a point, because
  // there is no rotation in the correction to apply.
  const double xi[6] = {0.0, 0.0, 0.0, dt[0], dt[1], dt[2]};
  std::vector<std::int64_t> kt;
  std::vector<double> ks;
  kt.reserve(poses.size());
  ks.reserve(poses.size());
  const double span = cum[best_b] - cum[best_a];
  for (std::size_t i = 0; i < poses.size(); ++i) {
    double s;
    if (i <= best_a) {
      s = 0.0;
    } else if (i >= best_b) {
      s = 1.0;
    } else {
      s = span > 1e-9 ? (cum[i] - cum[best_a]) / span : 0.0;
    }
    // Strictly ascending stamps only — a repeated pose time would make the
    // interpolation's bracketing ambiguous.
    if (!kt.empty() && poses[i].t_ns <= kt.back()) continue;
    kt.push_back(poses[i].t_ns);
    ks.push_back(s);
  }
  if (kt.size() < 2) {
    rep.decision = LoopEndDecision::kNoTrajectory;
    rep.reason = "loop end: the pose stream has no two distinct timestamps to interpolate over";
    return rep;
  }

  TrajectoryCorrection corr;
  corr.build(kt, ks, xi);

  // --- gate 6: crispness where the map overlaps (ROUND 11's gate 5) --------
  //
  // Applied to the WHOLE cloud and asked of the CLOUD, not of the solver. And
  // only asked where the question has an answer: occupancy carries
  // information about a correction only where the walk painted the same place
  // twice, so below `min_overlap_for_crispness` the gate abstains and says so
  // rather than voting on no evidence.
  std::vector<PointVertex> moved;
  {
    const std::int64_t apart_ns = static_cast<std::int64_t>(cfg.min_loop_seconds * 1e9);
    std::vector<PointVertex> as_is(cloud.begin(), cloud.end());
    rep.overlap_fraction = revisit_overlap(as_is, point_times, cfg.crispness_voxel_m, apart_ns);
    rep.occupied_voxels_before = occupied_voxels(as_is, cfg.crispness_voxel_m);
    moved = std::move(as_is);
    for (std::size_t i = 0; i < moved.size(); ++i) {
      float xyz[3] = {moved[i].x, moved[i].y, moved[i].z};
      corr.apply_point(point_times[i], xyz);
      moved[i].x = xyz[0];
      moved[i].y = xyz[1];
      moved[i].z = xyz[2];
    }
    rep.occupied_voxels_after = occupied_voxels(moved, cfg.crispness_voxel_m);
  }
  if (cfg.require_global_crispness && rep.overlap_fraction >= cfg.min_overlap_for_crispness) {
    rep.crispness_checked = true;
    const double allowed = static_cast<double>(rep.occupied_voxels_before) *
                           (1.0 + cfg.crispness_tolerance);
    if (static_cast<double>(rep.occupied_voxels_after) > allowed) {
      rep.decision = LoopEndDecision::kMapGotWorse;
      rep.reason = "loop end: applying the correction spread the map out where the walk had "
                   "painted the same place twice, which is what a WRONG correction does — "
                   "refusing it";
      SCAN_LOG_WARN(kMod, "no closure: occupied voxels %llu -> %llu (overlap %.3f)",
                    static_cast<unsigned long long>(rep.occupied_voxels_before),
                    static_cast<unsigned long long>(rep.occupied_voxels_after),
                    rep.overlap_fraction);
      return rep;
    }
  }

  // --- gate 7: the ruler has the last word ---------------------------------
  //
  // ROUND 12's map self-consistency over the WHOLE cloud, before and after,
  // and it is the only gate here that is not computed from the two submaps the
  // solver chose. That independence is the entire point: see
  // LoopEndConfig::require_self_consistency for the capture that taught it.
  //
  // `moved` is already the corrected cloud — gate 6 built it — so this costs
  // one ruler pass and no second warp.
  if (cfg.require_self_consistency) {
    std::vector<PointVertex> as_is(cloud.begin(), cloud.end());
    const std::vector<std::int64_t> times(point_times.begin(), point_times.end());
    const MapConsistencyReport rb = measure_map_consistency(as_is, times, cfg.consistency);
    if (rb.measurable) {
      const MapConsistencyReport ra = measure_map_consistency(moved, times, cfg.consistency);
      rep.self_check_checked = true;
      rep.self_check_before_m = rb.nearest_offset_m;
      rep.self_check_after_m = ra.measurable ? ra.nearest_offset_m : rb.nearest_offset_m;
      if (!ra.measurable ||
          ra.nearest_offset_m > rb.nearest_offset_m + cfg.self_consistency_tolerance_m) {
        rep.decision = LoopEndDecision::kRulerSaysWorse;
        rep.reason =
            "loop end: the correction made the map agree with itself LESS — the same surface, "
            "painted twice, ended up further apart than it started. Every other gate is "
            "computed from the two places the solver matched; this one is computed from the "
            "whole map, and it is the number the summary card prints, so a closure that fails "
            "it would make the operator's own measurement worse";
        SCAN_LOG_WARN(kMod, "no closure: self-consistency %.4f -> %.4f m",
                      rep.self_check_before_m, rep.self_check_after_m);
        return rep;
      }
    }
  }

  // --- accepted ------------------------------------------------------------
  rep.decision = LoopEndDecision::kClosed;
  rep.reason = "loop end: closed on a measured translation with the rotation held at the "
               "tracker's own (which the recorded gyro agrees with), distributed along the "
               "walk by arc length";
  rep.poses_corrected = poses.size();
  rep.points_corrected = cloud.size();

  // The end gap AFTER, computed the way the card computes it: the corrected
  // last pose against the corrected first one.
  {
    double qa[4] = {poses.front().q[0], poses.front().q[1], poses.front().q[2],
                    poses.front().q[3]};
    double pa[3] = {poses.front().p[0], poses.front().p[1], poses.front().p[2]};
    double qb[4] = {poses.back().q[0], poses.back().q[1], poses.back().q[2], poses.back().q[3]};
    double pb[3] = {poses.back().p[0], poses.back().p[1], poses.back().p[2]};
    corr.apply_pose(poses.front().t_ns, qa, pa);
    corr.apply_pose(poses.back().t_ns, qb, pb);
    rep.end_gap_after_m = dist3(pb, pa);
  }

  SCAN_LOG_INFO(kMod,
                "closed: %.3f m translation (0.000 deg by construction), same place %.1f -> "
                "%.1f cm over %zu pairs, end gap %.3f -> %.3f m, observability %.4f",
                rep.correction_translation_m, rep.submap_mismatch_before_m * 100.0,
                rep.submap_mismatch_after_m * 100.0, rep.mismatch_pairs, rep.end_gap_before_m,
                rep.end_gap_after_m, rep.observability);

  if (out != nullptr) *out = corr;
  return rep;
}

}  // namespace post
}  // namespace scanengine
