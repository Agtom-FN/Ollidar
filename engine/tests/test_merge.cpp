// test_merge.cpp — task A13: multi-session merge (Tech Spec §3.10).
//
// One synthetic building, scanned three times, is the fixture for almost
// everything here, because it is the only place ground truth exists:
//
//   merge/geo*      the georeferenced path — three sessions, three DIFFERENT
//                   ENU origins, one CRS. The claim under test is that the
//                   composition through ECEF is exact, so the alignment error
//                   is A10's transform error and nothing of A13's.
//   merge/pick*     the 3-point manual path: exact picks, noisy picks, and
//                   the collinear pick set that must be refused rather than
//                   answered.
//   merge/icp*      refinement: a perturbed alignment, the before/after
//                   residuals, and the monotonicity of the trace.
//   merge/overlap*  the pair with no shared geometry, which must report low
//                   overlap instead of a confident wrong transform.
//   merge/yaw*      the Manhattan fallback, including the symmetric room it
//                   is honestly unable to resolve.
//   merge/build*    dedup counts, the provenance run table, pages, and
//                   determinism.
//
// The building is 30 x 12 x 3 m with two interior partitions, and the three
// sessions cover x in [0,13], [9,22] and [18,30]: sessions 0-1 and 1-2
// overlap by 4 m each, and 0-2 share nothing at all — which is where the
// false-merge test comes from for free.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

#include "scanengine/cloud/page_store.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/merge/merge.h"
#include "scanengine/poses/se3.h"

using namespace scanengine;
using namespace scanengine::merge;

namespace {

// --- deterministic noise ----------------------------------------------------
//
// xorshift64 + Box-Muller, deliberately NOT <random>: the standard does not
// specify the output of its distributions, so the five CI legs would disagree
// on every number in this file. Same choice test_post.cpp and test_lio.cpp
// made.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next_u64() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() {  // [0, 1)
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
  }
  double gauss() {
    const double u1 = uniform() + 1e-12, u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

using Pt = std::array<double, 3>;

// axis 0: plane x = fixed, parameters (y, z)
// axis 1: plane y = fixed, parameters (x, z)
// axis 2: plane z = fixed, parameters (x, y)
void sample_plane(std::vector<Pt>* out, int axis, double fixed, double u0, double u1, double v0,
                  double v1, double pitch, Rng* rng) {
  for (double u = u0; u <= u1 + 1e-9; u += pitch) {
    for (double v = v0; v <= v1 + 1e-9; v += pitch) {
      double uu = u + (rng->uniform() - 0.5) * pitch;
      double vv = v + (rng->uniform() - 0.5) * pitch;
      uu = std::min(std::max(uu, u0), u1);
      vv = std::min(std::max(vv, v0), v1);
      Pt p{};
      if (axis == 0) {
        p = {fixed, uu, vv};
      } else if (axis == 1) {
        p = {uu, fixed, vv};
      } else {
        p = {uu, vv, fixed};
      }
      out->push_back(p);
    }
  }
}

// The building, in the world (== the reference ENU) frame. Independently
// jittered per call, so two sessions are two independent samplings of the
// same surfaces rather than the same points twice.
void building_surfaces(std::vector<Pt>* out, Rng* rng) {
  sample_plane(out, 2, 0.0, 0.0, 30.0, 0.0, 12.0, 0.20, rng);   // floor
  sample_plane(out, 2, 3.0, 0.0, 30.0, 0.0, 12.0, 0.25, rng);   // ceiling
  sample_plane(out, 1, 0.0, 0.0, 30.0, 0.0, 3.0, 0.16, rng);    // long wall y=0
  sample_plane(out, 1, 12.0, 0.0, 30.0, 0.0, 3.0, 0.16, rng);   // long wall y=12
  sample_plane(out, 0, 0.0, 0.0, 12.0, 0.0, 3.0, 0.16, rng);    // end wall x=0
  sample_plane(out, 0, 30.0, 0.0, 12.0, 0.0, 3.0, 0.16, rng);   // end wall x=30
  sample_plane(out, 0, 10.0, 0.0, 8.0, 0.0, 3.0, 0.16, rng);    // partition (corridor at y>8)
  sample_plane(out, 0, 20.0, 0.0, 8.0, 0.0, 3.0, 0.16, rng);    // partition
}

void make_pose(double yaw_deg, double x, double y, double z, double m[16]) {
  const double c = std::cos(yaw_deg * se3::kDegToRad), s = std::sin(yaw_deg * se3::kDegToRad);
  const double R[9] = {c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0};
  const double t[3] = {x, y, z};
  se3::mat4_from_rt(R, t, m);
}

// The part of the building inside [xlo, xhi], expressed in the session's own
// local frame, with per-point range noise.
std::vector<PointVertex> session_cloud(std::uint64_t seed, double xlo, double xhi,
                                       const double world_from_session[16], double noise_m,
                                       std::uint8_t tint) {
  Rng rng(seed);
  std::vector<Pt> pts;
  building_surfaces(&pts, &rng);
  double session_from_world[16];
  se3::mat4_inverse_rigid(world_from_session, session_from_world);
  std::vector<PointVertex> out;
  out.reserve(pts.size() / 2);
  for (const Pt& p : pts) {
    if (p[0] < xlo || p[0] > xhi) continue;
    const double w[3] = {p[0] + rng.gauss() * noise_m, p[1] + rng.gauss() * noise_m,
                         p[2] + rng.gauss() * noise_m};
    double l[3];
    se3::mat4_apply(session_from_world, w, l);
    PointVertex v{};
    v.x = static_cast<float>(l[0]);
    v.y = static_cast<float>(l[1]);
    v.z = static_cast<float>(l[2]);
    v.r = tint;
    v.g = 180;
    v.b = static_cast<std::uint8_t>(255 - tint);
    v.a = 255;
    out.push_back(v);
  }
  return out;
}

Span<const PointVertex> span_of(const std::vector<PointVertex>& v) {
  return Span<const PointVertex>(v.data(), v.size());
}

// ENU_to <- ENU_from, computed through GEODETIC coordinates rather than
// through the ECEF rotation composition align.h uses. Two independent routes
// to the same rigid transform is what makes the georeferencing test a test.
void enu_from_enu_numeric(const crs::EnuFrame& to, const crs::EnuFrame& from, double m[16]) {
  auto map = [&](double e, double n, double u, double out[3]) {
    const crs::Geodetic g = crs::enu_to_geodetic(from, crs::Enu{e, n, u});
    const crs::Enu q = crs::geodetic_to_enu(to, g);
    out[0] = q.e;
    out[1] = q.n;
    out[2] = q.u;
  };
  double o[3], ex[3], ey[3], ez[3];
  map(0.0, 0.0, 0.0, o);
  map(1.0, 0.0, 0.0, ex);
  map(0.0, 1.0, 0.0, ey);
  map(0.0, 0.0, 1.0, ez);
  double R[9] = {ex[0] - o[0], ey[0] - o[0], ez[0] - o[0], ex[1] - o[1], ey[1] - o[1],
                 ez[1] - o[1], ex[2] - o[2], ey[2] - o[2], ez[2] - o[2]};
  double q[4];
  se3::matrix_to_quat(R, q);
  se3::quat_to_matrix(q, R);  // project the finite-difference basis onto SO(3)
  se3::mat4_from_rt(R, o, m);
}

SessionGeoref make_georef(const crs::EnuFrame& session_enu, const crs::EnuFrame& world_enu,
                          const double world_from_session[16], double sigma_h_m, int epsg) {
  SessionGeoref g;
  g.valid = true;
  g.enu = session_enu;
  g.epsg = epsg;
  g.solution.converged = true;
  g.solution.scale = 1.0;
  g.solution.horizontal_sigma_m = sigma_h_m;
  g.solution.vertical_sigma_m = sigma_h_m * 1.6;
  double enu_from_world[16];
  enu_from_enu_numeric(session_enu, world_enu, enu_from_world);
  se3::mat4_mul(enu_from_world, world_from_session, g.solution.global_from_local);
  return g;
}

void transform_error_mm_deg(const double a[16], const double b[16], double* mm, double* deg) {
  se3::transform_error(a, b, deg, mm);
}

// The three truth poses. Session 0 is the anchor, so the merged frame is its
// local frame and every expectation below is expressed against it.
struct Fixture {
  double world_from_s[3][16];
  std::vector<PointVertex> cloud[3];
  crs::EnuFrame world_enu;
  crs::EnuFrame session_enu[3];

  Fixture() {
    make_pose(0.0, 2.0, 6.0, 1.40, world_from_s[0]);
    make_pose(35.0, 15.0, 5.0, 1.42, world_from_s[1]);
    make_pose(-70.0, 26.0, 7.0, 1.38, world_from_s[2]);
    const double xlo[3] = {0.0, 9.0, 18.0};
    const double xhi[3] = {13.0, 22.0, 30.0};
    const std::uint8_t tint[3] = {240, 120, 30};
    for (int i = 0; i < 3; ++i) {
      cloud[i] = session_cloud(0x51ED0001ull + static_cast<std::uint64_t>(i) * 7919ull, xlo[i],
                               xhi[i], world_from_s[i], 0.005, tint[i]);
    }
    // One CRS (WGS84 / UTM 32N territory), three ENU origins: the sessions
    // were anchored on three different days, hundreds of metres apart.
    world_enu = crs::make_enu_frame(crs::Geodetic{47.376900, 8.541700, 408.0});
    session_enu[0] = crs::make_enu_frame(crs::Geodetic{47.376900, 8.541700, 408.0});
    session_enu[1] = crs::make_enu_frame(crs::Geodetic{47.378700, 8.543900, 411.5});
    session_enu[2] = crs::make_enu_frame(crs::Geodetic{47.375100, 8.538200, 405.2});
  }

  // truth for `MergeProject`: anchor-local from session-local.
  void truth_world_from_session(int i, double out[16]) const {
    double inv0[16];
    se3::mat4_inverse_rigid(world_from_s[0], inv0);
    se3::mat4_mul(inv0, world_from_s[i], out);
  }

  SessionInput input(int i, bool with_georef) const {
    SessionInput in;
    in.provenance_id = "wing-" + std::to_string(i);
    in.lscan_dir = "/scans/wing-" + std::to_string(i) + ".lscan";
    in.cloud.add(span_of(cloud[i]));
    if (with_georef) {
      in.georef = make_georef(session_enu[i], world_enu, world_from_s[i], 0.02, 32632);
    }
    return in;
  }
};

}  // namespace

// --- the ENU composition ----------------------------------------------------

TEST_CASE("merge/enu_composition_matches_the_geodetic_route") {
  const crs::EnuFrame a = crs::make_enu_frame(crs::Geodetic{47.3769, 8.5417, 408.0});
  const crs::EnuFrame b = crs::make_enu_frame(crs::Geodetic{47.3801, 8.5502, 415.0});

  double direct[16], numeric[16];
  REQUIRE(enu_from_enu(a, b, direct).ok());
  enu_from_enu_numeric(a, b, numeric);

  double mm = 0.0, deg = 0.0;
  transform_error_mm_deg(direct, numeric, &mm, &deg);
  CHECK(mm < 0.001);  // micrometres
  CHECK(deg < 1e-7);

  // It is NOT the identity: 700 m of separation rotates the ENU basis.
  double R[9], t[3];
  se3::mat4_get_rt(direct, R, t);
  const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  CHECK(se3::rot_angle_deg(R, eye) > 0.001);
  CHECK(se3::norm3(t) > 500.0);

  // Round trip.
  double back[16], id[16];
  REQUIRE(enu_from_enu(b, a, back).ok());
  se3::mat4_mul(direct, back, id);
  double eye4[16];
  se3::mat4_identity(eye4);
  transform_error_mm_deg(id, eye4, &mm, &deg);
  CHECK(mm < 0.001);

  // An invalid frame is refused rather than silently treated as the identity.
  crs::EnuFrame bad{};
  double junk[16];
  CHECK_FALSE(enu_from_enu(a, bad, junk).ok());
}

// --- 1. the georeferenced path ---------------------------------------------

TEST_CASE("merge/geo_aligns_three_sessions_through_the_shared_crs") {
  Fixture f;
  MergeProject proj;
  for (int i = 0; i < 3; ++i) {
    const Result<std::uint32_t> id = proj.add_session(f.input(i, true));
    REQUIRE(id.ok());
    CHECK(id.value() == static_cast<std::uint32_t>(i));
  }
  CHECK(proj.session_count() == 3);
  CHECK(proj.anchor() == 0);
  CHECK(proj.find("wing-2") == 2);
  CHECK(proj.find("nope") == -1);

  MergeProject::GeorefAlignReport rep;
  REQUIRE(proj.align_georeferenced(&rep).ok());
  CHECK(rep.aligned == 3);
  CHECK(rep.skipped == 0);
  CHECK_FALSE(rep.epsg_mismatch);
  CHECK(rep.max_origin_separation_m > 200.0);  // the three ENU origins really differ

  double worst_mm = 0.0, worst_deg = 0.0;
  for (int i = 0; i < 3; ++i) {
    double truth[16];
    f.truth_world_from_session(i, truth);
    double mm = 0.0, deg = 0.0;
    transform_error_mm_deg(proj.session(static_cast<std::uint32_t>(i)).world_from_session, truth,
                           &mm, &deg);
    worst_mm = std::max(worst_mm, mm);
    worst_deg = std::max(worst_deg, deg);
    CHECK(proj.session(static_cast<std::uint32_t>(i)).align == AlignSource::kGeoreferenced);
  }
  MESSAGE("georeferenced alignment: worst " << worst_mm << " mm / " << worst_deg << " deg");
  CHECK(worst_mm < 5.0);   // the §3.10 exit number for this path
  CHECK(worst_deg < 0.01);

  // The anchor is exactly the merged frame.
  double eye[16];
  se3::mat4_identity(eye);
  double mm = 0.0, deg = 0.0;
  transform_error_mm_deg(proj.session(0).world_from_session, eye, &mm, &deg);
  CHECK(mm == 0.0);
}

TEST_CASE("merge/geo_refuses_when_the_anchor_is_not_georeferenced") {
  Fixture f;
  MergeProject proj;
  REQUIRE(proj.add_session(f.input(0, false)).ok());  // anchor, no georef
  REQUIRE(proj.add_session(f.input(1, true)).ok());
  MergeProject::GeorefAlignReport rep;
  const Status st = proj.align_georeferenced(&rep);
  CHECK(st.error() == ScanError::kInvalidState);
  CHECK(std::string(rep.blocker).find("anchor") != std::string::npos);
  CHECK(rep.aligned == 0);

  // With a project ENU frame there is no anchor requirement: the merged frame
  // is the CRS itself.
  REQUIRE(proj.set_project_enu_frame(f.world_enu).ok());
  MergeProject::GeorefAlignReport rep2;
  REQUIRE(proj.align_georeferenced(&rep2).ok());
  CHECK(rep2.aligned == 1);
  CHECK(rep2.skipped == 1);
  CHECK(rep2.reference_is_project);
  double truth[16], mm = 0.0, deg = 0.0;
  // In the project frame, session 1 sits at its own truth pose.
  for (int i = 0; i < 16; ++i) truth[i] = f.world_from_s[1][i];
  transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm, &deg);
  CHECK(mm < 5.0);
}

// --- 2. the manual 3-point path --------------------------------------------

namespace {

// Three physical features an operator could click, chosen non-collinear and
// spread across the shared volume.
const double kPickWorld[4][3] = {
    {10.0, 0.0, 0.0}, {10.0, 8.0, 3.0}, {12.5, 12.0, 0.0}, {9.5, 4.0, 3.0}};

std::vector<PointCorrespondence> picks_for(const Fixture& f, int session, std::size_t n,
                                           double pick_noise_m, std::uint64_t seed) {
  Rng rng(seed);
  double s_from_w[16], anchor_from_w[16];
  se3::mat4_inverse_rigid(f.world_from_s[session], s_from_w);
  se3::mat4_inverse_rigid(f.world_from_s[0], anchor_from_w);
  std::vector<PointCorrespondence> out;
  for (std::size_t i = 0; i < n; ++i) {
    PointCorrespondence c;
    se3::mat4_apply(s_from_w, kPickWorld[i], c.a);
    se3::mat4_apply(anchor_from_w, kPickWorld[i], c.b);
    for (int k = 0; k < 3; ++k) {
      c.a[k] += rng.gauss() * pick_noise_m;
      c.b[k] += rng.gauss() * pick_noise_m;
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

TEST_CASE("merge/picks_recover_a_known_transform") {
  Fixture f;
  MergeProject proj;
  REQUIRE(proj.add_session(f.input(0, false)).ok());
  REQUIRE(proj.add_session(f.input(1, false)).ok());

  double truth[16];
  f.truth_world_from_session(1, truth);

  SUBCASE("exact picks, three of them") {
    const std::vector<PointCorrespondence> picks = picks_for(f, 1, 3, 0.0, 1);
    CorrespondenceSolution sol;
    REQUIRE(proj.align_from_correspondences(1, Span<const PointCorrespondence>(picks.data(), 3), {},
                                            &sol)
                .ok());
    CHECK(sol.ok);
    CHECK(sol.pairs == 3);
    CHECK(sol.rms_m < 1e-9);
    CHECK(std::fabs(sol.implied_scale - 1.0) < 1e-9);
    // Three points are coplanar: the smallest spread is ~0 and that is fine.
    CHECK(sol.spread_m[0] < 1e-6);
    CHECK(sol.spread_m[1] > 1.0);
    double mm = 0.0, deg = 0.0;
    transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm, &deg);
    CHECK(mm < 1e-3);
    CHECK(deg < 1e-6);
    CHECK(proj.session(1).align == AlignSource::kManual);
  }

  SUBCASE("noisy picks: 2 cm per click") {
    const std::vector<PointCorrespondence> picks = picks_for(f, 1, 4, 0.02, 7);
    CorrespondenceSolution sol;
    REQUIRE(proj.align_from_correspondences(1, Span<const PointCorrespondence>(picks.data(), 4), {},
                                            &sol)
                .ok());
    CHECK(sol.ok);
    CHECK(sol.rms_m > 0.0);
    CHECK(sol.rms_m < 0.10);
    CHECK(sol.residuals_m.size() == 4);
    double mm = 0.0, deg = 0.0;
    transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm, &deg);
    MESSAGE("noisy 3-point pick error: " << mm << " mm / " << deg << " deg (rms "
                                         << sol.rms_m * 1000.0 << " mm)");
    // A 2 cm click over an ~8 m pick baseline: centimetres and a fraction of
    // a degree. This is what makes ICP mandatory after a manual placement.
    CHECK(mm < 120.0);
    CHECK(deg < 1.0);
  }

  SUBCASE("collinear picks are refused, not answered") {
    std::vector<PointCorrespondence> picks;
    for (int i = 0; i < 3; ++i) {
      const double w[3] = {10.0, 2.0 * i, 0.0};  // three points on one line
      PointCorrespondence c;
      double s_from_w[16], anchor_from_w[16];
      se3::mat4_inverse_rigid(f.world_from_s[1], s_from_w);
      se3::mat4_inverse_rigid(f.world_from_s[0], anchor_from_w);
      se3::mat4_apply(s_from_w, w, c.a);
      se3::mat4_apply(anchor_from_w, w, c.b);
      picks.push_back(c);
    }
    CorrespondenceSolution sol;
    const Status st = proj.align_from_correspondences(
        1, Span<const PointCorrespondence>(picks.data(), picks.size()), {}, &sol);
    CHECK_FALSE(st.ok());
    CHECK_FALSE(sol.ok);
    CHECK(std::string(sol.blocker) == "correspondences are collinear");
    CHECK(proj.session(1).align == AlignSource::kNone);  // nothing was applied
  }

  SUBCASE("two picks are not enough") {
    const std::vector<PointCorrespondence> picks = picks_for(f, 1, 2, 0.0, 3);
    CorrespondenceSolution sol;
    CHECK_FALSE(proj.align_from_correspondences(1, Span<const PointCorrespondence>(picks.data(), 2),
                                                {}, &sol)
                    .ok());
    CHECK(std::string(sol.blocker) == "too few usable correspondences");
  }

  SUBCASE("a scale is reported but never applied") {
    std::vector<PointCorrespondence> picks = picks_for(f, 1, 4, 0.0, 11);
    for (auto& c : picks) {
      for (int k = 0; k < 3; ++k) c.b[k] *= 1.02;  // 2% stretch in the target picks
    }
    CorrespondenceOptions opts;
    opts.allow_scale = true;
    CorrespondenceSolution sol;
    const Status st = proj.align_from_correspondences(
        1, Span<const PointCorrespondence>(picks.data(), picks.size()), opts, &sol);
    CHECK(st.error() == ScanError::kNotSupported);
    CHECK(sol.ok);
    CHECK(sol.scale > 1.0);
    CHECK(proj.session(1).align == AlignSource::kNone);
  }
}

// --- 3. refinement ----------------------------------------------------------

TEST_CASE("merge/icp_improves_a_perturbed_alignment") {
  Fixture f;
  MergeProject proj;
  REQUIRE(proj.add_session(f.input(0, false)).ok());
  REQUIRE(proj.add_session(f.input(1, false)).ok());

  double truth[16];
  f.truth_world_from_session(1, truth);

  // A plausible coarse error: 2 degrees of yaw and 15 cm of translation.
  double perturb[16], start[16];
  make_pose(2.0, 0.12, -0.09, 0.03, perturb);
  se3::mat4_mul(perturb, truth, start);
  REQUIRE(proj.set_alignment(1, start, AlignSource::kManual).ok());

  double mm_before = 0.0, deg_before = 0.0;
  transform_error_mm_deg(start, truth, &mm_before, &deg_before);

  RefineConfig cfg;
  REQUIRE(proj.refine(cfg).ok());

  const MergeReport& rep = proj.report();
  REQUIRE(rep.pairs.size() == 1);
  const MergePair& p = rep.pairs[0];
  CHECK(p.refined);
  CHECK(p.converged);
  CHECK_FALSE(p.low_overlap);
  CHECK(p.overlap_fraction > 0.20f);
  CHECK(p.inliers > 1000);

  MESSAGE("pair rms " << p.rms_before_m * 1000.0 << " mm -> " << p.rms_residual_m * 1000.0
                      << " mm in " << p.iterations << " iterations, overlap "
                      << p.overlap_fraction);
  CHECK(p.rms_residual_m < p.rms_before_m);
  CHECK(p.rms_residual_m < 0.015f);

  double mm_after = 0.0, deg_after = 0.0;
  transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm_after, &deg_after);
  MESSAGE("alignment error " << mm_before << " mm / " << deg_before << " deg -> " << mm_after
                             << " mm / " << deg_after << " deg");
  CHECK(mm_after < mm_before);
  CHECK(deg_after < deg_before);
  CHECK(mm_after < 15.0);
  CHECK(deg_after < 0.1);
  CHECK(proj.session(1).align == AlignSource::kIcp);
}

TEST_CASE("merge/icp_trace_is_monotone_and_reports_every_iteration") {
  Fixture f;
  double truth[16], perturb[16], start[16];
  double inv0[16];
  se3::mat4_inverse_rigid(f.world_from_s[0], inv0);
  se3::mat4_mul(inv0, f.world_from_s[1], truth);
  make_pose(2.5, -0.18, 0.11, -0.04, perturb);
  se3::mat4_mul(perturb, truth, start);

  const PairIcpResult r =
      refine_pair(span_of(f.cloud[1]), span_of(f.cloud[0]), start, MergeIcpConfig{});
  for (const auto& e : r.trace) {
    MESSAGE("  it " << e.index << " gate " << e.gate_m << " m: rms " << e.rms_m * 1000.0
                    << " mm, mean |r| " << e.fitness_m * 1000.0 << " mm, " << e.inliers
                    << " inliers, step " << e.step_rot_deg << " deg / " << e.step_trans_m * 1000.0
                    << " mm");
  }
  REQUIRE(r.refined);
  REQUIRE(r.trace.size() >= 3);
  CHECK(r.trace.size() == r.iterations + 1);  // one evaluation per step, plus the final one
  for (std::size_t i = 1; i < r.trace.size(); ++i) {
    CHECK(r.trace[i].rms_m <= r.trace[i - 1].rms_m);
  }
  CHECK(r.rms_before_m == r.trace.front().rms_m);
  CHECK(r.rms_after_m == r.trace.back().rms_m);
  CHECK(r.trace.back().step_rot_deg == 0.0);
  CHECK(r.trace.front().step_trans_m > 0.0);
  CHECK(r.converged);
  CHECK(r.source_points > 1000);
  CHECK(r.target_points > 1000);
  MESSAGE("icp trace: " << r.trace.size() << " entries, " << r.trace.front().rms_m * 1000.0
                        << " mm -> " << r.trace.back().rms_m * 1000.0 << " mm, "
                        << r.rejected_steps << " rejected, " << r.ms << " ms");

  // A non-rigid init is refused rather than quietly mirrored.
  double bad[16];
  for (int i = 0; i < 16; ++i) bad[i] = start[i];
  bad[0] *= 2.0;
  const PairIcpResult rb =
      refine_pair(span_of(f.cloud[1]), span_of(f.cloud[0]), bad, MergeIcpConfig{});
  CHECK_FALSE(rb.refined);
  CHECK(std::string(rb.blocker) == "initial transform is not rigid");
}

// --- 4. the overlap gate ----------------------------------------------------

TEST_CASE("merge/non_overlapping_pair_reports_overlap_not_a_merge") {
  Fixture f;
  MergeProject proj;
  REQUIRE(proj.add_session(f.input(0, true)).ok());
  REQUIRE(proj.add_session(f.input(2, true)).ok());  // x in [0,13] and [18,30]
  REQUIRE(proj.align_georeferenced().ok());

  double before[16];
  for (int i = 0; i < 16; ++i) before[i] = proj.session(1).world_from_session[i];

  const Status st = proj.refine();
  CHECK(st.error() == ScanError::kNotFound);  // nothing was refined

  const MergeReport& rep = proj.report();
  REQUIRE(rep.pairs.size() == 1);
  const MergePair& p = rep.pairs[0];
  CHECK(p.low_overlap);
  CHECK_FALSE(p.refined);
  CHECK_FALSE(p.converged);
  CHECK(p.overlap_fraction < 0.05f);
  CHECK(std::string(p.blocker) == "overlap below threshold");
  CHECK(rep.pairs_low_overlap == 1);
  MESSAGE("disjoint wings: overlap a_in_b " << p.overlap_a_in_b << " b_in_a " << p.overlap_b_in_a);

  // And the alignment is untouched — a low-overlap pair must not move anything.
  double mm = 0.0, deg = 0.0;
  transform_error_mm_deg(proj.session(1).world_from_session, before, &mm, &deg);
  CHECK(mm == 0.0);
  CHECK(deg == 0.0);
}

TEST_CASE("merge/overlap_survey_matches_the_geometry") {
  Fixture f;
  MergeProject proj;
  for (int i = 0; i < 3; ++i) REQUIRE(proj.add_session(f.input(i, true)).ok());
  REQUIRE(proj.align_georeferenced().ok());
  REQUIRE(proj.survey_overlap().ok());

  const MergeReport& rep = proj.report();
  REQUIRE(rep.pairs.size() == 3);
  const MergePair* p01 = rep.pair(0, 1);
  const MergePair* p12 = rep.pair(1, 2);
  const MergePair* p02 = rep.pair(0, 2);
  REQUIRE(p01 != nullptr);
  REQUIRE(p12 != nullptr);
  REQUIRE(p02 != nullptr);
  // 4 m of shared building out of a 13 m window is ~30%.
  CHECK(p01->overlap_fraction > 0.20f);
  CHECK(p01->overlap_fraction < 0.50f);
  CHECK(p12->overlap_fraction > 0.20f);
  CHECK(p02->overlap_fraction < 0.05f);
  CHECK(p02->low_overlap);
  CHECK(rep.pairs_low_overlap == 1);
}

// --- 5. the yaw + translation fallback -------------------------------------

TEST_CASE("merge/yaw_search_places_a_co_located_session") {
  // The case the fallback is FOR: two sessions of the same volume, no
  // georeferencing, no picks. Independent sampling and noise, different local
  // frames.
  Fixture f;
  double world_from_b[16];
  make_pose(-48.0, 6.5, 9.0, 1.44, world_from_b);
  const std::vector<PointVertex> cloud_b =
      session_cloud(0x77A13ull, 0.0, 13.0, world_from_b, 0.005, 60);

  MergeProject proj;
  SessionInput a = f.input(0, false);
  REQUIRE(proj.add_session(a).ok());
  SessionInput b;
  b.provenance_id = "second-visit";
  b.cloud.add(span_of(cloud_b));
  REQUIRE(proj.add_session(b).ok());

  YawSearchResult r;
  const Status st = proj.align_yaw_search(1, 0, YawSearchConfig{}, &r);
  MESSAGE("yaw search: yaw " << r.yaw_deg << " deg, overlap " << r.overlap << ", runner-up "
                             << r.runner_up_overlap << " at " << r.runner_up_yaw_deg
                             << " deg, margin " << r.margin << " (" << r.yaws_scored
                             << " yaws scored)");
  REQUIRE(st.ok());
  CHECK(r.ok);
  CHECK_FALSE(r.ambiguous);
  CHECK(r.overlap > 0.7);

  double truth[16], inv0[16];
  se3::mat4_inverse_rigid(f.world_from_s[0], inv0);
  se3::mat4_mul(inv0, world_from_b, truth);
  double mm = 0.0, deg = 0.0;
  transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm, &deg);
  MESSAGE("yaw-search placement error: " << mm / 1000.0 << " m / " << deg << " deg");
  // Coarse is all it claims to be: a degree or so and well under a metre.
  CHECK(deg < 2.0);
  CHECK(mm < 800.0);

  // ...and that is good enough for ICP to finish the job, which is the whole
  // reason a coarse path is allowed to be this rough.
  REQUIRE(proj.refine().ok());
  transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm, &deg);
  MESSAGE("after ICP: " << mm << " mm / " << deg << " deg");
  CHECK(mm < 20.0);
  CHECK(deg < 0.2);
}

TEST_CASE("merge/yaw_search_slides_along_a_corridor_and_says_it_is_confident") {
  // THE FAILURE THAT HAS TO BE ON THE RECORD. Sessions 0 and 1 overlap by
  // only 4 m of a 30 m extruded corridor. Occupancy overlap is maximized by
  // hiding the source INSIDE the target rather than by putting it where it
  // belongs: the wrong answer scores 0.95, the right one about 0.3. Neither
  // the overlap score nor the ICP residual afterwards detects it — ICP
  // converges beautifully onto the wrong 10 m.
  //
  // This is why §3.10 makes the 3-point pick the non-georeferenced path and
  // why align_yaw_search() is documented as operator-confirmed.
  Fixture f;
  MergeProject proj;
  REQUIRE(proj.add_session(f.input(0, false)).ok());
  REQUIRE(proj.add_session(f.input(1, false)).ok());

  YawSearchResult r;
  const Status st = proj.align_yaw_search(1, 0, YawSearchConfig{}, &r);
  REQUIRE(st.ok());  // it is CONFIDENT
  CHECK(r.ok);
  CHECK(r.overlap > 0.9);
  CHECK(r.margin > 0.05);

  double truth[16];
  f.truth_world_from_session(1, truth);
  double mm = 0.0, deg = 0.0;
  transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm, &deg);
  MESSAGE("corridor slide: overlap " << r.overlap << ", margin " << r.margin << ", placement error "
                                     << mm / 1000.0 << " m / " << deg << " deg");
  CHECK(mm > 3000.0);  // metres wrong, with a 0.95 score

  // ICP does not save it: it converges, reports a small residual, and is
  // still in the wrong place.
  REQUIRE(proj.refine().ok());
  const MergePair& p = proj.report().pairs[0];
  transform_error_mm_deg(proj.session(1).world_from_session, truth, &mm, &deg);
  MESSAGE("after ICP: rms " << p.rms_residual_m * 1000.0 << " mm, overlap " << p.overlap_fraction
                            << ", still " << mm / 1000.0 << " m from truth");
  CHECK(p.converged);
  CHECK(p.rms_residual_m < 0.03f);
  CHECK(mm > 3000.0);
}

TEST_CASE("merge/yaw_search_says_ambiguous_in_a_symmetric_room") {
  // A square room has four equally good yaws. The search finds them all; the
  // honest answer is "ask the operator", not a transform.
  Rng rng(0xA13BEEFull);
  std::vector<Pt> pts;
  sample_plane(&pts, 2, 0.0, 0.0, 12.0, 0.0, 12.0, 0.20, &rng);
  sample_plane(&pts, 2, 3.0, 0.0, 12.0, 0.0, 12.0, 0.25, &rng);
  sample_plane(&pts, 1, 0.0, 0.0, 12.0, 0.0, 3.0, 0.16, &rng);
  sample_plane(&pts, 1, 12.0, 0.0, 12.0, 0.0, 3.0, 0.16, &rng);
  sample_plane(&pts, 0, 0.0, 0.0, 12.0, 0.0, 3.0, 0.16, &rng);
  sample_plane(&pts, 0, 12.0, 0.0, 12.0, 0.0, 3.0, 0.16, &rng);

  double centre_from_world[16];
  make_pose(0.0, -6.0, -6.0, 0.0, centre_from_world);
  double rot[16], src_from_world[16];
  make_pose(37.0, 0.0, 0.0, 0.0, rot);
  se3::mat4_mul(rot, centre_from_world, src_from_world);

  std::vector<PointVertex> target, source;
  for (const Pt& p : pts) {
    PointVertex v{};
    v.x = static_cast<float>(p[0]);
    v.y = static_cast<float>(p[1]);
    v.z = static_cast<float>(p[2]);
    v.a = 255;
    target.push_back(v);
    double q[3];
    se3::mat4_apply(src_from_world, p.data(), q);
    PointVertex w = v;
    w.x = static_cast<float>(q[0]);
    w.y = static_cast<float>(q[1]);
    w.z = static_cast<float>(q[2]);
    source.push_back(w);
  }

  const YawSearchResult r =
      yaw_translation_search(span_of(source), span_of(target), YawSearchConfig{});
  MESSAGE("symmetric room: best yaw " << r.yaw_deg << " overlap " << r.overlap << ", runner-up "
                                      << r.runner_up_yaw_deg << " overlap " << r.runner_up_overlap
                                      << ", margin " << r.margin);
  CHECK(r.overlap > 0.7);          // it does find the room
  CHECK(r.margin < 0.05);          // and it cannot tell which way round
  CHECK(r.ambiguous);
  CHECK_FALSE(r.ok);
  // The four-fold symmetry, measured: the runner-up is ~90 degrees away.
  const double d = std::fabs(r.runner_up_yaw_deg - r.yaw_deg);
  CHECK(std::fabs(std::fmod(d + 360.0, 90.0)) < 6.0);
}

// --- 6. build, provenance, dedup, determinism ------------------------------

namespace {

void run_full_merge(MergeProject* proj, const Fixture& f, MergeResult* out) {
  for (int i = 0; i < 3; ++i) REQUIRE(proj->add_session(f.input(i, true)).ok());
  REQUIRE(proj->align_georeferenced().ok());
  REQUIRE(proj->refine().ok());
  MergeOutputConfig cfg;
  cfg.dedup_voxel_m = 0.15;
  REQUIRE(proj->build(cfg, out).ok());
}

}  // namespace

TEST_CASE("merge/build_keeps_per_session_provenance_and_dedups") {
  Fixture f;
  MergeProject proj;
  MergeResult res;
  run_full_merge(&proj, f, &res);

  const MergeReport& rep = proj.report();
  const std::uint64_t in = rep.input_points;
  MESSAGE("merged " << in << " -> " << res.cloud.size() << " points (" << rep.dedup_dropped_points
                    << " intra-session, " << rep.priority_dropped_points
                    << " cross-session duplicates)");

  CHECK(in > 0);
  CHECK(res.cloud.size() < in);                 // dedup did something
  CHECK(res.cloud.size() > in / 3);             // and did not eat the building
  CHECK(rep.merged_points == res.cloud.size());
  CHECK(in == res.cloud.size() + rep.dedup_dropped_points + rep.priority_dropped_points);
  // Sessions 0-1 and 1-2 overlap by 4 m each, so somebody had to lose voxels.
  CHECK(rep.priority_dropped_points > 0);

  // The run table: contiguous, ascending, gap-free, and covering the cloud.
  REQUIRE(res.ranges.size() == 3);
  std::uint64_t cursor = 0;
  for (const SessionRange& r : res.ranges) {
    CHECK(r.first == cursor);
    CHECK(r.count > 0);
    cursor += r.count;
  }
  CHECK(cursor == res.cloud.size());
  CHECK(res.session_at(0) == res.ranges[0].session);
  CHECK(res.session_at(res.cloud.size() - 1) == res.ranges.back().session);
  CHECK(res.session_at(res.cloud.size()) == 0xFFFFFFFFu);

  // Provenance is real: each run's points still carry that session's tint.
  const std::uint8_t tint[3] = {240, 120, 30};
  for (const SessionRange& r : res.ranges) {
    for (std::uint64_t i = 0; i < r.count; i += 97) {
      CHECK(res.cloud[static_cast<std::size_t>(r.first + i)].r == tint[r.session]);
      CHECK(res.session_at(r.first + i) == r.session);
    }
  }

  // Every merged point is inside the building, in the anchor's frame.
  double anchor_from_world[16];
  se3::mat4_inverse_rigid(f.world_from_s[0], anchor_from_world);
  double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
  for (int cx = 0; cx < 2; ++cx) {
    for (int cy = 0; cy < 2; ++cy) {
      for (int cz = 0; cz < 2; ++cz) {
        const double c[3] = {cx ? 30.0 : 0.0, cy ? 12.0 : 0.0, cz ? 3.0 : 0.0};
        double q[3];
        se3::mat4_apply(anchor_from_world, c, q);
        for (int k = 0; k < 3; ++k) {
          lo[k] = std::min(lo[k], q[k]);
          hi[k] = std::max(hi[k], q[k]);
        }
      }
    }
  }
  for (int k = 0; k < 3; ++k) {
    CHECK(res.bounds_min[k] > lo[k] - 0.10);
    CHECK(res.bounds_max[k] < hi[k] + 0.10);
  }

  // The report is the C6 surface: every session summarized, every pair scored.
  REQUIRE(rep.sessions.size() == 3);
  for (const SessionSummary& s : rep.sessions) {
    CHECK(s.provenance_id == "wing-" + std::to_string(s.id));
    CHECK(s.georeferenced);
    CHECK(s.input_points > 0);
    CHECK(s.kept_points > 0);
    CHECK(s.point_count == s.kept_points);
  }
  CHECK(rep.pairs.size() == 3);
  CHECK(rep.pairs_refined == 2);
  CHECK(rep.pairs_low_overlap == 1);
  CHECK(rep.relaxed);  // three sessions => the pose graph ran
  CHECK(rep.graph.variables == 2);
  MESSAGE("relaxation: chi2 " << rep.graph.initial_chi2 << " -> " << rep.graph.final_chi2 << " in "
                              << rep.graph.iterations << " iterations; worst pair rms "
                              << rep.worst_rms_m * 1000.0 << " mm");
  CHECK(rep.worst_rms_m < 0.02f);
}

TEST_CASE("merge/priority_decides_who_owns_the_overlap") {
  Fixture f;
  MergeProject proj;
  SessionInput a = f.input(0, true);
  SessionInput b = f.input(1, true);
  b.priority = 0;  // wing-1 outranks wing-0
  a.priority = 5;
  REQUIRE(proj.add_session(a).ok());
  REQUIRE(proj.add_session(b).ok());
  REQUIRE(proj.align_georeferenced().ok());

  MergeOutputConfig cfg;
  cfg.dedup_voxel_m = 0.15;
  cfg.priority = MergePriority::kSessionPriority;
  MergeResult res;
  REQUIRE(proj.build(cfg, &res).ok());

  // The higher-priority session is laid down first and keeps the contested
  // voxels; the other one is the only session that loses points.
  REQUIRE(res.ranges.size() == 2);
  CHECK(res.ranges[0].session == 1);
  CHECK(res.ranges[1].session == 0);
  const MergeReport& rep = proj.report();
  CHECK(rep.sessions[1].dropped_duplicate_points == 0);
  CHECK(rep.sessions[0].dropped_duplicate_points > 0);
}

TEST_CASE("merge/publish_maps_pages_to_sessions") {
  Fixture f;
  MergeProject proj;
  MergeResult res;
  run_full_merge(&proj, f, &res);

  PageStoreConfig scfg;
  scfg.page_capacity = 4096;
  scfg.max_pages = 256;
  PageStore store(scfg);
  REQUIRE(proj.publish(&res, &store).ok());

  CHECK(store.total_points() == res.cloud.size());
  REQUIRE_FALSE(res.pages.empty());

  // Every published point is accounted for exactly once, and each page entry
  // agrees with the run table it came from.
  std::uint64_t total = 0;
  std::vector<std::uint64_t> per_session(3, 0);
  for (const PageProvenance& pp : res.pages) {
    total += pp.count;
    per_session[pp.session] += pp.count;
    const PageView v = store.page_view(pp.page);
    CHECK(v.valid());
    CHECK(pp.first + pp.count <= v.count);
  }
  CHECK(total == res.cloud.size());
  for (const SessionRange& r : res.ranges) CHECK(per_session[r.session] == r.count);

  // At most one page per session boundary can carry two sessions, and the
  // report says how many did.
  const MergeReport& rep = proj.report();
  CHECK(rep.pages_appended > 1);
  CHECK(rep.pages_shared <= 2);
  MESSAGE("published " << res.cloud.size() << " points into " << rep.pages_appended << " pages, "
                       << rep.pages_shared << " shared between two sessions");
}

TEST_CASE("merge/two_runs_are_bit_identical") {
  Fixture f;
  MergeProject p1, p2;
  MergeResult r1, r2;
  run_full_merge(&p1, f, &r1);
  run_full_merge(&p2, f, &r2);

  REQUIRE(r1.cloud.size() == r2.cloud.size());
  CHECK(std::memcmp(r1.cloud.data(), r2.cloud.data(), r1.cloud.size() * sizeof(PointVertex)) == 0);
  REQUIRE(r1.ranges.size() == r2.ranges.size());
  for (std::size_t i = 0; i < r1.ranges.size(); ++i) {
    CHECK(r1.ranges[i].session == r2.ranges[i].session);
    CHECK(r1.ranges[i].first == r2.ranges[i].first);
    CHECK(r1.ranges[i].count == r2.ranges[i].count);
  }
  const MergeReport& a = p1.report();
  const MergeReport& b = p2.report();
  REQUIRE(a.pairs.size() == b.pairs.size());
  for (std::size_t i = 0; i < a.pairs.size(); ++i) {
    CHECK(a.pairs[i].rms_residual_m == b.pairs[i].rms_residual_m);
    CHECK(a.pairs[i].overlap_fraction == b.pairs[i].overlap_fraction);
    CHECK(a.pairs[i].iterations == b.pairs[i].iterations);
    for (int k = 0; k < 16; ++k) CHECK(a.pairs[i].b_from_a[k] == b.pairs[i].b_from_a[k]);
  }
  CHECK(a.graph.final_chi2 == b.graph.final_chi2);
  CHECK(a.merged_points == b.merged_points);
  CHECK(a.priority_dropped_points == b.priority_dropped_points);
  for (std::size_t i = 0; i < a.sessions.size(); ++i) {
    for (int k = 0; k < 16; ++k) {
      CHECK(a.sessions[i].world_from_session[k] == b.sessions[i].world_from_session[k]);
    }
  }
}

// --- 7. plumbing ------------------------------------------------------------

TEST_CASE("merge/session_clouds_come_from_pages") {
  Fixture f;
  PageStore store;
  std::uint32_t appended = 0;
  REQUIRE(store.append(StreamId::kSlamMap, span_of(f.cloud[0]), 0, &appended).ok());
  CHECK(appended == f.cloud[0].size());

  SessionCloud sc;
  REQUIRE(collect_pages(store, StreamId::kSlamMap, &sc).ok());
  CHECK(sc.point_count() == f.cloud[0].size());
  CHECK_FALSE(sc.empty());

  // A stream with nothing on it is kNotFound, not an empty success.
  SessionCloud other;
  CHECK_FALSE(collect_pages(store, StreamId::kLidarD6, &other).ok());

  MergeProject proj;
  SessionInput in;
  in.provenance_id = "from-pages";
  in.cloud = sc;
  REQUIRE(proj.add_session(in).ok());
  CHECK(proj.session(0).point_count() == f.cloud[0].size());

  // Duplicate provenance ids are refused: the report is keyed by them.
  SessionInput dup = in;
  CHECK(proj.add_session(dup).error() == ScanError::kAlreadyExists);

  // An empty cloud is refused too — the usual cause is a caller handing over
  // a span into a vector that has already gone out of scope.
  SessionInput empty;
  empty.provenance_id = "nothing";
  CHECK(proj.add_session(empty).error() == ScanError::kInvalidArgument);
}

TEST_CASE("merge/keyframes_move_into_the_merged_frame") {
  Fixture f;
  MergeProject proj;
  SessionInput in0 = f.input(0, true);
  SessionInput in1 = f.input(1, true);
  SessionKeyframe kf;
  kf.t_ns = 12345;
  kf.p[0] = 1.0;
  kf.p[1] = 2.0;
  kf.p[2] = 0.0;
  const double yaw = 20.0 * se3::kDegToRad;
  kf.q[2] = std::sin(yaw * 0.5);
  kf.q[3] = std::cos(yaw * 0.5);
  in1.keyframes.push_back(kf);
  REQUIRE(proj.add_session(in0).ok());
  REQUIRE(proj.add_session(in1).ok());
  REQUIRE(proj.align_georeferenced().ok());

  std::vector<SessionKeyframe> world;
  REQUIRE(proj.keyframes_in_world(1, &world).ok());
  REQUIRE(world.size() == 1);
  CHECK(world[0].t_ns == 12345);

  double expect[16], got[16], local[16];
  se3::mat4_from_quat_pos(kf.q, kf.p, local);
  se3::mat4_mul(proj.session(1).world_from_session, local, expect);
  se3::mat4_from_quat_pos(world[0].q, world[0].p, got);
  double mm = 0.0, deg = 0.0;
  transform_error_mm_deg(got, expect, &mm, &deg);
  CHECK(mm < 1e-6);
  CHECK(deg < 1e-9);
  CHECK(proj.report().sessions[1].keyframes == 1);
}

// The measured basis for MergeIcpConfig's 0.5 m correspondence gate
// (docs/A13-merge.md §4): the SAME, correct, alignment reports a 5x larger
// RMS under A7's 1.0 m gate, entirely from correspondences past a surface
// edge. A number an operator is shown has to mean something.
TEST_CASE("merge/icp_gate_choice_is_measured_not_preferred") {
  Fixture f;
  double truth[16], inv0[16];
  se3::mat4_inverse_rigid(f.world_from_s[0], inv0);
  se3::mat4_mul(inv0, f.world_from_s[1], truth);

  double rms_at[2] = {0.0, 0.0};
  const double gates[2] = {1.0, 0.5};
  for (int i = 0; i < 2; ++i) {
    MergeIcpConfig c;
    c.icp.max_correspondence_m = gates[i];
    c.icp.target_cell_m = gates[i];
    c.coarse_iterations = 0;  // measure the gate, not the anneal
    c.max_iterations = 1;
    const PairIcpResult r = refine_pair(span_of(f.cloud[1]), span_of(f.cloud[0]), truth, c);
    rms_at[i] = r.rms_before_m;
    MESSAGE("gate " << gates[i] << " m at the TRUE alignment: rms " << r.rms_before_m * 1000.0
                    << " mm, mean |r| " << r.trace.front().fitness_m * 1000.0 << " mm, "
                    << r.inliers << " correspondences");
  }
  CHECK(rms_at[1] < 0.5 * rms_at[0]);
  CHECK(rms_at[1] < 0.015);  // ~2x the 5 mm range noise, which is the floor
}

TEST_CASE("merge/align_source_labels_are_stable") {
  CHECK(std::string(to_string(AlignSource::kNone)) == "unaligned");
  CHECK(std::string(to_string(AlignSource::kAnchor)) == "anchor");
  CHECK(std::string(to_string(AlignSource::kGeoreferenced)) == "georeferenced");
  CHECK(std::string(to_string(AlignSource::kManual)) == "manual");
  CHECK(std::string(to_string(AlignSource::kYawSearch)) == "yaw-search");
  CHECK(std::string(to_string(AlignSource::kIcp)) == "icp");
  CHECK(std::string(to_string(AlignSource::kRelaxed)) == "relaxed");
}
