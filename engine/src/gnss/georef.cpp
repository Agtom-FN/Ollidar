#include "scanengine/gnss/georef.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "georef";

// Rayleigh circular-error factors: CEP50 = 1.1774σ, CEP95 = 2.4477σ for a
// circular bivariate normal. Standard survey reporting, and what a user
// comparing this to a receiver datasheet expects to see.
constexpr double kCep50 = 1.17741002251547;
constexpr double kCep95 = 2.44774683068390;

double sq(double x) { return x * x; }

void mat3_mul(const double a[9], const double b[9], double out[9]) {
  double t[9];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      t[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] +
                     a[r * 3 + 2] * b[2 * 3 + c];
    }
  }
  for (int i = 0; i < 9; ++i) out[i] = t[i];
}

}  // namespace

// ===========================================================================
// WeightedSimilarityEstimator
// ===========================================================================

WeightedSimilarityEstimator::WeightedSimilarityEstimator(
    const SimilarityEstimatorConfig& cfg)
    : cfg_(cfg) {
  if (cfg_.window == 0) cfg_.window = 1;
  obs_.reserve(std::min<std::size_t>(cfg_.window, 4096));
}

void WeightedSimilarityEstimator::add(const GeorefObservation& o) {
  GeorefObservation obs = o;
  // A zero or negative sigma would become an infinite weight; a fix with no
  // stated accuracy gets the fix-state table instead of dominating the fit.
  if (!(obs.sigma_h_m > 0.0) || !std::isfinite(obs.sigma_h_m)) {
    obs.sigma_h_m = default_sigma_for_fix(obs.fix);
  }
  if (!(obs.sigma_v_m > 0.0) || !std::isfinite(obs.sigma_v_m)) {
    obs.sigma_v_m = obs.sigma_h_m * 1.6;
  }
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(obs.local[i]) || !std::isfinite(obs.global[i])) return;
  }
  obs_.push_back(obs);
  if (obs_.size() > cfg_.window) {
    obs_.erase(obs_.begin(),
               obs_.begin() + static_cast<std::ptrdiff_t>(obs_.size() - cfg_.window));
  }
}

void WeightedSimilarityEstimator::clear() {
  obs_.clear();
  w_.clear();
  r_.clear();
}

bool WeightedSimilarityEstimator::solve(GeorefSolution* out) {
  GeorefSolution sol;
  if (out) *out = sol;
  const std::size_t n = obs_.size();
  if (n == 0) {
    sol.blocker = "no observations";
    if (out) *out = sol;
    return false;
  }

  sol.samples = n;
  sol.t_first_ns = obs_.front().t_ns;
  sol.t_last_ns = obs_.back().t_ns;

  std::vector<char> active(n, 1);
  for (std::size_t i = 0; i < n; ++i) {
    if (!fix_at_least(obs_[i].fix, cfg_.min_fix)) active[i] = 0;
  }

  double yaw = 0.0, scale = 1.0;
  double t[3] = {0.0, 0.0, 0.0};
  double sum_wh = 0.0, sum_wv = 0.0, sum_wh_r2 = 0.0;
  std::size_t inliers = 0;
  std::uint32_t iters = 0;

  w_.assign(n, 0.0);
  r_.assign(n, 0.0);
  std::vector<double> wv(n, 0.0);
  std::vector<double> rv(n, 0.0);

  for (int pass = 0; pass <= cfg_.reject_passes; ++pass) {
    // ---- IRLS ------------------------------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
      w_[i] = active[i] ? 1.0 / sq(obs_[i].sigma_h_m) : 0.0;
      wv[i] = active[i] ? 1.0 / sq(obs_[i].sigma_v_m) : 0.0;
    }

    double prev_yaw = 1e9, prev_tx = 1e9, prev_ty = 1e9;
    for (int it = 0; it < cfg_.max_iterations; ++it) {
      ++iters;
      double swh = 0.0, swv = 0.0;
      double pbx = 0.0, pby = 0.0, qbx = 0.0, qby = 0.0;
      double pbz = 0.0, qbz = 0.0;
      for (std::size_t i = 0; i < n; ++i) {
        if (!active[i]) continue;
        swh += w_[i];
        swv += wv[i];
        pbx += w_[i] * obs_[i].local[0];
        pby += w_[i] * obs_[i].local[1];
        qbx += w_[i] * obs_[i].global[0];
        qby += w_[i] * obs_[i].global[1];
        pbz += wv[i] * obs_[i].local[2];
        qbz += wv[i] * obs_[i].global[2];
      }
      if (!(swh > 0.0)) break;
      pbx /= swh; pby /= swh; qbx /= swh; qby /= swh;
      if (swv > 0.0) { pbz /= swv; qbz /= swv; }

      // Closed-form weighted Umeyama, restricted to a rotation about +Up.
      // Sxy is the cross term that carries the sense of rotation; Sxx the
      // aligned term. atan2 of the pair is the exact minimiser — no
      // linearisation, so a 170° initial misalignment converges in the same
      // one step a 1° one does.
      double Sxx = 0.0, Sxy = 0.0, Spp = 0.0;
      for (std::size_t i = 0; i < n; ++i) {
        if (!active[i]) continue;
        const double px = obs_[i].local[0] - pbx, py = obs_[i].local[1] - pby;
        const double qx = obs_[i].global[0] - qbx, qy = obs_[i].global[1] - qby;
        Sxx += w_[i] * (px * qx + py * qy);
        Sxy += w_[i] * (px * qy - py * qx);
        Spp += w_[i] * (px * px + py * py);
      }
      yaw = std::atan2(Sxy, Sxx);
      scale = 1.0;
      if (!cfg_.lock_scale && Spp > 0.0) {
        scale = std::sqrt(Sxx * Sxx + Sxy * Sxy) / Spp;
        if (!(scale > 1e-6) || !std::isfinite(scale)) scale = 1.0;
      }
      const double cy = std::cos(yaw), sy = std::sin(yaw);
      t[0] = qbx - scale * (cy * pbx - sy * pby);
      t[1] = qby - scale * (sy * pbx + cy * pby);
      t[2] = (swv > 0.0) ? (qbz - scale * pbz) : 0.0;

      sum_wh = swh;
      sum_wv = swv;
      sum_wh_r2 = Spp;

      // ---- residuals and Huber re-weighting -----------------------------
      for (std::size_t i = 0; i < n; ++i) {
        const double px = obs_[i].local[0], py = obs_[i].local[1], pz = obs_[i].local[2];
        const double ex = obs_[i].global[0] - (scale * (cy * px - sy * py) + t[0]);
        const double ey = obs_[i].global[1] - (scale * (sy * px + cy * py) + t[1]);
        const double ez = obs_[i].global[2] - (scale * pz + t[2]);
        r_[i] = std::sqrt(ex * ex + ey * ey);
        rv[i] = std::fabs(ez);
        if (!active[i]) continue;
        // Huber in units of the observation's OWN sigma: a 2 cm Fixed sample
        // and a 2 m Single sample are held to the same statistical standard,
        // which is the whole point of weighting by fix quality rather than
        // thresholding on metres.
        const double zn = r_[i] / obs_[i].sigma_h_m;
        const double f = (zn <= cfg_.huber_k || zn <= 0.0) ? 1.0 : cfg_.huber_k / zn;
        w_[i] = f / sq(obs_[i].sigma_h_m);
        const double zv = rv[i] / obs_[i].sigma_v_m;
        const double fv = (zv <= cfg_.huber_k || zv <= 0.0) ? 1.0 : cfg_.huber_k / zv;
        wv[i] = fv / sq(obs_[i].sigma_v_m);
      }

      if (std::fabs(yaw - prev_yaw) < cfg_.convergence_yaw_rad &&
          std::fabs(t[0] - prev_tx) < cfg_.convergence_translation_m &&
          std::fabs(t[1] - prev_ty) < cfg_.convergence_translation_m) {
        break;
      }
      prev_yaw = yaw;
      prev_tx = t[0];
      prev_ty = t[1];
    }

    // ---- hard outlier rejection -----------------------------------------
    if (pass == cfg_.reject_passes) break;
    std::size_t newly_rejected = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (!active[i]) continue;
      const double limit = cfg_.reject_k * std::max(obs_[i].sigma_h_m, cfg_.reject_floor_m);
      if (r_[i] > limit) {
        active[i] = 0;
        ++newly_rejected;
      }
    }
    if (newly_rejected == 0) break;
  }

  // ---- statistics --------------------------------------------------------
  double sum_r2 = 0.0, sum_rh2 = 0.0, sum_rv2 = 0.0, rmax = 0.0;
  double wsum_sigma = 0.0, wsum = 0.0;
  double lever2_w = 0.0;
  double pbx = 0.0, pby = 0.0, swh_plain = 0.0;
  std::size_t counts[5] = {0, 0, 0, 0, 0};
  FixType best = FixType::kNone;

  for (std::size_t i = 0; i < n; ++i) {
    if (!active[i]) continue;
    ++inliers;
    swh_plain += 1.0 / sq(obs_[i].sigma_h_m);
    pbx += obs_[i].local[0] / sq(obs_[i].sigma_h_m);
    pby += obs_[i].local[1] / sq(obs_[i].sigma_h_m);
  }
  if (swh_plain > 0.0) { pbx /= swh_plain; pby /= swh_plain; }

  for (std::size_t i = 0; i < n; ++i) {
    if (!active[i]) continue;
    sum_rh2 += sq(r_[i]);
    sum_rv2 += sq(rv[i]);
    sum_r2 += sq(r_[i]) + sq(rv[i]);
    rmax = std::max(rmax, std::sqrt(sq(r_[i]) + sq(rv[i])));
    const double wi = 1.0 / sq(obs_[i].sigma_h_m);
    wsum += wi;
    wsum_sigma += wi * obs_[i].sigma_h_m;
    lever2_w += wi * (sq(obs_[i].local[0] - pbx) + sq(obs_[i].local[1] - pby));
    counts[static_cast<std::size_t>(obs_[i].fix)]++;
    if (static_cast<std::uint8_t>(obs_[i].fix) > static_cast<std::uint8_t>(best)) {
      best = obs_[i].fix;
    }
  }
  sol.rejected = n - inliers;
  sol.inliers = inliers;

  if (inliers > 0) {
    sol.residual_rms_m = std::sqrt(sum_r2 / static_cast<double>(inliers));
    sol.residual_rms_h_m = std::sqrt(sum_rh2 / static_cast<double>(inliers));
    sol.residual_rms_v_m = std::sqrt(sum_rv2 / static_cast<double>(inliers));
    sol.residual_max_m = rmax;
    sol.mean_fix_sigma_m = wsum > 0.0 ? wsum_sigma / wsum : 0.0;
    sol.lever_arm_rms_m = wsum > 0.0 ? std::sqrt(lever2_w / wsum) : 0.0;
    sol.best_fix = best;
    std::size_t dom = 0;
    for (std::size_t k = 1; k < 5; ++k) {
      if (counts[k] > counts[dom]) dom = k;
    }
    sol.dominant_fix = static_cast<FixType>(dom);
    for (std::size_t k = 0; k < 5; ++k) {
      sol.fix_fraction[k] = static_cast<double>(counts[k]) / static_cast<double>(inliers);
    }
  }

  // Spatial extent: two-pass farthest-point diameter. O(n) and never
  // overestimates, which matters because min_span_m is a gate.
  {
    double best_d = 0.0;
    std::size_t a = 0;
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
      if (!active[i]) continue;
      const double d = sq(obs_[i].local[0] - pbx) + sq(obs_[i].local[1] - pby);
      if (!any || d > best_d) { best_d = d; a = i; any = true; }
    }
    double diam = 0.0;
    if (any) {
      for (std::size_t i = 0; i < n; ++i) {
        if (!active[i]) continue;
        const double d = sq(obs_[i].local[0] - obs_[a].local[0]) +
                         sq(obs_[i].local[1] - obs_[a].local[1]);
        diam = std::max(diam, d);
      }
    }
    sol.span_m = std::sqrt(diam);
  }

  // Parameter uncertainty from the (JᵀWJ)⁻¹ of this 4-parameter problem —
  // which is diagonal in (yaw, tx, ty, tz) once the points are centred.
  // Then INFLATED by the reduced chi-square when the residuals are bigger
  // than the weights claim they should be: that excess is SLAM drift or an
  // unmodelled lever arm, and pretending otherwise is how a georeferencing
  // report ends up claiming 2 cm on a 20 cm cloud.
  double chi_h = 1.0, chi_v = 1.0;
  if (inliers > 4) {
    double s2h = 0.0, s2v = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (!active[i]) continue;
      s2h += sq(r_[i] / obs_[i].sigma_h_m);
      s2v += sq(rv[i] / obs_[i].sigma_v_m);
    }
    // 4 horizontal dof (yaw, tx, ty, and scale when free); 1 vertical.
    const double dofh = static_cast<double>(2 * inliers) - (cfg_.lock_scale ? 3.0 : 4.0);
    const double dofv = static_cast<double>(inliers) - 1.0;
    if (dofh > 0.0) chi_h = std::max(1.0, std::sqrt(s2h / dofh));
    if (dofv > 0.0) chi_v = std::max(1.0, std::sqrt(s2v / dofv));
  }

  if (sum_wh > 0.0) sol.translation_sigma_h_m = chi_h / std::sqrt(sum_wh);
  if (sum_wv > 0.0) sol.translation_sigma_v_m = chi_v / std::sqrt(sum_wv);
  if (sum_wh_r2 > 0.0) {
    sol.yaw_sigma_deg = chi_h / std::sqrt(sum_wh_r2) * se3::kRadToDeg;
  } else {
    sol.yaw_sigma_deg = 180.0;
  }

  // Vertical residual vs horizontal radius: a local frame whose +Z is not
  // gravity shows up here as a slope, and the number reported is the
  // vertical error that tilt produces at the working radius.
  if (inliers >= 3 && sol.lever_arm_rms_m > 1e-6) {
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, cnt = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (!active[i]) continue;
      const double rad = std::sqrt(sq(obs_[i].local[0] - pbx) + sq(obs_[i].local[1] - pby));
      const double ez = obs_[i].global[2] - (scale * obs_[i].local[2] + t[2]);
      sx += rad; sy += ez; sxx += rad * rad; sxy += rad * ez; cnt += 1.0;
    }
    const double den = cnt * sxx - sx * sx;
    if (std::fabs(den) > 1e-12) {
      const double slope = (cnt * sxy - sx * sy) / den;
      sol.gravity_residual_m = std::fabs(slope) * sol.lever_arm_rms_m;
    }
  }

  // The number a UI shows. Three independent contributions, added in
  // quadrature:
  //   1. the transform's own translation uncertainty        (averages down)
  //   2. yaw uncertainty × the working radius               (averages down)
  //   3. the fixes' own accuracy                            (does NOT)
  // (3) is the common-mode term: every fix in a session shares the base
  // station's coordinate error, its antenna phase-centre model and the
  // atmospheric bias, so no amount of averaging removes it. Treating it as
  // fully correlated is pessimistic; treating it as independent (which is
  // what leaving it out amounts to) is optimistic by an order of magnitude.
  // docs/A10-gnss.md §5 shows what each mix reports.
  const double yaw_term = sol.yaw_sigma_deg * se3::kDegToRad * sol.lever_arm_rms_m;
  sol.horizontal_sigma_m = std::sqrt(sq(sol.translation_sigma_h_m) + sq(yaw_term) +
                                     sq(sol.mean_fix_sigma_m));
  sol.vertical_sigma_m =
      std::sqrt(sq(sol.translation_sigma_v_m) + sq(sol.mean_fix_sigma_m * 1.6));
  sol.cep50_m = kCep50 * sol.horizontal_sigma_m;
  sol.cep95_m = kCep95 * sol.horizontal_sigma_m;

  sol.yaw_rad = yaw;
  sol.yaw_deg = yaw * se3::kRadToDeg;
  sol.scale = scale;
  sol.translation[0] = t[0];
  sol.translation[1] = t[1];
  sol.translation[2] = t[2];
  sol.iterations = iters;

  const double cy = std::cos(yaw), sy = std::sin(yaw);
  const double R[9] = {scale * cy, -scale * sy, 0.0, scale * sy, scale * cy, 0.0,
                       0.0,        0.0,         scale};
  se3::mat4_from_rt(R, t, sol.global_from_local);

  // ---- convergence gates -------------------------------------------------
  if (inliers < cfg_.min_samples) {
    sol.blocker = "not enough usable fixes";
  } else if (sol.span_m < cfg_.min_span_m) {
    // Yaw is only observable from a baseline. Say so instead of returning a
    // confident heading derived from a metre of walking.
    sol.blocker = "trajectory too short to observe heading";
  } else if (sol.yaw_sigma_deg > cfg_.max_yaw_sigma_deg) {
    sol.blocker = "heading uncertainty above threshold";
  } else {
    sol.converged = true;
    sol.blocker = "";
  }

  if (out) *out = sol;
  return sol.converged;
}

// ===========================================================================
// GeorefFusion
// ===========================================================================

GeorefFusion::GeorefFusion(const GeorefConfig& cfg) : cfg_(cfg) {
  cfg_.estimator.min_fix = cfg_.min_fix;
  est_.reset(new WeightedSimilarityEstimator(cfg_.estimator));
  if (!cfg_.crs.epsg.empty()) epsg_ = crs::parse_epsg_string(cfg_.crs.epsg);
}

GeorefFusion::~GeorefFusion() = default;

Status GeorefFusion::set_estimator(std::unique_ptr<GeorefEstimator> est) {
  if (!est) return set_last_error(ScanError::kInvalidArgument, "GeorefFusion: null estimator");
  std::lock_guard<std::mutex> lock(m_);
  if (est_ && est_->size() > 0) {
    return set_last_error(ScanError::kInvalidState,
                          "GeorefFusion::set_estimator: %zu observations already "
                          "accumulated; swap the estimator before the first fix",
                          est_->size());
  }
  est_ = std::move(est);
  return kOkStatus;
}

void GeorefFusion::set_local_source(const PoseInterpolator* src) {
  std::lock_guard<std::mutex> lock(m_);
  local_ = src;
}

Status GeorefFusion::set_enu_frame(const crs::EnuFrame& frame) {
  if (!frame.valid) {
    return set_last_error(ScanError::kInvalidArgument, "GeorefFusion: invalid ENU frame");
  }
  std::lock_guard<std::mutex> lock(m_);
  frame_ = frame;
  has_frame_ = true;
  if (epsg_ == 0 && cfg_.crs.auto_utm) {
    epsg_ = crs::utm_epsg(crs::utm_zone_for(frame.origin.lat_deg, frame.origin.lon_deg));
    SCAN_LOG_INFO(kMod, "CRS auto-selected: %s (%s)", crs::epsg_string(epsg_).c_str(),
                  crs::crs_name_for_epsg(epsg_).c_str());
  }
  return kOkStatus;
}

bool GeorefFusion::has_frame() const {
  std::lock_guard<std::mutex> lock(m_);
  return has_frame_;
}

void GeorefFusion::set_allow_unconverged(bool v) {
  std::lock_guard<std::mutex> lock(m_);
  allow_unconverged_ = v;
}

Status GeorefFusion::add_observation(const GeorefObservation& obs) {
  std::lock_guard<std::mutex> lock(m_);
  ++stats_.offered;
  if (!fix_at_least(obs.fix, cfg_.min_fix)) {
    ++stats_.skipped_fix_quality;
    return kOkStatus;
  }
  if (cfg_.min_interval_ns > 0 && stats_.accepted > 0 &&
      obs.t_ns - last_accept_ns_ < cfg_.min_interval_ns) {
    ++stats_.skipped_decimation;
    return kOkStatus;
  }
  est_->add(obs);
  last_accept_ns_ = obs.t_ns;
  ++stats_.accepted;
  dirty_ = true;
  if (cfg_.resolve_interval_ns <= 0 || obs.t_ns - last_solve_ns_ >= cfg_.resolve_interval_ns) {
    solve_locked_();
    last_solve_ns_ = obs.t_ns;
  }
  return kOkStatus;
}

Status GeorefFusion::add_pair(std::int64_t t_ns, const double local_xyz[3],
                              const GnssFix& fix) {
  crs::EnuFrame frame;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!has_frame_) {
      return set_last_error(ScanError::kInvalidState,
                            "GeorefFusion: set_enu_frame() before adding fixes");
    }
    frame = frame_;
  }
  crs::Geodetic g;
  g.lat_deg = fix.lat_deg;
  g.lon_deg = fix.lon_deg;
  g.height_m = fix.height_ellipsoid_m;
  const crs::Enu e = crs::geodetic_to_enu(frame, g);

  GeorefObservation obs;
  obs.t_ns = t_ns;
  obs.local[0] = local_xyz[0];
  obs.local[1] = local_xyz[1];
  obs.local[2] = local_xyz[2];
  obs.global[0] = e.e;
  obs.global[1] = e.n;
  obs.global[2] = e.u;
  obs.sigma_h_m = fix.sigma_horizontal_m > 0.f ? fix.sigma_horizontal_m
                                               : default_sigma_for_fix(fix.fix);
  obs.sigma_v_m = fix.sigma_up_m > 0.f ? fix.sigma_up_m : obs.sigma_h_m * 1.6;
  obs.fix = fix.fix;
  return add_observation(obs);
}

Status GeorefFusion::add_fix(const GnssFix& fix) {
  const PoseInterpolator* src = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_);
    src = local_;
  }
  if (src == nullptr) {
    return set_last_error(ScanError::kInvalidState,
                          "GeorefFusion::add_fix: no local pose source set");
  }
  const PoseSample s = src->sample_at(fix.t_mono_ns);
  if (!s.has_pose) {
    std::lock_guard<std::mutex> lock(m_);
    ++stats_.offered;
    ++stats_.skipped_no_pose;
    return s.retryable() ? Status(ScanError::kAgain) : kOkStatus;
  }
  if (cfg_.require_ungated_local_pose && !s.ok()) {
    std::lock_guard<std::mutex> lock(m_);
    ++stats_.offered;
    ++stats_.skipped_gated_pose;
    return kOkStatus;
  }
  return add_pair(fix.t_mono_ns, s.pose.position, fix);
}

bool GeorefFusion::solve() {
  std::lock_guard<std::mutex> lock(m_);
  return solve_locked_();
}

bool GeorefFusion::solve_locked_() {
  ++stats_.solves;
  dirty_ = false;
  return est_->solve(&sol_);
}

GeorefSolution GeorefFusion::solution() const {
  std::lock_guard<std::mutex> lock(m_);
  return sol_;
}

bool GeorefFusion::converged() const {
  std::lock_guard<std::mutex> lock(m_);
  return sol_.converged;
}

GeorefFusion::Stats GeorefFusion::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  return stats_;
}

Status GeorefFusion::to_global_point(const double local[3], double enu_out[3]) const {
  std::lock_guard<std::mutex> lock(m_);
  if (!sol_.converged && !allow_unconverged_) {
    return set_last_error(ScanError::kInvalidState,
                          "GeorefFusion: transform not converged (%s)", sol_.blocker);
  }
  se3::mat4_apply(sol_.global_from_local, local, enu_out);
  return kOkStatus;
}

Status GeorefFusion::to_global(const Pose& local, Pose* out) const {
  if (out == nullptr) return set_last_error(ScanError::kInvalidArgument, "null out");
  std::lock_guard<std::mutex> lock(m_);
  if (!sol_.converged && !allow_unconverged_) {
    return set_last_error(ScanError::kInvalidState,
                          "GeorefFusion: transform not converged (%s)", sol_.blocker);
  }
  Pose p = local;
  se3::mat4_apply(sol_.global_from_local, local.position, p.position);
  // Orientation: compose the yaw. Via matrices because se3.h deliberately
  // ships no quaternion product (nothing needed one until now) and a 3x3
  // multiply is cheaper than adding a public helper other modules would then
  // have to keep working.
  double Rl[9], Rg[9];
  se3::quat_to_matrix(local.orientation, Rl);
  const double cy = std::cos(sol_.yaw_rad), sy = std::sin(sol_.yaw_rad);
  const double Ry[9] = {cy, -sy, 0.0, sy, cy, 0.0, 0.0, 0.0, 1.0};
  mat3_mul(Ry, Rl, Rg);
  se3::matrix_to_quat(Rg, p.orientation);
  // The transform's own uncertainty adds to the pose's.
  p.position_sigma_m = static_cast<float>(
      std::sqrt(sq(static_cast<double>(local.position_sigma_m)) + sq(sol_.horizontal_sigma_m)));
  p.orientation_sigma_deg = static_cast<float>(
      std::sqrt(sq(static_cast<double>(local.orientation_sigma_deg)) + sq(sol_.yaw_sigma_deg)));
  *out = p;
  return kOkStatus;
}

Status GeorefFusion::to_global_points(const PointVertex* pts, std::size_t n,
                                      double* out_xyz) const {
  if ((pts == nullptr && n > 0) || out_xyz == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "GeorefFusion::to_global_points: null");
  }
  double m[16];
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!sol_.converged && !allow_unconverged_) {
      return set_last_error(ScanError::kInvalidState,
                            "GeorefFusion: transform not converged (%s)", sol_.blocker);
    }
    for (int i = 0; i < 16; ++i) m[i] = sol_.global_from_local[i];
  }
  // Snapshot the matrix and release the lock: a page can hold a million
  // points and holding the fusion mutex across that would stall the GNSS
  // push thread for milliseconds.
  for (std::size_t i = 0; i < n; ++i) {
    const double p[3] = {static_cast<double>(pts[i].x), static_cast<double>(pts[i].y),
                         static_cast<double>(pts[i].z)};
    se3::mat4_apply(m, p, out_xyz + 3 * i);
  }
  return kOkStatus;
}

Status GeorefFusion::to_global_points(const PageView& page, double* out_xyz,
                                      std::size_t cap) const {
  if (!page.valid()) return set_last_error(ScanError::kInvalidArgument, "invalid page");
  if (cap < static_cast<std::size_t>(page.count) * 3) {
    return set_last_error(ScanError::kCapacityExceeded,
                          "GeorefFusion::to_global_points: need %zu doubles, got %zu",
                          static_cast<std::size_t>(page.count) * 3, cap);
  }
  return to_global_points(page.data, page.count, out_xyz);
}

Status GeorefFusion::to_wgs84(const double local[3], crs::Geodetic* out) const {
  if (out == nullptr) return set_last_error(ScanError::kInvalidArgument, "null out");
  double enu[3];
  SCAN_TRY(to_global_point(local, enu));
  crs::EnuFrame frame;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!has_frame_) return set_last_error(ScanError::kInvalidState, "no ENU frame");
    frame = frame_;
  }
  *out = crs::enu_to_geodetic(frame, crs::Enu{enu[0], enu[1], enu[2]});
  return kOkStatus;
}

Status GeorefFusion::to_utm(const double local[3], crs::UtmCoord* out) const {
  if (out == nullptr) return set_last_error(ScanError::kInvalidArgument, "null out");
  crs::Geodetic g;
  SCAN_TRY(to_wgs84(local, &g));
  const int code = epsg();
  const crs::UtmZone z = crs::utm_zone_from_epsg(code);
  *out = z.valid() ? crs::geodetic_to_utm_zone(g, z) : crs::geodetic_to_utm(g);
  return kOkStatus;
}

bool GeorefFusion::origin_wgs84(crs::Geodetic* out) const {
  std::lock_guard<std::mutex> lock(m_);
  if (!has_frame_) return false;
  if (out) *out = frame_.origin;
  return true;
}

bool GeorefFusion::origin_utm(crs::UtmCoord* out) const {
  crs::Geodetic g;
  if (!origin_wgs84(&g)) return false;
  const int code = epsg();
  const crs::UtmZone z = crs::utm_zone_from_epsg(code);
  const crs::UtmCoord c = z.valid() ? crs::geodetic_to_utm_zone(g, z) : crs::geodetic_to_utm(g);
  if (out) *out = c;
  return true;
}

int GeorefFusion::epsg() const {
  std::lock_guard<std::mutex> lock(m_);
  return epsg_;
}

std::string GeorefFusion::epsg_string() const { return crs::epsg_string(epsg()); }

std::string GeorefFusion::crs_wkt() const {
  const int code = epsg();
  if (code == 0) return std::string();
  return crs::wkt1_for_epsg(code);
}

std::string GeorefFusion::proj_string() const {
  const int code = epsg();
  if (code == 0) return std::string();
  return crs::proj_string_for_epsg(code);
}

}  // namespace scanengine
