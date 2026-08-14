// floor_plan.h — floor-plan extraction (§3.6).
//
// SEAM ONLY. Owner: A12 (Opus: slice, line extraction, snapping; Sonnet:
// DXF/PDF writers, split PR — the writers implement export/Exporter, not
// this interface).
//
// Pipeline fixed by the spec: gravity-aligned cloud → horizontal slice band
// (default 1.0–1.5 m, configurable) → 2D occupancy → RANSAC line extraction
// + merge → optional orthogonality snapping → wall polylines + opening
// heuristics.
#ifndef SCANENGINE_PLAN_FLOOR_PLAN_H
#define SCANENGINE_PLAN_FLOOR_PLAN_H

#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"

namespace scanengine {

struct SliceOptions {
  float z_min_m = 1.0f;
  float z_max_m = 1.5f;
  float grid_res_m = 0.02f;
  bool snap_orthogonal = true;
  float snap_tolerance_deg = 5.0f;
};

struct Polyline2D {
  std::vector<float> xy;      // interleaved x,y
  std::uint8_t layer = 0;     // 0 = wall, 1 = opening, 2 = furniture
};

struct FloorPlan {
  std::vector<Polyline2D> polylines;
  float scale_m_per_unit = 1.0f;
};

class FloorPlanExtractor {
 public:
  virtual ~FloorPlanExtractor() = default;
  virtual Status extract(const PageStore& points, const SliceOptions& opts, FloorPlan* out) = 0;
  virtual float progress() const = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_PLAN_FLOOR_PLAN_H
