// plan_editor.cpp — the §3.6 Editor v1 (slice-height slider + include/exclude
// regions), as pure functions over a plain state struct.
#include "scanengine/plan/plan_editor.h"

#include <algorithm>
#include <cmath>

#include "plan_internal.h"

namespace scanengine {
namespace plan {
namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

bool edit_accepts(Span<const PlanRegion> regions, double x, double y) {
  if (regions.empty()) return true;
  bool has_include = false;
  bool inside_include = false;
  for (std::size_t k = 0; k < regions.size(); ++k) {
    const PlanRegion& r = regions[k];
    if (r.include) {
      has_include = true;
      if (r.contains(x, y)) inside_include = true;
    } else if (r.contains(x, y)) {
      return false;  // an exclude always wins
    }
  }
  return !has_include || inside_include;
}

bool edit_accepts(const PlanEditState& state, double x, double y) {
  return edit_accepts(Span<const PlanRegion>(state.regions.data(), state.regions.size()), x, y);
}

bool edit_has_regions(const PlanEditState& state) { return !state.regions.empty(); }

PlanRegion normalized_region(const PlanRegion& r) {
  PlanRegion o = r;
  if (o.min_x > o.max_x) std::swap(o.min_x, o.max_x);
  if (o.min_y > o.max_y) std::swap(o.min_y, o.max_y);
  return o;
}

PlanEditState with_slice_band(const PlanEditState& s, float z_min_m, float z_max_m) {
  PlanEditState o = s;
  float lo = std::min(z_min_m, z_max_m);
  float hi = std::max(z_min_m, z_max_m);
  // A slider that lands both handles on the same value must not produce an
  // empty band and an empty plan; 1 cm is the narrowest that still means
  // something at a 2 cm grid.
  if (hi - lo < 0.01f) hi = lo + 0.01f;
  o.options.slice.z_min_m = lo;
  o.options.slice.z_max_m = hi;
  return o;
}

PlanEditState with_slice_center(const PlanEditState& s, float z_center_m) {
  const float thickness =
      std::max(0.01f, s.options.slice.z_max_m - s.options.slice.z_min_m);
  return with_slice_band(s, z_center_m - thickness * 0.5f, z_center_m + thickness * 0.5f);
}

PlanEditState with_grid_resolution(const PlanEditState& s, float res_m) {
  PlanEditState o = s;
  o.options.slice.grid_res_m = clampf(res_m, 0.002f, 1.0f);
  return o;
}

PlanEditState with_orthogonality(const PlanEditState& s, bool enabled, float tolerance_deg) {
  PlanEditState o = s;
  o.options.slice.snap_orthogonal = enabled;
  o.options.slice.snap_tolerance_deg = clampf(tolerance_deg, 0.f, 45.f);
  return o;
}

PlanEditState with_up_axis(const PlanEditState& s, UpAxis up) {
  PlanEditState o = s;
  o.options.slice.up = up;
  return o;
}

PlanEditState with_sill_check(const PlanEditState& s, bool enabled, float z_min_m,
                              float z_max_m) {
  PlanEditState o = s;
  o.options.slice.window_sill_check = enabled;
  float lo = std::min(z_min_m, z_max_m);
  float hi = std::max(z_min_m, z_max_m);
  if (hi - lo < 0.01f) hi = lo + 0.01f;
  o.options.slice.sill_z_min_m = lo;
  o.options.slice.sill_z_max_m = hi;
  return o;
}

PlanEditState with_region(const PlanEditState& s, const PlanRegion& r) {
  PlanEditState o = s;
  o.regions.push_back(normalized_region(r));
  return o;
}

PlanEditState with_include_region(const PlanEditState& s, double min_x, double min_y,
                                  double max_x, double max_y) {
  PlanRegion r;
  r.min_x = min_x;
  r.min_y = min_y;
  r.max_x = max_x;
  r.max_y = max_y;
  r.include = true;
  return with_region(s, r);
}

PlanEditState with_exclude_region(const PlanEditState& s, double min_x, double min_y,
                                  double max_x, double max_y) {
  PlanRegion r;
  r.min_x = min_x;
  r.min_y = min_y;
  r.max_x = max_x;
  r.max_y = max_y;
  r.include = false;
  return with_region(s, r);
}

PlanEditState without_region(const PlanEditState& s, std::size_t index) {
  PlanEditState o = s;
  if (index < o.regions.size()) {
    o.regions.erase(o.regions.begin() + static_cast<std::ptrdiff_t>(index));
  }
  return o;
}

PlanEditState with_regions_cleared(const PlanEditState& s) {
  PlanEditState o = s;
  o.regions.clear();
  return o;
}

Status recompute_grids(const PlanInput& in, const PlanEditState& s, OccupancyGrid* main_grid,
                       OccupancyGrid* sill_grid) {
  if (main_grid == nullptr) {
    return set_last_error(ScanError::kInvalidArgument,
                          "plan: recompute_grids(main_grid == null)");
  }
  const Span<const PlanRegion> regions(s.regions.data(), s.regions.size());
  SCAN_TRY(build_occupancy(in, main_band(s.options.slice, regions), main_grid));
  if (sill_grid != nullptr) {
    *sill_grid = OccupancyGrid{};
    if (s.options.slice.window_sill_check && main_grid->valid()) {
      SCAN_TRY(build_occupancy(in, sill_band(s.options.slice, *main_grid, regions), sill_grid));
    }
  }
  return kOkStatus;
}

Status recompute_walls(const OccupancyGrid& main_grid, const OccupancyGrid* sill_grid,
                       const PlanEditState& s, PlanModel* out) {
  const OccupancyGrid* sill =
      (sill_grid != nullptr && sill_grid->valid()) ? sill_grid : nullptr;
  return extract_walls(main_grid, sill, s.options, out);
}

Status recompute_plan(const PlanInput& in, const PlanEditState& s, PlanModel* out,
                      PlanProgressCallback progress_cb, void* progress_user_data,
                      PlanCancelToken* cancel) {
  const Span<const PlanRegion> regions(s.regions.data(), s.regions.size());
  return extract_floor_plan_with_regions(in, s.options, regions, out, progress_cb,
                                         progress_user_data, cancel);
}

}  // namespace plan
}  // namespace scanengine
