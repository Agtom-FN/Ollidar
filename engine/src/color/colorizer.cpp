#include "scanengine/color/colorizer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

namespace scanengine {
namespace color {

namespace {

constexpr const char* kMod = "color";
constexpr double kDegToRad = se3::kDegToRad;
constexpr double kRadToDeg = se3::kRadToDeg;

inline double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// --- 3x3 symmetric eigen (cyclic Jacobi) ------------------------------------
//
// Used only for the surface normal: the eigenvector of the smallest
// eigenvalue of a neighbourhood's covariance. Jacobi rather than the
// closed-form cubic because the closed form loses its digits exactly where a
// point cloud lives — on a near-perfect plane, where two eigenvalues are
// equal and the third is ~1e-9 of them.
void smallest_eigenvector_sym3(const double c[9], double out[3]) {
  double a[9];
  std::memcpy(a, c, sizeof(a));
  double v[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (int sweep = 0; sweep < 12; ++sweep) {
    double off = 0.0;
    for (int p = 0; p < 3; ++p) {
      for (int q = p + 1; q < 3; ++q) off += a[p * 3 + q] * a[p * 3 + q];
    }
    if (off < 1e-30) break;
    for (int p = 0; p < 3; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        const double apq = a[p * 3 + q];
        if (std::fabs(apq) < 1e-32) continue;
        const double theta = (a[q * 3 + q] - a[p * 3 + p]) / (2.0 * apq);
        const double t =
            (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double cs = 1.0 / std::sqrt(t * t + 1.0);
        const double sn = t * cs;
        for (int k = 0; k < 3; ++k) {
          const double akp = a[k * 3 + p], akq = a[k * 3 + q];
          a[k * 3 + p] = cs * akp - sn * akq;
          a[k * 3 + q] = sn * akp + cs * akq;
        }
        for (int k = 0; k < 3; ++k) {
          const double apk = a[p * 3 + k], aqk = a[q * 3 + k];
          a[p * 3 + k] = cs * apk - sn * aqk;
          a[q * 3 + k] = sn * apk + cs * aqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = v[k * 3 + p], vkq = v[k * 3 + q];
          v[k * 3 + p] = cs * vkp - sn * vkq;
          v[k * 3 + q] = sn * vkp + cs * vkq;
        }
      }
    }
  }
  int best = 0;
  double best_val = a[0];
  for (int i = 1; i < 3; ++i) {
    if (a[i * 3 + i] < best_val) {
      best_val = a[i * 3 + i];
      best = i;
    }
  }
  out[0] = v[0 * 3 + best];
  out[1] = v[1 * 3 + best];
  out[2] = v[2 * 3 + best];
  se3::normalize3(out);
}

}  // namespace

const char* to_string(ColorCoverage c) noexcept {
  switch (c) {
    case ColorCoverage::kNone: return "none";
    case ColorCoverage::kColorized: return "colorized";
    case ColorCoverage::kLowConfidence: return "low-confidence";
  }
  return "unknown";
}

const char* to_string(ColorStage s) noexcept {
  switch (s) {
    case ColorStage::kIdle: return "idle";
    case ColorStage::kPreparing: return "preparing";
    case ColorStage::kNormals: return "estimating normals";
    case ColorStage::kSelecting: return "selecting keyframes";
    case ColorStage::kDone: return "done";
    case ColorStage::kCancelled: return "cancelled";
    case ColorStage::kFailed: return "failed";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------

struct PointColorizer::Impl {
  // --- a flat view over one or more point buffers ---------------------------
  //
  // A PageStore holds points in per-page buffers that never move; a caller
  // with a std::vector has one buffer. Everything below indexes points
  // globally, so both look the same: sequential passes walk the chunks, and
  // the (rarer) random access in the normal estimator binary-searches the
  // chunk table, which is at most `max_pages` (64) entries long.
  struct PointRefs {
    struct Chunk {
      PointVertex* data = nullptr;
      std::size_t count = 0;
      std::size_t first = 0;
    };
    std::vector<Chunk> chunks;
    std::size_t total = 0;

    void add(PointVertex* data, std::size_t count) {
      if (data == nullptr || count == 0) return;
      chunks.push_back(Chunk{data, count, total});
      total += count;
    }
    PointVertex& at(std::size_t i) const {
      std::size_t lo = 0, hi = chunks.size();
      while (lo + 1 < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (chunks[mid].first <= i) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      return chunks[lo].data[i - chunks[lo].first];
    }
  };

  // --- per-keyframe derived state -------------------------------------------
  struct KfState {
    double world_from_cam[16] = {0};
    double cam_from_world[16] = {0};
    double cam_pos[3] = {0, 0, 0};
    // Constant-velocity model for the rolling shutter, in the WORLD frame.
    double omega[3] = {0, 0, 0};  // rad/s
    double vel[3] = {0, 0, 0};    // m/s
    double rate_deg_s = 0.0;
    bool rate_known = false;
    double motion_score = 1.0;
    bool gated = false;  // above the gate: usable, but low confidence
    bool skip = false;
    std::int64_t t_ns = 0;  // clock-offset corrected
    double row_time_s = 0.0;
  };

  ColorizeConfig cfg;
  ColorizeStats stats;
  ColorizationPolicy policy;
  std::vector<Keyframe> keyframes;
  std::vector<KfState> kfs;
  std::vector<ColorCoverage> coverage;
  double camera_from_lidar[16];
  bool have_extrinsics = false;

  ImageSource* images = nullptr;
  std::unique_ptr<FileImageSource> owned_images;
  AngularRateFn rate_fn;
  PoseAtFn pose_fn;
  ColorProgressFn progress_cb;
  post::CancelToken own_token;
  post::CancelToken* token = nullptr;

  std::atomic<float> progress{0.f};
  std::atomic<ColorStage> stage{ColorStage::kIdle};

  // Working buffers, sized to the point count.
  std::vector<float> best_score;
  std::vector<std::uint8_t> best_rgb;    // 3 per point
  std::vector<std::uint8_t> best_gated;  // 1 per point
  std::vector<float> normals;            // 3 per point; (0,0,0) = unknown
  std::vector<float> zbuf;

  Impl() {
    se3::mat4_identity(camera_from_lidar);
    token = &own_token;
  }

  bool cancelled() const { return post::cancelled(token); }

  void report(ColorStage s, float overall, float within, std::uint64_t done,
              std::uint64_t total) {
    stage.store(s, std::memory_order_relaxed);
    progress.store(overall, std::memory_order_relaxed);
    if (!progress_cb) return;
    ColorProgress p;
    p.stage = s;
    p.label = to_string(s);
    p.fraction = overall;
    p.stage_fraction = within;
    p.done = done;
    p.total = total;
    progress_cb(p);
  }

  // --- geometry -------------------------------------------------------------

  // Camera pose `dt` seconds after the keyframe stamp, under the
  // constant-velocity model (used when the caller supplied no trajectory).
  void pose_at_dt(const KfState& k, double dt, double cam_from_world[16]) const {
    if (dt == 0.0) {
      std::memcpy(cam_from_world, k.cam_from_world, sizeof(double) * 16);
      return;
    }
    const double w[3] = {k.omega[0] * dt, k.omega[1] * dt, k.omega[2] * dt};
    double dR[9];
    se3::so3_exp(w, dR);
    double R0[9], t0[3];
    se3::mat4_get_rt(k.world_from_cam, R0, t0);
    // world_from_cam(t) = exp(w·dt) · world_from_cam(0), with the position
    // advanced by the linear velocity: a rigid body turning as it travels.
    double R[9];
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        double s = 0.0;
        for (int q = 0; q < 3; ++q) s += dR[r * 3 + q] * R0[q * 3 + c];
        R[r * 3 + c] = s;
      }
    }
    const double t[3] = {t0[0] + k.vel[0] * dt, t0[1] + k.vel[1] * dt, t0[2] + k.vel[2] * dt};
    double m[16];
    se3::mat4_from_rt(R, t, m);
    se3::mat4_inverse_rigid(m, cam_from_world);
  }

  // Pinhole + Brown–Conrady forward projection. False when behind the camera.
  static bool project_cam(const CameraIntrinsics& in, const double pc[3], double* u, double* v) {
    if (!(pc[2] > 1e-6)) return false;
    const double xn = pc[0] / pc[2];
    const double yn = pc[1] / pc[2];
    const double k1 = in.distortion[0], k2 = in.distortion[1];
    const double p1 = in.distortion[2], p2 = in.distortion[3], k3 = in.distortion[4];
    double xd = xn, yd = yn;
    if (k1 != 0.f || k2 != 0.f || p1 != 0.f || p2 != 0.f || k3 != 0.f) {
      const double r2 = xn * xn + yn * yn;
      const double radial = 1.0 + r2 * (k1 + r2 * (k2 + r2 * k3));
      xd = xn * radial + 2.0 * p1 * xn * yn + p2 * (r2 + 2.0 * xn * xn);
      yd = yn * radial + p1 * (r2 + 2.0 * yn * yn) + 2.0 * p2 * xn * yn;
    }
    *u = in.fx * xd + in.cx;
    *v = in.fy * yd + in.cy;
    return true;
  }

  // World point → keyframe pixel, with the rolling-shutter fixed point.
  // `depth` is along the optical axis (what the z-buffer stores); `range` is
  // the euclidean distance (what the distance term scores).
  bool project(const KfState& k, const Keyframe& kf, const double pw[3], double* u, double* v,
               double* depth, double* range) const {
    double cfw[16];
    std::memcpy(cfw, k.cam_from_world, sizeof(cfw));
    double pc[3];
    se3::mat4_apply(cfw, pw, pc);
    if (!project_cam(kf.intrinsics, pc, u, v)) return false;

    if (cfg.rolling_shutter && k.row_time_s != 0.0) {
      const std::uint32_t iters = std::max(1u, cfg.rolling_shutter_iterations);
      const double hmax = static_cast<double>(kf.intrinsics.height);
      for (std::uint32_t it = 0; it < iters; ++it) {
        // Row 0 is exposed at the keyframe stamp; row v, `v · row_time` later.
        double row = *v;
        if (row < 0.0) row = 0.0;
        if (row > hmax) row = hmax;
        const double dt = row * k.row_time_s;
        if (pose_fn) {
          double wfc[16];
          const std::int64_t t = k.t_ns + static_cast<std::int64_t>(std::llround(dt * 1e9));
          if (!pose_fn(t, wfc)) break;
          se3::mat4_inverse_rigid(wfc, cfw);
        } else {
          pose_at_dt(k, dt, cfw);
        }
        se3::mat4_apply(cfw, pw, pc);
        double u2 = 0.0, v2 = 0.0;
        if (!project_cam(kf.intrinsics, pc, &u2, &v2)) return false;
        const double moved = std::fabs(v2 - *v);
        *u = u2;
        *v = v2;
        if (moved < 0.01) break;  // a hundredth of a row: converged
      }
    }
    *depth = pc[2];
    *range = std::sqrt(pc[0] * pc[0] + pc[1] * pc[1] + pc[2] * pc[2]);
    return *depth > 1e-6;
  }

  // --- the run --------------------------------------------------------------
  Status prepare_keyframes();
  void estimate_normals(const PointRefs& pts);
  Status run(const PointRefs& pts);
};

// ---------------------------------------------------------------------------

PointColorizer::PointColorizer(const ColorizeConfig& cfg) : impl_(new Impl) { impl_->cfg = cfg; }
PointColorizer::~PointColorizer() = default;

Status PointColorizer::set_extrinsics(const double camera_from_lidar[16]) {
  if (camera_from_lidar == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: set_extrinsics(null)");
  }
  if (!se3::mat4_is_rigid(camera_from_lidar)) {
    // A8 §4.4: the two ways this fails in the field are a column-major matrix
    // handed across JNI and an uninitialised buffer. Both otherwise produce a
    // plausible-looking but mirrored result nobody notices until export.
    return set_last_error(ScanError::kInvalidArgument,
                          "color: camera_from_lidar is not a rigid transform "
                          "(row-major 4x4 expected)");
  }
  std::memcpy(impl_->camera_from_lidar, camera_from_lidar, sizeof(double) * 16);
  impl_->have_extrinsics = true;
  return kOkStatus;
}

Status PointColorizer::add_keyframe(const Keyframe& kf) {
  SCAN_TRY(validate_keyframe(kf));
  impl_->keyframes.push_back(kf);
  return kOkStatus;
}

Status PointColorizer::load_keyframes(const std::string& lscan_dir, FrameIndexStats* stats) {
  std::vector<Keyframe> kfs;
  SCAN_TRY(read_frame_index(lscan_dir, &kfs, stats));
  if (kfs.empty()) {
    return set_last_error(ScanError::kNotFound, "color: frames.idx in '%s' holds no keyframes",
                          lscan_dir.c_str());
  }
  impl_->keyframes = std::move(kfs);
  if (impl_->images == nullptr) {
    impl_->owned_images.reset(new FileImageSource(lscan_dir));
    impl_->images = impl_->owned_images.get();
  }
  return kOkStatus;
}

void PointColorizer::set_image_source(ImageSource* src) { impl_->images = src; }
void PointColorizer::set_angular_rate_fn(AngularRateFn fn) { impl_->rate_fn = std::move(fn); }
void PointColorizer::set_pose_fn(PoseAtFn fn) { impl_->pose_fn = std::move(fn); }
void PointColorizer::set_cancel_token(post::CancelToken* token) {
  impl_->token = (token != nullptr) ? token : &impl_->own_token;
}
void PointColorizer::set_progress_callback(ColorProgressFn cb) {
  impl_->progress_cb = std::move(cb);
}
void PointColorizer::cancel() { impl_->own_token.cancel(); }

float PointColorizer::progress() const { return impl_->progress.load(std::memory_order_relaxed); }
ColorStage PointColorizer::stage() const { return impl_->stage.load(std::memory_order_relaxed); }
const ColorizeStats& PointColorizer::stats() const { return impl_->stats; }
const ColorizeConfig& PointColorizer::config() const { return impl_->cfg; }
const std::vector<Keyframe>& PointColorizer::keyframes() const { return impl_->keyframes; }
const ColorizationPolicy& PointColorizer::policy() const { return impl_->policy; }
Span<const ColorCoverage> PointColorizer::coverage() const {
  return Span<const ColorCoverage>(impl_->coverage.data(), impl_->coverage.size());
}

Status PointColorizer::colorize_points(Span<PointVertex> points) {
  Impl::PointRefs refs;
  refs.add(points.data(), points.size());
  return impl_->run(refs);
}

Status PointColorizer::colorize(PageStore* points) {
  if (points == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: colorize(null store)");
  }
  Impl::PointRefs refs;
  for (const PageId id : points->page_ids()) {
    const PageView pv = points->page_view(id);
    if (!pv.valid() || pv.count == 0) continue;
    // The store hands out const views because its normal traffic is
    // append-then-render. Colorization is the one producer that REWRITES
    // points that already exist, and §3.5 defines it as exactly that ("RGB
    // into final cloud"). The buffer is non-const inside the store and never
    // reallocates, so writing through the view is defined; only the r/g/b
    // (and optionally a) bytes change, so no bound, count, page id or time
    // range is invalidated. docs/A11-color.md §7 asks cloud/ for a
    // `page_data_mutable()` accessor to make this explicit rather than
    // implicit.
    refs.add(const_cast<PointVertex*>(pv.data), pv.count);
  }
  if (refs.total == 0) {
    return set_last_error(ScanError::kInvalidArgument, "color: the page store holds no points");
  }
  return impl_->run(refs);
}

// ---------------------------------------------------------------------------

Status PointColorizer::Impl::prepare_keyframes() {
  const std::size_t n = keyframes.size();
  kfs.assign(n, KfState{});
  stats.keyframes_total = static_cast<std::uint32_t>(n);

  for (std::size_t i = 0; i < n; ++i) {
    const Keyframe& kf = keyframes[i];
    KfState& k = kfs[i];
    k.t_ns = kf.t_mono_ns + cfg.camera_clock_offset_ns;
    k.row_time_s = static_cast<double>(kf.intrinsics.rolling_shutter_row_time_ns) * 1e-9;

    double m[16];
    se3::mat4_from_quat_pos(kf.pose.orientation, kf.pose.position, m);
    if (cfg.pose_frame == KeyframePoseFrame::kLidarBody) {
      // world_from_camera = world_from_body · inverse(camera_from_lidar)
      double lidar_from_camera[16];
      se3::mat4_inverse_rigid(camera_from_lidar, lidar_from_camera);
      se3::mat4_mul(m, lidar_from_camera, m);
    }
    std::memcpy(k.world_from_cam, m, sizeof(m));
    se3::mat4_inverse_rigid(m, k.cam_from_world);
    k.cam_pos[0] = m[3];
    k.cam_pos[1] = m[7];
    k.cam_pos[2] = m[11];

    // Tracking-loss keyframes are refused outright: their pose is not a pose.
    if (kf.pose.tracking_lost != 0 || kf.pose.quality == PoseQuality::kInvalid) {
      k.skip = true;
      ++stats.keyframes_rejected_pose;
      continue;
    }

    // Motion gate — the caller's rate function first (A4's ImuIngest is the
    // production one), then whatever the capture side recorded.
    double rad_s = 0.0;
    if (rate_fn && rate_fn(k.t_ns, &rad_s)) {
      k.rate_known = true;
    } else if (kf.has_motion()) {
      rad_s = kf.angular_rate_rad_s;
      k.rate_known = true;
    }
    k.rate_deg_s = rad_s * kRadToDeg;
    const double gate = policy.motion_gate_deg_s;
    const double reject = policy.motion_reject_deg_s;
    if (k.rate_known && k.rate_deg_s > reject) {
      k.skip = true;
      ++stats.keyframes_rejected_motion;
      continue;
    }
    if (!k.rate_known || k.rate_deg_s <= gate) {
      k.motion_score = 1.0;
      k.gated = false;
    } else {
      // Falls off as gate/rate: at twice the gate a frame scores half, which
      // with w_motion = 2 is a quarter of the selection weight.
      k.motion_score = gate / std::max(k.rate_deg_s, 1e-6);
      k.gated = true;
    }
  }

  // Constant-velocity model for the rolling shutter, from the neighbouring
  // keyframes. Only needed when no trajectory function was supplied.
  if (!pose_fn) {
    for (std::size_t i = 0; i < n; ++i) {
      KfState& k = kfs[i];
      if (k.row_time_s == 0.0) continue;
      const std::size_t a = (i > 0) ? i - 1 : i;
      const std::size_t b = (i + 1 < n) ? i + 1 : i;
      if (a == b) continue;
      const double dt = static_cast<double>(kfs[b].t_ns - kfs[a].t_ns) * 1e-9;
      if (!(dt > 1e-6)) continue;
      for (int c = 0; c < 3; ++c) {
        k.vel[c] = (kfs[b].cam_pos[c] - kfs[a].cam_pos[c]) / dt;
      }
      double Ra[9], Rb[9], ta[3], tb[3];
      se3::mat4_get_rt(kfs[a].world_from_cam, Ra, ta);
      se3::mat4_get_rt(kfs[b].world_from_cam, Rb, tb);
      // Rb = exp(w·dt) · Ra  ⇒  w = log(Rb · Raᵀ) / dt
      double RbRaT[9];
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          double s = 0.0;
          for (int q = 0; q < 3; ++q) s += Rb[r * 3 + q] * Ra[c * 3 + q];
          RbRaT[r * 3 + c] = s;
        }
      }
      double w[3];
      se3::so3_log(RbRaT, w);
      for (int c = 0; c < 3; ++c) k.omega[c] = w[c] / dt;
    }
  }
  return kOkStatus;
}

void PointColorizer::Impl::estimate_normals(const PointRefs& pts) {
  const double cell = std::max(1e-3, static_cast<double>(cfg.normal_radius_m));
  const double r2max = cell * cell;
  const std::size_t n = pts.total;

  struct Cell {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
  };
  auto key_at = [](std::int64_t ix, std::int64_t iy, std::int64_t iz) {
    // 21 bits per axis, biased: a ±1e6-cell world, which at a 15 cm cell is
    // ±150 km. A handheld scan cannot collide across that.
    const std::uint64_t ux = static_cast<std::uint64_t>(ix + 1048576) & 0x1FFFFF;
    const std::uint64_t uy = static_cast<std::uint64_t>(iy + 1048576) & 0x1FFFFF;
    const std::uint64_t uz = static_cast<std::uint64_t>(iz + 1048576) & 0x1FFFFF;
    return (ux << 42) | (uy << 21) | uz;
  };

  std::vector<std::uint64_t> keys(n);
  std::vector<std::uint32_t> order(n);
  for (std::size_t i = 0; i < n; ++i) {
    const PointVertex& p = pts.at(i);
    keys[i] = key_at(static_cast<std::int64_t>(std::floor(p.x / cell)),
                     static_cast<std::int64_t>(std::floor(p.y / cell)),
                     static_cast<std::int64_t>(std::floor(p.z / cell)));
    order[i] = static_cast<std::uint32_t>(i);
  }
  std::sort(order.begin(), order.end(), [&keys](std::uint32_t a, std::uint32_t b) {
    // Index as the tiebreak keeps the sort a total order, so the neighbour
    // list — and therefore every normal — is identical run to run.
    return keys[a] != keys[b] ? keys[a] < keys[b] : a < b;
  });
  std::unordered_map<std::uint64_t, Cell> grid;
  grid.reserve(n / 2 + 8);
  for (std::size_t i = 0; i < n;) {
    const std::uint64_t k = keys[order[i]];
    std::size_t j = i;
    while (j < n && keys[order[j]] == k) ++j;
    Cell c;
    c.first = static_cast<std::uint32_t>(i);
    c.count = static_cast<std::uint32_t>(j - i);
    grid.emplace(k, c);
    i = j;
  }

  normals.assign(n * 3, 0.f);
  std::vector<double> nbr;
  nbr.reserve(192);
  const std::uint32_t interval = std::max(1u, cfg.progress_point_interval);
  for (std::size_t i = 0; i < n; ++i) {
    if ((i % interval) == 0) {
      if (cancelled()) return;
      const float f = static_cast<float>(i) / static_cast<float>(std::max<std::size_t>(1, n));
      report(ColorStage::kNormals, 0.05f + 0.15f * f, f, i, n);
    }
    const PointVertex& p = pts.at(i);
    const std::int64_t ix = static_cast<std::int64_t>(std::floor(p.x / cell));
    const std::int64_t iy = static_cast<std::int64_t>(std::floor(p.y / cell));
    const std::int64_t iz = static_cast<std::int64_t>(std::floor(p.z / cell));
    nbr.clear();
    double mean[3] = {0, 0, 0};
    std::size_t count = 0;
    for (int dx = -1; dx <= 1; ++dx) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
          const auto it = grid.find(key_at(ix + dx, iy + dy, iz + dz));
          if (it == grid.end()) continue;
          for (std::uint32_t q = 0; q < it->second.count; ++q) {
            const PointVertex& o = pts.at(order[it->second.first + q]);
            const double ddx = static_cast<double>(o.x) - p.x;
            const double ddy = static_cast<double>(o.y) - p.y;
            const double ddz = static_cast<double>(o.z) - p.z;
            if (ddx * ddx + ddy * ddy + ddz * ddz > r2max) continue;
            nbr.push_back(o.x);
            nbr.push_back(o.y);
            nbr.push_back(o.z);
            mean[0] += o.x;
            mean[1] += o.y;
            mean[2] += o.z;
            ++count;
          }
        }
      }
    }
    if (count < cfg.normal_min_neighbors) {
      ++stats.normals_missing;
      continue;
    }
    for (int c = 0; c < 3; ++c) mean[c] /= static_cast<double>(count);
    double cov[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t q = 0; q < count; ++q) {
      const double dv[3] = {nbr[q * 3 + 0] - mean[0], nbr[q * 3 + 1] - mean[1],
                            nbr[q * 3 + 2] - mean[2]};
      for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) cov[a * 3 + b] += dv[a] * dv[b];
      }
    }
    double nrm[3];
    smallest_eigenvector_sym3(cov, nrm);
    normals[i * 3 + 0] = static_cast<float>(nrm[0]);
    normals[i * 3 + 1] = static_cast<float>(nrm[1]);
    normals[i * 3 + 2] = static_cast<float>(nrm[2]);
    ++stats.normals_estimated;
  }
}

Status PointColorizer::Impl::run(const PointRefs& pts) {
  const double t_start = now_ms();
  stats = ColorizeStats{};
  stats.points_total = pts.total;
  stage.store(ColorStage::kPreparing, std::memory_order_relaxed);
  progress.store(0.f, std::memory_order_relaxed);

  if (pts.total == 0) {
    stage.store(ColorStage::kFailed, std::memory_order_relaxed);
    return set_last_error(ScanError::kInvalidArgument, "color: nothing to colorize");
  }
  if (keyframes.empty()) {
    stage.store(ColorStage::kFailed, std::memory_order_relaxed);
    return set_last_error(ScanError::kNotFound,
                          "color: no keyframes — a desktop-captured session has no camera");
  }
  if (images == nullptr) {
    stage.store(ColorStage::kFailed, std::memory_order_relaxed);
    return set_last_error(ScanError::kInvalidState,
                          "color: no image source (call load_keyframes() or set_image_source())");
  }

  // --- the S6 go/no-go, before any work ------------------------------------
  policy = policy_for(cfg.sync_quality, cfg.allow_poor_sync);
  if (cfg.motion_gate_deg_s > 0.f) policy.motion_gate_deg_s = cfg.motion_gate_deg_s;
  if (cfg.motion_reject_deg_s > 0.f) policy.motion_reject_deg_s = cfg.motion_reject_deg_s;
  if (!policy.colorize) {
    stage.store(ColorStage::kFailed, std::memory_order_relaxed);
    return set_last_error(ScanError::kNotSupported, "color: %s", policy.reason);
  }
  if (cancelled()) {
    stage.store(ColorStage::kCancelled, std::memory_order_relaxed);
    return ScanError::kCancelled;
  }

  SCAN_TRY(prepare_keyframes());
  report(ColorStage::kPreparing, 0.05f, 1.f, kfs.size(), kfs.size());

  // Cloud bounds, for the per-keyframe frustum reject.
  double lo[3] = {1e300, 1e300, 1e300};
  double hi[3] = {-1e300, -1e300, -1e300};
  for (const auto& ch : pts.chunks) {
    for (std::size_t i = 0; i < ch.count; ++i) {
      const PointVertex& p = ch.data[i];
      lo[0] = std::min<double>(lo[0], p.x);
      lo[1] = std::min<double>(lo[1], p.y);
      lo[2] = std::min<double>(lo[2], p.z);
      hi[0] = std::max<double>(hi[0], p.x);
      hi[1] = std::max<double>(hi[1], p.y);
      hi[2] = std::max<double>(hi[2], p.z);
    }
  }
  double centre[3], radius = 0.0;
  for (int c = 0; c < 3; ++c) {
    centre[c] = 0.5 * (lo[c] + hi[c]);
    radius += (hi[c] - lo[c]) * (hi[c] - lo[c]);
  }
  radius = 0.5 * std::sqrt(radius);

  // --- normals --------------------------------------------------------------
  normals.clear();
  if (cfg.estimate_normals) {
    const double t0 = now_ms();
    report(ColorStage::kNormals, 0.05f, 0.f, 0, pts.total);
    estimate_normals(pts);
    stats.ms_normals = now_ms() - t0;
    if (cancelled()) {
      stage.store(ColorStage::kCancelled, std::memory_order_relaxed);
      return ScanError::kCancelled;
    }
  }

  // --- the best-view competition -------------------------------------------
  best_score.assign(pts.total, 0.f);
  best_rgb.assign(pts.total * 3, 0);
  best_gated.assign(pts.total, 0);
  coverage.assign(pts.total, ColorCoverage::kNone);

  const double cos_max_inc = std::cos(static_cast<double>(cfg.max_incidence_deg) * kDegToRad);
  const double dist_ref = std::max(1e-3, static_cast<double>(cfg.distance_ref_m));
  const std::uint32_t interval = std::max(1u, cfg.progress_point_interval);
  DecodedImage image;
  const std::size_t nkf = kfs.size();

  for (std::size_t ki = 0; ki < nkf; ++ki) {
    if (cancelled()) {
      stage.store(ColorStage::kCancelled, std::memory_order_relaxed);
      return ScanError::kCancelled;
    }
    const float base = 0.20f + 0.80f * static_cast<float>(ki) / static_cast<float>(nkf);
    report(ColorStage::kSelecting, base, static_cast<float>(ki) / static_cast<float>(nkf), ki,
           nkf);

    KfState& k = kfs[ki];
    if (k.skip) continue;
    const Keyframe& kf = keyframes[ki];

    // Cheap frustum reject: is the cloud's bounding sphere anywhere in front
    // of this camera, and within range?
    {
      double c_cam[3];
      se3::mat4_apply(k.cam_from_world, centre, c_cam);
      const double dist =
          std::sqrt(c_cam[0] * c_cam[0] + c_cam[1] * c_cam[1] + c_cam[2] * c_cam[2]);
      if (c_cam[2] < -radius || dist - radius > cfg.max_range_m) {
        ++stats.keyframes_outside_cloud;
        continue;
      }
    }

    const double t_dec = now_ms();
    image.clear();
    const Status img = images->load(kf, &image);
    stats.ms_decode += now_ms() - t_dec;
    if (!img.ok() || !image.valid()) {
      ++stats.keyframes_image_failed;
      SCAN_LOG_WARN(kMod, "keyframe '%s' skipped: %s", kf.image_path.c_str(),
                    error_str(img.error()));
      continue;
    }

    // --- z-buffer -----------------------------------------------------------
    const double scale = std::min(1.0, std::max(0.01, static_cast<double>(cfg.depth_scale)));
    const std::uint32_t zw = std::max(
        1u, static_cast<std::uint32_t>(std::lround(kf.intrinsics.width * scale)));
    const std::uint32_t zh = std::max(
        1u, static_cast<std::uint32_t>(std::lround(kf.intrinsics.height * scale)));
    if (cfg.occlusion_test) {
      const double t_z = now_ms();
      zbuf.assign(static_cast<std::size_t>(zw) * zh, std::numeric_limits<float>::infinity());
      const int rad = static_cast<int>(cfg.splat_radius_px);
      for (const auto& ch : pts.chunks) {
        for (std::size_t i = 0; i < ch.count; ++i) {
          const PointVertex& p = ch.data[i];
          const double pw[3] = {p.x, p.y, p.z};
          double u, v, depth, range;
          if (!project(k, kf, pw, &u, &v, &depth, &range)) continue;
          if (range < cfg.min_range_m || range > cfg.max_range_m) continue;
          const int zx = static_cast<int>(std::floor(u * scale));
          const int zy = static_cast<int>(std::floor(v * scale));
          for (int dy = -rad; dy <= rad; ++dy) {
            const int yy = zy + dy;
            if (yy < 0 || yy >= static_cast<int>(zh)) continue;
            for (int dx = -rad; dx <= rad; ++dx) {
              const int xx = zx + dx;
              if (xx < 0 || xx >= static_cast<int>(zw)) continue;
              float& z = zbuf[static_cast<std::size_t>(yy) * zw + static_cast<std::size_t>(xx)];
              if (static_cast<float>(depth) < z) z = static_cast<float>(depth);
            }
          }
        }
      }
      stats.ms_depth += now_ms() - t_z;
    }

    // --- competition --------------------------------------------------------
    const double t_sel = now_ms();
    const double margin = static_cast<double>(cfg.edge_margin_px);
    const double w_lim = static_cast<double>(kf.intrinsics.width) - margin;
    const double h_lim = static_cast<double>(kf.intrinsics.height) - margin;
    bool used = false;
    std::size_t gi = 0;
    for (const auto& ch : pts.chunks) {
      for (std::size_t i = 0; i < ch.count; ++i, ++gi) {
        if ((gi % interval) == 0 && cancelled()) {
          stage.store(ColorStage::kCancelled, std::memory_order_relaxed);
          return ScanError::kCancelled;
        }
        const PointVertex& p = ch.data[i];
        const double pw[3] = {p.x, p.y, p.z};
        double u, v, depth, range;
        if (!project(k, kf, pw, &u, &v, &depth, &range)) {
          ++stats.rejected_behind;
          continue;
        }
        if (u < margin || v < margin || u >= w_lim || v >= h_lim) {
          ++stats.rejected_outside;
          continue;
        }
        if (range < cfg.min_range_m || range > cfg.max_range_m) {
          ++stats.rejected_range;
          continue;
        }

        // Incidence: the angle between the surface normal and the ray back to
        // the camera. |cos| because a cloud's normals have no orientation.
        double cos_inc = 1.0;
        if (!normals.empty()) {
          const double nx = normals[gi * 3 + 0];
          const double ny = normals[gi * 3 + 1];
          const double nz = normals[gi * 3 + 2];
          if (nx != 0.0 || ny != 0.0 || nz != 0.0) {
            double dir[3] = {k.cam_pos[0] - pw[0], k.cam_pos[1] - pw[1], k.cam_pos[2] - pw[2]};
            if (se3::normalize3(dir)) {
              cos_inc = std::fabs(nx * dir[0] + ny * dir[1] + nz * dir[2]);
              if (cos_inc > 1.0) cos_inc = 1.0;
            }
          }
        }
        if (cos_inc < cos_max_inc) {
          ++stats.rejected_incidence;
          continue;
        }

        if (cfg.occlusion_test) {
          int zx = static_cast<int>(std::floor(u * scale));
          int zy = static_cast<int>(std::floor(v * scale));
          zx = std::min<int>(std::max(zx, 0), static_cast<int>(zw) - 1);
          zy = std::min<int>(std::max(zy, 0), static_cast<int>(zh) - 1);
          const double front =
              zbuf[static_cast<std::size_t>(zy) * zw + static_cast<std::size_t>(zx)];
          double tol = cfg.depth_tolerance_m + cfg.depth_relative_tolerance * depth;
          if (cfg.depth_slope_bias > 0.f && cos_inc < 0.999) {
            // Half the cell (plus its splat) times the surface's slope: the
            // depth spread the buffer cannot resolve at this obliquity.
            const double cell_px =
                (static_cast<double>(cfg.splat_radius_px) + 0.5) / std::max(0.01, scale);
            const double cell_m = depth * cell_px / std::max(1e-6f, kf.intrinsics.fx);
            const double slope = std::min(20.0, std::sqrt(1.0 - cos_inc * cos_inc) /
                                                    std::max(1e-3, cos_inc));
            tol += cfg.depth_slope_bias * cell_m * slope;
          }
          if (depth > front + tol) {
            ++stats.rejected_occluded;
            continue;
          }
        }

        const double s_dist = dist_ref / (dist_ref + range);
        const double score = std::pow(cos_inc, static_cast<double>(cfg.w_incidence)) *
                             std::pow(s_dist, static_cast<double>(cfg.w_distance)) *
                             std::pow(k.motion_score, static_cast<double>(cfg.w_motion));
        if (!(score > 0.0)) continue;
        ++stats.accepted_samples;
        // Strictly greater: an exact tie keeps the EARLIER keyframe, which is
        // what makes the result independent of anything but the input order.
        if (static_cast<float>(score) <= best_score[gi]) continue;
        std::uint8_t rgb[3];
        image.sample_bilinear(u, v, rgb);
        best_score[gi] = static_cast<float>(score);
        best_rgb[gi * 3 + 0] = rgb[0];
        best_rgb[gi * 3 + 1] = rgb[1];
        best_rgb[gi * 3 + 2] = rgb[2];
        best_gated[gi] = k.gated ? 1u : 0u;
        used = true;
      }
    }
    stats.ms_select += now_ms() - t_sel;
    if (used) ++stats.keyframes_used;
  }

  // --- write ----------------------------------------------------------------
  std::size_t gi = 0;
  for (const auto& ch : pts.chunks) {
    for (std::size_t i = 0; i < ch.count; ++i, ++gi) {
      PointVertex& p = ch.data[i];
      if (best_score[gi] <= 0.f) {
        ++stats.points_uncovered;
        coverage[gi] = ColorCoverage::kNone;
        if (cfg.uncovered_alpha != 0) p.a = cfg.uncovered_alpha;
        continue;
      }
      p.r = best_rgb[gi * 3 + 0];
      p.g = best_rgb[gi * 3 + 1];
      p.b = best_rgb[gi * 3 + 2];
      ++stats.points_colorized;
      if (best_gated[gi] != 0) {
        ++stats.points_low_confidence;
        coverage[gi] = ColorCoverage::kLowConfidence;
        if (cfg.low_confidence_alpha != 0) p.a = cfg.low_confidence_alpha;
      } else {
        coverage[gi] = ColorCoverage::kColorized;
      }
    }
  }

  stats.ms_total = now_ms() - t_start;
  report(ColorStage::kDone, 1.f, 1.f, pts.total, pts.total);
  SCAN_LOG_INFO(kMod,
                "colorized %llu/%llu points (%.1f%%) from %u/%u keyframes, %llu low-confidence, "
                "%.0f ms",
                static_cast<unsigned long long>(stats.points_colorized),
                static_cast<unsigned long long>(stats.points_total),
                100.0 * static_cast<double>(stats.coverage_fraction()), stats.keyframes_used,
                stats.keyframes_total,
                static_cast<unsigned long long>(stats.points_low_confidence), stats.ms_total);
  return kOkStatus;
}

}  // namespace color
}  // namespace scanengine
