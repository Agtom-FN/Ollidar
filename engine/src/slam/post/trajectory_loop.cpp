// trajectory_loop.cpp — ROUND 11 item 41. See trajectory_loop.h for why this
// is not a pose graph and for the three-gate guard; this file is the
// arithmetic and the bookkeeping.
//
// Determinism, the same three rules the rest of post/ lives by:
//   * every reduction runs in a fixed index order;
//   * every subsample is a STRIDE, never a random draw;
//   * every tie is broken by the smaller index, explicitly.
#include "scanengine/slam/post/trajectory_loop.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

#include "point_grid.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "trajloop";

// Below this the small-angle series is used instead of the closed form; at
// 1e-8 rad the two agree to well past double precision.
constexpr double kSmallAngle = 1e-8;

void skew(const double w[3], double out[9]) {
  out[0] = 0.0;    out[1] = -w[2];  out[2] = w[1];
  out[3] = w[2];   out[4] = 0.0;    out[5] = -w[0];
  out[6] = -w[1];  out[7] = w[0];   out[8] = 0.0;
}

void mat3_mul3(const double a[9], const double b[9], double out[9]) {
  double t[9];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += a[r * 3 + k] * b[k * 3 + c];
      t[r * 3 + c] = s;
    }
  }
  for (int i = 0; i < 9; ++i) out[i] = t[i];
}

void mat3_apply3(const double a[9], const double v[3], double out[3]) {
  const double x = a[0] * v[0] + a[1] * v[1] + a[2] * v[2];
  const double y = a[3] * v[0] + a[4] * v[1] + a[5] * v[2];
  const double z = a[6] * v[0] + a[7] * v[1] + a[8] * v[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

double dist3(const double a[3], const double b[3]) {
  const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

// --- SE(3) exp / log --------------------------------------------------------
//
// The standard closed forms. `xi` is (rotation[0..2], translation[3..5]) —
// rotation first, which is the convention pose_graph.h already states for its
// own 6-vectors, so the two files cannot be read the wrong way round.

void se3_log(const double m[16], double xi[6]) {
  double R[9], t[3];
  se3::mat4_get_rt(m, R, t);
  double w[3];
  se3::so3_log(R, w);
  const double theta = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);

  double Vinv[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double W[9];
  skew(w, W);
  if (theta > kSmallAngle) {
    // V^-1 = I - 1/2 [w] + (1/theta^2)(1 - theta*sin(theta)/(2(1-cos theta))) [w]^2
    double W2[9];
    mat3_mul3(W, W, W2);
    const double a = 0.5;
    const double b = (1.0 / (theta * theta)) *
                     (1.0 - (theta * std::sin(theta)) / (2.0 * (1.0 - std::cos(theta))));
    for (int i = 0; i < 9; ++i) Vinv[i] += -a * W[i] + b * W2[i];
  } else {
    for (int i = 0; i < 9; ++i) Vinv[i] += -0.5 * W[i];
  }
  double u[3];
  mat3_apply3(Vinv, t, u);
  xi[0] = w[0];
  xi[1] = w[1];
  xi[2] = w[2];
  xi[3] = u[0];
  xi[4] = u[1];
  xi[5] = u[2];
}

void se3_exp(const double xi[6], double m[16]) {
  const double w[3] = {xi[0], xi[1], xi[2]};
  const double u[3] = {xi[3], xi[4], xi[5]};
  double R[9];
  se3::so3_exp(w, R);
  const double theta = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);

  double V[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double W[9];
  skew(w, W);
  if (theta > kSmallAngle) {
    double W2[9];
    mat3_mul3(W, W, W2);
    const double a = (1.0 - std::cos(theta)) / (theta * theta);
    const double b = (theta - std::sin(theta)) / (theta * theta * theta);
    for (int i = 0; i < 9; ++i) V[i] += a * W[i] + b * W2[i];
  } else {
    for (int i = 0; i < 9; ++i) V[i] += 0.5 * W[i];
  }
  double t[3];
  mat3_apply3(V, u, t);
  se3::mat4_from_rt(R, t, m);
}

// --- TrajectoryCorrection ---------------------------------------------------

void TrajectoryCorrection::build(std::vector<std::int64_t> knot_t_ns, std::vector<double> knot_s,
                                 const double xi[6]) {
  t_ = std::move(knot_t_ns);
  s_ = std::move(knot_s);
  for (int i = 0; i < 6; ++i) xi_[i] = xi[i];
  active_ = t_.size() >= 2 && t_.size() == s_.size();
}

double TrajectoryCorrection::fraction_at(std::int64_t t_ns) const {
  if (!active_) return 0.0;
  if (t_ns <= t_.front()) return s_.front();
  if (t_ns >= t_.back()) return s_.back();
  // upper_bound, then linear between the bracketing knots. The knots are
  // strictly ascending by construction, so no zero-width interval can appear.
  const auto it = std::upper_bound(t_.begin(), t_.end(), t_ns);
  const std::size_t hi = static_cast<std::size_t>(it - t_.begin());
  const std::size_t lo = hi - 1;
  const double span = static_cast<double>(t_[hi] - t_[lo]);
  if (span <= 0.0) return s_[lo];
  const double u = static_cast<double>(t_ns - t_[lo]) / span;
  return s_[lo] + u * (s_[hi] - s_[lo]);
}

void TrajectoryCorrection::matrix_at(std::int64_t t_ns, double out[16]) const {
  if (!active_) {
    se3::mat4_identity(out);
    return;
  }
  const double s = fraction_at(t_ns);
  const double scaled[6] = {xi_[0] * s, xi_[1] * s, xi_[2] * s,
                            xi_[3] * s, xi_[4] * s, xi_[5] * s};
  se3_exp(scaled, out);
}

void TrajectoryCorrection::apply_point(std::int64_t t_ns, float xyz[3]) const {
  if (!active_) return;
  double m[16];
  matrix_at(t_ns, m);
  const double in[3] = {static_cast<double>(xyz[0]), static_cast<double>(xyz[1]),
                        static_cast<double>(xyz[2])};
  double outp[3];
  se3::mat4_apply(m, in, outp);
  xyz[0] = static_cast<float>(outp[0]);
  xyz[1] = static_cast<float>(outp[1]);
  xyz[2] = static_cast<float>(outp[2]);
}

void TrajectoryCorrection::apply_pose(std::int64_t t_ns, double q[4], double p[3]) const {
  if (!active_) return;
  double c[16];
  matrix_at(t_ns, c);
  double pose[16];
  se3::mat4_from_quat_pos(q, p, pose);
  double corrected[16];
  se3::mat4_mul(c, pose, corrected);
  double R[9], t[3];
  se3::mat4_get_rt(corrected, R, t);
  se3::matrix_to_quat(R, q);
  for (int i = 0; i < 3; ++i) p[i] = t[i];
}

// --- config -----------------------------------------------------------------

TrajectoryLoopConfig::TrajectoryLoopConfig() {
  // A7's ICP defaults are tuned for a Mid-360 keyframe against an accumulated
  // submap. Two things change for a D6 pair of submaps and both are stated
  // here rather than left as magic numbers at the call site:
  //
  //  * `max_correspondence_m` stays at A7's 1.0 m. A6 measured that
  //    tightening it makes things WORSE (docs/A6-lio.md §7.3) and the same
  //    argument applies: the gap this has to bridge IS the drift being
  //    measured, so a tight gate would refuse to see it.
  //  * `plane_thickness_m` drops from 15 cm to 8 cm. A D6 submap is one
  //    vertical fan swept over a few seconds, so its "planes" are genuinely
  //    thin wall patches; 15 cm would accept a wall and the floor in front of
  //    it as one plane and the normal would be meaningless.
  icp.plane_thickness_m = 0.08;
  // A7's 30 iterations and its 1e-5 rad / 1e-4 m convergence thresholds were
  // written for a Mid-360 keyframe against a dense accumulated submap, where
  // the residual really does go to zero. Two D6 submaps of the same room are
  // sparse, anisotropic (a fan sweeps a ring, not a volume) and carry real
  // sensor noise, so the step size floors out around 1e-4 rad and A7's
  // thresholds would report "did not converge" on a perfectly good closure.
  //
  // 1e-4 rad is 0.006 degrees and 1e-3 m is one millimetre; both are two
  // orders of magnitude below anything that changes the answer, so this loosens
  // the STOPPING rule without loosening a single ACCEPTANCE rule — the inlier,
  // RMS and magnitude gates below are untouched and they are what decides.
  icp.max_iterations = 60;
  icp.converge_rot_rad = 1e-4;
  icp.converge_trans_m = 1e-3;

  // The acceptance gate. A7's min_inliers of 150 is kept; the ratio is raised
  // because both clouds here are the SAME sensor looking at the SAME room
  // from nearly the same place, so a genuine revisit overlaps heavily. A
  // corridor mistaken for another corridor does not.
  accept.min_inlier_ratio = 0.45;
  accept.max_rms_m = 0.20;
  // How far ICP is allowed to move from its identity initialization. This is
  // the same number as max_close_translation_m and it is deliberately
  // duplicated: `loop_is_acceptable` measures the MOVEMENT and gate 4
  // measures the RESULT, and with an identity init they coincide — if that
  // ever stops being true (a future non-identity init from odometry) the two
  // must not silently become one gate.
  accept.max_translation_m = 1.5;
  accept.max_rotation_deg = 20.0;
}

const char* to_string(LoopDecision d) {
  switch (d) {
    case LoopDecision::kClosed: return "closed";
    case LoopDecision::kNoTrajectory: return "no-trajectory";
    case LoopDecision::kNoRevisit: return "no-revisit";
    case LoopDecision::kNoExcursion: return "no-excursion";
    case LoopDecision::kThinSubmap: return "thin-submap";
    case LoopDecision::kUnobservable: return "unobservable";
    case LoopDecision::kIcpFailed: return "icp-failed";
    case LoopDecision::kGeometryRejected: return "geometry-rejected";
    case LoopDecision::kCorrectionTooBig: return "correction-too-big";
    case LoopDecision::kMapGotWorse: return "map-got-worse";
  }
  return "unknown";
}

namespace {

// Points whose pose-time falls inside [t - half, t + half], strided down to
// `max_points`. The stride is computed from the FULL count so it is a pure
// function of the data, not of the order the caller happened to iterate in.
std::vector<PointVertex> submap_at(Span<const PointVertex> cloud,
                                   Span<const std::int64_t> times, std::int64_t t_ns,
                                   std::int64_t half_ns, std::size_t max_points) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < times.size(); ++i) {
    const std::int64_t dt = times[i] - t_ns;
    if (dt >= -half_ns && dt <= half_ns) ++n;
  }
  std::vector<PointVertex> out;
  if (n == 0) return out;
  const std::size_t stride = (max_points > 0 && n > max_points) ? (n / max_points + 1) : 1;
  out.reserve(n / stride + 1);
  std::size_t k = 0;
  for (std::size_t i = 0; i < times.size(); ++i) {
    const std::int64_t dt = times[i] - t_ns;
    if (dt < -half_ns || dt > half_ns) continue;
    if ((k % stride) == 0) out.push_back(cloud[i]);
    ++k;
  }
  return out;
}

// Mean nearest-neighbour distance from `source` (transformed by `m`) to
// `target`, over the pairs closer than `gate_m`. This is the honest
// before/after "how far apart was the same place" number: it looks only at
// the points, so applying the correction cannot flatter it.
double mean_nn(const detail::PointIndex& target_index, const std::vector<PointVertex>& source,
               const double m[16], double gate_m, std::size_t* out_pairs) {
  double sum = 0.0;
  std::size_t pairs = 0;
  std::uint32_t idx = 0;
  double d2 = 0.0;
  for (const PointVertex& v : source) {
    const double in[3] = {v.x, v.y, v.z};
    double q[3];
    se3::mat4_apply(m, in, q);
    if (target_index.knn(q, 1, gate_m, &idx, &d2) == 1) {
      sum += std::sqrt(d2);
      ++pairs;
    }
  }
  if (out_pairs != nullptr) *out_pairs = pairs;
  return pairs > 0 ? sum / static_cast<double>(pairs) : 0.0;
}

// --- 3x3 symmetric eigen, cyclic Jacobi ------------------------------------
//
// The same routine, with the same determinism argument, that
// loop_closure.cpp's plane fitter uses: a fixed sweep order, a fixed sweep
// count and no pivoting on magnitude, so identical input gives an identical
// eigenvector on every platform. Eigenvalues ascending in `w`, matching
// eigenvectors as the COLUMNS of `v` (row-major 3x3).
void sym_eigen3(const double a_in[9], double w[3], double v[9]) {
  double a[9];
  for (int i = 0; i < 9; ++i) a[i] = a_in[i];
  for (int i = 0; i < 9; ++i) v[i] = 0.0;
  v[0] = v[4] = v[8] = 1.0;
  for (int sweep = 0; sweep < 12; ++sweep) {
    const double off = a[1] * a[1] + a[2] * a[2] + a[5] * a[5];
    if (off < 1e-30) break;
    const int pq[3][2] = {{0, 1}, {0, 2}, {1, 2}};
    for (int k = 0; k < 3; ++k) {
      const int p = pq[k][0], q = pq[k][1];
      const double apq = a[p * 3 + q];
      if (std::fabs(apq) < 1e-300) continue;
      const double theta = (a[q * 3 + q] - a[p * 3 + p]) / (2.0 * apq);
      const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                       (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
      const double c = 1.0 / std::sqrt(t * t + 1.0);
      const double sn = t * c;
      for (int i = 0; i < 3; ++i) {
        const double aip = a[i * 3 + p], aiq = a[i * 3 + q];
        a[i * 3 + p] = c * aip - sn * aiq;
        a[i * 3 + q] = sn * aip + c * aiq;
      }
      for (int j = 0; j < 3; ++j) {
        const double apj = a[p * 3 + j], aqj = a[q * 3 + j];
        a[p * 3 + j] = c * apj - sn * aqj;
        a[q * 3 + j] = sn * apj + c * aqj;
      }
      for (int i = 0; i < 3; ++i) {
        const double vip = v[i * 3 + p], viq = v[i * 3 + q];
        v[i * 3 + p] = c * vip - sn * viq;
        v[i * 3 + q] = sn * vip + c * viq;
      }
    }
  }
  w[0] = a[0];
  w[1] = a[4];
  w[2] = a[8];
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2 - i; ++j) {
      if (w[j] > w[j + 1]) {
        std::swap(w[j], w[j + 1]);
        for (int r = 0; r < 3; ++r) std::swap(v[r * 3 + j], v[r * 3 + j + 1]);
      }
    }
  }
}

// How well the target submap's surfaces span three-dimensional space.
//
// Local plane fits over a strided sample; the normal of each accepted fit
// (one whose smallest eigenvalue is a small fraction of the next, i.e. a real
// plane and not a blob) is accumulated into `sum(n n^T)`. The returned
// coverage is lambda_min / lambda_max of that scatter and `weak` is the
// direction of lambda_min — the direction along which point-to-plane ICP has
// no information at all when the coverage is near zero.
double normal_coverage(const std::vector<PointVertex>& target, double radius,
                       std::size_t max_samples, double weak[3], std::size_t* out_fitted) {
  if (out_fitted != nullptr) *out_fitted = 0;
  weak[0] = weak[1] = weak[2] = 0.0;
  if (target.size() < 32) return 0.0;

  detail::PointIndex index;
  index.build(&target[0].x, 4, target.size(), radius);

  constexpr std::size_t kK = 12;
  std::uint32_t idx[kK];
  double d2[kK];
  double scatter[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::size_t fitted = 0;
  const std::size_t stride =
      (max_samples > 0 && target.size() > max_samples) ? (target.size() / max_samples + 1) : 1;
  for (std::size_t i = 0; i < target.size(); i += stride) {
    const double q[3] = {target[i].x, target[i].y, target[i].z};
    const std::size_t n = index.knn(q, kK, radius, idx, d2);
    if (n < 6) continue;
    double mean[3] = {0, 0, 0};
    for (std::size_t k = 0; k < n; ++k) {
      const PointVertex& p = target[idx[k]];
      mean[0] += p.x;
      mean[1] += p.y;
      mean[2] += p.z;
    }
    for (int k = 0; k < 3; ++k) mean[k] /= static_cast<double>(n);
    double cov[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t k = 0; k < n; ++k) {
      const PointVertex& p = target[idx[k]];
      const double e[3] = {p.x - mean[0], p.y - mean[1], p.z - mean[2]};
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) cov[r * 3 + c] += e[r] * e[c];
      }
    }
    double ev[3], vec[9];
    sym_eigen3(cov, ev, vec);
    // A plane: the smallest eigenvalue must be a small fraction of the middle
    // one. Otherwise the neighbourhood is a blob or a line and its "normal"
    // is noise.
    if (!(ev[1] > 1e-12) || ev[0] > 0.10 * ev[1]) continue;
    const double nrm[3] = {vec[0], vec[3], vec[6]};  // column 0 = smallest
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) scatter[r * 3 + c] += nrm[r] * nrm[c];
    }
    ++fitted;
  }
  if (out_fitted != nullptr) *out_fitted = fitted;
  if (fitted < 20) return 0.0;

  double ev[3], vec[9];
  sym_eigen3(scatter, ev, vec);
  weak[0] = vec[0];
  weak[1] = vec[3];
  weak[2] = vec[6];
  if (!(ev[2] > 1e-12)) return 0.0;
  return ev[0] / ev[2];
}

// ROUND 10's crispness metric, verbatim in method: the number of occupied
// voxels at a fixed pitch. Packed 64-bit keys and a sort rather than a hash
// map — 300k points, and the packing is exact for any room inside +/-16 km at
// 3 cm, which is every room. Deterministic by construction.
std::uint64_t voxel_key_of(const PointVertex& p, double voxel_m) {
  const std::int64_t i = static_cast<std::int64_t>(std::floor(static_cast<double>(p.x) / voxel_m));
  const std::int64_t j = static_cast<std::int64_t>(std::floor(static_cast<double>(p.y) / voxel_m));
  const std::int64_t k = static_cast<std::int64_t>(std::floor(static_cast<double>(p.z) / voxel_m));
  return (static_cast<std::uint64_t>(i + 2097152) << 42) |
         (static_cast<std::uint64_t>(j + 2097152) << 21) |
         static_cast<std::uint64_t>(k + 2097152);
}

std::uint64_t occupied_voxels(const std::vector<PointVertex>& pts, double voxel_m) {
  if (pts.empty() || voxel_m <= 0.0) return 0;
  std::vector<std::uint64_t> keys;
  keys.reserve(pts.size());
  for (const PointVertex& p : pts) keys.push_back(voxel_key_of(p, voxel_m));
  std::sort(keys.begin(), keys.end());
  std::uint64_t n = 0;
  for (std::size_t i = 0; i < keys.size();) {
    std::size_t j = i;
    while (j < keys.size() && keys[j] == keys[i]) ++j;
    ++n;
    i = j;
  }
  return n;
}

// How much of the map was painted TWICE, well apart in time — the only part
// of it a closure can merge or split, and therefore the only part where the
// occupancy comparison means anything. Returns revisited / occupied.
double revisit_overlap(const std::vector<PointVertex>& pts,
                       Span<const std::int64_t> times, double voxel_m,
                       std::int64_t apart_ns) {
  if (pts.empty() || pts.size() != times.size() || voxel_m <= 0.0) return 0.0;
  std::vector<std::pair<std::uint64_t, std::int64_t>> kv;
  kv.reserve(pts.size());
  for (std::size_t i = 0; i < pts.size(); ++i) {
    kv.emplace_back(voxel_key_of(pts[i], voxel_m), times[i]);
  }
  std::sort(kv.begin(), kv.end());
  std::uint64_t occupied = 0, revisited = 0;
  for (std::size_t i = 0; i < kv.size();) {
    std::size_t j = i;
    std::int64_t lo = kv[i].second, hi = kv[i].second;
    while (j < kv.size() && kv[j].first == kv[i].first) {
      if (kv[j].second < lo) lo = kv[j].second;
      if (kv[j].second > hi) hi = kv[j].second;
      ++j;
    }
    ++occupied;
    if (hi - lo >= apart_ns) ++revisited;
    i = j;
  }
  return occupied > 0 ? static_cast<double>(revisited) / static_cast<double>(occupied) : 0.0;
}

}  // namespace

LoopClosureReport close_trajectory_loop(const std::vector<TrajPose>& poses,
                                        Span<const PointVertex> cloud,
                                        Span<const std::int64_t> point_times,
                                        const TrajectoryLoopConfig& cfg,
                                        TrajectoryCorrection* out) {
  LoopClosureReport rep;
  if (out != nullptr) *out = TrajectoryCorrection{};

  if (poses.size() < 2 || cloud.size() != point_times.size() || cloud.empty()) {
    rep.decision = LoopDecision::kNoTrajectory;
    rep.reason = "loop: no trajectory, or the cloud and its timestamps disagree in length";
    return rep;
  }

  // --- cumulative path length, in pose order -------------------------------
  std::vector<double> cum(poses.size(), 0.0);
  for (std::size_t i = 1; i < poses.size(); ++i) {
    cum[i] = cum[i - 1] + dist3(poses[i].p, poses[i - 1].p);
  }

  // --- decimate for the O(N^2) search --------------------------------------
  const std::int64_t stride_ns =
      static_cast<std::int64_t>(cfg.candidate_stride_s * 1e9);
  std::vector<std::size_t> knot;
  knot.push_back(0);
  for (std::size_t i = 1; i < poses.size(); ++i) {
    if (stride_ns <= 0 || poses[i].t_ns - poses[knot.back()].t_ns >= stride_ns) knot.push_back(i);
  }
  if (knot.back() != poses.size() - 1) knot.push_back(poses.size() - 1);

  // --- gates 1 and 2 -------------------------------------------------------
  //
  // `a` outer, `b` ascending inner, carrying a running maximum of
  // |p_b - p_a| — which IS the excursion over [a, b], because the maximum
  // over a growing prefix is monotone. That is what keeps the whole search
  // O(N^2) instead of O(N^3).
  const double min_dt = cfg.min_loop_seconds;
  bool have = false;
  std::size_t best_a = 0, best_b = 0;
  double best_path = 0.0, best_gap = 0.0, best_exc = 0.0, best_dt = 0.0;
  bool saw_spatial = false;   // a pair passed gate 1 (distance/time/path)
  bool saw_excursion = false; // ... and gate 2

  for (std::size_t ia = 0; ia + 1 < knot.size(); ++ia) {
    const std::size_t a = knot[ia];
    double run_max = 0.0;
    for (std::size_t ib = ia + 1; ib < knot.size(); ++ib) {
      const std::size_t b = knot[ib];
      const double gap = dist3(poses[b].p, poses[a].p);
      if (gap > run_max) run_max = gap;
      const double dt = static_cast<double>(poses[b].t_ns - poses[a].t_ns) * 1e-9;
      if (dt < min_dt) continue;
      const double path = cum[b] - cum[a];
      if (path < cfg.min_loop_path_m) continue;
      if (gap > cfg.max_revisit_m) continue;
      saw_spatial = true;
      ++rep.candidates_seen;
      if (run_max < cfg.min_excursion_m) continue;
      saw_excursion = true;
      // Prefer the loop that closes the most path. Ties (which only happen on
      // synthetic data) go to the tighter gap, then to the smaller indices.
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
    rep.decision = saw_spatial ? LoopDecision::kNoExcursion : LoopDecision::kNoRevisit;
    rep.reason = saw_spatial
                     ? "loop: the trajectory revisited a place but never left it in between — "
                       "a stationary or shuffling rig, not a loop"
                     : "loop: the trajectory never returned within max_revisit_m of a place it "
                       "had already been, so there is nothing to close (this is the normal and "
                       "correct answer for a one-way walk)";
    (void)saw_excursion;
    SCAN_LOG_INFO(kMod, "no loop: %s (%llu spatial candidates)", to_string(rep.decision),
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
    rep.decision = LoopDecision::kThinSubmap;
    rep.reason = "loop: one of the two visits has too few returns to compare — refusing to "
                 "close on evidence that thin";
    SCAN_LOG_INFO(kMod, "no loop: thin submap (%zu / %zu points)", sa.size(), sb.size());
    return rep;
  }

  // --- gate 3a: is the closure observable? ---------------------------------
  //
  // Before ICP, not after: an unobservable problem does not produce a bad
  // answer that a later gate can catch by its size, it produces an answer
  // that is right in the observed directions and arbitrary in the others.
  rep.normal_coverage =
      normal_coverage(sa, cfg.normal_radius_m, cfg.normal_samples, rep.weak_axis,
                      &rep.normals_fitted);
  if (rep.normal_coverage < cfg.min_normal_coverage) {
    rep.decision = LoopDecision::kUnobservable;
    rep.reason = "loop: the two visits see no surface facing along the walk, so the closing "
                 "translation in that direction is not measured by anything — a straight "
                 "out-and-back with a pushbroom cannot close its own loop, and inventing the "
                 "missing component is exactly the fold this refuses to make";
    SCAN_LOG_INFO(kMod,
                  "no loop: normal coverage %.4f < %.4f from %zu plane fits; weak axis "
                  "(%.3f, %.3f, %.3f)",
                  rep.normal_coverage, cfg.min_normal_coverage, rep.normals_fitted,
                  rep.weak_axis[0], rep.weak_axis[1], rep.weak_axis[2]);
    return rep;
  }

  // --- gate 3: ICP ---------------------------------------------------------
  //
  // Source is the LATER visit, target the earlier one, so the result is
  // "where the later visit should have been" — which is the correction the
  // tail of the trajectory needs. Initialized at identity because both clouds
  // are already in the same world frame and the offset between them IS the
  // drift.
  const double init_q[4] = {0.0, 0.0, 0.0, 1.0};
  const double init_p[3] = {0.0, 0.0, 0.0};
  rep.icp = icp_point_to_plane(Span<const PointVertex>(sb.data(), sb.size()),
                               Span<const PointVertex>(sa.data(), sa.size()), init_q, init_p,
                               cfg.icp, nullptr);

  // The before/after mismatch, measured on the points. Built here because the
  // index is wanted whatever the verdict — a refusal is more useful when it
  // says how much was left on the table.
  detail::PointIndex ta;
  ta.build(&sa[0].x, 4, sa.size(), cfg.icp.max_correspondence_m);
  double identity[16];
  se3::mat4_identity(identity);
  rep.submap_mismatch_before_m =
      mean_nn(ta, sb, identity, cfg.icp.max_correspondence_m, &rep.mismatch_pairs);

  double fix[16];
  se3::mat4_from_quat_pos(rep.icp.q, rep.icp.p, fix);
  rep.submap_mismatch_after_m =
      mean_nn(ta, sb, fix, cfg.icp.max_correspondence_m, nullptr);

  // The measured transform, computed BEFORE any verdict so that a refusal can
  // still say how big the thing it refused was. A rejection that reports
  // zeroes is a rejection nobody can audit.
  rep.drift_translation_m =
      std::sqrt(rep.icp.p[0] * rep.icp.p[0] + rep.icp.p[1] * rep.icp.p[1] +
                rep.icp.p[2] * rep.icp.p[2]);
  {
    double w[3];
    se3::quat_to_rotvec(rep.icp.q, w);
    rep.drift_rotation_deg =
        std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * se3::kRadToDeg;
  }

  if (!rep.icp.converged) {
    rep.decision = LoopDecision::kIcpFailed;
    rep.reason = "loop: ICP did not converge on the two visits — they are not the same place, "
                 "or one of them has no usable surface";
    SCAN_LOG_INFO(kMod, "no loop: icp did not converge after %u iterations", rep.icp.iterations);
    return rep;
  }

  const char* gate_reason = nullptr;
  if (!loop_is_acceptable(rep.icp, cfg.accept, &gate_reason)) {
    rep.decision = LoopDecision::kGeometryRejected;
    rep.reason = gate_reason != nullptr ? gate_reason : "loop: the acceptance gate refused";
    SCAN_LOG_INFO(kMod,
                  "no loop: gate refused (%s) — inliers %llu (%.2f), rms %.3f m",
                  rep.reason, static_cast<unsigned long long>(rep.icp.inliers),
                  rep.icp.inlier_ratio, rep.icp.rms_m);
    return rep;
  }

  // --- gate 4: is this a plausible amount of drift? ------------------------
  if (rep.drift_translation_m > cfg.max_close_translation_m ||
      rep.drift_rotation_deg > cfg.max_close_rotation_deg) {
    rep.decision = LoopDecision::kCorrectionTooBig;
    rep.reason = "loop: the closing transform is too large to be VIO drift — treating it as a "
                 "mismatched place rather than a big correction";
    SCAN_LOG_WARN(kMod, "no loop: correction %.3f m / %.2f deg exceeds the plausible bound",
                  rep.drift_translation_m, rep.drift_rotation_deg);
    return rep;
  }

  // --- build C(t) ----------------------------------------------------------
  //
  // Knots at every pose so the arc-length parameterisation is exact rather
  // than resampled, plus the two clamps outside [a, b].
  double xi[6];
  se3_log(fix, xi);

  const double total = cum[best_b] - cum[best_a];
  std::vector<std::int64_t> kt;
  std::vector<double> ks;
  kt.reserve(poses.size());
  ks.reserve(poses.size());
  std::int64_t last_t = 0;
  bool first = true;
  for (std::size_t i = 0; i < poses.size(); ++i) {
    double s;
    if (i <= best_a) {
      s = 0.0;
    } else if (i >= best_b) {
      s = 1.0;
    } else {
      s = total > 1e-9 ? (cum[i] - cum[best_a]) / total : 0.0;
      if (s < 0.0) s = 0.0;
      if (s > 1.0) s = 1.0;
    }
    // Strictly ascending timestamps: a container with two poses at the same
    // nanosecond would otherwise make the bracket search degenerate.
    if (!first && poses[i].t_ns <= last_t) continue;
    kt.push_back(poses[i].t_ns);
    ks.push_back(s);
    last_t = poses[i].t_ns;
    first = false;
  }
  if (kt.size() < 2) {
    rep.decision = LoopDecision::kNoTrajectory;
    rep.reason = "loop: the trajectory has no two poses with distinct timestamps";
    return rep;
  }

  TrajectoryCorrection candidate;
  candidate.build(kt, ks, xi);

  // --- gate 5: did it actually make the map crisper? -----------------------
  //
  // Applied to a COPY, so a refusal here costs the caller nothing. This is
  // the only gate that looks at the whole cloud, and it is the one that
  // caught the 17-degree false closure on scan-020 that all four gates above
  // had waved through.
  if (cfg.require_global_crispness) {
    const std::vector<PointVertex> flat(cloud.begin(), cloud.end());
    rep.overlap_fraction = revisit_overlap(
        flat, point_times, cfg.crispness_voxel_m,
        static_cast<std::int64_t>(cfg.min_loop_seconds * 1e9));
    rep.occupied_voxels_before = occupied_voxels(flat, cfg.crispness_voxel_m);
    std::vector<PointVertex> moved(cloud.begin(), cloud.end());
    for (std::size_t i = 0; i < moved.size(); ++i) {
      float xyz[3] = {moved[i].x, moved[i].y, moved[i].z};
      candidate.apply_point(point_times[i], xyz);
      moved[i].x = xyz[0];
      moved[i].y = xyz[1];
      moved[i].z = xyz[2];
    }
    rep.occupied_voxels_after = occupied_voxels(moved, cfg.crispness_voxel_m);
    const double before = static_cast<double>(rep.occupied_voxels_before);
    const double after = static_cast<double>(rep.occupied_voxels_after);
    rep.crispness_checked = rep.overlap_fraction >= cfg.min_overlap_for_crispness;
    if (!rep.crispness_checked) {
      SCAN_LOG_INFO(kMod,
                    "crispness gate abstains: only %.1f %% of occupied voxels were painted "
                    "twice, so the occupancy comparison (%llu -> %llu) carries no signal",
                    100.0 * rep.overlap_fraction,
                    static_cast<unsigned long long>(rep.occupied_voxels_before),
                    static_cast<unsigned long long>(rep.occupied_voxels_after));
    }
    if (rep.crispness_checked && before > 0.0 &&
        after > before * (1.0 + cfg.crispness_tolerance)) {
      rep.decision = LoopDecision::kMapGotWorse;
      rep.reason = "loop: the closure passed every geometric gate and then made the whole map "
                   "blurrier when applied — locally right, globally a fold. Refused.";
      SCAN_LOG_WARN(kMod,
                    "no loop: occupied 3 cm voxels %llu -> %llu (%+.2f %%) — refusing a "
                    "correction of %.3f m / %.2f deg that blurs the map",
                    static_cast<unsigned long long>(rep.occupied_voxels_before),
                    static_cast<unsigned long long>(rep.occupied_voxels_after),
                    100.0 * (after - before) / before, rep.drift_translation_m,
                    rep.drift_rotation_deg);
      return rep;
    }
  }

  if (out != nullptr) *out = candidate;
  rep.poses_corrected = poses.size();
  rep.points_corrected = cloud.size();
  rep.decision = LoopDecision::kClosed;
  rep.reason = "loop: closed";

  SCAN_LOG_INFO(kMod,
                "CLOSED: visit at %.1f s revisited at %.1f s (gap %.2f m, path %.1f m, "
                "excursion %.1f m); ICP %llu inliers (%.2f), rms %.3f m; drift %.3f m / "
                "%.2f deg; same place was %.1f cm apart, now %.1f cm; occupied voxels "
                "%llu -> %llu",
                static_cast<double>(rep.t_a_ns) * 1e-9, static_cast<double>(rep.t_b_ns) * 1e-9,
                rep.revisit_gap_m, rep.loop_path_m, rep.excursion_m,
                static_cast<unsigned long long>(rep.icp.inliers), rep.icp.inlier_ratio,
                rep.icp.rms_m, rep.drift_translation_m, rep.drift_rotation_deg,
                100.0 * rep.submap_mismatch_before_m, 100.0 * rep.submap_mismatch_after_m,
                static_cast<unsigned long long>(rep.occupied_voxels_before),
                static_cast<unsigned long long>(rep.occupied_voxels_after));
  return rep;
}

}  // namespace post
}  // namespace scanengine
