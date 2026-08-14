// plan_model.h — the 2D floor-plan output model (Tech Spec §3.6).
//
// This is the ONE structure everything downstream reads: the DXF writer, the
// PDF sheet writer, the Qt editor, the Android viewer, and the tests. It is
// deliberately plain data — no interfaces, no ownership, no engine
// back-pointer (DESIGN.md §6.3) — so an app can hold it, diff it, serialize
// it, or hand it back to `recompute_plan()` without any of this module's
// implementation being linked into its own translation units.
//
// UNITS AND FRAME. Everything here is in **metres, in the plan frame**: the
// two axes of the gravity-aligned cloud that are perpendicular to the up
// axis (see `UpAxis`). The plan frame is a rigid re-labelling of the
// session's local metric frame, never a rescaling — a wall that is 4.00 m in
// the cloud is 4.00 m here. Scaling to paper happens exactly once, inside
// the PDF writer.
//
// WHY DOUBLE AND NOT FLOAT. `PointVertex` is float32 because it is a GPU
// buffer (cloud/point_page.h). A plan is not: it accumulates least-squares
// fits, line intersections and polygon offsets over spans of tens of metres,
// and it has to be bit-reproducible run to run. float32 gives ~1 mm of
// representation slack at 20 m from the origin, which is the same order as
// the numbers this module is trying to resolve. So the model is double, and
// the float→double widening happens once, at the slice.
//
// Owner: A12.
#ifndef SCANENGINE_PLAN_PLAN_MODEL_H
#define SCANENGINE_PLAN_PLAN_MODEL_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace scanengine {
namespace plan {

// --- geometry ---------------------------------------------------------------

struct Vec2 {
  double x = 0.0;
  double y = 0.0;
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, double s) { return Vec2{a.x * s, a.y * s}; }
inline Vec2 operator*(double s, Vec2 a) { return Vec2{a.x * s, a.y * s}; }
inline bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Vec2 a, Vec2 b) { return !(a == b); }

inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline double length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline double distance(Vec2 a, Vec2 b) { return length(b - a); }

// Zero-length input returns {1,0} rather than NaN: every caller here would
// otherwise have to guard, and a degenerate direction is always rejected by
// a length test one step later anyway.
inline Vec2 normalized(Vec2 a) {
  const double n = length(a);
  if (n <= 0.0) return Vec2{1.0, 0.0};
  return Vec2{a.x / n, a.y / n};
}

// Left normal of `d` (90 deg CCW). For a CCW polygon the interior lies to
// the left of every directed edge, which is what the room inset uses.
inline Vec2 left_normal(Vec2 d) { return Vec2{-d.y, d.x}; }

// Which axis of the gravity-aligned cloud points up. The remaining two axes
// become the plan's (x, y) in the order below, chosen so the plan frame
// stays right-handed with the up axis:
//
//   kZ  ->  plan x = world x, plan y = world y   (the normal case)
//   kY  ->  plan x = world z, plan y = world x
//   kX  ->  plan x = world y, plan y = world z
enum class UpAxis : std::uint8_t { kZ = 0, kY = 1, kX = 2 };

const char* to_string(UpAxis a) noexcept;

struct PlanBounds {
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
  bool valid = false;

  double width() const { return valid ? max_x - min_x : 0.0; }
  double height() const { return valid ? max_y - min_y : 0.0; }
  Vec2 center() const { return Vec2{(min_x + max_x) * 0.5, (min_y + max_y) * 0.5}; }
  void expand(Vec2 p);
  void expand(const PlanBounds& other);
};

// An axis-aligned rectangle in the plan frame, used by the §3.6 editor's
// include/exclude tool. See plan_editor.h for the combination rule.
struct PlanRegion {
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
  bool include = false;  // true = keep-only region, false = cut-out region

  bool contains(double x, double y) const {
    return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
  }
};

// --- walls ------------------------------------------------------------------

// How the wall's centerline was established. This is not decoration: it
// decides whether room polygons are inset by half the thickness, because a
// single-face wall's "centerline" IS the visible face, not the middle of the
// wall. See docs/A12-plan.md §"Single-face walls".
enum class WallEvidence : std::uint8_t {
  kSingleFace = 0,   // one scanned face; thickness is an assumption
  kPairedFaces = 1,  // both faces scanned; thickness is measured
};

const char* to_string(WallEvidence e) noexcept;

struct WallSegment {
  std::uint32_t id = 0;
  Vec2 a;  // centerline start
  Vec2 b;  // centerline end
  double thickness_m = 0.10;
  WallEvidence evidence = WallEvidence::kSingleFace;

  // Quality, all in [0,1] except the residual.
  double rms_residual_m = 0.0;  // RMS of inlier cells about the fitted line
  double coverage = 0.0;        // occupied fraction of [a,b] (openings lower it)
  std::uint32_t support_cells = 0;
  float confidence = 0.f;

  bool snapped = false;  // orthogonality snap moved this wall

  double length() const { return distance(a, b); }
  Vec2 direction() const { return normalized(b - a); }
  Vec2 midpoint() const { return Vec2{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5}; }
  // Heading in [0, pi): a wall has no head or tail, so 0 and 180 deg are the
  // same wall and every angle comparison in this module folds accordingly.
  double angle_rad() const;
  // Half-thickness to inset a room polygon by along this wall. Zero for a
  // single-face wall, because the fitted line already sits on the face.
  double room_inset_m() const {
    return evidence == WallEvidence::kPairedFaces ? thickness_m * 0.5 : 0.0;
  }
};

// --- openings ---------------------------------------------------------------

// Everything here is a CANDIDATE. The pipeline sees a hole in a horizontal
// slice; it cannot see a door leaf, a frame or a glazing bar. §3.6's editor
// is where a human confirms these, and docs/A12-plan.md §"Honest limits"
// lists what fools each rule.
enum class OpeningKind : std::uint8_t {
  kUnknown = 0,
  kNarrowGap = 1,       // below door width: occlusion, furniture shadow, noise
  kDoorCandidate = 2,   // door-width gap, and the sill band is open too
  kWindowCandidate = 3, // door-width-or-wider gap with SOLID wall below it
  kWideOpening = 4,     // wider than a door: archway, pass-through, or a miss
};

const char* to_string(OpeningKind k) noexcept;

// Result of the sill-height re-slice. A window has wall under it; a door
// does not. kNoData is reported rather than guessed when the lower band has
// no coverage on this wall at all — a scan taken from a tripod at 1.4 m in a
// cluttered room frequently has exactly that problem.
enum class SillCheck : std::uint8_t {
  kNotChecked = 0,
  kNoData = 1,      // lower band has no support anywhere along this wall
  kOpenBelow = 2,   // lower band is open at the gap  -> door-like
  kSolidBelow = 3,  // lower band is solid at the gap -> window-like
};

const char* to_string(SillCheck c) noexcept;

struct Opening {
  std::uint32_t id = 0;
  std::uint32_t wall_id = 0;  // the WallSegment this gap sits inside
  Vec2 a;                     // gap start, on the wall centerline
  Vec2 b;                     // gap end, on the wall centerline
  double width_m = 0.0;
  OpeningKind kind = OpeningKind::kUnknown;
  SillCheck sill = SillCheck::kNotChecked;
  double sill_occupancy = 0.0;  // occupied fraction of the gap in the sill band
  float confidence = 0.f;

  Vec2 midpoint() const { return Vec2{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5}; }
};

// --- rooms ------------------------------------------------------------------

struct Room {
  std::uint32_t id = 0;
  std::string label;          // "R1", "R2", ... assigned in deterministic order
  std::vector<Vec2> polygon;  // CCW, interior face, implicitly closed
  double area_m2 = 0.0;
  double perimeter_m = 0.0;
  Vec2 centroid;
  float confidence = 0.f;
  // False when the bounding cycle used a bridged opening or an extended
  // corner to close itself — the area is still reported, but it rests on an
  // inference rather than on measured wall.
  bool fully_measured = true;
};

// --- the model --------------------------------------------------------------

struct PlanStats {
  std::uint64_t points_considered = 0;
  std::uint64_t points_in_band = 0;
  std::uint64_t points_after_filter = 0;
  std::uint32_t grid_w = 0;
  std::uint32_t grid_h = 0;
  std::uint32_t occupied_cells = 0;
  std::uint32_t ransac_lines = 0;
  std::uint32_t snapped_walls = 0;
  std::uint32_t paired_walls = 0;
  double dominant_angle_rad = 0.0;  // estimated dominant direction, [0, pi/2)
  double total_wall_length_m = 0.0;
  double total_room_area_m2 = 0.0;
};

struct PlanModel {
  std::vector<WallSegment> walls;
  std::vector<Opening> openings;
  std::vector<Room> rooms;
  PlanBounds bounds;
  PlanStats stats;

  // Provenance of this model, echoed back so a writer can put it in a title
  // block without the caller having to carry the options alongside.
  double slice_z_min_m = 1.0;
  double slice_z_max_m = 1.5;
  double grid_res_m = 0.02;
  UpAxis up = UpAxis::kZ;

  bool empty() const { return walls.empty() && rooms.empty(); }
  const WallSegment* wall_by_id(std::uint32_t id) const;
};

// --- polygon helpers (public: the tests and the writers both use them) -------

// Signed shoelace area. Positive for a counter-clockwise ring.
double polygon_signed_area(const std::vector<Vec2>& poly);
inline double polygon_area(const std::vector<Vec2>& poly) {
  return std::fabs(polygon_signed_area(poly));
}
double polygon_perimeter(const std::vector<Vec2>& poly);
// Area-weighted centroid; falls back to the vertex mean for a degenerate ring.
Vec2 polygon_centroid(const std::vector<Vec2>& poly);
PlanBounds polygon_bounds(const std::vector<Vec2>& poly);
bool point_in_polygon(const std::vector<Vec2>& poly, Vec2 p);

// --- the legacy A1 seam -----------------------------------------------------
//
// A1 declared a flat polyline bag in plan/floor_plan.h and tests/test_headers.cpp
// instantiates it. It is kept, and is now a *view* of PlanModel produced by
// to_polylines() — one code path, no second extractor.
struct Polyline2D {
  std::vector<float> xy;   // interleaved x,y
  std::uint8_t layer = 0;  // 0 = wall, 1 = opening, 2 = room
  bool closed = false;
};

inline constexpr std::uint8_t kPolylineLayerWall = 0;
inline constexpr std::uint8_t kPolylineLayerOpening = 1;
inline constexpr std::uint8_t kPolylineLayerRoom = 2;

struct FloorPlan {
  std::vector<Polyline2D> polylines;
  float scale_m_per_unit = 1.0f;  // the model is in metres; this stays 1.0
};

FloorPlan to_polylines(const PlanModel& model);

}  // namespace plan

// Seam-compatible names in the engine's root namespace (A1's header put them
// there and tests/test_headers.cpp uses them unqualified).
using plan::FloorPlan;
using plan::Polyline2D;

}  // namespace scanengine

#endif  // SCANENGINE_PLAN_PLAN_MODEL_H
