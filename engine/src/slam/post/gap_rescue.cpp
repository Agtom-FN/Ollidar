// gap_rescue.cpp — ROUND 19 item 73. See gap_rescue.h for the derivation:
// rotation locked to the gyro's witness, translation found by a coarse grid
// plus ROUND 13's point-to-plane solve in the observable subspace, and the
// ruler voting last.

#include "scanengine/slam/post/gap_rescue.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

#include "point_grid.h"
#include "post_geom.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "gaprescue";

using detail::plane_at;
using detail::sym_eigen3;

// Points of the cloud whose stamp falls in [t0, t1), decimated to at most
// `max_points` — section_stitch.cpp's window_points, same shape for the same
// reason (a half-open one-sided window, visited in point order).
std::vector<PointVertex> window_points(Span<const PointVertex> cloud,
                                       Span<const std::int64_t> times, std::int64_t t0,
                                       std::int64_t t1, std::size_t max_points) {
  std::vector<PointVertex> out;
  std::size_t n = 0;
  for (std::size_t i = 0; i < times.size(); ++i) {
    if (times[i] >= t0 && times[i] < t1) ++n;
  }
  if (n == 0) return out;
  const std::size_t stride = (max_points > 0 && n > max_points) ? (n / max_points + 1) : 1;
  out.reserve(n / stride + 1);
  std::size_t seen = 0;
  for (std::size_t i = 0; i < times.size(); ++i) {
    if (times[i] < t0 || times[i] >= t1) continue;
    if (seen % stride == 0) out.push_back(cloud[i]);
    ++seen;
  }
  return out;
}

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

// ROUND 13's translation solve with one addition: the step lives in the
// OBSERVABLE SUBSPACE of the system matrix. Directions whose eigenvalue ratio
// clears the gate take a Newton step; the rest take none and stay wherever
// stage 1 left them (which its tie-break biased toward zero). `dt` is in/out:
// it arrives holding the coarse seed.
enum class SolveOutcome { kOk, kThin, kUnobservable, kNotConverged };

SolveOutcome solve_translation_subspace(const std::vector<PointVertex>& source,
                                        const std::vector<PointVertex>& target,
                                        const GapRescueConfig& cfg, double dt[3],
                                        double* observability, double weak[3], int* solved_axes,
                                        std::size_t* out_pairs, std::uint32_t* out_iterations) {
  *observability = 0.0;
  weak[0] = weak[1] = weak[2] = 0.0;
  *solved_axes = 0;
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

    double an[9];
    for (int i = 0; i < 9; ++i) an[i] = a[i] / static_cast<double>(pairs);
    double ev[3], vec[9];
    sym_eigen3(an, ev, vec);  // ascending; eigenvectors are COLUMNS of vec
    if (!(ev[2] > 1e-12)) return SolveOutcome::kUnobservable;
    *observability = ev[0] / ev[2];
    weak[0] = vec[0];
    weak[1] = vec[3];
    weak[2] = vec[6];

    int axes = 0;
    double step[3] = {0.0, 0.0, 0.0};
    for (int k = 0; k < 3; ++k) {
      if (ev[k] / ev[2] < cfg.min_translation_observability) continue;
      ++axes;
      const double vk[3] = {vec[k], vec[3 + k], vec[6 + k]};
      // A vk = (pairs * ev[k]) vk, so the subspace Newton step along vk is
      // (vk . b) / (pairs * ev[k]).
      const double coeff =
          (vk[0] * b[0] + vk[1] * b[1] + vk[2] * b[2]) / (static_cast<double>(pairs) * ev[k]);
      step[0] += coeff * vk[0];
      step[1] += coeff * vk[1];
      step[2] += coeff * vk[2];
    }
    *solved_axes = axes;
    // Fewer than two measurable directions is a corridor seen end-on: the one
    // constraint left cannot pin a plane's worth of ambiguity, and inventing
    // the rest is what this module exists to refuse.
    if (axes < 2) return SolveOutcome::kUnobservable;

    dt[0] += step[0];
    dt[1] += step[1];
    dt[2] += step[2];
    const double mag = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
    if (mag < cfg.converge_translation_m) {
      converged = true;
      break;
    }
  }
  return converged ? SolveOutcome::kOk : SolveOutcome::kNotConverged;
}

GapRescueReport refuse(GapRescueReport r, GapRescueDecision d, const char* why) {
  r.decision = d;
  r.reason = why;
  return r;
}

}  // namespace

const char* to_string(GapRescueDecision d) {
  switch (d) {
    case GapRescueDecision::kRescued: return "rescued";
    case GapRescueDecision::kNoGyro: return "rescue-no-gyro";
    case GapRescueDecision::kNoAnchor: return "rescue-no-anchor";
    case GapRescueDecision::kThinSubmap: return "rescue-thin-submap";
    case GapRescueDecision::kNoOverlap: return "rescue-no-overlap";
    case GapRescueDecision::kUnobservable: return "rescue-unobservable";
    case GapRescueDecision::kNotConverged: return "rescue-not-converged";
    case GapRescueDecision::kCorrectionTooBig: return "rescue-correction-too-big";
    case GapRescueDecision::kNoImprovement: return "rescue-no-improvement";
    case GapRescueDecision::kRulerSaysWorse: return "rescue-ruler-says-worse";
    case GapRescueDecision::kDegenerate: return "rescue-degenerate";
  }
  return "unknown";
}

GapRescueReport rescue_gap(const std::vector<TrajPose>& poses, Span<const PointVertex> cloud,
                           Span<const std::int64_t> point_times, std::size_t pose_before,
                           std::size_t pose_after, const GapRescueConfig& cfg) {
  GapRescueReport rep;
  rep.pose_before = pose_before;
  rep.pose_after = pose_after;

  if (pose_before >= poses.size() || pose_after >= poses.size() || pose_after <= pose_before) {
    return refuse(rep, GapRescueDecision::kDegenerate, "the gap's pose indices are not usable");
  }
  const TrajPose& pb = poses[pose_before];
  const TrajPose& pa = poses[pose_after];
  rep.t_before_ns = pb.t_ns;
  rep.t_after_ns = pa.t_ns;
  if (pa.t_ns <= pb.t_ns) {
    return refuse(rep, GapRescueDecision::kDegenerate, "the two anchor poses are not in order");
  }
  rep.gap_s = static_cast<double>(pa.t_ns - pb.t_ns) * 1e-9;
  if (pb.tracking_lost != 0 || pb.quality == 0 || pa.tracking_lost != 0 || pa.quality == 0) {
    return refuse(rep, GapRescueDecision::kNoAnchor,
                  "a disowned pose is not a frame to rescue from — the rescue needs one "
                  "TRACKED pose on each side of the blindness, and this gap has none on at "
                  "least one side (scan-039's case: no tracked pose anywhere, so no anchor, "
                  "so no rescue)");
  }
  if (cloud.size() != point_times.size() || cloud.empty()) {
    return refuse(rep, GapRescueDecision::kDegenerate,
                  "the cloud and its per-point timestamps disagree in length");
  }

  // --- the lock: what the gyro witnessed ------------------------------------
  if (cfg.gyro == nullptr) {
    return refuse(rep, GapRescueDecision::kNoGyro,
                  "no gyro was supplied — an unconstrained rescue would be the free-rotation "
                  "ICP round 12 measured inventing 14-19 degrees, and is refused by name");
  }
  double q_gyro[4] = {0, 0, 0, 1};
  double peak = 0.0;
  bool hole = false;
  if (!cfg.gyro->relative_rotation(pb.t_ns, pa.t_ns, q_gyro, &peak, &hole) || hole) {
    return refuse(rep, GapRescueDecision::kNoGyro,
                  "the gyro does not cover the blind window continuously — the rotation "
                  "cannot be locked, so the rescue is refused rather than guessed");
  }
  double qb[4] = {pb.q[0], pb.q[1], pb.q[2], pb.q[3]};
  double qa[4] = {pa.q[0], pa.q[1], pa.q[2], pa.q[3]};
  double qg[4] = {q_gyro[0], q_gyro[1], q_gyro[2], q_gyro[3]};
  if (!se3::quat_normalize(qb) || !se3::quat_normalize(qa) || !se3::quat_normalize(qg)) {
    return refuse(rep, GapRescueDecision::kDegenerate, "a rotation in the gap is degenerate");
  }
  {
    double Rg[9];
    se3::quat_to_matrix(qg, Rg);
    const double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    rep.gyro_rotation_deg = se3::rot_angle_deg(I, Rg);
  }

  // T0 = M_after * M_pred^-1 with the prediction anchored at p_before: "he
  // ended where he lost tracking", wrong by exactly the walk stage 1 searches.
  double q_pred[4];
  se3::quat_mul(qb, qg, q_pred);
  se3::quat_normalize(q_pred);
  double t0[16];
  {
    double ma[16], mp[16], mpi[16];
    se3::mat4_from_quat_pos(qa, pa.p, ma);
    se3::mat4_from_quat_pos(q_pred, pb.p, mp);
    se3::mat4_inverse_rigid(mp, mpi);
    se3::mat4_mul(ma, mpi, t0);
  }
  if (!se3::mat4_is_rigid(t0, 1e-4)) {
    return refuse(rep, GapRescueDecision::kDegenerate,
                  "the locked-rotation transform is not rigid");
  }
  {
    double R[9], t[3];
    se3::mat4_get_rt(t0, R, t);
    const double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    rep.rotation_applied_deg = se3::rot_angle_deg(I, R);
  }

  // --- the submaps ----------------------------------------------------------
  const std::int64_t half_ns = static_cast<std::int64_t>(cfg.submap_half_window_s * 1e9);
  std::vector<PointVertex> src_raw =
      window_points(cloud, point_times, pb.t_ns - half_ns, pb.t_ns, cfg.max_submap_points);
  std::vector<PointVertex> tgt =
      window_points(cloud, point_times, pa.t_ns, pa.t_ns + half_ns, cfg.max_submap_points);
  rep.submap_before_points = src_raw.size();
  rep.submap_after_points = tgt.size();
  if (src_raw.size() < cfg.min_submap_points || tgt.size() < cfg.min_submap_points) {
    return refuse(rep, GapRescueDecision::kThinSubmap,
                  "too few points on one side of the gap to register anything — a rescue "
                  "needs walls on both sides of the blindness");
  }

  // The refused state, measured before anything moves: how well the two sides
  // agree with NO transform. Near-zero pairs is the honest reading of a large
  // fold, and gate 5 abstains on it (the ruler still votes).
  detail::PointIndex ti;
  ti.build(&tgt[0].x, 4, tgt.size(), cfg.max_correspondence_m);
  const double zero3[3] = {0, 0, 0};
  rep.mismatch_identity_m =
      mean_nn(ti, src_raw, zero3, cfg.max_correspondence_m, &rep.mismatch_identity_pairs);

  // Source through the locked rotation.
  std::vector<PointVertex> src = src_raw;
  for (PointVertex& v : src) {
    const double in[3] = {v.x, v.y, v.z};
    double o[3];
    se3::mat4_apply(t0, in, o);
    v.x = static_cast<float>(o[0]);
    v.y = static_cast<float>(o[1]);
    v.z = static_cast<float>(o[2]);
  }

  // --- stage 1: the coarse grid ---------------------------------------------
  //
  // Scored on a deterministic decimation of the source. Ties prefer the
  // smaller mean distance, then the smaller |dt| — the second tie-break is
  // what keeps an unobservable direction at zero instead of at the grid edge.
  std::vector<PointVertex> coarse;
  {
    const std::size_t stride =
        (cfg.coarse_source_points > 0 && src.size() > cfg.coarse_source_points)
            ? (src.size() / cfg.coarse_source_points + 1)
            : 1;
    coarse.reserve(src.size() / stride + 1);
    for (std::size_t i = 0; i < src.size(); i += stride) coarse.push_back(src[i]);
  }
  const int up = (cfg.up_axis >= 0 && cfg.up_axis <= 2) ? cfg.up_axis : 1;
  const int h0 = (up == 0) ? 1 : 0;
  const int h1 = (up == 2) ? 1 : 2;
  const int nh = std::max(0, static_cast<int>(std::floor(
                                 cfg.search_radius_m / std::max(cfg.search_step_m, 1e-6))));
  const int nv = std::max(0, static_cast<int>(std::floor(cfg.search_vertical_m /
                                                         std::max(cfg.search_vertical_step_m,
                                                                  1e-6))));
  std::size_t best_pairs = 0;
  double best_mean = 0.0;
  double best_norm2 = 0.0;
  double best_dt[3] = {0, 0, 0};
  bool have_best = false;
  for (int iv = -nv; iv <= nv; ++iv) {
    for (int ia = -nh; ia <= nh; ++ia) {
      for (int ib = -nh; ib <= nh; ++ib) {
        double dt[3] = {0, 0, 0};
        dt[up] = iv * cfg.search_vertical_step_m;
        dt[h0] = ia * cfg.search_step_m;
        dt[h1] = ib * cfg.search_step_m;
        std::size_t pairs = 0;
        const double mean = mean_nn(ti, coarse, dt, cfg.coarse_gate_m, &pairs);
        const double n2 = dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2];
        const bool better =
            !have_best || pairs > best_pairs ||
            (pairs == best_pairs &&
             (mean < best_mean - 1e-9 ||
              (std::fabs(mean - best_mean) <= 1e-9 && n2 < best_norm2 - 1e-12)));
        if (better) {
          have_best = true;
          best_pairs = pairs;
          best_mean = mean;
          best_norm2 = n2;
          best_dt[0] = dt[0];
          best_dt[1] = dt[1];
          best_dt[2] = dt[2];
        }
      }
    }
  }
  rep.coarse_dt[0] = best_dt[0];
  rep.coarse_dt[1] = best_dt[1];
  rep.coarse_dt[2] = best_dt[2];
  rep.coarse_overlap =
      coarse.empty() ? 0.0 : static_cast<double>(best_pairs) / static_cast<double>(coarse.size());
  if (rep.coarse_overlap < cfg.min_coarse_overlap) {
    return refuse(rep, GapRescueDecision::kNoOverlap,
                  "the two sides of the gap do not paint any shared surface within the search "
                  "radius — there is nothing to register the frames with, and a transform "
                  "fitted to nothing would be a guess");
  }

  // --- stage 2: the refinement ----------------------------------------------
  double dt[3] = {best_dt[0], best_dt[1], best_dt[2]};
  const SolveOutcome outcome = solve_translation_subspace(
      src, tgt, cfg, dt, &rep.observability, rep.weak_axis, &rep.solved_axes, &rep.pairs,
      &rep.iterations);
  rep.dt[0] = dt[0];
  rep.dt[1] = dt[1];
  rep.dt[2] = dt[2];
  rep.translation_m = std::sqrt(dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2]);
  switch (outcome) {
    case SolveOutcome::kThin:
      return refuse(rep, GapRescueDecision::kThinSubmap,
                    "too few plane correspondences between the two sides to solve a "
                    "translation");
    case SolveOutcome::kUnobservable:
      return refuse(rep, GapRescueDecision::kUnobservable,
                    "the shared surfaces face too nearly one way to measure the translation "
                    "in more than one direction — the missing components would be invented, "
                    "so the rescue is refused (the weak axis is reported)");
    case SolveOutcome::kNotConverged:
      return refuse(rep, GapRescueDecision::kNotConverged,
                    "the translation solve did not settle — the two sides are not rigidly "
                    "the same room within reach of the search");
    case SolveOutcome::kOk:
      break;
  }

  if (rep.translation_m > cfg.max_rescue_translation_m) {
    return refuse(rep, GapRescueDecision::kCorrectionTooBig,
                  "the solved translation is beyond anything the blind walk can explain — "
                  "treating the two sides as different places rather than sliding them "
                  "together");
  }

  // The full candidate correction: T = Trans(dt) * T0.
  double T[16];
  for (int i = 0; i < 16; ++i) T[i] = t0[i];
  T[3] += dt[0];
  T[7] += dt[1];
  T[11] += dt[2];

  // --- gate 5: the two sides must agree better than the refused state -------
  {
    std::vector<PointVertex> moved = src_raw;
    for (PointVertex& v : moved) {
      const double in[3] = {v.x, v.y, v.z};
      double o[3];
      se3::mat4_apply(T, in, o);
      v.x = static_cast<float>(o[0]);
      v.y = static_cast<float>(o[1]);
      v.z = static_cast<float>(o[2]);
    }
    rep.mismatch_rescued_m = mean_nn(ti, moved, zero3, cfg.max_correspondence_m, &rep.mismatch_pairs);
    if (rep.mismatch_pairs < cfg.min_pairs) {
      return refuse(rep, GapRescueDecision::kNoImprovement,
                    "after the rescue the two sides still share too little surface to call "
                    "them registered");
    }
    // Only when the refused state had enough pairs to BE a state: a 106
    // degree fold pairs almost nothing, and "better than nothing measurable"
    // is not a comparison — the ruler below is the verdict then.
    if (rep.mismatch_identity_pairs >= cfg.min_pairs &&
        !(rep.mismatch_identity_m - rep.mismatch_rescued_m > cfg.min_mismatch_improvement_m)) {
      return refuse(rep, GapRescueDecision::kNoImprovement,
                    "the rescue did not bring the two sides of the gap closer than leaving "
                    "the frame alone");
    }
  }

  // --- the ruler votes last --------------------------------------------------
  if (cfg.require_self_consistency) {
    std::vector<PointVertex> as_is(cloud.begin(), cloud.end());
    const std::vector<std::int64_t> times(point_times.begin(), point_times.end());
    const MapConsistencyReport before = measure_map_consistency(as_is, times, cfg.consistency);
    if (before.measurable) {
      std::vector<PointVertex> after_cloud = as_is;
      for (std::size_t i = 0; i < after_cloud.size(); ++i) {
        if (times[i] >= pa.t_ns) continue;  // at the seam belongs to the later frame
        const double in[3] = {after_cloud[i].x, after_cloud[i].y, after_cloud[i].z};
        double o[3];
        se3::mat4_apply(T, in, o);
        after_cloud[i].x = static_cast<float>(o[0]);
        after_cloud[i].y = static_cast<float>(o[1]);
        after_cloud[i].z = static_cast<float>(o[2]);
      }
      const MapConsistencyReport after = measure_map_consistency(after_cloud, times, cfg.consistency);
      rep.self_check_checked = true;
      rep.self_check_before_m = before.nearest_offset_m;
      rep.self_check_after_m = after.measurable ? after.nearest_offset_m : before.nearest_offset_m;
      if (!after.measurable ||
          after.nearest_offset_m >
              before.nearest_offset_m + cfg.self_consistency_tolerance_m) {
        return refuse(rep, GapRescueDecision::kRulerSaysWorse,
                      "the rescue made the whole map agree with itself LESS — the number the "
                      "summary card prints would get worse, so the seam stays. Same seventh-"
                      "gate doctrine as the loop-end closure, same reason: a registration "
                      "cannot referee itself");
      }
    }
  }

  rep.decision = GapRescueDecision::kRescued;
  rep.reason = "rescued: rotation locked to the gyro's witness of the blind window, "
               "translation measured from the walls the two sides share";
  for (int i = 0; i < 16; ++i) rep.correction[i] = T[i];
  SCAN_LOG_INFO(kMod,
                "gap of %.3f s rescued: gyro %.2f deg locked, translation %.3f m "
                "(coarse overlap %.2f, observability %.4f, %d axes, %zu pairs), sides "
                "%.1f -> %.1f cm, self-check %.2f -> %.2f cm",
                rep.gap_s, rep.gyro_rotation_deg, rep.translation_m, rep.coarse_overlap,
                rep.observability, rep.solved_axes, rep.mismatch_pairs,
                rep.mismatch_identity_m * 100.0, rep.mismatch_rescued_m * 100.0,
                rep.self_check_before_m * 100.0, rep.self_check_after_m * 100.0);
  return rep;
}

}  // namespace post
}  // namespace scanengine
