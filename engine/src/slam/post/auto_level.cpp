// auto_level.cpp — ROUND 20, item 80. See auto_level.h for the contract; this
// file is the measurement and the exact per-point application.

#include "scanengine/slam/post/auto_level.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

#include "post_geom.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "auto-level";

// Deterministic xorshift64* — a fixed seed and integer arithmetic, so the
// "random" plane hypotheses are the same sequence on every platform. This is
// a SEARCH heuristic, not a statistical claim; determinism outranks entropy.
struct Xorshift {
  std::uint64_t s = 0x9E3779B97F4A7C15ull;
  std::uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

// p-th percentile of `values` by nth_element on a copy (deterministic).
double percentile(std::vector<float> values, double p) {
  if (values.empty()) return 0.0;
  const std::size_t k =
      static_cast<std::size_t>(p * static_cast<double>(values.size() - 1) + 0.5);
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
  return static_cast<double>(values[k]);
}

struct FloorFit {
  bool found = false;
  double normal[3] = {0.0, 1.0, 0.0};  // unit, +Y hemisphere
  std::size_t inliers = 0;
  double coverage_m2 = 0.0;
  double tilt_deg = 0.0;
};

// The floor meter: low-band candidates, deterministic RANSAC for the dominant
// upward-facing plane, then a fixed-sweep eigen refinement over its inliers.
FloorFit fit_floor(const std::vector<PointVertex>& pts, const AutoLevelConfig& cfg) {
  FloorFit fit;
  if (pts.size() < 3) return fit;

  std::vector<float> ys;
  ys.reserve(pts.size());
  for (const PointVertex& p : pts) ys.push_back(p.y);
  const double y_lo = percentile(ys, 0.01);
  const double y_hi = percentile(ys, 0.99);
  const double band_top = y_lo + cfg.band_fraction * (y_hi - y_lo);

  std::vector<std::size_t> band;
  band.reserve(pts.size() / 4);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const double y = pts[i].y;
    if (y > y_lo - 0.05 && y < band_top) band.push_back(i);
  }
  if (band.size() < 500) return fit;

  // RANSAC over the band. A hypothesis only counts as a FLOOR when its normal
  // is within 45 degrees of +Y — a wall's plane can dominate the low band of a
  // corridor and must not be "leveled".
  Xorshift rng;
  double best_n[3] = {0, 1, 0};
  double best_d = 0.0;
  std::size_t best_count = 0;
  const double tol = cfg.plane_tolerance_m;
  for (std::size_t it = 0; it < cfg.plane_iterations; ++it) {
    const std::size_t ia = band[rng.below(band.size())];
    const std::size_t ib = band[rng.below(band.size())];
    const std::size_t ic = band[rng.below(band.size())];
    if (ia == ib || ib == ic || ia == ic) continue;
    const PointVertex& a = pts[ia];
    const PointVertex& b = pts[ib];
    const PointVertex& c = pts[ic];
    const double ab[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
    const double ac[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
    double n[3];
    se3::cross3(ab, ac, n);
    if (!se3::normalize3(n)) continue;
    if (n[1] < 0.0) {
      n[0] = -n[0];
      n[1] = -n[1];
      n[2] = -n[2];
    }
    if (n[1] < 0.70710678) continue;  // > 45 deg from +Y: not a floor hypothesis
    const double d = -(n[0] * a.x + n[1] * a.y + n[2] * a.z);
    std::size_t count = 0;
    for (const std::size_t idx : band) {
      const PointVertex& p = pts[idx];
      const double dist = std::fabs(n[0] * p.x + n[1] * p.y + n[2] * p.z + d);
      if (dist < tol) ++count;
    }
    if (count > best_count) {
      best_count = count;
      best_n[0] = n[0];
      best_n[1] = n[1];
      best_n[2] = n[2];
      best_d = d;
    }
  }
  if (best_count < 3) return fit;

  // Refine on the inliers: centroid + scatter, smallest eigenvector — the
  // same fixed-sweep Jacobi every other post module fits planes with. Also
  // collect the horizontal coverage while walking the inliers.
  double cen[3] = {0, 0, 0};
  std::vector<std::size_t> inliers;
  inliers.reserve(best_count);
  for (const std::size_t idx : band) {
    const PointVertex& p = pts[idx];
    const double dist = std::fabs(best_n[0] * p.x + best_n[1] * p.y + best_n[2] * p.z + best_d);
    if (dist < tol) {
      inliers.push_back(idx);
      cen[0] += p.x;
      cen[1] += p.y;
      cen[2] += p.z;
    }
  }
  const double inv = 1.0 / static_cast<double>(inliers.size());
  cen[0] *= inv;
  cen[1] *= inv;
  cen[2] *= inv;
  double scatter[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<std::int64_t> cells;
  cells.reserve(inliers.size());
  constexpr double kCoverageCell = 0.25;
  for (const std::size_t idx : inliers) {
    const PointVertex& p = pts[idx];
    const double dx = p.x - cen[0], dy = p.y - cen[1], dz = p.z - cen[2];
    scatter[0] += dx * dx;
    scatter[1] += dx * dy;
    scatter[2] += dx * dz;
    scatter[4] += dy * dy;
    scatter[5] += dy * dz;
    scatter[8] += dz * dz;
    const std::int64_t cx = static_cast<std::int64_t>(std::floor(p.x / kCoverageCell));
    const std::int64_t cz = static_cast<std::int64_t>(std::floor(p.z / kCoverageCell));
    cells.push_back((cx << 32) ^ (cz & 0xFFFFFFFFll));
  }
  scatter[3] = scatter[1];
  scatter[6] = scatter[2];
  scatter[7] = scatter[5];
  double w[3], v[9];
  detail::sym_eigen3(scatter, w, v);
  double n2[3] = {v[0], v[3], v[6]};  // smallest eigenvalue's column
  if (!se3::normalize3(n2)) return fit;
  if (n2[1] < 0.0) {
    n2[0] = -n2[0];
    n2[1] = -n2[1];
    n2[2] = -n2[2];
  }
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());

  fit.found = true;
  fit.normal[0] = n2[0];
  fit.normal[1] = n2[1];
  fit.normal[2] = n2[2];
  fit.inliers = inliers.size();
  fit.coverage_m2 = static_cast<double>(cells.size()) * kCoverageCell * kCoverageCell;
  fit.tilt_deg = std::acos(std::min(1.0, std::fabs(n2[1]))) * se3::kRadToDeg;
  return fit;
}

// Chordal mean of the TRACKED pose orientations, sign-aligned to the first —
// the attitude the phone-frame conjugation is taken at.
bool mean_attitude(const std::vector<TrajPose>& poses, double out[4]) {
  double acc[4] = {0, 0, 0, 0};
  const double* first = nullptr;
  for (const TrajPose& p : poses) {
    if (p.tracking_lost != 0 || p.quality == 0) continue;
    if (first == nullptr) first = p.q;
    const double dot =
        p.q[0] * first[0] + p.q[1] * first[1] + p.q[2] * first[2] + p.q[3] * first[3];
    const double s = dot < 0.0 ? -1.0 : 1.0;
    for (int i = 0; i < 4; ++i) acc[i] += s * p.q[i];
  }
  if (first == nullptr) return false;
  double n = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] + acc[3] * acc[3]);
  if (n < 1e-12) return false;
  for (int i = 0; i < 4; ++i) out[i] = acc[i] / n;
  return true;
}

// World attitude of the phone at time `t_ns`, slerped between the bracketing
// poses (clamped at the ends). `poses` is chronological — the resolve emits
// it that way.
void attitude_at(const std::vector<TrajPose>& poses, std::int64_t t_ns, double out[4]) {
  if (poses.empty()) {
    se3::quat_identity(out);
    return;
  }
  if (t_ns <= poses.front().t_ns) {
    std::memcpy(out, poses.front().q, sizeof(double) * 4);
    return;
  }
  if (t_ns >= poses.back().t_ns) {
    std::memcpy(out, poses.back().q, sizeof(double) * 4);
    return;
  }
  std::size_t lo = 0, hi = poses.size() - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (poses[mid].t_ns <= t_ns) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const TrajPose& a = poses[lo];
  const TrajPose& b = poses[hi];
  const double span = static_cast<double>(b.t_ns - a.t_ns);
  const double u = span > 0.0 ? static_cast<double>(t_ns - a.t_ns) / span : 0.0;
  se3::quat_slerp(a.q, b.q, u, out);
}

// Interpolated phone position at `t_ns` (for the pivot of the per-point map).
void position_at(const std::vector<TrajPose>& poses, std::int64_t t_ns, double out[3]) {
  if (poses.empty()) {
    out[0] = out[1] = out[2] = 0.0;
    return;
  }
  if (t_ns <= poses.front().t_ns) {
    std::memcpy(out, poses.front().p, sizeof(double) * 3);
    return;
  }
  if (t_ns >= poses.back().t_ns) {
    std::memcpy(out, poses.back().p, sizeof(double) * 3);
    return;
  }
  std::size_t lo = 0, hi = poses.size() - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (poses[mid].t_ns <= t_ns) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const TrajPose& a = poses[lo];
  const TrajPose& b = poses[hi];
  const double span = static_cast<double>(b.t_ns - a.t_ns);
  const double u = span > 0.0 ? static_cast<double>(t_ns - a.t_ns) / span : 0.0;
  for (int i = 0; i < 3; ++i) out[i] = a.p[i] + u * (b.p[i] - a.p[i]);
}

void quat_rotate_v(const double q[4], const double v[3], double out[3]) {
  // q v q* via the two-cross form.
  const double u[3] = {q[0], q[1], q[2]};
  const double s = q[3];
  double uv[3], uuv[3];
  se3::cross3(u, v, uv);
  se3::cross3(u, uv, uuv);
  for (int i = 0; i < 3; ++i) out[i] = v[i] + 2.0 * (s * uv[i] + uuv[i]);
}

// Apply the phone-frame correction `dq_p` to every point:
//   p' = t(t) + R(t) dq_p R(t)^T (p - t(t))
void apply_phone_correction(std::vector<PointVertex>& pts,
                            const std::vector<std::int64_t>& times,
                            const std::vector<TrajPose>& poses, const double dq_p[4]) {
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const std::int64_t t = i < times.size() ? times[i] : 0;
    double q[4], pos[3];
    attitude_at(poses, t, q);
    position_at(poses, t, pos);
    double qc[4], eff[4], tmp[4];
    se3::quat_conj(q, qc);
    se3::quat_mul(q, dq_p, tmp);
    se3::quat_mul(tmp, qc, eff);  // world-frame effective rotation at this pose
    const double rel[3] = {pts[i].x - pos[0], pts[i].y - pos[1], pts[i].z - pos[2]};
    double out[3];
    quat_rotate_v(eff, rel, out);
    pts[i].x = static_cast<float>(out[0] + pos[0]);
    pts[i].y = static_cast<float>(out[1] + pos[1]);
    pts[i].z = static_cast<float>(out[2] + pos[2]);
  }
}

AutoLevelReport refuse(AutoLevelReport rep, AutoLevelDecision d, const char* why) {
  rep.decision = d;
  rep.reason = why;
  SCAN_LOG_INFO(kMod, "refused (%s): %s — tilt %.2f deg, floor %zu inliers / %.2f m2",
                to_string(d), why, rep.tilt_before_deg, rep.floor_inliers,
                rep.floor_coverage_m2);
  return rep;
}

}  // namespace

const char* to_string(AutoLevelDecision d) {
  switch (d) {
    case AutoLevelDecision::kApplied: return "applied";
    case AutoLevelDecision::kNoFloor: return "no-floor";
    case AutoLevelDecision::kThinFloor: return "thin-floor";
    case AutoLevelDecision::kAlreadyLevel: return "already-level";
    case AutoLevelDecision::kTiltTooBig: return "tilt-too-big";
    case AutoLevelDecision::kNoImprovement: return "no-improvement";
    case AutoLevelDecision::kRulerSaysWorse: return "ruler-says-worse";
    case AutoLevelDecision::kNotEnoughData: return "not-enough-data";
  }
  return "?";
}

AutoLevelReport measure_floor_tilt(const std::vector<PointVertex>& points,
                                   const AutoLevelConfig& cfg) {
  AutoLevelReport rep;
  if (points.size() < 1000) {
    rep.decision = AutoLevelDecision::kNotEnoughData;
    rep.reason = "too few points to measure a floor";
    return rep;
  }
  const FloorFit fit = fit_floor(points, cfg);
  if (!fit.found) {
    rep.decision = AutoLevelDecision::kNoFloor;
    rep.reason = "no upward-facing dominant plane in the low band";
    return rep;
  }
  rep.floor_found = true;
  rep.floor_inliers = fit.inliers;
  rep.floor_coverage_m2 = fit.coverage_m2;
  for (int i = 0; i < 3; ++i) rep.floor_normal[i] = fit.normal[i];
  rep.tilt_before_deg = fit.tilt_deg;
  rep.tilt_after_deg = fit.tilt_deg;
  if (fit.inliers < cfg.min_inliers || fit.coverage_m2 < cfg.min_coverage_m2) {
    rep.decision = AutoLevelDecision::kThinFloor;
    rep.reason = "the plane is too thin to be trusted as THE floor";
    return rep;
  }
  rep.decision = AutoLevelDecision::kAlreadyLevel;  // caller re-judges the tilt
  rep.reason = "";
  return rep;
}

AutoLevelReport auto_level_floor(std::vector<PointVertex>& points,
                                 const std::vector<std::int64_t>& point_times,
                                 const std::vector<TrajPose>& poses,
                                 const AutoLevelConfig& cfg) {
  AutoLevelReport rep = measure_floor_tilt(points, cfg);
  if (rep.decision == AutoLevelDecision::kNotEnoughData ||
      rep.decision == AutoLevelDecision::kNoFloor ||
      rep.decision == AutoLevelDecision::kThinFloor) {
    return refuse(rep, rep.decision, rep.reason);
  }
  if (poses.empty()) {
    return refuse(rep, AutoLevelDecision::kNotEnoughData,
                  "no trajectory to conjugate the correction through");
  }
  if (rep.tilt_before_deg < cfg.min_tilt_deg) {
    return refuse(rep, AutoLevelDecision::kAlreadyLevel,
                  "the floor is already level to within the threshold");
  }
  if (rep.tilt_before_deg > cfg.max_tilt_deg) {
    return refuse(rep, AutoLevelDecision::kTiltTooBig,
                  "a tilt this large is not a residual mount-trim error — refusing to chase "
                  "what is probably not the floor");
  }

  double q_mean[4];
  if (!mean_attitude(poses, q_mean)) {
    return refuse(rep, AutoLevelDecision::kNotEnoughData, "no tracked pose to average");
  }

  // The working copy: refusals must leave the caller's points byte-identical.
  std::vector<PointVertex> work = points;
  double total_dq[4] = {0, 0, 0, 1};
  double tilt = rep.tilt_before_deg;
  double normal[3] = {rep.floor_normal[0], rep.floor_normal[1], rep.floor_normal[2]};
  int iters = 0;
  for (; iters < cfg.max_iterations && tilt > cfg.converge_deg; ++iters) {
    // World-frame leveling rotation: n -> +Y about the horizontal axis n x Y.
    // Zero about-gravity twist BY CONSTRUCTION — the floor cannot witness yaw
    // and this module never invents one.
    const double up[3] = {0.0, 1.0, 0.0};
    double axis[3];
    se3::cross3(normal, up, axis);
    if (!se3::normalize3(axis)) break;
    const double angle = std::acos(std::min(1.0, std::fabs(normal[1])));
    const double rv[3] = {axis[0] * angle, axis[1] * angle, axis[2] * angle};
    double dq_w[4];
    se3::quat_from_rotvec(rv, dq_w);
    // Conjugate into the phone frame at the mean attitude: dq_p = q̄* dq_w q̄.
    double qc[4], tmp[4], dq_p[4];
    se3::quat_conj(q_mean, qc);
    se3::quat_mul(qc, dq_w, tmp);
    se3::quat_mul(tmp, q_mean, dq_p);
    apply_phone_correction(work, point_times, poses, dq_p);
    double acc[4];
    se3::quat_mul(dq_p, total_dq, acc);  // later correction composes on the left of M
    std::memcpy(total_dq, acc, sizeof(acc));

    const FloorFit refit = fit_floor(work, cfg);
    if (!refit.found) break;
    tilt = refit.tilt_deg;
    for (int i = 0; i < 3; ++i) normal[i] = refit.normal[i];
  }
  rep.iterations = iters;
  rep.tilt_after_deg = tilt;

  if (!(tilt < rep.tilt_before_deg)) {
    return refuse(rep, AutoLevelDecision::kNoImprovement,
                  "the iteration could not reduce the measured tilt");
  }

  // The ruler votes last — gate 7 doctrine, verbatim from gap_rescue.cpp: a
  // leveling that makes the whole-map self-check worse is refused however
  // level the floor now looks.
  if (cfg.require_self_consistency) {
    MapConsistencyConfig mcfg = cfg.consistency;
    const MapConsistencyReport before = measure_map_consistency(points, point_times, mcfg);
    if (before.measurable) {
      const MapConsistencyReport after = measure_map_consistency(work, point_times, mcfg);
      rep.self_check_checked = true;
      rep.self_check_before_m = before.nearest_offset_m;
      rep.self_check_after_m = after.measurable ? after.nearest_offset_m : before.nearest_offset_m;
      if (!after.measurable ||
          after.nearest_offset_m > before.nearest_offset_m + cfg.self_consistency_tolerance_m) {
        return refuse(rep, AutoLevelDecision::kRulerSaysWorse,
                      "leveling the floor made the whole map agree with itself LESS — the "
                      "correction cannot referee itself, so the cloud stays as resolved");
      }
    }
  }

  double n_total = std::sqrt(total_dq[0] * total_dq[0] + total_dq[1] * total_dq[1] +
                             total_dq[2] * total_dq[2] + total_dq[3] * total_dq[3]);
  if (n_total > 1e-12) {
    for (int i = 0; i < 4; ++i) total_dq[i] /= n_total;
  }
  std::memcpy(rep.correction_quat, total_dq, sizeof(total_dq));
  rep.correction_deg = 2.0 * std::acos(std::min(1.0, std::fabs(total_dq[3]))) * se3::kRadToDeg;

  points.swap(work);
  rep.decision = AutoLevelDecision::kApplied;
  rep.reason = "floor leveled against gravity (pitch/roll only — a floor cannot witness yaw)";
  SCAN_LOG_INFO(kMod,
                "applied: floor tilt %.2f -> %.2f deg over %d iteration(s), correction "
                "%.2f deg (phone frame), floor %zu inliers / %.2f m2, self-check "
                "%.2f -> %.2f cm",
                rep.tilt_before_deg, rep.tilt_after_deg, rep.iterations, rep.correction_deg,
                rep.floor_inliers, rep.floor_coverage_m2, rep.self_check_before_m * 100.0,
                rep.self_check_after_m * 100.0);
  return rep;
}

}  // namespace post
}  // namespace scanengine
