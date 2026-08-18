// ROUND 10, owner item 36 — the time offset between the two clocks, and the
// two claims made about it.
//
// > "current scan right when i go forward but when i turn around the scan
// >  position shifted"
//
// THE CLAIM THIS FILE EXISTS TO MAKE FALSIFIABLE, in two halves:
//
//  1. **A constant lidar/pose time offset is INVISIBLE in straight walking.**
//     Every geometry test in this repository before ROUND 10 walked in a
//     straight line at constant speed, which is exactly the motion that hides
//     it: a constant offset `dt` shifts every point by `v*dt` in the SAME
//     direction, so the room is translated a couple of centimetres and every
//     wall is exactly as straight as it was. Eight rounds of green tests could
//     not have caught this, and the first case proves that by showing a 40 ms
//     offset barely moving the wall RMS on a straight walk.
//
//  2. **The same offset is GLARING in a turn.** Rotating at `omega`, the
//     offset costs `omega*dt` of yaw, which is a TANGENTIAL error proportional
//     to range and with a sign that follows the turn direction — so a wall
//     scanned while turning left and again while turning right is painted in
//     two places. The second case turns on the spot and shows the same 40 ms
//     blowing the wall out by more than an order of magnitude.
//
// And the fix has to be able to undo it: the third case injects a known offset
// into the DATA and shows that `PushbroomConfig::pose_time_offset_ns` set to
// that value recovers the clean geometry — a sweep with a minimum where the
// truth is, which is what `tools/engine_cli.cpp --d6-timesweep` does on the
// owner's real capture.
//
// The fourth case is about LATENCY rather than accuracy, and it is the other
// half of "the scanning speed seems a bit slow and delay":
// `PushbroomConfig::batch_points` alone held 4096 points before publishing
// ANY of them, which on the owner's measured D6 rate (1,453 points/s) is 2.8
// seconds. `max_batch_span_ns` bounds it in point time, and the case pins both
// that it does and that it does NOT break the determinism the whole assembler
// rests on.

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
constexpr std::int64_t kPoseNs = 33'333'333LL;  // ARCore ~30 Hz, as measured
constexpr std::int64_t kRevNs = 100'000'000LL;  // D6 10 Hz
constexpr int kReturnsPerRev = 400;             // 4 kHz / 10 Hz
constexpr std::int64_t kT0 = 1'000'000'000LL;

// The room: one flat wall at x = +2 m, the only thing being measured.
constexpr double kWallX = 2.0;
constexpr double kRigHeight = 1.4;

// The CAD nominal, verbatim from BracketNominals.cadNominal(COIN_D6).
void cad_nominal(double m[16]) {
  se3::mat4_identity(m);
  m[3] = 0.0;
  m[7] = -0.060;
  m[11] = -0.035;
}

// The rig's TRUE state at time `t` seconds. `yaw` is about world +Y (up).
struct RigState {
  double pos[3];
  double yaw_rad;
};

Pose pose_of(const RigState& r, std::int64_t t_ns) {
  Pose p{};
  p.t_mono_ns = t_ns;
  for (int i = 0; i < 3; ++i) p.position[i] = r.pos[i];
  // Rotation about +Y by yaw: q = (0, sin(yaw/2), 0, cos(yaw/2)).
  p.orientation[0] = 0.0;
  p.orientation[1] = std::sin(r.yaw_rad * 0.5);
  p.orientation[2] = 0.0;
  p.orientation[3] = std::cos(r.yaw_rad * 0.5);
  p.source = StreamId::kPoseAr;
  p.quality = PoseQuality::kGood;
  p.tracking_lost = 0;
  return p;
}

// Ray-cast one fan return against the single wall, in the rig's frame.
// Returns < 0 for "no return".
double range_to_wall(const double origin[3], const double world_from_lidar[16], double angle_deg) {
  double dir_lidar[3];
  d6::fan_point(angle_deg, 1.0, dir_lidar);
  // Rotate the unit ray into world (rotation part only).
  double dir[3];
  for (int r = 0; r < 3; ++r) {
    dir[r] = world_from_lidar[r * 4 + 0] * dir_lidar[0] + world_from_lidar[r * 4 + 1] * dir_lidar[1] +
             world_from_lidar[r * 4 + 2] * dir_lidar[2];
  }
  if (std::fabs(dir[0]) < 1e-9) return -1.0;
  const double d = (kWallX - origin[0]) / dir[0];
  if (d <= 0.05 || d > 12.0) return -1.0;
  const double y = origin[1] + d * dir[1];
  if (y < 0.2 || y > 2.4) return -1.0;  // the wall is a finite panel
  const double z = origin[2] + d * dir[2];
  if (std::fabs(z) > 6.0) return -1.0;
  return d;
}

// A motion profile: where the rig truly is at time `t` seconds.
using Motion = RigState (*)(double t);

// STRAIGHT: walking along -Z at 1 m/s, never turning. This is the motion every
// pre-ROUND-10 fixture used.
RigState straight_walk(double t) {
  RigState r{};
  r.pos[0] = 0.0;
  r.pos[1] = kRigHeight;
  r.pos[2] = -1.0 * t;
  r.yaw_rad = 0.0;
  return r;
}

// TURNING: standing on the spot and rotating at 60 deg/s, which is a normal
// look-around, well inside what the owner does at "normal walking speed".
RigState turn_in_place(double t) {
  RigState r{};
  r.pos[0] = 0.0;
  r.pos[1] = kRigHeight;
  r.pos[2] = 0.0;
  r.yaw_rad = (60.0 * kPi / 180.0) * t;
  return r;
}

// Resolve a walk. `stamp_skew_ns` is added to every LIDAR stamp and to nothing
// else — i.e. it is the bug being simulated, a lidar clock that runs late
// against the pose clock. `correction_ns` is what the assembler is told to do
// about it. With `correction_ns == -stamp_skew_ns` the two cancel exactly.
std::vector<PointVertex> resolve_walk(Motion motion, std::int64_t stamp_skew_ns,
                                      std::int64_t correction_ns, double seconds,
                                      PushbroomStats* out_stats = nullptr) {
  const std::int64_t end_ns = kT0 + static_cast<std::int64_t>(seconds * 1e9);

  double mount[16];
  cad_nominal(mount);

  ExternalPoseSource poses;
  for (std::int64_t t = kT0 - 4 * kPoseNs; t <= end_ns + 4 * kPoseNs; t += kPoseNs) {
    REQUIRE(poses.push_pose(pose_of(motion(static_cast<double>(t - kT0) * 1e-9), t)).ok());
  }

  PageStore store;
  PushbroomConfig cfg;
  cfg.pose_time_offset_ns = correction_ns;
  D6PushbroomAssembler a(&store, cfg);
  REQUIRE(a.set_mount_extrinsics(mount).ok());
  a.set_pose_source(&poses);

  std::vector<ProfilePoint> profile;
  for (std::int64_t t_rev = kT0; t_rev < end_ns; t_rev += kRevNs) {
    profile.clear();
    for (int i = 0; i < kReturnsPerRev; ++i) {
      const std::int64_t t_ns = t_rev + kRevNs * i / kReturnsPerRev;
      // The TRUTH: where the rig really was when this return was taken.
      const RigState truth = motion(static_cast<double>(t_ns - kT0) * 1e-9);
      double world_from_phone[16];
      const Pose p = pose_of(truth, t_ns);
      se3::mat4_from_quat_pos(p.orientation, p.position, world_from_phone);
      double world_from_lidar[16];
      se3::mat4_mul(world_from_phone, mount, world_from_lidar);
      const double origin[3] = {world_from_lidar[3], world_from_lidar[7], world_from_lidar[11]};

      const double angle = 360.0 * i / kReturnsPerRev;
      const double d = range_to_wall(origin, world_from_lidar, angle);
      if (d < 0.0) continue;

      ProfilePoint pp{};
      // ...and the BUG: the return is dated `stamp_skew_ns` late.
      pp.t_mono_ns = t_ns + stamp_skew_ns;
      pp.angle_deg = static_cast<float>(angle);
      pp.range_m = static_cast<float>(d);
      pp.intensity = 120;
      profile.push_back(pp);
    }
    if (!profile.empty()) {
      REQUIRE(a.push_profile(Span<const ProfilePoint>(profile.data(), profile.size())).ok());
    }
  }
  REQUIRE(a.flush().ok());
  if (out_stats != nullptr) *out_stats = a.stats();

  std::vector<PointVertex> out;
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) out.push_back(v.data[k]);
  }
  return out;
}

// RMS distance of the resolved points from the wall they all came off, in cm.
// The wall's plane is KNOWN (x = kWallX), so this is an absolute error against
// truth, not a best fit that could absorb the very error being measured.
double wall_rms_cm(const std::vector<PointVertex>& pts) {
  if (pts.empty()) return 1e9;
  double sum = 0.0;
  for (const PointVertex& p : pts) {
    const double e = static_cast<double>(p.x) - kWallX;
    sum += e * e;
  }
  return 100.0 * std::sqrt(sum / static_cast<double>(pts.size()));
}

}  // namespace

TEST_CASE("round10/a_constant_time_offset_is_invisible_walking_straight") {
  // 40 ms of skew — ten times what the owner's capture actually shows, and
  // deliberately large so that "invisible" cannot be read as "too small to
  // see".
  constexpr std::int64_t kSkew = 40'000'000LL;

  const std::vector<PointVertex> clean = resolve_walk(straight_walk, 0, 0, 4.0);
  const std::vector<PointVertex> skewed = resolve_walk(straight_walk, kSkew, 0, 4.0);

  REQUIRE(clean.size() > 2000);
  CHECK(skewed.size() == clean.size());

  const double rms_clean = wall_rms_cm(clean);
  const double rms_skewed = wall_rms_cm(skewed);
  MESSAGE("straight walk: wall RMS " << rms_clean << " cm clean, " << rms_skewed
                                     << " cm with 40 ms of skew");

  // The wall is PERPENDICULAR to the walk, so sliding the whole cloud 4 cm
  // along -Z does not move a single point off the plane. This is the blind
  // spot, stated as an assertion: a 40 ms error changes the measured wall by
  // less than a tenth of a millimetre.
  CHECK(rms_clean < 0.05);
  CHECK(rms_skewed < 0.05);
  CHECK(std::fabs(rms_skewed - rms_clean) < 0.01);
}

TEST_CASE("round10/the_same_offset_is_glaring_in_a_turn") {
  constexpr std::int64_t kSkew = 40'000'000LL;

  const std::vector<PointVertex> clean = resolve_walk(turn_in_place, 0, 0, 3.0);
  const std::vector<PointVertex> skewed = resolve_walk(turn_in_place, kSkew, 0, 3.0);

  REQUIRE(clean.size() > 1000);
  const double rms_clean = wall_rms_cm(clean);
  const double rms_skewed = wall_rms_cm(skewed);
  MESSAGE("60 deg/s turn: wall RMS " << rms_clean << " cm clean, " << rms_skewed
                                     << " cm with 40 ms of skew");

  // 40 ms at 60 deg/s is 2.4 degrees of yaw, which at ~2-4 m of range is
  // several centimetres of TANGENTIAL error — and unlike the straight case it
  // lands square on the wall's normal for most of the sweep.
  CHECK(rms_clean < 0.05);
  CHECK(rms_skewed > 10.0 * rms_clean);
  CHECK(rms_skewed > 1.0);  // centimetres, on a wall that is otherwise sub-mm
}

TEST_CASE("round10/pose_time_offset_recovers_a_known_skew") {
  constexpr std::int64_t kSkew = 40'000'000LL;

  // The correction has the OPPOSITE sign to the skew, by construction: the
  // return is stamped `kSkew` LATE, so its pose must be looked up `kSkew`
  // EARLIER than the stamp claims.
  PushbroomStats st{};
  const std::vector<PointVertex> corrected = resolve_walk(turn_in_place, kSkew, -kSkew, 3.0, &st);
  const std::vector<PointVertex> clean = resolve_walk(turn_in_place, 0, 0, 3.0);

  MESSAGE("corrected: wall RMS " << wall_rms_cm(corrected) << " cm against " << wall_rms_cm(clean)
                                 << " cm clean, " << st.points_out << " points out");
  CHECK(corrected.size() == clean.size());
  CHECK(wall_rms_cm(corrected) == doctest::Approx(wall_rms_cm(clean)).epsilon(0.02));

  // A SWEEP, which is what the field tool does: the minimum has to be AT the
  // truth and not merely near it. Anything else and "resolve at a sweep of
  // offsets and take the crispest" is not a measurement.
  double best_rms = 1e9;
  std::int64_t best_ns = 0;
  for (std::int64_t c = -60'000'000LL; c <= 0; c += 10'000'000LL) {
    const double rms = wall_rms_cm(resolve_walk(turn_in_place, kSkew, c, 3.0));
    if (rms < best_rms) {
      best_rms = rms;
      best_ns = c;
    }
  }
  MESSAGE("sweep minimum at " << best_ns / 1'000'000 << " ms (truth " << -kSkew / 1'000'000
                              << " ms), RMS " << best_rms << " cm");
  CHECK(best_ns == -kSkew);
}

TEST_CASE("round10/the_batch_is_bounded_in_point_time_not_only_in_count") {
  // A D6 at the owner's MEASURED rate: 293,524 in-range returns over 202.1 s
  // is 1,453 points/s. At `batch_points = 4096` that is 2.8 s of points held
  // in a vector before the PageStore — and therefore the renderer, and
  // therefore the operator — sees any of them.
  ExternalPoseSource poses;
  const std::int64_t end_ns = kT0 + 3'000'000'000LL;
  for (std::int64_t t = kT0 - 4 * kPoseNs; t <= end_ns + 4 * kPoseNs; t += kPoseNs) {
    REQUIRE(poses.push_pose(pose_of(straight_walk(static_cast<double>(t - kT0) * 1e-9), t)).ok());
  }
  double mount[16];
  cad_nominal(mount);

  // 1,450 points/s, pushed one at a time exactly as the driver does.
  constexpr std::int64_t kPointNs = 690'000LL;  // ~1,450 Hz

  auto run = [&](std::int64_t span_ns) {
    PageStore store;
    PushbroomConfig cfg;
    cfg.max_batch_span_ns = span_ns;
    D6PushbroomAssembler a(&store, cfg);
    REQUIRE(a.set_mount_extrinsics(mount).ok());
    a.set_pose_source(&poses);

    struct Result {
      std::uint64_t points = 0;
      std::int64_t first_visible_ns = -1;  // point time at which the store first held anything
    } r;
    for (std::int64_t t = kT0; t < end_ns; t += kPointNs) {
      ProfilePoint p{};
      p.t_mono_ns = t;
      p.angle_deg = 90.0f;  // straight out the +? side; any fixed ray will do
      p.range_m = 2.0f;
      p.intensity = 100;
      // One point per call, which is exactly how Engine::Impl::on_d6_profile
      // feeds it (`push_profile(Span<const ProfilePoint>(&p, 1))`) — and it
      // has to be push_profile and not push_point, because only the former
      // drains. That asymmetry is the API's, not this test's.
      REQUIRE(a.push_profile(Span<const ProfilePoint>(&p, 1)).ok());
      if (r.first_visible_ns < 0 && store.total_points() > 0) r.first_visible_ns = t - kT0;
    }
    REQUIRE(a.flush().ok());
    r.points = store.total_points();
    return r;
  };

  const auto bounded = run(100'000'000LL);   // the shipped default: 100 ms
  const auto unbounded = run(0);             // count-only, i.e. pre-ROUND-10

  MESSAGE("first point visible after " << bounded.first_visible_ns / 1'000'000
                                       << " ms (bounded) vs " << unbounded.first_visible_ns / 1'000'000
                                       << " ms (count-only)");

  // The whole complaint, as a number: count-only holds the first point for
  // seconds; the time bound publishes within one D6 revolution.
  CHECK(bounded.first_visible_ns <= 110'000'000LL);
  CHECK(unbounded.first_visible_ns > 2'500'000'000LL);

  // ...and the falsifiable half: bounding the batch must not change WHAT is
  // produced, only when. Same points, same count, whichever way it is
  // batched — which is the property `assembles_identically_live_and_offline`
  // depends on and the reason the bound is in POINT time and not wall time.
  CHECK(bounded.points == unbounded.points);
  CHECK(bounded.points > 4000u);
}

TEST_CASE("round10/the_time_bound_leaves_the_geometry_bit_identical") {
  // The same walk, resolved with and without the time bound. Every point must
  // land on exactly the same float — not "close", identical — or "replay ==
  // capture" (Tech Spec §3 key rule 2) has quietly stopped being true.
  double mount[16];
  cad_nominal(mount);

  auto resolve = [&](std::int64_t span_ns) {
    ExternalPoseSource poses;
    const std::int64_t end_ns = kT0 + 4'000'000'000LL;
    for (std::int64_t t = kT0 - 4 * kPoseNs; t <= end_ns + 4 * kPoseNs; t += kPoseNs) {
      REQUIRE(poses.push_pose(pose_of(straight_walk(static_cast<double>(t - kT0) * 1e-9), t)).ok());
    }
    PageStore store;
    PushbroomConfig cfg;
    cfg.max_batch_span_ns = span_ns;
    D6PushbroomAssembler a(&store, cfg);
    REQUIRE(a.set_mount_extrinsics(mount).ok());
    a.set_pose_source(&poses);

    std::vector<ProfilePoint> profile;
    for (std::int64_t t_rev = kT0; t_rev < end_ns; t_rev += kRevNs) {
      profile.clear();
      for (int i = 0; i < kReturnsPerRev; ++i) {
        const std::int64_t t_ns = t_rev + kRevNs * i / kReturnsPerRev;
        const RigState truth = straight_walk(static_cast<double>(t_ns - kT0) * 1e-9);
        double world_from_phone[16], world_from_lidar[16];
        const Pose p = pose_of(truth, t_ns);
        se3::mat4_from_quat_pos(p.orientation, p.position, world_from_phone);
        se3::mat4_mul(world_from_phone, mount, world_from_lidar);
        const double origin[3] = {world_from_lidar[3], world_from_lidar[7], world_from_lidar[11]};
        const double angle = 360.0 * i / kReturnsPerRev;
        const double d = range_to_wall(origin, world_from_lidar, angle);
        if (d < 0.0) continue;
        ProfilePoint pp{};
        pp.t_mono_ns = t_ns;
        pp.angle_deg = static_cast<float>(angle);
        pp.range_m = static_cast<float>(d);
        pp.intensity = 120;
        profile.push_back(pp);
      }
      if (!profile.empty()) {
        REQUIRE(a.push_profile(Span<const ProfilePoint>(profile.data(), profile.size())).ok());
      }
    }
    REQUIRE(a.flush().ok());
    std::vector<PointVertex> out;
    for (const PageId id : store.page_ids()) {
      const PageView v = store.page_view(id);
      if (!v.valid()) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) out.push_back(v.data[k]);
    }
    return out;
  };

  const std::vector<PointVertex> bounded = resolve(100'000'000LL);
  const std::vector<PointVertex> counted = resolve(0);
  REQUIRE(bounded.size() > 2000);
  REQUIRE(bounded.size() == counted.size());
  std::size_t identical = 0;
  for (std::size_t i = 0; i < bounded.size(); ++i) {
    if (bounded[i].x == counted[i].x && bounded[i].y == counted[i].y &&
        bounded[i].z == counted[i].z) {
      ++identical;
    }
  }
  MESSAGE(identical << " of " << bounded.size() << " points bit-identical across batching");
  CHECK(identical == bounded.size());
}
