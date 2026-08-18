// ROUND 11 item 45c — what a mount-trim error costs, WHICH PART of it reverses
// when the operator turns around, and which part does not.
//
// > "current scan right when i go forward but when i turn around the scan
// >  position shifted"   — owner, on 0.6.0
//
// ROUND 10 falsified the clock-offset explanation with numbers and left mount
// trim as suspect #1, with an arithmetic estimate ("2.4 deg is ~12 cm at 3 m")
// and no measurement. This file is the measurement, on the motion that
// produces the symptom: an OUT-AND-BACK walk with the same feature painted
// once on the way out and once on the way back.
//
// --- THE MECHANISM, AND THE PART THE ARITHMETIC GUESS GOT WRONG -------------
//
// A mount trim error is a small rotation `d` of the LIDAR inside the PHONE
// frame. It never changes a range, so it displaces every return PERPENDICULAR
// to its own ray by `r*sin(d)`, in a direction fixed in the phone. Turning
// around does not change the trim — it changes which way the phone's axes
// point in the world — so the world-frame displacement reverses, and the same
// feature is painted at `+r*sin(d)` outbound and `-r*sin(d)` on the return:
//
//     split = 2 * r * sin(d)
//
// It is rate-independent (a clock offset is not), proportional to range, and
// completely invisible on a one-way walk. That is the shape of the symptom.
//
// BUT THE AXIS DECIDES WHETHER IT REVERSES AT ALL, and this is the part worth
// having measured rather than reasoned:
//
//   * about the phone's UP axis (camera +Y, which for a vertically-held phone
//     IS world up) — the fan yaws. This does NOT reverse on a turn-around: the
//     rotation axis is world-vertical whichever way the operator faces, so
//     both passes are displaced the same way and the feature stays single.
//     Measured below: 5.9 cm of displacement at 2.4 deg, and a 0.19 cm split.
//   * about the phone's RIGHT axis (camera +X) — the fan plane tilts
//     forward/backward. That axis points world +X on the way out and world -X
//     on the way back, so this component reverses in full, and it displaces
//     OVERHEAD and UNDERFOOT returns ALONG the walk. This is the one that
//     doubles a feature.
//   * about the phone's FORWARD axis (camera -Z) — a rotation inside the fan's
//     own plane, so on a 360 deg fan it changes nothing at all.
//
// The fixture therefore measures a CEILING BEAM (a return going up, at 1.66 m
// of range) under a camera-X trim error, and keeps the side POST under a
// camera-Y error as the control that shows the non-reversing case.
//
// --- WHY THIS NEEDED A NEW FIXTURE ------------------------------------------
//
// The displacement is perpendicular to the ray, so for a wall the D6 looks at
// square-on the points slide ALONG the wall and the wall does not thicken by
// one millimetre. Every geometry metric in this repository before ROUND 11 was
// a wall-flatness metric — plane-fit RMS, band thickness, wall probes — and
// not one of them can see this. "The scan position shifted" is the right
// description of the symptom and "the walls got thick" is the wrong one.

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
constexpr std::int64_t kPoseNs = 33'333'333LL;  // ARCore ~30 Hz
constexpr std::int64_t kRevNs = 100'000'000LL;  // D6 10 Hz
constexpr int kReturnsPerRev = 400;             // 4 kHz / 10 Hz
constexpr std::int64_t kT0 = 1'000'000'000LL;

// The room, in ARCore's +Y-up world. 5 m wide, 7 m long, 3.2 m tall — the
// owner describes a ~3.1 m room (ROUND 9 item 34e).
constexpr double kRoomXMin = -2.5, kRoomXMax = 2.5;
constexpr double kRoomYMin = 0.0, kRoomYMax = 3.2;
constexpr double kRoomZMin = -6.0, kRoomZMax = 1.0;

// The post: a square column 1.5 m to the operator's right on the way out.
constexpr double kPostX = 1.5, kPostZ = -3.0, kPostHalf = 0.10;

// The beam: a joist spanning the ceiling at z = -3, its underside at y = 3.0.
// Nothing else in the room lives at that height, so a return can be attributed
// to it by height alone.
constexpr double kBeamZ = -3.0, kBeamHalf = 0.15, kBeamBottomY = 3.0;

constexpr double kRigHeight = 1.4;
constexpr double kWalkSpeed = 0.6;    // m/s
constexpr double kLegSeconds = 10.0;  // one way
constexpr double kTurnSeconds = 2.0;  // the 180 on the spot

// The CAD nominal, verbatim from BracketNominals.cadNominal(COIN_D6).
void cad_nominal(double m[16]) {
  se3::mat4_identity(m);
  m[3] = 0.0;
  m[7] = -0.060;
  m[11] = -0.035;
}

// The sensor's height above the floor once the bracket offset is applied, and
// therefore the range of a straight-up return to the beam.
const double kSensorY = kRigHeight - 0.060;
const double kBeamRange = kBeamBottomY - kSensorY;  // 1.66 m

struct RigState {
  double pos[3];
  double yaw_rad;
};

// Out, turn 180 on the spot, back. Yaw 0 => forward is -Z (ARCore's forward),
// yaw pi => forward is +Z.
RigState out_and_back(double t) {
  RigState r{};
  r.pos[0] = 0.0;
  r.pos[1] = kRigHeight;
  const double z0 = 0.5;
  if (t <= kLegSeconds) {
    r.pos[2] = z0 - kWalkSpeed * t;
    r.yaw_rad = 0.0;
  } else if (t <= kLegSeconds + kTurnSeconds) {
    r.pos[2] = z0 - kWalkSpeed * kLegSeconds;
    r.yaw_rad = kPi * (t - kLegSeconds) / kTurnSeconds;
  } else {
    const double u = t - kLegSeconds - kTurnSeconds;
    r.pos[2] = z0 - kWalkSpeed * kLegSeconds + kWalkSpeed * u;
    r.yaw_rad = kPi;
  }
  return r;
}

Pose pose_of(const RigState& r, std::int64_t t_ns) {
  Pose p{};
  p.t_mono_ns = t_ns;
  for (int i = 0; i < 3; ++i) p.position[i] = r.pos[i];
  p.orientation[0] = 0.0;
  p.orientation[1] = std::sin(r.yaw_rad * 0.5);
  p.orientation[2] = 0.0;
  p.orientation[3] = std::cos(r.yaw_rad * 0.5);
  p.source = StreamId::kPoseAr;
  p.quality = PoseQuality::kGood;
  p.tracking_lost = 0;
  return p;
}

// Nearest positive hit of the ray on an axis-aligned box, entering from
// outside. Returns < 0 for a miss.
double hit_box(const double o[3], const double d[3], const double lo[3], const double hi[3]) {
  double tmin = 0.0, tmax = 1e30;
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(d[k]) < 1e-12) {
      if (o[k] < lo[k] || o[k] > hi[k]) return -1.0;
      continue;
    }
    double t1 = (lo[k] - o[k]) / d[k];
    double t2 = (hi[k] - o[k]) / d[k];
    if (t1 > t2) std::swap(t1, t2);
    if (t1 > tmin) tmin = t1;
    if (t2 < tmax) tmax = t2;
    if (tmin > tmax) return -1.0;
  }
  return tmin > 0.05 ? tmin : -1.0;
}

// Room interior + the two obstacles, nearest wins. Returns < 0 for no return.
double cast(const double o[3], const double d[3]) {
  double best = 1e30;

  const double post_lo[3] = {kPostX - kPostHalf, kRoomYMin, kPostZ - kPostHalf};
  const double post_hi[3] = {kPostX + kPostHalf, kRoomYMax, kPostZ + kPostHalf};
  const double t_post = hit_box(o, d, post_lo, post_hi);
  if (t_post > 0.0 && t_post < best) best = t_post;

  const double beam_lo[3] = {kRoomXMin, kBeamBottomY, kBeamZ - kBeamHalf};
  const double beam_hi[3] = {kRoomXMax, kRoomYMax, kBeamZ + kBeamHalf};
  const double t_beam = hit_box(o, d, beam_lo, beam_hi);
  if (t_beam > 0.0 && t_beam < best) best = t_beam;

  const double lo[3] = {kRoomXMin, kRoomYMin, kRoomZMin};
  const double hi[3] = {kRoomXMax, kRoomYMax, kRoomZMax};
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(d[k]) < 1e-12) continue;
    for (int side = 0; side < 2; ++side) {
      const double plane = side == 0 ? lo[k] : hi[k];
      const double t = (plane - o[k]) / d[k];
      if (t <= 0.05 || t >= best) continue;
      bool inside = true;
      for (int j = 0; j < 3 && inside; ++j) {
        if (j == k) continue;
        const double v = o[j] + t * d[j];
        if (v < lo[j] - 1e-9 || v > hi[j] + 1e-9) inside = false;
      }
      if (inside) best = t;
    }
  }
  if (best > 12.0 || best > 1e29) return -1.0;
  return best;
}

struct Painted {
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> t;
};

// Walk the fixture with a TRUE phone_from_lidar of `nominal * Exp(d * axis)`
// while the assembler is told `nominal`. That disagreement IS the mount trim
// error, injected exactly where a real one lives.
//
// `axis` is 0 = camera +X (the phone's right — the component that reverses),
// 1 = camera +Y (up — the component that does not).
Painted walk(double trim_err_deg, int axis) {
  double nominal[16];
  cad_nominal(nominal);

  double err[16];
  {
    const double a = trim_err_deg * kPi / 180.0;
    double w[3] = {0.0, 0.0, 0.0};
    w[axis] = a;
    double R[9];
    se3::so3_exp(w, R);
    const double zero[3] = {0.0, 0.0, 0.0};
    se3::mat4_from_rt(R, zero, err);
  }
  double truth_mount[16];
  se3::mat4_mul(nominal, err, truth_mount);

  const double total_s = 2.0 * kLegSeconds + kTurnSeconds;
  const std::int64_t end_ns = kT0 + static_cast<std::int64_t>(total_s * 1e9);

  ExternalPoseSource poses;
  for (std::int64_t t = kT0 - 4 * kPoseNs; t <= end_ns + 4 * kPoseNs; t += kPoseNs) {
    REQUIRE(poses.push_pose(pose_of(out_and_back(static_cast<double>(t - kT0) * 1e-9), t)).ok());
  }

  Painted out;
  PageStore store;
  PushbroomConfig cfg;
  cfg.out_point_times = &out.t;
  D6PushbroomAssembler a(&store, cfg);
  REQUIRE(a.set_mount_extrinsics(nominal).ok());
  a.set_pose_source(&poses);

  std::vector<ProfilePoint> profile;
  for (std::int64_t t_rev = kT0; t_rev < end_ns; t_rev += kRevNs) {
    profile.clear();
    for (int i = 0; i < kReturnsPerRev; ++i) {
      const std::int64_t t_ns = t_rev + kRevNs * i / kReturnsPerRev;
      const RigState truth = out_and_back(static_cast<double>(t_ns - kT0) * 1e-9);
      double world_from_phone[16];
      const Pose p = pose_of(truth, t_ns);
      se3::mat4_from_quat_pos(p.orientation, p.position, world_from_phone);
      double world_from_lidar[16];
      se3::mat4_mul(world_from_phone, truth_mount, world_from_lidar);
      const double origin[3] = {world_from_lidar[3], world_from_lidar[7], world_from_lidar[11]};

      const double angle = 360.0 * i / kReturnsPerRev;
      double dir_lidar[3];
      d6::fan_point(angle, 1.0, dir_lidar);
      double dir[3];
      se3::mat4_rotate(world_from_lidar, dir_lidar, dir);
      const double d = cast(origin, dir);
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

  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) out.pts.push_back(v.data[k]);
  }
  REQUIRE(out.pts.size() == out.t.size());
  return out;
}

// The two legs, with the TURN ITSELF excluded from both. During the 180 the
// rig stands still and sweeps through every yaw, so its returns belong to
// neither leg; counting them into one would put a yaw-smeared population into
// one centroid and not the other, which is a fixture artefact and not a trim
// error. (It was worth 1 cm of noise before this line existed.)
const std::int64_t kOutEndNs = kT0 + static_cast<std::int64_t>(kLegSeconds * 1e9);
const std::int64_t kRetStartNs =
    kT0 + static_cast<std::int64_t>((kLegSeconds + kTurnSeconds) * 1e9);

struct Split {
  double outbound_z = 0.0;
  double return_z = 0.0;
  double split_m = 0.0;
  std::size_t n_out = 0;
  std::size_t n_ret = 0;
};

// Where the BEAM's underside was painted, per leg.
//
// Selected by HEIGHT plus a side-wall exclusion, never by proximity to where
// the beam is expected: a selection that chased the feature would report the
// answer it was given. The side walls (x = +/-2.5) also carry returns at
// y = 3.0, which is the one other thing in the room at that height, so |x|
// is bounded well inside them; the end walls are 3 m and 6 m away in z and
// the +/-0.6 m window cannot reach them.
bool beam_split(const Painted& w, Split* out) {
  double so = 0.0, sr = 0.0;
  std::size_t no = 0, nr = 0;
  for (std::size_t i = 0; i < w.pts.size(); ++i) {
    const PointVertex& p = w.pts[i];
    if (std::fabs(static_cast<double>(p.y) - kBeamBottomY) > 0.06) continue;
    if (std::fabs(static_cast<double>(p.x)) > 2.3) continue;
    if (std::fabs(static_cast<double>(p.z) - kBeamZ) > 0.6) continue;
    if (w.t[i] < kOutEndNs) { so += p.z; ++no; }
    else if (w.t[i] > kRetStartNs) { sr += p.z; ++nr; }
  }
  if (no < 20 || nr < 20) return false;
  out->outbound_z = so / static_cast<double>(no);
  out->return_z = sr / static_cast<double>(nr);
  out->split_m = std::fabs(out->outbound_z - out->return_z);
  out->n_out = no;
  out->n_ret = nr;
  return true;
}

// The same thing for the side POST's front face.
bool post_split(const Painted& w, Split* out) {
  double so = 0.0, sr = 0.0;
  std::size_t no = 0, nr = 0;
  for (std::size_t i = 0; i < w.pts.size(); ++i) {
    const PointVertex& p = w.pts[i];
    if (std::fabs(static_cast<double>(p.x) - (kPostX - kPostHalf)) > 0.06) continue;
    if (std::fabs(static_cast<double>(p.z) - kPostZ) > 0.9) continue;
    if (p.y < 0.6 || p.y > 2.0) continue;
    if (w.t[i] < kOutEndNs) { so += p.z; ++no; }
    else if (w.t[i] > kRetStartNs) { sr += p.z; ++nr; }
  }
  if (no < 20 || nr < 20) return false;
  out->outbound_z = so / static_cast<double>(no);
  out->return_z = sr / static_cast<double>(nr);
  out->split_m = std::fabs(out->outbound_z - out->return_z);
  out->n_out = no;
  out->n_ret = nr;
  return true;
}

}  // namespace

TEST_CASE("round11/trim/a perfect mount paints the ceiling beam in one place") {
  const Painted w = walk(0.0, 0);
  Split s;
  REQUIRE(beam_split(w, &s));
  MESSAGE("trim 0.0 deg: the beam is painted at z = " << s.outbound_z << " m outbound and z = "
                                                      << s.return_z << " m on the return -> split "
                                                      << 100.0 * s.split_m << " cm (" << s.n_out
                                                      << " / " << s.n_ret << " returns)");
  // The control, and it also establishes the fixture's RESOLUTION. A D6 puts
  // 400 returns into one 10 Hz revolution, so the beam's 0.30 m underside seen
  // from 1.66 m below is sampled by about eleven fan angles: the centroid can
  // only move in ~1 cm steps, and the outbound and return legs cross the beam
  // at different points of that lattice. Every number below is therefore good
  // to about a centimetre and no better, which is stated rather than hidden.
  CHECK(s.split_m < 0.015);
  CHECK(std::fabs(s.outbound_z - kBeamZ) < 0.02);
}

TEST_CASE("round11/trim/THE NUMBER — a trim error about the phone's right axis "
          "doubles an overhead feature, and the split is 2*r*sin(d)") {
  struct Row {
    double deg;
    const char* what;
  };
  const Row rows[] = {
      {2.4, "scan-020's accepted trim — the gate's own ceiling (MAX_SPREAD_P90_DEG = 2.5)"},
      {1.4, "a mid-refinement reading, the sort the new ring shows on its way down"},
      {0.8, "ROUND 11 item 45c's refinement target"},
      {0.5, "a well-averaged trim"},
  };

  double split_24 = 0.0, split_08 = 0.0, split_05 = 0.0;
  for (const Row& row : rows) {
    const Painted w = walk(row.deg, /*axis=*/0);
    Split s;
    REQUIRE(beam_split(w, &s));
    const double predicted = 2.0 * kBeamRange * std::sin(row.deg * kPi / 180.0);
    MESSAGE("trim " << row.deg << " deg (" << row.what << "): beam painted at z = " << s.outbound_z
                    << " m outbound, z = " << s.return_z << " m on the return -> SPLIT "
                    << 100.0 * s.split_m << " cm   [2*r*sin(d) at r = " << kBeamRange
                    << " m predicts " << 100.0 * predicted << " cm]");
    // The mechanism, not only the magnitude: matching the closed form is what
    // makes the number usable at any range without re-running anything. A
    // return at 3 m is 3/1.66 = 1.8x these numbers. The tolerance is the
    // fixture's ~1 cm sampling lattice (see the control case), doubled.
    CHECK(std::fabs(s.split_m - predicted) < 0.02);
    // ... and the two paintings land on OPPOSITE sides of the truth, which is
    // the "it shifts when I turn around" claim itself rather than its size.
    // Asserted only where the signal clears the lattice: at 0.5 deg the whole
    // effect is 1.4 cm per side and the fixture cannot resolve the sign.
    if (row.deg >= 1.4) CHECK(((s.outbound_z - kBeamZ) * (s.return_z - kBeamZ)) < 0.0);
    if (row.deg == 2.4) split_24 = s.split_m;
    if (row.deg == 0.8) split_08 = s.split_m;
    if (row.deg == 0.5) split_05 = s.split_m;
  }

  MESSAGE("HEADLINE (item 45c): at 1.66 m of range, tightening the trim from 2.4 deg to 0.8 deg "
          << "shrinks the turn-around split from " << 100.0 * split_24 << " cm to "
          << 100.0 * split_08 << " cm; at 0.5 deg it is " << 100.0 * split_05
          << " cm. Scale by range: multiply by r/1.66.");
  // Linear in the angle to within the sampling, which is the claim that lets
  // "keep averaging until the spread comes down" be a proportionate fix.
  CHECK(split_24 > 3.0 * split_08);
  CHECK(split_24 > 0.10);  // > 10 cm at 1.66 m: plainly visible
  CHECK(split_05 < 0.02);  // at the fixture's own noise floor: invisible
}

TEST_CASE("round11/trim/the same error about the phone's UP axis does NOT reverse") {
  // The finding that corrects ROUND 10's arithmetic guess. A trim error about
  // camera +Y is a rotation about WORLD up for a vertically-held phone, and a
  // rotation about world up is the same rotation whichever way the operator
  // faces. So it displaces the feature — by the full r*sin(d) — and displaces
  // it the SAME WAY on both passes. The map is wrong and it is wrong
  // consistently, which is exactly the kind of error nobody reports.
  const Painted w = walk(2.4, /*axis=*/1);
  Split s;
  REQUIRE(post_split(w, &s));
  const double r = kPostX - kPostHalf;
  const double displacement = r * std::sin(2.4 * kPi / 180.0);
  MESSAGE("trim 2.4 deg about camera +Y: the post is painted at z = "
          << s.outbound_z << " m outbound and z = " << s.return_z
          << " m on the return -> split " << 100.0 * s.split_m << " cm, while BOTH are "
          << 100.0 * std::fabs(0.5 * (s.outbound_z + s.return_z) - kPostZ)
          << " cm from the truth (r*sin(d) at r = " << r << " m predicts "
          << 100.0 * displacement << " cm)");
  CHECK(s.split_m < 0.015);
  CHECK(std::fabs(0.5 * (s.outbound_z + s.return_z) - kPostZ) ==
        doctest::Approx(displacement).epsilon(0.25));
}

TEST_CASE("round11/trim/a one-way walk hides the reversing error completely") {
  // The other half of the claim, and the reason eight rounds of fixtures never
  // saw this: on a one-way walk the SAME trim error paints the beam in exactly
  // one place, displaced but self-consistent. Nothing looks wrong, no wall
  // gets thicker, and no metric this repository had could tell.
  auto outbound_only = [](const Painted& w, double* out_z, double* out_spread) {
    double sum = 0.0, lo = 1e30, hi = -1e30;
    std::size_t n = 0;
    for (std::size_t i = 0; i < w.pts.size(); ++i) {
      if (w.t[i] >= kOutEndNs) continue;
      const PointVertex& p = w.pts[i];
      if (std::fabs(static_cast<double>(p.y) - kBeamBottomY) > 0.06) continue;
      if (std::fabs(static_cast<double>(p.x)) > 2.3) continue;
      if (std::fabs(static_cast<double>(p.z) - kBeamZ) > 0.6) continue;
      sum += p.z;
      lo = std::min(lo, static_cast<double>(p.z));
      hi = std::max(hi, static_cast<double>(p.z));
      ++n;
    }
    REQUIRE(n > 20);
    *out_z = sum / static_cast<double>(n);
    *out_spread = hi - lo;
  };

  double clean_z = 0.0, clean_spread = 0.0, bad_z = 0.0, bad_spread = 0.0;
  outbound_only(walk(0.0, 0), &clean_z, &clean_spread);
  outbound_only(walk(2.4, 0), &bad_z, &bad_spread);
  MESSAGE("outbound only: beam at z = " << clean_z << " m (spread " << 100.0 * clean_spread
                                        << " cm) with a perfect mount, z = " << bad_z << " m (spread "
                                        << 100.0 * bad_spread << " cm) with 2.4 deg of trim");
  // The feature is DISPLACED by half the split...
  CHECK(std::fabs(bad_z - clean_z) > 0.03);
  // ...and not SMEARED. One pass sees one beam, in the wrong place, and has no
  // way to know it.
  CHECK(bad_spread < clean_spread + 0.03);
}
