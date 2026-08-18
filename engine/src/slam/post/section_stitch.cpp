// section_stitch.cpp — ROUND 13. See section_stitch.h for the derivation.

#include "scanengine/slam/post/section_stitch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

#include "point_grid.h"
#include "post_geom.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "stitch";

// ROUND 16: sym_eigen3, solve3_spd and plane_at moved to post_geom.h, shared
// with trajectory_loop.cpp and loop_end.cpp. Same code, one copy — see that
// header. `window_points` and `mean_nn` stay here: their signatures are this
// module's (a half-open [t0, t1) window rather than a centred one, and a pure
// translation rather than a 4x4), and folding them into the shared header
// would mean inventing a general shape neither caller wants.
using detail::plane_at;
using detail::solve3_spd;
using detail::sym_eigen3;

// Points of the cloud whose stamp falls in [t0, t1), decimated to at most
// `max_points`. Traversal is in point order, so the answer does not depend on
// the arrival order of anything.
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

// The whole refinement: rotation frozen, translation solved.
//
// `source` is already in the target's frame up to the unknown translation
// (i.e. the analytic transform has been applied). Returns the decision and
// writes the translation into `dt`.
SeamDecision solve_translation(const std::vector<PointVertex>& source,
                               const std::vector<PointVertex>& target,
                               const SectionStitchConfig& cfg, double dt[3], double* observability,
                               std::size_t* out_pairs) {
  dt[0] = dt[1] = dt[2] = 0.0;
  *observability = 0.0;
  *out_pairs = 0;
  if (source.size() < cfg.min_submap_points || target.size() < cfg.min_submap_points) {
    return SeamDecision::kThinSubmap;
  }

  detail::PointIndex index;
  index.build(&target[0].x, 4, target.size(), cfg.max_correspondence_m);

  double last_step = 0.0;
  bool converged = false;
  for (std::uint32_t it = 0; it < cfg.max_iterations; ++it) {
    double a[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    double b[3] = {0, 0, 0};
    std::size_t pairs = 0;
    for (const PointVertex& v : source) {
      const double s[3] = {v.x + dt[0], v.y + dt[1], v.z + dt[2]};
      double nrm[3], cen[3];
      if (!plane_at(index, target, s, cfg.plane_radius_m, cfg.max_planarity_ratio, nrm, cen)) {
        continue;
      }
      const double d = nrm[0] * (s[0] - cen[0]) + nrm[1] * (s[1] - cen[1]) + nrm[2] * (s[2] - cen[2]);
      if (std::fabs(d) > cfg.max_correspondence_m) continue;
      // Huber, the same shape loop_closure.cpp uses.
      const double ad = std::fabs(d);
      const double w = ad <= cfg.huber_m ? 1.0 : cfg.huber_m / ad;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) a[r * 3 + c] += w * nrm[r] * nrm[c];
        b[r] -= w * nrm[r] * d;
      }
      ++pairs;
    }
    *out_pairs = pairs;
    if (pairs < cfg.min_pairs) return SeamDecision::kThinSubmap;

    // The system matrix IS the observability. Normalise by the pair count so
    // the eigenvalues are a mean outer product and the ratio is scale free.
    double an[9];
    for (int i = 0; i < 9; ++i) an[i] = a[i] / static_cast<double>(pairs);
    double ev[3], vec[9];
    sym_eigen3(an, ev, vec);
    *observability = ev[2] > 1e-12 ? ev[0] / ev[2] : 0.0;
    if (*observability < cfg.min_translation_observability) return SeamDecision::kUnobservable;

    double step[3];
    if (!solve3_spd(a, b, step)) return SeamDecision::kUnobservable;
    dt[0] += step[0];
    dt[1] += step[1];
    dt[2] += step[2];
    last_step = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
    if (last_step < cfg.converge_translation_m) {
      converged = true;
      break;
    }
  }
  if (!converged) return SeamDecision::kNotConverged;

  const double mag = std::sqrt(dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2]);
  if (mag > cfg.max_refine_translation_m) return SeamDecision::kRefinementTooBig;
  return SeamDecision::kRefined;
}

}  // namespace

const char* to_string(SeamDecision d) {
  switch (d) {
    case SeamDecision::kAnalytic: return "analytic";
    case SeamDecision::kRefined: return "refined";
    case SeamDecision::kNoTrajectory: return "no-trajectory";
    case SeamDecision::kThinSubmap: return "thin-submap";
    case SeamDecision::kUnobservable: return "unobservable";
    case SeamDecision::kNotConverged: return "not-converged";
    case SeamDecision::kRefinementTooBig: return "refinement-too-big";
    case SeamDecision::kMapGotWorse: return "map-got-worse";
  }
  return "unknown";
}

// --- SectionCorrection -------------------------------------------------------

void SectionCorrection::build(std::vector<std::int64_t> seam_t_ns,
                              std::vector<double> per_section_4x4) {
  seam_t_ = std::move(seam_t_ns);
  m_ = std::move(per_section_4x4);
  active_ = m_.size() >= 32;  // more than one section
}

std::size_t SectionCorrection::section_of(std::int64_t t_ns) const {
  // A point stamped exactly at a seam belongs to the LATER section: t_ns is
  // pose_after's stamp and pose_after is the first pose of the NEW frame.
  // upper_bound (not lower_bound) is what makes that true, and getting it
  // backwards costs exactly one pose per seam — which, since each of those
  // poses is then a metre out, took scan-030's stitched trajectory from
  // 0.27 m of vertical wander to 1.56 m. Four poses.
  return static_cast<std::size_t>(
      std::upper_bound(seam_t_.begin(), seam_t_.end(), t_ns) - seam_t_.begin());
}

void SectionCorrection::matrix_at(std::int64_t t_ns, double out[16]) const {
  if (!active_) {
    se3::mat4_identity(out);
    return;
  }
  const std::size_t k = std::min(section_of(t_ns), sections() - 1);
  std::memcpy(out, &m_[k * 16], 16 * sizeof(double));
}

void SectionCorrection::apply_point(std::int64_t t_ns, float xyz[3]) const {
  if (!active_) return;
  const std::size_t k = std::min(section_of(t_ns), sections() - 1);
  const double* m = &m_[k * 16];
  const double in[3] = {xyz[0], xyz[1], xyz[2]};
  double o[3];
  se3::mat4_apply(m, in, o);
  xyz[0] = static_cast<float>(o[0]);
  xyz[1] = static_cast<float>(o[1]);
  xyz[2] = static_cast<float>(o[2]);
}

void SectionCorrection::apply_pose(std::int64_t t_ns, double q[4], double p[3]) const {
  if (!active_) return;
  const std::size_t k = std::min(section_of(t_ns), sections() - 1);
  const double* m = &m_[k * 16];
  double R[9], t[3];
  se3::mat4_get_rt(m, R, t);
  double cq[4];
  se3::matrix_to_quat(R, cq);
  double nq[4];
  se3::quat_mul(cq, q, nq);
  se3::quat_normalize(nq);
  double np[3];
  se3::mat4_apply(m, p, np);
  for (int i = 0; i < 4; ++i) q[i] = nq[i];
  for (int i = 0; i < 3; ++i) p[i] = np[i];
}

// --- the entry point ---------------------------------------------------------

SectionStitchReport stitch_sections(const std::vector<TrajPose>& poses,
                                    Span<const PointVertex> cloud,
                                    Span<const std::int64_t> point_times,
                                    const SectionStitchConfig& cfg, SectionCorrection* out) {
  SectionStitchReport rep;
  rep.poses = poses.size();
  rep.points = cloud.size();
  if (poses.size() < 2) {
    rep.summary = "no trajectory";
    return rep;
  }

  // --- 1. derive the seams from the pose stream ------------------------------
  //
  // Exactly PoseSectionTracker's rule: a step no person could take, or a turn
  // no hand could make, between two consecutive poses.
  for (std::size_t i = 1; i < poses.size(); ++i) {
    const double dt_s = static_cast<double>(poses[i].t_ns - poses[i - 1].t_ns) * 1e-9;
    if (!(dt_s > cfg.min_dt_s)) continue;
    // A pose the tracker disowned is not a world frame, so the step into or
    // out of one is not a re-anchor. The owner's scan-030 opens with 14 poses
    // at exactly the origin carrying quality 0 / tracking_lost 1; counting
    // the step out of them as a seam manufactures a 1.2 m / 10.8 deg frame
    // change and, measurably, makes the stitched map worse. The Android
    // PoseSectionTracker is fed only tracking poses, so this also keeps the
    // two detectors agreeing on the same capture.
    if (poses[i].tracking_lost != 0 || poses[i - 1].tracking_lost != 0) continue;
    if (poses[i].quality == 0 || poses[i - 1].quality == 0) continue;
    const double dx = poses[i].p[0] - poses[i - 1].p[0];
    const double dy = poses[i].p[1] - poses[i - 1].p[1];
    const double dz = poses[i].p[2] - poses[i - 1].p[2];
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    double ra[9], rb[9];
    se3::quat_to_matrix(poses[i - 1].q, ra);
    se3::quat_to_matrix(poses[i].q, rb);
    const double rot_deg = se3::rot_angle_deg(ra, rb);
    if (d / dt_s <= cfg.max_speed_mps && rot_deg / dt_s <= cfg.max_turn_rate_deg_s) continue;

    SectionSeam s;
    s.index = rep.seams.size();
    s.pose_before = i - 1;
    s.pose_after = i;
    s.t_ns = poses[i].t_ns;
    s.gap_s = dt_s;
    s.jump_translation_m = d;
    s.jump_rotation_deg = rot_deg;
    // T_k = pose_after * pose_before^-1, in the world frame.
    double ma[16], mb[16], mbi[16];
    se3::mat4_from_quat_pos(poses[i].q, poses[i].p, ma);
    se3::mat4_from_quat_pos(poses[i - 1].q, poses[i - 1].p, mb);
    se3::mat4_inverse_rigid(mb, mbi);
    se3::mat4_mul(ma, mbi, s.analytic);
    s.reason = "ARCore re-anchored; frame change taken from the pose jump";
    rep.seams.push_back(s);
  }
  rep.sections = rep.seams.size() + 1;
  if (rep.seams.empty()) {
    rep.summary = "one section — nothing to stitch";
    if (out != nullptr) *out = SectionCorrection();
    return rep;
  }

  // --- 2. compose the analytic corrections -----------------------------------
  //
  // C_N = I; C_k = C_{k+1} * T_k, so every section lands in the LAST one's
  // frame. The last section is chosen as the reference on purpose: it is the
  // most recently re-anchored, i.e. the frame ARCore currently believes, and
  // it is also the frame the operator's final position is expressed in.
  const std::size_t n_sec = rep.sections;
  std::vector<double> corr(n_sec * 16, 0.0);
  se3::mat4_identity(&corr[(n_sec - 1) * 16]);
  for (std::size_t k = n_sec - 1; k-- > 0;) {
    se3::mat4_mul(&corr[(k + 1) * 16], rep.seams[k].analytic, &corr[k * 16]);
  }

  // --- 3. refine each seam, latest first -------------------------------------
  //
  // Latest first so that when seam k is refined, sections k+1..N have their
  // final corrections already — the target it is measured against is the map
  // as it will actually be.
  if (cfg.refine && cloud.size() > 0 && cloud.size() == point_times.size()) {
    const std::int64_t half_ns = static_cast<std::int64_t>(cfg.submap_half_window_s * 1e9);
    for (std::size_t k = n_sec - 1; k-- > 0;) {
      SectionSeam& seam = rep.seams[k];
      const std::int64_t t = seam.t_ns;

      // Source: the points of section k in the window before the seam, moved
      // by C_k. Target: everything from section k+1 on, in the window after,
      // moved by its own correction.
      std::vector<PointVertex> src = window_points(cloud, point_times, t - half_ns, t,
                                                   cfg.max_submap_points);
      std::vector<PointVertex> tgt = window_points(cloud, point_times, t, t + half_ns,
                                                   cfg.max_submap_points);
      seam.submap_before_points = src.size();
      seam.submap_after_points = tgt.size();
      for (PointVertex& v : src) {
        const double in[3] = {v.x, v.y, v.z};
        double o[3];
        se3::mat4_apply(&corr[k * 16], in, o);
        v.x = static_cast<float>(o[0]);
        v.y = static_cast<float>(o[1]);
        v.z = static_cast<float>(o[2]);
      }
      for (PointVertex& v : tgt) {
        const double in[3] = {v.x, v.y, v.z};
        double o[3];
        se3::mat4_apply(&corr[(k + 1) * 16], in, o);
        v.x = static_cast<float>(o[0]);
        v.y = static_cast<float>(o[1]);
        v.z = static_cast<float>(o[2]);
      }

      double dt[3] = {0, 0, 0};
      const SeamDecision d =
          solve_translation(src, tgt, cfg, dt, &seam.observability, &seam.pairs);
      seam.decision = d;
      if (d != SeamDecision::kRefined) {
        switch (d) {
          case SeamDecision::kThinSubmap:
            seam.reason = "too few points either side of the seam to measure it";
            break;
          case SeamDecision::kUnobservable:
            seam.reason = "the surfaces here cannot measure a translation (pushbroom null space)";
            break;
          case SeamDecision::kNotConverged:
            seam.reason = "translation solve did not settle; analytic transform kept";
            break;
          case SeamDecision::kRefinementTooBig:
            seam.reason = "refinement wanted more than the bound; analytic transform kept";
            break;
          default: seam.reason = "analytic transform kept"; break;
        }
        continue;
      }

      // Earn it: the two sides must actually agree better afterwards.
      detail::PointIndex ti;
      ti.build(&tgt[0].x, 4, tgt.size(), cfg.max_correspondence_m);
      const double zero[3] = {0, 0, 0};
      std::size_t p0 = 0, p1 = 0;
      seam.mismatch_analytic_m = mean_nn(ti, src, zero, cfg.max_correspondence_m, &p0);
      seam.mismatch_refined_m = mean_nn(ti, src, dt, cfg.max_correspondence_m, &p1);
      if (!(p1 >= cfg.min_pairs) || !(seam.mismatch_refined_m < seam.mismatch_analytic_m)) {
        seam.decision = SeamDecision::kMapGotWorse;
        seam.reason = "the refinement did not bring the two sides closer; analytic kept";
        continue;
      }

      seam.refine_delta[0] = dt[0];
      seam.refine_delta[1] = dt[1];
      seam.refine_delta[2] = dt[2];
      seam.reason = "analytic frame change plus a measured translation";
      // Fold the translation into every correction at or before this seam.
      for (std::size_t j = 0; j <= k; ++j) {
        corr[j * 16 + 3] += dt[0];
        corr[j * 16 + 7] += dt[1];
        corr[j * 16 + 11] += dt[2];
      }
    }
  } else if (cfg.refine) {
    for (SectionSeam& s : rep.seams) {
      s.decision = SeamDecision::kNoTrajectory;
      s.reason = "no cloud supplied; analytic transform only";
    }
  }

  // --- 4. report -------------------------------------------------------------
  {
    double R[9], t[3];
    se3::mat4_get_rt(&corr[0], R, t);
    rep.total_translation_m = se3::norm3(t);
    double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    rep.total_rotation_deg = se3::rot_angle_deg(I, R);
  }
  std::vector<std::int64_t> seam_t;
  seam_t.reserve(rep.seams.size());
  for (const SectionSeam& s : rep.seams) seam_t.push_back(s.t_ns);

  SectionCorrection built;
  built.build(seam_t, corr);

  // The independent check: the operator walks on a flat floor, so the
  // trajectory's extent along gravity is bounded by the reach of an arm.
  // ARCore's world is gravity-aligned with +Y up.
  {
    double lo0 = 0, hi0 = 0, lo1 = 0, hi1 = 0;
    bool first = true;
    for (const TrajPose& tp : poses) {
      double q[4] = {tp.q[0], tp.q[1], tp.q[2], tp.q[3]};
      double p[3] = {tp.p[0], tp.p[1], tp.p[2]};
      const double before = p[rep.up_axis];
      built.apply_pose(tp.t_ns, q, p);
      const double after = p[rep.up_axis];
      if (first) {
        lo0 = hi0 = before;
        lo1 = hi1 = after;
        first = false;
      } else {
        lo0 = std::min(lo0, before);
        hi0 = std::max(hi0, before);
        lo1 = std::min(lo1, after);
        hi1 = std::max(hi1, after);
      }
    }
    rep.trajectory_vertical_extent_before_m = hi0 - lo0;
    rep.trajectory_vertical_extent_after_m = hi1 - lo1;

    // The end gap, measured over TRACKED poses only. Including the placeholder
    // poses a tracker emits before it has a frame would report the distance
    // from the world origin, which is not a walk.
    std::size_t first_tracked = poses.size(), last = poses.size();
    for (std::size_t i = 0; i < poses.size(); ++i) {
      if (poses[i].tracking_lost != 0 || poses[i].quality == 0) {
        ++rep.poses_untracked;
        continue;
      }
      if (first_tracked == poses.size()) first_tracked = i;
      last = i;
    }
    if (first_tracked < poses.size() && last > first_tracked) {
      const auto gap = [&](bool corrected) {
        double a[3] = {poses[first_tracked].p[0], poses[first_tracked].p[1],
                       poses[first_tracked].p[2]};
        double b[3] = {poses[last].p[0], poses[last].p[1], poses[last].p[2]};
        if (corrected) {
          double qa[4] = {0, 0, 0, 1}, qb[4] = {0, 0, 0, 1};
          built.apply_pose(poses[first_tracked].t_ns, qa, a);
          built.apply_pose(poses[last].t_ns, qb, b);
        }
        const double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
      };
      rep.trajectory_end_gap_before_m = gap(false);
      rep.trajectory_end_gap_after_m = gap(true);
    }
  }

  if (out != nullptr) *out = built;
  rep.summary = "sections stitched into the last section's frame";
  SCAN_LOG_INFO(kMod, "%zu sections, %zu seams; first section moved %.3f m / %.2f deg; "
                      "trajectory vertical extent %.2f m -> %.2f m",
                rep.sections, rep.seams.size(), rep.total_translation_m, rep.total_rotation_deg,
                rep.trajectory_vertical_extent_before_m, rep.trajectory_vertical_extent_after_m);
  return rep;
}

}  // namespace post
}  // namespace scanengine
