// plan_model.cpp — the model's own small vocabulary: enum names, polygon
// arithmetic, and the flattening into A1's legacy Polyline2D bag.
#include "scanengine/plan/plan_model.h"

#include <algorithm>
#include <cstddef>

#include "plan_internal.h"

namespace scanengine {
namespace plan {

const char* to_string(UpAxis a) noexcept {
  switch (a) {
    case UpAxis::kZ: return "z";
    case UpAxis::kY: return "y";
    case UpAxis::kX: return "x";
  }
  return "?";
}

const char* to_string(WallEvidence e) noexcept {
  switch (e) {
    case WallEvidence::kSingleFace: return "single-face";
    case WallEvidence::kPairedFaces: return "paired-faces";
  }
  return "?";
}

const char* to_string(OpeningKind k) noexcept {
  switch (k) {
    case OpeningKind::kUnknown: return "unknown";
    case OpeningKind::kNarrowGap: return "narrow-gap";
    case OpeningKind::kDoorCandidate: return "door";
    case OpeningKind::kWindowCandidate: return "window";
    case OpeningKind::kWideOpening: return "wide-opening";
  }
  return "?";
}

const char* to_string(SillCheck c) noexcept {
  switch (c) {
    case SillCheck::kNotChecked: return "not-checked";
    case SillCheck::kNoData: return "no-data";
    case SillCheck::kOpenBelow: return "open-below";
    case SillCheck::kSolidBelow: return "solid-below";
  }
  return "?";
}

void PlanBounds::expand(Vec2 p) {
  if (!valid) {
    min_x = max_x = p.x;
    min_y = max_y = p.y;
    valid = true;
    return;
  }
  min_x = std::min(min_x, p.x);
  min_y = std::min(min_y, p.y);
  max_x = std::max(max_x, p.x);
  max_y = std::max(max_y, p.y);
}

void PlanBounds::expand(const PlanBounds& other) {
  if (!other.valid) return;
  expand(Vec2{other.min_x, other.min_y});
  expand(Vec2{other.max_x, other.max_y});
}

double WallSegment::angle_rad() const {
  const Vec2 d = direction();
  double t = std::atan2(d.y, d.x);
  // Fold to [0, pi): a wall segment has no orientation, only a heading.
  const double pi = 3.14159265358979323846;
  while (t < 0.0) t += pi;
  while (t >= pi) t -= pi;
  return t;
}

const WallSegment* PlanModel::wall_by_id(std::uint32_t id) const {
  for (const auto& w : walls) {
    if (w.id == id) return &w;
  }
  return nullptr;
}

// --- polygon helpers --------------------------------------------------------

double polygon_signed_area(const std::vector<Vec2>& poly) {
  const std::size_t n = poly.size();
  if (n < 3) return 0.0;
  double acc = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2& p = poly[i];
    const Vec2& q = poly[(i + 1) % n];
    acc += p.x * q.y - q.x * p.y;
  }
  return acc * 0.5;
}

double polygon_perimeter(const std::vector<Vec2>& poly) {
  const std::size_t n = poly.size();
  if (n < 2) return 0.0;
  double acc = 0.0;
  for (std::size_t i = 0; i < n; ++i) acc += distance(poly[i], poly[(i + 1) % n]);
  return acc;
}

Vec2 polygon_centroid(const std::vector<Vec2>& poly) {
  const std::size_t n = poly.size();
  if (n == 0) return Vec2{};
  const double a = polygon_signed_area(poly);
  if (std::fabs(a) < 1e-12) {
    // Degenerate ring: the vertex mean is the only defensible answer.
    Vec2 m{};
    for (const auto& p : poly) {
      m.x += p.x;
      m.y += p.y;
    }
    return Vec2{m.x / static_cast<double>(n), m.y / static_cast<double>(n)};
  }
  double cx = 0.0, cy = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Vec2& p = poly[i];
    const Vec2& q = poly[(i + 1) % n];
    const double w = p.x * q.y - q.x * p.y;
    cx += (p.x + q.x) * w;
    cy += (p.y + q.y) * w;
  }
  return Vec2{cx / (6.0 * a), cy / (6.0 * a)};
}

PlanBounds polygon_bounds(const std::vector<Vec2>& poly) {
  PlanBounds b;
  for (const auto& p : poly) b.expand(p);
  return b;
}

bool point_in_polygon(const std::vector<Vec2>& poly, Vec2 p) {
  const std::size_t n = poly.size();
  if (n < 3) return false;
  bool inside = false;
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2& a = poly[i];
    const Vec2& b = poly[j];
    if ((a.y > p.y) != (b.y > p.y)) {
      const double t = (p.y - a.y) / (b.y - a.y);
      if (p.x < a.x + t * (b.x - a.x)) inside = !inside;
    }
  }
  return inside;
}

std::vector<std::vector<Vec2>> wall_footprints(const WallSegment& w,
                                               const std::vector<Opening>& openings) {
  std::vector<std::vector<Vec2>> out;
  const double len = w.length();
  if (len <= 1e-9) return out;
  const Vec2 d = w.direction();
  const Vec2 n = left_normal(d);
  const double h = w.thickness_m * 0.5;

  std::vector<std::pair<double, double>> gaps;
  for (const auto& op : openings) {
    if (op.wall_id != w.id) continue;
    double t0 = dot(op.a - w.a, d);
    double t1 = dot(op.b - w.a, d);
    if (t1 < t0) std::swap(t0, t1);
    t0 = std::max(0.0, t0);
    t1 = std::min(len, t1);
    if (t1 - t0 > 1e-6) gaps.push_back({t0, t1});
  }
  std::sort(gaps.begin(), gaps.end());

  double cursor = 0.0;
  auto emit = [&](double s0, double s1) {
    if (s1 - s0 <= 1e-6) return;
    out.push_back({w.a + d * s0 + n * h, w.a + d * s1 + n * h, w.a + d * s1 - n * h,
                   w.a + d * s0 - n * h});
  };
  for (const auto& g : gaps) {
    emit(cursor, g.first);
    cursor = std::max(cursor, g.second);
  }
  emit(cursor, len);
  return out;
}

// --- legacy flattening ------------------------------------------------------

namespace {

void push_xy(Polyline2D* pl, Vec2 p) {
  pl->xy.push_back(static_cast<float>(p.x));
  pl->xy.push_back(static_cast<float>(p.y));
}

}  // namespace

FloorPlan to_polylines(const PlanModel& model) {
  FloorPlan fp;
  fp.scale_m_per_unit = 1.0f;
  fp.polylines.reserve(model.walls.size() + model.openings.size() + model.rooms.size());

  for (const auto& w : model.walls) {
    Polyline2D pl;
    pl.layer = kPolylineLayerWall;
    push_xy(&pl, w.a);
    push_xy(&pl, w.b);
    fp.polylines.push_back(std::move(pl));
  }
  for (const auto& o : model.openings) {
    Polyline2D pl;
    pl.layer = kPolylineLayerOpening;
    push_xy(&pl, o.a);
    push_xy(&pl, o.b);
    fp.polylines.push_back(std::move(pl));
  }
  for (const auto& r : model.rooms) {
    Polyline2D pl;
    pl.layer = kPolylineLayerRoom;
    pl.closed = true;
    for (const auto& p : r.polygon) push_xy(&pl, p);
    fp.polylines.push_back(std::move(pl));
  }
  return fp;
}

}  // namespace plan
}  // namespace scanengine
