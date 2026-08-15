#include "app/SyntheticBuilding.h"

#include <cmath>
#include <cstdint>

// Ported from engine/tests/test_plan.cpp's emit_face/emit_box_sides/
// make_building (BuildOpts default). See SyntheticBuilding.h for why this is
// a port and not a shared header.
namespace lidarscan {
namespace {

using scanengine::PointVertex;

constexpr double kSouthY = 0.00;
constexpr double kNorthY = 5.00;
constexpr double kWestX = 0.00;
constexpr double kEastX = 8.00;
constexpr double kPartLoY = 1.40;
constexpr double kPartHiY = 1.55;
constexpr double kVpartLoX = 3.85;
constexpr double kVpartHiX = 4.00;

constexpr double kDoorAx0 = 1.50, kDoorAx1 = 2.40;
constexpr double kDoorBx0 = 5.50, kDoorBx1 = 6.40;
constexpr double kWinX0 = 1.00, kWinX1 = 2.10;
constexpr double kWinSillZ = 0.90, kWinHeadZ = 2.10;

constexpr double kFaceNoiseSigma = 0.02;

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  double gauss() {
    double u1 = uniform();
    if (u1 < 1e-12) u1 = 1e-12;
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

struct Hole {
  double t0, t1, z0, z1;
};

struct Face {
  double x0, y0, x1, y1;
  double z0 = 0.05;
  double z1 = 2.40;
  double sigma = kFaceNoiseSigma;
  std::vector<Hole> holes;
};

// A plain function rather than Face{x0,y0,x1,y1} aggregate-init: leaving the
// trailing default-member-initialized fields (z0/z1/sigma/holes) out of a
// brace-init list is exactly what -Wmissing-field-initializers warns about,
// even though they resolve to their in-class defaults either way.
Face wallFace(double x0, double y0, double x1, double y1) {
  Face f;
  f.x0 = x0; f.y0 = y0; f.x1 = x1; f.y1 = y1;
  return f;
}

void emit_face(const Face& f, Rng* rng, std::vector<PointVertex>* out) {
  const double dx = f.x1 - f.x0;
  const double dy = f.y1 - f.y0;
  const double len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0.0) return;
  const double ux = dx / len, uy = dy / len;
  const double nx = -uy, ny = ux;
  const int nt = static_cast<int>(len / 0.02);
  const int nz = static_cast<int>((f.z1 - f.z0) / 0.04);
  for (int it = 0; it <= nt; ++it) {
    const double t = static_cast<double>(it) * 0.02;
    for (int iz = 0; iz <= nz; ++iz) {
      const double z = f.z0 + static_cast<double>(iz) * 0.04;
      bool in_hole = false;
      for (const auto& h : f.holes) {
        if (t > h.t0 - 1e-9 && t < h.t1 + 1e-9 && z > h.z0 - 1e-9 && z < h.z1 + 1e-9) {
          in_hole = true;
          break;
        }
      }
      if (in_hole) continue;
      const double e = rng->gauss() * f.sigma;
      const double a = rng->gauss() * 0.005;
      PointVertex p{};
      p.x = static_cast<float>(f.x0 + ux * (t + a) + nx * e);
      p.y = static_cast<float>(f.y0 + uy * (t + a) + ny * e);
      p.z = static_cast<float>(z + rng->gauss() * 0.005);
      p.r = p.g = p.b = 200;
      p.a = 255;
      out->push_back(p);
    }
  }
}

void emit_box_sides(double x0, double y0, double x1, double y1, double zb, double zt, Rng* rng,
                    std::vector<PointVertex>* out) {
  Face f;
  f.z0 = zb;
  f.z1 = zt;
  f.sigma = 0.004;
  f.x0 = x0; f.y0 = y0; f.x1 = x1; f.y1 = y0; emit_face(f, rng, out);
  f.x0 = x1; f.y0 = y0; f.x1 = x1; f.y1 = y1; emit_face(f, rng, out);
  f.x0 = x1; f.y0 = y1; f.x1 = x0; f.y1 = y1; emit_face(f, rng, out);
  f.x0 = x0; f.y0 = y1; f.x1 = x0; f.y1 = y0; emit_face(f, rng, out);
}

}  // namespace

std::vector<PointVertex> buildSyntheticBuildingPoints() {
  Rng rng(0xA12F100Full);
  std::vector<PointVertex> pts;

  Face south = wallFace(kWestX, kSouthY, kEastX, kSouthY);
  emit_face(south, &rng, &pts);
  Face east = wallFace(kEastX, kSouthY, kEastX, kNorthY);
  emit_face(east, &rng, &pts);
  Face north = wallFace(kEastX, kNorthY, kWestX, kNorthY);
  north.holes.push_back(Hole{kEastX - kWinX1, kEastX - kWinX0, kWinSillZ, kWinHeadZ});
  emit_face(north, &rng, &pts);
  Face west = wallFace(kWestX, kNorthY, kWestX, kSouthY);
  emit_face(west, &rng, &pts);

  Face part_lo = wallFace(kWestX, kPartLoY, kEastX, kPartLoY);
  part_lo.holes.push_back(Hole{kDoorAx0, kDoorAx1, 0.0, 2.10});
  part_lo.holes.push_back(Hole{kDoorBx0, kDoorBx1, 0.0, 2.10});
  emit_face(part_lo, &rng, &pts);
  Face part_hi = wallFace(kWestX, kPartHiY, kEastX, kPartHiY);
  part_hi.holes = part_lo.holes;
  emit_face(part_hi, &rng, &pts);

  Face vp_lo = wallFace(kVpartLoX, kPartHiY, kVpartLoX, kNorthY);
  emit_face(vp_lo, &rng, &pts);
  Face vp_hi = wallFace(kVpartHiX, kPartHiY, kVpartHiX, kNorthY);
  emit_face(vp_hi, &rng, &pts);

  // Clutter (BuildOpts default: clutter = true).
  Face table = wallFace(5.0, 2.5, 6.2, 2.5);
  table.z0 = 0.72;
  table.z1 = 0.76;
  table.sigma = 0.004;
  emit_face(table, &rng, &pts);
  emit_box_sides(5.8, 3.4, 6.3, 3.8, 0.05, 1.85, &rng, &pts);
  for (int k = 0; k < 400; ++k) {
    PointVertex p{};
    p.x = static_cast<float>(rng.uniform() * 7.8 + 0.1);
    p.y = static_cast<float>(rng.uniform() * 4.8 + 0.1);
    p.z = static_cast<float>(1.0 + rng.uniform() * 0.5);
    p.r = p.g = p.b = 120;
    p.a = 255;
    pts.push_back(p);
  }
  for (int ix = 0; ix <= 80; ++ix) {
    for (int iy = 0; iy <= 50; ++iy) {
      PointVertex p{};
      p.x = static_cast<float>(ix) * 0.1f;
      p.y = static_cast<float>(iy) * 0.1f;
      p.z = 0.f;
      p.r = p.g = p.b = 80;
      p.a = 255;
      pts.push_back(p);
      p.z = 2.5f;
      pts.push_back(p);
    }
  }

  return pts;
}

}  // namespace lidarscan
