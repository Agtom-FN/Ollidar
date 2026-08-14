// colorize.h — camera colorization (§3.5).
//
// SEAM ONLY. Owner: A11 (depends on A5, A7 and the S6 calibration spike).
//
// S6 findings A11 must implement, not rediscover:
//   • Extrinsic calibration uses a PLANAR CHECKERBOARD (A1 0.80×0.60 m min,
//     ≥8 poses including roll variation). The quality gate is split-half
//     agreement, NOT solver covariance.
//   • Mid-360 colorization is GO at ≤5 ms sync jitter (≤15 ms with
//     motion-gated keyframe selection). D6 colorization needs a bench
//     calibration (~45 poses) and range noise ≤ ~10 mm 1σ.
//   • Sync jitter, not extrinsics, dominates the error budget (83% of the
//     reprojection budget at 15 ms). Required mitigations: constant
//     clock-offset estimation in the wizard (8 s sweep), a rolling-shutter
//     per-row time model, and motion-gated keyframe selection.
//   • Hardware time-sync is explicitly out of Phase 1.
//
// Capture is Android-only (CameraX sharing the ARCore session, keyframes at
// 2–5 fps into .lscan/streams/frames/); PROCESSING runs on any platform,
// which is why this seam lives in the shared engine.
#ifndef SCANENGINE_COLOR_COLORIZE_H
#define SCANENGINE_COLOR_COLORIZE_H

#include <cstdint>
#include <string>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/poses/pose_source.h"

namespace scanengine {

struct CameraIntrinsics {
  float fx = 0.f, fy = 0.f, cx = 0.f, cy = 0.f;
  float distortion[5] = {0.f, 0.f, 0.f, 0.f, 0.f};
  std::uint32_t width = 0, height = 0;
  float rolling_shutter_row_time_ns = 0.f;  // 0 = global shutter
};

struct Keyframe {
  std::int64_t t_mono_ns = 0;
  std::string image_path;      // relative to the .lscan root
  Pose pose;
  CameraIntrinsics intrinsics;
};

class Colorizer {
 public:
  virtual ~Colorizer() = default;
  virtual Status set_extrinsics(const double camera_from_lidar[16]) = 0;
  virtual Status add_keyframe(const Keyframe& kf) = 0;
  // Best-view selection (angle/distance/occlusion z-buffer), then RGB write.
  virtual Status colorize(PageStore* points) = 0;
  virtual float progress() const = 0;
};

// Mount/extrinsics wizard solver (S6 → A11).
class ExtrinsicsSolver {
 public:
  virtual ~ExtrinsicsSolver() = default;
  virtual Status add_observation(const Keyframe& kf, Span<const PointVertex> board_points) = 0;
  virtual Status solve(double camera_from_lidar[16]) = 0;
  // Split-half agreement in pixels — the S6-mandated quality gate.
  virtual float split_half_agreement_px() const = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_COLOR_COLORIZE_H
