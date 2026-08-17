// WalkSpeedEstimator.h — how fast is the operator actually walking?
//
// WHY THIS IS ITS OWN CLASS (round-5 field bug A, NOTES.md §17.9)
//
// The owner's first hardware session with the redesigned capture panel reported
// the walk hint saying "walking / moving" with the rig standing still on a
// tripod. The round-5 implementation derived the speed inline in
// CaptureWindow::pollTrajectory() from two consecutive LioPoseSource::latest()
// samples, and every one of the following was wrong at once:
//
//   1. WALL CLOCK, NOT POSE TIME. dt came from a QElapsedTimer ticking in the
//      GUI thread, not from Pose::t_mono_ns. A Qt timer that coalesces two
//      100 ms ticks into one 2 ms gap turned 3 cm of pose noise into 15 m/s.
//   2. NO dt FLOOR WORTH THE NAME. The guard was `dt > 1e-3`, i.e. a 1.1 ms
//      gap was accepted and multiplied the measured step by ~90.
//   3. NO SAMPLE-IDENTITY CHECK. latest() returns the newest pose whether or
//      not it is NEW; a stalled LIO was polled ten times a second and every
//      poll was treated as a fresh observation.
//   4. SINGLE-SAMPLE DECISION. One 10 Hz step was the whole measurement, so
//      LIO's stationary jitter (centimetres, and unbounded in the instant after
//      a re-initialization) WAS the signal.
//   5. NO FRAME-RESET HANDLING. Every Start / Pause / Resume / Stop restarts
//      the engine session, which restarts LioOdometry at the ORIGIN. The next
//      poll saw the whole previous trajectory as one 100 ms step — tens of m/s
//      of "walking", from a rig that never moved.
//
// WHAT THIS DOES INSTEAD. It keeps a short ring of poses stamped with THEIR OWN
// t_mono_ns and reports the NET displacement across a window of at least
// `window_s` of pose time, divided by that window's real span. Net displacement
// is the measurement that matches the question being asked ("am I getting
// anywhere?"): jitter about a fixed point has a net displacement of ~0 no matter
// how large each individual wobble is, while a walker at 1.4 m/s covers 1.7 m in
// 1.2 s. A deadband under `noise_floor_m` reads as a hard zero so a stationary
// rig prints 0.00, not 0.04. A step larger than `max_step_m` is not motion at
// all — nothing on two legs moves 2 m between two 10 Hz poses — so it is treated
// as a discontinuity (LIO re-init, a relocalization jump) and RESETS the window
// rather than being reported.
//
// No Qt, no engine types, no I/O: --walk-speed-selftest (main.cpp) drives this
// class directly through the stationary, walking, frame-reset, coalesced-poll
// and stalled-stream cases.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace lidarscan {

// --- the drift-free half of the answer -------------------------------------
//
// WHY A POSE-ONLY SPEED CANNOT BE FIXED BY BETTER ARITHMETIC (NOTES.md §17.9).
//
// Measured on spikes/s2-mid360-sim, whose platform follows an analytic slow
// lissajous — |v| never exceeds 0.31 m/s and |a| never exceeds 0.015 m/s²:
//
//     t=10 s   LIO pose (2.22, 1.86)   truth (2.17, 1.87)     — tracking
//     t=20 s   LIO pose (5.96, 3.83)   truth (3.46, 2.35)     — 2.5 m out
//     t=33 s   LIO pose (23.8, 7.39)   truth  < 6 m possible  — diverged
//
// The live odometry's position runs away, monotonically and superlinearly, and
// there is no covariance, no quality flag and no divergence event on the pose
// API to tell an app that it has. So a speed derived from pose displacement —
// however carefully windowed, however robustly filtered — eventually reports a
// stationary rig sprinting. Round 5 shipped exactly that, and it is what the
// owner saw. Fixing the arithmetic alone (which the estimator below also does)
// buys minutes, not correctness.
//
// WHAT DOES NOT DRIFT is the accelerometer. Over the same run it read
// |a| = 9.8083 ± 0.008 m/s²: pure gravity, i.e. the rig was never accelerated.
// A velocity nothing accelerated into is not a velocity. And a human carrying a
// scanner cannot walk without gait: heel strike and body sway put 1–3 m/s² of
// deviation on |a| and 0.2–0.6 rad/s on the gyro, every step, on every rig. So
// the IMU answers "is this thing being carried anywhere?" with no integration
// and therefore no drift — which is the exact question the walk hint needs, and
// the one the pose cannot answer honestly.
//
// This gate only ever SUPPRESSES. It never invents motion and never raises the
// speed; the thresholds are set well above sensor noise (measured 0.008 m/s²)
// and well below the gentlest gait, so a false "still" would take a walker who
// produces no gait at all.
class MotionGate {
 public:
  struct Config {
    // RMS deviation of |accel| from its own window mean. The drift-free
    // evidence that the rig was actually accelerated. Sensor noise measured at
    // 0.008 m/s²; walking gait is 1–3 m/s².
    double accel_dev_still_m_s2 = 0.30;
    // Mean |gyro|. A carried rig turns; a parked one does not. The sim's slow
    // yaw sweep reads 0.03 rad/s, a walker 0.2–0.6.
    double gyro_rms_still_rad_s = 0.15;
    // Below this many samples the window says nothing (200 Hz Mid-360 IMU).
    std::size_t min_samples = 40;
  };

  struct Reading {
    double accel_dev_m_s2 = 0.0;
    double gyro_rms_rad_s = 0.0;
    double accel_mean_m_s2 = 0.0;
    std::size_t samples = 0;
    bool valid = false;  // enough samples to say anything at all
    bool still = false;  // valid AND both statistics under their thresholds
  };

  // `gyro` and `accel` are 3*n interleaved (x,y,z) values in rad/s and m/s².
  // No IMU at all (n == 0) yields valid=false, still=false — an unknown gate
  // must never suppress, or a rig with a dead IMU would silently stop hinting.
  static Reading measure(const double* gyro, const double* accel, std::size_t n,
                         const Config& cfg);
  // The shipped thresholds.
  static Reading measure(const double* gyro, const double* accel, std::size_t n) {
    return measure(gyro, accel, n, Config{});
  }
};

class WalkSpeedEstimator {
 public:
  struct Config {
    // At least this much POSE time must be in the window before a speed is
    // reported at all. One second is ~10 LIO poses: long enough that jitter
    // cancels, short enough that a walker who stops is shown stopping.
    double window_s = 1.2;
    // Below this, two poses are the same instant as far as a 10 Hz odometry is
    // concerned; dividing by such a gap is what produced the 15 m/s spikes.
    double min_dt_s = 0.04;
    // A gap this long means the pose stream stalled (a session restart, a
    // device drop). The window is not meaningful across it.
    double max_gap_s = 1.5;
    // Net displacement under this, across the whole window, is noise.
    // Mid-360 LIO parked on a tripod wanders a couple of centimetres.
    double noise_floor_m = 0.08;
    // One 10 Hz step longer than this is a frame reset, not a stride.
    double max_step_m = 2.0;
    // Never report more than this. A human with a scanner does not exceed it,
    // and a number above it is by definition a measurement fault, not a fast
    // operator.
    double max_speed_mps = 8.0;
  };

  WalkSpeedEstimator() = default;
  explicit WalkSpeedEstimator(const Config& cfg) : cfg_(cfg) {}

  const Config& config() const { return cfg_; }

  // Forget everything. Call on every session (re)start: the pose frame the next
  // samples arrive in has no relationship to the previous one.
  void reset();

  // Feed the newest pose. Returns true when this was a genuinely NEW sample
  // (i.e. its timestamp advanced and it was not a discontinuity), which is also
  // the only case in which the caller should consider extending a trail.
  //
  // `t_mono_ns` is Pose::t_mono_ns — the odometry's own stamp, never a wall
  // clock. `p` is the position in metres in the session's local metric frame.
  bool update(std::int64_t t_mono_ns, const double p[3]);

  // Metres per second, or 0.0 while `valid()` is false. Never negative, never
  // above Config::max_speed_mps.
  double speedMps() const { return speed_mps_; }
  // True once a full `window_s` of pose time has been observed without a
  // discontinuity — i.e. once speedMps() means something.
  bool valid() const { return valid_; }
  // How many discontinuities (frame resets / teleports) have been absorbed.
  // Reported in the capture log so a field run shows how often LIO restarted.
  std::uint32_t discontinuities() const { return discontinuities_; }
  // Poses ignored because their timestamp did not advance (a stalled stream
  // polled at 10 Hz), or because they arrived faster than min_dt_s.
  std::uint32_t staleSamples() const { return stale_samples_; }
  std::size_t windowSamples() const { return win_.size(); }
  // Pose time currently spanned by the window, in seconds.
  double windowSpanS() const;

 private:
  struct Sample {
    double t_s = 0.0;
    double p[3] = {0.0, 0.0, 0.0};
  };

  void recompute();

  Config cfg_{};
  std::deque<Sample> win_;
  double speed_mps_ = 0.0;
  bool valid_ = false;
  bool have_last_ = false;
  std::int64_t last_t_ns_ = 0;
  std::uint32_t discontinuities_ = 0;
  std::uint32_t stale_samples_ = 0;
};

}  // namespace lidarscan
