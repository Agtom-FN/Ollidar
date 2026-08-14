// colorize.h — camera colorization (§3.5): the shared vocabulary.
//
// Owner: A11 (depends on A5, A7, A8 and the S6 calibration spike).
//
// This header carries the TYPES every colorization consumer speaks
// (`CameraIntrinsics`, `Keyframe`) plus the two A1 seams (`Colorizer`,
// `ExtrinsicsSolver`). The A11 implementation lives beside it:
//
//   color/frames_idx.h   the `streams/frames/frames.idx` binary format —
//                        THE format B8's capture path writes and the
//                        colorizer reads (encode/decode/validate + a
//                        standalone writer and a tolerant reader).
//   color/image_source.h JPEG/PNG keyframe decoding (vendored stb_image).
//   color/colorizer.h    `PointColorizer` — best-view selection, z-buffer
//                        occlusion, rolling-shutter projection, RGB sampling.
//   color/clock_sweep.h  the wizard's 8-second constant-clock-offset
//                        estimator and the S6 sync-quality policy.
//
// S6 findings A11 implements, rather than rediscovers:
//   • Extrinsic calibration uses a PLANAR CHECKERBOARD (A1 0.80×0.60 m min,
//     ≥8 poses including roll variation). The quality gate is split-half
//     agreement, NOT solver covariance. A8 shipped that solver
//     (`slam/pushbroom/mount_calibration.h`); A11 wraps it, per A8 §6.
//   • Mid-360 colorization is GO at ≤5 ms sync jitter (≤15 ms with
//     motion-gated keyframe selection). D6 colorization needs a bench
//     calibration (~45 poses) and range noise ≤ ~10 mm 1σ.
//   • Sync jitter, not extrinsics, dominates the error budget (83% of the
//     reprojection budget at 15 ms). Required mitigations: constant
//     clock-offset estimation in the wizard (8 s sweep — `clock_sweep.h`),
//     a rolling-shutter per-row time model (`colorizer.h`), and
//     motion-gated keyframe selection (`colorizer.h`).
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

// Pinhole + Brown–Conrady, in the layout ARCore's `CameraIntrinsics` and
// OpenCV both hand out. `distortion` is (k1, k2, p1, p2, k3) — the OpenCV
// order — and the projector applies the FORWARD model (world → pixel), which
// is the direction those coefficients are defined in.
//
// `rolling_shutter_row_time_ns` is the per-row readout delay: image row `r`
// is exposed at `t_mono_ns + r * rolling_shutter_row_time_ns`. 0 means a
// global shutter. S6 §6 measures this term at 6.8 px of the 20.2 px budget
// — "correctable, currently unmodelled" — and A11 corrects it.
struct CameraIntrinsics {
  float fx = 0.f, fy = 0.f, cx = 0.f, cy = 0.f;
  float distortion[5] = {0.f, 0.f, 0.f, 0.f, 0.f};
  std::uint32_t width = 0, height = 0;
  float rolling_shutter_row_time_ns = 0.f;  // 0 = global shutter
};

// Keyframe flags, mirrored bit-for-bit into the frames.idx record (see
// color/frames_idx.h). Stable and append-only: a shipped .lscan carries them.
inline constexpr std::uint32_t kKeyframeFlagMotionValid = 1u << 0;
inline constexpr std::uint32_t kKeyframeFlagExposureValid = 1u << 1;
inline constexpr std::uint32_t kKeyframeFlagTrackingLost = 1u << 2;
inline constexpr std::uint32_t kKeyframeFlagAutoExposureLocked = 1u << 3;

// One recorded camera keyframe.
//
// `t_mono_ns` is the engine clock (A4's domain — the same `t_mono_ns` every
// other stream is stamped in), and it is the exposure time of **image row 0**,
// so the rolling-shutter model above needs no extra reference point.
//
// `pose` is `world_from_camera`: ARCore's camera pose in the session's local
// metric frame. A8 §3.1 established that the phone frame and the camera frame
// are one frame, so this is the same trajectory the pushbroom assembler and
// the LIO put points into. When a caller's keyframe poses are instead
// `world_from_lidar_body`, `ColorizeConfig::pose_frame` says so and the
// colorizer composes the mount extrinsic itself.
struct Keyframe {
  std::int64_t t_mono_ns = 0;
  std::string image_path;      // relative to the .lscan root
  Pose pose;
  CameraIntrinsics intrinsics;

  // --- capture metadata (A11; recorded by B8, optional per `flags`) -------
  std::uint32_t flags = 0;
  std::int64_t exposure_duration_ns = 0;  // valid iff kKeyframeFlagExposureValid
  float iso = 0.f;                        // valid iff kKeyframeFlagExposureValid
  // The rig's motion AT CAPTURE TIME, as the capture side measured it
  // (ARCore, or the Mid-360's own 200 Hz IMU through A4's ImuIngest). This is
  // what makes S6 §6.3's motion gate work on any platform from a .lscan
  // alone, with no IMU stream to re-integrate. Valid iff
  // kKeyframeFlagMotionValid; a colorizer given an angular-rate function
  // prefers that function's answer.
  float angular_rate_rad_s = 0.f;
  float linear_speed_m_s = 0.f;
  // Size of the JPEG on disk, for a cheap integrity check at read time.
  std::uint32_t image_bytes = 0;

  bool has_motion() const { return (flags & kKeyframeFlagMotionValid) != 0; }
  bool has_exposure() const { return (flags & kKeyframeFlagExposureValid) != 0; }
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
//
// A8 shipped the solver this seam describes — `MountCalibrationSolver` in
// `slam/pushbroom/mount_calibration.h`, with the split-half gate — and A8 §6
// asks A11 to WRAP it rather than reimplement it: turn a keyframe's
// checkerboard detection into the plane `(n, d)` the solver's residual wants
// and delegate. `split_half_agreement_px()` is `MountCalibResult::split_half_px`.
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
