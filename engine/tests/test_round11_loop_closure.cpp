// ROUND 11 item 41 — trajectory loop closure, and the guard that has to be
// stronger than the feature.
//
// The owner's requirement has two halves and the second one is the hard one:
//
//   > "When the trajectory returns near its origin ... detect the revisit,
//   >  compute the closing correction, and distribute it back along the path"
//   > "loop closure must NEVER fire on a non-loop walk (false positive =
//   >  dragging a straight path closed = catastrophic)"
//
// So this file proves the correction against a ground truth it can compute
// exactly, and then spends the rest of its cases trying to make the detector
// fire when it must not.
//
// --- THE GROUND TRUTH -------------------------------------------------------
//
// The fixture walks a 2.5 m-radius circle inside a 8 x 8 x 3 m room with three
// posts in it, and resolves the SAME ranges twice:
//
//   * against the TRUE poses          -> the cloud that should exist;
//   * against DRIFTED poses           -> the cloud a real ARCore session gives.
//
// Both go through the production D6PushbroomAssembler with the same profile
// stream, so they emit the same points in the same order and the error is a
// per-point distance rather than a statistic. The drift injected is a yaw that
// grows as s^1.5 plus a translation with a quadratic term — deliberately NOT
// the SE(3) geodesic the correction is built from, so "it closes" is a claim
// about recovery and not about a fixture that was rigged to be recoverable.
//
// --- WHY THE CORRECTION IS EXACT AT BOTH ENDS -------------------------------
//
// C(s) = Exp(s * Log(T_fix)) with s the ARC-LENGTH fraction between the two
// visits. Exp(0) is identity to the last bit and Exp(1 * Log(T)) is T to the
// last bit, so the first visit is untouched and the second lands exactly on
// the measured transform. Everything in between is a screw interpolation. One
// case below pins exactly that.

#include <algorithm>
#include <string>
#include <cmath>
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/drivers/d6/d6_fan.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/poses/se3.h"
#include "scanengine/slam/post/trajectory_loop.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"

using namespace scanengine;
using namespace scanengine::post;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kPoseNs = 33'333'333LL;
constexpr std::int64_t kRevNs = 100'000'000LL;
constexpr int kReturnsPerRev = 400;
constexpr std::int64_t kT0 = 1'000'000'000LL;

// The room: 8 x 8 m, 3 m tall.
constexpr double kX0 = -4.0, kX1 = 4.0;
constexpr double kY0 = 0.0, kY1 = 3.0;
constexpr double kZ0 = -4.0, kZ1 = 4.0;

// Three posts, deliberately at different places and sizes so the room has no
// rotational symmetry for ICP to lock onto.
struct Box {
  double lo[3];
  double hi[3];
};
const Box kPosts[3] = {
    {{2.30, 0.0, 0.80}, {2.60, 3.0, 1.10}},
    {{-1.20, 0.0, -3.00}, {-0.85, 3.0, -2.65}},
    {{0.30, 0.0, 3.10}, {0.75, 3.0, 3.35}},
};

constexpr double kRadius = 2.5;
constexpr double kSpeed = 0.7;  // m/s
const double kLapSeconds = 2.0 * kPi * kRadius / kSpeed;  // ~22.4 s
constexpr double kRigHeight = 1.5;

void cad_nominal(double m[16]) {
  se3::mat4_identity(m);
  m[3] = 0.0;
  m[7] = -0.060;
  m[11] = -0.035;
}

struct RigState {
  double pos[3];
  double yaw_rad;
};

// One lap of a circle, phone facing the direction of travel.
RigState circle(double t) {
  RigState r{};
  const double phi = 2.0 * kPi * t / kLapSeconds;
  r.pos[0] = kRadius * std::sin(phi);
  r.pos[1] = kRigHeight;
  r.pos[2] = kRadius * std::cos(phi);
  // forward = d/dt position, normalized = (cos phi, 0, -sin phi).
  // pose_of maps yaw to forward = (-sin yaw, 0, -cos yaw), so invert it.
  r.yaw_rad = std::atan2(-std::cos(phi), std::sin(phi));
  return r;
}

// Straight down the room and out — the walk that must NEVER close.
RigState straight(double t) {
  RigState r{};
  r.pos[0] = 0.0;
  r.pos[1] = kRigHeight;
  r.pos[2] = 3.5 - kSpeed * t;
  r.yaw_rad = 0.0;
  return r;
}

// Shuffling in one spot — the operator working a corner over and does not
// leave it. This walks 20 m of PATH and 40 s of time without ever getting
// more than a metre from where it started, so gates 1 and 2 disagree about it
// and gate 2 has to be the one that wins.
RigState shuffle(double t) {
  RigState r{};
  r.pos[0] = 0.25 * std::sin(2.0 * kPi * t / 3.0);
  r.pos[1] = kRigHeight + 0.02 * std::sin(2.0 * t);
  r.pos[2] = 0.40 * std::sin(2.0 * kPi * t / 4.0);
  r.yaw_rad = 0.35 * std::sin(1.5 * t);
  return r;
}

using Motion = RigState (*)(double);

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

double cast(const double o[3], const double d[3]) {
  double best = 1e30;
  for (const Box& b : kPosts) {
    const double t = hit_box(o, d, b.lo, b.hi);
    if (t > 0.0 && t < best) best = t;
  }
  const double lo[3] = {kX0, kY0, kZ0};
  const double hi[3] = {kX1, kY1, kZ1};
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

// ARCore-shaped drift as a function of the arc-length fraction `s`.
//
// Yaw grows as s^1.5 and the translation carries a quadratic term, so the
// drift is NOT the screw geodesic the correction is built from. That is the
// point: if it were, the correction would cancel it identically and the test
// would prove only that Exp and Log are inverses.
void drift_at(double s, double yaw_max_deg, double trans_max_m, double out[16]) {
  const double yaw = (yaw_max_deg * kPi / 180.0) * std::pow(s, 1.5);
  const double w[3] = {0.0, yaw, 0.0};
  double R[9];
  se3::so3_exp(w, R);
  const double t[3] = {trans_max_m * s, 0.04 * trans_max_m * s,
                       -0.6 * trans_max_m * s * s};
  se3::mat4_from_rt(R, t, out);
}

struct Walk {
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  std::vector<TrajPose> traj;
};

// Resolve one walk. Ranges always come from the TRUE geometry; the pose stream
// the assembler resolves against is optionally corrupted by drift_at().
Walk resolve(Motion motion, double seconds, double yaw_max_deg, double trans_max_m) {
  double mount[16];
  cad_nominal(mount);
  const std::int64_t end_ns = kT0 + static_cast<std::int64_t>(seconds * 1e9);

  // Arc length, so the drift can be parameterised the way real VIO drift
  // accumulates (with distance, not with time).
  const int kArcSteps = 4000;
  std::vector<double> arc(kArcSteps + 1, 0.0);
  for (int i = 1; i <= kArcSteps; ++i) {
    const double ta = seconds * (i - 1) / kArcSteps;
    const double tb = seconds * i / kArcSteps;
    const RigState a = motion(ta);
    const RigState b = motion(tb);
    double d = 0.0;
    for (int k = 0; k < 3; ++k) {
      const double e = b.pos[k] - a.pos[k];
      d += e * e;
    }
    arc[i] = arc[i - 1] + std::sqrt(d);
  }
  const double total_arc = arc[kArcSteps] > 1e-9 ? arc[kArcSteps] : 1.0;
  auto frac_at = [&](double t) {
    const double u = t / seconds * kArcSteps;
    int i = static_cast<int>(u);
    if (i < 0) i = 0;
    if (i >= kArcSteps) return 1.0;
    const double f = u - i;
    return (arc[i] + f * (arc[i + 1] - arc[i])) / total_arc;
  };

  Walk w;
  ExternalPoseSource poses;
  for (std::int64_t t = kT0 - 4 * kPoseNs; t <= end_ns + 4 * kPoseNs; t += kPoseNs) {
    const double ts = static_cast<double>(t - kT0) * 1e-9;
    const double tc = std::min(std::max(ts, 0.0), seconds);
    Pose p = pose_of(motion(ts), t);
    if (yaw_max_deg != 0.0 || trans_max_m != 0.0) {
      double D[16];
      drift_at(frac_at(tc), yaw_max_deg, trans_max_m, D);
      double truth[16], drifted[16];
      se3::mat4_from_quat_pos(p.orientation, p.position, truth);
      se3::mat4_mul(D, truth, drifted);
      double R[9], tt[3];
      se3::mat4_get_rt(drifted, R, tt);
      se3::matrix_to_quat(R, p.orientation);
      for (int k = 0; k < 3; ++k) p.position[k] = tt[k];
    }
    REQUIRE(poses.push_pose(p).ok());
    if (t >= kT0 && t <= end_ns) {
      TrajPose tp;
      tp.t_ns = t;
      for (int k = 0; k < 4; ++k) tp.q[k] = p.orientation[k];
      for (int k = 0; k < 3; ++k) tp.p[k] = p.position[k];
      w.traj.push_back(tp);
    }
  }

  PageStore store;
  PushbroomConfig cfg;
  cfg.out_point_times = &w.times;
  D6PushbroomAssembler a(&store, cfg);
  REQUIRE(a.set_mount_extrinsics(mount).ok());
  a.set_pose_source(&poses);

  std::vector<ProfilePoint> profile;
  for (std::int64_t t_rev = kT0; t_rev < end_ns; t_rev += kRevNs) {
    profile.clear();
    for (int i = 0; i < kReturnsPerRev; ++i) {
      const std::int64_t t_ns = t_rev + kRevNs * i / kReturnsPerRev;
      const RigState truth = motion(static_cast<double>(t_ns - kT0) * 1e-9);
      double world_from_phone[16];
      const Pose p = pose_of(truth, t_ns);
      se3::mat4_from_quat_pos(p.orientation, p.position, world_from_phone);
      double world_from_lidar[16];
      se3::mat4_mul(world_from_phone, mount, world_from_lidar);
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
      pp.intensity = 130;
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
    for (std::uint32_t k = 0; k < v.count; ++k) w.pts.push_back(v.data[k]);
  }
  REQUIRE(w.pts.size() == w.times.size());
  return w;
}

// Mean and worst per-point distance between two clouds emitted in the same
// order. Both walks push identical profile streams, so index k is the same
// return in both — this is a true error, not a distribution comparison.
void compare(const std::vector<PointVertex>& a, const std::vector<PointVertex>& b, double* mean,
             double* worst) {
  REQUIRE(a.size() == b.size());
  double sum = 0.0, hi = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double dx = static_cast<double>(a[i].x) - b[i].x;
    const double dy = static_cast<double>(a[i].y) - b[i].y;
    const double dz = static_cast<double>(a[i].z) - b[i].z;
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    sum += d;
    if (d > hi) hi = d;
  }
  *mean = sum / static_cast<double>(a.size());
  *worst = hi;
}

std::vector<PointVertex> apply(const Walk& w, const TrajectoryCorrection& c) {
  std::vector<PointVertex> out = w.pts;
  for (std::size_t i = 0; i < out.size(); ++i) {
    float xyz[3] = {out[i].x, out[i].y, out[i].z};
    c.apply_point(w.times[i], xyz);
    out[i].x = xyz[0];
    out[i].y = xyz[1];
    out[i].z = xyz[2];
  }
  return out;
}

}  // namespace

TEST_CASE("round11/loop/se3 exp and log are inverses") {
  // The one piece of new arithmetic in the module. `se3.h` has carried so3_exp
  // and so3_log since A6; the six-dimensional pair is new here and everything
  // else rests on Exp(Log(T)) == T.
  const double axes[4][6] = {
      {0.0, 0.0, 0.0, 1.0, -2.0, 0.5},
      {0.3, -0.1, 0.7, 0.2, 0.0, -1.5},
      {1e-10, 0.0, -1e-10, 0.001, 0.002, 0.003},
      {2.9, 0.4, -1.1, 5.0, -3.0, 2.0},
  };
  for (const auto& xi : axes) {
    double m[16];
    se3_exp(xi, m);
    CHECK(se3::mat4_is_rigid(m, 1e-9));
    double back[6];
    se3_log(m, back);
    for (int i = 0; i < 6; ++i) CHECK(back[i] == doctest::Approx(xi[i]).epsilon(1e-9));
  }
  // Exp(0) is the identity exactly, which is what makes the first visit of a
  // closed loop untouched rather than nearly untouched.
  const double zero[6] = {0, 0, 0, 0, 0, 0};
  double id[16];
  se3_exp(zero, id);
  double want[16];
  se3::mat4_identity(want);
  for (int i = 0; i < 16; ++i) CHECK(id[i] == want[i]);
}

TEST_CASE("round11/loop/a loop with injected ARCore drift closes back to ground truth") {
  const double kYawDeg = 4.0;
  const double kTransM = 0.30;

  const Walk truth = resolve(circle, kLapSeconds, 0.0, 0.0);
  const Walk drifted = resolve(circle, kLapSeconds, kYawDeg, kTransM);
  REQUIRE(truth.pts.size() == drifted.pts.size());
  REQUIRE(truth.pts.size() > 50000);

  double mean_before = 0.0, worst_before = 0.0;
  compare(drifted.pts, truth.pts, &mean_before, &worst_before);

  TrajectoryLoopConfig cfg;
  TrajectoryCorrection corr;
  const LoopClosureReport rep = close_trajectory_loop(
      drifted.traj, Span<const PointVertex>(drifted.pts.data(), drifted.pts.size()),
      Span<const std::int64_t>(drifted.times.data(), drifted.times.size()), cfg, &corr);

  MESSAGE("decision: " << std::string(to_string(rep.decision)) << " — " << std::string(rep.reason));
  MESSAGE("revisit " << rep.loop_seconds << " s apart, " << rep.loop_path_m << " m of path, gap "
                     << rep.revisit_gap_m << " m, excursion " << rep.excursion_m << " m; ICP "
                     << rep.icp.inliers << " inliers (" << rep.icp.inlier_ratio << "), rms "
                     << rep.icp.rms_m << " m");
  MESSAGE("measured drift: " << rep.drift_translation_m << " m, " << rep.drift_rotation_deg
                             << " deg   (injected: " << kTransM << " m, " << kYawDeg << " deg)");
  MESSAGE("occupied 3 cm voxels: " << rep.occupied_voxels_before << " -> "
                                   << rep.occupied_voxels_after);
  REQUIRE(rep.decision == LoopDecision::kClosed);
  REQUIRE(corr.active());

  const std::vector<PointVertex> closed = apply(drifted, corr);
  double mean_after = 0.0, worst_after = 0.0;
  compare(closed, truth.pts, &mean_after, &worst_after);

  MESSAGE("per-point error against ground truth: mean " << 100.0 * mean_before << " cm -> "
                                                        << 100.0 * mean_after << " cm, worst "
                                                        << 100.0 * worst_before << " cm -> "
                                                        << 100.0 * worst_after << " cm");
  MESSAGE("overlap: " << 100.0 * rep.overlap_fraction
                      << " % of occupied voxels were painted twice; crispness gate "
                      << std::string(rep.crispness_checked ? "voted" : "abstained"));
  // The headline: the closure recovers most of the injected drift, and most
  // of ALL of it at the worst point (the far side of the loop, which is where
  // an operator sees the error).
  CHECK(mean_after < 0.60 * mean_before);
  CHECK(worst_after < 0.35 * worst_before);
  // The residual is not zero and it is not noise: the injected drift grows as
  // s^1.5 while the correction is the SE(3) geodesic (linear in s), so the two
  // agree exactly at both ends and differ by up to 0.6 deg in the middle —
  // about 3 cm at 3 m. That is the honest limit of a one-loop correction and
  // the reason a multi-loop capture would want a pose graph.
  CHECK(mean_after > 0.02);
  // A single lap paints almost every surface once, so gate 5 correctly
  // abstains here — the case that keeps it from vetoing good closures.
  CHECK(rep.overlap_fraction < 0.05);
  CHECK_FALSE(rep.crispness_checked);
  // The measured drift must resemble what was injected. Not equal: the
  // trajectory ends a fraction of a metre from where it started, and ICP sees
  // the geometry rather than the pose stream.
  CHECK(rep.drift_rotation_deg == doctest::Approx(kYawDeg).epsilon(0.45));
}

TEST_CASE("round11/loop/the two ends of the correction are exact") {
  const Walk drifted = resolve(circle, kLapSeconds, 4.0, 0.30);
  TrajectoryLoopConfig cfg;
  TrajectoryCorrection corr;
  const LoopClosureReport rep = close_trajectory_loop(
      drifted.traj, Span<const PointVertex>(drifted.pts.data(), drifted.pts.size()),
      Span<const std::int64_t>(drifted.times.data(), drifted.times.size()), cfg, &corr);
  REQUIRE(rep.decision == LoopDecision::kClosed);

  // s = 0 at the first visit: identity, bit for bit, so the beginning of the
  // walk is not moved by a closure at the end of it.
  CHECK(corr.fraction_at(rep.t_a_ns) == 0.0);
  double m[16];
  corr.matrix_at(rep.t_a_ns, m);
  double id[16];
  se3::mat4_identity(id);
  for (int i = 0; i < 16; ++i) CHECK(m[i] == id[i]);

  // s = 1 from the second visit onwards: the full measured transform, and it
  // stays there rather than continuing to grow.
  CHECK(corr.fraction_at(rep.t_b_ns) == 1.0);
  CHECK(corr.fraction_at(rep.t_b_ns + 60'000'000'000LL) == 1.0);
  corr.matrix_at(rep.t_b_ns, m);
  double fix[16];
  se3::mat4_from_quat_pos(rep.icp.q, rep.icp.p, fix);
  for (int i = 0; i < 16; ++i) CHECK(m[i] == doctest::Approx(fix[i]).epsilon(1e-9));

  // Monotone in between — the correction is spread, never reversed.
  double prev = 0.0;
  for (int k = 0; k <= 20; ++k) {
    const std::int64_t t = rep.t_a_ns + (rep.t_b_ns - rep.t_a_ns) * k / 20;
    const double s = corr.fraction_at(t);
    CHECK(s >= prev - 1e-12);
    prev = s;
  }
  CHECK(prev == doctest::Approx(1.0));
}

TEST_CASE("round11/loop/A ONE-WAY WALK NEVER CLOSES") {
  // The catastrophic case, and the reason it cannot happen is structural
  // rather than numerical: a walk that never comes back produces no candidate
  // pair at all, so there is nothing for any threshold to get wrong.
  const Walk w = resolve(straight, 9.0, 3.0, 0.25);
  TrajectoryLoopConfig cfg;
  TrajectoryCorrection corr;
  const LoopClosureReport rep = close_trajectory_loop(
      w.traj, Span<const PointVertex>(w.pts.data(), w.pts.size()),
      Span<const std::int64_t>(w.times.data(), w.times.size()), cfg, &corr);

  MESSAGE("straight walk: " << std::string(to_string(rep.decision)) << " ("
                            << rep.candidates_seen << " spatial candidates) — "
                            << std::string(rep.reason));
  CHECK(rep.decision == LoopDecision::kNoRevisit);
  CHECK(rep.candidates_seen == 0);
  CHECK_FALSE(corr.active());

  // And an inactive correction is a no-op on every point, bit for bit — the
  // cloud a refusal hands back is the cloud it was given.
  const std::vector<PointVertex> after = apply(w, corr);
  for (std::size_t i = 0; i < after.size(); ++i) {
    CHECK(after[i].x == w.pts[i].x);
    CHECK(after[i].y == w.pts[i].y);
    CHECK(after[i].z == w.pts[i].z);
  }
}

TEST_CASE("round11/loop/shuffling in one spot is a revisit and is not a loop") {
  // Every pose is within centimetres of every other one, so gate 1 fires
  // thousands of times. Gate 2 — the excursion — is what stops it, and
  // without gate 2 this fixture would close a loop on a rig that never moved.
  const Walk w = resolve(shuffle, 40.0, 0.0, 0.0);
  TrajectoryLoopConfig cfg;
  TrajectoryCorrection corr;
  const LoopClosureReport rep = close_trajectory_loop(
      w.traj, Span<const PointVertex>(w.pts.data(), w.pts.size()),
      Span<const std::int64_t>(w.times.data(), w.times.size()), cfg, &corr);

  MESSAGE("shuffling rig: " << std::string(to_string(rep.decision)) << " after "
                             << rep.candidates_seen << " spatial candidates");
  CHECK(rep.decision == LoopDecision::kNoExcursion);
  CHECK(rep.candidates_seen > 100);
  CHECK_FALSE(corr.active());
}

TEST_CASE("round11/loop/a correction that would blur the map is refused") {
  // Gate 5, exercised on the real closure by demanding an improvement it
  // cannot deliver (a tolerance of -100 % requires the occupancy to fall to
  // zero). The point is not the absurd threshold — it is that the gate
  // computes a real before/after on the whole cloud, and can veto a closure
  // that every other gate has already passed.
  //
  // This is not hypothetical. Run against the owner's scan-020 with the
  // pre-ROUND-11 magnitude bounds, ICP produced a 0.97 m / 17.0 deg "closure"
  // with 77.8 % inliers whose same-place mismatch genuinely improved from
  // 77 cm to 12 cm — and which raised the occupied-voxel count by 8.6 %. That
  // measurement is what set max_close_rotation_deg to 6.
  const Walk drifted = resolve(circle, kLapSeconds, 4.0, 0.30);

  TrajectoryLoopConfig ok_cfg;
  TrajectoryCorrection ok_corr;
  const LoopClosureReport ok = close_trajectory_loop(
      drifted.traj, Span<const PointVertex>(drifted.pts.data(), drifted.pts.size()),
      Span<const std::int64_t>(drifted.times.data(), drifted.times.size()), ok_cfg, &ok_corr);
  REQUIRE(ok.decision == LoopDecision::kClosed);

  TrajectoryLoopConfig strict = ok_cfg;
  // Both knobs, deliberately: force the gate to VOTE on a walk whose overlap
  // would normally make it abstain, and then demand an improvement it cannot
  // deliver.
  strict.min_overlap_for_crispness = 0.0;
  strict.crispness_tolerance = -1.0;
  TrajectoryCorrection none;
  const LoopClosureReport bad = close_trajectory_loop(
      drifted.traj, Span<const PointVertex>(drifted.pts.data(), drifted.pts.size()),
      Span<const std::int64_t>(drifted.times.data(), drifted.times.size()), strict, &none);

  MESSAGE("same closure, impossible crispness demand: "
          << std::string(to_string(bad.decision)) << " (" << bad.occupied_voxels_before
          << " -> " << bad.occupied_voxels_after << " voxels)");
  CHECK(bad.decision == LoopDecision::kMapGotWorse);
  CHECK_FALSE(none.active());
  // The gate reached the same ICP answer as the accepted run — it refused on
  // the crispness alone, not by disagreeing about the geometry.
  CHECK(bad.drift_translation_m == doctest::Approx(ok.drift_translation_m));
  CHECK(bad.occupied_voxels_before == ok.occupied_voxels_before);
}

TEST_CASE("round11/loop/an implausibly large correction is refused as a mismatch") {
  // Gate 4. 14 degrees of yaw over one 22 second lap is not VIO drift; a
  // closure that says it is has matched the wrong place. The fixture injects
  // it anyway and the gate has to say no rather than "close" 14 degrees of
  // fiction into the map.
  const Walk drifted = resolve(circle, kLapSeconds, 14.0, 0.9);
  TrajectoryLoopConfig cfg;
  TrajectoryCorrection corr;
  const LoopClosureReport rep = close_trajectory_loop(
      drifted.traj, Span<const PointVertex>(drifted.pts.data(), drifted.pts.size()),
      Span<const std::int64_t>(drifted.times.data(), drifted.times.size()), cfg, &corr);
  MESSAGE("14 deg of injected drift: " << std::string(to_string(rep.decision))
                                       << " — measured " << rep.drift_translation_m
                                       << " m / " << rep.drift_rotation_deg << " deg");
  CHECK((rep.decision == LoopDecision::kCorrectionTooBig ||
         rep.decision == LoopDecision::kGeometryRejected ||
         rep.decision == LoopDecision::kIcpFailed));
  CHECK_FALSE(corr.active());
}

TEST_CASE("round11/loop/closing is deterministic") {
  // Same bytes in, same bytes out — the doctrine, applied to the one new
  // module. Run twice and compare every corrected point exactly.
  const Walk drifted = resolve(circle, kLapSeconds, 4.0, 0.30);
  TrajectoryLoopConfig cfg;
  TrajectoryCorrection a, b;
  const LoopClosureReport ra = close_trajectory_loop(
      drifted.traj, Span<const PointVertex>(drifted.pts.data(), drifted.pts.size()),
      Span<const std::int64_t>(drifted.times.data(), drifted.times.size()), cfg, &a);
  const LoopClosureReport rb = close_trajectory_loop(
      drifted.traj, Span<const PointVertex>(drifted.pts.data(), drifted.pts.size()),
      Span<const std::int64_t>(drifted.times.data(), drifted.times.size()), cfg, &b);
  REQUIRE(ra.decision == LoopDecision::kClosed);
  CHECK(rb.decision == ra.decision);
  CHECK(rb.idx_a == ra.idx_a);
  CHECK(rb.idx_b == ra.idx_b);
  for (int i = 0; i < 4; ++i) CHECK(rb.icp.q[i] == ra.icp.q[i]);
  for (int i = 0; i < 3; ++i) CHECK(rb.icp.p[i] == ra.icp.p[i]);
  const std::vector<PointVertex> ca = apply(drifted, a);
  const std::vector<PointVertex> cb = apply(drifted, b);
  REQUIRE(ca.size() == cb.size());
  std::size_t diff = 0;
  for (std::size_t i = 0; i < ca.size(); ++i) {
    if (ca[i].x != cb[i].x || ca[i].y != cb[i].y || ca[i].z != cb[i].z) ++diff;
  }
  CHECK(diff == 0);
}
