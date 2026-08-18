#pragma once

// ROUND 9, owner item 35 — densifying the pose stream with the phone's IMU.
//
// > "lidar data and the imu position data need sync the frequency"
//
// The owner is describing a rate mismatch that is real and structural. ARCore
// delivers poses at ~30 Hz — measured on the owner's own scan-017, 150 poses
// over 4.999 s, median interval 33.33 ms. The COIN-D6 samples at 4000 Hz and,
// after ROUND 9's per-sample timestamping, every one of those returns carries
// its own instant. So a single ARCore interval spans ~133 lidar returns, and
// for all 133 the trajectory is whatever a lerp/slerp between two endpoints
// says it is.
//
// That is fine for the slow part of walking. It is not fine for the fast part:
// heel strike, hand tremor and the small rotations of a handheld rig live at
// 5-15 Hz, and a 30 Hz sampler simply cannot represent them — everything above
// 15 Hz aliases and everything near it is attenuated. The phone's own gyro runs
// at 200-400 Hz and sees all of it.
//
// ORIENTATION is where this pays. A 1 degree orientation error puts a 3 m
// return 5 cm out of place; the same 1 mm of position error puts it 1 mm out.
// So this class densifies rotation from the gyro and leaves position on the
// lerp, exactly as the item asks.
//
// ---------------------------------------------------------------------------
// The method
// ---------------------------------------------------------------------------
//
// For a query at `t` bracketed by ARCore poses `a` (at ta) and `b` (at tb):
//
//   1. Integrate the bias-corrected gyro from ta forward to t, in the body
//      frame:  q_int(t) = q_a * prod_k exp(omega_k dt_k).
//   2. Integrate all the way to tb as well and form the closing error
//      e = q_int(tb)^-1 * q_b — everything the gyro got wrong over the
//      interval: residual bias, scale, and the fact that ARCore has its own
//      opinion informed by the camera.
//   3. Distribute that error linearly in time:
//          q(t) = q_int(t) * exp( u * log(e) ),   u = (t - ta) / (tb - ta).
//
// Step 3 is what makes this safe. At u = 0 and u = 1 the result is EXACTLY the
// ARCore pose, so the densifier can never drag the trajectory away from VIO,
// can never accumulate drift across intervals, and degrades continuously: if
// the gyro says nothing interesting, `e` absorbs the whole rotation and the
// output is a slerp again. All the IMU is allowed to do is choose the PATH
// between two points ARCore has already fixed.
//
// Bias is estimated from the same closing error (a bias b over an interval of
// length T shows up as exactly b*T of closing error, to first order), leaked in
// slowly and clamped. When the rig is still — no gyro energy and no ARCore
// rotation — the raw gyro IS the bias, and that is the strongest observation
// available, so it is weighted harder.
//
// ---------------------------------------------------------------------------
// Falling back
// ---------------------------------------------------------------------------
//
// Every one of these must hold or the query falls through to the wrapped
// source's plain interpolation, and `stats().fallbacks` counts it:
//
//   * the wrapped source returned a usable bracket at all;
//   * IMU samples actually cover [ta, tb] with no hole wider than
//     `max_imu_gap_ns` (default 25 ms — an IMU that stutters is worse than no
//     IMU, because it fabricates a shape);
//   * the bracket is not longer than `max_bracket_ns` (default 200 ms): past
//     that the linear error distribution stops being a good model;
//   * the closing error is not absurd (`max_closing_deg`, default 20 deg),
//     which is the guard against a wrong `camera_from_imu` or a mis-stamped
//     stream quietly making things worse.
//
// It is a strict wrapper: `ExternalPoseSource` is untouched, and so is
// `D6PushbroomAssembler`, which already takes the `PoseInterpolator`
// interface rather than a concrete class.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "scanengine/core/types.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/poses/se3.h"

namespace scanengine {

struct ImuDensifyConfig {
  // Widest hole in the IMU stream, inside a bracket, that is still tolerated.
  std::int64_t max_imu_gap_ns = 25'000'000;  // 25 ms

  // Longest ARCore bracket the linear error distribution is trusted over.
  // A 30 Hz stream brackets at 33 ms; 200 ms means ~6 dropped poses.
  std::int64_t max_bracket_ns = 200'000'000;

  // Sanity ceiling on the gyro-vs-ARCore disagreement over one bracket.
  double max_closing_deg = 20.0;

  // Rotation taking a vector in the IMU's frame to the frame ARCore reports
  // its pose in (the camera frame), as a quaternion (x, y, z, w).
  //
  // These are NOT the same frame on a real phone: Android's sensor frame is
  // defined against the display in its natural orientation, while ARCore's is
  // defined against the camera image, and the camera is usually mounted at
  // 90 degrees to the display (CameraCharacteristics.SENSOR_ORIENTATION). The
  // Android layer must supply this; the identity default is correct only for a
  // synthetic stream or a device where the two coincide.
  //
  // Getting it wrong does not corrupt the endpoints — those are pinned to
  // ARCore — but it distorts the path between them, which is the entire value
  // being added, so it is worth getting right.
  double camera_from_imu[4] = {0.0, 0.0, 0.0, 1.0};

  // Gyro bias estimation.
  bool estimate_bias = true;
  // Fraction of each bracket's implied bias folded into the estimate.
  double bias_gain = 0.05;
  // Larger gain while the rig is judged still (see stationary thresholds).
  double bias_gain_stationary = 0.25;
  // Hard clamp, rad/s. A consumer gyro's bias is well under 0.05 rad/s;
  // anything past this is a fault, not a bias.
  double max_bias_rad_s = 0.10;
  // "Still" = every gyro sample in the bracket under this rate AND ARCore
  // rotating less than `stationary_arcore_deg` across it.
  double stationary_rate_rad_s = 0.02;
  double stationary_arcore_deg = 0.15;

  // Ring capacity for the IMU samples. 400 Hz * 8 s.
  std::size_t capacity = 3200;
};

struct ImuDensifyStats {
  std::uint64_t samples_in = 0;
  std::uint64_t samples_rejected = 0;   // non-finite, or out of order
  std::uint64_t queries = 0;
  std::uint64_t densified = 0;
  std::uint64_t fallbacks = 0;          // fell through to plain interpolation
  // ROUND 14: the reasons must ADD UP to `fallbacks`. They did not: the
  // commonest bucket by far — the wrapped source had nothing usable to
  // densify — was counted in the total and nowhere else, so on the owner's
  // scan-034 only 11,522 of 63,805 fallbacks had a reason and a reader was
  // left to conclude the densifier was failing for some unnamed cause. It was
  // not failing at all; ARCore simply had no pose there. The two are split
  // because they ask for different fixes: `no_pose` is a trajectory hole (the
  // point is unresolvable by ANY method and the pushbroom drops it too),
  // `gate` is a pose that exists but is stale/tracking-lost/low-confidence.
  std::uint64_t fallback_no_pose = 0;   // the wrapped source returned no sample
  std::uint64_t fallback_gate = 0;      // a sample, but PoseGate != kOk
  std::uint64_t fallback_no_imu = 0;
  std::uint64_t fallback_gap = 0;
  std::uint64_t fallback_bracket = 0;
  std::uint64_t fallback_closing = 0;
  std::uint64_t bias_updates = 0;
  double bias_rad_s[3] = {0.0, 0.0, 0.0};
  // Worst and mean closing error seen, degrees — the honest measure of how
  // much the gyro and ARCore disagree over a bracket on this rig.
  double worst_closing_deg = 0.0;
  double mean_closing_deg = 0.0;
};

// One phone IMU sample, in the IMU's own frame, engine time.
struct PhoneImuSample {
  std::int64_t t_mono_ns = 0;
  float gyro_rad_s[3] = {0.0f, 0.0f, 0.0f};
  float accel_m_s2[3] = {0.0f, 0.0f, 0.0f};
};

// Wraps a PoseInterpolator and replaces its orientation with a gyro-integrated
// path between the same bracketing poses. Position is untouched.
//
// Thread-safety mirrors `ExternalPoseSource`: `push_imu()` may be called from
// the sensor thread while `sample_at()` runs on the lidar thread.
class ImuDensifiedPoseSource : public PoseInterpolator {
 public:
  ImuDensifiedPoseSource(const PoseInterpolator* base, const ImuDensifyConfig& cfg = {});

  // Rejects non-finite values and out-of-order stamps (an Android
  // SensorEventListener can deliver those across sensor types).
  bool push_imu(const PhoneImuSample& s);

  PoseSample sample_at(std::int64_t t_mono_ns) const override;
  bool time_span(std::int64_t* first_ns, std::int64_t* last_ns) const override;

  ImuDensifyStats stats() const;
  void reset();

  // For the offline path, which knows the whole stream up front.
  std::size_t buffered() const;

 private:
  // Integrate the bias-corrected gyro from `t0` to `t1`, returning the relative
  // rotation as a quaternion in the CAMERA frame. False when the ring does not
  // cover [t0, t1] closely enough.
  bool integrate_(std::int64_t t0, std::int64_t t1, double q_rel[4],
                  double* peak_rate, bool* saw_gap) const;

  const PoseInterpolator* base_ = nullptr;
  ImuDensifyConfig cfg_{};

  mutable std::mutex m_;
  std::vector<PhoneImuSample> ring_;
  std::size_t head_ = 0;   // next write slot
  std::size_t size_ = 0;
  std::int64_t last_t_ns_ = 0;
  // Mutable because bias estimation happens on the query path: `sample_at()`
  // is const by the PoseInterpolator contract, but the closing error it
  // computes is the ONLY observation of the bias there is. Guarded by m_.
  mutable double bias_[3] = {0.0, 0.0, 0.0};
  mutable ImuDensifyStats stats_{};
  mutable double closing_sum_deg_ = 0.0;
  mutable std::uint64_t closing_n_ = 0;
};

}  // namespace scanengine
