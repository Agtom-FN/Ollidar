// eskf.h — the 18-state error-state Kalman filter behind the live LIO.
//
// Tech Spec §3.3: "ESKF lidar-inertial odometry (Point-LIO/FAST-LIO2 family):
// IMU propagation @200 Hz, iterated update against incremental voxel map".
// This file is the IMU half of that. slam/lio.h is the lidar half; the
// iterated update lives there because it needs the map.
//
// --- the state ------------------------------------------------------------
//
// NOMINAL state (EskfState): position, velocity, orientation, gyro bias,
// accel bias, gravity — 3 + 3 + 4 + 3 + 3 + 3 stored numbers, but the filter
// works on the 18-dimensional ERROR state:
//
//     [ 0: 3)  dp    position       (world)
//     [ 3: 6)  dth   orientation    (RIGHT perturbation: R_true = R * Exp(dth))
//     [ 6: 9)  dv    velocity       (world)
//     [ 9:12)  dbg   gyro bias      (body)
//     [12:15)  dba   accel bias     (body)
//     [15:18)  dg    gravity        (world)
//
// The ordering is part of the contract: cov(i, j) and boxplus() both index it,
// and tests assert on specific blocks.
//
// WHY GRAVITY IS A STATE, and a full 3-vector at that. FAST-LIO2 carries
// gravity as a 2-DOF quantity on S^2 because its magnitude is known. We carry
// 3 DOF and no manifold constraint, for two reasons: (a) the third DOF absorbs
// accelerometer scale error, which on the Mid-360's MEMS IMU is real and
// otherwise leaks straight into vertical velocity, and (b) a 3-vector keeps
// the error state a plain R^18 so the whole update is one dense symmetric
// solve with no chart bookkeeping. The cost is that |g| is only *observed*
// rather than *imposed* — which is why it is worth asserting on, and
// tests/test_lio.cpp does exactly that.
//
// WHY THE WORLD FRAME IS Z-UP. init_from_static() builds the initial
// orientation as the rotation taking the measured specific force onto +Z, so
// the session's local metric frame (poses/pose_source.h) is gravity-aligned
// from the first pose and the floor-plan slice band in §3.6 means what it
// says. Yaw is unobservable from an IMU and is left at identity.
//
// Owner: A6.
#ifndef SCANENGINE_SLAM_ESKF_H
#define SCANENGINE_SLAM_ESKF_H

#include <cstdint>
#include <memory>

#include "scanengine/core/error.h"

namespace scanengine {

inline constexpr int kEskfDim = 18;

struct EskfState {
  std::int64_t t_ns = 0;
  double p[3] = {0, 0, 0};              // world position, metres
  double v[3] = {0, 0, 0};              // world velocity, m/s
  double q[4] = {0, 0, 0, 1};           // body->world rotation, (x, y, z, w)
  double bg[3] = {0, 0, 0};             // gyro bias, rad/s
  double ba[3] = {0, 0, 0};             // accel bias, m/s^2
  double g[3] = {0, 0, -9.80665};       // world gravity, m/s^2
};

struct EskfConfig {
  // Continuous-time noise densities. Defaults are the order of magnitude of
  // the Mid-360's built-in MEMS IMU; they are deliberately a little
  // pessimistic, because an ESKF that trusts its IMU too much rejects good
  // lidar corrections and one that trusts it too little merely converges a
  // touch slower.
  double gyro_noise_rad_s = 0.01;        // white noise on the rate
  double accel_noise_m_s2 = 0.1;         // white noise on the specific force
  double gyro_bias_rw_rad_s2 = 1e-4;     // bias random walk
  double accel_bias_rw_m_s3 = 1e-3;      // bias random walk

  // Initial covariance diagonals. Position and orientation start tight (the
  // session frame is DEFINED by the first pose, so its error is zero by
  // construction); the biases and gravity start loose because they are what
  // the first few seconds of scanning are actually estimating.
  double init_sigma_p_m = 1e-3;
  double init_sigma_rot_rad = 1e-3;
  double init_sigma_v_m_s = 0.1;
  double init_sigma_bg_rad_s = 1e-2;
  double init_sigma_ba_m_s2 = 1e-1;
  double init_sigma_g_m_s2 = 1e-2;

  double gravity_m_s2 = 9.80665;

  // Longest IMU gap the filter will integrate across in one step. A larger
  // gap is split into sub-steps so the first-order covariance propagation
  // stays valid; a gap larger than `max_gap_s` is treated as a stream break.
  double max_step_s = 0.02;
  double max_gap_s = 0.5;
};

class Eskf {
 public:
  explicit Eskf(const EskfConfig& cfg = {});
  ~Eskf();

  Eskf(const Eskf&) = delete;
  Eskf& operator=(const Eskf&) = delete;

  // Initialize from a static-ish window of IMU samples: `mean_gyro` becomes
  // the initial gyro bias, and `mean_accel` (m/s^2, body frame) fixes both
  // the initial roll/pitch and the gravity direction. Returns
  // kInvalidArgument if `mean_accel` is degenerate.
  Status init_from_static(std::int64_t t_ns, const double mean_gyro[3],
                          const double mean_accel[3]);

  // Initialize explicitly (tests, and a session resuming from a known pose).
  void init(const EskfState& s);

  bool initialized() const;

  // One IMU sample. `gyro` rad/s, `accel` m/s^2, both in the body frame, both
  // raw (biases are subtracted here). Propagates the nominal state and the
  // covariance to `t_ns`. A sample at or before the current time is ignored
  // and reported as kAgain; a gap longer than max_gap_s returns kTimeout and
  // re-anchors the clock without integrating a fiction.
  Status propagate(std::int64_t t_ns, const double gyro[3], const double accel[3]);

  const EskfState& state() const;
  void set_state(const EskfState& s);

  // 18x18 covariance, row-major. Writable so slam/lio.cpp can install the
  // posterior it computes; there is no reason to hide it behind an interface
  // only one translation unit uses.
  double* cov();
  const double* cov() const;

  // x <- x [+] dx, with the error ordering documented at the top of this file.
  void boxplus(const double dx[kEskfDim]);

  // dx <- this [-] other (the inverse of boxplus, same ordering).
  void boxminus(const EskfState& other, double dx[kEskfDim]) const;

  // True if any element of the state or covariance has gone non-finite, or
  // the speed has left anything a hand-carried scanner can do. The caller
  // must stop trusting the filter, not merely log it.
  bool diverged(double max_speed_m_s = 30.0) const;

  const EskfConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_SLAM_ESKF_H
