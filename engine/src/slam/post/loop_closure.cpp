// loop_closure.cpp — point-to-plane ICP and the loop acceptance gate (A7).
#include "scanengine/slam/post/loop_closure.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "scanengine/poses/se3.h"

#include "point_grid.h"

namespace scanengine {
namespace post {
namespace {

// --- 3x3 symmetric eigen, cyclic Jacobi ------------------------------------
//
// A fixed sweep order (0,1), (0,2), (1,2), a fixed sweep count, and no
// pivoting on magnitude: identical input gives an identical eigenvector on
// every one of the five CI legs. Returns eigenvalues ascending in `w` and the
// matching eigenvectors as the COLUMNS of `v` (row-major 3x3).
void sym_eigen3(const double a_in[9], double w[3], double v[9]) {
  double a[9];
  for (int i = 0; i < 9; ++i) a[i] = a_in[i];
  for (int i = 0; i < 9; ++i) v[i] = 0.0;
  v[0] = v[4] = v[8] = 1.0;

  for (int sweep = 0; sweep < 12; ++sweep) {
    double off = 0.0;
    off += a[1] * a[1] + a[2] * a[2] + a[5] * a[5];
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
      const double s = t * c;
      // A <- J^T A J
      for (int i = 0; i < 3; ++i) {
        const double aip = a[i * 3 + p], aiq = a[i * 3 + q];
        a[i * 3 + p] = c * aip - s * aiq;
        a[i * 3 + q] = s * aip + c * aiq;
      }
      for (int j = 0; j < 3; ++j) {
        const double apj = a[p * 3 + j], aqj = a[q * 3 + j];
        a[p * 3 + j] = c * apj - s * aqj;
        a[q * 3 + j] = s * apj + c * aqj;
      }
      for (int i = 0; i < 3; ++i) {
        const double vip = v[i * 3 + p], viq = v[i * 3 + q];
        v[i * 3 + p] = c * vip - s * viq;
        v[i * 3 + q] = s * vip + c * viq;
      }
    }
  }
  w[0] = a[0];
  w[1] = a[4];
  w[2] = a[8];
  // Sort ascending; swap the matching eigenvector columns. Three elements, so
  // an explicit bubble is both the shortest and the most obviously stable.
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2 - i; ++j) {
      if (w[j] > w[j + 1]) {
        std::swap(w[j], w[j + 1]);
        for (int r = 0; r < 3; ++r) std::swap(v[r * 3 + j], v[r * 3 + j + 1]);
      }
    }
  }
}

// Symmetric 6x6 solve by LDL^T. Same argument as A6's 18x18 (docs/A6-lio.md
// §3.6): the matrix is a well-conditioned information matrix, unpivoted LDL^T
// is the right algorithm, and pivoting would cost determinism for nothing.
bool solve6(const double a_in[36], const double b[6], double x[6]) {
  double m[36];
  for (int i = 0; i < 36; ++i) m[i] = a_in[i];
  double d[6];
  for (int i = 0; i < 6; ++i) {
    for (int j = 0; j < i; ++j) {
      double s = m[i * 6 + j];
      for (int k = 0; k < j; ++k) s -= m[i * 6 + k] * d[k] * m[j * 6 + k];
      m[i * 6 + j] = s / d[j];
    }
    double s = m[i * 6 + i];
    for (int k = 0; k < i; ++k) s -= m[i * 6 + k] * m[i * 6 + k] * d[k];
    if (!(s > 1e-14) || !std::isfinite(s)) return false;
    d[i] = s;
  }
  double y[6];
  for (int i = 0; i < 6; ++i) {
    double s = b[i];
    for (int k = 0; k < i; ++k) s -= m[i * 6 + k] * y[k];
    y[i] = s;
  }
  for (int i = 0; i < 6; ++i) y[i] /= d[i];
  for (int ii = 6; ii-- > 0;) {
    double s = y[ii];
    for (int k = ii + 1; k < 6; ++k) s -= m[k * 6 + ii] * x[k];
    x[ii] = s;
  }
  return true;
}

}  // namespace

IcpResult icp_point_to_plane(Span<const PointVertex> source, Span<const PointVertex> target,
                             const double init_q[4], const double init_p[3], const IcpConfig& cfg,
                             CancelToken* cancel) {
  IcpResult res;
  for (int i = 0; i < 4; ++i) res.q[i] = init_q[i];
  for (int i = 0; i < 3; ++i) res.p[i] = init_p[i];
  se3::quat_normalize(res.q);
  for (int i = 0; i < 4; ++i) res.init_q[i] = res.q[i];
  for (int i = 0; i < 3; ++i) res.init_p[i] = res.p[i];
  if (source.size() == 0 || target.size() == 0) return res;

  const std::uint32_t kfit = cfg.plane_points < 3 ? 3u : cfg.plane_points;
  const std::size_t kmax = kfit;

  detail::PointIndex index;
  index.build(&target[0].x, sizeof(PointVertex) / sizeof(float), target.size(),
              cfg.target_cell_m > 0.0 ? cfg.target_cell_m : cfg.max_correspondence_m);

  std::vector<std::uint32_t> nidx(kmax);
  std::vector<double> nd2(kmax);

  for (std::uint32_t it = 0; it < cfg.max_iterations; ++it) {
    if (cancelled(cancel)) return res;
    double R[9];
    se3::quat_to_matrix(res.q, R);

    double H[36] = {0.0};
    double g[6] = {0.0};
    double sum_r2 = 0.0, sum_abs = 0.0;
    std::uint64_t used = 0;

    for (std::size_t si = 0; si < source.size(); ++si) {
      if ((si & 0xFFFFu) == 0 && cancelled(cancel)) return res;
      const double ps[3] = {source[si].x, source[si].y, source[si].z};
      const double pw[3] = {
          R[0] * ps[0] + R[1] * ps[1] + R[2] * ps[2] + res.p[0],
          R[3] * ps[0] + R[4] * ps[1] + R[5] * ps[2] + res.p[1],
          R[6] * ps[0] + R[7] * ps[1] + R[8] * ps[2] + res.p[2]};
      const std::size_t n =
          index.knn(pw, kmax, cfg.max_correspondence_m, nidx.data(), nd2.data());
      if (n < kfit) continue;

      // Plane by covariance eigen-decomposition about the centroid. NOT the
      // n.p + 1 = 0 least squares: that form is singular for any plane through
      // the world origin, and the world origin is the scanner's own starting
      // pose (A6 §3.5).
      double c[3] = {0.0, 0.0, 0.0};
      for (std::size_t j = 0; j < n; ++j) {
        const PointVertex& t = target[nidx[j]];
        c[0] += t.x;
        c[1] += t.y;
        c[2] += t.z;
      }
      const double inv_n = 1.0 / static_cast<double>(n);
      c[0] *= inv_n;
      c[1] *= inv_n;
      c[2] *= inv_n;
      double cov[9] = {0.0};
      for (std::size_t j = 0; j < n; ++j) {
        const PointVertex& t = target[nidx[j]];
        const double d[3] = {t.x - c[0], t.y - c[1], t.z - c[2]};
        for (int a = 0; a < 3; ++a) {
          for (int b = 0; b < 3; ++b) cov[a * 3 + b] += d[a] * d[b];
        }
      }
      for (int i = 0; i < 9; ++i) cov[i] *= inv_n;
      double w[3], v[9];
      sym_eigen3(cov, w, v);
      if (w[1] <= 0.0 || w[0] > cfg.max_planarity_ratio * w[1]) continue;
      double nrm[3] = {v[0], v[3], v[6]};  // column 0 = smallest eigenvalue
      const double nn = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
      if (!(nn > 0.0)) continue;
      nrm[0] /= nn;
      nrm[1] /= nn;
      nrm[2] /= nn;
      const double dplane = -(nrm[0] * c[0] + nrm[1] * c[1] + nrm[2] * c[2]);

      bool thick = false;
      for (std::size_t j = 0; j < n; ++j) {
        const PointVertex& t = target[nidx[j]];
        const double e = nrm[0] * t.x + nrm[1] * t.y + nrm[2] * t.z + dplane;
        if (std::fabs(e) > cfg.plane_thickness_m) {
          thick = true;
          break;
        }
      }
      if (thick) continue;

      const double r = nrm[0] * pw[0] + nrm[1] * pw[1] + nrm[2] * pw[2] + dplane;

      // J = [ -((R^T n) x ps)^T | (R^T n)^T ]  -- see A6 §3.4 on why the
      // rotation half is (R^T n) x ps and not n x (R ps).
      const double rn[3] = {R[0] * nrm[0] + R[3] * nrm[1] + R[6] * nrm[2],
                            R[1] * nrm[0] + R[4] * nrm[1] + R[7] * nrm[2],
                            R[2] * nrm[0] + R[5] * nrm[1] + R[8] * nrm[2]};
      const double cr[3] = {rn[1] * ps[2] - rn[2] * ps[1], rn[2] * ps[0] - rn[0] * ps[2],
                            rn[0] * ps[1] - rn[1] * ps[0]};
      const double J[6] = {-cr[0], -cr[1], -cr[2], rn[0], rn[1], rn[2]};

      double weight = 1.0;
      if (cfg.huber_m > 0.0) {
        const double ar = std::fabs(r);
        if (ar > cfg.huber_m) weight = cfg.huber_m / ar;
      }
      for (int a = 0; a < 6; ++a) {
        for (int b = 0; b <= a; ++b) H[a * 6 + b] += weight * J[a] * J[b];
        g[a] += weight * J[a] * r;
      }
      sum_r2 += r * r;
      sum_abs += std::fabs(r);
      ++used;
    }

    if (used < 6) {
      res.inliers = used;
      res.inlier_ratio = source.size() == 0
                             ? 0.0
                             : static_cast<double>(used) / static_cast<double>(source.size());
      res.rms_m = used == 0 ? 0.0 : std::sqrt(sum_r2 / static_cast<double>(used));
      res.fitness_m = used == 0 ? 0.0 : sum_abs / static_cast<double>(used);
      return res;
    }
    for (int a = 0; a < 6; ++a) {
      for (int b = a + 1; b < 6; ++b) H[a * 6 + b] = H[b * 6 + a];
    }
    // A small Levenberg floor. Not tuning: a revisit whose overlap is one
    // planar wall is rank-deficient in two directions by construction, and
    // without this the solve either fails or throws the estimate to infinity.
    for (int a = 0; a < 6; ++a) H[a * 6 + a] += 1e-9 * static_cast<double>(used);

    double rhs[6];
    for (int a = 0; a < 6; ++a) rhs[a] = -g[a];
    double delta[6];
    if (!solve6(H, rhs, delta)) break;

    bool finite = true;
    for (int a = 0; a < 6; ++a) {
      if (!std::isfinite(delta[a])) finite = false;
    }
    if (!finite) break;

    // Retract: R <- R Exp(dtheta), p <- p + R dp — the same convention as
    // pose_graph.cpp, so an ICP result drops straight into a BetweenFactor.
    const double dp[3] = {delta[3], delta[4], delta[5]};
    res.p[0] += R[0] * dp[0] + R[1] * dp[1] + R[2] * dp[2];
    res.p[1] += R[3] * dp[0] + R[4] * dp[1] + R[5] * dp[2];
    res.p[2] += R[6] * dp[0] + R[7] * dp[1] + R[8] * dp[2];
    const double dth[3] = {delta[0], delta[1], delta[2]};
    double E[9], Rn[9];
    se3::so3_exp(dth, E);
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) {
        double s = 0.0;
        for (int k = 0; k < 3; ++k) s += R[a * 3 + k] * E[k * 3 + b];
        Rn[a * 3 + b] = s;
      }
    }
    se3::matrix_to_quat(Rn, res.q);

    ++res.iterations;
    res.inliers = used;
    res.inlier_ratio = static_cast<double>(used) / static_cast<double>(source.size());
    res.rms_m = std::sqrt(sum_r2 / static_cast<double>(used));
    res.fitness_m = sum_abs / static_cast<double>(used);

    const double rot_step = std::sqrt(dth[0] * dth[0] + dth[1] * dth[1] + dth[2] * dth[2]);
    const double trans_step = std::sqrt(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]);
    if (rot_step < cfg.converge_rot_rad && trans_step < cfg.converge_trans_m) {
      res.converged = true;
      break;
    }
  }
  return res;
}

bool loop_is_acceptable(const IcpResult& r, const LoopAcceptConfig& cfg, const char** reason) {
  const char* why = nullptr;
  const double dt[3] = {r.p[0] - r.init_p[0], r.p[1] - r.init_p[1], r.p[2] - r.init_p[2]};
  const double t = std::sqrt(dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2]);
  double R[9], R0[9];
  se3::quat_to_matrix(r.q, R);
  se3::quat_to_matrix(r.init_q, R0);
  const double rot_deg = se3::rot_angle_deg(R, R0);

  if (!r.converged) {
    why = "icp did not converge";
  } else if (r.inliers < cfg.min_inliers) {
    why = "too few inliers";
  } else if (r.inlier_ratio < cfg.min_inlier_ratio) {
    why = "inlier ratio below threshold";
  } else if (r.rms_m > cfg.max_rms_m) {
    why = "residual rms above threshold";
  } else if (t > cfg.max_translation_m) {
    why = "translation correction implausible";
  } else if (rot_deg > cfg.max_rotation_deg) {
    why = "rotation correction implausible";
  }
  if (reason != nullptr) *reason = why == nullptr ? "accepted" : why;
  return why == nullptr;
}

}  // namespace post
}  // namespace scanengine
