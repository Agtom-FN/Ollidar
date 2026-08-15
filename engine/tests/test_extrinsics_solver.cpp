// test_extrinsics_solver.cpp — A11's `ExtrinsicsSolver`, implemented over A8's
// `MountCalibrationSolver` (A8 §6, A11 §8.3, docs/INT34-wiring.md §9 item 1).
//
// The observations here are SYNTHETIC DETECTIONS, not synthetic images: the
// wrapper's contract starts where B7's detector ends, at a plane `(n, d)` in
// the camera frame plus the lidar returns that landed on the board. So the
// fixture generates exactly that pair from a known mount, and the claim under
// test is that the wrapper recovers the mount — i.e. that it hands A8 the
// right thing in the right frame. A sign error or a transposed rotation
// anywhere in the adapter shows up as a solve that does not converge on truth,
// which is the only test of a coordinate convention worth writing.
//
// Included first and alone, so this file is also the self-containment check
// for the new header.
#include "scanengine/color/extrinsics_solver.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/poses/se3.h"

using namespace scanengine;
using scanengine::color::BoardDetection;
using scanengine::color::ExtrinsicsSolverConfig;
using scanengine::color::MountExtrinsicsSolver;

namespace {

constexpr double kDeg = se3::kPi / 180.0;

// Deterministic across the five CI legs; <random>'s distributions are not.
class Rng {
 public:
  explicit Rng(std::uint64_t s) : s_(s ? s : 0x9E3779B97F4A7C15ULL) {}
  double u01() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return static_cast<double>(s_ >> 11) * (1.0 / 9007199254740992.0);
  }
  double normal(double sigma) {
    const double a = std::max(u01(), 1e-300), b = u01();
    return sigma * std::sqrt(-2.0 * std::log(a)) * std::cos(2.0 * se3::kPi * b);
  }

 private:
  std::uint64_t s_;
};

void euler_zyx(double rx_deg, double ry_deg, double rz_deg, double R[9]) {
  const double a = rx_deg * kDeg, b = ry_deg * kDeg, c = rz_deg * kDeg;
  const double ca = std::cos(a), sa = std::sin(a);
  const double cb = std::cos(b), sb = std::sin(b);
  const double cc = std::cos(c), sc = std::sin(c);
  R[0] = cc * cb;
  R[1] = cc * sb * sa - sc * ca;
  R[2] = cc * sb * ca + sc * sa;
  R[3] = sc * cb;
  R[4] = sc * sb * sa + cc * ca;
  R[5] = sc * sb * ca - cc * sa;
  R[6] = -sb;
  R[7] = cb * sa;
  R[8] = cb * ca;
}

void make_mat4(const double R[9], const double t[3], double m[16]) {
  m[0] = R[0]; m[1] = R[1]; m[2] = R[2];  m[3] = t[0];
  m[4] = R[3]; m[5] = R[4]; m[6] = R[5];  m[7] = t[1];
  m[8] = R[6]; m[9] = R[7]; m[10] = R[8]; m[11] = t[2];
  m[12] = 0;   m[13] = 0;   m[14] = 0;    m[15] = 1;
}

// The truth mount: a bracket that holds the lidar 6 cm below and 3 cm behind
// the camera, rotated a few degrees off the camera's axes.
void truth_mount(double m[16]) {
  double R[9];
  euler_zyx(3.0, -2.0, 1.5, R);
  const double t[3] = {0.03, -0.06, 0.02};
  make_mat4(R, t, m);
}

// The bracket's CAD nominal: S6's own starting distance from truth.
void cad_nominal(const double truth[16], double cad[16]) {
  double R[9];
  euler_zyx(-2.5, 1.5, -2.0, R);
  const double t[3] = {0.0, 0.0, 0.0};
  double perturb[16];
  make_mat4(R, t, perturb);
  se3::mat4_mul(perturb, truth, cad);
  cad[3] += 0.018;
  cad[7] -= 0.012;
  cad[11] += 0.015;
}

const std::uint32_t kW = 4032, kH = 3024;
const float kFx = 2912.f;

Keyframe make_keyframe(std::int64_t t_ns, int i) {
  Keyframe kf;
  kf.t_mono_ns = t_ns;
  kf.image_path = "streams/frames/kf_" + std::to_string(i) + ".jpg";
  kf.intrinsics.fx = kFx;
  kf.intrinsics.fy = kFx;
  kf.intrinsics.cx = static_cast<float>(kW) * 0.5f;
  kf.intrinsics.cy = static_cast<float>(kH) * 0.5f;
  kf.intrinsics.width = kW;
  kf.intrinsics.height = kH;
  return kf;
}

// One wizard pose. The camera measures the board as a plane; the lidar sees a
// patch of returns ON that plane, in ITS OWN frame — which is where the mount
// enters, and the only place it does.
struct WizardPose {
  BoardDetection det;
  std::vector<PointVertex> points;
};

WizardPose make_pose(const double camera_from_lidar[16], int i, int n, double sigma_m, Rng* rng) {
  WizardPose out;
  (void)n;

  // A spread of board orientations and distances — Zhang-Pless needs the
  // CONFIGURATIONS to differ, not just the poses, and what a plane-only solve
  // sees of a configuration is exactly its NORMAL. So the normals are spread
  // isotropically about the optical axis with a low-discrepancy additive
  // recurrence (the R2 sequence), which is the same property B7's PosePlan is
  // built for: any PREFIX is well spread, so "stop at 5 / 8 / 12" are all
  // well-conditioned choices.
  const auto frac = [](double x) { return x - std::floor(x); };
  const double theta = 32.0 * kDeg * frac(0.5 + static_cast<double>(i) * 0.7548776662);
  const double phi = 2.0 * se3::kPi * frac(static_cast<double>(i) * 0.5698402909);
  const double dist = 1.2 + 0.6 * frac(static_cast<double>(i) * 0.6180339887);

  // The board faces the camera: its normal is within `theta` of the optical
  // axis, pointing away from the camera (+z), so d > 0.
  double n_c[3] = {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi),
                   std::cos(theta)};
  se3::normalize3(n_c);
  const double d_c = dist;

  out.det.t_engine_ns = 0;  // filled by the caller
  out.det.normal[0] = n_c[0];
  out.det.normal[1] = n_c[1];
  out.det.normal[2] = n_c[2];
  out.det.d = d_c;
  out.det.sigma_m = sigma_m;
  out.det.corners = 48;

  // The SAME plane in the lidar frame. X_c = R X_l + t, so
  //   n_c . (R X_l + t) = d_c  =>  (R^T n_c) . X_l = d_c - n_c . t
  double R[9], t[3];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) R[r * 3 + c] = camera_from_lidar[r * 4 + c];
    t[r] = camera_from_lidar[r * 4 + 3];
  }
  double n_l[3] = {R[0] * n_c[0] + R[3] * n_c[1] + R[6] * n_c[2],
                   R[1] * n_c[0] + R[4] * n_c[1] + R[7] * n_c[2],
                   R[2] * n_c[0] + R[5] * n_c[1] + R[8] * n_c[2]};
  const double d_l = d_c - (n_c[0] * t[0] + n_c[1] * t[1] + n_c[2] * t[2]);

  // Two tangents, and a grid of returns on the board (an A1 board is
  // 0.80 x 0.60 m; the lidar sees a patch of it).
  double u[3];
  const double axis[3] = {0.0, 0.0, 1.0};
  const double alt[3] = {1.0, 0.0, 0.0};
  se3::cross3(n_l, std::fabs(n_l[2]) > 0.9 ? alt : axis, u);
  se3::normalize3(u);
  double v[3];
  se3::cross3(n_l, u, v);
  se3::normalize3(v);

  // An A1 board is 0.80 x 0.60 m; a Mid-360 at ~1.5 m puts a few hundred
  // returns on it, and the plane fit per pose is what the mount solve is
  // built on — so the count matters as much as the spread.
  for (int a = -7; a <= 7; ++a) {
    for (int b = -5; b <= 5; ++b) {
      const double sa = 0.05 * a, sb = 0.05 * b;
      const double noise = rng->normal(sigma_m);
      PointVertex p{};
      p.x = static_cast<float>((d_l + noise) * n_l[0] + sa * u[0] + sb * v[0]);
      p.y = static_cast<float>((d_l + noise) * n_l[1] + sa * u[1] + sb * v[1]);
      p.z = static_cast<float>((d_l + noise) * n_l[2] + sa * u[2] + sb * v[2]);
      p.a = 255;
      out.points.push_back(p);
    }
  }
  return out;
}

}  // namespace

// ===========================================================================

TEST_CASE("extrinsics/wrapper_recovers_the_mount_from_synthetic_detections") {
  double truth[16];
  truth_mount(truth);

  ExtrinsicsSolverConfig cfg;
  cad_nominal(truth, cfg.cad_camera_from_lidar);
  cfg.default_sigma_m = 0.02;
  MountExtrinsicsSolver solver(cfg);

  Rng rng(0xC0FFEEu);
  const int n = 12;
  for (int i = 0; i < n; ++i) {
    WizardPose pose = make_pose(truth, i, n, 0.02, &rng);
    const std::int64_t t = 1'000'000'000LL + static_cast<std::int64_t>(i) * 300'000'000LL;
    pose.det.t_engine_ns = t;
    // The two halves arrive independently, exactly as they do in the wizard:
    // the detector posts its plane, and the capture path posts the keyframe
    // with the segmented returns.
    REQUIRE(solver.add_detection(pose.det).ok());
    REQUIRE(solver.add_observation(make_keyframe(t, i),
                                   Span<const PointVertex>(pose.points.data(),
                                                           pose.points.size()))
                .ok());
  }
  CHECK(solver.observation_count() == static_cast<std::size_t>(n));
  // Every detection was consumed by the keyframe it belonged to.
  CHECK(solver.pending_detections() == 0);
  // Nothing has been solved yet, so there is no gate to report.
  CHECK(solver.split_half_agreement_px() == -1.f);
  CHECK(solver.gate() == CalibGate::kUnknown);

  double got[16];
  REQUIRE(solver.solve(got).ok());
  CHECK(solver.result().converged);
  CHECK_FALSE(solver.result().degenerate);

  double rot_deg = 0, trans_mm = 0;
  se3::transform_error(truth, got, &rot_deg, &trans_mm);
  MESSAGE("recovered mount: " << rot_deg << " deg, " << trans_mm << " mm from truth; split-half "
                              << solver.split_half_agreement_px() << " px");
  // docs/A8-pushbroom.md §5.1 measures the SOLVER at 12 poses / 20 mm noise as
  // 0.172 deg / 2.4 mm (median over 21 sessions). This is one fixed seed
  // through the WRAPPER, so the bound is loose enough not to be a re-run of
  // A8's accuracy study — what it is actually asserting is that the adapter
  // put the plane in the right frame. A transposed rotation or a sign error in
  // the camera->lidar plane transform lands tens of degrees away, not
  // fractions of one.
  CHECK(rot_deg < 0.5);
  CHECK(trans_mm < 5.0);

  SUBCASE("the gate is A8's, reported in the wizard's units") {
    CHECK(solver.split_half_agreement_px() >= 0.f);
    CHECK(solver.gate() != CalibGate::kUnknown);
    // WIZARD.md: "±5 mm at 3 m — Good", never pixels. A8 §6's conversion is
    // px * range / fx, so the two must agree exactly.
    const double px = solver.split_half_agreement_px();
    CHECK(solver.split_half_mm_at(3.0) == doctest::Approx(px * 3.0 / kFx * 1000.0));
    CHECK(solver.split_half_mm_at(0.0) == -1.0);
  }

  SUBCASE("clear() puts it back to a fresh solver") {
    solver.clear();
    CHECK(solver.observation_count() == 0);
    CHECK(solver.split_half_agreement_px() == -1.f);
    CHECK(solver.solve(got).error() == ScanError::kInvalidState);
  }
}

TEST_CASE("extrinsics/the_keyframe_pose_is_not_used_so_tracking_loss_is_not_fatal") {
  // The residual is point-on-plane between two SENSOR frames at one instant.
  // ARCore's world pose does not enter it, which means a wizard shot taken
  // while tracking was lost is still a perfectly good calibration
  // observation — worth asserting, because the opposite assumption would
  // quietly throw away half a bench capture.
  double truth[16];
  truth_mount(truth);
  ExtrinsicsSolverConfig cfg;
  cad_nominal(truth, cfg.cad_camera_from_lidar);

  MountExtrinsicsSolver good(cfg), lost(cfg);
  Rng r1(7), r2(7);
  const int n = 8;
  for (int i = 0; i < n; ++i) {
    const std::int64_t t = static_cast<std::int64_t>(i) * 1'000'000LL;
    WizardPose a = make_pose(truth, i, n, 0.02, &r1);
    WizardPose b = make_pose(truth, i, n, 0.02, &r2);
    a.det.t_engine_ns = t;
    b.det.t_engine_ns = t;
    Keyframe kf_ok = make_keyframe(t, i);
    Keyframe kf_lost = make_keyframe(t, i);
    kf_lost.pose.tracking_lost = 1;
    kf_lost.pose.quality = PoseQuality::kInvalid;
    kf_lost.flags |= kKeyframeFlagTrackingLost;
    REQUIRE(good.add_detection(a.det).ok());
    REQUIRE(good.add_observation(kf_ok, Span<const PointVertex>(a.points.data(), a.points.size()))
                .ok());
    REQUIRE(lost.add_detection(b.det).ok());
    REQUIRE(lost.add_observation(kf_lost,
                                 Span<const PointVertex>(b.points.data(), b.points.size()))
                .ok());
  }
  double m1[16], m2[16];
  REQUIRE(good.solve(m1).ok());
  REQUIRE(lost.solve(m2).ok());
  for (int i = 0; i < 16; ++i) CHECK(m1[i] == doctest::Approx(m2[i]));
}

TEST_CASE("extrinsics/pairing_and_validation") {
  double truth[16];
  truth_mount(truth);
  ExtrinsicsSolverConfig cfg;
  cad_nominal(truth, cfg.cad_camera_from_lidar);
  MountExtrinsicsSolver solver(cfg);
  Rng rng(11);
  WizardPose pose = make_pose(truth, 0, 8, 0.02, &rng);
  const Span<const PointVertex> pts(pose.points.data(), pose.points.size());

  SUBCASE("a keyframe with no detection is kNotFound, not a silent skip") {
    // "The detector found no board in this frame" is something the wizard has
    // to SAY — an observation that quietly vanishes leaves the operator
    // wondering why the shot counter did not move.
    CHECK(solver.add_observation(make_keyframe(500, 0), pts).error() == ScanError::kNotFound);
  }

  SUBCASE("a detection outside the match window does not pair") {
    pose.det.t_engine_ns = 0;
    REQUIRE(solver.add_detection(pose.det).ok());
    CHECK(solver.add_observation(make_keyframe(50'000'000, 0), pts).error() ==
          ScanError::kNotFound);
    // ... and it is still pending, so the right keyframe can still claim it.
    CHECK(solver.pending_detections() == 1);
    CHECK(solver.add_observation(make_keyframe(1'000'000, 0), pts).ok());
    CHECK(solver.pending_detections() == 0);
  }

  SUBCASE("a non-unit normal is refused at the detection, not at the solve") {
    BoardDetection bad = pose.det;
    bad.normal[0] = 0.5;
    bad.normal[1] = 0.5;
    bad.normal[2] = 0.5;  // length 0.87
    CHECK(solver.add_detection(bad).error() == ScanError::kInvalidArgument);
  }

  SUBCASE("a board behind the camera is refused") {
    BoardDetection bad = pose.det;
    bad.d = -1.5;
    CHECK(solver.add_detection(bad).error() == ScanError::kInvalidArgument);
    bad.d = 0.0;
    CHECK(solver.add_detection(bad).error() == ScanError::kInvalidArgument);
  }

  SUBCASE("a non-finite plane is refused") {
    BoardDetection bad = pose.det;
    bad.normal[1] = std::nan("");
    CHECK(solver.add_detection(bad).error() == ScanError::kInvalidArgument);
  }

  SUBCASE("re-detecting one frame replaces, it does not duplicate") {
    pose.det.t_engine_ns = 42;
    REQUIRE(solver.add_detection(pose.det).ok());
    BoardDetection again = pose.det;
    again.d += 0.01;
    REQUIRE(solver.add_detection(again).ok());
    CHECK(solver.pending_detections() == 1);
  }

  SUBCASE("solve with nothing to solve") {
    double m[16];
    CHECK(solver.solve(m).error() == ScanError::kInvalidState);
    CHECK(solver.solve(nullptr).error() == ScanError::kInvalidArgument);
  }
}

TEST_CASE("extrinsics/the_gate_camera_comes_from_the_keyframes") {
  double truth[16];
  truth_mount(truth);
  ExtrinsicsSolverConfig cfg;
  cad_nominal(truth, cfg.cad_camera_from_lidar);
  // The default is S6's model camera, which is NOT the camera these keyframes
  // came from.
  CHECK(cfg.calib.camera.fx == doctest::Approx(2912.0));
  MountExtrinsicsSolver solver(cfg);

  Rng rng(3);
  WizardPose pose = make_pose(truth, 0, 8, 0.02, &rng);
  pose.det.t_engine_ns = 100;
  REQUIRE(solver.add_detection(pose.det).ok());

  Keyframe kf = make_keyframe(100, 0);
  kf.intrinsics.fx = 1400.f;  // a different phone
  kf.intrinsics.fy = 1400.f;
  kf.intrinsics.width = 1920;
  kf.intrinsics.height = 1440;
  REQUIRE(solver.add_observation(kf, Span<const PointVertex>(pose.points.data(),
                                                             pose.points.size()))
              .ok());
  // The split-half gate is quoted in PIXELS at a range, so it has to be
  // measured in the pixels of the camera that will do the colouring.
  CHECK(solver.solver().config().camera.fx == doctest::Approx(1400.0));
  CHECK(solver.solver().config().camera.width == 1920u);

  SUBCASE("a second camera in the same calibration is refused") {
    WizardPose p2 = make_pose(truth, 1, 8, 0.02, &rng);
    p2.det.t_engine_ns = 200;
    REQUIRE(solver.add_detection(p2.det).ok());
    Keyframe other = make_keyframe(200, 1);  // back to the 4032x3024 model
    CHECK(solver.add_observation(other, Span<const PointVertex>(p2.points.data(),
                                                                p2.points.size()))
              .error() == ScanError::kInvalidArgument);
  }

  SUBCASE("camera_from_keyframes off leaves the caller's camera alone") {
    ExtrinsicsSolverConfig fixed = cfg;
    fixed.camera_from_keyframes = false;
    MountExtrinsicsSolver s2(fixed);
    WizardPose p2 = make_pose(truth, 2, 8, 0.02, &rng);
    p2.det.t_engine_ns = 300;
    REQUIRE(s2.add_detection(p2.det).ok());
    Keyframe k2 = make_keyframe(300, 2);
    k2.intrinsics.fx = 1400.f;
    REQUIRE(s2.add_observation(k2, Span<const PointVertex>(p2.points.data(), p2.points.size()))
                .ok());
    CHECK(s2.solver().config().camera.fx == doctest::Approx(2912.0));
  }
}

TEST_CASE("extrinsics/too_few_observations_is_degenerate_and_gated_reject") {
  // A8's floor is 5 configurations for a plane-only solve. Fewer still
  // ANSWERS — a caller may want to inspect it — but it is marked degenerate
  // and gated kReject, and the wrapper must not hide either.
  double truth[16];
  truth_mount(truth);
  ExtrinsicsSolverConfig cfg;
  cad_nominal(truth, cfg.cad_camera_from_lidar);
  MountExtrinsicsSolver solver(cfg);

  Rng rng(5);
  for (int i = 0; i < 3; ++i) {
    WizardPose p = make_pose(truth, i, 3, 0.02, &rng);
    p.det.t_engine_ns = i;
    REQUIRE(solver.add_detection(p.det).ok());
    REQUIRE(
        solver.add_observation(make_keyframe(i, i),
                               Span<const PointVertex>(p.points.data(), p.points.size()))
            .ok());
  }
  double m[16];
  const Status st = solver.solve(m);
  if (st.ok()) {
    CHECK(solver.result().degenerate);
    CHECK(solver.gate() == CalibGate::kReject);
  } else {
    CHECK(st.error() == ScanError::kInvalidArgument);
  }
}
