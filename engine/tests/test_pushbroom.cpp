// test_pushbroom.cpp — A8: SE(3) helpers, ExternalPoseSource, and the D6
// pushbroom assembler.
//
// These four headers are included FIRST, alone, so this file doubles as the
// self-containment check test_headers.cpp performs for the A1 seams (that
// file is not A8's to edit).
#include "scanengine/poses/se3.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"
#include "scanengine/drivers/d6/d6_fan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"

using namespace scanengine;

namespace {

constexpr double kPi = se3::kPi;

// ---------------------------------------------------------------------------
// Test-side geometry, written INDEPENDENTLY of se3.h wherever it is used to
// check se3.h: the ground-truth world point is produced by the quaternion
// sandwich v' = v + 2w(u x v) + 2u x (u x v), never by a 4x4 product.
// ---------------------------------------------------------------------------

struct Quat {
  double x = 0, y = 0, z = 0, w = 1;
};

// Rotate `v` by unit quaternion `q`.
void qrot(const Quat& q, const double v[3], double out[3]) {
  const double u[3] = {q.x, q.y, q.z};
  double uv[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                  u[0] * v[1] - u[1] * v[0]};
  double uuv[3] = {u[1] * uv[2] - u[2] * uv[1], u[2] * uv[0] - u[0] * uv[2],
                   u[0] * uv[1] - u[1] * uv[0]};
  for (int i = 0; i < 3; ++i) out[i] = v[i] + 2.0 * q.w * uv[i] + 2.0 * uuv[i];
}

Quat axis_angle(double ax, double ay, double az, double angle_rad) {
  const double n = std::sqrt(ax * ax + ay * ay + az * az);
  const double s = std::sin(angle_rad * 0.5) / (n > 0 ? n : 1.0);
  return Quat{ax * s, ay * s, az * s, std::cos(angle_rad * 0.5)};
}

Quat qmul(const Quat& a, const Quat& b) {
  return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The trajectory the assembler tests are written against: a constant-velocity
// walk with a constant yaw rate.
//
// Both halves are deliberately EXACTLY representable by the interpolator —
// a straight line is exactly what position lerp reproduces, and a constant
// angular velocity is exactly what SLERP reproduces. That is what lets the
// ground-truth assertions run at micrometre tolerance: any disagreement is a
// wrong frame or a wrong composition order, never the interpolator's
// modelling error. (Add a vertical bob and the lerp error alone is ~185 um
// across a 33 ms ARCore interval, which would mask exactly the bugs this
// test is for.)
struct Trajectory {
  double vx = 1.0;       // m/s along world +x
  double yaw_rate = 0.5; // rad/s about world +z

  void position_at(double t, double out[3]) const {
    out[0] = vx * t;
    out[1] = 0.15 * t;
    out[2] = 1.4 - 0.03 * t;
  }
  Quat orientation_at(double t) const {
    // Constant yaw rate composed with a FIXED tilt. A time-varying second
    // axis would break the "slerp is exact" property this file relies on.
    const Quat yaw = axis_angle(0, 0, 1, yaw_rate * t);
    const Quat tilt = axis_angle(1, 0, 0, 0.12);
    return qmul(yaw, tilt);
  }
};

// The S6 case-(a) mount: D6 vertical on the bracket, ~15 cm from the camera.
// lidar +x -> phone +z (forward), lidar +y -> phone -y (up), lidar +z -> phone +x.
void d6_mount(double m[16], Quat* q_out, double t_out[3]) {
  const double R[9] = {0, 0, 1,
                       0, -1, 0,
                       1, 0, 0};
  const double t[3] = {0.015, 0.150, -0.030};
  se3::mat4_from_rt(R, t, m);
  double q[4];
  se3::matrix_to_quat(R, q);
  *q_out = Quat{q[0], q[1], q[2], q[3]};
  for (int i = 0; i < 3; ++i) t_out[i] = t[i];
}

Pose make_pose(std::int64_t t_ns, const Trajectory& traj, PoseQuality qual = PoseQuality::kGood,
               std::uint8_t lost = 0) {
  const double t = static_cast<double>(t_ns) * 1e-9;
  Pose p;
  p.t_mono_ns = t_ns;
  traj.position_at(t, p.position);
  const Quat q = traj.orientation_at(t);
  p.orientation[0] = q.x;
  p.orientation[1] = q.y;
  p.orientation[2] = q.z;
  p.orientation[3] = q.w;
  p.source = StreamId::kPoseAr;
  p.quality = qual;
  p.tracking_lost = lost;
  return p;
}

// Ground truth for one D6 return, computed with quaternions only.
void expected_world(const Trajectory& traj, const Quat& q_mount, const double t_mount[3],
                    std::int64_t t_ns, double angle_deg, double range_m, double out[3]) {
  const double t = static_cast<double>(t_ns) * 1e-9;
  // The fan frame comes from the ONE production definition (d6_fan.h) — the
  // point of this ground truth is to check the COMPOSITION independently (via
  // a quaternion sandwich, never a 4x4 product), not to re-derive the sensor
  // convention and risk the two silently disagreeing. ROUND 9 item 34 is what
  // happens when they do.
  double p_l[3];
  d6::fan_point(angle_deg, range_m, p_l);
  double p_phone[3];
  qrot(q_mount, p_l, p_phone);
  for (int i = 0; i < 3; ++i) p_phone[i] += t_mount[i];
  double p_world[3];
  qrot(traj.orientation_at(t), p_phone, p_world);
  double pos[3];
  traj.position_at(t, pos);
  for (int i = 0; i < 3; ++i) out[i] = p_world[i] + pos[i];
}

std::vector<PointVertex> read_all(const PageStore& store) {
  std::vector<PointVertex> out;
  for (PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    for (std::uint32_t i = 0; i < v.count; ++i) out.push_back(v.data[i]);
  }
  return out;
}

// A profile of `n` returns spanning the full 360 degrees, timestamped across
// one 100 ms revolution (the D6 spins at 10 Hz).
std::vector<ProfilePoint> make_revolution(std::int64_t t0_ns, int n, double base_range) {
  std::vector<ProfilePoint> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    ProfilePoint p;
    p.t_mono_ns = t0_ns + static_cast<std::int64_t>(100'000'000LL * i / n);
    p.angle_deg = static_cast<float>(360.0 * i / n);
    p.range_m = static_cast<float>(base_range + 0.5 * std::sin(0.11 * i));
    p.intensity = static_cast<std::uint8_t>(40 + (i % 200));
    p.high_reflectivity = (i % 37 == 0) ? 1 : 0;
    out.push_back(p);
  }
  return out;
}

}  // namespace

// ===========================================================================
// SE(3) helpers
// ===========================================================================

TEST_CASE("se3/quaternion_matrix_roundtrip") {
  const double axes[4][3] = {{1, 0, 0}, {0, 1, 0}, {0.3, -0.5, 0.81}, {1, 1, 1}};
  const double angles[5] = {0.0, 0.3, 1.7, kPi - 1e-4, 2.9};
  for (const auto& ax : axes) {
    for (double ang : angles) {
      const Quat q = axis_angle(ax[0], ax[1], ax[2], ang);
      const double qa[4] = {q.x, q.y, q.z, q.w};
      double R[9];
      se3::quat_to_matrix(qa, R);
      double qb[4];
      se3::matrix_to_quat(R, qb);
      double Rb[9];
      se3::quat_to_matrix(qb, Rb);
      for (int i = 0; i < 9; ++i) CHECK(std::fabs(R[i] - Rb[i]) < 1e-12);
      // And the rotation itself agrees with the independent sandwich form.
      const double v[3] = {0.31, -1.7, 0.9};
      double via_q[3], via_m[3];
      qrot(q, v, via_q);
      double Rm[9];
      se3::quat_to_matrix(qa, Rm);
      via_m[0] = Rm[0] * v[0] + Rm[1] * v[1] + Rm[2] * v[2];
      via_m[1] = Rm[3] * v[0] + Rm[4] * v[1] + Rm[5] * v[2];
      via_m[2] = Rm[6] * v[0] + Rm[7] * v[1] + Rm[8] * v[2];
      for (int i = 0; i < 3; ++i) CHECK(std::fabs(via_q[i] - via_m[i]) < 1e-12);
    }
  }
}

TEST_CASE("se3/exp_log_roundtrip_including_near_pi") {
  const double vecs[5][3] = {{0, 0, 0}, {1e-10, 0, 0}, {0.4, -0.2, 0.1},
                             {0, 0, kPi - 1e-7}, {1.0, 1.0, 1.0}};
  for (const auto& w : vecs) {
    double R[9], back[3];
    se3::so3_exp(w, R);
    se3::so3_log(R, back);
    double R2[9];
    se3::so3_exp(back, R2);
    for (int i = 0; i < 9; ++i) CHECK(std::fabs(R[i] - R2[i]) < 1e-9);
  }
}

TEST_CASE("se3/slerp_takes_the_short_way_round") {
  // 170 degrees apart with the SECOND quaternion negated: the same rotation,
  // the opposite sign. Without the sign fix the interpolation goes the long
  // way and the midpoint lands 180 degrees away from where it should.
  const Quat a = axis_angle(0, 0, 1, 0.0);
  const Quat b = axis_angle(0, 0, 1, 170.0 * kPi / 180.0);
  const double qa[4] = {a.x, a.y, a.z, a.w};
  const double qb_neg[4] = {-b.x, -b.y, -b.z, -b.w};
  double mid[4];
  se3::quat_slerp(qa, qb_neg, 0.5, mid);

  const Quat want = axis_angle(0, 0, 1, 85.0 * kPi / 180.0);
  const double v[3] = {1, 0, 0};
  double got[3], expect[3];
  double Rm[9];
  se3::quat_to_matrix(mid, Rm);
  got[0] = Rm[0] * v[0] + Rm[1] * v[1] + Rm[2] * v[2];
  got[1] = Rm[3] * v[0] + Rm[4] * v[1] + Rm[5] * v[2];
  got[2] = Rm[6] * v[0] + Rm[7] * v[1] + Rm[8] * v[2];
  qrot(want, v, expect);
  for (int i = 0; i < 3; ++i) CHECK(std::fabs(got[i] - expect[i]) < 1e-9);
}

TEST_CASE("se3/rigid_inverse_and_rigidity_check") {
  double m[16];
  Quat qm;
  double tm[3];
  d6_mount(m, &qm, tm);
  CHECK(se3::mat4_is_rigid(m));

  double inv[16], round[16];
  se3::mat4_inverse_rigid(m, inv);
  se3::mat4_mul(m, inv, round);
  double ident[16];
  se3::mat4_identity(ident);
  for (int i = 0; i < 16; ++i) CHECK(std::fabs(round[i] - ident[i]) < 1e-12);

  // The two failure modes the check exists for.
  double scaled[16];
  for (int i = 0; i < 16; ++i) scaled[i] = m[i];
  for (int i = 0; i < 3; ++i) scaled[i] *= 1.01;
  CHECK_FALSE(se3::mat4_is_rigid(scaled));

  double mirrored[16];
  const double Rmir[9] = {1, 0, 0, 0, 1, 0, 0, 0, -1};  // det = -1
  const double zero[3] = {0, 0, 0};
  se3::mat4_from_rt(Rmir, zero, mirrored);
  CHECK_FALSE(se3::mat4_is_rigid(mirrored));
}

// ===========================================================================
// ExternalPoseSource
// ===========================================================================

TEST_CASE("poses/interpolates_a_constant_rate_trajectory_exactly") {
  Trajectory traj;
  ExternalPoseConfig cfg;
  cfg.max_gap_ns = 200'000'000;
  ExternalPoseSource src(cfg);
  REQUIRE(src.start().ok());

  // 30 Hz, like ARCore.
  for (int i = 0; i <= 60; ++i) {
    REQUIRE(src.push_pose(make_pose(static_cast<std::int64_t>(i) * 33'333'333LL, traj)).ok());
  }

  // Sample BETWEEN knots. Constant velocity + constant angular velocity means
  // lerp and slerp are the exact trajectory, so the tolerance is numerical.
  for (int k = 0; k < 200; ++k) {
    const std::int64_t t = 100'000'000LL + static_cast<std::int64_t>(k) * 4'321'987LL;
    const PoseSample s = src.sample_at(t);
    REQUIRE(s.ok());
    double want_pos[3];
    traj.position_at(static_cast<double>(t) * 1e-9, want_pos);
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(s.pose.position[i] - want_pos[i]) < 1e-9);

    const Quat want_q = traj.orientation_at(static_cast<double>(t) * 1e-9);
    const double v[3] = {1.0, 0.3, -0.7};
    double want_v[3], got_v[3];
    qrot(want_q, v, want_v);
    double R[9];
    se3::quat_to_matrix(s.pose.orientation, R);
    got_v[0] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
    got_v[1] = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
    got_v[2] = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(got_v[i] - want_v[i]) < 1e-7);
  }
}

TEST_CASE("poses/edge_cases_before_first_at_knot_and_in_the_future") {
  Trajectory traj;
  ExternalPoseSource src;
  CHECK(src.sample_at(1000).gate == PoseGate::kNoData);

  REQUIRE(src.push_pose(make_pose(1'000'000'000LL, traj)).ok());
  REQUIRE(src.push_pose(make_pose(1'033'000'000LL, traj)).ok());

  CHECK(src.sample_at(999'999'999LL).gate == PoseGate::kBeforeFirst);
  CHECK_FALSE(src.sample_at(999'999'999LL).has_pose);
  CHECK(src.sample_at(2'000'000'000LL).gate == PoseGate::kFuture);
  CHECK(src.sample_at(2'000'000'000LL).retryable());

  // Exactly on a pushed knot: the pose comes back untouched, gap 0.
  const PoseSample at_knot = src.sample_at(1'033'000'000LL);
  REQUIRE(at_knot.ok());
  CHECK(at_knot.bracket_gap_ns == 0);
  Pose want = make_pose(1'033'000'000LL, traj);
  for (int i = 0; i < 3; ++i) CHECK(std::fabs(at_knot.pose.position[i] - want.position[i]) < 1e-12);

  // The narrow PoseSource contract is honoured too.
  Pose out;
  CHECK(src.pose_at(999'999'999LL, &out).error() == ScanError::kNotFound);
  CHECK(src.pose_at(2'000'000'000LL, &out).error() == ScanError::kAgain);
  CHECK(src.pose_at(1'010'000'000LL, &out).ok());
}

TEST_CASE("poses/gates_stale_gaps_low_confidence_and_tracking_loss") {
  Trajectory traj;
  ExternalPoseConfig cfg;
  cfg.max_gap_ns = 100'000'000;
  cfg.min_confidence = 0.5f;
  ExternalPoseSource src(cfg);

  REQUIRE(src.push_pose(make_pose(0, traj)).ok());
  REQUIRE(src.push_pose(make_pose(50'000'000LL, traj)).ok());
  // A 400 ms hole: interpolating across it is fiction.
  REQUIRE(src.push_pose(make_pose(450'000'000LL, traj)).ok());
  // Tracking lost for the next interval.
  REQUIRE(src.push_pose(make_pose(480'000'000LL, traj, PoseQuality::kPoor, 1)).ok());
  REQUIRE(src.push_pose(make_pose(510'000'000LL, traj, PoseQuality::kPoor, 0)).ok());
  REQUIRE(src.push_pose(make_pose(540'000'000LL, traj, PoseQuality::kGood, 0)).ok());

  CHECK(src.sample_at(25'000'000LL).gate == PoseGate::kOk);
  CHECK(src.sample_at(200'000'000LL).gate == PoseGate::kStale);
  CHECK(src.sample_at(200'000'000LL).has_pose);   // geometry still available
  CHECK(src.sample_at(200'000'000LL).flagged());
  CHECK(src.sample_at(460'000'000LL).gate == PoseGate::kTrackingLost);
  CHECK(src.sample_at(495'000'000LL).gate == PoseGate::kTrackingLost);
  // Recovered but still kPoor -> confidence 0.25 < 0.5.
  CHECK(src.sample_at(520'000'000LL).gate == PoseGate::kLowConfidence);

  // An explicit confidence from the pusher wins over the derived one.
  ExternalPoseSource src2(cfg);
  Pose p = make_pose(0, traj, PoseQuality::kPoor);
  REQUIRE(src2.push_pose(p, 0.9f).ok());
  p = make_pose(30'000'000LL, traj, PoseQuality::kPoor);
  REQUIRE(src2.push_pose(p, 0.9f).ok());
  CHECK(src2.sample_at(15'000'000LL).gate == PoseGate::kOk);
}

TEST_CASE("poses/rejects_bad_input_and_rolls_the_ring") {
  Trajectory traj;
  ExternalPoseConfig cfg;
  cfg.capacity = 8;
  ExternalPoseSource src(cfg);

  Pose bad = make_pose(0, traj);
  bad.position[1] = std::nan("");
  CHECK(src.push_pose(bad).error() == ScanError::kInvalidArgument);

  Pose zeroq = make_pose(0, traj);
  for (int i = 0; i < 4; ++i) zeroq.orientation[i] = 0.0;
  CHECK(src.push_pose(zeroq).error() == ScanError::kInvalidArgument);

  for (int i = 0; i < 20; ++i) {
    REQUIRE(src.push_pose(make_pose(static_cast<std::int64_t>(i) * 10'000'000LL, traj)).ok());
  }
  // Out of order: a VIO trajectory is monotone; accepting a rewind would
  // corrupt every interpolation after it.
  CHECK(src.push_pose(make_pose(50'000'000LL, traj)).error() == ScanError::kInvalidArgument);

  CHECK(src.size() == 8);
  std::int64_t first = 0, last = 0;
  REQUIRE(src.time_span(&first, &last));
  CHECK(first == 120'000'000LL);
  CHECK(last == 190'000'000LL);
  // The rolled-off region is unresolvable, not "future".
  CHECK(src.sample_at(50'000'000LL).gate == PoseGate::kBeforeFirst);

  const ExternalPoseStats st = src.stats();
  CHECK(st.pushed == 20);
  CHECK(st.rejected_invalid == 2);
  CHECK(st.rejected_out_of_order == 1);
  CHECK(st.overwritten == 12);
}

TEST_CASE("poses/extrapolation_holds_the_last_pose_and_is_off_by_default") {
  Trajectory traj;
  ExternalPoseSource strict;
  REQUIRE(strict.push_pose(make_pose(0, traj)).ok());
  REQUIRE(strict.push_pose(make_pose(30'000'000LL, traj)).ok());
  CHECK(strict.sample_at(40'000'000LL).gate == PoseGate::kFuture);

  ExternalPoseConfig cfg;
  cfg.max_extrapolation_ns = 50'000'000;
  ExternalPoseSource loose(cfg);
  REQUIRE(loose.push_pose(make_pose(0, traj)).ok());
  REQUIRE(loose.push_pose(make_pose(30'000'000LL, traj)).ok());
  const PoseSample s = loose.sample_at(60'000'000LL);
  CHECK(s.ok());
  // HELD, not projected: identical to the last pose, no invented motion.
  Pose last = make_pose(30'000'000LL, traj);
  for (int i = 0; i < 3; ++i) CHECK(std::fabs(s.pose.position[i] - last.position[i]) < 1e-12);
  CHECK(loose.sample_at(90'000'000LL).gate == PoseGate::kFuture);
}

// ===========================================================================
// The assembler
// ===========================================================================

TEST_CASE("pushbroom/world_points_match_the_analytic_ground_truth") {
  Trajectory traj;
  double mount[16];
  Quat q_mount;
  double t_mount[3];
  d6_mount(mount, &q_mount, t_mount);

  ExternalPoseSource poses;
  for (int i = 0; i <= 120; ++i) {
    REQUIRE(poses.push_pose(make_pose(static_cast<std::int64_t>(i) * 33'333'333LL, traj)).ok());
  }

  PageStore store;
  D6PushbroomAssembler asm_(&store);
  REQUIRE(asm_.set_mount_extrinsics(mount).ok());
  asm_.set_pose_source(&poses);

  // Ten revolutions, 400 returns each, inside the pose span.
  std::vector<ProfilePoint> all;
  for (int rev = 0; rev < 10; ++rev) {
    const std::int64_t t0 = 200'000'000LL + static_cast<std::int64_t>(rev) * 100'000'000LL;
    const auto prof = make_revolution(t0, 400, 2.5);
    REQUIRE(asm_.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
    for (const auto& p : prof) all.push_back(p);
  }
  REQUIRE(asm_.flush().ok());

  const auto got = read_all(store);
  REQUIRE(got.size() == all.size());
  CHECK(asm_.stats().points_out == all.size());
  CHECK(asm_.stats().dropped_no_pose == 0);
  CHECK(asm_.stats().flagged_total() == 0);

  double worst = 0.0;
  for (std::size_t i = 0; i < all.size(); ++i) {
    double want[3];
    expected_world(traj, q_mount, t_mount, all[i].t_mono_ns, all[i].angle_deg, all[i].range_m,
                   want);
    const double dx = got[i].x - want[0];
    const double dy = got[i].y - want[1];
    const double dz = got[i].z - want[2];
    const double e = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (e > worst) worst = e;
    // Colour convention must match D6Driver's live preview.
    CHECK(got[i].r == all[i].intensity);
    CHECK(got[i].a == 255);
  }
  // Micrometres: the only error left is float32 storage of a ~4 m coordinate
  // (~0.5 um) plus SLERP round-off. Anything larger means a frame, an order of
  // composition, or a timestamp is wrong.
  MESSAGE("worst world-point error: " << worst * 1e6 << " um over " << all.size() << " points");
  CHECK(worst < 1e-5);
}

TEST_CASE("pushbroom/angle_wraparound_is_continuous") {
  Trajectory traj;
  double mount[16];
  Quat q_mount;
  double t_mount[3];
  d6_mount(mount, &q_mount, t_mount);

  ExternalPoseSource poses;
  for (int i = 0; i <= 30; ++i) {
    REQUIRE(poses.push_pose(make_pose(static_cast<std::int64_t>(i) * 33'333'333LL, traj)).ok());
  }
  PageStore store;
  D6PushbroomAssembler asm_(&store);
  REQUIRE(asm_.set_mount_extrinsics(mount).ok());
  asm_.set_pose_source(&poses);

  // Angles straddling the 360 -> 0 seam, all at the SAME instant so the only
  // variable is the bearing.
  const double angles[6] = {358.0, 359.0, 359.9, 0.0, 0.1, 1.0};
  std::vector<ProfilePoint> prof;
  for (double a : angles) {
    ProfilePoint p;
    p.t_mono_ns = 500'000'000LL;
    p.angle_deg = static_cast<float>(a);
    p.range_m = 3.0f;
    p.intensity = 100;
    prof.push_back(p);
  }
  REQUIRE(asm_.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
  REQUIRE(asm_.flush().ok());

  const auto got = read_all(store);
  REQUIRE(got.size() == 6);
  for (std::size_t i = 0; i < 6; ++i) {
    double want[3];
    expected_world(traj, q_mount, t_mount, 500'000'000LL, angles[i], 3.0, want);
    CHECK(std::fabs(got[i].x - want[0]) < 1e-5);
    CHECK(std::fabs(got[i].y - want[1]) < 1e-5);
    CHECK(std::fabs(got[i].z - want[2]) < 1e-5);
  }
  // 359.9 and 0.1 are 0.2 degrees apart: at 3 m that is ~10 mm, and nothing
  // about crossing the seam may add to it.
  const double d = std::sqrt(std::pow(got[2].x - got[4].x, 2) + std::pow(got[2].y - got[4].y, 2) +
                             std::pow(got[2].z - got[4].z, 2));
  CHECK(d < 0.012);
}

TEST_CASE("pushbroom/cartesian_seam_overload_agrees_with_the_polar_path") {
  Trajectory traj;
  double mount[16];
  Quat q_mount;
  double t_mount[3];
  d6_mount(mount, &q_mount, t_mount);

  ExternalPoseSource poses;
  for (int i = 0; i <= 30; ++i) {
    REQUIRE(poses.push_pose(make_pose(static_cast<std::int64_t>(i) * 33'333'333LL, traj)).ok());
  }

  const auto prof = make_revolution(400'000'000LL, 90, 2.0);
  // The same returns as sensor-frame PointVertex, the way D6Driver emits them.
  std::vector<PointVertex> cart;
  for (const auto& p : prof) {
    double p_l[3];
    d6::fan_point(static_cast<double>(p.angle_deg), static_cast<double>(p.range_m), p_l);
    PointVertex v{};
    v.x = static_cast<float>(p_l[0]);
    v.y = static_cast<float>(p_l[1]);
    v.z = 0.0f;
    v.r = v.b = p.intensity;
    v.g = p.high_reflectivity ? 255 : p.intensity;
    cart.push_back(v);
  }

  PageStore store_polar, store_cart;
  D6PushbroomAssembler a_polar(&store_polar), a_cart(&store_cart);
  for (auto* a : {&a_polar, &a_cart}) {
    REQUIRE(a->set_mount_extrinsics(mount).ok());
    a->set_pose_source(&poses);
  }
  // The coarse overload shares one stamp, so give the polar path one too.
  std::vector<ProfilePoint> prof_flat = prof;
  for (auto& p : prof_flat) p.t_mono_ns = 400'000'000LL;

  REQUIRE(a_polar.push_profile(Span<const ProfilePoint>(prof_flat.data(), prof_flat.size())).ok());
  REQUIRE(a_polar.flush().ok());
  REQUIRE(a_cart.push_profile(Span<const PointVertex>(cart.data(), cart.size()), 400'000'000LL)
              .ok());
  REQUIRE(a_cart.flush().ok());

  const auto gp = read_all(store_polar);
  const auto gc = read_all(store_cart);
  REQUIRE(gp.size() == gc.size());
  for (std::size_t i = 0; i < gp.size(); ++i) {
    CHECK(std::fabs(gp[i].x - gc[i].x) < 2e-4);
    CHECK(std::fabs(gp[i].y - gc[i].y) < 2e-4);
    CHECK(std::fabs(gp[i].z - gc[i].z) < 2e-4);
    CHECK(gp[i].g == gc[i].g);  // high-reflectivity tint survives the round trip
  }
}

TEST_CASE("pushbroom/tracking_loss_points_are_excluded_by_default_and_flagged_when_kept") {
  Trajectory traj;
  double mount[16];
  Quat q_mount;
  double t_mount[3];
  d6_mount(mount, &q_mount, t_mount);

  // 3 s of poses at 30 Hz; ARCore loses tracking for 400 ms in the middle.
  ExternalPoseSource poses;
  for (int i = 0; i <= 90; ++i) {
    const std::int64_t t = static_cast<std::int64_t>(i) * 33'333'333LL;
    const bool lost = (t >= 1'000'000'000LL && t < 1'400'000'000LL);
    REQUIRE(poses.push_pose(make_pose(t, traj, lost ? PoseQuality::kPoor : PoseQuality::kGood,
                                      lost ? 1 : 0))
                .ok());
  }

  std::vector<ProfilePoint> prof;
  for (int i = 0; i < 900; ++i) {
    ProfilePoint p;
    p.t_mono_ns = 100'000'000LL + static_cast<std::int64_t>(i) * 3'000'000LL;  // 0.1 .. 2.8 s
    p.angle_deg = static_cast<float>((i * 7) % 360);
    p.range_m = 2.0f;
    p.intensity = 128;
    prof.push_back(p);
  }

  // Default: excluded.
  {
    PageStore store;
    D6PushbroomAssembler a(&store);
    REQUIRE(a.set_mount_extrinsics(mount).ok());
    a.set_pose_source(&poses);
    REQUIRE(a.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
    REQUIRE(a.flush().ok());
    const PushbroomStats s = a.stats();
    CHECK(s.points_in == 900);
    CHECK(s.flagged_tracking_lost > 100);   // ~433 ms of a 3 ms cadence
    CHECK(s.flagged_tracking_lost < 200);
    CHECK(s.flagged_emitted == 0);
    CHECK(s.points_out == 900 - s.flagged_total());
    CHECK(read_all(store).size() == s.points_out);
  }

  // Configured to keep them: same count in, every flagged point marked.
  {
    PushbroomConfig cfg;
    cfg.exclude_flagged = false;
    cfg.flagged_alpha = 64;
    PageStore store;
    D6PushbroomAssembler a(&store, cfg);
    REQUIRE(a.set_mount_extrinsics(mount).ok());
    a.set_pose_source(&poses);
    REQUIRE(a.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
    REQUIRE(a.flush().ok());
    const PushbroomStats s = a.stats();
    CHECK(s.points_out == 900);
    CHECK(s.flagged_emitted == s.flagged_total());
    std::uint64_t marked = 0;
    for (const auto& v : read_all(store)) {
      if (v.a == 64) ++marked;
    }
    CHECK(marked == s.flagged_total());
  }
}

TEST_CASE("pushbroom/points_wait_for_their_pose_instead_of_being_dropped") {
  Trajectory traj;
  double mount[16];
  Quat q_mount;
  double t_mount[3];
  d6_mount(mount, &q_mount, t_mount);

  ExternalPoseSource poses;
  PageStore store;
  D6PushbroomAssembler a(&store);
  REQUIRE(a.set_mount_extrinsics(mount).ok());
  a.set_pose_source(&poses);

  // Points first, poses second — the real ordering, since ARCore delivers at
  // 30 Hz behind a 4 kpts/s point stream.
  const auto prof = make_revolution(100'000'000LL, 200, 2.0);
  REQUIRE(a.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
  CHECK(a.pending() == 200);
  CHECK(a.stats().points_out == 0);
  CHECK(a.stats().dropped_no_pose == 0);

  for (int i = 0; i <= 10; ++i) {
    REQUIRE(poses.push_pose(make_pose(static_cast<std::int64_t>(i) * 33'333'333LL, traj)).ok());
  }
  REQUIRE(a.drain().ok());
  CHECK(a.pending() == 0);
  REQUIRE(a.flush().ok());
  CHECK(a.stats().points_out == 200);
  CHECK(a.stats().dropped_no_pose == 0);

  // A point that predates the pose stream can never be resolved and is
  // dropped, not held forever.
  ProfilePoint early;
  early.t_mono_ns = -1;
  early.angle_deg = 10.0f;
  early.range_m = 1.0f;
  REQUIRE(a.push_point(early).ok());
  REQUIRE(a.drain().ok());
  CHECK(a.stats().dropped_no_pose == 1);
}

TEST_CASE("pushbroom/assembles_identically_live_and_offline") {
  // Tech Spec §3 key rule 2: replay == capture. The assembler reads no clock,
  // so a capture in which poses and points interleave must produce byte-for-
  // byte the same cloud as an offline pass over a replayed .lscan, where every
  // pose is already available before the first point.
  Trajectory traj;
  double mount[16];
  Quat q_mount;
  double t_mount[3];
  d6_mount(mount, &q_mount, t_mount);

  std::vector<Pose> pose_list;
  for (int i = 0; i <= 150; ++i) {
    pose_list.push_back(make_pose(static_cast<std::int64_t>(i) * 33'333'333LL, traj));
  }
  std::vector<std::vector<ProfilePoint>> revs;
  for (int rev = 0; rev < 40; ++rev) {
    revs.push_back(make_revolution(200'000'000LL + static_cast<std::int64_t>(rev) * 100'000'000LL,
                                   97, 2.2));
  }

  // Live: poses trickle in between profiles, and the profiles are pushed in
  // ragged chunks that tear across revolution boundaries.
  ExternalPoseSource live_poses;
  PageStore live_store;
  D6PushbroomAssembler live(&live_store);
  REQUIRE(live.set_mount_extrinsics(mount).ok());
  live.set_pose_source(&live_poses);
  std::size_t next_pose = 0;
  for (std::size_t r = 0; r < revs.size(); ++r) {
    const std::int64_t horizon = revs[r].back().t_mono_ns + 40'000'000LL;
    while (next_pose < pose_list.size() && pose_list[next_pose].t_mono_ns <= horizon) {
      REQUIRE(live_poses.push_pose(pose_list[next_pose++]).ok());
    }
    for (std::size_t off = 0; off < revs[r].size(); off += 13) {
      const std::size_t n = std::min<std::size_t>(13, revs[r].size() - off);
      REQUIRE(live.push_profile(Span<const ProfilePoint>(revs[r].data() + off, n)).ok());
    }
  }
  while (next_pose < pose_list.size()) {
    REQUIRE(live_poses.push_pose(pose_list[next_pose++]).ok());
  }
  REQUIRE(live.flush().ok());

  // Offline: every pose first, then every point in one go.
  ExternalPoseSource off_poses;
  for (const Pose& p : pose_list) REQUIRE(off_poses.push_pose(p).ok());
  PageStore off_store;
  PushbroomConfig off_cfg;
  off_cfg.drain_on_push = false;  // the offline job drains once at the end
  D6PushbroomAssembler off(&off_store, off_cfg);
  REQUIRE(off.set_mount_extrinsics(mount).ok());
  off.set_pose_source(&off_poses);
  for (const auto& rev : revs) {
    REQUIRE(off.push_profile(Span<const ProfilePoint>(rev.data(), rev.size())).ok());
  }
  REQUIRE(off.flush().ok());

  const auto a = read_all(live_store);
  const auto b = read_all(off_store);
  REQUIRE(a.size() == b.size());
  REQUIRE(a.size() == 40u * 97u);
  for (std::size_t i = 0; i < a.size(); ++i) {
    // Bit-for-bit, not approximately.
    CHECK(a[i].x == b[i].x);
    CHECK(a[i].y == b[i].y);
    CHECK(a[i].z == b[i].z);
    CHECK(a[i].a == b[i].a);
  }
}

TEST_CASE("pushbroom/rejects_a_non_rigid_extrinsic_and_bounds_the_pending_queue") {
  PageStore store;
  PushbroomConfig cfg;
  cfg.max_pending_points = 100;
  D6PushbroomAssembler a(&store, cfg);

  double column_major[16];  // what a JNI caller who forgot the convention sends
  const double R[9] = {0, 0, 1, 0, -1, 0, 1, 0, 0};
  const double t[3] = {0.015, 0.150, -0.030};
  se3::mat4_from_rt(R, t, column_major);
  column_major[12] = t[0];  // corrupt the bottom row the way a transpose would
  column_major[13] = t[1];
  CHECK(a.set_mount_extrinsics(column_major).error() == ScanError::kInvalidArgument);
  CHECK(a.set_mount_extrinsics(nullptr).error() == ScanError::kInvalidArgument);
  CHECK_FALSE(a.has_mount_extrinsics());

  // Out-of-window returns never enter the queue at all.
  ProfilePoint zero;
  zero.t_mono_ns = 1;
  zero.range_m = 0.0f;
  REQUIRE(a.push_point(zero).ok());
  ProfilePoint far_away = zero;
  far_away.range_m = 40.0f;
  REQUIRE(a.push_point(far_away).ok());
  CHECK(a.stats().dropped_range == 2);
  CHECK(a.pending() == 0);

  // With no pose source the queue fills and then sheds its oldest entries.
  const auto prof = make_revolution(1'000'000, 250, 2.0);
  REQUIRE(a.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
  CHECK(a.pending() == 100);
  CHECK(a.stats().dropped_overflow == 150);
}

// ===========================================================================
// ROUND 7 — the walking-gait wall test
//
// Every other case in this file uses a trajectory chosen so the interpolator
// is EXACT (constant velocity, constant yaw rate), which is right for proving
// the frame algebra and useless for the question the owner is actually asking:
// "when I walk through the room it is not a stable scan with straight walls."
//
// A person walking is not a constant-velocity rig. They sway laterally and bob
// vertically at about 2 Hz, and the phone yaws a degree or two with each step.
// The claim under test is therefore the field claim: **a flat wall, scanned
// while walking past it at 1 m/s with realistic gait, comes back flat** — and
// the falsifiable half is that it does NOT come back flat if all the returns
// of one revolution are paired with a single pose, which is exactly what the
// D6 path was doing before this round (see
// d6driver/round7_control_one_stamp_per_chunk_smears_the_whole_read).
//
// Nothing here is bespoke geometry: it drives the real ExternalPoseSource and
// the real D6PushbroomAssembler through their real entry points. Only the
// stimulus is synthetic.
// ===========================================================================
namespace {

// A walking operator, phone held in front, D6 clamped to its back.
struct GaitTrajectory {
  double speed = 1.0;          // m/s along world +x
  double step_hz = 2.0;        // both sway components, per the field brief
  double lateral_amp = 0.020;  // +/- 2 cm of side-to-side sway
  double vertical_amp = 0.030; // +/- 3 cm of bob
  // Trunk/hand yaw over a step is the biggest of the four for a scanner on a
  // 1-3 m lever arm: gait studies put trunk rotation at a few degrees per
  // step, and a hand-held phone adds to that rather than damping it. +/- 3 deg
  // is a walk, not a stumble; the "straight walls" claim has to survive it.
  double yaw_amp = 0.052;      // +/- 3.0 deg
  double roll_amp = 0.030;     // +/- 1.7 deg

  void position_at(double t, double out[3]) const {
    const double w = 2.0 * kPi * step_hz;
    out[0] = speed * t;
    out[1] = lateral_amp * std::sin(w * t);
    out[2] = 1.35 + vertical_amp * std::sin(w * t + 0.9);
  }
  Quat orientation_at(double t) const {
    const double w = 2.0 * kPi * step_hz;
    const Quat yaw = axis_angle(0, 0, 1, yaw_amp * std::sin(w * t + 0.4));
    const Quat roll = axis_angle(1, 0, 0, roll_amp * std::sin(w * t + 2.1));
    return qmul(yaw, roll);
  }
};

Pose gait_pose(std::int64_t t_ns, const GaitTrajectory& g) {
  const double t = static_cast<double>(t_ns) * 1e-9;
  Pose p;
  p.t_mono_ns = t_ns;
  g.position_at(t, p.position);
  const Quat q = g.orientation_at(t);
  p.orientation[0] = q.x;
  p.orientation[1] = q.y;
  p.orientation[2] = q.z;
  p.orientation[3] = q.w;
  p.source = StreamId::kPoseAr;
  p.quality = PoseQuality::kGood;
  p.tracking_lost = 0;
  return p;
}

// The owner's mount: D6 flat on the BACK of the phone with the scan fan
// VERTICAL and across the direction of travel. In lidar coordinates the fan is
// the xy plane (d6_fan.h), so the mount must send lidar +x -> world +y
// (across), lidar +y -> world +z (up), lidar +z -> world +x (along the walk).
//
// NOTE this fixture's world is +z-up and its "lidar +z -> forward" is a TEST
// convention, unrelated to the phone frame; it exists to make the planarity
// arithmetic readable. It is deliberately NOT a statement about which end of
// the unit +z comes out of — d6_fan.h owns that, and the chirality assertion
// lives in test_round9_chirality.cpp, which uses the real ARCore frame.
void pushbroom_mount(double m[16], Quat* q_out) {
  const double R[9] = {0, 0, 1,
                       1, 0, 0,
                       0, 1, 0};
  const double t[3] = {0.0, 0.0, 0.0};
  se3::mat4_from_rt(R, t, m);
  double q[4];
  se3::matrix_to_quat(R, q);
  *q_out = Quat{q[0], q[1], q[2], q[3]};
}

// Range to a wall at y = wall_y, for a fan return at `angle_deg` taken from
// the true pose at `t`. Returns < 0 when the ray misses (or hits behind, or
// out of the D6's range).
double range_to_wall(const GaitTrajectory& g, const Quat& q_mount, double wall_y, double t,
                     double angle_deg) {
  double dir_l[3];
  d6::fan_point(angle_deg, 1.0, dir_l);  // unit ray in the fan frame
  double dir_p[3];
  qrot(q_mount, dir_l, dir_p);
  double dir_w[3];
  qrot(g.orientation_at(t), dir_p, dir_w);
  double pos[3];
  g.position_at(t, pos);
  if (std::fabs(dir_w[1]) < 1e-6) return -1.0;
  const double d = (wall_y - pos[1]) / dir_w[1];
  if (d < 0.4 || d > 8.0) return -1.0;
  // Keep the wall a wall: floor at 0, ceiling at 2.9 m, and the returns that
  // would land outside that are the floor/ceiling of a real room, not the wall.
  const double z = pos[2] + d * dir_w[2];
  if (z < 0.05 || z > 2.9) return -1.0;
  return d;
}

// RMS distance of the points to their own best-fit plane y = a + b*x + c*z.
// A best-fit plane rather than the known wall so a constant bias (which is a
// time-offset symptom, not a bending one) does not masquerade as bending;
// tilt and bow both show up in the residual, which is what "straight walls"
// means to the eye.
double plane_fit_rms(const std::vector<PointVertex>& pts) {
  const std::size_t n = pts.size();
  double S[3][4] = {};
  for (const auto& p : pts) {
    const double b[3] = {1.0, static_cast<double>(p.x), static_cast<double>(p.z)};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) S[i][j] += b[i] * b[j];
      S[i][3] += b[i] * static_cast<double>(p.y);
    }
  }
  for (int c = 0; c < 3; ++c) {  // Gaussian elimination with partial pivoting
    int piv = c;
    for (int r = c + 1; r < 3; ++r) {
      if (std::fabs(S[r][c]) > std::fabs(S[piv][c])) piv = r;
    }
    for (int j = 0; j < 4; ++j) std::swap(S[c][j], S[piv][j]);
    const double d = S[c][c];
    if (std::fabs(d) < 1e-12) return 1e9;
    for (int j = c; j < 4; ++j) S[c][j] /= d;
    for (int r = 0; r < 3; ++r) {
      if (r == c) continue;
      const double f = S[r][c];
      for (int j = c; j < 4; ++j) S[r][j] -= f * S[c][j];
    }
  }
  const double a = S[0][3], b = S[1][3], c = S[2][3];
  double sum = 0.0;
  for (const auto& p : pts) {
    const double resid = static_cast<double>(p.y) - (a + b * static_cast<double>(p.x) +
                                                     c * static_cast<double>(p.z));
    sum += resid * resid;
  }
  // y = a + b x + c z  =>  the plane normal is (-b, 1, -c) / |.|, so a vertical
  // residual is a perpendicular distance once divided by that norm.
  const double norm = std::sqrt(1.0 + b * b + c * c);
  return std::sqrt(sum / static_cast<double>(n)) / norm;
}

// How the returns are timestamped on their way into the assembler.
//   kPerPoint   — the truth, and what ROUND 7's driver change now produces.
//   kPerRev     — one stamp per 100 ms revolution (the brief's control).
//   kPerChunk   — one stamp per 178 ms USB read, which is what the phone
//                 actually did: 4096 bytes at 230400 8N1, spanning 1.8
//                 revolutions, every return in it claiming the same instant.
enum class Pairing { kPerPoint, kPerRev, kPerChunk };

// Walks 4 s past a wall and returns the resolved cloud.
std::vector<PointVertex> walk_past_a_wall(Pairing pairing, std::size_t* out_kept) {
  constexpr std::int64_t kChunkNs = 177'778'000LL;  // 4096 bytes at 230400 8N1
  constexpr double kWallY = 1.5;
  constexpr int kRevolutions = 40;                  // 4 s at 10 Hz
  constexpr int kReturnsPerRev = 360;               // the D6's ~1 deg resolution
  constexpr std::int64_t kRevNs = 100'000'000LL;    // 10 Hz
  constexpr std::int64_t kPoseNs = 33'333'333LL;    // ARCore at ~30 Hz
  constexpr std::int64_t kT0 = 1'000'000'000LL;

  const GaitTrajectory g;
  double mount[16];
  Quat q_mount;
  pushbroom_mount(mount, &q_mount);

  ExternalPoseSource poses;
  // Poses cover the whole walk with a margin either side, so nothing is
  // dropped for want of a bracket. The trailing margin is a full USB chunk
  // wide because the kPerChunk arm dates a return at the END of the 177.8 ms
  // read holding it, which can land past the last return's own time.
  for (std::int64_t t = kT0 - 2 * kPoseNs;
       t <= kT0 + kRevolutions * kRevNs + kChunkNs + 2 * kPoseNs; t += kPoseNs) {
    REQUIRE(poses.push_pose(gait_pose(t, g)).ok());
  }

  PageStore store;
  D6PushbroomAssembler a(&store);
  REQUIRE(a.set_mount_extrinsics(mount).ok());
  a.set_pose_source(&poses);

  std::size_t kept = 0;
  for (int rev = 0; rev < kRevolutions; ++rev) {
    const std::int64_t t_rev = kT0 + static_cast<std::int64_t>(rev) * kRevNs;
    std::vector<ProfilePoint> prof;
    for (int i = 0; i < kReturnsPerRev; ++i) {
      const std::int64_t t_pt = t_rev + kRevNs * i / kReturnsPerRev;
      const double angle = 360.0 * i / kReturnsPerRev;
      // The RANGE is always measured at the point's true time — that is
      // physics, and it is the same in both arms. Only the timestamp handed to
      // the assembler differs.
      const double d = range_to_wall(g, q_mount, kWallY, static_cast<double>(t_pt) * 1e-9, angle);
      if (d < 0) continue;
      ProfilePoint p;
      switch (pairing) {
        case Pairing::kPerPoint: p.t_mono_ns = t_pt; break;
        case Pairing::kPerRev:   p.t_mono_ns = t_rev; break;
        // Stamp-on-arrival semantics: the whole read is dated when its LAST
        // byte landed, so a point is dated at the END of the chunk holding it.
        case Pairing::kPerChunk:
          p.t_mono_ns = ((t_pt / kChunkNs) + 1) * kChunkNs;
          break;
      }
      p.angle_deg = static_cast<float>(angle);
      p.range_m = static_cast<float>(d);
      p.intensity = 120;
      p.high_reflectivity = 0;
      prof.push_back(p);
      ++kept;
    }
    if (!prof.empty()) {
      REQUIRE(a.push_profile(Span<const ProfilePoint>(prof.data(), prof.size())).ok());
    }
  }
  REQUIRE(a.flush().ok());
  CHECK(a.stats().dropped_no_pose == 0);
  if (out_kept != nullptr) *out_kept = kept;
  return read_all(store);
}

}  // namespace

TEST_CASE("pushbroom/round7_a_walked_wall_is_planar_with_per_point_pose_pairing") {
  std::size_t kept_pt = 0, kept_rev = 0, kept_chunk = 0;
  const auto pts_point = walk_past_a_wall(Pairing::kPerPoint, &kept_pt);
  const auto pts_rev = walk_past_a_wall(Pairing::kPerRev, &kept_rev);
  const auto pts_chunk = walk_past_a_wall(Pairing::kPerChunk, &kept_chunk);

  // Same stimulus in all three arms — only the timestamps differ.
  REQUIRE(kept_pt > 2000);
  REQUIRE(kept_rev == kept_pt);
  REQUIRE(kept_chunk == kept_pt);
  REQUIRE(pts_point.size() == kept_pt);

  const double rms_point = plane_fit_rms(pts_point);
  const double rms_rev = plane_fit_rms(pts_rev);
  const double rms_chunk = plane_fit_rms(pts_chunk);
  MESSAGE("plane-fit RMS -- per-point " << rms_point * 100.0 << " cm, per-revolution "
                                        << rms_rev * 100.0 << " cm, per-178ms-chunk "
                                        << rms_chunk * 100.0 << " cm");

  // 1. The field bar: a wall you would call straight. What is left here is the
  //    interpolator's own modelling error against a 2 Hz sinusoid sampled at
  //    30 Hz — sub-millimetre. The 2 cm is the owner's tolerance, not the
  //    algorithm's, and pinning the tighter number is what catches a
  //    regression before a field session does.
  CHECK(rms_point < 0.02);
  CHECK(rms_point < 0.004);

  // 2. THE FALSIFIABLE CONTROL. Same wall, same ranges, same assembler, same
  //    poses — timestamps collapsed to one per 178 ms USB read, which is
  //    literally what the phone was doing before this round — and the wall
  //    stops being a wall. If this ever drops under the bar, the pairing fix
  //    has become untestable and this file is lying about proving anything.
  CHECK(rms_chunk > 0.02);

  // 3. The brief's own control, one pose per 10 Hz revolution: milder than the
  //    chunk case (a revolution is 100 ms, not 178) but unambiguously worse
  //    than per-point pairing. Asserted as a ratio rather than an absolute so
  //    it stays meaningful if the gait model is ever made more or less
  //    aggressive.
  CHECK(rms_rev > 4.0 * rms_point);
  CHECK(rms_chunk > rms_rev);
}
