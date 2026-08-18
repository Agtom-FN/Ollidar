// test_round13_section_stitch.cpp — ROUND 13.
//
// Two modules, both measured against injected truth rather than against
// themselves:
//
//   post::stitch_sections        — a capture broken into N frames goes back
//                                  into one, and a clean capture is a
//                                  bit-identical no-op.
//   post::check_mount_consistency — a puck rotated off its reference is
//                                  caught from where the returns land, and a
//                                  correctly mounted one is not.

#include <cmath>
#include <filesystem>
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "scanengine/poses/se3.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/slam/post/d6_resolve.h"
#include "scanengine/slam/post/mount_watch.h"
#include "scanengine/slam/post/reprocess.h"
#include "scanengine/slam/post/section_stitch.h"

using namespace scanengine;
using namespace scanengine::post;

namespace {

constexpr std::int64_t kHz30 = 33'333'333;
// 0.33 m/s at 30 Hz. The residual the analytic transform cannot remove.
constexpr double kStepM = 0.011;

void quat_from_yaw(double yaw_rad, double q[4]) {
  q[0] = 0.0;
  q[1] = std::sin(yaw_rad * 0.5);
  q[2] = 0.0;
  q[3] = std::cos(yaw_rad * 0.5);
}

// A walk down a corridor at the owner's own measured pace (0.33 m/s — see
// ROUND 12's table), at a constant height, looking along +X. Poses at 30 Hz.
//
// The pace matters to the assertions below. `T_k = pose_after * pose_before^-1`
// contains the operator's real motion across the pose gap as well as the frame
// change, which section_stitch.h names as the approximation it is. At 0.33 m/s
// and 33 ms that residual is 1.1 cm, and it is what the tolerances are set
// from — not from a hope about floating point.
std::vector<TrajPose> straight_walk(std::size_t n, std::int64_t t0 = 1'000'000'000) {
  std::vector<TrajPose> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    TrajPose p;
    p.t_ns = t0 + static_cast<std::int64_t>(i) * kHz30;
    p.p[0] = static_cast<double>(i) * kStepM;
    p.p[1] = 1.40;
    p.p[2] = 0.0;
    quat_from_yaw(0.0, p.q);
    out.push_back(p);
  }
  return out;
}

// Points on the two side walls and the floor and ceiling of a 3 m corridor,
// painted by whichever pose is current. Deterministic, no RNG.
void paint(const std::vector<TrajPose>& poses, std::vector<PointVertex>* pts,
           std::vector<std::int64_t>* times) {
  for (std::size_t i = 0; i < poses.size(); ++i) {
    const TrajPose& p = poses[i];
    // 24 returns per pose: the four surfaces at six positions along the sweep.
    for (int k = 0; k < 6; ++k) {
      const double along = -0.5 + 0.2 * static_cast<double>(k);
      const double x = p.p[0] + along;
      const struct {
        double y, z;
      } surf[4] = {{p.p[1] - 1.40, -1.5}, {p.p[1] - 1.40, 1.5}, {0.0, -1.5 + 0.5}, {2.6, 0.0}};
      for (int sfc = 0; sfc < 4; ++sfc) {
        PointVertex v{};
        v.x = static_cast<float>(x);
        v.y = static_cast<float>(sfc < 2 ? surf[sfc].y + 0.9 * static_cast<double>(k) * 0.1
                                         : surf[sfc].y);
        v.z = static_cast<float>(surf[sfc].z);
        v.r = v.g = v.b = 200;
        v.a = 255;
        pts->push_back(v);
        times->push_back(p.t_ns);
      }
    }
  }
}

// Re-express every pose and point from index `first` onward in a NEW world
// frame, i.e. exactly what ARCore does when it re-anchors: apply T to the
// poses, and to the points those poses painted.
void apply_reanchor(std::vector<TrajPose>* poses, std::vector<PointVertex>* pts,
              std::vector<std::int64_t>* times, std::size_t first, const double T[16]) {
  const std::int64_t t_at = (*poses)[first].t_ns;
  for (std::size_t i = first; i < poses->size(); ++i) {
    TrajPose& p = (*poses)[i];
    double R[9], t[3];
    se3::mat4_get_rt(T, R, t);
    double cq[4];
    se3::matrix_to_quat(R, cq);
    double nq[4];
    se3::quat_mul(cq, p.q, nq);
    se3::quat_normalize(nq);
    for (int k = 0; k < 4; ++k) p.q[k] = nq[k];
    double np[3];
    se3::mat4_apply(T, p.p, np);
    for (int k = 0; k < 3; ++k) p.p[k] = np[k];
  }
  for (std::size_t i = 0; i < pts->size(); ++i) {
    if ((*times)[i] < t_at) continue;
    const double in[3] = {(*pts)[i].x, (*pts)[i].y, (*pts)[i].z};
    double o[3];
    se3::mat4_apply(T, in, o);
    (*pts)[i].x = static_cast<float>(o[0]);
    (*pts)[i].y = static_cast<float>(o[1]);
    (*pts)[i].z = static_cast<float>(o[2]);
  }
}

double max_error(const std::vector<PointVertex>& a, const std::vector<PointVertex>& b) {
  REQUIRE(a.size() == b.size());
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double dx = a[i].x - b[i].x, dy = a[i].y - b[i].y, dz = a[i].z - b[i].z;
    worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  return worst;
}

}  // namespace

TEST_CASE("round13/stitch/a clean capture is an exact no-op") {
  const std::vector<TrajPose> poses = straight_walk(200);
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  paint(poses, &pts, &times);

  SectionCorrection corr;
  const SectionStitchReport r = stitch_sections(
      poses, Span<const PointVertex>(pts.data(), pts.size()),
      Span<const std::int64_t>(times.data(), times.size()), SectionStitchConfig{}, &corr);

  CHECK(r.sections == 1);
  CHECK(r.seams.empty());
  // Not "small". Inactive, so not one byte of the cloud can move.
  CHECK_FALSE(corr.active());
  std::vector<PointVertex> after = pts;
  for (std::size_t i = 0; i < after.size(); ++i) {
    float xyz[3] = {after[i].x, after[i].y, after[i].z};
    corr.apply_point(times[i], xyz);
    after[i].x = xyz[0];
    after[i].y = xyz[1];
    after[i].z = xyz[2];
  }
  CHECK(max_error(pts, after) == 0.0);
}

TEST_CASE("round13/stitch/one re-anchor is undone to the millimetre") {
  std::vector<TrajPose> poses = straight_walk(200);
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  paint(poses, &pts, &times);
  const std::vector<PointVertex> truth = pts;

  // ARCore decides it had drifted: 0.9 m and 11 degrees of yaw, applied to
  // everything from pose 100 on. Exactly the shape and size of the owner's
  // scan-030 breaks.
  double T[16];
  {
    double q[4];
    quat_from_yaw(11.0 * M_PI / 180.0, q);
    const double t[3] = {0.62, 0.05, -0.63};
    se3::mat4_from_quat_pos(q, t, T);
  }
  apply_reanchor(&poses, &pts, &times, 100, T);
  CHECK(max_error(truth, pts) > 0.5);  // the capture really is broken

  SectionStitchConfig cfg;
  cfg.refine = false;  // the analytic transform alone must do this
  SectionCorrection corr;
  const SectionStitchReport r =
      stitch_sections(poses, Span<const PointVertex>(pts.data(), pts.size()),
                      Span<const std::int64_t>(times.data(), times.size()), cfg, &corr);
  REQUIRE(r.sections == 2);
  REQUIRE(r.seams.size() == 1);
  CHECK(r.seams[0].jump_rotation_deg == doctest::Approx(11.0).epsilon(0.02));
  REQUIRE(corr.active());

  // Everything is now in the LAST section's frame, which is `truth` pushed
  // through T. So compare against that, not against truth itself.
  std::vector<PointVertex> expect = truth;
  for (PointVertex& v : expect) {
    const double in[3] = {v.x, v.y, v.z};
    double o[3];
    se3::mat4_apply(T, in, o);
    v.x = static_cast<float>(o[0]);
    v.y = static_cast<float>(o[1]);
    v.z = static_cast<float>(o[2]);
  }
  std::vector<PointVertex> after = pts;
  for (std::size_t i = 0; i < after.size(); ++i) {
    float xyz[3] = {after[i].x, after[i].y, after[i].z};
    corr.apply_point(times[i], xyz);
    after[i].x = xyz[0];
    after[i].y = xyz[1];
    after[i].z = xyz[2];
  }
  // The residual is the operator's own motion across the 33 ms gap and
  // nothing else: one pose step, 1.1 cm. A composition error would be the
  // size of the break itself, 0.9 m — eighty times larger.
  CHECK(max_error(expect, after) < 1.5 * kStepM);
  CHECK(max_error(truth, pts) > 50.0 * kStepM);
}

TEST_CASE("round13/stitch/four re-anchors compose in the right order") {
  std::vector<TrajPose> poses = straight_walk(500);
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  paint(poses, &pts, &times);
  const std::vector<PointVertex> truth = pts;

  const double yaw[4] = {8.0, -11.0, 12.0, -9.0};
  const double tx[4] = {0.51, -0.44, 0.60, -0.38};
  std::vector<double> Ts;
  for (int k = 0; k < 4; ++k) {
    double T[16], q[4];
    quat_from_yaw(yaw[k] * M_PI / 180.0, q);
    const double t[3] = {tx[k], 0.03 * (k + 1), -tx[k] * 0.8};
    se3::mat4_from_quat_pos(q, t, T);
    apply_reanchor(&poses, &pts, &times, 100 * static_cast<std::size_t>(k + 1), T);
    for (int i = 0; i < 16; ++i) Ts.push_back(T[i]);
  }

  SectionStitchConfig cfg;
  cfg.refine = false;
  SectionCorrection corr;
  const SectionStitchReport r =
      stitch_sections(poses, Span<const PointVertex>(pts.data(), pts.size()),
                      Span<const std::int64_t>(times.data(), times.size()), cfg, &corr);
  REQUIRE(r.sections == 5);
  REQUIRE(r.seams.size() == 4);

  // Truth in the last frame is T3*T2*T1*T0 applied to the original. Composing
  // the other way round is a different, wrong answer, and it was the bug this
  // case exists to pin: getting it backwards took scan-030's stitched
  // trajectory from 0.27 m of vertical wander to 1.56 m.
  double acc[16];
  se3::mat4_identity(acc);
  for (int k = 0; k < 4; ++k) {
    double next[16];
    se3::mat4_mul(&Ts[static_cast<std::size_t>(k) * 16], acc, next);
    for (int i = 0; i < 16; ++i) acc[i] = next[i];
  }
  std::vector<PointVertex> expect = truth;
  for (PointVertex& v : expect) {
    const double in[3] = {v.x, v.y, v.z};
    double o[3];
    se3::mat4_apply(acc, in, o);
    v.x = static_cast<float>(o[0]);
    v.y = static_cast<float>(o[1]);
    v.z = static_cast<float>(o[2]);
  }
  std::vector<PointVertex> after = pts;
  for (std::size_t i = 0; i < after.size(); ++i) {
    float xyz[3] = {after[i].x, after[i].y, after[i].z};
    corr.apply_point(times[i], xyz);
    after[i].x = xyz[0];
    after[i].y = xyz[1];
    after[i].z = xyz[2];
  }
  // Four seams, so up to four pose steps of un-removable operator motion.
  // Composing the other way round is metres out, so this still discriminates
  // by two orders of magnitude.
  CHECK(max_error(expect, after) < 5.0 * kStepM);
}

TEST_CASE("round13/stitch/a pose the tracker disowned is not a seam") {
  std::vector<TrajPose> poses = straight_walk(200);
  // The owner's scan-030 opens with 14 poses at exactly the origin carrying
  // quality 0 / tracking_lost 1. The step OUT of them is 1.2 m in 33 ms and
  // is not a re-anchor — it is the tracker starting to work.
  for (std::size_t i = 0; i < 14; ++i) {
    poses[i].p[0] = poses[i].p[1] = poses[i].p[2] = 0.0;
    poses[i].quality = 0;
    poses[i].tracking_lost = 1;
  }
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  paint(poses, &pts, &times);

  SectionCorrection corr;
  const SectionStitchReport r = stitch_sections(
      poses, Span<const PointVertex>(pts.data(), pts.size()),
      Span<const std::int64_t>(times.data(), times.size()), SectionStitchConfig{}, &corr);
  CHECK(r.sections == 1);
  CHECK(r.seams.empty());

  // ... and with the flags cleared, the same pose stream DOES produce a seam,
  // so the case is testing the flag and not the geometry.
  for (std::size_t i = 0; i < 14; ++i) {
    poses[i].quality = 3;
    poses[i].tracking_lost = 0;
  }
  const SectionStitchReport r2 = stitch_sections(
      poses, Span<const PointVertex>(pts.data(), pts.size()),
      Span<const std::int64_t>(times.data(), times.size()), SectionStitchConfig{}, &corr);
  CHECK(r2.sections == 2);
}

TEST_CASE("round13/stitch/the answer does not depend on the order points arrive in") {
  std::vector<TrajPose> poses = straight_walk(300);
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  paint(poses, &pts, &times);
  double T[16], q[4];
  quat_from_yaw(9.0 * M_PI / 180.0, q);
  const double t[3] = {0.5, 0.02, -0.4};
  se3::mat4_from_quat_pos(q, t, T);
  apply_reanchor(&poses, &pts, &times, 150, T);

  SectionCorrection c1, c2;
  const SectionStitchReport r1 = stitch_sections(
      poses, Span<const PointVertex>(pts.data(), pts.size()),
      Span<const std::int64_t>(times.data(), times.size()), SectionStitchConfig{}, &c1);

  // Reverse the cloud (and its pairing) — the same points, presented backwards.
  std::vector<PointVertex> rp(pts.rbegin(), pts.rend());
  std::vector<std::int64_t> rt(times.rbegin(), times.rend());
  const SectionStitchReport r2 =
      stitch_sections(poses, Span<const PointVertex>(rp.data(), rp.size()),
                      Span<const std::int64_t>(rt.data(), rt.size()), SectionStitchConfig{}, &c2);

  CHECK(r1.sections == r2.sections);
  CHECK(r1.total_translation_m == doctest::Approx(r2.total_translation_m).epsilon(1e-9));
  CHECK(r1.total_rotation_deg == doctest::Approx(r2.total_rotation_deg).epsilon(1e-9));
}

// --- the mount watchdog -----------------------------------------------------

namespace {

// One second of D6 revolutions from a phone at 1.4 m, fan vertical: returns
// run from the floor at -1.4 m to a 2.6 m ceiling at +1.2 m about the sensor.
void sweep(bool tilted, std::vector<TrajPose>* poses, std::vector<PointVertex>* pts,
           std::vector<std::int64_t>* times, double seconds = 8.0) {
  const std::int64_t t0 = 1'000'000'000;
  const std::size_t n_pose = static_cast<std::size_t>(seconds * 30.0);
  for (std::size_t i = 0; i < n_pose; ++i) {
    TrajPose p;
    p.t_ns = t0 + static_cast<std::int64_t>(i) * kHz30;
    p.p[0] = static_cast<double>(i) * 0.01;
    p.p[1] = 1.40;
    p.p[2] = 0.0;
    quat_from_yaw(0.0, p.q);
    poses->push_back(p);
  }
  // 100 returns per revolution, 10 revolutions per second.
  const std::size_t n_rev = static_cast<std::size_t>(seconds * 10.0);
  for (std::size_t r = 0; r < n_rev; ++r) {
    const std::int64_t tr = t0 + static_cast<std::int64_t>(r) * 100'000'000;
    double sp[3];
    {
      const std::size_t pi = std::min(n_pose - 1, static_cast<std::size_t>(r * 3));
      for (int k = 0; k < 3; ++k) sp[k] = (*poses)[pi].p[k];
    }
    for (int k = 0; k < 100; ++k) {
      const double theta = 2.0 * M_PI * static_cast<double>(k) / 100.0;
      // Range to the corridor's surfaces, capped at what a 3 m room gives.
      const double range = 1.5;
      double dy = range * std::sin(theta);
      double dz = range * std::cos(theta);
      // A vertical fan cannot put a return outside the room: clamp to the
      // floor and the ceiling the way real returns are.
      dy = std::max(-1.40, std::min(1.20, dy));
      // A puck rotated off its reference sends the same ranges to elevations
      // the room does not have — the fan is no longer clipped by floor and
      // ceiling because the assumed extrinsic points it somewhere else.
      if (tilted) dy = 3.2 * std::sin(theta);
      PointVertex v{};
      v.x = static_cast<float>(sp[0] + dz);
      v.y = static_cast<float>(sp[1] + dy);
      v.z = static_cast<float>(sp[2] + 0.3 * std::cos(theta));
      v.a = 255;
      pts->push_back(v);
      times->push_back(tr + static_cast<std::int64_t>(k) * 1'000'000);
    }
  }
}

}  // namespace

TEST_CASE("round13/mountwatch/a correctly mounted puck passes") {
  std::vector<TrajPose> poses;
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  sweep(false, &poses, &pts, &times);
  const MountWatchReport r =
      check_mount_consistency(poses, Span<const PointVertex>(pts.data(), pts.size()),
                              Span<const std::int64_t>(times.data(), times.size()));
  CHECK(r.verdict == MountWatchVerdict::kOk);
  CHECK(r.impossible_fraction == 0.0);
  CHECK(r.median_revolution_extent_m < 3.0);
}

TEST_CASE("round13/mountwatch/a rotated puck is caught in the first seconds") {
  std::vector<TrajPose> poses;
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  sweep(true, &poses, &pts, &times);
  MountWatchConfig cfg;
  cfg.window_seconds = 6.0;  // the operator has not walked the room yet
  const MountWatchReport r =
      check_mount_consistency(poses, Span<const PointVertex>(pts.data(), pts.size()),
                              Span<const std::int64_t>(times.data(), times.size()), cfg);
  CHECK(r.verdict == MountWatchVerdict::kMismatch);
  CHECK(r.impossible_fraction > 0.05);
  CHECK(r.operator_message[0] != '\0');
}

TEST_CASE("round13/mountwatch/refuses honestly rather than passing on no evidence") {
  std::vector<TrajPose> poses;
  std::vector<PointVertex> pts;
  std::vector<std::int64_t> times;
  sweep(false, &poses, &pts, &times, 0.5);  // five revolutions
  MountWatchConfig cfg;
  cfg.window_seconds = 6.0;
  const MountWatchReport r =
      check_mount_consistency(poses, Span<const PointVertex>(pts.data(), pts.size()),
                              Span<const std::int64_t>(times.data(), times.size()), cfg);
  // Not kOk. A watchdog that says "fine" when it has seen nothing is worse
  // than one that says nothing.
  CHECK(r.verdict == MountWatchVerdict::kNotMeasurable);
}

// --- the derived-product format (ABI 10's engine half) ----------------------

TEST_CASE("round13/reprocess/the stitched cloud round-trips and the reader prefers it") {
  // The real risk in the "Process this scan" path is not the arithmetic — that
  // is covered above — it is the FORMAT. The corrected cloud is written to
  // `processed/map_stitched.bin` and read back by a hand-rolled single-file
  // chunk reader in d6_resolve.cpp, because FileRecordReader walks a
  // hard-coded list of `streams/*.bin` and a derived product is deliberately
  // not on it (ROUND 9 named that hazard; this is the first thing to rely on
  // it). So writer and reader are tested against each other.
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "round13_stitched.lscan";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "processed");

  std::vector<PointVertex> pts;
  for (int i = 0; i < 9000; ++i) {  // spans several 4096-point chunks
    PointVertex v{};
    v.x = static_cast<float>(i) * 0.011f;
    v.y = static_cast<float>(i % 37) * 0.05f;
    v.z = static_cast<float>(i % 11) * -0.07f;
    v.r = static_cast<std::uint8_t>(i & 0xFF);
    v.g = 7;
    v.b = 200;
    v.a = 255;
    pts.push_back(v);
  }

  CHECK_FALSE(has_stitched_cloud(dir.string()));
  REQUIRE(write_point_chunk_file((dir / "processed/map_stitched.bin").string(), pts, 1234).ok());
  CHECK(has_stitched_cloud(dir.string()));

  PageStore store;
  std::uint64_t n = 0;
  REQUIRE(load_recorded_cloud(dir.string(), &store, StreamId::kSlamMap, &n).ok());
  CHECK(n == pts.size());

  // Bit-identical, not "close": these are the points the viewer draws, and a
  // lossy round trip here would be a silently different map.
  std::vector<PointVertex> back;
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) back.push_back(v.data[k]);
  }
  REQUIRE(back.size() == pts.size());
  for (std::size_t i = 0; i < pts.size(); ++i) {
    CHECK(back[i].x == pts[i].x);
    CHECK(back[i].y == pts[i].y);
    CHECK(back[i].z == pts[i].z);
    CHECK(back[i].r == pts[i].r);
    CHECK(back[i].a == pts[i].a);
  }

  // Deleting the derived file returns the container to exactly what the phone
  // sealed — the promise the sidecar makes in writing.
  std::filesystem::remove(dir / "processed/map_stitched.bin");
  CHECK_FALSE(has_stitched_cloud(dir.string()));
  PageStore store2;
  std::uint64_t n2 = 0;
  (void)load_recorded_cloud(dir.string(), &store2, StreamId::kSlamMap, &n2);
  CHECK(n2 == 0);

  std::filesystem::remove_all(dir);
}
