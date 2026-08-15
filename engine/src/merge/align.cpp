// align.cpp — the three coarse alignment paths (A13). See merge/align.h for
// what each one is for and where each one is unreliable.
#include "scanengine/merge/align.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/poses/se3.h"
#include "scanengine/slam/post/cloud_filter.h"

#include "voxel_grid.h"

namespace scanengine {
namespace merge {
namespace {

// --- symmetric eigen, cyclic Jacobi -----------------------------------------
//
// Same shape as the 3x3 in src/slam/post/loop_closure.cpp, and for the same
// reason: a fixed sweep order and a fixed sweep count with no magnitude
// pivoting gives an identical eigenvector on all five CI legs. Eigenvalues
// ascending in `w`, eigenvectors as the COLUMNS of `v` (row-major).
template <int N>
void sym_jacobi(const double a_in[N * N], double w[N], double v[N * N]) {
  double a[N * N];
  for (int i = 0; i < N * N; ++i) a[i] = a_in[i];
  for (int i = 0; i < N * N; ++i) v[i] = 0.0;
  for (int i = 0; i < N; ++i) v[i * N + i] = 1.0;

  for (int sweep = 0; sweep < 16; ++sweep) {
    double off = 0.0;
    for (int p = 0; p < N; ++p) {
      for (int q = p + 1; q < N; ++q) off += a[p * N + q] * a[p * N + q];
    }
    if (off < 1e-30) break;
    for (int p = 0; p < N; ++p) {
      for (int q = p + 1; q < N; ++q) {
        const double apq = a[p * N + q];
        if (std::fabs(apq) < 1e-300) continue;
        const double theta = (a[q * N + q] - a[p * N + p]) / (2.0 * apq);
        const double t =
            (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (int i = 0; i < N; ++i) {
          const double aip = a[i * N + p], aiq = a[i * N + q];
          a[i * N + p] = c * aip - s * aiq;
          a[i * N + q] = s * aip + c * aiq;
        }
        for (int j = 0; j < N; ++j) {
          const double apj = a[p * N + j], aqj = a[q * N + j];
          a[p * N + j] = c * apj - s * aqj;
          a[q * N + j] = s * apj + c * aqj;
        }
        for (int i = 0; i < N; ++i) {
          const double vip = v[i * N + p], viq = v[i * N + q];
          v[i * N + p] = c * vip - s * viq;
          v[i * N + q] = s * vip + c * viq;
        }
      }
    }
  }
  for (int i = 0; i < N; ++i) w[i] = a[i * N + i];
  for (int i = 0; i < N - 1; ++i) {
    for (int j = 0; j < N - 1 - i; ++j) {
      if (w[j] > w[j + 1]) {
        std::swap(w[j], w[j + 1]);
        for (int r = 0; r < N; ++r) std::swap(v[r * N + j], v[r * N + j + 1]);
      }
    }
  }
}

// --- 1-D occupancy histogram + normalized cross-correlation ------------------

struct Hist1D {
  double bin = 1.0;
  std::int64_t base = 0;  // index 0 covers [base*bin, (base+1)*bin)
  std::vector<double> c;

  double norm() const {
    double s = 0.0;
    for (double v : c) s += v * v;
    return std::sqrt(s);
  }
};

void hist_build(const double* v, std::size_t stride, std::size_t n, double bin, Hist1D* out) {
  out->bin = bin;
  out->c.clear();
  if (n == 0) {
    out->base = 0;
    return;
  }
  double lo = v[0], hi = v[0];
  for (std::size_t i = 1; i < n; ++i) {
    const double x = v[i * stride];
    if (x < lo) lo = x;
    if (x > hi) hi = x;
  }
  const double inv = 1.0 / bin;
  out->base = static_cast<std::int64_t>(std::floor(lo * inv));
  const std::int64_t top = static_cast<std::int64_t>(std::floor(hi * inv));
  const std::int64_t span = top - out->base + 1;
  if (span <= 0 || span > 4'000'000) {  // a 1000 km extent: not a building
    out->c.clear();
    return;
  }
  out->c.assign(static_cast<std::size_t>(span), 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const std::int64_t k = static_cast<std::int64_t>(std::floor(v[i * stride] * inv)) - out->base;
    if (k >= 0 && k < span) out->c[static_cast<std::size_t>(k)] += 1.0;
  }
}

// Shift (in metres) that best aligns `src` onto `dst`, searched over
// +-max_shift. The peak is refined by fitting a parabola to its two
// neighbours, which recovers roughly a tenth of a bin and costs three
// multiplies — worth it because the bin is 25 cm and ICP's basin is not.
bool hist_best_shift(const Hist1D& src, const Hist1D& dst, double max_shift, double* shift_m,
                     double* score) {
  *shift_m = 0.0;
  *score = 0.0;
  if (src.c.empty() || dst.c.empty()) return false;
  const double ns = src.norm(), nd = dst.norm();
  if (!(ns > 0.0) || !(nd > 0.0)) return false;
  const std::int64_t off = src.base - dst.base;
  const std::int64_t d_max = static_cast<std::int64_t>(std::floor(max_shift / src.bin));
  const std::int64_t n_src = static_cast<std::int64_t>(src.c.size());
  const std::int64_t n_dst = static_cast<std::int64_t>(dst.c.size());

  std::int64_t best_d = 0;
  double best = -1.0;
  std::vector<double> scores(static_cast<std::size_t>(2 * d_max + 1), 0.0);
  for (std::int64_t d = -d_max; d <= d_max; ++d) {
    double acc = 0.0;
    // i ranges over the overlap of [0, n_src) with [-(off+d), n_dst-(off+d)).
    std::int64_t i0 = std::max<std::int64_t>(0, -(off + d));
    std::int64_t i1 = std::min<std::int64_t>(n_src, n_dst - (off + d));
    for (std::int64_t i = i0; i < i1; ++i) {
      acc += src.c[static_cast<std::size_t>(i)] * dst.c[static_cast<std::size_t>(i + off + d)];
    }
    const double s = acc / (ns * nd);
    scores[static_cast<std::size_t>(d + d_max)] = s;
    if (s > best) {
      best = s;
      best_d = d;
    }
  }
  double frac = 0.0;
  const std::size_t bi = static_cast<std::size_t>(best_d + d_max);
  if (bi > 0 && bi + 1 < scores.size()) {
    const double y0 = scores[bi - 1], y1 = scores[bi], y2 = scores[bi + 1];
    const double den = y0 - 2.0 * y1 + y2;
    if (std::fabs(den) > 1e-12) {
      frac = 0.5 * (y0 - y2) / den;
      if (frac > 0.5) frac = 0.5;
      if (frac < -0.5) frac = -0.5;
    }
  }
  *shift_m = (static_cast<double>(best_d) + frac) * src.bin;
  *score = best < 0.0 ? 0.0 : best;
  return true;
}

void stride_sample(const std::vector<PointVertex>& in, std::uint32_t cap,
                   std::vector<PointVertex>* out) {
  out->clear();
  if (in.empty()) return;
  if (cap == 0 || in.size() <= cap) {
    *out = in;
    return;
  }
  const std::size_t step = (in.size() + cap - 1) / cap;
  out->reserve(cap);
  for (std::size_t i = 0; i < in.size(); i += step) out->push_back(in[i]);
}

double wrap180(double deg) {
  while (deg > 180.0) deg -= 360.0;
  while (deg < -180.0) deg += 360.0;
  return deg;
}

}  // namespace

// --- 1. the georeferenced composition ---------------------------------------

Status enu_from_enu(const crs::EnuFrame& to, const crs::EnuFrame& from, double to_from[16]) {
  if (to_from == nullptr) return ScanError::kInvalidArgument;
  se3::mat4_identity(to_from);
  if (!to.valid || !from.valid) {
    return set_last_error(ScanError::kInvalidArgument,
                          "merge: enu_from_enu needs two valid ENU frames (to=%d from=%d)",
                          to.valid ? 1 : 0, from.valid ? 1 : 0);
  }
  // R = R_to^T R_from, t = R_to^T (o_from - o_to).
  double R[9];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += to.ecef_from_enu[k * 3 + i] * from.ecef_from_enu[k * 3 + j];
      R[i * 3 + j] = s;
    }
  }
  const double d[3] = {from.origin_ecef.x - to.origin_ecef.x, from.origin_ecef.y - to.origin_ecef.y,
                       from.origin_ecef.z - to.origin_ecef.z};
  double t[3];
  for (int i = 0; i < 3; ++i) {
    t[i] = to.ecef_from_enu[0 * 3 + i] * d[0] + to.ecef_from_enu[1 * 3 + i] * d[1] +
           to.ecef_from_enu[2 * 3 + i] * d[2];
  }
  se3::mat4_from_rt(R, t, to_from);
  return kOkStatus;
}

// --- 2. manual correspondences ----------------------------------------------

CorrespondenceSolution solve_correspondences(Span<const PointCorrespondence> picks,
                                             const CorrespondenceOptions& opts) {
  CorrespondenceSolution out;
  se3::mat4_identity(out.b_from_a);

  double wsum = 0.0;
  double ca[3] = {0.0, 0.0, 0.0}, cb[3] = {0.0, 0.0, 0.0};
  std::size_t used = 0;
  for (std::size_t i = 0; i < picks.size(); ++i) {
    const double w = picks[i].weight;
    if (!(w > 0.0)) continue;
    bool finite = true;
    for (int k = 0; k < 3; ++k) {
      if (!std::isfinite(picks[i].a[k]) || !std::isfinite(picks[i].b[k])) finite = false;
    }
    if (!finite) continue;
    for (int k = 0; k < 3; ++k) {
      ca[k] += w * picks[i].a[k];
      cb[k] += w * picks[i].b[k];
    }
    wsum += w;
    ++used;
  }
  out.pairs = used;
  if (used < opts.min_pairs || !(wsum > 0.0)) {
    out.blocker = "too few usable correspondences";
    return out;
  }
  for (int k = 0; k < 3; ++k) {
    ca[k] /= wsum;
    cb[k] /= wsum;
  }

  // S[i][j] = sum w a'_i b'_j ; C = sum w a' a'^T / sum w (the pick spread).
  double S[9] = {0.0};
  double C[9] = {0.0};
  double var_a = 0.0;
  for (std::size_t i = 0; i < picks.size(); ++i) {
    const double w = picks[i].weight;
    if (!(w > 0.0)) continue;
    const double da[3] = {picks[i].a[0] - ca[0], picks[i].a[1] - ca[1], picks[i].a[2] - ca[2]};
    const double db[3] = {picks[i].b[0] - cb[0], picks[i].b[1] - cb[1], picks[i].b[2] - cb[2]};
    if (!std::isfinite(da[0]) || !std::isfinite(db[0])) continue;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        S[r * 3 + c] += w * da[r] * db[c];
        C[r * 3 + c] += w * da[r] * da[c];
      }
    }
    var_a += w * (da[0] * da[0] + da[1] * da[1] + da[2] * da[2]);
  }
  for (int i = 0; i < 9; ++i) C[i] /= wsum;

  double cw[3], cv[9];
  sym_jacobi<3>(C, cw, cv);
  for (int i = 0; i < 3; ++i) out.spread_m[i] = cw[i] > 0.0 ? std::sqrt(cw[i]) : 0.0;
  // spread_m[0] is ~0 for any coplanar pick set (three points always are), so
  // the collinearity test is on spread_m[1].
  if (out.spread_m[1] < opts.min_spread_m ||
      out.spread_m[1] < opts.min_spread_ratio * out.spread_m[2]) {
    out.blocker = "correspondences are collinear";
    return out;
  }

  // Horn's 4x4. Eigenvector of the largest eigenvalue is the quaternion
  // (w, x, y, z) of the rotation carrying a into b.
  const double Sxx = S[0], Sxy = S[1], Sxz = S[2];
  const double Syx = S[3], Syy = S[4], Syz = S[5];
  const double Szx = S[6], Szy = S[7], Szz = S[8];
  const double N[16] = {
      Sxx + Syy + Szz, Syz - Szy,        Szx - Sxz,        Sxy - Syx,
      Syz - Szy,       Sxx - Syy - Szz,  Sxy + Syx,        Szx + Sxz,
      Szx - Sxz,       Sxy + Syx,        -Sxx + Syy - Szz, Syz + Szy,
      Sxy - Syx,       Szx + Sxz,        Syz + Szy,        -Sxx - Syy + Szz};
  double nw[4], nv[16];
  sym_jacobi<4>(N, nw, nv);
  // Largest eigenvalue is the last (ascending); its eigenvector is column 3.
  double q[4] = {nv[1 * 4 + 3], nv[2 * 4 + 3], nv[3 * 4 + 3], nv[0 * 4 + 3]};  // (x,y,z,w)
  if (!se3::quat_normalize(q)) {
    out.blocker = "degenerate rotation";
    return out;
  }
  double R[9];
  se3::quat_to_matrix(q, R);

  double tr = 0.0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) tr += R[i * 3 + j] * S[j * 3 + i];
  }
  out.implied_scale = var_a > 0.0 ? tr / var_a : 1.0;
  out.scale = opts.allow_scale ? out.implied_scale : 1.0;
  if (!(out.scale > 0.0) || !std::isfinite(out.scale)) {
    out.blocker = "non-positive scale";
    return out;
  }

  double M[9];
  for (int i = 0; i < 9; ++i) M[i] = out.scale * R[i];
  double t[3];
  for (int i = 0; i < 3; ++i) {
    t[i] = cb[i] - (M[i * 3 + 0] * ca[0] + M[i * 3 + 1] * ca[1] + M[i * 3 + 2] * ca[2]);
  }
  se3::mat4_from_rt(M, t, out.b_from_a);

  out.residuals_m.assign(picks.size(), 0.0);
  double sum2 = 0.0;
  double wtot = 0.0;
  for (std::size_t i = 0; i < picks.size(); ++i) {
    if (!(picks[i].weight > 0.0)) continue;
    double p[3];
    se3::mat4_apply(out.b_from_a, picks[i].a, p);
    const double d[3] = {p[0] - picks[i].b[0], p[1] - picks[i].b[1], p[2] - picks[i].b[2]};
    const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    out.residuals_m[i] = r;
    if (r > out.max_residual_m) out.max_residual_m = r;
    sum2 += picks[i].weight * r * r;
    wtot += picks[i].weight;
  }
  out.rms_m = wtot > 0.0 ? std::sqrt(sum2 / wtot) : 0.0;
  out.ok = true;
  out.blocker = "";
  return out;
}

// --- 3. the yaw + translation fallback --------------------------------------

YawSearchResult yaw_translation_search(Span<const PointVertex> source,
                                       Span<const PointVertex> target,
                                       const YawSearchConfig& cfg) {
  YawSearchResult out;
  se3::mat4_identity(out.b_from_a);
  if (source.size() == 0 || target.size() == 0) {
    out.blocker = "empty cloud";
    return out;
  }

  post::VoxelDedupConfig dcfg;
  dcfg.voxel_size_m = cfg.work_voxel_m > 0.0 ? cfg.work_voxel_m : 0.0;
  std::vector<PointVertex> src, tgt;
  if (dcfg.voxel_size_m > 0.0) {
    post::voxel_downsample(source, dcfg, &src);
    post::voxel_downsample(target, dcfg, &tgt);
  } else {
    src.assign(source.begin(), source.end());
    tgt.assign(target.begin(), target.end());
  }
  out.source_points = src.size();
  out.target_points = tgt.size();
  if (src.size() < 16 || tgt.size() < 16) {
    out.blocker = "too few points after downsampling";
    return out;
  }

  const double bin = cfg.hist_bin_m > 0.0 ? cfg.hist_bin_m : 0.25;
  // Target marginals, built once.
  std::vector<double> tx(tgt.size()), ty(tgt.size()), tz(tgt.size());
  for (std::size_t i = 0; i < tgt.size(); ++i) {
    tx[i] = tgt[i].x;
    ty[i] = tgt[i].y;
    tz[i] = tgt[i].z;
  }
  Hist1D htx, hty, htz;
  hist_build(tx.data(), 1, tx.size(), bin, &htx);
  hist_build(ty.data(), 1, ty.size(), bin, &hty);
  hist_build(tz.data(), 1, tz.size(), bin, &htz);

  // z is invariant under a yaw, so it is solved once.
  std::vector<double> sz(src.size());
  for (std::size_t i = 0; i < src.size(); ++i) sz[i] = src[i].z;
  Hist1D hsz;
  hist_build(sz.data(), 1, sz.size(), bin, &hsz);
  double dz = 0.0, sz_score = 0.0;
  hist_best_shift(hsz, htz, cfg.max_translation_m, &dz, &sz_score);

  // Occupancy of the target, for scoring.
  detail::VoxelSet occ(cfg.score_voxel_m > 0.0 ? cfg.score_voxel_m : 0.30);
  for (const auto& p : tgt) occ.insert(p.x, p.y, p.z);

  std::vector<PointVertex> sweep_samples, score_samples;
  stride_sample(src, std::min<std::uint32_t>(cfg.score_samples, 4000u), &sweep_samples);
  stride_sample(src, cfg.score_samples, &score_samples);

  const double step = cfg.yaw_step_deg > 0.0 ? cfg.yaw_step_deg : 2.0;
  const int n_yaw = static_cast<int>(std::floor((cfg.yaw_max_deg - cfg.yaw_min_deg) / step));
  if (n_yaw <= 0) {
    out.blocker = "empty yaw range";
    return out;
  }

  struct Candidate {
    double yaw_deg = 0.0;
    double t[3] = {0.0, 0.0, 0.0};
    double score = 0.0;
  };
  std::vector<Candidate> cands;
  cands.reserve(static_cast<std::size_t>(n_yaw));

  std::vector<double> rx(src.size()), ry(src.size());
  // Propose a translation for one yaw (three separable 1-D correlations) and
  // score it by real 3-D occupancy: the marginals only propose, they never
  // decide.
  auto eval_yaw = [&](double yaw, const std::vector<PointVertex>& samples, Candidate* cd) {
    const double c = std::cos(yaw * se3::kDegToRad), s = std::sin(yaw * se3::kDegToRad);
    for (std::size_t i = 0; i < src.size(); ++i) {
      rx[i] = c * src[i].x - s * src[i].y;
      ry[i] = s * src[i].x + c * src[i].y;
    }
    Hist1D hsx, hsy;
    hist_build(rx.data(), 1, rx.size(), bin, &hsx);
    hist_build(ry.data(), 1, ry.size(), bin, &hsy);
    double dx = 0.0, dy = 0.0, sx = 0.0, sy = 0.0;
    if (!hist_best_shift(hsx, htx, cfg.max_translation_m, &dx, &sx)) return false;
    if (!hist_best_shift(hsy, hty, cfg.max_translation_m, &dy, &sy)) return false;
    std::uint64_t hit = 0;
    for (const auto& pt : samples) {
      const double x = c * pt.x - s * pt.y + dx;
      const double y = s * pt.x + c * pt.y + dy;
      const double z = pt.z + dz;
      if (occ.near(x, y, z)) ++hit;
    }
    cd->yaw_deg = yaw;
    cd->t[0] = dx;
    cd->t[1] = dy;
    cd->t[2] = dz;
    cd->score =
        samples.empty() ? 0.0 : static_cast<double>(hit) / static_cast<double>(samples.size());
    return true;
  };

  for (int iy = 0; iy < n_yaw; ++iy) {
    Candidate cd;
    if (eval_yaw(cfg.yaw_min_deg + step * static_cast<double>(iy), sweep_samples, &cd)) {
      cands.push_back(cd);
    }
  }
  out.yaws_scored = static_cast<std::uint32_t>(cands.size());
  if (cands.empty()) {
    out.blocker = "no yaw candidate produced a translation";
    return out;
  }

  std::size_t best = 0;
  for (std::size_t i = 1; i < cands.size(); ++i) {
    if (cands[i].score > cands[best].score) best = i;
  }
  std::size_t runner = cands.size();
  for (std::size_t i = 0; i < cands.size(); ++i) {
    if (std::fabs(wrap180(cands[i].yaw_deg - cands[best].yaw_deg)) < cfg.distinct_yaw_deg) continue;
    if (runner == cands.size() || cands[i].score > cands[runner].score) runner = i;
  }

  // Refine the winning yaw below the sweep step. The sweep is 2 degrees, and
  // 2 degrees of yaw is 0.35 m of error at a 10 m lever arm — well outside
  // what a coarse alignment should hand to ICP when eight more scorings buy
  // it back.
  {
    Candidate refined = cands[best];
    const double base_yaw = refined.yaw_deg;
    for (int k = -4; k <= 4; ++k) {
      if (k == 0) continue;
      Candidate cd;
      if (!eval_yaw(base_yaw + step * (static_cast<double>(k) / 4.0), sweep_samples, &cd)) continue;
      if (cd.score > refined.score) refined = cd;
    }
    cands[best] = refined;
  }

  // Re-score the winner (and the runner-up) at full sample count.
  auto full_score = [&](const Candidate& cd) {
    const double c = std::cos(cd.yaw_deg * se3::kDegToRad);
    const double s = std::sin(cd.yaw_deg * se3::kDegToRad);
    std::uint64_t hit = 0;
    for (const auto& p : score_samples) {
      const double x = c * p.x - s * p.y + cd.t[0];
      const double y = s * p.x + c * p.y + cd.t[1];
      const double z = p.z + cd.t[2];
      if (occ.near(x, y, z)) ++hit;
    }
    return score_samples.empty()
               ? 0.0
               : static_cast<double>(hit) / static_cast<double>(score_samples.size());
  };
  out.overlap = full_score(cands[best]);
  out.yaw_deg = cands[best].yaw_deg;
  for (int i = 0; i < 3; ++i) out.translation[i] = cands[best].t[i];
  if (runner < cands.size()) {
    out.runner_up_overlap = full_score(cands[runner]);
    out.runner_up_yaw_deg = cands[runner].yaw_deg;
  }
  out.margin = out.overlap - out.runner_up_overlap;

  const double cy = std::cos(out.yaw_deg * se3::kDegToRad);
  const double sy2 = std::sin(out.yaw_deg * se3::kDegToRad);
  const double R[9] = {cy, -sy2, 0.0, sy2, cy, 0.0, 0.0, 0.0, 1.0};
  se3::mat4_from_rt(R, out.translation, out.b_from_a);

  if (out.overlap < cfg.min_overlap) {
    out.blocker = "overlap below threshold";
    return out;
  }
  if (out.margin < cfg.min_margin) {
    out.ambiguous = true;
    out.blocker = "ambiguous: a distinct yaw scores nearly as well";
    return out;
  }
  out.ok = true;
  out.blocker = "";
  return out;
}

}  // namespace merge
}  // namespace scanengine
