#include "scanengine/color/extrinsics_solver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "scanengine/core/log.h"

namespace scanengine {
namespace color {
namespace {

constexpr const char* kMod = "color";

bool finite3(const double v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

}  // namespace

struct MountExtrinsicsSolver::Impl {
  ExtrinsicsSolverConfig cfg;
  MountCalibrationSolver solver;
  std::vector<BoardDetection> pending;
  MountCalibResult result{};
  bool solved = false;

  // The camera the gate is measured in, fixed by the first keyframe that
  // carried usable intrinsics.
  bool camera_fixed = false;
  CameraIntrinsics camera{};

  explicit Impl(const ExtrinsicsSolverConfig& c) : cfg(c), solver(c.calib) {}
};

MountExtrinsicsSolver::MountExtrinsicsSolver(const ExtrinsicsSolverConfig& cfg)
    : impl_(new Impl(cfg)) {}

MountExtrinsicsSolver::~MountExtrinsicsSolver() = default;

Status MountExtrinsicsSolver::add_detection(const BoardDetection& det) {
  if (!finite3(det.normal) || !std::isfinite(det.d)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "ExtrinsicsSolver: detection at %lld ns has a non-finite plane",
                          static_cast<long long>(det.t_engine_ns));
  }
  const double n2 = det.normal[0] * det.normal[0] + det.normal[1] * det.normal[1] +
                    det.normal[2] * det.normal[2];
  if (std::fabs(n2 - 1.0) > 1e-6) {
    return set_last_error(ScanError::kInvalidArgument,
                          "ExtrinsicsSolver: detection at %lld ns has a normal of length %.6f "
                          "(it must be a UNIT normal in the camera frame)",
                          static_cast<long long>(det.t_engine_ns), std::sqrt(n2));
  }
  if (det.d <= 0.0) {
    return set_last_error(ScanError::kInvalidArgument,
                          "ExtrinsicsSolver: detection at %lld ns has d = %.4f; the board is in "
                          "front of the camera, so d must be positive metres",
                          static_cast<long long>(det.t_engine_ns), det.d);
  }
  for (BoardDetection& p : impl_->pending) {
    if (p.t_engine_ns == det.t_engine_ns) {
      p = det;  // one frame, one detection
      return kOkStatus;
    }
  }
  impl_->pending.push_back(det);
  return kOkStatus;
}

std::size_t MountExtrinsicsSolver::pending_detections() const { return impl_->pending.size(); }

Status MountExtrinsicsSolver::add_observation(const Keyframe& kf,
                                              Span<const PointVertex> board_points) {
  std::size_t best = impl_->pending.size();
  std::int64_t best_dt = 0;
  for (std::size_t i = 0; i < impl_->pending.size(); ++i) {
    const std::int64_t dt = std::llabs(impl_->pending[i].t_engine_ns - kf.t_mono_ns);
    if (dt <= impl_->cfg.match_tolerance_ns && (best == impl_->pending.size() || dt < best_dt)) {
      best = i;
      best_dt = dt;
    }
  }
  if (best == impl_->pending.size()) {
    return set_last_error(ScanError::kNotFound,
                          "ExtrinsicsSolver: no board detection within %lld ns of keyframe "
                          "'%s' (t = %lld) — the detector found no target in that frame, or its "
                          "stamps are not on the engine clock",
                          static_cast<long long>(impl_->cfg.match_tolerance_ns),
                          kf.image_path.c_str(), static_cast<long long>(kf.t_mono_ns));
  }
  const BoardDetection det = impl_->pending[best];
  const Status st = add_observation(kf, det, board_points);
  // Consumed only on success: a rejected pair must stay visible to the wizard
  // (and re-addable with better lidar returns) rather than vanishing.
  if (st.ok()) {
    impl_->pending.erase(impl_->pending.begin() + static_cast<std::ptrdiff_t>(best));
  }
  return st;
}

Status MountExtrinsicsSolver::add_observation(const Keyframe& kf, const BoardDetection& det,
                                              Span<const PointVertex> board_points) {
  // Fix (or check) the camera the gate will be measured in.
  const CameraIntrinsics& in = kf.intrinsics;
  const bool usable = in.fx > 0.f && in.fy > 0.f && in.width > 0 && in.height > 0;
  if (impl_->cfg.camera_from_keyframes && usable) {
    if (!impl_->camera_fixed) {
      impl_->camera = in;
      impl_->camera_fixed = true;
      MountCalibConfig c = impl_->solver.config();
      c.camera.fx = in.fx;
      c.camera.fy = in.fy;
      c.camera.cx = in.cx;
      c.camera.cy = in.cy;
      c.camera.width = in.width;
      c.camera.height = in.height;
      impl_->solver.set_config(c);
      impl_->cfg.calib = c;
      SCAN_LOG_INFO(kMod,
                    "extrinsics solver: gate camera fixed from keyframe intrinsics "
                    "(%ux%u, fx %.1f)",
                    in.width, in.height, static_cast<double>(in.fx));
    } else {
      const CameraIntrinsics& c0 = impl_->camera;
      const bool same = c0.width == in.width && c0.height == in.height &&
                        std::fabs(c0.fx - in.fx) <= 0.01f * c0.fx &&
                        std::fabs(c0.fy - in.fy) <= 0.01f * c0.fy;
      if (!same) {
        return set_last_error(ScanError::kInvalidArgument,
                              "ExtrinsicsSolver: keyframe '%s' is %ux%u at fx %.1f but this "
                              "calibration was started at %ux%u fx %.1f. The split-half gate is "
                              "quoted in PIXELS at a range, so mixing cameras silently rescales "
                              "the verdict — calibrate one camera at a time.",
                              kf.image_path.c_str(), in.width, in.height,
                              static_cast<double>(in.fx), c0.width, c0.height,
                              static_cast<double>(c0.fx));
      }
    }
  }

  BoardDetection d = det;
  if (!(d.sigma_m > 0.0)) d.sigma_m = impl_->cfg.default_sigma_m;
  if (!finite3(d.normal) || !std::isfinite(d.d) || d.d <= 0.0) {
    return set_last_error(ScanError::kInvalidArgument,
                          "ExtrinsicsSolver: keyframe '%s' was paired with an invalid plane",
                          kf.image_path.c_str());
  }
  // A8 validates the rest (unit normal, positive sigma, point count) and says
  // which rule failed; there is no reason to duplicate its messages.
  return impl_->solver.add_observation(d.normal, d.d, board_points, d.sigma_m);
}

Status MountExtrinsicsSolver::solve(double camera_from_lidar[16]) {
  if (camera_from_lidar == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "ExtrinsicsSolver::solve: null output");
  }
  if (impl_->solver.observation_count() == 0) {
    return set_last_error(ScanError::kInvalidState,
                          "ExtrinsicsSolver::solve: no observations (add_observation() first; "
                          "A8 wants at least %zu for a non-degenerate plane-only solve)",
                          impl_->solver.config().min_observations);
  }
  auto r = impl_->solver.solve(impl_->cfg.cad_camera_from_lidar);
  if (!r.ok()) return r.status();
  impl_->result = r.value();
  impl_->solved = true;
  std::memcpy(camera_from_lidar, impl_->result.camera_from_lidar,
              sizeof(impl_->result.camera_from_lidar));
  return kOkStatus;
}

float MountExtrinsicsSolver::split_half_agreement_px() const {
  if (!impl_->solved) return -1.f;
  return static_cast<float>(impl_->result.split_half_px);
}

double MountExtrinsicsSolver::split_half_mm_at(double range_m) const {
  if (!impl_->solved || impl_->result.split_half_px < 0.0 || !(range_m > 0.0)) return -1.0;
  const double fx = impl_->solver.config().camera.fx;
  if (!(fx > 0.0)) return -1.0;
  // A8 §6: px * range / fx, in metres; WIZARD.md wants millimetres.
  return impl_->result.split_half_px * range_m / fx * 1000.0;
}

const MountCalibResult& MountExtrinsicsSolver::result() const { return impl_->result; }

CalibGate MountExtrinsicsSolver::gate() const {
  return impl_->solved ? impl_->result.gate : CalibGate::kUnknown;
}

std::size_t MountExtrinsicsSolver::observation_count() const {
  return impl_->solver.observation_count();
}

const MountCalibrationSolver& MountExtrinsicsSolver::solver() const { return impl_->solver; }

const ExtrinsicsSolverConfig& MountExtrinsicsSolver::config() const { return impl_->cfg; }

void MountExtrinsicsSolver::clear() {
  impl_->solver.clear();
  impl_->pending.clear();
  impl_->result = MountCalibResult{};
  impl_->solved = false;
  impl_->camera_fixed = false;
  impl_->camera = CameraIntrinsics{};
}

}  // namespace color
}  // namespace scanengine
