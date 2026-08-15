#include "app/MergeFixture.h"

#include <algorithm>
#include <cmath>

#include "scanengine/poses/se3.h"

namespace lidarscan {
namespace {

using scanengine::PointVertex;
using scanengine::Span;
using scanengine::crs::Geodetic;
using scanengine::crs::make_enu_frame;
using scanengine::merge::SessionGeoref;

// xorshift64 + Box-Muller, deliberately NOT <random> — same reason
// engine/tests/test_merge.cpp gives: the standard does not specify the output
// of its distributions, so five CI legs (and this app's own build) would
// disagree bit-for-bit on the "same" fixture otherwise.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next_u64() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() { return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0); }
  double gauss() {
    const double u1 = uniform() + 1e-12, u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

using Pt = std::array<double, 3>;

void samplePlane(std::vector<Pt>* out, int axis, double fixed, double u0, double u1, double v0,
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

// The building, in the world (== reference ENU) frame. Independently
// jittered per call, so two sessions are two independent samplings of the
// same surfaces, not the same points twice.
void buildingSurfaces(std::vector<Pt>* out, Rng* rng) {
  samplePlane(out, 2, 0.0, 0.0, 30.0, 0.0, 12.0, 0.20, rng);   // floor
  samplePlane(out, 2, 3.0, 0.0, 30.0, 0.0, 12.0, 0.25, rng);   // ceiling
  samplePlane(out, 1, 0.0, 0.0, 30.0, 0.0, 3.0, 0.16, rng);    // long wall y=0
  samplePlane(out, 1, 12.0, 0.0, 30.0, 0.0, 3.0, 0.16, rng);   // long wall y=12
  samplePlane(out, 0, 0.0, 0.0, 12.0, 0.0, 3.0, 0.16, rng);    // end wall x=0
  samplePlane(out, 0, 30.0, 0.0, 12.0, 0.0, 3.0, 0.16, rng);   // end wall x=30
  samplePlane(out, 0, 10.0, 0.0, 8.0, 0.0, 3.0, 0.16, rng);    // partition
  samplePlane(out, 0, 20.0, 0.0, 8.0, 0.0, 3.0, 0.16, rng);    // partition
}

void makePose(double yawDeg, double x, double y, double z, double m[16]) {
  const double c = std::cos(yawDeg * scanengine::se3::kDegToRad);
  const double s = std::sin(yawDeg * scanengine::se3::kDegToRad);
  const double R[9] = {c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0};
  const double t[3] = {x, y, z};
  scanengine::se3::mat4_from_rt(R, t, m);
}

std::vector<PointVertex> sessionCloud(std::uint64_t seed, double xlo, double xhi,
                                      const double worldFromSession[16], double noiseM,
                                      std::uint8_t tint) {
  Rng rng(seed);
  std::vector<Pt> pts;
  buildingSurfaces(&pts, &rng);
  double sessionFromWorld[16];
  scanengine::se3::mat4_inverse_rigid(worldFromSession, sessionFromWorld);
  std::vector<PointVertex> out;
  out.reserve(pts.size() / 2);
  for (const Pt& p : pts) {
    if (p[0] < xlo || p[0] > xhi) continue;
    const double w[3] = {p[0] + rng.gauss() * noiseM, p[1] + rng.gauss() * noiseM,
                         p[2] + rng.gauss() * noiseM};
    double l[3];
    scanengine::se3::mat4_apply(sessionFromWorld, w, l);
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

// ENU_to <- ENU_from computed through GEODETIC coordinates rather than
// align.h's ECEF rotation composition — two independent routes to the same
// rigid transform, matching engine/tests/test_merge.cpp's own cross-check.
void enuFromEnuNumeric(const scanengine::crs::EnuFrame& to, const scanengine::crs::EnuFrame& from,
                       double m[16]) {
  auto map = [&](double e, double n, double u, double out[3]) {
    const Geodetic g = scanengine::crs::enu_to_geodetic(from, scanengine::crs::Enu{e, n, u});
    const scanengine::crs::Enu q = scanengine::crs::geodetic_to_enu(to, g);
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
  scanengine::se3::matrix_to_quat(R, q);
  scanengine::se3::quat_to_matrix(q, R);
  scanengine::se3::mat4_from_rt(R, o, m);
}

SessionGeoref makeGeoref(const scanengine::crs::EnuFrame& sessionEnu,
                         const scanengine::crs::EnuFrame& worldEnu,
                         const double worldFromSession[16], double sigmaHM, int epsg) {
  SessionGeoref g;
  g.valid = true;
  g.enu = sessionEnu;
  g.epsg = epsg;
  g.solution.converged = true;
  g.solution.scale = 1.0;
  g.solution.horizontal_sigma_m = sigmaHM;
  g.solution.vertical_sigma_m = sigmaHM * 1.6;
  double enuFromWorld[16];
  enuFromEnuNumeric(sessionEnu, worldEnu, enuFromWorld);
  scanengine::se3::mat4_mul(enuFromWorld, worldFromSession, g.solution.global_from_local);
  return g;
}

// The 4 physical features an operator could click: non-collinear, spread
// across the shared overlap volume between session 0 and session 1.
constexpr double kPickWorld[4][3] = {
    {10.0, 0.0, 0.0}, {10.0, 8.0, 3.0}, {12.5, 12.0, 0.0}, {9.5, 4.0, 3.0}};

}  // namespace

MergeFixture::MergeFixture() {
  makePose(0.0, 2.0, 6.0, 1.40, world_from_s_[0]);
  makePose(35.0, 15.0, 5.0, 1.42, world_from_s_[1]);
  makePose(-70.0, 26.0, 7.0, 1.38, world_from_s_[2]);
  const double xlo[3] = {0.0, 9.0, 18.0};
  const double xhi[3] = {13.0, 22.0, 30.0};
  const std::uint8_t tint[3] = {240, 120, 30};
  for (int i = 0; i < 3; ++i) {
    cloud_[i] = sessionCloud(0x51ED0001ull + static_cast<std::uint64_t>(i) * 7919ull, xlo[i], xhi[i],
                             world_from_s_[i], 0.005, tint[i]);
  }
  // One CRS (WGS84 / UTM 32N territory), three ENU origins: the sessions were
  // anchored on three different days, hundreds of metres apart.
  world_enu_ = make_enu_frame(Geodetic{47.376900, 8.541700, 408.0});
  session_enu_[0] = make_enu_frame(Geodetic{47.376900, 8.541700, 408.0});
  session_enu_[1] = make_enu_frame(Geodetic{47.378700, 8.543900, 411.5});
  session_enu_[2] = make_enu_frame(Geodetic{47.375100, 8.538200, 405.2});
}

std::vector<MergeFixtureSession> MergeFixture::sessions(bool withGeoref) const {
  std::vector<MergeFixtureSession> out;
  for (int i = 0; i < 3; ++i) {
    MergeFixtureSession s;
    s.provenanceId = QString("wing-%1").arg(i);
    s.cloud = cloud_[i];
    if (withGeoref) {
      s.has_georef = true;
      s.georef = makeGeoref(session_enu_[i], world_enu_, world_from_s_[i], 0.02, 32632);
    }
    out.push_back(std::move(s));
  }
  return out;
}

void MergeFixture::pick(int session, std::size_t index, double noiseM, std::uint64_t seed,
                        float sourceLocalOut[3], float targetLocalOut[3]) const {
  Rng rng(seed + index * 0x9E37u);
  double sFromW[16], anchorFromW[16];
  scanengine::se3::mat4_inverse_rigid(world_from_s_[session], sFromW);
  scanengine::se3::mat4_inverse_rigid(world_from_s_[0], anchorFromW);
  double a[3], b[3];
  scanengine::se3::mat4_apply(sFromW, kPickWorld[index], a);
  scanengine::se3::mat4_apply(anchorFromW, kPickWorld[index], b);
  for (int k = 0; k < 3; ++k) {
    sourceLocalOut[k] = static_cast<float>(a[k] + rng.gauss() * noiseM);
    targetLocalOut[k] = static_cast<float>(b[k] + rng.gauss() * noiseM);
  }
}

void MergeFixture::truthWorldFromSession(int session, double out[16]) const {
  double inv0[16];
  scanengine::se3::mat4_inverse_rigid(world_from_s_[0], inv0);
  scanengine::se3::mat4_mul(inv0, world_from_s_[session], out);
}

}  // namespace lidarscan
