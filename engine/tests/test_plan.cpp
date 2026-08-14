// A12: floor-plan extraction (§3.6) — slice, RANSAC walls, snapping, openings,
// rooms, the editor API, and the DXF/PDF writers.
//
// The fixture is a synthetic two-room-plus-corridor building whose true
// geometry is written out in metres at the top of this file, sampled the way a
// scanner sees it: only the faces that are visible from inside, 2 cm of range
// noise on every face normal, furniture, and speckle. Every assertion is
// against those declared numbers — never against "whatever the extractor
// produced last time" — so a regression shows up as a wrong distance in
// metres, not as a diff.
//
// Both writers are checked by readers written FROM SCRATCH in this file (the
// A9 posture, docs/A9-export.md): the DXF reader is a group-code tokenizer
// that knows nothing about src/plan/dxf_writer.cpp, and the PDF reader
// recomputes every cross-reference offset from the bytes rather than trusting
// the writer's own bookkeeping, so a writer and a "reader" sharing the same
// mistake cannot agree their way past a test.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/plan/plan_editor.h"
#include "scanengine/plan/plan_model.h"
#include "scanengine/plan/plan_writers.h"

using namespace scanengine;
using namespace scanengine::plan;

namespace {

namespace fs = std::filesystem;

// ===========================================================================
// The synthetic building — TRUE geometry, in metres.
//
//   y=5.00  +-------[window 1.00..2.10]-------------------------+   north
//           |                        |                          |
//           |        room A          |         room B           |
//           |   x 0..3.85            |    x 4.00..8.00          |
//           |   y 1.55..5.00         |    y 1.55..5.00          |
//   y=1.55  +------------------------+--------------------------+
//   y=1.40  +--[door 1.50..2.40]--------[door 5.50..6.40]-------+   partition
//           |                   corridor  y 0..1.40             |
//   y=0.00  +--------------------------------------------------+   south
//          x=0                                                x=8
//
// Exterior walls: only the INNER face is scanned (a scanner inside a building
// never sees the outside of it), so the extractor gets a single-face wall and
// has to know not to inset the room polygon by an assumed thickness.
// Partitions: BOTH faces are scanned, 0.15 m apart, so their thickness is
// measured and the room polygons ARE inset by half of it.
// ===========================================================================

constexpr double kSouthY = 0.00;
constexpr double kNorthY = 5.00;
constexpr double kWestX = 0.00;
constexpr double kEastX = 8.00;
constexpr double kPartLoY = 1.40;  // corridor-side face
constexpr double kPartHiY = 1.55;  // room-side face
constexpr double kPartCenterY = 0.5 * (kPartLoY + kPartHiY);
constexpr double kVpartLoX = 3.85;  // room-A-side face
constexpr double kVpartHiX = 4.00;  // room-B-side face
constexpr double kVpartCenterX = 0.5 * (kVpartLoX + kVpartHiX);
constexpr double kPartThickness = kPartHiY - kPartLoY;  // 0.15

constexpr double kDoorAx0 = 1.50, kDoorAx1 = 2.40;  // 0.90 m
constexpr double kDoorBx0 = 5.50, kDoorBx1 = 6.40;  // 0.90 m
constexpr double kWinX0 = 1.00, kWinX1 = 2.10;      // 1.10 m
constexpr double kWinSillZ = 0.90, kWinHeadZ = 2.10;

constexpr double kCorridorArea = (kEastX - kWestX) * (kPartLoY - kSouthY);       // 11.2000
constexpr double kRoomAArea = (kVpartLoX - kWestX) * (kNorthY - kPartHiY);       // 13.2825
constexpr double kRoomBArea = (kEastX - kVpartHiX) * (kNorthY - kPartHiY);       // 13.8000

constexpr double kFaceNoiseSigma = 0.02;  // "realistic 2 cm noise"

// --- deterministic PRNG (the test's own; nothing here uses <random>) --------

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

// A rectangular hole in a face, in (along-face parameter, height).
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
    // Integer-stepped so a hole boundary that is a multiple of the step is
    // hit exactly and the generated gap is exactly the declared width.
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
      PointVertex p;
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

struct BuildOpts {
  bool clutter = true;
  bool shelf_against_wall = false;  // the documented failure mode
  bool diagonal_wall = false;       // a non-Manhattan wall at 30 degrees
  double rotate_deg = 0.0;
};

std::vector<PointVertex> make_building(const BuildOpts& bo) {
  Rng rng(0xA12F100Full);
  std::vector<PointVertex> pts;

  Face south{kWestX, kSouthY, kEastX, kSouthY};
  emit_face(south, &rng, &pts);
  Face east{kEastX, kSouthY, kEastX, kNorthY};
  emit_face(east, &rng, &pts);
  // Walked from (8,5) toward (0,5), so the window at x in [1.00, 2.10] is at
  // t in [8 - 2.10, 8 - 1.00].
  Face north{kEastX, kNorthY, kWestX, kNorthY};
  north.holes.push_back(Hole{kEastX - kWinX1, kEastX - kWinX0, kWinSillZ, kWinHeadZ});
  emit_face(north, &rng, &pts);
  Face west{kWestX, kNorthY, kWestX, kSouthY};
  emit_face(west, &rng, &pts);

  Face part_lo{kWestX, kPartLoY, kEastX, kPartLoY};
  part_lo.holes.push_back(Hole{kDoorAx0, kDoorAx1, 0.0, 2.10});
  part_lo.holes.push_back(Hole{kDoorBx0, kDoorBx1, 0.0, 2.10});
  emit_face(part_lo, &rng, &pts);
  Face part_hi{kWestX, kPartHiY, kEastX, kPartHiY};
  part_hi.holes = part_lo.holes;
  emit_face(part_hi, &rng, &pts);

  Face vp_lo{kVpartLoX, kPartHiY, kVpartLoX, kNorthY};
  emit_face(vp_lo, &rng, &pts);
  Face vp_hi{kVpartHiX, kPartHiY, kVpartHiX, kNorthY};
  emit_face(vp_hi, &rng, &pts);

  if (bo.clutter) {
    // A low table: entirely BELOW the 1.0-1.5 m band, so it must not appear
    // at all — and it sits inside the 0.35-0.80 m sill band, so it also
    // proves the sill check only looks near a wall.
    Face table{5.0, 2.5, 6.2, 2.5};
    table.z0 = 0.72;
    table.z1 = 0.76;
    table.sigma = 0.004;
    emit_face(table, &rng, &pts);
    // A tall cabinet, in the band, in the middle of room B. Every face is
    // shorter than min_wall_length_m, so it must be rejected as a wall.
    emit_box_sides(5.8, 3.4, 6.3, 3.8, 0.05, 1.85, &rng, &pts);
    // Speckle: one point per cell, below min_cell_points.
    for (int k = 0; k < 400; ++k) {
      PointVertex p;
      p.x = static_cast<float>(rng.uniform() * 7.8 + 0.1);
      p.y = static_cast<float>(rng.uniform() * 4.8 + 0.1);
      p.z = static_cast<float>(1.0 + rng.uniform() * 0.5);
      p.r = p.g = p.b = 120;
      p.a = 255;
      pts.push_back(p);
    }
    // Floor and ceiling: outside the band on both sides.
    for (int ix = 0; ix <= 80; ++ix) {
      for (int iy = 0; iy <= 50; ++iy) {
        PointVertex p;
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
  }
  if (bo.shelf_against_wall) {
    Face shelf{0.55, 2.0, 0.55, 3.4};
    shelf.z0 = 0.05;
    shelf.z1 = 1.80;
    shelf.sigma = 0.004;
    emit_face(shelf, &rng, &pts);
  }
  if (bo.diagonal_wall) {
    // A 30-degree partition across room B's corner: outside the +-7 degree
    // snap window, so it must survive unsnapped.
    const double len = 2.2;
    const double a = 30.0 * 3.14159265358979323846 / 180.0;
    Face diag{4.6, 2.2, 4.6 + len * std::cos(a), 2.2 + len * std::sin(a)};
    emit_face(diag, &rng, &pts);
  }
  if (bo.rotate_deg != 0.0) {
    const double a = bo.rotate_deg * 3.14159265358979323846 / 180.0;
    const double ca = std::cos(a), sa = std::sin(a);
    for (auto& p : pts) {
      const double x = p.x, y = p.y;
      p.x = static_cast<float>(ca * x - sa * y);
      p.y = static_cast<float>(sa * x + ca * y);
    }
  }
  return pts;
}

// Generating and extracting the fixture costs ~0.3 s; every case that wants
// the default building shares one.
const std::vector<PointVertex>& default_cloud() {
  static const std::vector<PointVertex> pts = make_building(BuildOpts{});
  return pts;
}

PlanInput input_of(const std::vector<PointVertex>& pts) {
  PlanInput in;
  in.points = Span<const PointVertex>(pts.data(), pts.size());
  return in;
}

const PlanModel& default_plan() {
  static const PlanModel m = [] {
    PlanModel out;
    const Status st = extract_floor_plan(input_of(default_cloud()), PlanOptions{}, &out);
    REQUIRE(st.ok());
    return out;
  }();
  return m;
}

// --- assertion helpers ------------------------------------------------------

double deg(double rad) { return rad * 180.0 / 3.14159265358979323846; }

// Distance from `p` to the segment [a,b].
double point_segment_distance(Vec2 p, Vec2 a, Vec2 b) {
  const Vec2 ab = b - a;
  const double L2 = dot(ab, ab);
  if (L2 <= 0.0) return distance(p, a);
  double t = dot(p - a, ab) / L2;
  t = std::max(0.0, std::min(1.0, t));
  return distance(p, a + ab * t);
}

struct ExpectedWall {
  const char* name;
  Vec2 a;
  Vec2 b;
  bool paired;      // both faces scanned -> thickness must be measured
  double coverage;  // minimum acceptable occupied fraction
};

// A wall matches when BOTH its endpoints are within `tol` of the expected
// endpoints (in either order) — which is a much stronger claim than "a line
// exists near here", because it pins the corner trimming too.
const WallSegment* find_wall(const PlanModel& m, const ExpectedWall& e, double tol) {
  for (const auto& w : m.walls) {
    if ((distance(w.a, e.a) <= tol && distance(w.b, e.b) <= tol) ||
        (distance(w.a, e.b) <= tol && distance(w.b, e.a) <= tol)) {
      return &w;
    }
  }
  return nullptr;
}

std::vector<ExpectedWall> expected_walls() {
  return {
      {"south", {kWestX, kSouthY}, {kEastX, kSouthY}, false, 0.95},
      {"north", {kWestX, kNorthY}, {kEastX, kNorthY}, false, 0.80},
      {"west", {kWestX, kSouthY}, {kWestX, kNorthY}, false, 0.95},
      {"east", {kEastX, kSouthY}, {kEastX, kNorthY}, false, 0.95},
      {"partition-h", {kWestX, kPartCenterY}, {kEastX, kPartCenterY}, true, 0.70},
      {"partition-v", {kVpartCenterX, kPartCenterY}, {kVpartCenterX, kNorthY}, true, 0.95},
  };
}

const Room* room_nearest(const PlanModel& m, Vec2 centroid, double tol) {
  for (const auto& r : m.rooms) {
    if (distance(r.centroid, centroid) <= tol) return &r;
  }
  return nullptr;
}

std::string temp_path(const char* tag, const char* ext) {
  static std::atomic<long long> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                     (std::string("scanengine_plan_test_") + tag + "_" + std::to_string(now) +
                      "_" + std::to_string(id) + "." + ext);
  std::error_code ec;
  fs::remove(p, ec);
  return p.string();
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ===========================================================================
// An independent ASCII DXF reader.
//
// It knows one rule: a DXF is alternating lines of (group code, value). It
// shares no code, no constants and no helper with src/plan/dxf_writer.cpp.
// ===========================================================================

struct DxfPair {
  int code;
  std::string value;
};

std::vector<DxfPair> dxf_tokenize(const std::string& text) {
  std::vector<DxfPair> out;
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      if (start < text.size()) lines.push_back(text.substr(start));
      break;
    }
    std::string ln = text.substr(start, nl - start);
    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
    lines.push_back(std::move(ln));
    start = nl + 1;
  }
  for (std::size_t k = 0; k + 1 < lines.size(); k += 2) {
    DxfPair p;
    p.code = std::atoi(lines[k].c_str());
    p.value = lines[k + 1];
    out.push_back(std::move(p));
  }
  return out;
}

// A DXF number, parsed WITHOUT strtod's locale dependence: this reader has to
// be able to catch the writer emitting "1,234" under a de_DE locale, and
// strtod under that same locale would happily accept it.
double dxf_number(const std::string& s) {
  bool neg = false;
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    neg = s[i] == '-';
    ++i;
  }
  double whole = 0.0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    whole = whole * 10.0 + static_cast<double>(s[i] - '0');
  }
  double frac = 0.0, scale = 0.1;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      frac += static_cast<double>(s[i] - '0') * scale;
      scale *= 0.1;
    }
  }
  const double v = whole + frac;
  return neg ? -v : v;
}

struct DxfEntity {
  std::string type;
  std::string layer;
  std::vector<Vec2> vertices;  // POLYLINE vertices, or LINE's two points
  bool closed = false;
  std::string text;
  double height = 0.0;
  bool sealed = false;  // a POLYLINE that saw its SEQEND
};

struct DxfDoc {
  std::string acadver;
  Vec2 extmin{}, extmax{};
  std::vector<std::string> layers;
  std::vector<std::string> styles;
  std::vector<DxfEntity> entities;
  bool saw_eof = false;
  std::vector<std::string> sections;

  bool has_layer(const std::string& n) const {
    return std::find(layers.begin(), layers.end(), n) != layers.end();
  }
  std::vector<const DxfEntity*> on_layer(const std::string& n, const std::string& type) const {
    std::vector<const DxfEntity*> out;
    for (const auto& e : entities) {
      if (e.layer == n && e.type == type) out.push_back(&e);
    }
    return out;
  }
};

DxfDoc dxf_parse(const std::string& text) {
  DxfDoc doc;
  const std::vector<DxfPair> t = dxf_tokenize(text);
  std::string section;
  bool in_tables = false;
  std::string table_name;
  std::string pending_header_var;
  std::size_t i = 0;

  while (i < t.size()) {
    const DxfPair& p = t[i];
    if (p.code == 0 && p.value == "SECTION") {
      section.clear();
      if (i + 1 < t.size() && t[i + 1].code == 2) {
        section = t[i + 1].value;
        doc.sections.push_back(section);
        i += 2;
        in_tables = section == "TABLES";
        continue;
      }
    }
    if (p.code == 0 && p.value == "ENDSEC") {
      section.clear();
      in_tables = false;
      ++i;
      continue;
    }
    if (p.code == 0 && p.value == "EOF") {
      doc.saw_eof = true;
      ++i;
      continue;
    }

    if (section == "HEADER") {
      if (p.code == 9) {
        pending_header_var = p.value;
      } else if (pending_header_var == "$ACADVER" && p.code == 1) {
        doc.acadver = p.value;
      } else if (pending_header_var == "$EXTMIN" && p.code == 10) {
        doc.extmin.x = dxf_number(p.value);
      } else if (pending_header_var == "$EXTMIN" && p.code == 20) {
        doc.extmin.y = dxf_number(p.value);
      } else if (pending_header_var == "$EXTMAX" && p.code == 10) {
        doc.extmax.x = dxf_number(p.value);
      } else if (pending_header_var == "$EXTMAX" && p.code == 20) {
        doc.extmax.y = dxf_number(p.value);
      }
      ++i;
      continue;
    }

    if (in_tables) {
      if (p.code == 0 && p.value == "TABLE" && i + 1 < t.size() && t[i + 1].code == 2) {
        table_name = t[i + 1].value;
        i += 2;
        continue;
      }
      if (p.code == 0 && p.value == "ENDTAB") {
        table_name.clear();
        ++i;
        continue;
      }
      if (p.code == 0 && p.value == "LAYER" && i + 1 < t.size() && t[i + 1].code == 2) {
        doc.layers.push_back(t[i + 1].value);
        i += 2;
        continue;
      }
      if (p.code == 0 && p.value == "STYLE" && i + 1 < t.size() && t[i + 1].code == 2) {
        doc.styles.push_back(t[i + 1].value);
        i += 2;
        continue;
      }
      ++i;
      continue;
    }

    if (section == "ENTITIES" && p.code == 0) {
      const std::string& kind = p.value;
      if (kind == "POLYLINE" || kind == "LINE" || kind == "TEXT") {
        DxfEntity e;
        e.type = kind;
        double x = 0.0, y = 0.0, x2 = 0.0, y2 = 0.0;
        bool have2 = false;
        ++i;
        for (; i < t.size() && t[i].code != 0; ++i) {
          switch (t[i].code) {
            case 8: e.layer = t[i].value; break;
            case 70: e.closed = (std::atoi(t[i].value.c_str()) & 1) != 0; break;
            case 10: x = dxf_number(t[i].value); break;
            case 20: y = dxf_number(t[i].value); break;
            case 11: x2 = dxf_number(t[i].value); have2 = true; break;
            case 21: y2 = dxf_number(t[i].value); break;
            case 40: e.height = dxf_number(t[i].value); break;
            case 1: e.text = t[i].value; break;
            default: break;
          }
        }
        if (kind == "LINE") {
          e.vertices.push_back(Vec2{x, y});
          e.vertices.push_back(Vec2{x2, y2});
        } else if (kind == "TEXT") {
          e.vertices.push_back(Vec2{x, y});
          (void)have2;
        }
        doc.entities.push_back(std::move(e));
        continue;
      }
      if (kind == "VERTEX") {
        double x = 0.0, y = 0.0;
        std::string layer;
        ++i;
        for (; i < t.size() && t[i].code != 0; ++i) {
          if (t[i].code == 10) x = dxf_number(t[i].value);
          if (t[i].code == 20) y = dxf_number(t[i].value);
          if (t[i].code == 8) layer = t[i].value;
        }
        REQUIRE_FALSE(doc.entities.empty());
        DxfEntity& owner = doc.entities.back();
        CHECK(owner.type == "POLYLINE");
        CHECK(owner.layer == layer);  // a VERTEX must repeat its owner's layer
        CHECK_FALSE(owner.sealed);
        owner.vertices.push_back(Vec2{x, y});
        continue;
      }
      if (kind == "SEQEND") {
        REQUIRE_FALSE(doc.entities.empty());
        doc.entities.back().sealed = true;
        ++i;
        for (; i < t.size() && t[i].code != 0; ++i) {
        }
        continue;
      }
    }
    ++i;
  }
  return doc;
}

// ===========================================================================
// An independent PDF structural reader.
// ===========================================================================

struct PdfDoc {
  bool header_ok = false;
  bool eof_ok = false;
  std::size_t startxref = 0;
  int size = 0;
  int root_obj = 0;
  std::vector<std::size_t> offsets;  // 1-based objects
  std::map<int, std::string> objects;
  std::string content;               // object 4's decoded stream
  double media[4] = {0, 0, 0, 0};
  std::vector<std::string> problems;
};

std::size_t find_last(const std::string& s, const std::string& what) {
  const std::size_t p = s.rfind(what);
  return p == std::string::npos ? 0 : p;
}

PdfDoc pdf_parse(const std::string& bytes) {
  PdfDoc d;
  d.header_ok = bytes.rfind("%PDF-1.", 0) == 0;
  d.eof_ok = bytes.size() > 6 && bytes.compare(bytes.size() - 6, 6, "%%EOF\n") == 0;

  const std::size_t sx = find_last(bytes, "startxref");
  if (sx == 0) {
    d.problems.push_back("no startxref");
    return d;
  }
  std::size_t p = sx + 9;
  while (p < bytes.size() && (bytes[p] == '\r' || bytes[p] == '\n' || bytes[p] == ' ')) ++p;
  std::size_t off = 0;
  while (p < bytes.size() && bytes[p] >= '0' && bytes[p] <= '9') {
    off = off * 10 + static_cast<std::size_t>(bytes[p] - '0');
    ++p;
  }
  d.startxref = off;
  if (off >= bytes.size() || bytes.compare(off, 4, "xref") != 0) {
    d.problems.push_back("startxref does not point at 'xref'");
    return d;
  }

  // "xref\n0 N\n" then N entries of EXACTLY 20 bytes each.
  std::size_t q = off + 5;
  std::size_t first = 0, count = 0;
  while (q < bytes.size() && bytes[q] >= '0' && bytes[q] <= '9') {
    first = first * 10 + static_cast<std::size_t>(bytes[q] - '0');
    ++q;
  }
  if (q < bytes.size() && bytes[q] == ' ') ++q;
  while (q < bytes.size() && bytes[q] >= '0' && bytes[q] <= '9') {
    count = count * 10 + static_cast<std::size_t>(bytes[q] - '0');
    ++q;
  }
  if (q < bytes.size() && bytes[q] == '\n') ++q;
  if (first != 0) d.problems.push_back("xref subsection does not start at object 0");

  for (std::size_t k = 0; k < count; ++k) {
    const std::size_t base = q + k * 20;
    if (base + 20 > bytes.size()) {
      d.problems.push_back("xref table is truncated");
      return d;
    }
    const std::string entry = bytes.substr(base, 20);
    if (entry.size() != 20 || entry[10] != ' ' || entry[16] != ' ' || entry[18] != ' ' ||
        entry[19] != '\n') {
      d.problems.push_back("xref entry " + std::to_string(k) + " is not 20 bytes wide");
      continue;
    }
    if (k == 0) {
      if (entry.compare(0, 18, "0000000000 65535 f") != 0) {
        d.problems.push_back("object 0 is not the free-list head");
      }
      continue;
    }
    if (entry[17] != 'n') d.problems.push_back("object " + std::to_string(k) + " is not 'n'");
    std::size_t o = 0;
    for (int c = 0; c < 10; ++c) o = o * 10 + static_cast<std::size_t>(entry[c] - '0');
    d.offsets.push_back(o);
    // The offset must land exactly on "<k> 0 obj".
    const std::string want = std::to_string(k) + " 0 obj";
    if (o + want.size() > bytes.size() || bytes.compare(o, want.size(), want) != 0) {
      d.problems.push_back("xref offset for object " + std::to_string(k) + " is wrong");
      continue;
    }
    const std::size_t body = o + want.size() + 1;
    const std::size_t end = bytes.find("\nendobj\n", body);
    if (end == std::string::npos) {
      d.problems.push_back("object " + std::to_string(k) + " has no endobj");
      continue;
    }
    d.objects[static_cast<int>(k)] = bytes.substr(body, end - body);
  }

  const std::size_t tr = find_last(bytes, "trailer");
  if (tr == 0) {
    d.problems.push_back("no trailer");
  } else {
    const std::string trailer = bytes.substr(tr, sx - tr);
    const std::size_t sp = trailer.find("/Size");
    if (sp != std::string::npos) d.size = std::atoi(trailer.c_str() + sp + 5);
    const std::size_t rp = trailer.find("/Root");
    if (rp != std::string::npos) d.root_obj = std::atoi(trailer.c_str() + rp + 5);
  }

  // The content stream: /Length must match the actual byte count.
  for (const auto& kv : d.objects) {
    const std::string& body = kv.second;
    const std::size_t sp = body.find("stream\n");
    if (sp == std::string::npos) continue;
    const std::size_t lp = body.find("/Length");
    if (lp == std::string::npos) {
      d.problems.push_back("stream object without /Length");
      continue;
    }
    const std::size_t declared = static_cast<std::size_t>(std::atoi(body.c_str() + lp + 7));
    const std::size_t data_start = sp + 7;
    const std::size_t ep = body.find("endstream", data_start);
    if (ep == std::string::npos) {
      d.problems.push_back("stream without endstream");
      continue;
    }
    const std::size_t actual = ep - data_start;
    if (actual != declared) {
      d.problems.push_back("/Length " + std::to_string(declared) + " but " +
                           std::to_string(actual) + " bytes of stream data");
    }
    d.content = body.substr(data_start, actual);
  }

  const auto page = d.objects.find(3);
  if (page != d.objects.end()) {
    const std::size_t mb = page->second.find("/MediaBox");
    if (mb != std::string::npos) {
      const char* c = page->second.c_str() + mb + 9;
      while (*c && *c != '[') ++c;
      if (*c == '[') ++c;
      for (int k = 0; k < 4 && *c; ++k) {
        while (*c == ' ') ++c;
        d.media[k] = std::strtod(c, const_cast<char**>(&c));
      }
    }
  }
  return d;
}

// Every point touched by an `m` or `l` operator inside the FIRST balanced
// q...Q block, which is where pdf_writer.cpp draws the plan itself.
std::vector<Vec2> pdf_plan_points(const std::string& content) {
  std::vector<Vec2> out;
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : content) {
    if (c == ' ' || c == '\n' || c == '\r') {
      if (!cur.empty()) tokens.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) tokens.push_back(cur);

  int depth = 0;
  bool started = false;
  for (std::size_t k = 0; k < tokens.size(); ++k) {
    if (tokens[k] == "q") {
      ++depth;
      started = true;
      continue;
    }
    if (tokens[k] == "Q") {
      --depth;
      if (started && depth == 0) break;
      continue;
    }
    if (!started) continue;
    if ((tokens[k] == "m" || tokens[k] == "l") && k >= 2) {
      out.push_back(Vec2{std::strtod(tokens[k - 2].c_str(), nullptr),
                         std::strtod(tokens[k - 1].c_str(), nullptr)});
    }
  }
  return out;
}

}  // namespace

// ===========================================================================
// Geometry primitives
// ===========================================================================

TEST_CASE("plan/polygon_area_and_centroid") {
  const std::vector<Vec2> ccw = {{0, 0}, {4, 0}, {4, 3}, {0, 3}};
  CHECK(polygon_signed_area(ccw) == doctest::Approx(12.0));
  CHECK(polygon_area(ccw) == doctest::Approx(12.0));
  CHECK(polygon_perimeter(ccw) == doctest::Approx(14.0));
  const Vec2 c = polygon_centroid(ccw);
  CHECK(c.x == doctest::Approx(2.0));
  CHECK(c.y == doctest::Approx(1.5));

  std::vector<Vec2> cw = ccw;
  std::reverse(cw.begin(), cw.end());
  CHECK(polygon_signed_area(cw) == doctest::Approx(-12.0));
  CHECK(polygon_area(cw) == doctest::Approx(12.0));

  CHECK(point_in_polygon(ccw, Vec2{2, 1}));
  CHECK_FALSE(point_in_polygon(ccw, Vec2{5, 1}));
  CHECK_FALSE(point_in_polygon(ccw, Vec2{2, -0.1}));

  const std::vector<Vec2> degenerate = {{1, 1}, {2, 2}};
  CHECK(polygon_signed_area(degenerate) == doctest::Approx(0.0));
  CHECK(polygon_centroid(degenerate).x == doctest::Approx(1.5));
}

TEST_CASE("plan/up_axis_projection_is_a_relabelling_not_a_rescale") {
  CHECK(project(1.f, 2.f, 3.f, UpAxis::kZ).x == doctest::Approx(1.0));
  CHECK(project(1.f, 2.f, 3.f, UpAxis::kZ).y == doctest::Approx(2.0));
  CHECK(up_coord(1.f, 2.f, 3.f, UpAxis::kZ) == doctest::Approx(3.0));

  CHECK(project(1.f, 2.f, 3.f, UpAxis::kY).x == doctest::Approx(3.0));
  CHECK(project(1.f, 2.f, 3.f, UpAxis::kY).y == doctest::Approx(1.0));
  CHECK(up_coord(1.f, 2.f, 3.f, UpAxis::kY) == doctest::Approx(2.0));

  CHECK(project(1.f, 2.f, 3.f, UpAxis::kX).x == doctest::Approx(2.0));
  CHECK(project(1.f, 2.f, 3.f, UpAxis::kX).y == doctest::Approx(3.0));
  CHECK(up_coord(1.f, 2.f, 3.f, UpAxis::kX) == doctest::Approx(1.0));
}

// ===========================================================================
// Slice + occupancy
// ===========================================================================

TEST_CASE("plan/occupancy_keeps_only_the_band") {
  OccupancyGrid g;
  PlanStats stats;
  BandOptions band;
  band.min_points = 3;
  REQUIRE(build_occupancy(input_of(default_cloud()), band, &g, &stats).ok());
  REQUIRE(g.valid());

  CHECK(stats.points_considered == default_cloud().size());
  CHECK(stats.points_in_band < stats.points_considered);
  // The floor and ceiling are ~8100 points and must all be gone.
  CHECK(stats.points_in_band > 20000);
  CHECK(stats.points_in_band < stats.points_considered / 2);

  const PlanBounds ext = g.extent();
  CHECK(ext.min_x <= 0.0);
  CHECK(ext.min_y <= 0.0);
  CHECK(ext.max_x >= kEastX);
  CHECK(ext.max_y >= kNorthY);
  // 8 x 5 m at 2 cm, plus a cell of noise margin either side.
  CHECK(g.w >= 400);
  CHECK(g.w <= 420);
  CHECK(g.h >= 250);
  CHECK(g.h <= 270);
  CHECK(g.occupied_count() > 3000);
  CHECK(g.occupied_count() < 9000);
}

TEST_CASE("plan/occupancy_lattice_is_stable_across_bands") {
  const PlanInput in = input_of(default_cloud());
  OccupancyGrid a, b;
  BandOptions band_a;
  band_a.min_points = 3;
  REQUIRE(build_occupancy(in, band_a, &a, nullptr).ok());

  BandOptions band_b = band_a;
  band_b.z_min_m = 1.2;
  band_b.z_max_m = 1.9;
  REQUIRE(build_occupancy(in, band_b, &b, nullptr).ok());

  // Different band, same lattice: origins are on the same multiple-of-res
  // grid, so a cell boundary never moves sideways under the slider.
  const double ka = a.origin_x / a.res_m;
  const double kb = b.origin_x / b.res_m;
  CHECK(std::fabs(ka - std::floor(ka + 0.5)) < 1e-6);
  CHECK(std::fabs(kb - std::floor(kb + 0.5)) < 1e-6);

  // An explicit lattice reproduces the frame exactly.
  OccupancyGrid c;
  BandOptions band_c = band_b;
  band_c.lattice = &a;
  REQUIRE(build_occupancy(in, band_c, &c, nullptr).ok());
  CHECK(c.w == a.w);
  CHECK(c.h == a.h);
  CHECK(c.origin_x == a.origin_x);
  CHECK(c.origin_y == a.origin_y);
  CHECK(c.res_m == a.res_m);
}

TEST_CASE("plan/occupancy_rejects_nonsense") {
  const PlanInput in = input_of(default_cloud());
  OccupancyGrid g;
  BandOptions band;

  CHECK(build_occupancy(in, band, nullptr, nullptr).error() == ScanError::kInvalidArgument);

  band.z_min_m = 1.5;
  band.z_max_m = 1.0;
  CHECK(build_occupancy(in, band, &g, nullptr).error() == ScanError::kInvalidArgument);

  band = BandOptions{};
  band.res_m = 0.0;
  CHECK(build_occupancy(in, band, &g, nullptr).error() == ScanError::kInvalidArgument);

  // 0.1 mm over an 8 m building is 80000 x 50000 = 4e9 cells.
  band = BandOptions{};
  band.res_m = 0.0001;
  CHECK(build_occupancy(in, band, &g, nullptr).error() == ScanError::kCapacityExceeded);

  PlanInput empty;
  CHECK(build_occupancy(empty, BandOptions{}, &g, nullptr).error() ==
        ScanError::kInvalidArgument);

  // A band above the ceiling is EMPTY, not an error.
  band = BandOptions{};
  band.z_min_m = 40.0;
  band.z_max_m = 41.0;
  CHECK(build_occupancy(in, band, &g, nullptr).ok());
  CHECK_FALSE(g.valid());
}

TEST_CASE("plan/empty_band_yields_an_empty_plan_not_an_error") {
  PlanOptions o;
  o.slice.z_min_m = 40.0f;
  o.slice.z_max_m = 41.0f;
  PlanModel m;
  CHECK(extract_floor_plan(input_of(default_cloud()), o, &m).ok());
  CHECK(m.walls.empty());
  CHECK(m.rooms.empty());
  CHECK(m.empty());
}

// ===========================================================================
// Wall extraction on the synthetic building
// ===========================================================================

TEST_CASE("plan/synthetic_building_recovers_the_wall_topology") {
  const PlanModel& m = default_plan();

  // Six real walls. Anything beyond that is furniture read as architecture,
  // and on this fixture there must be none.
  CHECK(m.walls.size() == 6);

  for (const auto& e : expected_walls()) {
    CAPTURE(e.name);
    const WallSegment* w = find_wall(m, e, 0.06);
    REQUIRE(w != nullptr);
    CHECK(w->length() == doctest::Approx(distance(e.a, e.b)).epsilon(0.01));
    CHECK(w->coverage >= e.coverage);
    CHECK(w->rms_residual_m < 0.025);   // the 2 cm face noise, clipped
    CHECK(w->confidence > 0.7f);
    if (e.paired) {
      CHECK(w->evidence == WallEvidence::kPairedFaces);
      // Thickness MEASURED from the two scanned faces, to within a cell.
      CHECK(w->thickness_m == doctest::Approx(kPartThickness).epsilon(0.10));
    } else {
      CHECK(w->evidence == WallEvidence::kSingleFace);
      CHECK(w->room_inset_m() == 0.0);
    }
  }

  CHECK(m.stats.paired_walls == 2);
  CHECK(m.stats.occupied_cells > 3000);
  CHECK(m.stats.total_wall_length_m == doctest::Approx(8 + 8 + 5 + 5 + 8 + 3.525).epsilon(0.02));
}

// Reports the numbers docs/A12-plan.md §6 quotes. Run with `-s` to see them;
// the same convention test_timesync.cpp uses for its convergence figures.
TEST_CASE("plan/extraction_quality_report") {
  const PlanModel& m = default_plan();
  MESSAGE("  cloud: " << default_cloud().size() << " points, "
                      << m.stats.points_in_band << " in band, "
                      << m.stats.occupied_cells << " occupied cells, "
                      << m.stats.ransac_lines << " RANSAC lines");
  MESSAGE("  dominant direction: " << deg(m.stats.dominant_angle_rad) << " deg, "
                                   << m.stats.snapped_walls << " walls snapped, "
                                   << m.stats.paired_walls << " paired");
  for (const auto& e : expected_walls()) {
    const WallSegment* w = find_wall(m, e, 0.06);
    if (w == nullptr) {
      MESSAGE("  wall " << std::string(e.name) << ": NOT FOUND");
      continue;
    }
    const double err = std::max(std::min(distance(w->a, e.a), distance(w->a, e.b)),
                                std::min(distance(w->b, e.a), distance(w->b, e.b)));
    MESSAGE("  wall " << std::string(e.name) << ": endpoint error " << err * 1000.0 << " mm, thickness "
                      << w->thickness_m << " m, rms " << w->rms_residual_m * 1000.0
                      << " mm, coverage " << w->coverage << ", confidence " << w->confidence);
  }
  for (const auto& o : m.openings) {
    MESSAGE("  opening " << std::string(to_string(o.kind)) << ": width " << o.width_m << " m at ("
                         << o.midpoint().x << ", " << o.midpoint().y << "), sill "
                         << std::string(to_string(o.sill)) << " occ " << o.sill_occupancy);
  }
  const double truth[] = {kCorridorArea, kRoomAArea, kRoomBArea};
  const Vec2 centers[] = {{4.0, 0.5 * kPartLoY},
                          {0.5 * kVpartLoX, 0.5 * (kPartHiY + kNorthY)},
                          {0.5 * (kVpartHiX + kEastX), 0.5 * (kPartHiY + kNorthY)}};
  for (int k = 0; k < 3; ++k) {
    const Room* r = room_nearest(m, centers[k], 0.15);
    if (r == nullptr) {
      MESSAGE("  room at (" << centers[k].x << ", " << centers[k].y << "): NOT FOUND");
      continue;
    }
    MESSAGE("  room " << r->label << ": " << r->area_m2 << " m2 vs " << truth[k]
                      << " m2 = " << (r->area_m2 / truth[k] - 1.0) * 100.0 << " %");
  }
}

TEST_CASE("plan/orthogonality_snapping_squares_the_plan") {
  const PlanModel& m = default_plan();
  CHECK(m.stats.snapped_walls >= 6);
  for (const auto& w : m.walls) {
    CHECK(w.snapped);
    // Every wall lands on one of the two dominant axes, to well under the
    // 0.5 degree a drafter would notice.
    const double rel = deg(w.angle_rad() - m.stats.dominant_angle_rad);
    const double folded = rel - 90.0 * std::floor(rel / 90.0 + 0.5);
    CAPTURE(folded);
    CHECK(std::fabs(folded) < 0.05);
  }
  // The building really is axis-aligned, so the estimate must say so
  // (dominant_angle_rad lives in [0, pi/2), where 0 and 90 deg are the same).
  const double dom = deg(m.stats.dominant_angle_rad);
  CHECK(std::min(dom, 90.0 - dom) < 0.1);
}

TEST_CASE("plan/orthogonality_snapping_follows_a_rotated_building") {
  const std::vector<PointVertex> pts = make_building([] {
    BuildOpts b;
    b.rotate_deg = 4.0;
    return b;
  }());
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(pts), PlanOptions{}, &m).ok());
  CHECK(m.walls.size() == 6);

  // The dominant direction tracks the building, not the world axes.
  CHECK(deg(m.stats.dominant_angle_rad) == doctest::Approx(4.0).epsilon(0.02));
  for (const auto& w : m.walls) {
    CHECK(w.snapped);
    const double rel = deg(w.angle_rad() - m.stats.dominant_angle_rad);
    const double folded = rel - 90.0 * std::floor(rel / 90.0 + 0.5);
    CHECK(std::fabs(folded) < 0.05);
  }
  // Rotation is rigid: the areas are unchanged.
  CHECK(m.rooms.size() == 3);
  double total = 0.0;
  for (const auto& r : m.rooms) total += r.area_m2;
  CHECK(total == doctest::Approx(kCorridorArea + kRoomAArea + kRoomBArea).epsilon(0.02));
}

TEST_CASE("plan/a_non_manhattan_wall_is_left_alone") {
  const std::vector<PointVertex> pts = make_building([] {
    BuildOpts b;
    b.diagonal_wall = true;
    return b;
  }());
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(pts), PlanOptions{}, &m).ok());

  // The 30 degree wall is 23 degrees outside the +-7 degree snap window, so
  // it must come back at 30 degrees, unsnapped, while everything else snaps.
  const WallSegment* diag = nullptr;
  for (const auto& w : m.walls) {
    const double a = deg(w.angle_rad());
    if (a > 20.0 && a < 40.0) diag = &w;
  }
  REQUIRE(diag != nullptr);
  CHECK(deg(diag->angle_rad()) == doctest::Approx(30.0).epsilon(0.03));
  CHECK_FALSE(diag->snapped);

  int snapped = 0;
  for (const auto& w : m.walls) {
    if (w.snapped) ++snapped;
  }
  CHECK(snapped >= 6);
}

TEST_CASE("plan/snapping_can_be_turned_off") {
  PlanOptions o;
  o.slice.snap_orthogonal = false;
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), o, &m).ok());
  CHECK(m.stats.snapped_walls == 0);
  for (const auto& w : m.walls) CHECK_FALSE(w.snapped);
  // Unsnapped, the fit is still good — just not exactly square.
  CHECK(m.walls.size() == 6);
  for (const auto& w : m.walls) {
    const double a = deg(w.angle_rad());
    const double folded = a - 90.0 * std::floor(a / 90.0 + 0.5);
    CHECK(std::fabs(folded) < 1.0);
  }
}

// ===========================================================================
// Openings
// ===========================================================================

TEST_CASE("plan/doors_are_detected_at_the_right_places_and_widths") {
  const PlanModel& m = default_plan();

  std::vector<const Opening*> doors;
  for (const auto& o : m.openings) {
    if (o.kind == OpeningKind::kDoorCandidate) doors.push_back(&o);
  }
  REQUIRE(doors.size() == 2);

  const Vec2 want_a{0.5 * (kDoorAx0 + kDoorAx1), kPartCenterY};
  const Vec2 want_b{0.5 * (kDoorBx0 + kDoorBx1), kPartCenterY};
  const Opening* a = nullptr;
  const Opening* b = nullptr;
  for (const Opening* o : doors) {
    if (distance(o->midpoint(), want_a) < 0.08) a = o;
    if (distance(o->midpoint(), want_b) < 0.08) b = o;
  }
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(a->width_m == doctest::Approx(kDoorAx1 - kDoorAx0).epsilon(0.04));
  CHECK(b->width_m == doctest::Approx(kDoorBx1 - kDoorBx0).epsilon(0.04));

  // A door is open below the band as well as in it.
  CHECK(a->sill == SillCheck::kOpenBelow);
  CHECK(b->sill == SillCheck::kOpenBelow);
  CHECK(a->sill_occupancy < 0.25);
  CHECK(a->confidence > 0.7f);

  // Both doors belong to the one partition wall, and it is that wall's
  // coverage that they lower.
  const WallSegment* wall = m.wall_by_id(a->wall_id);
  REQUIRE(wall != nullptr);
  CHECK(b->wall_id == a->wall_id);
  CHECK(wall->coverage == doctest::Approx(1.0 - 1.8 / 8.0).epsilon(0.05));
}

TEST_CASE("plan/the_window_is_told_apart_from_a_door_by_the_sill_reslice") {
  const PlanModel& m = default_plan();

  const Opening* win = nullptr;
  for (const auto& o : m.openings) {
    if (o.kind == OpeningKind::kWindowCandidate) win = &o;
  }
  REQUIRE(win != nullptr);
  CHECK(win->width_m == doctest::Approx(kWinX1 - kWinX0).epsilon(0.04));
  CHECK(win->midpoint().x == doctest::Approx(0.5 * (kWinX0 + kWinX1)).epsilon(0.04));
  CHECK(win->midpoint().y == doctest::Approx(kNorthY).epsilon(0.02));
  CHECK(win->sill == SillCheck::kSolidBelow);
  CHECK(win->sill_occupancy > 0.9);

  // Its gap is the same width class as a door: WITHOUT the re-slice the two
  // are indistinguishable, which is the whole point of the second band.
  PlanOptions no_sill;
  no_sill.slice.window_sill_check = false;
  PlanModel m2;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), no_sill, &m2).ok());
  int windows = 0, doors = 0;
  for (const auto& o : m2.openings) {
    if (o.kind == OpeningKind::kWindowCandidate) ++windows;
    if (o.kind == OpeningKind::kDoorCandidate) ++doors;
    CHECK(o.sill == SillCheck::kNotChecked);
  }
  CHECK(windows == 0);
  CHECK(doors == 3);
}

TEST_CASE("plan/a_slice_above_the_window_head_sees_no_opening_there") {
  // The window head is at 2.10 m; a band above it crosses solid wall.
  PlanOptions o;
  o.slice.z_min_m = 2.15f;
  o.slice.z_max_m = 2.35f;
  o.slice.min_cell_points = 2;  // a 20 cm band has fewer points per cell
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), o, &m).ok());

  for (const auto& op : m.openings) {
    // Nothing near the window's x range on the north wall.
    const bool near_window =
        std::fabs(op.midpoint().y - kNorthY) < 0.1 &&
        op.midpoint().x > kWinX0 - 0.2 && op.midpoint().x < kWinX1 + 0.2;
    CHECK_FALSE(near_window);
  }
  // The doors, whose leaves stop at 2.10 m, are also gone up here.
  CHECK(m.walls.size() >= 5);
}

TEST_CASE("plan/openings_wider_than_max_bridge_break_the_wall_instead") {
  PlanOptions o;
  o.openings.max_bridge_m = 0.5;  // narrower than the 0.9 m doors
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), o, &m).ok());

  // The partition is now three separate wall segments, not one bridged one,
  // and no opening is recorded for the gaps between them.
  int on_partition = 0;
  for (const auto& w : m.walls) {
    if (std::fabs(w.midpoint().y - kPartCenterY) < 0.1 && std::fabs(w.direction().y) < 0.1) {
      ++on_partition;
    }
  }
  CHECK(on_partition == 3);
  for (const auto& op : m.openings) CHECK(op.width_m <= 0.5);
}

// ===========================================================================
// Rooms
// ===========================================================================

TEST_CASE("plan/room_areas_are_within_two_percent") {
  const PlanModel& m = default_plan();
  REQUIRE(m.rooms.size() == 3);

  struct Want {
    const char* name;
    Vec2 centroid;
    double area;
  };
  const Want wants[] = {
      {"corridor", {0.5 * (kWestX + kEastX), 0.5 * (kSouthY + kPartLoY)}, kCorridorArea},
      {"room A", {0.5 * (kWestX + kVpartLoX), 0.5 * (kPartHiY + kNorthY)}, kRoomAArea},
      {"room B", {0.5 * (kVpartHiX + kEastX), 0.5 * (kPartHiY + kNorthY)}, kRoomBArea},
  };
  for (const auto& want : wants) {
    CAPTURE(want.name);
    const Room* r = room_nearest(m, want.centroid, 0.15);
    REQUIRE(r != nullptr);
    CHECK(r->area_m2 == doctest::Approx(want.area).epsilon(0.02));
    CHECK(r->polygon.size() == 4);
    CHECK(r->perimeter_m > 0.0);
    CHECK(r->confidence > 0.5f);
    CHECK(polygon_signed_area(r->polygon) > 0.0);  // rooms come out CCW
    CHECK(point_in_polygon(r->polygon, r->centroid));
  }

  CHECK(m.stats.total_room_area_m2 ==
        doctest::Approx(kCorridorArea + kRoomAArea + kRoomBArea).epsilon(0.02));

  // Labels are assigned in reading order and are unique.
  std::vector<std::string> labels;
  for (const auto& r : m.rooms) labels.push_back(r.label);
  std::sort(labels.begin(), labels.end());
  CHECK(std::unique(labels.begin(), labels.end()) == labels.end());
  CHECK(labels[0] == "R1");

  // A room bounded by a wall with doorways in it is not "fully measured",
  // and says so rather than pretending.
  const Room* corridor = room_nearest(m, Vec2{4.0, 0.7}, 0.15);
  REQUIRE(corridor != nullptr);
  CHECK_FALSE(corridor->fully_measured);
}

TEST_CASE("plan/single_face_walls_are_not_inset") {
  // The whole reason room areas land within 0.2%: the exterior walls' fitted
  // lines ARE their interior faces, so insetting them by an assumed
  // thickness would shrink every room by 2 * default_thickness / 2.
  const PlanModel& m = default_plan();
  const Room* corridor = room_nearest(m, Vec2{4.0, 0.7}, 0.15);
  REQUIRE(corridor != nullptr);
  const PlanBounds b = polygon_bounds(corridor->polygon);
  CHECK(std::fabs(b.min_x - kWestX) < 0.02);
  CHECK(std::fabs(b.max_x - kEastX) < 0.02);
  CHECK(std::fabs(b.min_y - kSouthY) < 0.02);
  // ... while the partition, whose thickness WAS measured, is inset by half.
  CHECK(std::fabs(b.max_y - kPartLoY) < 0.02);
}

TEST_CASE("plan/rooms_can_be_turned_off") {
  PlanOptions o;
  o.rooms.enabled = false;
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), o, &m).ok());
  CHECK(m.walls.size() == 6);
  CHECK(m.rooms.empty());
  CHECK(m.stats.total_room_area_m2 == 0.0);
}

// ===========================================================================
// Clutter — the honest limits
// ===========================================================================

TEST_CASE("plan/clutter_below_and_inside_the_band_is_rejected") {
  // The default fixture already carries a low table, a 0.5 x 0.4 m cabinet
  // standing 1.85 m tall in the middle of room B, 400 speckle points and a
  // full floor and ceiling. None of it becomes a wall.
  const PlanModel& m = default_plan();
  CHECK(m.walls.size() == 6);
  for (const auto& w : m.walls) {
    // No wall anywhere near the cabinet.
    CHECK(point_segment_distance(Vec2{6.05, 3.6}, w.a, w.b) > 0.5);
  }
}

TEST_CASE("plan/furniture_against_a_wall_reads_as_a_wall_documented_limit") {
  // A 1.4 m bookshelf standing 0.55 m off room A's west wall. It is longer
  // than min_wall_length_m and flat, so NOTHING in a horizontal slice can
  // tell it from a wall — and the extractor does not pretend otherwise. What
  // it must not do is corrupt the rest of the plan.
  const std::vector<PointVertex> pts = make_building([] {
    BuildOpts b;
    b.shelf_against_wall = true;
    return b;
  }());
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(pts), PlanOptions{}, &m).ok());

  CHECK(m.walls.size() == 7);
  const WallSegment* shelf = nullptr;
  for (const auto& w : m.walls) {
    if (std::fabs(w.midpoint().x - 0.55) < 0.05 && w.length() < 2.0) shelf = &w;
  }
  REQUIRE(shelf != nullptr);
  CHECK(shelf->length() == doctest::Approx(1.44).epsilon(0.05));

  // It is 0.55 m from the wall, past thickness_max_m, so it does NOT pair
  // with it and inflate that wall into a 0.55 m thick one.
  CHECK(shelf->evidence == WallEvidence::kSingleFace);

  // And the three rooms are still right, because a free-floating segment is
  // its own connected component in the planar graph.
  REQUIRE(m.rooms.size() == 3);
  const Room* room_a = room_nearest(m, Vec2{0.5 * (kWestX + kVpartLoX), 3.275}, 0.15);
  REQUIRE(room_a != nullptr);
  CHECK(room_a->area_m2 == doctest::Approx(kRoomAArea).epsilon(0.02));
}

TEST_CASE("plan/a_noise_band_between_two_faces_is_not_a_third_wall") {
  // At min_cell_points = 2 the 2 cm noise tails of the partition's two faces
  // meet in the middle, and sequential RANSAC will fit a line down there
  // once the faces themselves have been consumed. A wall inside a wall is
  // impossible, and the extractor drops it on exactly that ground.
  PlanOptions o;
  o.slice.min_cell_points = 2;
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), o, &m).ok());
  CHECK(m.walls.size() == 6);
  CHECK(m.rooms.size() == 3);
  for (const auto& r : m.rooms) CHECK(r.area_m2 > 10.0);
}

// ===========================================================================
// Editor v1 (§3.6)
// ===========================================================================

TEST_CASE("plan/editor_region_semantics") {
  PlanEditState s;
  CHECK_FALSE(edit_has_regions(s));
  CHECK(edit_accepts(s, 0.0, 0.0));       // no regions = keep everything
  CHECK(edit_accepts(s, 1e6, -1e6));

  s = with_include_region(s, 0, 0, 4, 4);
  CHECK(edit_has_regions(s));
  CHECK(edit_accepts(s, 2, 2));
  CHECK_FALSE(edit_accepts(s, 5, 2));

  s = with_exclude_region(s, 1, 1, 2, 2);
  CHECK_FALSE(edit_accepts(s, 1.5, 1.5));  // exclude beats include
  CHECK(edit_accepts(s, 3, 3));

  s = with_include_region(s, 10, 10, 12, 12);  // a second include widens
  CHECK(edit_accepts(s, 11, 11));
  CHECK_FALSE(edit_accepts(s, 5, 2));

  CHECK(s.regions.size() == 3);
  s = without_region(s, 1);
  CHECK(s.regions.size() == 2);
  CHECK(edit_accepts(s, 1.5, 1.5));  // the exclude is gone
  s = without_region(s, 99);         // out of range is a no-op, not an error
  CHECK(s.regions.size() == 2);
  s = with_regions_cleared(s);
  CHECK(s.regions.empty());
  CHECK(edit_accepts(s, -100, 100));
}

TEST_CASE("plan/editor_mutators_are_pure_and_validate") {
  PlanEditState base;
  const PlanEditState moved = with_slice_band(base, 2.0f, 1.0f);
  // The original is untouched — this is what makes undo a vector push.
  CHECK(base.options.slice.z_min_m == 1.0f);
  CHECK(base.options.slice.z_max_m == 1.5f);
  // Inverted handles are swapped, not rejected.
  CHECK(moved.options.slice.z_min_m == 1.0f);
  CHECK(moved.options.slice.z_max_m == 2.0f);

  const PlanEditState collapsed = with_slice_band(base, 1.4f, 1.4f);
  CHECK(collapsed.options.slice.z_max_m > collapsed.options.slice.z_min_m);

  const PlanEditState centered = with_slice_center(base, 2.0f);
  CHECK(centered.options.slice.z_min_m == doctest::Approx(1.75f));
  CHECK(centered.options.slice.z_max_m == doctest::Approx(2.25f));

  CHECK(with_grid_resolution(base, 0.0f).options.slice.grid_res_m == doctest::Approx(0.002f));
  CHECK(with_grid_resolution(base, 99.f).options.slice.grid_res_m == doctest::Approx(1.0f));
  CHECK(with_orthogonality(base, false, 900.f).options.slice.snap_tolerance_deg ==
        doctest::Approx(45.f));
  CHECK(with_orthogonality(base, false, 900.f).options.slice.snap_orthogonal == false);
  CHECK(with_up_axis(base, UpAxis::kY).options.slice.up == UpAxis::kY);
  CHECK(with_sill_check(base, true, 0.9f, 0.2f).options.slice.sill_z_min_m ==
        doctest::Approx(0.2f));

  PlanRegion r;
  r.min_x = 5;
  r.max_x = 1;
  r.min_y = 6;
  r.max_y = 2;
  const PlanRegion n = normalized_region(r);
  CHECK(n.min_x == 1);
  CHECK(n.max_x == 5);
  CHECK(n.min_y == 2);
  CHECK(n.max_y == 6);
}

TEST_CASE("plan/editor_exclude_region_removes_the_walls_inside_it") {
  const PlanInput in = input_of(default_cloud());
  PlanEditState s;
  PlanModel before;
  REQUIRE(recompute_plan(in, s, &before).ok());
  CHECK(before.walls.size() == 6);

  // Cut out room B entirely: everything east of the vertical partition and
  // north of the horizontal one.
  s = with_exclude_region(s, kVpartHiX - 0.05, kPartHiY - 0.05, 9.0, 6.0);
  PlanModel after;
  REQUIRE(recompute_plan(in, s, &after).ok());

  // The exterior walls are not deleted, they are CUT: the east wall now
  // stops at the corridor, the north wall stops at the partition. So the
  // count can stay the same while a third of the wall length disappears.
  CHECK(after.walls.size() <= before.walls.size());
  CHECK(after.stats.total_wall_length_m < before.stats.total_wall_length_m - 7.0);

  // Concretely: the east wall now stops at the cut, and the north wall now
  // stops at it too, instead of running the full building.
  const WallSegment* east = nullptr;
  const WallSegment* north = nullptr;
  for (const auto& w : after.walls) {
    if (std::fabs(w.midpoint().x - kEastX) < 0.05) east = &w;
    if (std::fabs(w.midpoint().y - kNorthY) < 0.05) north = &w;
  }
  REQUIRE(east != nullptr);
  REQUIRE(north != nullptr);
  CHECK(east->length() == doctest::Approx(kPartHiY - 0.05).epsilon(0.06));
  CHECK(north->length() == doctest::Approx(kVpartHiX - 0.05).epsilon(0.06));
  for (const auto& w : after.walls) {
    // No wall may pass through the middle of the cut-out.
    const Vec2 mid = w.midpoint();
    const bool inside = mid.x > kVpartHiX + 0.3 && mid.y > kPartHiY + 0.3;
    CAPTURE(mid.x);
    CAPTURE(mid.y);
    CHECK_FALSE(inside);
  }
  // Room B is gone; the corridor survives.
  CHECK(room_nearest(after, Vec2{6.0, 3.275}, 0.3) == nullptr);
  const Room* corridor = room_nearest(after, Vec2{4.0, 0.7}, 0.3);
  REQUIRE(corridor != nullptr);
  CHECK(corridor->area_m2 == doctest::Approx(kCorridorArea).epsilon(0.02));
}

TEST_CASE("plan/editor_include_region_keeps_only_what_is_inside") {
  const PlanInput in = input_of(default_cloud());
  PlanEditState s;
  s = with_include_region(s, -0.5, -0.5, kEastX + 0.5, kPartHiY + 0.5);
  PlanModel m;
  REQUIRE(recompute_plan(in, s, &m).ok());

  // Only the corridor's four bounding walls survive, so the corridor is the
  // one room and its area is unchanged.
  REQUIRE(m.rooms.size() == 1);
  CHECK(m.rooms[0].area_m2 == doctest::Approx(kCorridorArea).epsilon(0.02));
  for (const auto& w : m.walls) CHECK(w.midpoint().y < kPartHiY + 0.6);
}

TEST_CASE("plan/editor_slice_slider_and_the_cached_grid_path_agree") {
  const PlanInput in = input_of(default_cloud());
  PlanEditState s = with_slice_band(PlanEditState{}, 1.6f, 2.0f);

  OccupancyGrid main_grid, sill_grid;
  REQUIRE(recompute_grids(in, s, &main_grid, &sill_grid).ok());
  REQUIRE(main_grid.valid());
  REQUIRE(sill_grid.valid());
  CHECK(sill_grid.w == main_grid.w);
  CHECK(sill_grid.h == main_grid.h);

  PlanModel cached;
  REQUIRE(recompute_walls(main_grid, &sill_grid, s, &cached).ok());
  PlanModel full;
  REQUIRE(recompute_plan(in, s, &full).ok());

  // The fast path an app uses while dragging must produce the same plan as
  // the slow path, or the preview lies about what the export will contain.
  REQUIRE(cached.walls.size() == full.walls.size());
  for (std::size_t k = 0; k < cached.walls.size(); ++k) {
    CHECK(cached.walls[k].a.x == full.walls[k].a.x);
    CHECK(cached.walls[k].a.y == full.walls[k].a.y);
    CHECK(cached.walls[k].b.x == full.walls[k].b.x);
    CHECK(cached.walls[k].b.y == full.walls[k].b.y);
  }
  CHECK(cached.rooms.size() == full.rooms.size());
  CHECK(cached.openings.size() == full.openings.size());

  // A band at 1.6-2.0 m is above the door heads (2.10 m is the head, so the
  // doors are still open) and still finds the same six walls.
  CHECK(full.walls.size() == 6);
}

TEST_CASE("plan/editor_grid_resolution_changes_the_grid_not_the_geometry") {
  const PlanInput in = input_of(default_cloud());
  PlanEditState s = with_grid_resolution(PlanEditState{}, 0.04f);
  s.options.slice.min_cell_points = 3;
  PlanModel m;
  REQUIRE(recompute_plan(in, s, &m).ok());
  CHECK(m.grid_res_m == doctest::Approx(0.04));
  CHECK(m.stats.grid_w < default_plan().stats.grid_w);
  CHECK(m.walls.size() == 6);
  REQUIRE(m.rooms.size() == 3);
  double total = 0.0;
  for (const auto& r : m.rooms) total += r.area_m2;
  CHECK(total == doctest::Approx(kCorridorArea + kRoomAArea + kRoomBArea).epsilon(0.03));
}

TEST_CASE("plan/sill_check_reports_no_data_rather_than_guessing") {
  // Slice the sill band somewhere with no points at all (between the ceiling
  // and nothing). The check must abstain, not report every door as open.
  PlanOptions o;
  o.slice.sill_z_min_m = 3.0f;
  o.slice.sill_z_max_m = 3.4f;
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), o, &m).ok());
  int checked = 0;
  for (const auto& op : m.openings) {
    if (op.kind == OpeningKind::kDoorCandidate || op.kind == OpeningKind::kWideOpening ||
        op.kind == OpeningKind::kWindowCandidate) {
      ++checked;
      CHECK(op.sill == SillCheck::kNoData);
    }
  }
  CHECK(checked == 3);
}

// ===========================================================================
// DXF
// ===========================================================================

TEST_CASE("plan/dxf_structure_and_layers") {
  const PlanModel& m = default_plan();
  const std::string path = temp_path("plan", "dxf");
  DxfOptions opts;
  REQUIRE(write_dxf(m, opts, path).ok());
  const std::string text = read_file(path);
  REQUIRE_FALSE(text.empty());

  const DxfDoc doc = dxf_parse(text);
  CHECK(doc.saw_eof);
  CHECK(doc.acadver == "AC1009");  // R12
  CHECK(std::find(doc.sections.begin(), doc.sections.end(), "HEADER") != doc.sections.end());
  CHECK(std::find(doc.sections.begin(), doc.sections.end(), "TABLES") != doc.sections.end());
  CHECK(std::find(doc.sections.begin(), doc.sections.end(), "BLOCKS") != doc.sections.end());
  CHECK(std::find(doc.sections.begin(), doc.sections.end(), "ENTITIES") != doc.sections.end());

  CHECK(doc.has_layer("WALLS"));
  CHECK(doc.has_layer("OPENINGS"));
  CHECK(doc.has_layer("ROOMS"));
  CHECK(doc.has_layer("DIMENSIONS"));
  // TEXT entities name STANDARD, so the style table has to define it.
  CHECK(std::find(doc.styles.begin(), doc.styles.end(), "STANDARD") != doc.styles.end());

  // Every POLYLINE has its SEQEND and at least two vertices.
  for (const auto& e : doc.entities) {
    if (e.type != "POLYLINE") continue;
    CHECK(e.sealed);
    CHECK(e.vertices.size() >= 2);
  }

  // $EXTMIN/$EXTMAX bracket every coordinate actually written.
  for (const auto& e : doc.entities) {
    for (const Vec2& v : e.vertices) {
      if (e.type == "TEXT") continue;  // labels are placed outside the extents
      CHECK(v.x >= doc.extmin.x - 1e-4);
      CHECK(v.y >= doc.extmin.y - 1e-4);
      CHECK(v.x <= doc.extmax.x + 1e-4);
      CHECK(v.y <= doc.extmax.y + 1e-4);
    }
  }
  std::error_code ec;
  fs::remove(path, ec);
}

TEST_CASE("plan/dxf_geometry_matches_the_model") {
  const PlanModel& m = default_plan();
  std::string text;
  REQUIRE(build_dxf(m, DxfOptions{}, &text).ok());
  const DxfDoc doc = dxf_parse(text);

  const std::vector<const DxfEntity*> walls = doc.on_layer("WALLS", "POLYLINE");
  // One closed footprint per SOLID piece of wall: the two doors and the
  // window each split their wall, so 6 walls + 3 openings = 9 pieces.
  CHECK(walls.size() == m.walls.size() + m.openings.size());
  for (const auto* w : walls) {
    CHECK(w->closed);
    CHECK(w->vertices.size() == 4);
  }

  // Recompute one wall's footprint here, from the model, independently of
  // the writer, and find it in the file. The east wall has no openings, so
  // it is exactly one rectangle.
  const WallSegment* east = nullptr;
  for (const auto& w : m.walls) {
    if (std::fabs(w.midpoint().x - kEastX) < 0.05 && std::fabs(w.direction().x) < 0.05) {
      east = &w;
    }
  }
  REQUIRE(east != nullptr);
  const Vec2 d = east->direction();
  const Vec2 n = left_normal(d);
  const double h = east->thickness_m * 0.5;
  const std::vector<Vec2> want = {east->a + n * h, east->b + n * h, east->b - n * h,
                                  east->a - n * h};
  bool found = false;
  for (const auto* w : walls) {
    double worst = 0.0;
    for (std::size_t k = 0; k < 4; ++k) worst = std::max(worst, distance(w->vertices[k], want[k]));
    // The writer rounds to DxfOptions::decimals (5) => 10 micrometres.
    if (worst < 1e-4) found = true;
  }
  CHECK(found);

  // Openings: one 2-vertex polyline each, across the gap, at the right width.
  const std::vector<const DxfEntity*> openings = doc.on_layer("OPENINGS", "POLYLINE");
  REQUIRE(openings.size() == m.openings.size());
  for (const auto* e : openings) {
    REQUIRE(e->vertices.size() == 2);
    bool matched = false;
    for (const auto& op : m.openings) {
      if (distance(e->vertices[0], op.a) < 1e-4 && distance(e->vertices[1], op.b) < 1e-4) {
        CHECK(distance(e->vertices[0], e->vertices[1]) ==
              doctest::Approx(op.width_m).epsilon(1e-4));
        matched = true;
      }
    }
    CHECK(matched);
  }

  // Rooms: one closed polyline each, with the model's own vertices.
  const std::vector<const DxfEntity*> rooms = doc.on_layer("ROOMS", "POLYLINE");
  REQUIRE(rooms.size() == m.rooms.size());
  for (const auto* e : rooms) {
    CHECK(e->closed);
    bool matched = false;
    for (const auto& r : m.rooms) {
      if (e->vertices.size() != r.polygon.size()) continue;
      double worst = 0.0;
      for (std::size_t k = 0; k < r.polygon.size(); ++k) {
        worst = std::max(worst, distance(e->vertices[k], r.polygon[k]));
      }
      if (worst < 1e-4) matched = true;
    }
    CHECK(matched);
    // A room polyline's own area, recomputed from the FILE, must equal the
    // area the model reported.
    bool area_ok = false;
    for (const auto& r : m.rooms) {
      if (std::fabs(polygon_area(e->vertices) - r.area_m2) < 1e-3) area_ok = true;
    }
    CHECK(area_ok);
  }
}

TEST_CASE("plan/dxf_room_area_labels") {
  const PlanModel& m = default_plan();
  std::string text;
  REQUIRE(build_dxf(m, DxfOptions{}, &text).ok());
  const DxfDoc doc = dxf_parse(text);

  const std::vector<const DxfEntity*> labels = doc.on_layer("DIMENSIONS", "TEXT");
  // Two per room (name + area) plus the two overall dimensions.
  CHECK(labels.size() == m.rooms.size() * 2 + 2);

  for (const auto& r : m.rooms) {
    bool name_found = false;
    bool area_found = false;
    for (const auto* t : labels) {
      if (t->text == r.label && distance(t->vertices[0], r.centroid) < 1.0) name_found = true;
      // The area string is the model's number, to 2 dp, in ASCII "m2" —
      // reparsed here and compared numerically, so a formatting bug shows up
      // as a wrong number rather than a string mismatch.
      const std::size_t sp = t->text.find(" m2");
      if (sp != std::string::npos && distance(t->vertices[0], r.centroid) < 1.0) {
        const double v = dxf_number(t->text.substr(0, sp));
        if (std::fabs(v - r.area_m2) < 0.005) area_found = true;
      }
    }
    CAPTURE(r.label);
    CHECK(name_found);
    CHECK(area_found);
  }
}

TEST_CASE("plan/dxf_opening_labels_name_the_kind_and_width") {
  const PlanModel& m = default_plan();
  std::string text;
  REQUIRE(build_dxf(m, DxfOptions{}, &text).ok());
  const DxfDoc doc = dxf_parse(text);
  const std::vector<const DxfEntity*> labels = doc.on_layer("OPENINGS", "TEXT");
  REQUIRE(labels.size() == m.openings.size());

  int doors = 0, windows = 0;
  for (const auto* t : labels) {
    if (t->text.rfind("DOOR", 0) == 0) ++doors;
    if (t->text.rfind("WINDOW", 0) == 0) ++windows;
    const std::size_t sp = t->text.rfind(' ');
    REQUIRE(sp != std::string::npos);
    const double w = dxf_number(t->text.substr(sp + 1));
    bool ok_width = false;
    for (const auto& op : m.openings) {
      if (std::fabs(w - op.width_m) < 0.006) ok_width = true;
    }
    CHECK(ok_width);
  }
  CHECK(doors == 2);
  CHECK(windows == 1);
}

TEST_CASE("plan/dxf_is_locale_free_ascii") {
  const PlanModel& m = default_plan();
  std::string text;
  REQUIRE(build_dxf(m, DxfOptions{}, &text).ok());
  // No comma may appear anywhere: a decimal comma is the classic
  // locale-dependent-formatting failure and it silently breaks every reader.
  CHECK(text.find(',') == std::string::npos);
  for (unsigned char c : text) CHECK(c < 0x80);

  // Layer names are sanitized into R12's alphabet.
  DxfOptions o;
  o.layer_walls = "my walls (ünicode)";
  std::string text2;
  REQUIRE(build_dxf(m, o, &text2).ok());
  const DxfDoc doc = dxf_parse(text2);
  CHECK_FALSE(doc.has_layer("my walls (ünicode)"));
  bool found_sanitized = false;
  for (const auto& l : doc.layers) {
    if (l.rfind("MY_WALLS", 0) == 0) found_sanitized = true;
    for (char c : l) CHECK(c != ' ');
  }
  CHECK(found_sanitized);
}

TEST_CASE("plan/dxf_centerline_mode") {
  const PlanModel& m = default_plan();
  DxfOptions o;
  o.wall_footprint = false;
  std::string text;
  REQUIRE(build_dxf(m, o, &text).ok());
  const DxfDoc doc = dxf_parse(text);
  const std::vector<const DxfEntity*> walls = doc.on_layer("WALLS", "POLYLINE");
  REQUIRE(walls.size() == m.walls.size());
  for (std::size_t k = 0; k < walls.size(); ++k) {
    REQUIRE(walls[k]->vertices.size() == 2);
    CHECK(distance(walls[k]->vertices[0], m.walls[k].a) < 1e-4);
    CHECK(distance(walls[k]->vertices[1], m.walls[k].b) < 1e-4);
    CHECK_FALSE(walls[k]->closed);
  }
}

TEST_CASE("plan/dxf_write_errors") {
  const PlanModel& m = default_plan();
  CHECK(write_dxf(m, DxfOptions{}, "").error() == ScanError::kInvalidArgument);
  CHECK(write_dxf(m, DxfOptions{}, "/nonexistent-dir-a12/x.dxf").error() ==
        ScanError::kFileError);
  CHECK(build_dxf(m, DxfOptions{}, nullptr).error() == ScanError::kInvalidArgument);
}

TEST_CASE("plan/dxf_of_an_empty_plan_is_still_a_valid_file") {
  PlanModel empty;
  std::string text;
  REQUIRE(build_dxf(empty, DxfOptions{}, &text).ok());
  const DxfDoc doc = dxf_parse(text);
  CHECK(doc.saw_eof);
  CHECK(doc.acadver == "AC1009");
  CHECK(doc.has_layer("WALLS"));
  CHECK(doc.on_layer("WALLS", "POLYLINE").empty());
}

// ===========================================================================
// PDF
// ===========================================================================

TEST_CASE("plan/pdf_structure_is_valid") {
  const PlanModel& m = default_plan();
  const std::string path = temp_path("plan", "pdf");
  PdfOptions o;
  o.project = "A12 fixture";
  o.date = "2026-08-15";
  o.reference = "session-0001";
  REQUIRE(write_pdf(m, o, path).ok());
  const std::string bytes = read_file(path);
  REQUIRE(bytes.size() > 512);

  const PdfDoc d = pdf_parse(bytes);
  for (const auto& p : d.problems) {
    CAPTURE(p);
    CHECK(false);
  }
  CHECK(d.header_ok);
  CHECK(d.eof_ok);
  CHECK(d.size == 7);            // 6 objects + the free-list head
  CHECK(d.root_obj == 1);
  CHECK(d.offsets.size() == 6);
  // Offsets strictly increase — objects are written in order, once each.
  for (std::size_t k = 1; k < d.offsets.size(); ++k) CHECK(d.offsets[k] > d.offsets[k - 1]);

  CHECK(d.objects.at(1).find("/Type /Catalog") != std::string::npos);
  CHECK(d.objects.at(1).find("/Pages 2 0 R") != std::string::npos);
  CHECK(d.objects.at(2).find("/Type /Pages") != std::string::npos);
  CHECK(d.objects.at(2).find("/Kids [3 0 R]") != std::string::npos);
  CHECK(d.objects.at(3).find("/Type /Page") != std::string::npos);
  CHECK(d.objects.at(3).find("/Contents 4 0 R") != std::string::npos);
  CHECK(d.objects.at(5).find("/BaseFont /Helvetica") != std::string::npos);
  CHECK(d.objects.at(6).find("/BaseFont /Helvetica-Bold") != std::string::npos);

  // A4 portrait is 595.28 x 841.89 pt; the default is landscape.
  CHECK(d.media[0] == doctest::Approx(0.0));
  CHECK(d.media[1] == doctest::Approx(0.0));
  CHECK(d.media[2] == doctest::Approx(841.89).epsilon(0.001));
  CHECK(d.media[3] == doctest::Approx(595.28).epsilon(0.001));

  CHECK_FALSE(d.content.empty());
  // Nothing derived from the clock: a re-export tomorrow must be identical.
  CHECK(bytes.find("/CreationDate") == std::string::npos);
  CHECK(bytes.find("/ModDate") == std::string::npos);
  // The caller's strings did make it in.
  CHECK(d.content.find("A12 fixture") != std::string::npos);
  CHECK(d.content.find("2026-08-15") != std::string::npos);
  CHECK(d.content.find("session-0001") != std::string::npos);
  std::error_code ec;
  fs::remove(path, ec);
}

TEST_CASE("plan/pdf_geometry_lands_where_the_scale_says") {
  const PlanModel& m = default_plan();
  PdfOptions o;
  std::string bytes;
  REQUIRE(build_pdf(m, o, &bytes).ok());
  const PdfDoc d = pdf_parse(bytes);
  REQUIRE(d.problems.empty());

  const int denom = auto_scale_denominator(m.bounds, o);
  CHECK(denom == 50);  // an 8 x 5 m plan on landscape A4
  CHECK(d.content.find("1:50") != std::string::npos);

  // 1 m at 1:50 is 20 mm on paper = 20 * 72 / 25.4 pt.
  const double s = (1000.0 / static_cast<double>(denom)) * (72.0 / 25.4);

  const std::vector<Vec2> pts = pdf_plan_points(d.content);
  REQUIRE(pts.size() > 20);
  PlanBounds page;
  for (const Vec2& p : pts) page.expand(p);

  // What the writer drew is the wall footprints; recompute their extent here
  // from the model, independently.
  PlanBounds model;
  for (const auto& w : m.walls) {
    const Vec2 dir = w.direction();
    const Vec2 nrm = left_normal(dir);
    const double h = w.thickness_m * 0.5;
    model.expand(w.a + nrm * h);
    model.expand(w.a - nrm * h);
    model.expand(w.b + nrm * h);
    model.expand(w.b - nrm * h);
  }
  REQUIRE(model.valid);
  CHECK(page.width() == doctest::Approx(model.width() * s).epsilon(0.002));
  CHECK(page.height() == doctest::Approx(model.height() * s).epsilon(0.002));

  // Everything is inside the page.
  for (const Vec2& p : pts) {
    CHECK(p.x >= 0.0);
    CHECK(p.y >= 0.0);
    CHECK(p.x <= d.media[2]);
    CHECK(p.y <= d.media[3]);
  }

  // Spot-check the geometry itself: EVERY footprint corner of the model,
  // mapped through (page.min - model.min) * s, must appear in the content
  // stream. That pins the scale, the origin and the shape all at once, using
  // nothing from the writer but the bytes it produced.
  int corners_checked = 0;
  for (const auto& w : m.walls) {
    const Vec2 dir = w.direction();
    const Vec2 nrm = left_normal(dir);
    const double h = w.thickness_m * 0.5;
    for (const Vec2& corner : {w.a + nrm * h, w.a - nrm * h, w.b + nrm * h, w.b - nrm * h}) {
      const Vec2 want{page.min_x + (corner.x - model.min_x) * s,
                      page.min_y + (corner.y - model.min_y) * s};
      bool hit = false;
      for (const Vec2& p : pts) {
        if (std::fabs(p.x - want.x) < 0.05 && std::fabs(p.y - want.y) < 0.05) hit = true;
      }
      // A corner that an opening cut away is legitimately absent; a corner on
      // a wall with no openings is not.
      bool wall_has_openings = false;
      for (const auto& op : m.openings) {
        if (op.wall_id == w.id) wall_has_openings = true;
      }
      if (!wall_has_openings) {
        CAPTURE(w.id);
        CHECK(hit);
        ++corners_checked;
      }
    }
  }
  CHECK(corners_checked >= 12);

  // Room area labels are on the sheet, as text, with the model's numbers.
  for (const auto& r : m.rooms) {
    CAPTURE(r.label);
    CHECK(d.content.find("(" + r.label + ")") != std::string::npos);
  }
}

TEST_CASE("plan/pdf_scale_ladder_and_sheets") {
  PlanBounds small;
  small.expand(Vec2{0, 0});
  small.expand(Vec2{3, 2});
  PlanBounds big;
  big.expand(Vec2{0, 0});
  big.expand(Vec2{120, 80});

  PdfOptions a4;
  PdfOptions a3;
  a3.sheet = SheetSize::kA3;

  const int s_small = auto_scale_denominator(small, a4);
  const int s_big = auto_scale_denominator(big, a4);
  CHECK(s_small < s_big);          // a smaller building gets a larger drawing
  CHECK(s_small == 20);
  CHECK(s_big >= 200);
  // The same building on A3 fits at the same scale or a finer one.
  CHECK(auto_scale_denominator(big, a3) <= s_big);

  double w = 0, h = 0;
  sheet_size_pt(a4, &w, &h);
  CHECK(w == doctest::Approx(841.89).epsilon(0.001));
  CHECK(h == doctest::Approx(595.28).epsilon(0.001));
  PdfOptions portrait = a4;
  portrait.orientation = SheetOrientation::kPortrait;
  sheet_size_pt(portrait, &w, &h);
  CHECK(w == doctest::Approx(595.28).epsilon(0.001));
  CHECK(h == doctest::Approx(841.89).epsilon(0.001));
  sheet_size_pt(a3, &w, &h);
  CHECK(w == doctest::Approx(1190.55).epsilon(0.001));
  CHECK(std::string(to_string(SheetSize::kA3)) == "A3");
}

TEST_CASE("plan/pdf_pinned_scale_is_honoured") {
  const PlanModel& m = default_plan();
  PdfOptions o;
  o.scale_denominator = 200;
  std::string bytes;
  REQUIRE(build_pdf(m, o, &bytes).ok());
  const PdfDoc d = pdf_parse(bytes);
  REQUIRE(d.problems.empty());
  CHECK(d.content.find("1:200") != std::string::npos);

  const double s = (1000.0 / 200.0) * (72.0 / 25.4);
  const std::vector<Vec2> pts = pdf_plan_points(d.content);
  PlanBounds page;
  for (const Vec2& p : pts) page.expand(p);
  // 8 m at 1:200 is 40 mm = 113.4 pt.
  CHECK(page.width() == doctest::Approx(8.0 * s).epsilon(0.02));
}

TEST_CASE("plan/pdf_north_arrow_is_labelled_as_a_placeholder") {
  const PlanModel& m = default_plan();
  PdfOptions o;
  std::string assumed;
  REQUIRE(build_pdf(m, o, &assumed).ok());
  // A PDF literal string escapes its parentheses, so this is also the
  // escaping test: the label reaches the file as "N \(ASSUMED\)".
  CHECK(assumed.find("(N \\(ASSUMED\\)) Tj") != std::string::npos);

  o.north_known = true;
  o.north_angle_deg = 37.0;
  std::string known;
  REQUIRE(build_pdf(m, o, &known).ok());
  CHECK(known.find("ASSUMED") == std::string::npos);
  CHECK(known.find("(N) Tj") != std::string::npos);
  CHECK(known != assumed);
}

TEST_CASE("plan/pdf_write_errors_and_empty_plan") {
  const PlanModel& m = default_plan();
  CHECK(write_pdf(m, PdfOptions{}, "").error() == ScanError::kInvalidArgument);
  CHECK(write_pdf(m, PdfOptions{}, "/nonexistent-dir-a12/x.pdf").error() ==
        ScanError::kFileError);
  CHECK(build_pdf(m, PdfOptions{}, nullptr).error() == ScanError::kInvalidArgument);

  PlanModel empty;
  std::string bytes;
  REQUIRE(build_pdf(empty, PdfOptions{}, &bytes).ok());
  const PdfDoc d = pdf_parse(bytes);
  for (const auto& p : d.problems) {
    CAPTURE(p);
    CHECK(false);
  }
  CHECK(d.header_ok);
  CHECK(d.eof_ok);
}

// ===========================================================================
// Determinism
// ===========================================================================

TEST_CASE("plan/extraction_is_bit_deterministic") {
  const PlanInput in = input_of(default_cloud());
  PlanModel a, b;
  REQUIRE(extract_floor_plan(in, PlanOptions{}, &a).ok());
  REQUIRE(extract_floor_plan(in, PlanOptions{}, &b).ok());

  REQUIRE(a.walls.size() == b.walls.size());
  for (std::size_t k = 0; k < a.walls.size(); ++k) {
    CHECK(a.walls[k].id == b.walls[k].id);
    CHECK(a.walls[k].a.x == b.walls[k].a.x);
    CHECK(a.walls[k].a.y == b.walls[k].a.y);
    CHECK(a.walls[k].b.x == b.walls[k].b.x);
    CHECK(a.walls[k].b.y == b.walls[k].b.y);
    CHECK(a.walls[k].thickness_m == b.walls[k].thickness_m);
    CHECK(a.walls[k].confidence == b.walls[k].confidence);
    CHECK(a.walls[k].rms_residual_m == b.walls[k].rms_residual_m);
  }
  REQUIRE(a.openings.size() == b.openings.size());
  for (std::size_t k = 0; k < a.openings.size(); ++k) {
    CHECK(a.openings[k].width_m == b.openings[k].width_m);
    CHECK(a.openings[k].kind == b.openings[k].kind);
    CHECK(a.openings[k].sill == b.openings[k].sill);
  }
  REQUIRE(a.rooms.size() == b.rooms.size());
  for (std::size_t k = 0; k < a.rooms.size(); ++k) {
    CHECK(a.rooms[k].label == b.rooms[k].label);
    CHECK(a.rooms[k].area_m2 == b.rooms[k].area_m2);
    CHECK(a.rooms[k].polygon.size() == b.rooms[k].polygon.size());
  }
  CHECK(a.stats.dominant_angle_rad == b.stats.dominant_angle_rad);
}

TEST_CASE("plan/writers_are_byte_deterministic") {
  const PlanModel& m = default_plan();
  std::string dxf1, dxf2, pdf1, pdf2;
  REQUIRE(build_dxf(m, DxfOptions{}, &dxf1).ok());
  REQUIRE(build_dxf(m, DxfOptions{}, &dxf2).ok());
  CHECK(dxf1 == dxf2);

  PdfOptions o;
  o.date = "2026-08-15";
  REQUIRE(build_pdf(m, o, &pdf1).ok());
  REQUIRE(build_pdf(m, o, &pdf2).ok());
  CHECK(pdf1 == pdf2);

  // ... and a second extraction of the same cloud writes the same bytes.
  PlanModel again;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), PlanOptions{}, &again).ok());
  std::string dxf3, pdf3;
  REQUIRE(build_dxf(again, DxfOptions{}, &dxf3).ok());
  REQUIRE(build_pdf(again, o, &pdf3).ok());
  CHECK(dxf1 == dxf3);
  CHECK(pdf1 == pdf3);
}

TEST_CASE("plan/a_different_seed_is_still_a_correct_plan") {
  PlanOptions o;
  o.walls.seed = 0xDEADBEEFCAFEBABEull;
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), o, &m).ok());
  CHECK(m.walls.size() == 6);
  REQUIRE(m.rooms.size() == 3);
  double total = 0.0;
  for (const auto& r : m.rooms) total += r.area_m2;
  CHECK(total == doctest::Approx(kCorridorArea + kRoomAArea + kRoomBArea).epsilon(0.02));
}

// ===========================================================================
// Progress, cancellation, PageStore input, and the A1 seam
// ===========================================================================

TEST_CASE("plan/progress_is_monotone_and_reaches_one") {
  struct Sink {
    std::vector<float> seen;
  } sink;
  auto cb = [](float f, void* ud) { static_cast<Sink*>(ud)->seen.push_back(f); };
  PlanModel m;
  REQUIRE(extract_floor_plan(input_of(default_cloud()), PlanOptions{}, &m, cb, &sink).ok());
  REQUIRE(sink.seen.size() >= 2);
  for (std::size_t k = 1; k < sink.seen.size(); ++k) CHECK(sink.seen[k] >= sink.seen[k - 1]);
  CHECK(sink.seen.front() == doctest::Approx(0.f));
  CHECK(sink.seen.back() == doctest::Approx(1.f));
}

TEST_CASE("plan/a_cancelled_extraction_returns_cancelled") {
  PlanCancelToken token;
  token.request_cancel();
  CHECK(token.cancelled());
  PlanModel m;
  CHECK(extract_floor_plan(input_of(default_cloud()), PlanOptions{}, &m, nullptr, nullptr,
                           &token)
            .error() == ScanError::kCancelled);
  token.reset();
  CHECK_FALSE(token.cancelled());
  CHECK(extract_floor_plan(input_of(default_cloud()), PlanOptions{}, &m, nullptr, nullptr,
                           &token)
            .ok());
}

TEST_CASE("plan/extraction_from_a_PageStore_matches_the_span_path") {
  PageStoreConfig cfg;
  cfg.page_capacity = 30000;  // force several pages
  cfg.max_pages = 64;
  PageStore store(cfg);
  const std::vector<PointVertex>& pts = default_cloud();
  std::size_t off = 0;
  while (off < pts.size()) {
    const std::size_t n = std::min<std::size_t>(20000, pts.size() - off);
    REQUIRE(store.append(StreamId::kSlamMap, Span<const PointVertex>(pts.data() + off, n),
                         static_cast<std::int64_t>(off))
                .ok());
    off += n;
  }
  CHECK(store.page_count() > 1);

  PlanInput in;
  in.store = &store;
  PlanModel from_store;
  REQUIRE(extract_floor_plan(in, PlanOptions{}, &from_store).ok());

  const PlanModel& from_span = default_plan();
  REQUIRE(from_store.walls.size() == from_span.walls.size());
  for (std::size_t k = 0; k < from_store.walls.size(); ++k) {
    CHECK(from_store.walls[k].a.x == doctest::Approx(from_span.walls[k].a.x));
    CHECK(from_store.walls[k].b.y == doctest::Approx(from_span.walls[k].b.y));
  }
  CHECK(from_store.rooms.size() == from_span.rooms.size());

  // A stream filter that matches nothing yields an empty plan, not an error.
  const StreamId other = StreamId::kLidarD6;
  in.streams = Span<const StreamId>(&other, 1);
  PlanModel filtered;
  CHECK(extract_floor_plan(in, PlanOptions{}, &filtered).ok());
  CHECK(filtered.walls.empty());
}

TEST_CASE("plan/the_A1_seam_still_works") {
  PageStore store;
  const std::vector<PointVertex>& pts = default_cloud();
  std::size_t off = 0;
  while (off < pts.size()) {
    const std::size_t n = std::min<std::size_t>(50000, pts.size() - off);
    REQUIRE(store.append(StreamId::kSlamMap, Span<const PointVertex>(pts.data() + off, n), 0)
                .ok());
    off += n;
  }

  std::unique_ptr<FloorPlanExtractor> ex = make_floor_plan_extractor();
  REQUIRE(ex != nullptr);
  CHECK(ex->progress() == doctest::Approx(0.f));
  FloorPlan fp;
  SliceOptions opts;  // the A1 defaults, unchanged
  CHECK(opts.z_min_m == 1.0f);
  CHECK(opts.z_max_m == 1.5f);
  REQUIRE(ex->extract(store, opts, &fp).ok());
  CHECK(ex->progress() == doctest::Approx(1.f));

  CHECK(fp.scale_m_per_unit == 1.0f);
  int walls = 0, openings = 0, rooms = 0;
  for (const auto& pl : fp.polylines) {
    CHECK(pl.xy.size() % 2 == 0);
    if (pl.layer == kPolylineLayerWall) ++walls;
    if (pl.layer == kPolylineLayerOpening) ++openings;
    if (pl.layer == kPolylineLayerRoom) {
      ++rooms;
      CHECK(pl.closed);
    }
  }
  CHECK(walls == 6);
  CHECK(openings == 3);
  CHECK(rooms == 3);

  CHECK(ex->extract(store, opts, nullptr).error() == ScanError::kInvalidArgument);
}

TEST_CASE("plan/extract_walls_rejects_a_mismatched_sill_lattice") {
  const PlanInput in = input_of(default_cloud());
  OccupancyGrid main_grid, other;
  BandOptions band;
  band.min_points = 3;
  REQUIRE(build_occupancy(in, band, &main_grid, nullptr).ok());
  BandOptions coarse = band;
  coarse.res_m = 0.05;
  REQUIRE(build_occupancy(in, coarse, &other, nullptr).ok());

  PlanModel m;
  CHECK(extract_walls(main_grid, &other, PlanOptions{}, &m).error() ==
        ScanError::kInvalidArgument);
  CHECK(extract_walls(main_grid, nullptr, PlanOptions{}, &m).ok());
  CHECK(extract_walls(main_grid, nullptr, PlanOptions{}, nullptr).error() ==
        ScanError::kInvalidArgument);
}

TEST_CASE("plan/enum_names_are_stable") {
  CHECK(std::string(to_string(UpAxis::kZ)) == "z");
  CHECK(std::string(to_string(WallEvidence::kPairedFaces)) == "paired-faces");
  CHECK(std::string(to_string(WallEvidence::kSingleFace)) == "single-face");
  CHECK(std::string(to_string(OpeningKind::kDoorCandidate)) == "door");
  CHECK(std::string(to_string(OpeningKind::kWindowCandidate)) == "window");
  CHECK(std::string(to_string(OpeningKind::kWideOpening)) == "wide-opening");
  CHECK(std::string(to_string(OpeningKind::kNarrowGap)) == "narrow-gap");
  CHECK(std::string(to_string(SillCheck::kSolidBelow)) == "solid-below");
  CHECK(std::string(to_string(SillCheck::kOpenBelow)) == "open-below");
  CHECK(std::string(to_string(SillCheck::kNoData)) == "no-data");
  CHECK(std::string(to_string(SillCheck::kNotChecked)) == "not-checked");
}
