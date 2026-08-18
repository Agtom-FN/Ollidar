// ROUND 17 item 63 — the long-gap re-anchor policy, on the owner's real bytes.
//
// The numbers in `owner_scan040_*` below are lifted verbatim from
// captures/scan-040.lscan (2026-08-19): pose index 1476 is the last one ARCore
// owned before it went blind, index 1658 the first one after, and the gyro
// quaternion is the recorded 399.2 Hz stream integrated across the 6.065 s
// between them with the container's own `cameraFromImu`. Nothing here is
// synthetic except the small analytic cases, which exist to pin the arithmetic.
//
// The one that matters: ROUND 15 applied a 66.21 deg world rotation to that
// capture's live map, and this file is the assertion that it never will again.

#include "microtest_shim.h"

#include <cmath>

#include "scanengine/poses/imu_densified_pose.h"
#include "scanengine/poses/reanchor.h"
#include "scanengine/poses/se3.h"

using namespace scanengine;
using reanchor::GapPolicy;
using reanchor::GapResult;
using reanchor::GapVerdict;

namespace {

// --- captures/scan-040.lscan, the break at t = 55.32 s ----------------------
constexpr std::int64_t kBeforeNs = 129'920'466'280'772LL;
constexpr std::int64_t kAfterNs = 129'926'531'555'690LL;
const double kBeforeP[3] = {1.7716330289840698, 0.0038356948643922806, -3.138058662414551};
const double kBeforeQ[4] = {0.3709307610988617, -0.5624434947967529, -0.6052027940750122,
                            0.4240250587463379};
const double kAfterP[3] = {1.3250091075897217, -0.015514245256781578, -2.6277854442596436};
const double kAfterQ[4] = {0.6399774551391602, -0.714933454990387, -0.26890334486961365,
                           0.08360450714826584};
// The recorded gyro, integrated across the same span, in the pose's frame.
const double kGyro[4] = {0.9022524975346735, 0.007271607477780717, -0.3084752412060563,
                         0.301215172227427};

void quat_from_axis_deg(int axis, double deg, double q[4]) {
  double rv[3] = {0, 0, 0};
  rv[axis] = deg * 3.14159265358979323846 / 180.0;
  se3::quat_from_rotvec(rv, q);
}

double correction_rotation_deg(const GapResult& g) {
  double R[9], t[3];
  se3::mat4_get_rt(g.correction, R, t);
  const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  return se3::rot_angle_deg(eye, R);
}

double correction_translation_m(const GapResult& g) {
  return std::sqrt(g.correction[3] * g.correction[3] + g.correction[7] * g.correction[7] +
                   g.correction[11] * g.correction[11]);
}

}  // namespace

TEST_CASE("round17: scan-040's six blind seconds are REFUSED, not applied") {
  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(kBeforeQ, kBeforeP, kBeforeNs, kAfterQ, kAfterP, kAfterNs, kGyro,
                                 false, policy);

  // What the tracker claimed, to the numbers in the owner's seal log.
  CHECK(g.gap_s == doctest::Approx(6.065).epsilon(0.001));
  CHECK(g.reported_translation_m == doctest::Approx(0.678).epsilon(0.01));
  CHECK(g.reported_rotation_deg == doctest::Approx(66.21).epsilon(0.001));

  // What the phone's own gyro says the operator did in the same six seconds.
  // 145 degrees. The tracker saw 66 of them.
  CHECK(g.gyro_used);
  CHECK(g.gyro_rotation_deg == doctest::Approx(144.94).epsilon(0.001));
  CHECK(g.residual_rotation_deg == doctest::Approx(78.91).epsilon(0.001));

  // A person could have walked 11.2 m in that time; the tracker claims 0.68.
  // Nothing about the displacement is impossible, so none of it is the frame.
  CHECK(g.walk_bound_m > 11.0);
  CHECK(g.residual_translation_m == doctest::Approx(0.0));

  // THE ASSERTION. ROUND 15 applied a 66.21 deg rotation here.
  CHECK(g.verdict == GapVerdict::kRefusedDisagree);
  CHECK_FALSE(reanchor::applies(g.verdict));
  CHECK(reanchor::is_refusal(g.verdict));
  CHECK(correction_rotation_deg(g) == doctest::Approx(0.0));
  CHECK(correction_translation_m(g) == doctest::Approx(0.0));
}

TEST_CASE("round17: without the gyro the same gap is refused, for a different reason") {
  // A container with no phone IMU cannot know what the operator did, and the
  // honest answer to "what should I apply?" is nothing at all. It must never
  // fall back to the ROUND-13 transform, which is exactly the bug.
  GapPolicy policy;
  const GapResult g = reanchor::resolve_reanchor(kBeforeQ, kBeforeP, kBeforeNs, kAfterQ, kAfterP,
                                                 kAfterNs, nullptr, false, policy);
  CHECK(g.verdict == GapVerdict::kRefusedNoGyro);
  CHECK_FALSE(g.gyro_used);
  CHECK(correction_rotation_deg(g) == doctest::Approx(0.0));

  // A stuttering gyro is no gyro. Same verdict, same refusal.
  const GapResult holed = reanchor::resolve_reanchor(kBeforeQ, kBeforeP, kBeforeNs, kAfterQ,
                                                     kAfterP, kAfterNs, kGyro, true, policy);
  CHECK(holed.verdict == GapVerdict::kRefusedNoGyro);
}

TEST_CASE("round17: a 33 ms snap is still ROUND 13's transform, unchanged") {
  // The case ROUND 13/15 were written for must be untouched: over one ARCore
  // frame nobody moves, so the jump IS the frame and the whole of it applies.
  const double qb[4] = {0, 0, 0, 1};
  const double pb[3] = {1.0, 0.5, -2.0};
  double qj[4];
  quat_from_axis_deg(1, 11.0, qj);  // an 11 deg yaw, ROUND 13's measured size
  const double pa[3] = {1.62, 0.55, -2.63};

  GapPolicy policy;
  const GapResult g = reanchor::resolve_reanchor(qb, pb, 0, qj, pa, 33'000'000LL, nullptr, false,
                                                 policy);
  CHECK(g.verdict == GapVerdict::kSnap);
  CHECK(reanchor::applies(g.verdict));
  CHECK(correction_rotation_deg(g) == doctest::Approx(11.0).epsilon(0.001));

  // And it is literally pose_after * pose_before^-1.
  double ma[16], mb[16], mbi[16], expect[16];
  se3::mat4_from_quat_pos(qj, pa, ma);
  se3::mat4_from_quat_pos(qb, pb, mb);
  se3::mat4_inverse_rigid(mb, mbi);
  se3::mat4_mul(ma, mbi, expect);
  for (int i = 0; i < 16; ++i) CHECK(g.correction[i] == doctest::Approx(expect[i]));
}

TEST_CASE("round17: when the gyro agrees, the jump is the operator and nothing is applied") {
  // Two seconds blind. The operator turned 40 degrees and walked 1.5 m, and
  // the tracker reports exactly that when it comes back. There is no frame
  // change here at all, and ROUND 15 would have applied a 40 deg rotation.
  const double qb[4] = {0, 0, 0, 1};
  const double pb[3] = {0, 0, 0};
  double qg[4];
  quat_from_axis_deg(1, 40.0, qg);
  const double pa[3] = {1.5, 0.0, 0.0};

  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(qb, pb, 0, qg, pa, 2'000'000'000LL, qg, false, policy);
  CHECK(g.verdict == GapVerdict::kNegligible);
  CHECK_FALSE(reanchor::applies(g.verdict));
  // NOT a refusal: nothing went wrong, so the operator must not be cued.
  CHECK_FALSE(reanchor::is_refusal(g.verdict));
  CHECK(g.residual_rotation_deg == doctest::Approx(0.0).epsilon(0.01));
  CHECK(g.gyro_rotation_deg == doctest::Approx(40.0).epsilon(0.001));
}

TEST_CASE("round17: a real re-anchor inside a bridged gap is healed, at its residual size") {
  // Same two seconds, but this time the tracker comes back 12 degrees off what
  // the gyro says the operator did. THAT is a re-anchor, it is the size ROUND
  // 13 measured, and it is what gets applied — 12 degrees, not 52.
  const double qb[4] = {0, 0, 0, 1};
  const double pb[3] = {0, 0, 0};
  double qg[4], q_extra[4], qa[4];
  quat_from_axis_deg(1, 40.0, qg);
  quat_from_axis_deg(1, 12.0, q_extra);
  se3::quat_mul(q_extra, qg, qa);  // world-frame: the frame rotated by 12 deg
  se3::quat_normalize(qa);
  const double pa[3] = {1.5, 0.0, 0.0};

  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(qb, pb, 0, qa, pa, 2'000'000'000LL, qg, false, policy);
  CHECK(g.verdict == GapVerdict::kBridged);
  CHECK(reanchor::applies(g.verdict));
  CHECK(g.reported_rotation_deg == doctest::Approx(52.0).epsilon(0.001));
  CHECK(g.residual_rotation_deg == doctest::Approx(12.0).epsilon(0.001));
  CHECK(correction_rotation_deg(g) == doctest::Approx(12.0).epsilon(0.001));
}

TEST_CASE("round17: only impossible displacement is charged to the frame") {
  // Half a second blind, and the tracker comes back 6 m away. A person walks
  // 1.8 m/s, so at most 1.2 m of that could be them; the other 4.8 m is the
  // world moving. The bound is what gets subtracted, not the whole jump.
  const double qb[4] = {0, 0, 0, 1};
  const double pb[3] = {0, 0, 0};
  const double pa[3] = {6.0, 0.0, 0.0};
  const double qg[4] = {0, 0, 0, 1};  // the phone did not turn

  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(qb, pb, 0, qb, pa, 500'000'000LL, qg, false, policy);
  const double bound = policy.walk_speed_mps * 0.5 + policy.walk_bound_slack_m;  // 1.20 m
  CHECK(g.walk_bound_m == doctest::Approx(bound));
  CHECK(g.residual_translation_m == doctest::Approx(6.0 - bound));
  // Past max_residual_translation_m, so this one is refused rather than healed
  // — 4.8 m is not a re-anchor either.
  CHECK(g.verdict == GapVerdict::kRefusedDisagree);
}

TEST_CASE("round17: a gap longer than the gyro is trusted for is refused by duration alone") {
  const double q[4] = {0, 0, 0, 1};
  const double p[3] = {0, 0, 0};
  const double p2[3] = {1.0, 0, 0};
  GapPolicy policy;
  const GapResult g = reanchor::resolve_reanchor(q, p, 0, q, p2, 20'000'000'000LL, q, false,
                                                 policy);
  CHECK(g.verdict == GapVerdict::kRefusedTooLong);
  CHECK(correction_translation_m(g) == doctest::Approx(0.0));
}

TEST_CASE("round17: the densifier answers an arbitrary span, not just a bracket") {
  // relative_rotation() must NOT inherit sample_at()'s 200 ms bracket ceiling:
  // bridging a gap is the case where the span is long on purpose. 4 s of a
  // constant 10 deg/s about the sensor's Z, at 400 Hz.
  ImuDensifyConfig cfg;
  cfg.capacity = 2000;
  cfg.estimate_bias = false;
  ImuDensifiedPoseSource src(nullptr, cfg);
  const double rate = 10.0 * 3.14159265358979323846 / 180.0;
  // Stamps start at 1 s, not 0: push_imu() rejects a non-positive stamp, which
  // is the right rule (a zero CLOCK_BOOTTIME is a bug, not a sample).
  constexpr std::int64_t kT0 = 1'000'000'000LL;
  for (int k = 0; k <= 1600; ++k) {
    PhoneImuSample s;
    s.t_mono_ns = kT0 + static_cast<std::int64_t>(k) * 2'500'000LL;  // 400 Hz
    s.gyro_rad_s[2] = static_cast<float>(rate);
    s.accel_m_s2[1] = 9.81f;
    CHECK(src.push_imu(s));
  }
  double q[4];
  double peak = 0.0;
  bool hole = true;
  CHECK(src.relative_rotation(kT0, kT0 + 4'000'000'000LL, q, &peak, &hole));
  CHECK_FALSE(hole);
  double R[9];
  se3::quat_to_matrix(q, R);
  const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  CHECK(se3::rot_angle_deg(eye, R) == doctest::Approx(40.0).epsilon(0.001));

  // A span the ring does not straddle is refused rather than extrapolated.
  CHECK_FALSE(src.relative_rotation(kT0, kT0 + 10'000'000'000LL, q, &peak, &hole));
}
