// lio.h — live lidar-inertial odometry for the Mid-360 (task A6).
//
// Tech Spec §3.3, "Mid-360 live": ESKF lidar-inertial odometry of the
// Point-LIO / FAST-LIO2 family — IMU propagation at 200 Hz, an iterated
// update against an incremental voxel map, scan-to-map at 10 Hz, input
// decimated to ~40k pts/s, and a budget of two big cores on Android.
//
// Two objects, one job:
//
//   LioOdometry     the pipeline. Points in (slam-agnostic PointVertex, the
//                   same buffer the Mid-360 driver appends to the PageStore),
//                   IMU in (already mapped to engine time by A4), and out
//                   come poses plus a registered world-frame cloud.
//   LioPoseSource   the trajectory, as a poses/PoseSource. Everything
//                   downstream — the pushbroom assembler, the colorization
//                   projector, A10's fusion — consumes the LIO through this
//                   and cannot tell it apart from ARCore or RTK. That is
//                   Tech Spec §3 key rule 3, and it is why A6 adds no new
//                   trajectory interface of its own.
//
// --- how it is wired ------------------------------------------------------
//
//   Mid360Driver ──points──▶ PageStore(kLidarMid360) ──▶ push_points()
//                └──IMU────▶ ImuIngest (A4 mapped time) ──▶ push_imu()
//                                                              │
//                                            LioOdometry ──────┤
//                                              ├─ poses()   ──▶ LioPoseSource
//                                              └─ map_store() ▶ PageStore  ← the app renders this
//
// The driver hands out points in the LIDAR frame in metres and does no
// geometry (docs/A3-mid360-driver.md §4). A6 applies the lidar->IMU
// extrinsic, the motion undistortion and the world transform, in that order.
//
// --- determinism ----------------------------------------------------------
//
// Identical input produces an identical trajectory and an identical map, bit
// for bit, on a given build. "Identical input" means the same points, the
// same IMU samples, AND the same batch boundaries: push_points() timestamps a
// batch, and per-point times are interpolated across the batch, so splitting
// one 8192-point call into two 4096-point calls is a different input. The
// driver batches deterministically (A3), so a replay of the same bytes
// reproduces the same batches and therefore the same map.
//
// Everything else that could introduce nondeterminism is excluded by
// construction: no randomness, no time-based decimation, no unordered
// container iteration on any path that affects a result, no parallel
// reduction, and no dependence on how many IMU samples happen to be buffered
// when a scan closes (a scan is processed only once IMU covers its whole
// span).
//
// Owner: A6.
#ifndef SCANENGINE_SLAM_LIO_H
#define SCANENGINE_SLAM_LIO_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/poses/pose_source.h"
#include "scanengine/slam/eskf.h"
#include "scanengine/slam/ivox.h"

namespace scanengine {

// --- the trajectory -------------------------------------------------------

// A PoseSource backed by a bounded ring of poses, with interpolated lookup.
// Usable standalone (a test, or a replay of a recorded trajectory);
// LioOdometry owns one and pushes into it once per scan.
//
// Threading: every method is safe from any thread. The callback runs INLINE
// on the pushing thread — the engine-wide rule (DESIGN §2) — which for the
// LIO means "on the odometry thread", so a subscriber must be quick and must
// not re-enter the odometry.
class LioPoseSource final : public PoseSource {
 public:
  // `capacity` poses, oldest evicted. 36,000 is one hour at 10 Hz; a pose is
  // 56 bytes, so the ring costs 2 MB and pose_at() can always answer for the
  // whole session rather than only for the recent past.
  explicit LioPoseSource(std::size_t capacity = 36000,
                         StreamId stream = StreamId::kLidarMid360);
  ~LioPoseSource() override;

  const char* name() const override;
  StreamId stream() const override;
  Status start() override;
  Status stop() override;
  bool running() const override;

  // Poses must arrive in non-decreasing time order; an out-of-order pose is
  // rejected with kInvalidArgument rather than silently corrupting the
  // binary search that pose_at() depends on.
  Status push_pose(const Pose& pose) override;
  void set_callback(PoseCallback cb) override;

  // Linear position / shortest-arc quaternion interpolation. kNotFound before
  // the first pose (or if the ring has already evicted `t`), kAgain after the
  // newest one.
  Status pose_at(std::int64_t t_mono_ns, Pose* out) const override;

  bool latest(Pose* out) const;
  std::size_t size() const;
  void clear();

  // Sum of |p_i - p_{i-1}| over every pose ever pushed — not just the ones
  // still resident, so it stays honest after the ring wraps.
  double trajectory_length_m() const;

 private:
  mutable std::mutex m_;
  std::deque<Pose> poses_;
  std::size_t capacity_;
  StreamId stream_;
  PoseCallback cb_;
  bool running_ = false;
  bool have_last_ = false;
  double last_p_[3] = {0, 0, 0};
  double length_m_ = 0.0;
};

// --- the odometry ---------------------------------------------------------

struct LioConfig {
  // --- scan segmentation ------------------------------------------------
  // §3.3's "scan-to-map @10 Hz". A scan is closed when IMU time has passed
  // its end, so a scan is never processed on partial IMU coverage.
  double scan_period_s = 0.1;

  // --- input decimation (§3.3's "~40k pts/s live") ----------------------
  // 0 disables. The stride is recomputed once per scan from the PREVIOUS
  // scan's arrival count, which makes it deterministic (a wall-clock rate
  // estimate would not be) and makes it a no-op when the driver has already
  // decimated to the same budget.
  std::uint32_t live_points_per_sec = 40000;
  // Hard ceiling per scan, whatever the stride says. Protects the update
  // from a burst; 3x the nominal 4,000 is generous.
  std::uint32_t max_points_per_scan = 12000;

  // Range gate applied in the LIDAR frame, before anything else. The driver
  // already dropped no-returns and the <0.1 m blind zone (A3); this is the
  // odometry's own reach, and the far gate matters because a 70 m return
  // contributes almost nothing to a plane fit while costing a full knn.
  float min_range_m = 0.5f;
  float max_range_m = 80.0f;

  // --- extrinsic: LIDAR frame -> IMU (body) frame -----------------------
  // Identity by default. The Mid-360's own datasheet value is a pure
  // translation of (0.011, 0.02329, -0.04412) m with no rotation; it is left
  // to the caller because a real mount adds its own, and because S6's
  // extrinsics wizard (A11) is where a measured value will come from.
  double lidar_to_imu_t[3] = {0.0, 0.0, 0.0};
  double lidar_to_imu_q[4] = {0.0, 0.0, 0.0, 1.0};  // (x, y, z, w)

  // --- IMU --------------------------------------------------------------
  EskfConfig eskf{};
  // Samples averaged for the static initialization. 100 at 200 Hz is 0.5 s.
  // The scanner does not have to be perfectly still — the mean of half a
  // second of hand tremor is still gravity to well under a degree — but it
  // must not be accelerating.
  std::uint32_t init_imu_samples = 100;

  // --- map --------------------------------------------------------------
  IVoxConfig map{};
  // Points used for one plane fit, and how flat that fit has to be. 5 and
  // 0.1 m are FAST-LIO2's values.
  std::uint32_t plane_points = 5;
  double plane_thickness_m = 0.1;
  // Reject a fit whose smallest covariance eigenvalue is more than this
  // fraction of the middle one — i.e. neighbours that are collinear rather
  // than planar, whose normal is free to spin about the line and would
  // contribute a residual pointing anywhere.
  double max_planarity_ratio = 0.1;
  // A correspondence farther than this from the map is not a correspondence.
  double max_correspondence_m = 1.0;
  // Trim the map to this radius around the current pose every
  // `trim_every_scans` scans. 0 disables (what the post pipeline and short
  // captures want).
  double map_radius_m = 0.0;
  std::uint32_t trim_every_scans = 100;

  // --- iterated update --------------------------------------------------
  std::uint32_t max_iterations = 4;
  double converge_rot_rad = 1e-4;
  double converge_trans_m = 1e-4;
  // Point-to-plane measurement sigma. 2 cm is the Mid-360's range noise plus
  // the plane-fit residual, and it sets how hard the lidar pulls against the
  // IMU prior.
  double point_sigma_m = 0.02;
  // A scan with fewer usable correspondences than this is propagated but not
  // updated — better an honest dead-reckoned pose than a fit to noise.
  std::uint32_t min_correspondences = 30;

  // --- output -----------------------------------------------------------
  // Where registered world-frame points go. Null ⇒ LioOdometry allocates its
  // own store, reachable through map_store().
  //
  // NOTE ON `map_stream`: core/types.h has no StreamId for a SLAM map yet,
  // and A6 does not own that file. The default tags the map with the stream
  // it was built from, which is at least true; see docs/A6-lio.md §9 for the
  // one-line request to append StreamId::kSlamMap.
  PageStore* map_store = nullptr;
  StreamId map_stream = StreamId::kLidarMid360;
  // Publish the points accepted into the voxel map (true, the default: the
  // rendered cloud IS the map, bounded by the voxel grid) or every registered
  // point of every scan (false: unbounded, for a short diagnostic capture).
  bool publish_map_points_only = true;

  // --- threading --------------------------------------------------------
  // false: push_points() runs the whole pipeline inline on the caller's
  // thread — the engine's default posture (DESIGN §2) and what the tests use.
  // true: a dedicated odometry thread drains the input queues, which is what
  // a live capture wants so the SDK's receive thread is never blocked.
  bool internal_thread = false;
  // Denominator for cpu_budget_used(): §3.3's "≤ 2 big cores".
  double cpu_budget_cores = 2.0;
};

struct LioStats {
  bool initialized = false;
  bool diverged = false;

  std::uint64_t imu_samples = 0;
  std::uint64_t imu_gaps = 0;        // gaps longer than EskfConfig::max_gap_s
  std::uint64_t points_in = 0;       // handed to push_points()
  std::uint64_t points_kept = 0;     // survived range gate + decimation
  std::uint64_t points_mapped = 0;   // accepted into the voxel map
  std::uint64_t scans = 0;
  std::uint64_t scans_skipped = 0;   // too few correspondences to update

  std::uint64_t residuals_last = 0;
  std::uint32_t iterations_last = 0;
  // Totals, so a mean per scan can be reported honestly: `_last` is whatever
  // the final (usually partial, flushed) scan happened to be.
  std::uint64_t residuals_total = 0;
  std::uint64_t iterations_total = 0;
  // RMS point-to-plane residual of the last scan's converged fit, in metres.
  // Without ground truth this is the single best "is the odometry actually
  // tracking" number there is: it should sit at the sensor's own range noise
  // (a few cm) and it blows up long before the pose does.
  double residual_rms_m = 0.0;

  std::uint64_t points_late = 0;    // arrived before the filter, or after their scan closed
  std::uint64_t store_appends_failed = 0;  // PageStore backpressure (kCapacityExceeded)

  double scan_ms_last = 0.0;
  double scan_ms_mean = 0.0;
  double scan_ms_p50 = 0.0;
  double scan_ms_p95 = 0.0;
  double scan_ms_max = 0.0;
  // Odometry CPU time / wall time of the scanned interval, over
  // cpu_budget_cores. 1.0 means the budget is exactly spent.
  double cpu_budget_used = 0.0;

  std::size_t map_voxels = 0;
  std::size_t map_points = 0;

  double trajectory_length_m = 0.0;
  double gravity_m_s2 = 0.0;         // |g| as estimated; a health check
  double speed_m_s = 0.0;
  std::int64_t t_last_ns = 0;
};

class LioOdometry {
 public:
  explicit LioOdometry(const LioConfig& cfg = {});
  ~LioOdometry();

  LioOdometry(const LioOdometry&) = delete;
  LioOdometry& operator=(const LioOdometry&) = delete;

  Status start();
  Status stop();
  bool running() const;

  // One IMU sample on the ENGINE clock (A4 has already mapped it), gyro in
  // rad/s and accel in m/s^2, both body frame. The float overload is the one
  // that matches timesync/imu_ingest.h's ImuSample field types:
  //
  //     lio.push_imu(s.t_engine_ns, s.gyro_rad_s, s.accel_m_s2);
  //
  // (That header is deliberately NOT included here: it declares a
  // scanengine::ImuSample and so does slam/slam.h, with different fields.
  // See docs/A6-lio.md §9.)
  Status push_imu(std::int64_t t_engine_ns, const double gyro_rad_s[3],
                  const double accel_m_s2[3]);
  Status push_imu(std::int64_t t_engine_ns, const float gyro_rad_s[3],
                  const float accel_m_s2[3]);

  // One batch of points in the LIDAR frame, metres — exactly what the
  // Mid-360 driver appends to the PageStore. `t_engine_ns` stamps the batch;
  // per-point times are interpolated from the previous batch's stamp, which
  // is what makes 96-point datagram batching give ~5 us deskew granularity.
  Status push_points(Span<const PointVertex> points, std::int64_t t_engine_ns);

  // Close and process any pending scan even though its period has not
  // elapsed. Call at end of capture; the last partial scan is otherwise
  // never registered.
  Status flush();

  LioStats stats() const;
  double cpu_budget_used() const;

  bool current_pose(Pose* out) const;

  LioPoseSource& poses();
  const LioPoseSource& poses() const;
  PageStore& map_store();
  const IVox& map() const;
  const Eskf& filter() const;
  const LioConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_SLAM_LIO_H
