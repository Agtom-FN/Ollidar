// post_geom.h — ROUND 16. The small deterministic geometry primitives three
// post-processing modules now share.
//
// INTERNAL to src/slam/post/. Nothing outside this directory may include it
// (DESIGN.md §1), exactly like point_grid.h beside it.
//
// --- WHY THIS FILE EXISTS ---------------------------------------------------
//
// ROUND 11 wrote a cyclic-Jacobi 3x3 eigen solver, a strided submap cutter, a
// local-plane fitter, a normal-scatter coverage metric and ROUND 10's occupied
// voxel count into `trajectory_loop.cpp`'s anonymous namespace. ROUND 13 needed
// four of the five and copied them into `section_stitch.cpp`'s. ROUND 16 needs
// all of them again for `loop_end.cpp`, and a THIRD copy is where a shared
// routine stops being shared: the whole determinism argument of this project is
// "the same arithmetic, in the same order, every time", and that argument is
// only checkable when there is one copy of the arithmetic to check.
//
// So this is a pure MOVE, not a rewrite. Every routine below is byte-for-byte
// the code that was in those two files — the same fixed sweep count, the same
// unpivoted Jacobi, the same accumulation order — and the ROUND 11 and ROUND 13
// fixtures assert the same numbers to the same decimals afterwards, which is
// what makes the move safe to have made at all.
//
// Owner: ROUND 16.
#ifndef SCANENGINE_SRC_SLAM_POST_POST_GEOM_H
#define SCANENGINE_SRC_SLAM_POST_POST_GEOM_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/span.h"

#include "point_grid.h"

namespace scanengine {
namespace post {
namespace detail {

// --- 3x3 symmetric eigen, cyclic Jacobi -------------------------------------
//
// A fixed sweep order, a fixed sweep count, and no pivoting on magnitude, so
// identical input gives an identical eigenvector on every platform.
// Eigenvalues ascending in `w`, matching eigenvectors as the COLUMNS of `v`
// (row-major 3x3).
inline void sym_eigen3(const double a_in[9], double w[3], double v[9]) {
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
      const double t =
          (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
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

// Solve a 3x3 SPD system by Cholesky. False when it is not positive definite,
// which the observability gate should already have caught.
inline bool solve3_spd(const double a[9], const double b[3], double x[3]) {
  double l[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j <= i; ++j) {
      double s = a[i * 3 + j];
      for (int k = 0; k < j; ++k) s -= l[i * 3 + k] * l[j * 3 + k];
      if (i == j) {
        if (!(s > 1e-18)) return false;
        l[i * 3 + j] = std::sqrt(s);
      } else {
        l[i * 3 + j] = s / l[j * 3 + j];
      }
    }
  }
  double y[3];
  for (int i = 0; i < 3; ++i) {
    double s = b[i];
    for (int k = 0; k < i; ++k) s -= l[i * 3 + k] * y[k];
    y[i] = s / l[i * 3 + i];
  }
  for (int i = 2; i >= 0; --i) {
    double s = y[i];
    for (int k = i + 1; k < 3; ++k) s -= l[k * 3 + i] * x[k];
    x[i] = s / l[i * 3 + i];
  }
  return true;
}

// Points whose pose-time falls inside [t - half, t + half], strided down to
// `max_points`. The stride is computed from the FULL count so it is a pure
// function of the data, not of the order the caller happened to iterate in.
inline std::vector<PointVertex> submap_at(Span<const PointVertex> cloud,
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

// Local plane normal at `q` from the target index. False when the
// neighbourhood is a blob or a line rather than a surface.
inline bool plane_at(const PointIndex& index, const std::vector<PointVertex>& target,
                     const double q[3], double radius, double max_planarity, double n_out[3],
                     double c_out[3]) {
  constexpr std::size_t kK = 12;
  std::uint32_t idx[kK];
  double d2[kK];
  const std::size_t n = index.knn(q, kK, radius, idx, d2);
  if (n < 6) return false;
  double mean[3] = {0, 0, 0};
  for (std::size_t k = 0; k < n; ++k) {
    mean[0] += target[idx[k]].x;
    mean[1] += target[idx[k]].y;
    mean[2] += target[idx[k]].z;
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
  if (!(ev[1] > 1e-12) || ev[0] > max_planarity * ev[1]) return false;
  n_out[0] = vec[0];
  n_out[1] = vec[3];
  n_out[2] = vec[6];
  c_out[0] = mean[0];
  c_out[1] = mean[1];
  c_out[2] = mean[2];
  return true;
}

// How well the target submap's surfaces span three-dimensional space.
//
// Local plane fits over a strided sample; the normal of each accepted fit
// (one whose smallest eigenvalue is a small fraction of the next, i.e. a real
// plane and not a blob) is accumulated into `sum(n n^T)`. The returned
// coverage is lambda_min / lambda_max of that scatter and `weak` is the
// direction of lambda_min — the direction along which point-to-plane ICP has
// no information at all when the coverage is near zero.
inline double normal_coverage(const std::vector<PointVertex>& target, double radius,
                              std::size_t max_samples, double weak[3], std::size_t* out_fitted) {
  if (out_fitted != nullptr) *out_fitted = 0;
  weak[0] = weak[1] = weak[2] = 0.0;
  if (target.size() < 32) return 0.0;

  PointIndex index;
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
inline std::uint64_t voxel_key_of(const PointVertex& p, double voxel_m) {
  const std::int64_t i = static_cast<std::int64_t>(std::floor(static_cast<double>(p.x) / voxel_m));
  const std::int64_t j = static_cast<std::int64_t>(std::floor(static_cast<double>(p.y) / voxel_m));
  const std::int64_t k = static_cast<std::int64_t>(std::floor(static_cast<double>(p.z) / voxel_m));
  return (static_cast<std::uint64_t>(i + 2097152) << 42) |
         (static_cast<std::uint64_t>(j + 2097152) << 21) |
         static_cast<std::uint64_t>(k + 2097152);
}

inline std::uint64_t occupied_voxels(const std::vector<PointVertex>& pts, double voxel_m) {
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
inline double revisit_overlap(const std::vector<PointVertex>& pts, Span<const std::int64_t> times,
                              double voxel_m, std::int64_t apart_ns) {
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

}  // namespace detail
}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SRC_SLAM_POST_POST_GEOM_H
