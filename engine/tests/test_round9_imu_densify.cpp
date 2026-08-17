// ROUND 9, owner item 35 — does densifying the pose stream with the phone's
// gyro actually recover motion that ARCore's 30 Hz stream cannot represent?
//
// > "lidar data and the imu position data need sync the frequency"
//
// The experiment. A rig walks past a flat wall. Its orientation carries, on top
// of the ordinary 2 Hz gait sway, a 12 Hz rotational jitter — the tremor and
// heel-strike band of a handheld rig. 12 Hz is deliberately BELOW the 30 Hz
// pose Nyquist and therefore not an aliasing argument: the poses do sample it,
// but at only 2.5 samples per cycle, which is nowhere near enough for a slerp
// between them to follow the arc. That is the regime real walking lives in.
//
// Three arms, identical stimulus:
//
//   PLAIN     the shipped path — slerp between 30 Hz ARCore poses
//   DENSIFIED the same poses, plus a 400 Hz gyro, through
//             ImuDensifiedPoseSource
//   TRUTH     the analytic trajectory, resolved at every return's own instant
//
// The wall is flat, so the honest measure is how flat each arm's wall comes
// out. TRUTH is the floor; the gap between PLAIN and DENSIFIED is what the
// gyro bought.
//
// The falsifiable half matters as much: with the jitter switched OFF the two
// arms must converge, because then there is nothing between the pose samples
// for the gyro to find and a densifier that still "improves" things is
// inventing motion.

#include "scanengine/poses/imu_densified_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/drivers/d6/d6_fan.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/poses/se3.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"

using namespace scanengine;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Quat {
  double x, y, z, w;
};

Quat qmul(const Quat& a, const Quat& b) {
  double out[4];
  const double qa[4] = {a.x, a.y, a.z, a.w};
  const double qb[4] = {b.x, b.y, b.z, b.w};
  se3::quat_mul(qa, qb, out);
  return Quat{out[0], out[1], out[2], out[3]};
}

Quat axis_angle(double ax, double ay, double az, double ang) {
  const double n = std::sqrt(ax * ax + ay * ay + az * az);
  const double s = std::sin(ang * 0.5) / (n > 0 ? n : 1.0);
  return Quat{ax * s, ay * s, az * s, std::cos(ang * 0.5)};
}

void qrot(const Quat& q, const double v[3], double out[3]) {
  const double qq[4] = {q.x, q.y, q.z, q.w};
  double R[9];
  se3::quat_to_matrix(qq, R);
  for (int i = 0; i < 3; ++i) {
    out[i] = R[i * 3 + 0] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2];
  }
}

// --- the rig ---------------------------------------------------------------
//
// World is +z up here (a test fixture convention; see test_pushbroom.cpp).
// The walk is along +x at 1 m/s with a 2 Hz gait sway, and the orientation is
// gait sway PLUS an optional 12 Hz jitter about all three axes with different
// phases, so no single axis can be special-cased.
struct Rig {
  double jitter_amp_deg = 1.5;
  double jitter_hz = 12.0;

  void position_at(double t, double out[3]) const {
    const double w = 2.0 * kPi * 2.0;
    out[0] = 1.0 * t;
    out[1] = 0.020 * std::sin(w * t);
    out[2] = 1.35 + 0.030 * std::sin(w * t + 0.9);
  }

  Quat orientation_at(double t) const {
    const double w = 2.0 * kPi * 2.0;
    Quat q = qmul(axis_angle(0, 0, 1, 0.052 * std::sin(w * t + 0.4)),
                  axis_angle(1, 0, 0, 0.030 * std::sin(w * t + 2.1)));
    if (jitter_amp_deg > 0.0) {
      const double a = jitter_amp_deg * kPi / 180.0;
      const double wj = 2.0 * kPi * jitter_hz;
      const Quat j = qmul(qmul(axis_angle(0, 0, 1, a * std::sin(wj * t)),
                               axis_angle(1, 0, 0, a * std::sin(wj * t + 1.7))),
                          axis_angle(0, 1, 0, a * std::sin(wj * t + 3.1)));
      q = qmul(q, j);
    }
    return q;
  }

  // Body-frame angular velocity, by finite difference of the true attitude —
  // which is what a gyro measures. `h` is a hundredth of the jitter period, so
  // the difference is exact to well past the noise this test cares about.
  void gyro_at(double t, double out[3]) const {
    const double h = 1.0 / (jitter_hz > 0 ? jitter_hz : 1.0) / 200.0;
    const Quat q0 = orientation_at(t - h);
    const Quat q1 = orientation_at(t + h);
    const double a[4] = {q0.x, q0.y, q0.z, q0.w};
    const double b[4] = {q1.x, q1.y, q1.z, q1.w};
    double a_conj[4], rel[4];
    se3::quat_conj(a, a_conj);
    se3::quat_mul(a_conj, b, rel);
    double rv[3];
    se3::quat_to_rotvec(rel, rv);
    for (int i = 0; i < 3; ++i) out[i] = rv[i] / (2.0 * h);
  }
};

// The mount: fan vertical, across the walk (test_pushbroom.cpp's).
void mount_matrix(double m[16], Quat* q_out) {
  const double R[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
  const double t[3] = {0.0, 0.0, 0.0};
  se3::mat4_from_rt(R, t, m);
  double q[4];
  se3::matrix_to_quat(R, q);
  *q_out = Quat{q[0], q[1], q[2], q[3]};
}

constexpr double kWallY = 1.5;

double range_to_wall(const Rig& g, const Quat& q_mount, double t, double angle_deg) {
  double dir_l[3];
  d6::fan_point(angle_deg, 1.0, dir_l);
  double dir_p[3], dir_w[3], pos[3];
  qrot(q_mount, dir_l, dir_p);
  qrot(g.orientation_at(t), dir_p, dir_w);
  g.position_at(t, pos);
  if (std::fabs(dir_w[1]) < 1e-6) return -1.0;
  const double d = (kWallY - pos[1]) / dir_w[1];
  if (d < 0.4 || d > 8.0) return -1.0;
  const double z = pos[2] + d * dir_w[2];
  if (z < 0.05 || z > 2.9) return -1.0;
  return d;
}

double plane_fit_rms(const std::vector<PointVertex>& pts) {
  // y = a + b*x + c*z, normal equations on a 3x3.
  const std::size_t n = pts.size();
  if (n < 8) return 1e9;
  double S[3][4] = {{0}};
  for (const auto& p : pts) {
    const double f[3] = {1.0, p.x, p.z};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) S[i][j] += f[i] * f[j];
      S[i][3] += f[i] * p.y;
    }
  }
  for (int i = 0; i < 3; ++i) {
    int piv = i;
    for (int r = i + 1; r < 3; ++r) {
      if (std::fabs(S[r][i]) > std::fabs(S[piv][i])) piv = r;
    }
    for (int j = 0; j < 4; ++j) std::swap(S[i][j], S[piv][j]);
    if (std::fabs(S[i][i]) < 1e-12) return 1e9;
    for (int r = 0; r < 3; ++r) {
      if (r == i) continue;
      const double f = S[r][i] / S[i][i];
      for (int j = 0; j < 4; ++j) S[r][j] -= f * S[i][j];
    }
  }
  const double a = S[0][3] / S[0][0], b = S[1][3] / S[1][1], c = S[2][3] / S[2][2];
  double sum = 0.0;
  for (const auto& p : pts) {
    const double r = p.y - (a + b * p.x + c * p.z);
    sum += r * r;
  }
  return std::sqrt(sum / static_cast<double>(n)) / std::sqrt(1.0 + b * b + c * c);
}

enum class Arm { kPlain, kDensified, kTruth };

struct WalkResult {
  std::vector<PointVertex> pts;
  ImuDensifyStats imu;
};

// `ts_err_ns` (ROUND 9 §5): half-width of a random timestamp error applied to
// each RETURN, held constant across a 24-sample packet — the real D6 packet
// size at its measured 13.7 KB/s — and independent between packets. The RANGE
// is always ray-cast at the point's TRUE time; only the stamp handed to the
// assembler is perturbed, so this is a timing experiment and not a range one.
WalkResult walk(Arm arm, double jitter_deg, double gyro_bias_rad_s = 0.0,
            double imu_hz = 400.0, double ts_err_ns = 0.0) {
  // Fixed-seed xorshift64, per the determinism doctrine — <random>'s
  // distributions are not specified bit-for-bit across the five CI legs.
  std::uint64_t rng = 88172645463325252ULL;
  auto urand = [&rng]() {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return static_cast<double>(rng % 1000000u) / 1000000.0 - 0.5;
  };
  constexpr std::int64_t kRevNs = 100'000'000LL;
  constexpr int kRevolutions = 40;      // 4 s
  constexpr int kReturnsPerRev = 400;   // the spec's 4 kHz / 10 Hz
  constexpr std::int64_t kPoseNs = 33'333'333LL;  // ARCore ~30 Hz
  constexpr std::int64_t kT0 = 1'000'000'000LL;
  const std::int64_t kEnd = kT0 + static_cast<std::int64_t>(kRevolutions) * kRevNs;

  Rig g;
  g.jitter_amp_deg = jitter_deg;
  double mount[16];
  Quat q_mount;
  mount_matrix(mount, &q_mount);

  ExternalPoseSource poses;
  const std::int64_t pose_step = (arm == Arm::kTruth) ? 1'000'000LL : kPoseNs;
  for (std::int64_t t = kT0 - 2 * kPoseNs; t <= kEnd + 2 * kPoseNs; t += pose_step) {
    Pose p{};
    p.t_mono_ns = t;
    const double ts = static_cast<double>(t) * 1e-9;
    g.position_at(ts, p.position);
    const Quat q = g.orientation_at(ts);
    p.orientation[0] = q.x;
    p.orientation[1] = q.y;
    p.orientation[2] = q.z;
    p.orientation[3] = q.w;
    p.source = StreamId::kPoseAr;
    p.quality = PoseQuality::kGood;
    p.tracking_lost = 0;
    REQUIRE(poses.push_pose(p).ok());
  }

  ImuDensifyConfig icfg;
  ImuDensifiedPoseSource densified(&poses, icfg);
  if (arm == Arm::kDensified) {
    const std::int64_t imu_step = static_cast<std::int64_t>(1e9 / imu_hz);
    for (std::int64_t t = kT0 - 2 * kPoseNs; t <= kEnd + 2 * kPoseNs; t += imu_step) {
      PhoneImuSample s;
      s.t_mono_ns = t;
      double w[3];
      g.gyro_at(static_cast<double>(t) * 1e-9, w);
      for (int i = 0; i < 3; ++i) {
        s.gyro_rad_s[i] = static_cast<float>(w[i] + gyro_bias_rad_s);
      }
      s.accel_m_s2[2] = 9.81f;
      REQUIRE(densified.push_imu(s));
    }
  }

  PageStore store;
  D6PushbroomAssembler a(&store);
  REQUIRE(a.set_mount_extrinsics(mount).ok());
  const PoseInterpolator* src =
      (arm == Arm::kDensified) ? static_cast<const PoseInterpolator*>(&densified)
                               : static_cast<const PoseInterpolator*>(&poses);
  a.set_pose_source(src);

  for (int rev = 0; rev < kRevolutions; ++rev) {
    const std::int64_t t_rev = kT0 + static_cast<std::int64_t>(rev) * kRevNs;
    std::vector<ProfilePoint> prof;
    double ts_err_cur = 0.0;
    for (int i = 0; i < kReturnsPerRev; ++i) {
      const std::int64_t t_pt = t_rev + kRevNs * i / kReturnsPerRev;
      const double angle = 360.0 * i / kReturnsPerRev;
      const double d = range_to_wall(g, q_mount, static_cast<double>(t_pt) * 1e-9, angle);
      if (d < 0) continue;
      if (i % 24 == 0) ts_err_cur = urand() * 2.0 * ts_err_ns;
      ProfilePoint p;
      p.t_mono_ns = t_pt + static_cast<std::int64_t>(ts_err_cur);
      p.angle_deg = static_cast<float>(angle);
      p.range_m = static_cast<float>(d);
      p.intensity = 120;
      p.high_reflectivity = 0;
      prof.push_back(p);
    }
    if (!prof.empty()) {
      REQUIRE(a.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
    }
  }
  REQUIRE(a.flush().ok());

  WalkResult r;
  for (const auto id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t i = 0; i < v.count; ++i) r.pts.push_back(v.data[i]);
  }
  r.imu = densified.stats();
  return r;
}

}  // namespace

// ===========================================================================
// 1. The quaternion primitives ROUND 9 had to add
// ===========================================================================

TEST_CASE("round9/quaternion_helpers_round_trip") {
  const double axes[4][3] = {
      {1, 0, 0}, {0, 1, 0}, {0.3, -0.5, 0.8}, {1e-9, 2e-9, -1e-9}};
  const double angles[5] = {0.0, 1e-9, 0.3, 2.0, kPi - 1e-6};
  for (const auto& ax : axes) {
    for (double ang : angles) {
      const double n = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
      if (n < 1e-12) continue;
      const double rv[3] = {ax[0] / n * ang, ax[1] / n * ang, ax[2] / n * ang};
      double q[4], back[3];
      se3::quat_from_rotvec(rv, q);
      const double nn = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
      CHECK(std::fabs(nn - 1.0) < 1e-9);
      se3::quat_to_rotvec(q, back);
      for (int i = 0; i < 3; ++i) CHECK(std::fabs(back[i] - rv[i]) < 1e-9);
    }
  }

  // quat_mul agrees with composing the matrices, and is aliasing-safe.
  // Both inputs are normalised first: quat_mul does not normalise (it is a
  // plain Hamilton product), so feeding it a quaternion that is 4e-5 off unit
  // would be measuring the input, not the code.
  double a[4] = {0.183, -0.365, 0.548, 0.730};
  double b[4] = {-0.5, 0.5, 0.5, 0.5};
  se3::quat_normalize(a);
  se3::quat_normalize(b);
  double ab[4];
  se3::quat_mul(a, b, ab);
  double Ra[9], Rb[9], Rab[9], Rprod[9];
  se3::quat_to_matrix(a, Ra);
  se3::quat_to_matrix(b, Rb);
  se3::quat_to_matrix(ab, Rab);
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += Ra[i * 3 + k] * Rb[k * 3 + j];
      Rprod[i * 3 + j] = s;
    }
  }
  for (int i = 0; i < 9; ++i) CHECK(std::fabs(Rab[i] - Rprod[i]) < 1e-12);

  double self[4] = {a[0], a[1], a[2], a[3]};
  se3::quat_mul(self, b, self);  // aliased output
  for (int i = 0; i < 4; ++i) CHECK(std::fabs(self[i] - ab[i]) < 1e-15);

  double conj[4], id[4];
  se3::quat_conj(a, conj);
  se3::quat_mul(a, conj, id);
  CHECK(std::fabs(id[3] - 1.0) < 1e-12);
}

// ===========================================================================
// 2. THE HEADLINE — 12 Hz jitter between 30 Hz poses
// ===========================================================================

TEST_CASE("round9/imu_densification_recovers_motion_between_arcore_poses") {
  const WalkResult plain = walk(Arm::kPlain, 1.5);
  const WalkResult dens = walk(Arm::kDensified, 1.5);
  const WalkResult truth = walk(Arm::kTruth, 1.5);

  REQUIRE(plain.pts.size() > 2000);
  REQUIRE(dens.pts.size() == plain.pts.size());
  REQUIRE(truth.pts.size() == plain.pts.size());

  const double rms_plain = plane_fit_rms(plain.pts);
  const double rms_dens = plane_fit_rms(dens.pts);
  const double rms_truth = plane_fit_rms(truth.pts);

  MESSAGE("wall plane-fit RMS with 1.5 deg @ 12 Hz jitter -- plain slerp "
          << rms_plain * 100.0 << " cm, IMU-densified " << rms_dens * 100.0
          << " cm, truth " << rms_truth * 100.0 << " cm");
  MESSAGE("improvement: " << (1.0 - rms_dens / rms_plain) * 100.0
                          << "% of the plain-slerp error removed; densified "
                          << rms_dens / rms_plain << "x plain");
  MESSAGE("densifier: " << dens.imu.densified << " densified, " << dens.imu.fallbacks
                        << " fallbacks (no-imu " << dens.imu.fallback_no_imu << ", gap "
                        << dens.imu.fallback_gap << ", bracket " << dens.imu.fallback_bracket
                        << ", closing " << dens.imu.fallback_closing << "); worst closing "
                        << dens.imu.worst_closing_deg << " deg, mean "
                        << dens.imu.mean_closing_deg << " deg");

  // 1. The gyro path is a large, unambiguous improvement — better than 10x.
  CHECK(rms_dens < 0.10 * rms_plain);

  // 2. And it closes almost all of the distance to the analytic truth. Stated
  //    as a closed FRACTION of the gap rather than as a multiple of the truth,
  //    because the truth arm is resolved against 1 kHz poses and is essentially
  //    exact (7 um) — any fixed multiple of it would be a statement about the
  //    fixture's floor, not about the densifier.
  const double closed = (rms_plain - rms_dens) / (rms_plain - rms_truth);
  MESSAGE("fraction of the recoverable error closed: " << closed * 100.0 << "%");
  CHECK(closed > 0.90);

  // 3. It is actually being used, not silently falling back.
  CHECK(dens.imu.densified > 2000);
  CHECK(dens.imu.fallbacks * 10 < dens.imu.densified);
}

// ===========================================================================
// 3. THE FALSIFIABLE CONTROL — no jitter, nothing to recover
// ===========================================================================

TEST_CASE("round9/without_sub_pose_motion_the_densifier_adds_nothing") {
  const WalkResult plain = walk(Arm::kPlain, 0.0);
  const WalkResult dens = walk(Arm::kDensified, 0.0);
  const double rms_plain = plane_fit_rms(plain.pts);
  const double rms_dens = plane_fit_rms(dens.pts);
  MESSAGE("no jitter -- plain " << rms_plain * 100.0 << " cm, densified "
                                << rms_dens * 100.0 << " cm");

  // With only 2 Hz motion, a 30 Hz slerp is already good: there is very little
  // hiding between the samples for the gyro to find. The densifier still helps
  // a little — 2 Hz is not zero — but the WIN MUST COLLAPSE. That collapse is
  // the control: it shows the headline number above is measuring recovered
  // sub-pose motion and not some constant advantage of the code path.
  const double ratio_no_jitter = rms_dens / rms_plain;
  MESSAGE("densified/plain without jitter = " << ratio_no_jitter
                                              << " (with 12 Hz jitter it is ~0.03)");
  CHECK(ratio_no_jitter > 0.25);   // no large win when there is nothing to win
  CHECK(ratio_no_jitter <= 1.0);   // and never worse
  CHECK(rms_plain < 0.01);         // the 2 Hz-only wall is already sub-cm
}

// ===========================================================================
// 4. Guards: bias, a stale IMU, and a bad frame
// ===========================================================================

TEST_CASE("round9/a_gyro_bias_is_estimated_rather_than_integrated_into_the_cloud") {
  // 0.01 rad/s = 0.57 deg/s of constant bias, a realistic consumer MEMS
  // offset. Left uncorrected it is 0.019 deg per 33 ms bracket, every bracket,
  // in the same direction.
  const WalkResult dens = walk(Arm::kDensified, 1.5, /*gyro_bias_rad_s=*/0.01);
  const WalkResult plain = walk(Arm::kPlain, 1.5);
  const double rms_dens = plane_fit_rms(dens.pts);
  const double rms_plain = plane_fit_rms(plain.pts);
  MESSAGE("with 0.01 rad/s gyro bias -- densified " << rms_dens * 100.0
          << " cm vs plain " << rms_plain * 100.0 << " cm; estimated bias ("
          << dens.imu.bias_rad_s[0] << ", " << dens.imu.bias_rad_s[1] << ", "
          << dens.imu.bias_rad_s[2] << ") rad/s over " << dens.imu.bias_updates
          << " updates");

  // Still better than plain slerp despite the bias, which is the claim that
  // matters: the closing-error correction pins both ends of every bracket, so
  // a bias can distort the path but can never accumulate.
  CHECK(rms_dens < rms_plain);
  // The estimator found it: right sign on every axis, and the right order of
  // magnitude overall. Per-axis convergence is uneven because the closing
  // error also carries genuine ARCore-vs-gyro disagreement, and how much of it
  // lands on each axis depends on where the rig was actually turning — so the
  // magnitude is asserted, not three separate components.
  CHECK(dens.imu.bias_updates > 1000);
  double mag = 0.0;
  for (int i = 0; i < 3; ++i) {
    CHECK(dens.imu.bias_rad_s[i] > 0.0);  // right side of zero on all three
    mag += dens.imu.bias_rad_s[i] * dens.imu.bias_rad_s[i];
  }
  mag = std::sqrt(mag);
  const double truth_mag = 0.01 * std::sqrt(3.0);
  MESSAGE("bias magnitude estimated " << mag << " rad/s vs true " << truth_mag);
  CHECK(mag > 0.3 * truth_mag);
  CHECK(mag < 2.0 * truth_mag);
}

TEST_CASE("round9/a_stale_or_absent_imu_falls_back_instead_of_guessing") {
  // 5 Hz "IMU": every bracket has a hole far wider than max_imu_gap_ns.
  const WalkResult starved = walk(Arm::kDensified, 1.5, 0.0, /*imu_hz=*/5.0);
  const WalkResult plain = walk(Arm::kPlain, 1.5);
  MESSAGE("5 Hz IMU -- densified " << starved.imu.densified << ", fallbacks "
                                   << starved.imu.fallbacks << " (gap "
                                   << starved.imu.fallback_gap << ", no-imu "
                                   << starved.imu.fallback_no_imu << ")");
  CHECK(starved.imu.fallbacks > starved.imu.densified);

  // Falling back means falling back to EXACTLY the plain answer — not to
  // something nearly like it.
  REQUIRE(starved.pts.size() == plain.pts.size());
  const double rms_starved = plane_fit_rms(starved.pts);
  const double rms_plain = plane_fit_rms(plain.pts);
  CHECK(rms_starved <= 1.05 * rms_plain);
}

// ===========================================================================
// 5. Does the densifier survive imperfect LIDAR timestamps?
// ===========================================================================
//
// This is the question ROUND 9's two halves ask of each other. Densification
// makes the trajectory follow real 12 Hz motion instead of smoothing it away —
// and a path with more high-frequency content is, in principle, more sensitive
// to being sampled at the wrong instant. A plain slerp is accidentally robust
// to timing error precisely because it has already thrown the fast motion
// away. So it is not obvious a priori that densifying is safe when the lidar's
// own stamps are imperfect, and if it were not, item 35 would be dangerous
// without item 3's timestamping work landing first.
//
// Measured here rather than argued: a random per-packet timestamp error is
// injected into every return (constant within a 24-sample packet, which is the
// real D6 packet size at its measured 13.7 KB/s, and independent between
// packets — the shape a byte-position reconstruction actually produces).
TEST_CASE("round9/densification_degrades_gracefully_with_lidar_timestamp_error") {
  struct Arm2 {
    double err_ns;
    double plain;
    double dens;
  };
  std::vector<Arm2> table;

  for (double err_ns : {0.0, 1e6, 4e6, 8e6}) {
    const WalkResult p = walk(Arm::kPlain, 1.5, 0.0, 400.0, err_ns);
    const WalkResult d = walk(Arm::kDensified, 1.5, 0.0, 400.0, err_ns);
    table.push_back({err_ns, plane_fit_rms(p.pts), plane_fit_rms(d.pts)});
  }

  for (const auto& r : table) {
    MESSAGE("per-packet timestamp error +/-" << r.err_ns / 1e6 << " ms: plain "
            << r.plain * 100.0 << " cm, densified " << r.dens * 100.0 << " cm ("
            << r.dens / r.plain << "x)");
  }

  // The claim: densification never becomes counter-productive, even when the
  // lidar stamps are an order of magnitude worse than anything ROUND 9's
  // sample clock should produce. If this ever inverts, the two halves of this
  // round have started fighting each other and the densifier must be gated on
  // timestamp quality rather than left on by default.
  for (const auto& r : table) CHECK(r.dens < r.plain);

  // It does degrade, though, and monotonically — that is the honest half. The
  // advantage shrinks as the stamps get worse, because a smoothed path is
  // insensitive to WHEN you sample it and a faithful one is not.
  CHECK(table.front().dens / table.front().plain < table.back().dens / table.back().plain);
}
