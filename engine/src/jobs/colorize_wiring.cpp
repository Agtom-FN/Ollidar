#include "scanengine/jobs/colorize_wiring.h"

#include "scanengine/poses/se3.h"

namespace scanengine {
namespace jobs {

color::ColorizeConfig colorize_config_from(const ColorizeWiring& w,
                                           const color::ColorizeConfig& base) {
  color::ColorizeConfig cfg = base;
  if (w.timesync != nullptr) {
    // docs/A11-color.md §8.3's first line. A4 §7: consumers gate on
    // quality(), NEVER on jitter_ns, which is meaningless before convergence.
    cfg.sync_quality = w.timesync->quality(w.sync_stream);
  }
  cfg.allow_poor_sync = w.allow_poor_sync;
  cfg.camera_clock_offset_ns = w.camera_clock_offset_ns;
  return cfg;
}

Status wire_colorizer(const ColorizeWiring& w, color::PointColorizer* colorizer) {
  if (colorizer == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "jobs/colorize_wiring: null colorizer");
  }

  if (w.imu != nullptr) {
    ImuIngest* imu = w.imu;
    const std::int64_t window = w.imu_window_ns > 0 ? w.imu_window_ns : 250'000'000;
    colorizer->set_angular_rate_fn([imu, window](std::int64_t t, double* rad_per_s) {
      return imu->angular_rate_at(t, window, rad_per_s);
    });
  }

  if (w.trajectory != nullptr) {
    const PoseInterpolator* traj = w.trajectory;
    colorizer->set_pose_fn([traj](std::int64_t t, double world_from_camera[16]) {
      const PoseSample s = traj->sample_at(t);
      // `has_pose` and not `ok()`: a FLAGGED pose is still real geometry, and
      // refusing it here would silently drop the whole rolling-shutter
      // correction back to the constant-velocity fallback for exactly the
      // frames captured while the tracker was struggling — which are the
      // frames that need it most. A11 rejects tracking-lost KEYFRAMES
      // outright (colorizer.cpp's prepare_keyframes); this is a per-ROW pose
      // inside an already-accepted keyframe, ~20 ms away from it.
      if (!s.has_pose) return false;
      se3::mat4_from_quat_pos(s.pose.orientation, s.pose.position, world_from_camera);
      return true;
    });
  }

  return kOkStatus;
}

}  // namespace jobs
}  // namespace scanengine
