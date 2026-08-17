// ROUND 9, owner item 34 — the chirality proof.
//
// > "The output is left right reversed."
//
// Every geometry test in this suite before this file measured a SIGN-BLIND
// quantity: axis extents, best-fit-plane RMS, point counts, live-vs-offline
// equality. A mirrored room has exactly the same extents, exactly the same
// planarity and exactly the same point count as the real one, so all of them
// stayed green for eight rounds while the cloud came out backwards.
//
// This file is the first test that can tell a room from its mirror image.
//
// The stimulus is a corridor walk with ONE asymmetric feature — a doorway in
// the wall on the operator's LEFT — ray-cast against the production fan frame
// (`drivers/d6/d6_fan.h`) and resolved through the production assembler with
// the production CAD nominal extrinsic. "Left" is not hard-coded: it is
// recomputed from the resolved trajectory as `up x forward`, so the assertion
// survives anyone rotating the fixture.
//
// The falsifiable control is the whole point. `d6_legacy_fan_extrinsic()`
// reproduces the pre-ROUND-9 pipeline exactly (see d6_fan.h §5), so the second
// arm runs the SAME synthetic returns through the SAME assembler under the old
// convention — and must place the doorway on the RIGHT. If that arm ever stops
// failing the left-hand assertion, the fix has become untestable.

#include "scanengine/drivers/d6/d6_fan.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/poses/se3.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"

using namespace scanengine;

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- the world -------------------------------------------------------------
//
// ARCore's frame: +Y up, gravity-aligned. The phone is held portrait with its
// BACK facing the direction of travel, which is the owner's stated rig, so the
// camera basis and the world basis coincide and the phone's orientation is the
// identity quaternion for the whole walk. That is deliberate: it removes the
// trajectory's rotation from the argument entirely, leaving the fan convention
// and the mount extrinsic as the ONLY things that can decide handedness.
//
//   walk forward = world -Z   (ARCore looks along -Z, and the back faces it)
//   up           = world +Y
//   left         = up x forward = (0,1,0) x (0,0,-1) = (-1,0,0)
//
// The corridor: 3 m wide, walls at x = -1.5 (LEFT) and x = +1.5 (RIGHT),
// floor at y = 0, ceiling at y = +2.6. The rig walks the centre line at
// y = 1.4 m from z = 0 to z = -10 m.
//
// The asymmetric landmark: the LEFT wall has a 2 m doorway over z in
// [-6, -4]. Nothing else in the room distinguishes left from right.

constexpr double kHalfWidth = 1.5;
constexpr double kCeiling = 2.6;
constexpr double kRigHeight = 1.4;
constexpr double kDoorZLo = -6.0;
constexpr double kDoorZHi = -4.0;
constexpr double kWalkLen = 10.0;
constexpr double kSpeed = 1.0;  // m/s

// The CAD nominal, verbatim from BracketNominals.cadNominal(COIN_D6):
// identity rotation, lidar 6 cm below and 3.5 cm behind the camera.
void cad_nominal(double m[16]) {
  se3::mat4_identity(m);
  m[3] = 0.000;
  m[7] = -0.060;
  m[11] = -0.035;
}

void rig_position(double t, double out[3]) {
  out[0] = 0.0;
  out[1] = kRigHeight;
  out[2] = -kSpeed * t;
}

Pose walk_pose(std::int64_t t_ns) {
  Pose p{};
  p.t_mono_ns = t_ns;
  rig_position(static_cast<double>(t_ns) * 1e-9, p.position);
  p.orientation[0] = 0.0;
  p.orientation[1] = 0.0;
  p.orientation[2] = 0.0;
  p.orientation[3] = 1.0;  // identity: back faces -Z, up is +Y
  p.source = StreamId::kPoseAr;
  p.quality = PoseQuality::kGood;
  p.tracking_lost = 0;
  return p;
}

// Ray-cast one fan return against the corridor. `origin` is the SENSOR origin
// in world (rig position plus the nominal's lever arm, which is a pure
// translation here). Returns < 0 for "no return", which is what a doorway is.
//
// The ray direction is taken from the production fan frame. With the identity
// mount rotation the fan lies in the world x-y plane, so each revolution is a
// clean cross-section of the corridor at one z.
double range_along_fan(const double origin[3], double angle_deg) {
  double dir[3];
  d6::fan_point(angle_deg, 1.0, dir);  // unit ray, (x, y, 0)

  double best = -1.0;

  // The two side walls.
  for (int s = 0; s < 2; ++s) {
    const double wall_x = (s == 0) ? -kHalfWidth : kHalfWidth;
    if (std::fabs(dir[0]) < 1e-9) continue;
    const double d = (wall_x - origin[0]) / dir[0];
    if (d <= 0.05 || d > 12.0) continue;
    const double y = origin[1] + d * dir[1];
    if (y < 0.0 || y > kCeiling) continue;
    // The doorway: the LEFT wall is simply absent over this z band, so a ray
    // aimed into it comes back with nothing.
    const bool is_left = (wall_x < 0.0);
    if (is_left && origin[2] >= kDoorZLo && origin[2] <= kDoorZHi) continue;
    if (best < 0.0 || d < best) best = d;
  }
  // Floor and ceiling, so the cloud is a room and not two stripes.
  for (int s = 0; s < 2; ++s) {
    const double plane_y = (s == 0) ? 0.0 : kCeiling;
    if (std::fabs(dir[1]) < 1e-9) continue;
    const double d = (plane_y - origin[1]) / dir[1];
    if (d <= 0.05 || d > 12.0) continue;
    const double x = origin[0] + d * dir[0];
    if (std::fabs(x) > kHalfWidth) continue;
    if (best < 0.0 || d < best) best = d;
  }
  return best;
}

struct Arm {
  std::vector<PointVertex> pts;
  double forward[3] = {0, 0, 0};
};

// Walk the corridor and resolve it. `legacy` swaps in the pre-ROUND-9
// convention by post-multiplying the extrinsic with diag(-1, +1, -1), which is
// algebraically identical to running the old `x = +d*sin(theta)` fan.
Arm walk_the_corridor(bool legacy) {
  constexpr std::int64_t kPoseNs = 33'333'333LL;  // ARCore ~30 Hz
  constexpr std::int64_t kRevNs = 100'000'000LL;  // D6 10 Hz
  constexpr int kReturnsPerRev = 400;             // 4 kHz sampling / 10 Hz
  constexpr std::int64_t kT0 = 1'000'000'000LL;
  const std::int64_t kEndNs =
      kT0 + static_cast<std::int64_t>(kWalkLen / kSpeed * 1e9);

  double mount[16];
  cad_nominal(mount);
  if (legacy) {
    double legacy_mount[16];
    d6::d6_legacy_fan_extrinsic(mount, legacy_mount);
    for (int i = 0; i < 16; ++i) mount[i] = legacy_mount[i];
  }

  ExternalPoseSource poses;
  for (std::int64_t t = kT0 - 2 * kPoseNs; t <= kEndNs + 2 * kPoseNs; t += kPoseNs) {
    REQUIRE(poses.push_pose(walk_pose(t)).ok());
  }

  PageStore store;
  D6PushbroomAssembler a(&store);
  REQUIRE(a.set_mount_extrinsics(mount).ok());
  a.set_pose_source(&poses);

  for (std::int64_t t_rev = kT0; t_rev < kEndNs; t_rev += kRevNs) {
    std::vector<ProfilePoint> prof;
    for (int i = 0; i < kReturnsPerRev; ++i) {
      const std::int64_t t_pt = t_rev + kRevNs * i / kReturnsPerRev;
      const double angle = 360.0 * i / kReturnsPerRev;

      // The sensor origin at this instant: rig position plus the nominal's
      // lever arm. The mount rotation is the identity in BOTH arms' TRUTH —
      // the legacy arm mirrors the RECONSTRUCTION, never the physics.
      double origin[3];
      rig_position(static_cast<double>(t_pt) * 1e-9, origin);
      origin[0] += 0.000;
      origin[1] += -0.060;
      origin[2] += -0.035;

      const double d = range_along_fan(origin, angle);
      if (d < 0.0) continue;

      ProfilePoint p;
      p.t_mono_ns = t_pt;
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

  Arm arm;
  const auto ids = store.page_ids();
  for (const auto id : ids) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t i = 0; i < v.count; ++i) arm.pts.push_back(v.data[i]);
  }

  // Forward, measured from the trajectory itself rather than assumed.
  double p0[3], p1[3];
  rig_position(0.0, p0);
  rig_position(kWalkLen / kSpeed, p1);
  double n = 0.0;
  for (int i = 0; i < 3; ++i) {
    arm.forward[i] = p1[i] - p0[i];
    n += arm.forward[i] * arm.forward[i];
  }
  n = std::sqrt(n);
  for (int i = 0; i < 3; ++i) arm.forward[i] /= n;
  return arm;
}

// The task's definition, computed rather than written down: left = up x fwd.
void left_hat(const double forward[3], double out[3]) {
  const double up[3] = {0.0, 1.0, 0.0};
  se3::cross3(up, forward, out);
  se3::normalize3(out);
}

// How many wall returns fall on each side, inside the doorway's z band.
struct Sides {
  int left = 0;
  int right = 0;
};

Sides count_sides_in_door_band(const Arm& arm) {
  double lh[3];
  left_hat(arm.forward, lh);
  Sides s;
  for (const auto& p : arm.pts) {
    if (p.z < kDoorZLo || p.z > kDoorZHi) continue;
    // Wall returns only: ignore floor and ceiling, which are symmetric and
    // would dilute the count.
    if (p.y < 0.35 || p.y > kCeiling - 0.35) continue;
    const double lat = p.x * lh[0] + p.y * lh[1] + p.z * lh[2];
    if (lat > 0.9) ++s.left;
    if (lat < -0.9) ++s.right;
  }
  return s;
}

}  // namespace

// ===========================================================================
// 1. The frame itself
// ===========================================================================

TEST_CASE("round9/fan_frame_matches_the_documented_derivation") {
  double p[3];

  // theta = 0 is the zero mark, on +y.
  d6::fan_point(0.0, 2.0, p);
  CHECK(p[0] == doctest::Approx(0.0).epsilon(1e-12));
  CHECK(p[1] == doctest::Approx(2.0).epsilon(1e-12));
  CHECK(p[2] == 0.0);

  // theta = 90 deg is on -x. THIS is the sign ROUND 9 corrected; before the
  // fix it was +x, and that one character mirrored every cloud the app has
  // ever produced.
  d6::fan_point(90.0, 2.0, p);
  CHECK(p[0] == doctest::Approx(-2.0).epsilon(1e-12));
  CHECK(p[1] == doctest::Approx(0.0).epsilon(1e-12));

  d6::fan_point(180.0, 2.0, p);
  CHECK(p[1] == doctest::Approx(-2.0).epsilon(1e-12));
  d6::fan_point(270.0, 2.0, p);
  CHECK(p[0] == doctest::Approx(2.0).epsilon(1e-12));

  // Every return is exactly in the z = 0 plane. This is why the bug could
  // masquerade as a proper rotation for eight rounds.
  for (double a = 0.0; a < 360.0; a += 7.5) {
    d6::fan_point(a, 3.0, p);
    CHECK(p[2] == 0.0);
    // ...and the round-trip through the Cartesian seam is exact.
    const double back = d6::fan_angle_deg(p[0], p[1]);
    CHECK(std::fabs(back - a) < 1e-9);
  }
}

TEST_CASE("round9/legacy_fan_is_a_reflection_realised_by_a_proper_rotation") {
  // The claim d6_fan.h §3 rests on: old_fan == diag(-1,+1,-1) * new_fan, and
  // that matrix has det = +1. A reflection of a PLANAR fan is achievable by a
  // proper rotation, which is exactly why `mat4_is_rigid` never fired and why
  // the owner's stored extrinsic being det=+1 proved nothing.
  double m[16];
  cad_nominal(m);
  CHECK(se3::mat4_is_rigid(m, 1e-9));

  double legacy[16];
  d6::d6_legacy_fan_extrinsic(m, legacy);
  CHECK(se3::mat4_is_rigid(legacy, 1e-9));  // still a PROPER rotation, det = +1

  for (double a = 0.0; a < 360.0; a += 11.0) {
    double neu[3], old[3];
    d6::fan_point(a, 2.5, neu);
    // The pre-ROUND-9 formula, written out here and nowhere else in the tree.
    const double rad = a * kPi / 180.0;
    old[0] = 2.5 * std::sin(rad);
    old[1] = 2.5 * std::cos(rad);
    old[2] = 0.0;

    double via_legacy[3], via_new[3];
    se3::mat4_apply(m, old, via_legacy);
    se3::mat4_apply(legacy, neu, via_new);
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(via_legacy[i] - via_new[i]) < 1e-12);
  }

  // It is its own inverse, so re-resolving is idempotent in both directions.
  double back[16];
  d6::d6_legacy_fan_extrinsic(legacy, back);
  for (int i = 0; i < 16; ++i) CHECK(std::fabs(back[i] - m[i]) < 1e-15);
}

// ===========================================================================
// 2. THE PROOF — a doorway on the left must come out on the left
// ===========================================================================

TEST_CASE("round9/a_doorway_on_the_left_resolves_on_the_left") {
  const Arm fixed = walk_the_corridor(/*legacy=*/false);
  const Arm legacy = walk_the_corridor(/*legacy=*/true);

  REQUIRE(fixed.pts.size() > 20000);
  // Identical stimulus: the two arms differ only in the reconstruction.
  CHECK(fixed.pts.size() == legacy.pts.size());

  double lh[3];
  left_hat(fixed.forward, lh);
  MESSAGE("walk forward = (" << fixed.forward[0] << ", " << fixed.forward[1] << ", "
                             << fixed.forward[2] << "), left = up x forward = (" << lh[0]
                             << ", " << lh[1] << ", " << lh[2] << ")");

  const Sides f = count_sides_in_door_band(fixed);
  const Sides l = count_sides_in_door_band(legacy);
  MESSAGE("doorway band, wall returns -- FIXED: left " << f.left << ", right " << f.right
                                                       << "  |  LEGACY: left " << l.left
                                                       << ", right " << l.right);

  // --- the assertion ------------------------------------------------------
  // The doorway was cut into the LEFT wall, so in its z band the left side is
  // empty and the right side is a solid wall.
  CHECK(f.left == 0);
  CHECK(f.right > 500);

  // --- the falsifiable control -------------------------------------------
  // The pre-fix convention, same returns, same assembler, must FAIL the line
  // above: it puts the doorway on the right.
  CHECK(l.right == 0);
  CHECK(l.left > 500);
  CHECK_FALSE(l.left == 0);  // spelled out: the old arm fails the real test

  // ...and the two clouds really are mirror images of each other about the
  // walk axis, not merely different.
  CHECK(fixed.pts.size() == legacy.pts.size());
  double worst = 0.0;
  for (std::size_t i = 0; i < fixed.pts.size(); ++i) {
    // The corridor is symmetric in x apart from the doorway, and `left` is
    // -x here, so mirroring means negating the lateral coordinate.
    const double dx = static_cast<double>(fixed.pts[i].x) + static_cast<double>(legacy.pts[i].x);
    const double dy = static_cast<double>(fixed.pts[i].y) - static_cast<double>(legacy.pts[i].y);
    const double dz = static_cast<double>(fixed.pts[i].z) - static_cast<double>(legacy.pts[i].z);
    worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  MESSAGE("worst |fixed - mirror(legacy)| = " << worst * 1000.0 << " mm");
  CHECK(worst < 1e-6);
}

// ===========================================================================
// 3. Outside the doorway the corridor stays symmetric
// ===========================================================================

TEST_CASE("round9/the_rest_of_the_corridor_is_symmetric_so_only_the_doorway_decides") {
  const Arm fixed = walk_the_corridor(/*legacy=*/false);
  double lh[3];
  left_hat(fixed.forward, lh);

  int left = 0, right = 0;
  for (const auto& p : fixed.pts) {
    // Two bands, both well clear of the doorway's [-6, -4] z range.
    if (p.z > -1.0 || p.z < -9.0) continue;
    if (p.z < -3.5 && p.z > -6.5) continue;
    if (p.y < 0.35 || p.y > kCeiling - 0.35) continue;
    const double lat = p.x * lh[0] + p.y * lh[1] + p.z * lh[2];
    if (lat > 0.9) ++left;
    if (lat < -0.9) ++right;
  }
  MESSAGE("clear band -- left " << left << ", right " << right);
  REQUIRE(left > 500);
  REQUIRE(right > 500);
  // Within 5 %: the only asymmetry in this room is the doorway, which is the
  // point — if the walls themselves were lopsided the proof above would be
  // measuring the fixture instead of the convention. The couple of percent
  // that is left is band-edge quantisation (revolutions are discrete, and the
  // sensor sits 3.5 cm behind the rig origin so the door's z edges do not fall
  // exactly on a revolution). For scale, the doorway itself moves this ratio
  // to 1.50 — an order of magnitude larger than the residue.
  const double ratio = static_cast<double>(left) / static_cast<double>(right);
  CHECK(ratio > 0.95);
  CHECK(ratio < 1.05);
}

// ===========================================================================
// 4. ROUND 9: the phone-IMU extrinsic must survive the container
// ===========================================================================
//
// ROUND 8's lesson, applied a second time. A `.lscan` that carries a
// `kPhoneImu` stream but no IMU extrinsic is self-contained only by accident:
// the gyro samples are in the Android sensor frame, ARCore reports its pose in
// the camera frame, and on a real phone those differ by the camera's
// SENSOR_ORIENTATION — usually 90 degrees. Re-resolving without it does not
// fail loudly, it silently distorts the densified path between pose knots,
// which is the one thing the gyro was added to improve. So it goes in the
// manifest next to `mountCalibration`, and this pins the round trip.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "scanengine/record/lscan.h"

TEST_CASE("round9/the_imu_extrinsic_round_trips_through_the_manifest") {
  const std::string dir =
      std::string(std::tmpnam(nullptr)) + "-r9imucal.lscan";

  // A 90-degree rotation about z — the real SENSOR_ORIENTATION case, and
  // deliberately NOT the identity, so a writer that drops the field cannot
  // pass by accident.
  const double kCameraFromImu[4] = {0.0, 0.0, 0.70710678118654752,
                                    0.70710678118654752};
  {
    lscan::FileRecordWriter w;
    w.set_imu_calibration(kCameraFromImu);
    REQUIRE(w.open(dir).ok());
    REQUIRE(w.close().ok());
  }

  std::ifstream in(dir + "/" + lscan::kManifestFile, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string j = ss.str();

  // The key exists and carries the value, at full precision.
  CHECK(j.find("\"imuCalibration\"") != std::string::npos);
  CHECK(j.find("\"cameraFromImu\"") != std::string::npos);
  const std::size_t at = j.find("\"cameraFromImu\"");
  REQUIRE(at != std::string::npos);
  const std::size_t open_br = j.find('[', at);
  const std::size_t close_br = j.find(']', open_br);
  REQUIRE(open_br != std::string::npos);
  REQUIRE(close_br != std::string::npos);
  const std::string arr = j.substr(open_br + 1, close_br - open_br - 1);

  double got[4] = {0, 0, 0, 0};
  int n = 0;
  const char* p = arr.c_str();
  const char* end = p + arr.size();
  while (p < end && n < 4) {
    char* stop = nullptr;
    const double v = std::strtod(p, &stop);
    if (stop == p) { ++p; continue; }
    got[n++] = v;
    p = stop;
  }
  REQUIRE(n == 4);
  // Bit-exact, not Approx: a quaternion that decodes slightly differently
  // rotates the gyro slightly differently, and "slightly" is the whole point.
  for (int i = 0; i < 4; ++i) CHECK(got[i] == kCameraFromImu[i]);

  // Unset stays null, exactly as `mountCalibration` does, so a consumer can
  // rely on the key being present either way.
  const std::string dir2 = std::string(std::tmpnam(nullptr)) + "-r9noimucal.lscan";
  {
    lscan::FileRecordWriter w2;
    REQUIRE(w2.open(dir2).ok());
    REQUIRE(w2.close().ok());
  }
  std::ifstream in2(dir2 + "/" + lscan::kManifestFile, std::ios::binary);
  REQUIRE(in2.good());
  std::ostringstream ss2;
  ss2 << in2.rdbuf();
  CHECK(ss2.str().find("\"imuCalibration\": null") != std::string::npos);
}
