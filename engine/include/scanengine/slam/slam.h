// slam.h — SLAM pipeline seams (Tech Spec §3.3).
//
// SEAM ONLY. Owners:
//   A6  live/  — Mid-360 ESKF lidar-inertial odometry (Point-LIO/FAST-LIO2
//                family): IMU propagation @200 Hz, iterated update against an
//                incremental voxel map (iVox), scan-to-map @10 Hz, input
//                decimated to ~40k pts/s live, ≤ 2 big cores on Android.
//   A7  post/  — full-density LIO re-run → Scan Context loop candidates →
//                GTSAM pose-graph optimization → re-integration → voxel
//                dedup/outlier filter. Cancellable, progress-reported.
//   A8  pushbroom/ — D6 profile assembler + mount-extrinsics solver. S6
//                finding: calibration uses a PLANAR CHECKERBOARD target;
//                corner/doorframe capture is geometrically unusable for a 2D
//                scanner. Points captured during ARCore tracking loss are
//                flagged and excluded by default.
//
// Everything here runs off the engine's own threads (A6 owns its odometry
// thread; A7 runs as a job under jobs/), consumes poses through
// poses/PoseSource, and publishes results into cloud/PageStore. GTSAM and
// Ceres arrive as vcpkg dependencies at A7 — see vcpkg.json's commented
// placeholders and the S7 note that each new port must be re-verified
// against the macOS universal overlay triplet.
#ifndef SCANENGINE_SLAM_SLAM_H
#define SCANENGINE_SLAM_SLAM_H

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/poses/pose_source.h"

namespace scanengine {

struct ImuSample {
  std::int64_t t_mono_ns = 0;
  float accel_mps2[3] = {0.f, 0.f, 0.f};
  float gyro_radps[3] = {0.f, 0.f, 0.f};
};

// Live odometry (A6). Points in, pose out, map into the PageStore.
class LiveOdometry : public PoseSource {
 public:
  virtual Status push_imu(const ImuSample& s) = 0;
  virtual Status push_points(Span<const PointVertex> points, std::int64_t t_mono_ns) = 0;
  virtual double cpu_budget_used() const = 0;  // 0..1 of the configured budget
};

// Post-processing pipeline (A7). Runs as a job; see jobs/job.h.
class PostPipeline {
 public:
  virtual ~PostPipeline() = default;
  virtual Status run(const std::string& lscan_dir) = 0;
  virtual float progress() const = 0;
  virtual void cancel() = 0;
};

// D6 pushbroom assembler (A8): profile points + trajectory → 3-D cloud.
class PushbroomAssembler {
 public:
  virtual ~PushbroomAssembler() = default;
  virtual Status set_mount_extrinsics(const double transform[16]) = 0;
  virtual Status push_profile(Span<const PointVertex> profile, std::int64_t t_mono_ns) = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_SLAM_SLAM_H
