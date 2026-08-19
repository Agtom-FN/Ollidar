// ROUND 20 item 80 — the offline auto-level: floor-vs-gravity as an
// extrinsics-error meter, pitch/roll-only correction, ruler voting last.
//
// The owner-capture truths this exists to reproduce (measured with engine_cli
// on copies of the owner's bundles, 2026-08-19):
//   scan-054  floor tilt 3.61 deg, selfCheck 1.97 cm  -> expected ~1 deg / ~1.2 cm
//   scan-055  floor tilt 6.30 deg, selfCheck 5.79 cm  -> expected better
//   scan-056  floor tilt 1.09 deg — ALREADY LEVEL, must be refused as a no-op
//
// The fixtures below are synthetic (ground truth to the millimetre); the real
// bundles are validated with engine_cli in the round notes.

#include "microtest_shim.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "scanengine/poses/se3.h"
#include "scanengine/slam/post/auto_level.h"

using namespace scanengine;
using namespace scanengine::post;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000ll;

PointVertex pv(double x, double y, double z) {
  PointVertex p{};
  p.x = static_cast<float>(x);
  p.y = static_cast<float>(y);
  p.z = static_cast<float>(z);
  p.a = 255;
  return p;
}

TrajPose pose_at(std::int64_t t_ns, const double q[4]) {
  TrajPose p;
  p.t_ns = t_ns;
  std::memcpy(p.q, q, sizeof(double) * 4);
  p.p[0] = p.p[1] = p.p[2] = 0.0;
  return p;
}

struct Cloud {
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  std::vector<TrajPose> poses;
};

// A level room: floor 4x4 m at y = -1.4, two walls, all painted from identity
// attitude over 10 s.
Cloud level_room() {
  Cloud c;
  const double id[4] = {0, 0, 0, 1};
  for (int i = 0; i <= 300; ++i) c.poses.push_back(pose_at(i * kSecond / 30, id));
  std::int64_t t = 0;
  auto push = [&](double x, double y, double z) {
    c.pts.push_back(pv(x, y, z));
    c.times.push_back(t % (10 * kSecond));
    t += 700'000;
  };
  for (double x = -2.0; x <= 2.0; x += 0.05) {
    for (double z = -2.0; z <= 2.0; z += 0.05) push(x, -1.4, z);
  }
  for (double y = -1.4; y <= 1.0; y += 0.05) {
    for (double z = -2.0; z <= 2.0; z += 0.05) push(2.0, y, z);
  }
  for (double y = -1.4; y <= 1.0; y += 0.05) {
    for (double x = -2.0; x <= 2.0; x += 0.05) push(x, y, 2.0);
  }
  return c;
}

// The same room with a KNOWN extrinsics tilt applied: every point rotated by
// `deg` about the world Z axis through the (origin-held) trajectory — exactly
// what a phone-frame pitch/roll trim error does to a resolve whose poses sit
// at the origin with identity attitude.
Cloud tilted_room(double deg) {
  Cloud c = level_room();
  double q[4];
  const double rv[3] = {0.0, 0.0, deg * se3::kDegToRad};
  se3::quat_from_rotvec(rv, q);
  for (PointVertex& p : c.pts) {
    const double v[3] = {p.x, p.y, p.z};
    double u[3] = {q[0], q[1], q[2]};
    double uv[3], uuv[3];
    se3::cross3(u, v, uv);
    se3::cross3(u, uv, uuv);
    p.x = static_cast<float>(v[0] + 2.0 * (q[3] * uv[0] + uuv[0]));
    p.y = static_cast<float>(v[1] + 2.0 * (q[3] * uv[1] + uuv[1]));
    p.z = static_cast<float>(v[2] + 2.0 * (q[3] * uv[2] + uuv[2]));
  }
  return c;
}

AutoLevelConfig no_ruler_cfg() {
  AutoLevelConfig cfg;
  cfg.require_self_consistency = false;  // the geometry tests; the ruler has its own case
  return cfg;
}

bool bytes_equal(const std::vector<PointVertex>& a, const std::vector<PointVertex>& b) {
  if (a.size() != b.size()) return false;
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(PointVertex)) == 0;
}

}  // namespace

TEST_CASE("round20/auto-level/a level floor is refused as already-level, bytes untouched") {
  Cloud c = level_room();
  const std::vector<PointVertex> before = c.pts;
  const AutoLevelReport r = auto_level_floor(c.pts, c.times, c.poses, no_ruler_cfg());
  CHECK(r.decision == AutoLevelDecision::kAlreadyLevel);
  CHECK(r.floor_found);
  CHECK(r.tilt_before_deg < 0.5);
  CHECK(bytes_equal(before, c.pts));
}

TEST_CASE("round20/auto-level/a 3.6 deg tilt (scan-054's) is measured and leveled") {
  Cloud c = tilted_room(3.61);
  const AutoLevelReport r = auto_level_floor(c.pts, c.times, c.poses, no_ruler_cfg());
  CHECK(r.decision == AutoLevelDecision::kApplied);
  CHECK(r.tilt_before_deg == doctest::Approx(3.61).epsilon(0.05));
  CHECK(r.tilt_after_deg < 0.35);
  CHECK(r.correction_deg == doctest::Approx(3.61).epsilon(0.10));
  // Yaw honesty: with identity poses the phone-frame correction IS the
  // world-frame one, and its about-gravity twist must be zero — a floor
  // cannot witness yaw, so none may be invented.
  const double twist_y = std::fabs(r.correction_quat[1]);
  CHECK(twist_y < 0.01);
}

TEST_CASE("round20/auto-level/scan-055's 6.3 deg is inside the working range") {
  Cloud c = tilted_room(6.30);
  const AutoLevelReport r = auto_level_floor(c.pts, c.times, c.poses, no_ruler_cfg());
  CHECK(r.decision == AutoLevelDecision::kApplied);
  CHECK(r.tilt_before_deg == doctest::Approx(6.30).epsilon(0.05));
  CHECK(r.tilt_after_deg < 0.35);
}

TEST_CASE("round20/auto-level/a 25 deg 'floor' is refused — that is not a trim error") {
  Cloud c = tilted_room(25.0);
  const std::vector<PointVertex> before = c.pts;
  const AutoLevelReport r = auto_level_floor(c.pts, c.times, c.poses, no_ruler_cfg());
  CHECK(r.decision == AutoLevelDecision::kTiltTooBig);
  CHECK(bytes_equal(before, c.pts));
}

TEST_CASE("round20/auto-level/walls alone are no floor") {
  Cloud c;
  const double id[4] = {0, 0, 0, 1};
  for (int i = 0; i <= 300; ++i) c.poses.push_back(pose_at(i * kSecond / 30, id));
  std::int64_t t = 0;
  for (double y = -1.4; y <= 1.0; y += 0.02) {
    for (double z = -2.0; z <= 2.0; z += 0.02) {
      c.pts.push_back(pv(2.0, y, z));
      c.times.push_back(t % (10 * kSecond));
      t += 700'000;
    }
  }
  const std::vector<PointVertex> before = c.pts;
  const AutoLevelReport r = auto_level_floor(c.pts, c.times, c.poses, no_ruler_cfg());
  CHECK(r.decision == AutoLevelDecision::kNoFloor);
  CHECK(bytes_equal(before, c.pts));
}

TEST_CASE("round20/auto-level/a glimpse of floor is thin-floor, not evidence") {
  Cloud c;
  const double id[4] = {0, 0, 0, 1};
  for (int i = 0; i <= 300; ++i) c.poses.push_back(pose_at(i * kSecond / 30, id));
  std::int64_t t = 0;
  auto push = [&](double x, double y, double z) {
    c.pts.push_back(pv(x, y, z));
    c.times.push_back(t % (10 * kSecond));
    t += 700'000;
  };
  // A 0.6 x 0.6 m patch, tilted — a desk seen through a doorway. 900 points
  // clears the RANSAC's 500-candidate floor but not min_inliers/coverage.
  for (double x = -0.3; x <= 0.3; x += 0.02) {
    for (double z = -0.3; z <= 0.3; z += 0.02) push(x, -1.4 + 0.06 * x, z);
  }
  // Enough wall to make the cloud big enough to judge at all.
  for (double y = -1.0; y <= 1.0; y += 0.05) {
    for (double z = -2.0; z <= 2.0; z += 0.05) push(2.0, y, z);
  }
  const std::vector<PointVertex> before = c.pts;
  const AutoLevelReport r = auto_level_floor(c.pts, c.times, c.poses, no_ruler_cfg());
  CHECK(r.decision == AutoLevelDecision::kThinFloor);
  CHECK(bytes_equal(before, c.pts));
}

TEST_CASE("round20/auto-level/the ruler votes last — a leveling that shears the map is refused") {
  // A wall painted TWICE (two windows, identical points) plus a genuinely
  // tilted 4-deg floor painted once. Most poses are identity, but the second
  // painting of the wall was walked at yaw 180 — so the phone-frame
  // correction that levels the floor rotates the two paintings of the wall in
  // OPPOSITE world directions, and the whole-map self-check gets worse. The
  // floor looks better; the map is worse; the ruler must win.
  Cloud c;
  const double id[4] = {0, 0, 0, 1};
  double yaw180[4];
  {
    const double rv[3] = {0.0, se3::kPi, 0.0};
    se3::quat_from_rotvec(rv, yaw180);
  }
  // Poses: 0-12 s identity, 12-14 s yaw180, 14-16 s identity.
  for (int i = 0; i <= 480; ++i) {
    const std::int64_t t = i * kSecond / 30;
    const bool flipped = t >= 12 * kSecond && t <= 14 * kSecond;
    c.poses.push_back(pose_at(t, flipped ? yaw180 : id));
  }
  auto push = [&](double x, double y, double z, std::int64_t t) {
    c.pts.push_back(pv(x, y, z));
    c.times.push_back(t);
  };
  // The tilted floor, window 0 (t ~ 1 s).
  for (double x = -2.0; x <= 2.0; x += 0.05) {
    for (double z = -2.0; z <= 2.0; z += 0.05) push(x, -1.4 + std::tan(4.0 * se3::kDegToRad) * x, z, 1 * kSecond);
  }
  // The wall at x = 2, painted identically in window 0 (t ~ 2 s, identity
  // poses) and window 1 (t ~ 13 s, yaw-180 poses).
  for (double y = -1.4; y <= 1.0; y += 0.04) {
    for (double z = -1.0; z <= 1.0; z += 0.04) {
      push(2.0, y, z, 2 * kSecond);
      push(2.0, y, z, 13 * kSecond);
    }
  }
  const std::vector<PointVertex> before = c.pts;
  AutoLevelConfig cfg;  // ruler ON — that is the point
  const AutoLevelReport r = auto_level_floor(c.pts, c.times, c.poses, cfg);
  CHECK(r.decision == AutoLevelDecision::kRulerSaysWorse);
  CHECK(r.self_check_checked);
  CHECK(r.self_check_after_m > r.self_check_before_m);
  CHECK(bytes_equal(before, c.pts));
}

TEST_CASE("round20/auto-level/determinism — same bytes in, same verdict and same bytes out") {
  Cloud a = tilted_room(3.0);
  Cloud b = tilted_room(3.0);
  const AutoLevelReport ra = auto_level_floor(a.pts, a.times, a.poses, no_ruler_cfg());
  const AutoLevelReport rb = auto_level_floor(b.pts, b.times, b.poses, no_ruler_cfg());
  CHECK(ra.decision == rb.decision);
  CHECK(ra.tilt_before_deg == rb.tilt_before_deg);
  CHECK(ra.tilt_after_deg == rb.tilt_after_deg);
  CHECK(bytes_equal(a.pts, b.pts));
}
