// ROUND 18 items 68 + 69 — the snap gate, and the bridge's edge slack, on the
// owner's real bytes.
//
// Two failures from the 0.9.2 field session (2026-08-19, 03:11-03:25):
//
//  * item 69: scan-047 break #3 — a ONE-FRAME jump of 0.371 m / 56.85 deg
//    (implied 1,720 deg/s) was healed live through the snap path, which
//    applied T wholesale with no check at all. The capture's own 399 Hz gyro
//    says the phone turned 0.68 deg in that frame. Round 13 measured real
//    ARCore re-anchors at 8-13.5 deg; 56.85 deg in 33 ms is a relocalisation
//    or a frame restart, and taking it on faith gave scan-047 the session's
//    worst self-check (6.92 cm). The snap fast-path now applies only at or
//    under `max_residual_rotation_deg`; past it the pair takes the
//    gyro-checked route.
//
//  * item 68: scan-053 break #1 — "refused: no continuous gyro across the
//    gap" for a 0.010 m / 0.28 deg jump, while the same capture's IMU stream
//    ran at 399.1 Hz throughout. The first gyro sample lands 46 ms after the
//    first ARCore pose (SensorManager start-up latency; ARCore was already
//    running), the 3 AM captures lose tracking AT START so the bracket's t0
//    IS the first pose, and the old straddle rule (within 25 ms of both ends)
//    refused a 1554 ms window it covered 97 % of. `bridge_edge_slack_ns`
//    (100 ms, the snap-gap span, same physical claim) fixes it; the numbers
//    below are scan-047 break #1's shape, which failed the same way at 68 ms.
//
// The scan-047 constants are lifted verbatim from captures/scan-047.lscan:
// pose index 483 -> 484 (the two sides of the 33 ms IMPOSSIBLE_STEP at
// t = 139614887760350), and the recorded gyro integrated across that frame
// with the container's own cameraFromImu (Rz(+90)).

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

// --- captures/scan-047.lscan, break #3 (IMPOSSIBLE_STEP, one frame) ---------
constexpr std::int64_t kB3BeforeNs = 139'614'854'474'611LL;
constexpr std::int64_t kB3AfterNs = 139'614'887'760'350LL;
const double kB3BeforeP[3] = {0.2965373694896698, 0.09016839414834976, -0.11569803208112717};
const double kB3BeforeQ[4] = {-0.1394781470298767, 0.29288923740386963, 0.6857072114944458,
                              -0.6515883803367615};
const double kB3AfterP[3] = {0.0009638499468564987, 0.15525946021080017, -0.3299499750137329};
const double kB3AfterQ[4] = {0.203603595495224, -0.05252338573336601, 0.6683783531188965,
                             -0.7134823799133301};
// The recorded gyro across the same 33 ms, in the pose's frame: 0.68 deg.
const double kB3Gyro[4] = {-0.005294311627869838, -0.00011471886339387873,
                           0.0026774104253504127, 0.9999823941337087};

void quat_from_axis_deg(int axis, double deg, double q[4]) {
  double rv[3] = {0, 0, 0};
  rv[axis] = deg * 3.14159265358979323846 / 180.0;
  se3::quat_from_rotvec(rv, q);
}

}  // namespace

TEST_CASE("round18: scan-047's 56.85 deg one-frame jump is REFUSED, not snapped") {
  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(kB3BeforeQ, kB3BeforeP, kB3BeforeNs, kB3AfterQ, kB3AfterP,
                                 kB3AfterNs, kB3Gyro, false, policy);
  // The tracker's claim, as the seal log printed it.
  CHECK(g.reported_rotation_deg == doctest::Approx(56.85).epsilon(0.01));
  CHECK(g.reported_translation_m == doctest::Approx(0.371).epsilon(0.01));
  // The witness: the phone turned two thirds of a degree in that frame.
  CHECK(g.gyro_rotation_deg == doctest::Approx(0.68).epsilon(0.05));
  // So the residual IS the jump, it is past anything a re-anchor can be, and
  // the frame is left alone — where 0.9.2 applied all 56.85 deg live.
  CHECK(g.verdict == GapVerdict::kRefusedDisagree);
  CHECK_FALSE(reanchor::applies(g.verdict));
  CHECK(reanchor::is_refusal(g.verdict));
  for (int i = 0; i < 16; ++i) {
    const double eye = (i % 5 == 0) ? 1.0 : 0.0;
    CHECK(g.correction[i] == doctest::Approx(eye));
  }
}

TEST_CASE("round18: a snap at the re-anchor sizes round 13 measured is bit-identical") {
  // 13.5 deg — the largest real re-anchor round 13 measured — must still take
  // the snap path with the exact analytic transform, no gyro consulted.
  const double qb[4] = {0, 0, 0, 1};
  const double pb[3] = {1.0, 0.5, -2.0};
  double qj[4];
  quat_from_axis_deg(1, 13.5, qj);
  const double pa[3] = {1.3, 0.5, -2.1};

  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(qb, pb, 0, qj, pa, 33'000'000LL, nullptr, false, policy);
  CHECK(g.verdict == GapVerdict::kSnap);
  double ma[16], mb[16], mbi[16], expect[16];
  se3::mat4_from_quat_pos(qj, pa, ma);
  se3::mat4_from_quat_pos(qb, pb, mb);
  se3::mat4_inverse_rigid(mb, mbi);
  se3::mat4_mul(ma, mbi, expect);
  for (int i = 0; i < 16; ++i) CHECK(g.correction[i] == doctest::Approx(expect[i]));
}

TEST_CASE("round18: an over-bounds snap with no gyro is refused, in plain words") {
  const double qb[4] = {0, 0, 0, 1};
  const double pb[3] = {0, 0, 0};
  double qj[4];
  quat_from_axis_deg(1, 56.85, qj);
  const double pa[3] = {0.3, 0.05, -0.2};

  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(qb, pb, 0, qj, pa, 33'000'000LL, nullptr, false, policy);
  CHECK(g.verdict == GapVerdict::kRefusedNoGyro);
  CHECK(std::string(g.reason).find("one-frame jump") != std::string::npos);
}

TEST_CASE("round18: an over-bounds snap the gyro can EXPLAIN is the operator, not the frame") {
  // A violent wrist flick: the tracker reports 30 deg in one frame and the
  // gyro read the same 30 deg. Sub-degree residual — nothing to heal, and no
  // cue. (Physically extreme, ~900 deg/s, but inside a consumer gyro's range;
  // the point is the DECISION, which must follow the witness.)
  const double qb[4] = {0, 0, 0, 1};
  const double pb[3] = {0, 0, 0};
  double qj[4];
  quat_from_axis_deg(1, 30.0, qj);
  const double pa[3] = {0.02, 0.0, 0.0};

  GapPolicy policy;
  const GapResult g =
      reanchor::resolve_reanchor(qb, pb, 0, qj, pa, 33'000'000LL, qj, false, policy);
  CHECK(g.verdict == GapVerdict::kNegligible);
  CHECK_FALSE(reanchor::is_refusal(g.verdict));
}

TEST_CASE("round18: the bridge tolerates the sensor's start-up latency at the interval's edge") {
  // scan-053/047 break #1's shape: the gyro stream begins 68 ms AFTER t0
  // (SensorManager start-up latency) and the bracket's t0 is the first pose.
  // 400 Hz of a stationary phone across a 1.6 s gap.
  ImuDensifyConfig cfg;
  cfg.capacity = 3200;
  cfg.estimate_bias = false;
  ImuDensifiedPoseSource src(nullptr, cfg);
  constexpr std::int64_t kT0 = 1'000'000'000LL;          // the "first pose"
  constexpr std::int64_t kImuStart = kT0 + 68'000'000LL;  // first gyro sample
  for (int k = 0; k <= 800; ++k) {
    PhoneImuSample s;
    s.t_mono_ns = kImuStart + static_cast<std::int64_t>(k) * 2'500'000LL;
    s.accel_m_s2[1] = 9.81f;
    CHECK(src.push_imu(s));
  }
  double q[4];
  double peak = 0.0;
  bool hole = true;
  // 0.9.2 refused this ("no continuous gyro across the gap"); the 68 ms edge
  // is inside bridge_edge_slack_ns and contributes zero rotation.
  CHECK(src.relative_rotation(kT0, kT0 + 1'600'000'000LL, q, &peak, &hole));
  CHECK_FALSE(hole);
  double R[9];
  se3::quat_to_matrix(q, R);
  const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  CHECK(se3::rot_angle_deg(eye, R) == doctest::Approx(0.0).epsilon(0.01));

  // Past the slack is still a refusal: a stream that starts 150 ms late is
  // not covering the interval, it is missing a step's worth of it.
  CHECK_FALSE(src.relative_rotation(kImuStart - 150'000'000LL, kT0 + 1'600'000'000LL, q, &peak,
                                    &hole));

  // And an INTERIOR hole is still fatal — the slack is an edge rule only.
  ImuDensifiedPoseSource holed(nullptr, cfg);
  for (int k = 0; k <= 800; ++k) {
    PhoneImuSample s;
    s.t_mono_ns = kT0 + static_cast<std::int64_t>(k) * 2'500'000LL;
    if (s.t_mono_ns > kT0 + 700'000'000LL && s.t_mono_ns < kT0 + 780'000'000LL) continue;
    s.accel_m_s2[1] = 9.81f;
    CHECK(holed.push_imu(s));
  }
  CHECK(holed.relative_rotation(kT0, kT0 + 2'000'000'000LL, q, &peak, &hole));
  CHECK(hole);  // reanchor treats this as no gyro, which is the point
}
