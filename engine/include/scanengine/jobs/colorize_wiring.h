// colorize_wiring.h — docs/A11-color.md §8.3's one-liners, in one place.
//
// INT-34.
//
// A11 shipped a colorizer that is deliberately decoupled from everything that
// feeds it: the sync gate arrives as a `SyncQuality` value, the motion gate
// as an `AngularRateFn` callable, the rolling-shutter trajectory as a
// `PoseAtFn` callable, and the clock offset as a plain integer. That is the
// right shape — it is what lets `tests/test_color.cpp` drive the gate
// analytically and what keeps `color/` from linking against A4 or A7 — but it
// left four connections that A11 §8.3 says "are one line each and neither is
// wired by anything today".
//
// This header is where those lines live, once, instead of being retyped in
// the CLI, in B6's Android service and in the Qt processing panel:
//
//   cfg.sync_quality              ← TimeSync::quality(stream)          (A4)
//   cfg.camera_clock_offset_ns    ← the wizard's sweep / the manifest   (A11)
//   set_angular_rate_fn(...)      ← ImuIngest::angular_rate_at(...)     (A4)
//   set_pose_fn(...)              ← a PoseInterpolator, in production
//                                   PostSlamPipeline::trajectory()      (A7)
//
// FAILING CLOSED IS THE POINT. `ColorizeConfig::sync_quality` defaults to
// `kUnknown`, and `policy_for(kUnknown)` refuses — so a caller who forgets
// A4 gets `kNotSupported` rather than a silently mis-registered cloud (A11
// §2). Passing a null `timesync` here therefore leaves the refusal in place
// on purpose; it does not quietly assume `kGood`.
//
// LIFETIME. Every pointer is BORROWED and must outlive the colorize() call
// the wiring is applied to — the two callables capture them. That is the same
// contract `PostConfig::store` and `ColorizeParams::colorizer` already carry.
//
// Owner: INT-34.
#ifndef SCANENGINE_JOBS_COLORIZE_WIRING_H
#define SCANENGINE_JOBS_COLORIZE_WIRING_H

#include <cstdint>

#include "scanengine/color/colorizer.h"
#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/timesync/imu_ingest.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {
namespace jobs {

struct ColorizeWiring {
  // --- A4 → A11: the go/no-go gate ---------------------------------------
  // Null leaves ColorizeConfig::sync_quality at whatever the base config
  // says, which defaults to kUnknown, which refuses. That is deliberate.
  const TimeSync* timesync = nullptr;
  // Which stream's convergence decides. The lidar's, because that is the
  // clock the points are stamped in and the one the camera is being aligned
  // to (S6 §6.1).
  StreamId sync_stream = StreamId::kLidarMid360;
  // The operator override behind which S6's 30 ms row sits — see
  // color/clock_sweep.h's policy_for().
  bool allow_poor_sync = false;

  // --- A11 → A11: the wizard's constant offset ---------------------------
  // t_engine_ns = t_camera_ns + camera_clock_offset_ns. Produced by
  // color::estimate_clock_offset(), persisted by
  // lscan::FileRecordWriter::add_clock_offset() (INT-34), and applied here.
  std::int64_t camera_clock_offset_ns = 0;

  // --- A4 → A11: the motion gate -----------------------------------------
  // Null falls back to the per-keyframe `angular_rate_rad_s` recorded in
  // frames.idx, which is what makes colorization work from a .lscan alone on
  // a desktop (A11 §3.3 item 4). A supplied IMU OUTRANKS the recorded rate.
  ImuIngest* imu = nullptr;
  // Window the mean |gyro| is taken over, ending at the keyframe's stamp.
  // 250 ms is A11's own documented example and covers a 2-5 fps keyframe
  // cadence without smearing a turn into a straight.
  std::int64_t imu_window_ns = 250'000'000;

  // --- A7 → A11: the rolling-shutter trajectory --------------------------
  // Null leaves the colorizer on its keyframe-difference constant-velocity
  // fallback, which A11 §7.2 measures as matching the exact trajectory to
  // 1 % over a 20 ms readout. In production this is
  // PostSlamPipeline::trajectory().
  const PoseInterpolator* trajectory = nullptr;
};

// Returns `base` with the two CONFIG fields filled in. Everything else is
// left exactly as the caller set it, so a caller's motion-gate override,
// depth_scale, weights and alpha policy survive.
color::ColorizeConfig colorize_config_from(const ColorizeWiring& w,
                                           const color::ColorizeConfig& base =
                                               color::ColorizeConfig());

// Installs the two CALLABLES on a concrete colorizer. Config fields are not
// touched here — a PointColorizer's config is fixed at construction, so build
// it from colorize_config_from() first and then call this.
//
// kInvalidArgument on a null colorizer. Installing nothing (both pointers
// null) is not an error: the colorizer's own fallbacks are legitimate.
Status wire_colorizer(const ColorizeWiring& w, color::PointColorizer* colorizer);

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_COLORIZE_WIRING_H
