// slam.h — SLAM pipeline seams (Tech Spec §3.3).
//
// SEAM ONLY (except where a later task has landed the real thing). Owners:
//   A6  live/  — Mid-360 ESKF lidar-inertial odometry (Point-LIO/FAST-LIO2
//                family): IMU propagation @200 Hz, iterated update against an
//                incremental voxel map (iVox), scan-to-map @10 Hz, input
//                decimated to ~40k pts/s live, ≤ 2 big cores on Android.
//                **LANDED, in slam/lio.h + slam/eskf.h + slam/ivox.h** as
//                LioOdometry / LioPoseSource / Eskf / IVox. See
//                docs/A6-lio.md. `LiveOdometry` below is kept as the abstract
//                seam and is NOT what A6 ships — see the ImuSample note.
//
// ================ ImuSample: A NAME COLLISION, DELIBERATELY LEFT ============
//
// This header declares `scanengine::ImuSample`, and so does
// timesync/imu_ingest.h (A4) — with different fields. Including both in one
// translation unit does not compile, and the two definitions coexisting in
// one program is a formal ODR violation. That state predates A6: A4 added its
// ImuSample without retiring this one, and tests/test_timesync.cpp and
// tests/test_headers.cpp have been linking the two into `scanengine_tests`
// ever since.
//
// A6 could not fix it without editing a file it does not own: the fix is to
// delete the struct below and update the one line in tests/test_headers.cpp
// that reads `ImuSample imu; CHECK(imu.t_mono_ns == 0);`. So A6 instead
// SIDESTEPPED it — slam/lio.h takes IMU through primitive arguments
// (`push_imu(t_engine_ns, gyro[3], accel[3])`) and includes neither this
// header nor imu_ingest.h, so nothing A6 ships adds a third party to the
// clash or forces a caller to pick a side.
//
// ACTION FOR A1/A4: delete `ImuSample` from this file, keep the one in
// timesync/imu_ingest.h (it is the one with a real producer), and fix that
// one test line. It is a five-minute change that this comment exists only
// because A6 was not allowed to make.
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

// Live odometry (A6) — THE ABSTRACT SEAM, not the implementation. A6 ships
// `LioOdometry` in slam/lio.h, which deliberately does not derive from this:
// `push_imu` here takes the ImuSample above, and binding the shipped class to
// that type would drag the collision documented at the top of this file into
// every consumer. Retire this class in favour of slam/lio.h once the
// ImuSample question is settled, or re-declare it over the timesync sample.
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
