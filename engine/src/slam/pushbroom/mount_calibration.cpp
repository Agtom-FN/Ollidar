#include "scanengine/slam/pushbroom/mount_calibration.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "mountcalib";

// ---------------------------------------------------------------------------
// A 6x6 symmetric dense linear algebra kernel.
//
// This is the entire numerical dependency of the solver. The parameter vector
// is six long — three for the rotation, three for the translation — so the
// normal equations are 6x6 no matter whether the capture has 8 poses or 45,
// and no matter whether each pose carries 20 returns or 600. That is why
// there is no Ceres here and no Eigen either: see engine/docs/A8-pushbroom.md
// §2.
// ---------------------------------------------------------------------------

// LDL^T of a symmetric matrix, solving A x = b in place. Returns false when a
// pivot is not positive — for a Levenberg–Marquardt normal matrix that means
// the damping is too small, and the caller raises lambda and retries, which
// is exactly the LM recovery path.
bool solve_sym6(const double A[36], const double b[6], double x[6]) {
  double L[36] = {0.0};
  double D[6] = {0.0};
  for (std::size_t i = 0; i < 6; ++i) {
    double s = A[i * 6 + i];
    for (std::size_t k = 0; k < i; ++k) s -= L[i * 6 + k] * L[i * 6 + k] * D[k];
    if (!(s > 1e-300) || !std::isfinite(s)) return false;
    D[i] = s;
    L[i * 6 + i] = 1.0;
    for (std::size_t j = i + 1; j < 6; ++j) {
      double t = A[j * 6 + i];
      for (std::size_t k = 0; k < i; ++k) t -= L[j * 6 + k] * L[i * 6 + k] * D[k];
      L[j * 6 + i] = t / D[i];
    }
  }
  double y[6];
  for (std::size_t i = 0; i < 6; ++i) {
    double t = b[i];
    for (std::size_t k = 0; k < i; ++k) t -= L[i * 6 + k] * y[k];
    y[i] = t;
  }
  for (std::size_t i = 0; i < 6; ++i) y[i] /= D[i];
  for (std::size_t ii = 6; ii-- > 0;) {
    double t = y[ii];
    for (std::size_t k = ii + 1; k < 6; ++k) t -= L[k * 6 + ii] * x[k];
    x[ii] = t;
  }
  return true;
}

// Inverse of a symmetric positive-definite 6x6, column by column. Only used
// for the DIAGNOSTIC covariance — never on the gate path.
bool inverse_sym6(const double A[36], double inv[36]) {
  for (std::size_t c = 0; c < 6; ++c) {
    double e[6] = {0, 0, 0, 0, 0, 0};
    e[c] = 1.0;
    double x[6];
    if (!solve_sym6(A, e, x)) return false;
    for (std::size_t r = 0; r < 6; ++r) inv[r * 6 + c] = x[r];
  }
  return true;
}

// Cyclic Jacobi eigenvalues of a symmetric 6x6 (values only). Used for the
// condition number, which is reported as a diagnostic and used to flag a
// rank-deficient capture — S6's N=3 column, where the problem is not
// determined at all.
void eigenvalues_sym6(const double A[36], double eig[6]) {
  double M[36];
  for (std::size_t i = 0; i < 36; ++i) M[i] = A[i];
  for (int sweep = 0; sweep < 60; ++sweep) {
    double off = 0.0;
    for (std::size_t p = 0; p < 6; ++p)
      for (std::size_t q = p + 1; q < 6; ++q) off += M[p * 6 + q] * M[p * 6 + q];
    if (off < 1e-30) break;
    for (std::size_t p = 0; p < 6; ++p) {
      for (std::size_t q = p + 1; q < 6; ++q) {
        const double apq = M[p * 6 + q];
        if (std::fabs(apq) < 1e-300) continue;
        const double theta = (M[q * 6 + q] - M[p * 6 + p]) / (2.0 * apq);
        const double sign = theta >= 0.0 ? 1.0 : -1.0;
        const double t = sign / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (std::size_t k = 0; k < 6; ++k) {
          const double akp = M[k * 6 + p];
          const double akq = M[k * 6 + q];
          M[k * 6 + p] = c * akp - s * akq;
          M[k * 6 + q] = s * akp + c * akq;
        }
        for (std::size_t k = 0; k < 6; ++k) {
          const double apk = M[p * 6 + k];
          const double aqk = M[q * 6 + k];
          M[p * 6 + k] = c * apk - s * aqk;
          M[q * 6 + k] = s * apk + c * aqk;
        }
      }
    }
  }
  for (std::size_t i = 0; i < 6; ++i) eig[i] = M[i * 6 + i];
}

// ---------------------------------------------------------------------------
// The residual.
//
//   r_ik = ( n_i · (R p_ik + t) − d_i ) / sigma_i
//
// one scalar per lidar return: "every point the lidar saw on the board must
// lie on the plane the camera saw" (Zhang & Pless 2004 / Unnikrishnan & Hebert
// 2005). A 3-D sensor puts a PATCH of returns on the board and so supplies
// 3 independent constraints per pose; a 2-D sensor puts a LINE and supplies
// only 2. That single fact is the whole reason the D6 needs ~45 poses where
// the Mid-360 needs 8 (S6 §5).
//
// Left ("global") perturbation:  R <- exp(dtheta) R,  t <- t + dt, so
//
//   dr/d(dtheta) = ((R p) x n) / sigma      dr/d(dt) = n / sigma
//
// which is exact, needs no numeric differencing, and stays well conditioned
// because the rotation increment is always small.
// ---------------------------------------------------------------------------

struct Pose6 {
  double R[9];
  double t[3];
};

void pose_from_mat4(const double m[16], Pose6* out) { se3::mat4_get_rt(m, out->R, out->t); }

void mat4_from_pose(const Pose6& p, double m[16]) { se3::mat4_from_rt(p.R, p.t, m); }

void apply_increment(Pose6* p, const double dx[6]) {
  double dR[9];
  const double w[3] = {dx[0], dx[1], dx[2]};
  se3::so3_exp(w, dR);
  double R[9];
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = 0; j < 3; ++j) {
      double s = 0.0;
      for (std::size_t k = 0; k < 3; ++k) s += dR[i * 3 + k] * p->R[k * 3 + j];
      R[i * 3 + j] = s;
    }
  }
  for (std::size_t i = 0; i < 9; ++i) p->R[i] = R[i];
  for (std::size_t i = 0; i < 3; ++i) p->t[i] += dx[3 + i];
}

void rotate3(const double R[9], const double p[3], double out[3]) {
  const double x = R[0] * p[0] + R[1] * p[1] + R[2] * p[2];
  const double y = R[3] * p[0] + R[4] * p[1] + R[5] * p[2];
  const double z = R[6] * p[0] + R[7] * p[1] + R[8] * p[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

// soft-L1 IRLS weight for a whitened residual. rho(s) = 2(sqrt(1+s)-1) with
// s = (r/f)^2 gives w = 1/sqrt(1+s) — scipy's `loss="soft_l1", f_scale=f`,
// which is what S6's stage 2 used.
inline double robust_weight(double r, double f_scale) {
  const double s = (r / f_scale) * (r / f_scale);
  return 1.0 / std::sqrt(1.0 + s);
}

double cost_of(const std::vector<const PlaneObservation*>& obs, const Pose6& p, bool robust,
               double f_scale) {
  double c = 0.0;
  for (const PlaneObservation* o : obs) {
    const std::size_t k = o->point_count();
    const double inv_sigma = 1.0 / o->sigma_m;
    for (std::size_t i = 0; i < k; ++i) {
      const double* pl = &o->points_lidar[i * 3];
      double Rp[3];
      rotate3(p.R, pl, Rp);
      const double X[3] = {Rp[0] + p.t[0], Rp[1] + p.t[1], Rp[2] + p.t[2]};
      const double r = (se3::dot3(o->normal, X) - o->d) * inv_sigma;
      if (robust) {
        const double s = (r / f_scale) * (r / f_scale);
        c += f_scale * f_scale * (std::sqrt(1.0 + s) - 1.0);
      } else {
        c += 0.5 * r * r;
      }
    }
  }
  return c;
}

struct StageOut {
  int iterations = 0;
  bool converged = false;
  double cost = 0.0;
  double H[36] = {0.0};  // normal matrix at the solution (unit weights)
  std::size_t residuals = 0;
  double sum_sq_whitened = 0.0;
};

// One Levenberg–Marquardt stage. `robust == false` is S6's stage 1 (plain L2
// from the CAD nominal); `robust == true` is stage 2.
StageOut lm_stage(const std::vector<const PlaneObservation*>& obs, Pose6* p, bool robust,
                  const MountCalibConfig& cfg) {
  StageOut out;
  double lambda = cfg.lambda_init;
  double cost = cost_of(obs, *p, robust, cfg.robust_f_scale);

  for (int it = 0; it < cfg.max_iterations; ++it) {
    double H[36] = {0.0};
    double g[6] = {0.0};
    double sum_sq = 0.0;
    std::size_t n_res = 0;

    for (const PlaneObservation* o : obs) {
      const std::size_t k = o->point_count();
      const double inv_sigma = 1.0 / o->sigma_m;
      for (std::size_t i = 0; i < k; ++i) {
        const double* pl = &o->points_lidar[i * 3];
        double Rp[3];
        rotate3(p->R, pl, Rp);
        const double X[3] = {Rp[0] + p->t[0], Rp[1] + p->t[1], Rp[2] + p->t[2]};
        const double r = (se3::dot3(o->normal, X) - o->d) * inv_sigma;

        double cr[3];
        se3::cross3(Rp, o->normal, cr);
        const double J[6] = {cr[0] * inv_sigma, cr[1] * inv_sigma, cr[2] * inv_sigma,
                             o->normal[0] * inv_sigma, o->normal[1] * inv_sigma,
                             o->normal[2] * inv_sigma};

        const double w = robust ? robust_weight(r, cfg.robust_f_scale) : 1.0;
        for (std::size_t a = 0; a < 6; ++a) {
          g[a] -= w * J[a] * r;
          for (std::size_t b = a; b < 6; ++b) H[a * 6 + b] += w * J[a] * J[b];
        }
        sum_sq += r * r;
        ++n_res;
      }
    }
    for (std::size_t a = 0; a < 6; ++a)
      for (std::size_t b = 0; b < a; ++b) H[a * 6 + b] = H[b * 6 + a];

    out.residuals = n_res;
    out.sum_sq_whitened = sum_sq;
    for (std::size_t i = 0; i < 36; ++i) out.H[i] = H[i];
    if (n_res == 0) break;

    bool stepped = false;
    for (int tries = 0; tries < 30; ++tries) {
      double A[36];
      for (std::size_t i = 0; i < 36; ++i) A[i] = H[i];
      for (std::size_t i = 0; i < 6; ++i) {
        // Marquardt scaling: damp by the diagonal, not by the identity, so
        // rotation (radians) and translation (metres) are damped in their own
        // units. A floor keeps a null direction from being un-damped.
        A[i * 6 + i] += lambda * (H[i * 6 + i] > 1e-12 ? H[i * 6 + i] : 1e-12);
      }
      double dx[6];
      if (!solve_sym6(A, g, dx)) {
        lambda *= 10.0;
        if (lambda > 1e14) break;
        continue;
      }
      Pose6 trial = *p;
      apply_increment(&trial, dx);
      const double c_new = cost_of(obs, trial, robust, cfg.robust_f_scale);
      if (std::isfinite(c_new) && c_new < cost) {
        const double improvement = cost - c_new;
        const double step = std::sqrt(dx[0] * dx[0] + dx[1] * dx[1] + dx[2] * dx[2] +
                                      dx[3] * dx[3] + dx[4] * dx[4] + dx[5] * dx[5]);
        *p = trial;
        cost = c_new;
        lambda = std::max(lambda * 0.3, 1e-12);
        stepped = true;
        ++out.iterations;
        if (improvement <= cfg.cost_tol * (std::fabs(cost) + cfg.cost_tol) ||
            step < cfg.step_tol) {
          out.converged = true;
        }
        break;
      }
      lambda *= 10.0;
      if (lambda > 1e14) break;
    }
    if (!stepped) {
      // No damping value improved the cost: we are at a local minimum to
      // within numerical precision.
      out.converged = true;
      break;
    }
    if (out.converged) break;
  }

  out.cost = cost;
  return out;
}

}  // namespace

const char* to_string(CalibGate g) noexcept {
  switch (g) {
    case CalibGate::kUnknown: return "unknown";
    case CalibGate::kGood: return "good";
    case CalibGate::kUsable: return "usable";
    case CalibGate::kReject: return "reject";
  }
  return "?";
}

CalibGate classify_gate(double split_half_px, const MountCalibConfig& cfg) noexcept {
  if (!(split_half_px >= 0.0) || !std::isfinite(split_half_px)) return CalibGate::kUnknown;
  if (split_half_px <= cfg.gate_good_px) return CalibGate::kGood;
  if (split_half_px <= cfg.gate_reject_px) return CalibGate::kUsable;
  return CalibGate::kReject;
}

double reprojection_disagreement_px(const double a[16], const double b[16], double range_m,
                                    const CalibCamera& cam) {
  // A deterministic grid over the central 85 % of the frame — S6 used a
  // fixed-seed random sample of the same region; the RMS over a grid is the
  // same statistic and is reproducible on every CI leg.
  constexpr int kNx = 24;
  constexpr int kNy = 18;
  constexpr double kFill = 0.85;

  double a_inv[16];
  se3::mat4_inverse_rigid(a, a_inv);

  double sum_sq = 0.0;
  int n = 0;
  for (int iy = 0; iy < kNy; ++iy) {
    for (int ix = 0; ix < kNx; ++ix) {
      const double fu = (0.5 - kFill / 2.0) + kFill * (ix + 0.5) / kNx;
      const double fv = (0.5 - kFill / 2.0) + kFill * (iy + 0.5) / kNy;
      const double u = fu * static_cast<double>(cam.width);
      const double v = fv * static_cast<double>(cam.height);
      double dir[3] = {(u - cam.cx) / cam.fx, (v - cam.cy) / cam.fy, 1.0};
      se3::normalize3(dir);

      // Where a point at `range_m` truly is, what the lidar would have
      // measured for it under `a`, and where `b` would then put it.
      const double X_true[3] = {dir[0] * range_m, dir[1] * range_m, dir[2] * range_m};
      double p_lidar[3];
      se3::mat4_apply(a_inv, X_true, p_lidar);
      double X_est[3];
      se3::mat4_apply(b, p_lidar, X_est);
      if (!(X_est[2] > 1e-6) || !(X_true[2] > 1e-6)) continue;

      const double du = (cam.fx * X_est[0] / X_est[2] + cam.cx) - u;
      const double dv = (cam.fy * X_est[1] / X_est[2] + cam.cy) - v;
      sum_sq += du * du + dv * dv;
      ++n;
    }
  }
  if (n == 0) return 0.0;
  return std::sqrt(sum_sq / static_cast<double>(n));
}

MountCalibrationSolver::MountCalibrationSolver(const MountCalibConfig& cfg) : cfg_(cfg) {}
MountCalibrationSolver::~MountCalibrationSolver() = default;

Status MountCalibrationSolver::add_observation(const PlaneObservation& obs) {
  const double n_len = se3::norm3(obs.normal);
  if (!std::isfinite(n_len) || std::fabs(n_len - 1.0) > 1e-3) {
    return set_last_error(ScanError::kInvalidArgument,
                          "mountcalib: plane normal must be a unit vector (|n| = %.6f)", n_len);
  }
  if (!std::isfinite(obs.d)) {
    return set_last_error(ScanError::kInvalidArgument, "mountcalib: non-finite plane offset");
  }
  if (!(obs.sigma_m > 0.0) || !std::isfinite(obs.sigma_m)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "mountcalib: sigma_m must be positive (got %.6f)", obs.sigma_m);
  }
  if (obs.points_lidar.size() % 3 != 0) {
    return set_last_error(ScanError::kInvalidArgument,
                          "mountcalib: points_lidar size %zu is not a multiple of 3",
                          obs.points_lidar.size());
  }
  if (obs.point_count() < cfg_.min_points_per_observation) {
    // WIZARD.md screen 2's "Lidar sees it" check exists so this never fires in
    // the field; it fires here for a capture that slipped past it.
    return set_last_error(ScanError::kInvalidArgument,
                          "mountcalib: observation has %zu points, need >= %zu",
                          obs.point_count(), cfg_.min_points_per_observation);
  }
  for (double v : obs.points_lidar) {
    if (!std::isfinite(v)) {
      return set_last_error(ScanError::kInvalidArgument, "mountcalib: non-finite lidar point");
    }
  }
  obs_.push_back(obs);
  return kOkStatus;
}

Status MountCalibrationSolver::add_observation(const double normal[3], double d,
                                               Span<const PointVertex> lidar_points,
                                               double sigma_m) {
  if (normal == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "mountcalib: null normal");
  }
  PlaneObservation o;
  o.normal[0] = normal[0];
  o.normal[1] = normal[1];
  o.normal[2] = normal[2];
  o.d = d;
  o.sigma_m = sigma_m;
  o.points_lidar.reserve(lidar_points.size() * 3);
  for (std::size_t i = 0; i < lidar_points.size(); ++i) {
    o.points_lidar.push_back(static_cast<double>(lidar_points[i].x));
    o.points_lidar.push_back(static_cast<double>(lidar_points[i].y));
    o.points_lidar.push_back(static_cast<double>(lidar_points[i].z));
  }
  return add_observation(o);
}

Result<MountCalibResult> MountCalibrationSolver::solve(const double cad_camera_from_lidar[16]) {
  if (cad_camera_from_lidar == nullptr || !se3::mat4_is_rigid(cad_camera_from_lidar, 1e-4)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "mountcalib: CAD initial guess is not a rigid row-major 4x4");
  }
  if (obs_.size() < 3) {
    // Six unknowns against 2–3 constraints per pose: not merely poorly
    // conditioned, undetermined. Refuse rather than return a number.
    return set_last_error(ScanError::kInvalidArgument,
                          "mountcalib: %zu observations, need at least 3 to determine 6 DoF",
                          obs_.size());
  }

  MountCalibResult res;
  res.observations = obs_.size();
  res.gate_range_m = cfg_.gate_range_m;

  std::vector<const PlaneObservation*> all;
  all.reserve(obs_.size());
  for (const PlaneObservation& o : obs_) all.push_back(&o);

  Pose6 p;
  pose_from_mat4(cad_camera_from_lidar, &p);

  // --- Stage 1: plain L2 from the CAD nominal. -----------------------------
  // S6 §2.3: a robust kernel here stalls the solve, because from 4 deg / 25 mm
  // out EVERY residual is large and soft-L1 down-weights all of them equally,
  // shrinking the gradient to nothing. `robust_first_stage` exists only so the
  // test suite can reproduce that failure.
  const StageOut s1 = lm_stage(all, &p, cfg_.robust_first_stage, cfg_);
  res.iterations_l2 = s1.iterations;
  StageOut last = s1;

  // --- Stage 2: robust refinement from the (now close) L2 answer. ----------
  if (cfg_.run_robust_stage) {
    const StageOut s2 = lm_stage(all, &p, true, cfg_);
    res.iterations_robust = s2.iterations;
    last = s2;
  }
  res.converged = last.converged;
  res.final_cost = last.cost;
  res.residuals = last.residuals;
  mat4_from_pose(p, res.camera_from_lidar);

  // RMS in metres: the residuals are whitened by sigma, so scale back by the
  // mean sigma to report something a user can read as a distance.
  double mean_sigma = 0.0;
  for (const PlaneObservation& o : obs_) mean_sigma += o.sigma_m;
  mean_sigma /= static_cast<double>(obs_.size());
  res.rms_residual_m =
      last.residuals > 0
          ? std::sqrt(last.sum_sq_whitened / static_cast<double>(last.residuals)) * mean_sigma
          : 0.0;

  // --- Diagnostics. NOT the gate (S6 action G). ----------------------------
  double eig[6];
  eigenvalues_sym6(last.H, eig);
  double lo = eig[0], hi = eig[0];
  for (std::size_t i = 1; i < 6; ++i) {
    lo = std::min(lo, eig[i]);
    hi = std::max(hi, eig[i]);
  }
  res.condition_number = (lo > 0.0) ? hi / lo : 1e300;
  double cov[36];
  if (last.residuals > 6 && inverse_sym6(last.H, cov)) {
    const double s2v = last.sum_sq_whitened / static_cast<double>(last.residuals - 6);
    double rot_var = 0.0, tr_var = 0.0;
    for (std::size_t i = 0; i < 3; ++i) rot_var += std::max(0.0, cov[i * 6 + i]) * s2v;
    for (std::size_t i = 3; i < 6; ++i) tr_var += std::max(0.0, cov[i * 6 + i]) * s2v;
    res.sigma_rot_deg = std::sqrt(rot_var / 3.0) * se3::kRadToDeg;
    res.sigma_trans_mm = std::sqrt(tr_var / 3.0) * 1000.0;
  }

  res.degenerate = obs_.size() < cfg_.min_observations || !(res.condition_number < 1e12);

  // --- The gate: split-half agreement (S6 action G / WIZARD.md §2). --------
  //
  // Solve twice on two DISJOINT halves of the captured poses and report how
  // far apart the two answers place a point at the gate range. It observes the
  // actual noise realisation and the actual pose geometry, which the
  // linearised covariance above cannot: with a prescribed pose set that
  // covariance is nearly constant session to session.
  //
  // Both halves start from the SAME CAD nominal, because in the field that is
  // the only initial guess there is.
  if (cfg_.compute_split_half && obs_.size() >= 4) {
    std::vector<const PlaneObservation*> even, odd;
    for (std::size_t i = 0; i < obs_.size(); ++i) {
      (i % 2 == 0 ? even : odd).push_back(&obs_[i]);
    }
    MountCalibConfig half_cfg = cfg_;
    half_cfg.compute_split_half = false;

    Pose6 pa, pb;
    pose_from_mat4(cad_camera_from_lidar, &pa);
    pose_from_mat4(cad_camera_from_lidar, &pb);
    (void)lm_stage(even, &pa, half_cfg.robust_first_stage, half_cfg);
    if (half_cfg.run_robust_stage) (void)lm_stage(even, &pa, true, half_cfg);
    (void)lm_stage(odd, &pb, half_cfg.robust_first_stage, half_cfg);
    if (half_cfg.run_robust_stage) (void)lm_stage(odd, &pb, true, half_cfg);

    double Ta[16], Tb[16];
    mat4_from_pose(pa, Ta);
    mat4_from_pose(pb, Tb);
    res.split_half_px =
        reprojection_disagreement_px(Ta, Tb, cfg_.gate_range_m, cfg_.camera);
    res.gate = classify_gate(res.split_half_px, cfg_);
  }
  if (res.degenerate) res.gate = CalibGate::kReject;

  SCAN_LOG_DEBUG(kMod,
                "solve: %zu obs, %zu residuals, rms %.2f mm, split-half %.1f px @ %.1f m -> %s%s",
                res.observations, res.residuals, res.rms_residual_m * 1000.0, res.split_half_px,
                res.gate_range_m, to_string(res.gate), res.degenerate ? " (degenerate)" : "");
  return res;
}

}  // namespace scanengine
