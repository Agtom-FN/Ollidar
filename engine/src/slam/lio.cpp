// lio.cpp — LioOdometry, from include/scanengine/slam/lio.h.
//
// The order of operations per scan, and why each step is where it is:
//
//   1. PROPAGATE the ESKF over every IMU sample covering [t_start, t_end],
//      keeping a pose snapshot at each one. This is the prior.
//   2. DECIMATE the scan's points to the live budget. Deterministically:
//      the stride comes from the previous scan's arrival count, never from a
//      wall clock.
//   3. UNDISTORT each point into the scan-END body frame using the snapshot
//      bracketing its own timestamp. A hand-carried scanner turning at
//      30 deg/s smears a 100 ms scan by 3 degrees, which at 10 m is half a
//      metre — this step is not optional.
//   4. ITERATE the update: transform to world, find a local plane in the
//      voxel map, accumulate a point-to-plane information matrix, solve the
//      18-dimensional MAP problem, repeat until the increment is below a
//      tenth of a millimetre.
//   5. REGISTER the points into the map with the posterior pose, publish the
//      accepted ones to the PageStore, and publish the pose.
#include "scanengine/slam/lio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <vector>

#include "scanengine/core/log.h"
#include "scanengine/timesync/clock.h"

#include "lio_math.h"

namespace scanengine {

using slam_detail::Mat3;
using slam_detail::Vec3;

namespace {

constexpr int N = kEskfDim;
constexpr int kP = 0, kR = 3;
constexpr const char* kMod = "lio";
constexpr double kRadToDeg = 57.29577951308232;

struct ImuIn {
  std::int64_t t_ns = 0;
  double gyro[3] = {0, 0, 0};
  double acc[3] = {0, 0, 0};
};

struct ScanPoint {
  float x = 0, y = 0, z = 0;
  std::uint8_t rgba[4] = {255, 255, 255, 255};
  std::int64_t t_ns = 0;
};

// Pose of the body frame at one IMU step, for undistortion.
struct Snap {
  std::int64_t t_ns = 0;
  Mat3 R;
  Vec3 p;
};

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const double idx = q * static_cast<double>(v.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(idx);
  const std::size_t hi = lo + 1 < v.size() ? lo + 1 : lo;
  const double f = idx - static_cast<double>(lo);
  return v[lo] * (1.0 - f) + v[hi] * f;
}

}  // namespace

struct LioOdometry::Impl {
  explicit Impl(const LioConfig& c)
      : cfg(c), eskf(c.eskf), map(c.map), poses(36000, c.map_stream) {}

  LioConfig cfg;
  Eskf eskf;
  IVox map;
  LioPoseSource poses;
  std::unique_ptr<PageStore> owned_store;
  PageStore* store = nullptr;

  Mat3 R_il = Mat3::identity();
  Vec3 t_il;

  // --- input side (in_m_) ------------------------------------------------
  mutable std::mutex in_m;
  std::condition_variable cv;
  std::vector<ImuIn> imu_q;
  std::vector<ScanPoint> pt_q;
  std::int64_t last_batch_t_ns = 0;
  bool have_batch_t = false;
  std::atomic<bool> stop_flag{false};
  std::atomic<bool> is_running{false};
  std::thread worker;

  // --- processing side (proc_m) ------------------------------------------
  mutable std::mutex proc_m;
  std::vector<ImuIn> imu_pending;
  std::vector<ScanPoint> scan_buf;
  std::vector<ImuIn> init_buf;
  std::vector<Snap> snaps;
  std::vector<ScanPoint> scan_take;
  std::vector<Vec3> body_pts;   // undistorted, scan-end body frame
  std::vector<PointVertex> out_pts;
  std::vector<double> scan_ms;

  std::int64_t newest_imu_ns = 0;
  std::int64_t newest_point_ns = 0;
  std::int64_t scan_end_ns = 0;
  std::int64_t points_floor_ns = 0;
  bool have_scan_window = false;
  std::uint64_t prev_scan_raw_points = 0;

  // --- stats (stats_m) ----------------------------------------------------
  mutable std::mutex stats_m;
  LioStats st;
  std::int64_t t_first_data_ns = 0;
  double cpu_ms_total = 0.0;
  std::uint64_t points_late = 0;
  std::uint64_t store_drops = 0;

  // -----------------------------------------------------------------------

  void pump(bool force_close);
  void process_scan(std::int64_t t_end_ns);
  bool try_init();
  void publish_pose(std::int64_t t_ns, bool updated);
  void refresh_stats();
};

// --- construction ---------------------------------------------------------

LioOdometry::LioOdometry(const LioConfig& cfg) : impl_(new Impl(cfg)) {
  Impl& s = *impl_;
  if (!(s.cfg.scan_period_s > 0.0)) s.cfg.scan_period_s = 0.1;
  if (s.cfg.plane_points < 3) s.cfg.plane_points = 3;
  if (s.cfg.plane_points > IVox::kMaxK) s.cfg.plane_points = IVox::kMaxK;
  if (s.cfg.max_iterations == 0) s.cfg.max_iterations = 1;
  if (!(s.cfg.point_sigma_m > 0.0)) s.cfg.point_sigma_m = 0.02;
  if (!(s.cfg.cpu_budget_cores > 0.0)) s.cfg.cpu_budget_cores = 1.0;
  if (s.cfg.init_imu_samples == 0) s.cfg.init_imu_samples = 1;
  if (!(s.cfg.max_planarity_ratio > 0.0)) s.cfg.max_planarity_ratio = 0.1;

  s.R_il = slam_detail::orthonormalize(slam_detail::quat_to_mat3(s.cfg.lidar_to_imu_q));
  s.t_il = Vec3(s.cfg.lidar_to_imu_t);

  if (s.cfg.map_store != nullptr) {
    s.store = s.cfg.map_store;
  } else {
    s.owned_store.reset(new PageStore());
    s.store = s.owned_store.get();
  }
}

LioOdometry::~LioOdometry() {
  Status ignored = stop();
  (void)ignored;
}

const LioConfig& LioOdometry::config() const { return impl_->cfg; }
LioPoseSource& LioOdometry::poses() { return impl_->poses; }
const LioPoseSource& LioOdometry::poses() const { return impl_->poses; }
PageStore& LioOdometry::map_store() { return *impl_->store; }
const IVox& LioOdometry::map() const { return impl_->map; }
const Eskf& LioOdometry::filter() const { return impl_->eskf; }
bool LioOdometry::running() const { return impl_->is_running.load(); }

Status LioOdometry::start() {
  Impl& s = *impl_;
  if (s.is_running.load()) return ScanError::kInvalidState;
  s.stop_flag.store(false);
  s.is_running.store(true);
  SCAN_TRY(s.poses.start());
  if (s.cfg.internal_thread) {
    s.worker = std::thread([&s]() {
      for (;;) {
        {
          std::unique_lock<std::mutex> lk(s.in_m);
          s.cv.wait(lk, [&s]() {
            return s.stop_flag.load() || !s.imu_q.empty() || !s.pt_q.empty();
          });
          if (s.stop_flag.load() && s.imu_q.empty() && s.pt_q.empty()) break;
        }
        s.pump(false);
      }
    });
    SCAN_LOG_INFO(kMod, "odometry thread started (scan %.0f ms, budget %.1f cores)",
                  s.cfg.scan_period_s * 1000.0, s.cfg.cpu_budget_cores);
  }
  return kOkStatus;
}

Status LioOdometry::stop() {
  Impl& s = *impl_;
  if (!s.is_running.exchange(false)) return kOkStatus;
  s.stop_flag.store(true);
  s.cv.notify_all();
  if (s.worker.joinable()) s.worker.join();
  return s.poses.stop();
}

// --- input ----------------------------------------------------------------

Status LioOdometry::push_imu(std::int64_t t_engine_ns, const double gyro_rad_s[3],
                             const double accel_m_s2[3]) {
  if (gyro_rad_s == nullptr || accel_m_s2 == nullptr) return ScanError::kInvalidArgument;
  Impl& s = *impl_;
  ImuIn in;
  in.t_ns = t_engine_ns;
  for (int i = 0; i < 3; ++i) {
    in.gyro[i] = gyro_rad_s[i];
    in.acc[i] = accel_m_s2[i];
    if (!std::isfinite(in.gyro[i]) || !std::isfinite(in.acc[i])) {
      return set_last_error(ScanError::kInvalidArgument, "lio: non-finite IMU sample at %lld",
                            static_cast<long long>(t_engine_ns));
    }
  }
  {
    std::lock_guard<std::mutex> lk(s.in_m);
    s.imu_q.push_back(in);
  }
  if (s.cfg.internal_thread) {
    s.cv.notify_one();
  } else {
    s.pump(false);
  }
  return kOkStatus;
}

Status LioOdometry::push_imu(std::int64_t t_engine_ns, const float gyro_rad_s[3],
                             const float accel_m_s2[3]) {
  if (gyro_rad_s == nullptr || accel_m_s2 == nullptr) return ScanError::kInvalidArgument;
  const double g[3] = {gyro_rad_s[0], gyro_rad_s[1], gyro_rad_s[2]};
  const double a[3] = {accel_m_s2[0], accel_m_s2[1], accel_m_s2[2]};
  return push_imu(t_engine_ns, g, a);
}

Status LioOdometry::push_points(Span<const PointVertex> points, std::int64_t t_engine_ns) {
  Impl& s = *impl_;
  const std::size_t n = points.size();
  {
    std::lock_guard<std::mutex> lk(s.in_m);
    // Per-point times spread across [previous batch stamp, this batch stamp].
    // A 96-point Mid-360 datagram at 2,083 packets/s therefore resolves to
    // ~5 us, which is the granularity the undistortion actually gets.
    std::int64_t t0 = s.have_batch_t ? s.last_batch_t_ns : t_engine_ns;
    if (t0 > t_engine_ns) t0 = t_engine_ns;
    const double span = static_cast<double>(t_engine_ns - t0);
    s.last_batch_t_ns = t_engine_ns;
    s.have_batch_t = true;

    const double rmin2 = static_cast<double>(s.cfg.min_range_m) * s.cfg.min_range_m;
    const double rmax2 = static_cast<double>(s.cfg.max_range_m) * s.cfg.max_range_m;
    for (std::size_t i = 0; i < n; ++i) {
      const PointVertex& v = points[i];
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) continue;
      const double d2 = static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                        static_cast<double>(v.z) * v.z;
      if (d2 < rmin2) continue;
      if (s.cfg.max_range_m > 0.f && d2 > rmax2) continue;
      ScanPoint sp;
      sp.x = v.x;
      sp.y = v.y;
      sp.z = v.z;
      sp.rgba[0] = v.r;
      sp.rgba[1] = v.g;
      sp.rgba[2] = v.b;
      sp.rgba[3] = v.a;
      sp.t_ns = t0 + static_cast<std::int64_t>(span * (static_cast<double>(i) + 1.0) /
                                               static_cast<double>(n));
      s.pt_q.push_back(sp);
    }
  }
  {
    std::lock_guard<std::mutex> lk(s.stats_m);
    s.st.points_in += n;
  }
  if (s.cfg.internal_thread) {
    s.cv.notify_one();
  } else {
    s.pump(false);
  }
  return kOkStatus;
}

Status LioOdometry::flush() {
  impl_->pump(true);
  return kOkStatus;
}

// --- the pipeline ---------------------------------------------------------

bool LioOdometry::Impl::try_init() {
  if (eskf.initialized()) return true;
  if (init_buf.size() < cfg.init_imu_samples) return false;
  // EXACTLY the first init_imu_samples, never "however many happened to be
  // buffered". With an odometry thread the buffer can hold 100 samples or
  // 400 depending on when the worker last woke up, and averaging over a
  // different window would give a different initial attitude — i.e. the whole
  // session would depend on scheduling. This is the one place that mattered,
  // and the internal-thread-matches-inline test is what found it.
  const std::size_t n = cfg.init_imu_samples;
  double mg[3] = {0, 0, 0}, ma[3] = {0, 0, 0};
  for (std::size_t k = 0; k < n; ++k) {
    for (int i = 0; i < 3; ++i) {
      mg[i] += init_buf[k].gyro[i];
      ma[i] += init_buf[k].acc[i];
    }
  }
  const double inv = 1.0 / static_cast<double>(n);
  for (int i = 0; i < 3; ++i) {
    mg[i] *= inv;
    ma[i] *= inv;
  }
  const std::int64_t t0 = init_buf[n - 1].t_ns;
  const Status st_init = eskf.init_from_static(t0, mg, ma);
  if (!st_init.ok()) {
    // Not fatal: keep collecting. A scanner that was picked up during the
    // first half second gets another half second.
    init_buf.erase(init_buf.begin(), init_buf.begin() + static_cast<std::ptrdiff_t>(
                                                            init_buf.size() / 2));
    return false;
  }
  scan_end_ns = t0 + static_cast<std::int64_t>(cfg.scan_period_s * 1e9);
  points_floor_ns = t0;
  have_scan_window = true;
  // Samples past the init window are real data, not scaffolding: hand them
  // to the filter rather than dropping them.
  imu_pending.insert(imu_pending.end(), init_buf.begin() + static_cast<std::ptrdiff_t>(n),
                     init_buf.end());
  init_buf.clear();
  SCAN_LOG_INFO(kMod, "initialized: |a|=%.4f m/s^2, bg=(%.5f %.5f %.5f) rad/s",
                std::sqrt(ma[0] * ma[0] + ma[1] * ma[1] + ma[2] * ma[2]), mg[0], mg[1], mg[2]);
  {
    std::lock_guard<std::mutex> lk(stats_m);
    st.initialized = true;
  }
  return true;
}

void LioOdometry::Impl::pump(bool force_close) {
  std::lock_guard<std::mutex> plk(proc_m);
  {
    std::lock_guard<std::mutex> lk(in_m);
    if (!imu_q.empty()) {
      for (const ImuIn& s : imu_q) {
        if (s.t_ns > newest_imu_ns) newest_imu_ns = s.t_ns;
      }
      if (eskf.initialized()) {
        imu_pending.insert(imu_pending.end(), imu_q.begin(), imu_q.end());
      } else {
        init_buf.insert(init_buf.end(), imu_q.begin(), imu_q.end());
      }
      {
        std::lock_guard<std::mutex> slk(stats_m);
        st.imu_samples += imu_q.size();
        if (t_first_data_ns == 0) t_first_data_ns = imu_q.front().t_ns;
      }
      imu_q.clear();
    }
    if (!pt_q.empty()) {
      newest_point_ns = pt_q.back().t_ns;
      scan_buf.insert(scan_buf.end(), pt_q.begin(), pt_q.end());
      pt_q.clear();
    }
  }

  if (!try_init()) {
    // Points that arrive before the filter is up have nowhere to go.
    if (!scan_buf.empty()) {
      points_late += scan_buf.size();
      scan_buf.clear();
    }
    return;
  }

  // Drop anything older than the moment the filter came up. Those points
  // predate every pose snapshot, so they cannot be undistorted — and whether
  // they are even present would otherwise depend on when the odometry thread
  // last woke, which is the difference between a deterministic pipeline and
  // one that merely usually agrees with itself.
  if (!scan_buf.empty() && scan_buf.front().t_ns < points_floor_ns) {
    std::size_t d = 0;
    while (d < scan_buf.size() && scan_buf[d].t_ns < points_floor_ns) ++d;
    points_late += d;
    scan_buf.erase(scan_buf.begin(), scan_buf.begin() + static_cast<std::ptrdiff_t>(d));
  }

  const std::int64_t period_ns = static_cast<std::int64_t>(cfg.scan_period_s * 1e9);
  while (have_scan_window) {
    const bool imu_covers = newest_imu_ns >= scan_end_ns;
    const bool points_cover = newest_point_ns >= scan_end_ns;
    // Closing a scan needs BOTH streams past its end: IMU so the propagation
    // is complete, points so no point of this scan is still in flight. The
    // third disjunct keeps dead reckoning alive when the lidar has gone
    // silent (a blocked window, a dropped link) instead of stalling forever.
    const bool stalled = newest_imu_ns > scan_end_ns + 3 * period_ns;
    if (imu_covers && (points_cover || stalled)) {
      process_scan(scan_end_ns);
      scan_end_ns += period_ns;
      continue;
    }
    if (!force_close) break;
    // End of capture: close the remaining partial scan, once. Note that this
    // runs only AFTER every fully-closable scan above, so a flush() on a
    // backed-up queue produces the same scan boundaries as an inline run.
    if (imu_pending.empty() && scan_buf.empty()) break;
    std::int64_t t_end = scan_end_ns;
    if (!imu_covers) {
      t_end = newest_imu_ns > eskf.state().t_ns ? newest_imu_ns : eskf.state().t_ns;
    }
    if (t_end <= eskf.state().t_ns && scan_buf.empty()) break;
    process_scan(t_end);
    scan_end_ns = t_end + period_ns;
    break;
  }
  refresh_stats();
}

void LioOdometry::Impl::process_scan(std::int64_t t_end_ns) {
  const TimePoint cpu_t0 = steady_now();

  // --- 1. propagate over this scan's IMU, recording snapshots -------------
  snaps.clear();
  std::size_t consumed = 0;
  ImuIn last_used;
  bool have_last_used = false;
  for (const ImuIn& s : imu_pending) {
    if (s.t_ns > t_end_ns) break;
    ++consumed;
    if (s.t_ns <= eskf.state().t_ns) {
      last_used = s;
      have_last_used = true;
      continue;
    }
    snaps.push_back(Snap{eskf.state().t_ns, slam_detail::quat_to_mat3(eskf.state().q),
                         Vec3(eskf.state().p)});
    const Status pst = eskf.propagate(s.t_ns, s.gyro, s.acc);
    if (pst.error() == ScanError::kTimeout) {
      std::lock_guard<std::mutex> lk(stats_m);
      ++st.imu_gaps;
    }
    last_used = s;
    have_last_used = true;
  }
  imu_pending.erase(imu_pending.begin(), imu_pending.begin() + static_cast<std::ptrdiff_t>(consumed));

  // Zero-order hold to the scan boundary so the prior is exactly at t_end.
  if (have_last_used && t_end_ns > eskf.state().t_ns) {
    snaps.push_back(Snap{eskf.state().t_ns, slam_detail::quat_to_mat3(eskf.state().q),
                         Vec3(eskf.state().p)});
    const Status pst = eskf.propagate(t_end_ns, last_used.gyro, last_used.acc);
    if (pst.error() == ScanError::kTimeout) {
      std::lock_guard<std::mutex> lk(stats_m);
      ++st.imu_gaps;
    }
  }
  snaps.push_back(Snap{eskf.state().t_ns, slam_detail::quat_to_mat3(eskf.state().q),
                       Vec3(eskf.state().p)});

  // --- 2. take this scan's points and decimate ----------------------------
  std::size_t take = 0;
  while (take < scan_buf.size() && scan_buf[take].t_ns <= t_end_ns) ++take;
  const std::uint64_t raw = take;

  std::size_t stride = 1;
  if (cfg.live_points_per_sec > 0) {
    // From the PREVIOUS scan's arrival count, so the stride never depends on
    // how much of this scan happens to have arrived yet.
    const double target = static_cast<double>(cfg.live_points_per_sec) * cfg.scan_period_s;
    const double basis = prev_scan_raw_points > 0 ? static_cast<double>(prev_scan_raw_points)
                                                  : static_cast<double>(raw);
    if (target > 0.0 && basis > target) {
      // Round, do not truncate. An integer stride cannot hit an arbitrary
      // ratio, but truncating always errs on the "keep more" side and the
      // error is up to 2x: on the real 116k pts/s capture, basis/target =
      // 2.9 truncates to 2 and runs the update at 58k pts/s — 45% over the
      // budget the whole config exists to enforce.
      stride = static_cast<std::size_t>(std::llround(basis / target));
      if (stride < 1) stride = 1;
    }
  }
  if (cfg.max_points_per_scan > 0) {
    const std::size_t kept = stride > 0 ? (take + stride - 1) / stride : take;
    if (kept > cfg.max_points_per_scan) {
      stride = (take + cfg.max_points_per_scan - 1) / cfg.max_points_per_scan;
      if (stride < 1) stride = 1;
    }
  }
  prev_scan_raw_points = raw;

  scan_take.clear();
  for (std::size_t i = 0; i < take; i += stride) scan_take.push_back(scan_buf[i]);
  scan_buf.erase(scan_buf.begin(), scan_buf.begin() + static_cast<std::ptrdiff_t>(take));

  {
    std::lock_guard<std::mutex> lk(stats_m);
    st.points_kept += scan_take.size();
    ++st.scans;
  }

  // --- 3. undistort into the scan-end body frame --------------------------
  const EskfState prior = eskf.state();
  const Mat3 R_end = slam_detail::quat_to_mat3(prior.q);
  const Vec3 p_end(prior.p);

  body_pts.clear();
  body_pts.reserve(scan_take.size());
  std::size_t si = 0;
  for (const ScanPoint& sp : scan_take) {
    const Vec3 p_l(sp.x, sp.y, sp.z);
    const Vec3 p_b = R_il * p_l + t_il;
    // snaps is time-ordered and the points are too, so this is a single
    // forward walk, not a search per point.
    while (si + 2 < snaps.size() && snaps[si + 1].t_ns <= sp.t_ns) ++si;
    Mat3 R_i = snaps[si].R;
    Vec3 p_i = snaps[si].p;
    if (si + 1 < snaps.size()) {
      const Snap& a = snaps[si];
      const Snap& b = snaps[si + 1];
      const double span = static_cast<double>(b.t_ns - a.t_ns);
      if (span > 0.0) {
        double u = static_cast<double>(sp.t_ns - a.t_ns) / span;
        if (u < 0.0) u = 0.0;
        if (u > 1.0) u = 1.0;
        p_i = a.p + (b.p - a.p) * u;
        R_i = a.R * slam_detail::so3_exp(
                        slam_detail::so3_log(slam_detail::transpose(a.R) * b.R) * u);
      }
    }
    const Vec3 w = R_i * p_b + p_i;
    body_pts.push_back(slam_detail::mul_transpose(R_end, w - p_end));
  }

  // --- 4. the iterated update ---------------------------------------------
  bool updated = false;
  std::uint64_t residuals = 0;
  std::uint32_t iters = 0;
  double residual_rms = 0.0;

  if (map.point_count() > 0 && !body_pts.empty()) {
    double P_inv[N * N];
    double scratch[N * N];
    if (slam_detail::ldlt_inverse(eskf.cov(), N, P_inv, scratch)) {
      const double inv_var = 1.0 / (cfg.point_sigma_m * cfg.point_sigma_m);
      double A[N * N];
      double rhs[N];
      double dx[N];
      double delta_prev[N] = {};
      const std::size_t k = cfg.plane_points;

      for (std::uint32_t it = 0; it < cfg.max_iterations; ++it) {
        ++iters;
        const EskfState cur = eskf.state();
        const Mat3 R = slam_detail::quat_to_mat3(cur.q);
        const Vec3 p(cur.p);

        // Only the position and rotation columns of H are non-zero, so the
        // information matrix is a 6x6 block. Accumulating it directly rather
        // than through an 18-wide H is the difference between 36 and 324
        // multiply-adds per residual.
        double HtH[36] = {};
        double Htr[6] = {};
        double sum_r2 = 0.0;
        std::size_t used = 0;

        double nb[IVox::kMaxK * 3];
        for (const Vec3& pb : body_pts) {
          const Vec3 w = R * pb + p;
          const double q[3] = {w[0], w[1], w[2]};
          const std::size_t got = map.knn(q, k, cfg.max_correspondence_m, nb);
          if (got < k) continue;

          Vec3 c;
          for (std::size_t j = 0; j < got; ++j) {
            c += Vec3(nb[j * 3 + 0], nb[j * 3 + 1], nb[j * 3 + 2]);
          }
          c = c * (1.0 / static_cast<double>(got));
          Mat3 C = Mat3::zero();
          for (std::size_t j = 0; j < got; ++j) {
            const Vec3 d = Vec3(nb[j * 3 + 0], nb[j * 3 + 1], nb[j * 3 + 2]) - c;
            for (int a1 = 0; a1 < 3; ++a1) {
              for (int b1 = 0; b1 < 3; ++b1) C(a1, b1) += d[a1] * d[b1];
            }
          }
          Vec3 nrm;
          double eig[3];
          if (!slam_detail::sym3_smallest_eigenvector(C, &nrm, eig)) continue;
          // Planarity. The thickness test below bounds the out-of-plane
          // spread but says nothing about COLLINEAR neighbours, which pass it
          // trivially and hand back a normal that is free to rotate about the
          // line. This ratio is what rejects those.
          if (!(eig[1] > 0.0) || eig[0] > cfg.max_planarity_ratio * eig[1]) continue;
          const double d0 = -slam_detail::dot(nrm, c);
          bool flat = true;
          for (std::size_t j = 0; j < got; ++j) {
            const Vec3 pj(nb[j * 3 + 0], nb[j * 3 + 1], nb[j * 3 + 2]);
            if (std::fabs(slam_detail::dot(nrm, pj) + d0) > cfg.plane_thickness_m) {
              flat = false;
              break;
            }
          }
          if (!flat) continue;

          const double r = slam_detail::dot(nrm, w) + d0;
          if (std::fabs(r) > cfg.max_correspondence_m) continue;

          // w = R Exp(dth) pb + p  =>  dw/dth = -R [pb]x, so
          //   dr/dth = -n^T R [pb]x = -((R^T n) x pb)^T.
          // (NOT -(n x R pb): that differs from this by a rotation, and it is
          // the exact kind of error that still converges on a slow synthetic
          // trajectory while quietly wrecking a fast real one.)
          const Vec3 hr = -slam_detail::cross(slam_detail::mul_transpose(R, nrm), pb);
          const double h[6] = {nrm[0], nrm[1], nrm[2], hr[0], hr[1], hr[2]};
          sum_r2 += r * r;
          for (int a1 = 0; a1 < 6; ++a1) {
            Htr[a1] += h[a1] * r;
            for (int b1 = a1; b1 < 6; ++b1) HtH[a1 * 6 + b1] += h[a1] * h[b1];
          }
          ++used;
        }
        for (int a1 = 0; a1 < 6; ++a1) {
          for (int b1 = 0; b1 < a1; ++b1) HtH[a1 * 6 + b1] = HtH[b1 * 6 + a1];
        }
        residuals = used;
        residual_rms = used > 0 ? std::sqrt(sum_r2 / static_cast<double>(used)) : 0.0;
        if (used < cfg.min_correspondences) break;

        // delta_j = current [-] prior, in the prior's error coordinates.
        double delta_j[N];
        eskf.boxminus(prior, delta_j);

        // A = H^T R^-1 H + P^-1 ; rhs = (H^T R^-1 H) delta_j - H^T R^-1 r.
        std::memcpy(A, P_inv, sizeof(A));
        for (int a1 = 0; a1 < 6; ++a1) {
          for (int b1 = 0; b1 < 6; ++b1) A[a1 * N + b1] += HtH[a1 * 6 + b1] * inv_var;
        }
        for (int a1 = 0; a1 < N; ++a1) rhs[a1] = 0.0;
        for (int a1 = 0; a1 < 6; ++a1) {
          double acc = 0.0;
          for (int b1 = 0; b1 < 6; ++b1) acc += HtH[a1 * 6 + b1] * delta_j[b1];
          rhs[a1] = (acc - Htr[a1]) * inv_var;
        }

        double fact[N * N];
        std::memcpy(fact, A, sizeof(fact));
        if (!slam_detail::ldlt_factor(fact, N)) break;
        std::memcpy(dx, rhs, sizeof(dx));
        slam_detail::ldlt_solve_inplace(fact, N, dx);

        bool bad = false;
        for (int a1 = 0; a1 < N; ++a1) {
          if (!std::isfinite(dx[a1])) bad = true;
        }
        if (bad) break;

        eskf.set_state(prior);
        eskf.boxplus(dx);
        updated = true;

        double drot = 0.0, dtr = 0.0;
        for (int a1 = 0; a1 < 3; ++a1) {
          const double a2 = dx[kR + a1] - delta_prev[kR + a1];
          const double t2 = dx[kP + a1] - delta_prev[kP + a1];
          drot += a2 * a2;
          dtr += t2 * t2;
        }
        std::memcpy(delta_prev, dx, sizeof(delta_prev));

        const bool converged = std::sqrt(drot) < cfg.converge_rot_rad &&
                               std::sqrt(dtr) < cfg.converge_trans_m;
        if (converged || it + 1 == cfg.max_iterations) {
          // Posterior covariance is the inverse of the information matrix we
          // just solved against.
          double post[N * N];
          double sc2[N * N];
          if (slam_detail::ldlt_inverse(A, N, post, sc2)) {
            std::memcpy(eskf.cov(), post, sizeof(post));
          }
          break;
        }
      }
    }
  }

  if (!updated) {
    std::lock_guard<std::mutex> lk(stats_m);
    ++st.scans_skipped;
  }

  // --- 5. register into the map and publish -------------------------------
  const EskfState post = eskf.state();
  const Mat3 R_post = slam_detail::quat_to_mat3(post.q);
  const Vec3 p_post(post.p);

  out_pts.clear();
  out_pts.reserve(body_pts.size());
  std::uint64_t mapped = 0;
  for (std::size_t i = 0; i < body_pts.size(); ++i) {
    const Vec3 w = R_post * body_pts[i] + p_post;
    if (!slam_detail::finite(w)) continue;
    const bool stored = map.insert(w[0], w[1], w[2]);
    if (stored) ++mapped;
    if (cfg.publish_map_points_only && !stored) continue;
    PointVertex pv;
    pv.x = static_cast<float>(w[0]);
    pv.y = static_cast<float>(w[1]);
    pv.z = static_cast<float>(w[2]);
    pv.r = scan_take[i].rgba[0];
    pv.g = scan_take[i].rgba[1];
    pv.b = scan_take[i].rgba[2];
    pv.a = scan_take[i].rgba[3];
    out_pts.push_back(pv);
  }
  if (!out_pts.empty() && store != nullptr) {
    std::uint32_t appended = 0;
    const Status ast = store->append(cfg.map_stream,
                                     Span<const PointVertex>(out_pts.data(), out_pts.size()),
                                     t_end_ns, &appended);
    (void)appended;
    if (!ast.ok()) ++store_drops;
  }

  if (cfg.map_radius_m > 0.0 && cfg.trim_every_scans > 0) {
    std::lock_guard<std::mutex> lk(stats_m);
    if (st.scans % cfg.trim_every_scans == 0) {
      const double c[3] = {post.p[0], post.p[1], post.p[2]};
      (void)map.trim(c, cfg.map_radius_m);
    }
  }

  publish_pose(t_end_ns, updated);

  const double ms = static_cast<double>(steady_now().nanos - cpu_t0.nanos) * 1e-6;
  std::lock_guard<std::mutex> lk(stats_m);
  st.points_mapped += mapped;
  st.residuals_last = residuals;
  st.iterations_last = iters;
  st.residuals_total += residuals;
  st.iterations_total += iters;
  st.residual_rms_m = residual_rms;
  st.points_late = points_late;
  st.store_appends_failed = store_drops;
  st.scan_ms_last = ms;
  cpu_ms_total += ms;
  scan_ms.push_back(ms);
  if (scan_ms.size() > 8192) scan_ms.erase(scan_ms.begin());
  st.t_last_ns = t_end_ns;
}

void LioOdometry::Impl::publish_pose(std::int64_t t_ns, bool updated) {
  const EskfState& x = eskf.state();
  Pose po;
  po.t_mono_ns = t_ns;
  for (int i = 0; i < 3; ++i) po.position[i] = x.p[i];
  for (int i = 0; i < 4; ++i) po.orientation[i] = x.q[i];
  const double* P = eskf.cov();
  double vp = 0.0, vr = 0.0;
  for (int i = 0; i < 3; ++i) {
    vp += P[(kP + i) * N + (kP + i)];
    vr += P[(kR + i) * N + (kR + i)];
  }
  po.position_sigma_m = static_cast<float>(std::sqrt(vp > 0.0 ? vp / 3.0 : 0.0));
  po.orientation_sigma_deg =
      static_cast<float>(std::sqrt(vr > 0.0 ? vr / 3.0 : 0.0) * kRadToDeg);
  po.source = poses.stream();
  const bool bad = eskf.diverged();
  po.quality = bad ? PoseQuality::kInvalid
                   : (updated ? PoseQuality::kGood : PoseQuality::kFair);
  po.tracking_lost = bad ? 1u : 0u;
  const Status pst = poses.push_pose(po);
  (void)pst;
  if (bad) {
    std::lock_guard<std::mutex> lk(stats_m);
    if (!st.diverged) {
      SCAN_LOG_ERROR(kMod, "diverged at t=%lld ns (|v|=%.2f m/s)", static_cast<long long>(t_ns),
                     std::sqrt(x.v[0] * x.v[0] + x.v[1] * x.v[1] + x.v[2] * x.v[2]));
    }
    st.diverged = true;
  }
}

void LioOdometry::Impl::refresh_stats() {
  std::lock_guard<std::mutex> lk(stats_m);
  st.map_voxels = map.voxel_count();
  st.map_points = map.point_count();
  st.trajectory_length_m = poses.trajectory_length_m();
  const EskfState& x = eskf.state();
  st.gravity_m_s2 = std::sqrt(x.g[0] * x.g[0] + x.g[1] * x.g[1] + x.g[2] * x.g[2]);
  st.speed_m_s = std::sqrt(x.v[0] * x.v[0] + x.v[1] * x.v[1] + x.v[2] * x.v[2]);
  if (!scan_ms.empty()) {
    double sum = 0.0, mx = 0.0;
    for (double v : scan_ms) {
      sum += v;
      if (v > mx) mx = v;
    }
    st.scan_ms_mean = sum / static_cast<double>(scan_ms.size());
    st.scan_ms_max = mx;
    st.scan_ms_p50 = percentile(scan_ms, 0.50);
    st.scan_ms_p95 = percentile(scan_ms, 0.95);
  }
  const double span_ns = static_cast<double>(st.t_last_ns - t_first_data_ns);
  if (span_ns > 0.0) {
    st.cpu_budget_used = (cpu_ms_total * 1e6) / (span_ns * cfg.cpu_budget_cores);
  }
}

// --- accessors ------------------------------------------------------------

LioStats LioOdometry::stats() const {
  std::lock_guard<std::mutex> lk(impl_->stats_m);
  return impl_->st;
}

double LioOdometry::cpu_budget_used() const {
  std::lock_guard<std::mutex> lk(impl_->stats_m);
  return impl_->st.cpu_budget_used;
}

bool LioOdometry::current_pose(Pose* out) const { return impl_->poses.latest(out); }

}  // namespace scanengine
