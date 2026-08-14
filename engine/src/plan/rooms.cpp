// rooms.cpp — closed rooms and their areas, by planar-face traversal of the
// wall network.
//
// WHY FACES AND NOT A FLOOD FILL. Flood-filling the free cells of the
// occupancy grid is the obvious approach and it is wrong twice over: door
// gaps let the fill leak from room to corridor to room (so a three-room flat
// comes back as one region), and the resulting area is quantized to the grid,
// which on a 20 m2 room at 2 cm is +-1.8% before any other error — the whole
// tolerance budget, spent on rasterization. The wall network already knows
// where the doorways are (they are bridged Openings), so its planar faces are
// the rooms, and their areas are exact rational functions of the fitted line
// positions.
//
// THE INSET. A face is bounded by wall CENTERLINES. What a floor plan labels
// is net internal area, so each edge is pushed toward the interior by that
// wall's room_inset_m() and consecutive offset lines are re-intersected.
// room_inset_m() is half the thickness for a wall whose two faces were both
// scanned, and ZERO for a single-face wall — because for those, the fitted
// line already IS the visible interior face and there is nothing to inset.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "plan_internal.h"

namespace scanengine {
namespace plan {
namespace {

struct HalfEdge {
  std::uint32_t origin = 0;
  std::uint32_t target = 0;
  std::uint32_t twin = 0;
  std::uint32_t wall = 0;  // index into the caller's wall vector
  double angle = 0.0;      // direction from origin, in (-pi, pi]
};

std::uint32_t intern_vertex(std::vector<Vec2>* verts, Vec2 p, double tol) {
  for (std::uint32_t k = 0; k < verts->size(); ++k) {
    if (distance((*verts)[k], p) <= tol) return k;
  }
  verts->push_back(p);
  return static_cast<std::uint32_t>(verts->size() - 1);
}

bool offset_intersection(Vec2 p1, Vec2 d1, Vec2 p2, Vec2 d2, Vec2* out) {
  const double den = cross(d1, d2);
  if (std::fabs(den) < 1e-9) return false;
  *out = p1 + d1 * (cross(p2 - p1, d2) / den);
  return true;
}

// Push every edge of a CCW ring inward by its own inset and re-intersect.
std::vector<Vec2> inset_polygon(const std::vector<Vec2>& poly, const std::vector<double>& inset) {
  const std::size_t n = poly.size();
  if (n < 3 || inset.size() != n) return poly;

  struct OffLine {
    Vec2 p;
    Vec2 d;
  };
  std::vector<OffLine> lines;
  lines.reserve(n);
  for (std::size_t k = 0; k < n; ++k) {
    const Vec2 a = poly[k];
    const Vec2 b = poly[(k + 1) % n];
    const Vec2 d = b - a;
    if (length(d) < 1e-9) continue;
    const Vec2 u = normalized(d);
    // CCW ring => interior on the left => inward is +left_normal.
    lines.push_back(OffLine{a + left_normal(u) * inset[k], u});
  }
  if (lines.size() < 3) return poly;

  // Collapse runs of collinear edges (they come from splitting a wall at a
  // T-junction); two parallel offset lines have no intersection to compute.
  std::vector<OffLine> uniq;
  for (const auto& l : lines) {
    if (!uniq.empty() && std::fabs(cross(uniq.back().d, l.d)) < 1e-9 &&
        std::fabs(cross(l.d, l.p - uniq.back().p)) < 1e-6) {
      continue;
    }
    uniq.push_back(l);
  }
  while (uniq.size() >= 2 && std::fabs(cross(uniq.back().d, uniq.front().d)) < 1e-9 &&
         std::fabs(cross(uniq.front().d, uniq.front().p - uniq.back().p)) < 1e-6) {
    uniq.pop_back();
  }
  if (uniq.size() < 3) return poly;

  std::vector<Vec2> out;
  out.reserve(uniq.size());
  for (std::size_t k = 0; k < uniq.size(); ++k) {
    const OffLine& a = uniq[k];
    const OffLine& b = uniq[(k + 1) % uniq.size()];
    Vec2 x{};
    if (offset_intersection(a.p, a.d, b.p, b.d, &x)) {
      out.push_back(x);
    } else {
      out.push_back(b.p);
    }
  }
  if (out.size() < 3) return poly;
  return out;
}

}  // namespace

Status detect_rooms(const std::vector<WallSegment>& walls, const RoomOptions& opts,
                    std::vector<Room>* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "plan: detect_rooms(out == null)");
  }
  out->clear();
  if (!opts.enabled || walls.size() < 3) return kOkStatus;

  const double tol = opts.weld_m;

  // --- vertices ------------------------------------------------------------
  std::vector<Vec2> verts;
  std::vector<std::uint32_t> wa(walls.size()), wb(walls.size());
  for (std::size_t k = 0; k < walls.size(); ++k) {
    wa[k] = intern_vertex(&verts, walls[k].a, tol);
    wb[k] = intern_vertex(&verts, walls[k].b, tol);
  }

  // --- split walls where another wall's endpoint lands on them -------------
  struct SubEdge {
    std::uint32_t u = 0;
    std::uint32_t v = 0;
    std::uint32_t wall = 0;
  };
  std::vector<SubEdge> edges;
  for (std::size_t k = 0; k < walls.size(); ++k) {
    const Vec2 a = verts[wa[k]];
    const Vec2 b = verts[wb[k]];
    const double len = distance(a, b);
    if (len < 1e-6) continue;
    const Vec2 d = normalized(b - a);

    std::vector<std::pair<double, std::uint32_t>> cuts;
    cuts.push_back({0.0, wa[k]});
    cuts.push_back({len, wb[k]});
    for (std::uint32_t vi = 0; vi < verts.size(); ++vi) {
      if (vi == wa[k] || vi == wb[k]) continue;
      const Vec2 q = verts[vi];
      const double t = dot(q - a, d);
      if (t <= tol || t >= len - tol) continue;
      if (std::fabs(cross(d, q - a)) > tol) continue;
      cuts.push_back({t, vi});
    }
    std::sort(cuts.begin(), cuts.end(),
              [](const std::pair<double, std::uint32_t>& x,
                 const std::pair<double, std::uint32_t>& y) {
                if (x.first != y.first) return x.first < y.first;
                return x.second < y.second;
              });
    for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
      if (cuts[c].second == cuts[c + 1].second) continue;
      if (cuts[c + 1].first - cuts[c].first < 1e-6) continue;
      edges.push_back(SubEdge{cuts[c].second, cuts[c + 1].second,
                              static_cast<std::uint32_t>(k)});
    }
  }
  if (edges.size() < 3) return kOkStatus;

  // --- half-edge structure -------------------------------------------------
  std::vector<HalfEdge> he;
  he.reserve(edges.size() * 2);
  for (const auto& e : edges) {
    const std::uint32_t id = static_cast<std::uint32_t>(he.size());
    HalfEdge h1;
    h1.origin = e.u;
    h1.target = e.v;
    h1.twin = id + 1;
    h1.wall = e.wall;
    const Vec2 d1 = verts[e.v] - verts[e.u];
    h1.angle = std::atan2(d1.y, d1.x);
    HalfEdge h2;
    h2.origin = e.v;
    h2.target = e.u;
    h2.twin = id;
    h2.wall = e.wall;
    h2.angle = std::atan2(-d1.y, -d1.x);
    he.push_back(h1);
    he.push_back(h2);
  }

  std::vector<std::vector<std::uint32_t>> outgoing(verts.size());
  for (std::uint32_t k = 0; k < he.size(); ++k) outgoing[he[k].origin].push_back(k);
  for (auto& lst : outgoing) {
    std::sort(lst.begin(), lst.end(), [&](std::uint32_t x, std::uint32_t y) {
      if (he[x].angle != he[y].angle) return he[x].angle < he[y].angle;
      return x < y;
    });
  }
  std::vector<std::uint32_t> slot(he.size(), 0);
  for (const auto& lst : outgoing) {
    for (std::uint32_t s = 0; s < lst.size(); ++s) slot[lst[s]] = s;
  }

  // next(h) = the half-edge one step CLOCKWISE from twin(h) around twin's
  // origin. This walks every face with its interior on the left, so bounded
  // faces come out counter-clockwise (positive area) and the unbounded one
  // comes out clockwise.
  auto next_of = [&](std::uint32_t h) {
    const std::uint32_t t = he[h].twin;
    const std::vector<std::uint32_t>& lst = outgoing[he[t].origin];
    const std::uint32_t s = slot[t];
    const std::uint32_t prev = (s == 0) ? static_cast<std::uint32_t>(lst.size() - 1) : s - 1;
    return lst[prev];
  };

  // --- faces ---------------------------------------------------------------
  std::vector<char> seen(he.size(), 0);
  struct Face {
    std::vector<Vec2> poly;
    std::vector<double> inset;
    std::vector<std::uint32_t> wall_ids;
    double signed_area = 0.0;
  };
  std::vector<Face> faces;
  const std::size_t guard = he.size() * 4 + 8;

  for (std::uint32_t start = 0; start < he.size(); ++start) {
    if (seen[start]) continue;
    Face f;
    std::uint32_t h = start;
    std::size_t steps = 0;
    while (!seen[h] && steps < guard) {
      seen[h] = 1;
      f.poly.push_back(verts[he[h].origin]);
      const WallSegment& w = walls[he[h].wall];
      f.inset.push_back(opts.inset_by_thickness ? w.room_inset_m() : 0.0);
      f.wall_ids.push_back(he[h].wall);
      h = next_of(h);
      ++steps;
    }
    if (f.poly.size() < 3) continue;
    f.signed_area = polygon_signed_area(f.poly);
    faces.push_back(std::move(f));
  }

  // --- rooms ---------------------------------------------------------------
  std::vector<Room> rooms;
  for (const auto& f : faces) {
    if (f.signed_area <= 0.0) continue;  // the unbounded face, and degenerates
    if (f.signed_area < opts.min_area_m2 * 0.25) continue;

    Room r;
    r.polygon = opts.inset_by_thickness ? inset_polygon(f.poly, f.inset) : f.poly;
    r.area_m2 = std::fabs(polygon_signed_area(r.polygon));
    if (r.area_m2 < opts.min_area_m2) continue;
    r.perimeter_m = polygon_perimeter(r.polygon);
    r.centroid = polygon_centroid(r.polygon);

    double conf = 0.0;
    double wlen = 0.0;
    bool measured = true;
    for (std::size_t k = 0; k < f.wall_ids.size(); ++k) {
      const WallSegment& w = walls[f.wall_ids[k]];
      const double seg = distance(f.poly[k], f.poly[(k + 1) % f.poly.size()]);
      conf += static_cast<double>(w.confidence) * seg;
      wlen += seg;
      if (w.coverage < 0.85) measured = false;
    }
    r.confidence = wlen > 0.0 ? static_cast<float>(conf / wlen) : 0.f;
    r.fully_measured = measured;
    rooms.push_back(std::move(r));
  }

  // Deterministic identity: bottom-to-top, then left-to-right, reading order.
  // The centroids are BANDED to 0.25 m first, because two rooms that face the
  // same corridor have centroids a few millimetres apart in y and an exact
  // comparison would order them by that millimetre instead of left-to-right —
  // stable, but not the order a human reading the sheet expects. The label
  // ends up in the DXF and on the PDF, so it has to be both.
  auto band = [](double v) { return std::floor(v / 0.25 + 0.5); };
  std::sort(rooms.begin(), rooms.end(), [&](const Room& a, const Room& b) {
    const double ay = band(a.centroid.y), by = band(b.centroid.y);
    if (ay != by) return ay < by;
    const double ax = band(a.centroid.x), bx = band(b.centroid.x);
    if (ax != bx) return ax < bx;
    if (a.area_m2 != b.area_m2) return a.area_m2 < b.area_m2;
    return a.centroid.x < b.centroid.x;
  });
  for (std::size_t k = 0; k < rooms.size(); ++k) {
    rooms[k].id = static_cast<std::uint32_t>(k + 1);
    rooms[k].label = "R" + fmt_int(static_cast<long long>(k + 1));
  }
  *out = std::move(rooms);
  return kOkStatus;
}

}  // namespace plan
}  // namespace scanengine
