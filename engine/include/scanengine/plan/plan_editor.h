// plan_editor.h — the §3.6 "Editor v1" API: slice-height slider +
// include/exclude regions. Owner: A12.
//
// §3.6 says "Editor v1: slice-height slider + include/exclude regions;
// CAD-grade editing is Phase 3." This header is exactly that and nothing
// more.
//
// EVERYTHING HERE IS PURE. `PlanEditState` is plain data the app owns; every
// mutator returns a NEW state rather than modifying one in place, so the Qt
// desktop and the Android viewer get undo/redo for free (keep a
// std::vector<PlanEditState>) and neither of them has to reason about when
// the engine's copy went stale. There is no editor object, no session, no
// engine back-pointer, and no thread: recompute_plan() is a function of
// (cloud, state) and nothing else, which is also what makes it trivially
// testable.
//
// THE COST MODEL THE SLIDER NEEDS. Dragging the slice slider changes which
// points land in the band, so the grid must be rebuilt — that is one
// streaming pass over the cloud (no copy), and it is the only expensive
// part. Adding or removing a rectangle changes the same thing. Toggling
// orthogonality snapping, changing the snap tolerance, or changing any
// WallOptions/OpeningOptions/RoomOptions value does NOT touch the cloud:
// call recompute_walls() with the cached grids and the pipeline re-runs from
// step 3 in milliseconds. Apps that want a live slider should hold the two
// OccupancyGrids from recompute_grids() and only redo the full pass when the
// band or the regions actually move.
#ifndef SCANENGINE_PLAN_PLAN_EDITOR_H
#define SCANENGINE_PLAN_PLAN_EDITOR_H

#include <cstddef>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/plan/plan_model.h"

namespace scanengine {
namespace plan {

// The complete, serializable description of an edit. Two states that compare
// equal produce identical plans from identical clouds.
struct PlanEditState {
  PlanOptions options;
  // Ordered, but order does not affect the result — see edit_accepts().
  std::vector<PlanRegion> regions;
};

// REGION SEMANTICS, in one sentence so no app has to guess:
//   a point is kept when (there is no include region, or it is inside at
//   least one include region) AND it is inside no exclude region.
// Exclude therefore always wins over include, which is the behaviour every
// selection tool in every CAD package has, and "no regions at all" means
// "keep everything" so an untouched plan is the unedited plan.
bool edit_accepts(const PlanEditState& state, double x, double y);
bool edit_accepts(Span<const PlanRegion> regions, double x, double y);

// Does this state restrict anything? (Used to skip the per-point test.)
bool edit_has_regions(const PlanEditState& state);

// --- pure mutators ----------------------------------------------------------
//
// Each returns a copy with one thing changed. Values are clamped/validated
// here so an app can wire a slider straight to them: z_min/z_max are swapped
// if inverted, a zero-height band is widened to 1 cm, resolution is clamped
// to [2 mm, 1 m], snap tolerance to [0, 45] degrees, and a rectangle is
// normalized so min <= max.

PlanEditState with_slice_band(const PlanEditState& s, float z_min_m, float z_max_m);
// Move a band of the current thickness so its centre sits at `z_center_m` —
// what a single-handle slider drives.
PlanEditState with_slice_center(const PlanEditState& s, float z_center_m);
PlanEditState with_grid_resolution(const PlanEditState& s, float res_m);
PlanEditState with_orthogonality(const PlanEditState& s, bool enabled, float tolerance_deg);
PlanEditState with_up_axis(const PlanEditState& s, UpAxis up);
PlanEditState with_sill_check(const PlanEditState& s, bool enabled, float z_min_m, float z_max_m);

PlanEditState with_region(const PlanEditState& s, const PlanRegion& r);
PlanEditState with_include_region(const PlanEditState& s, double min_x, double min_y,
                                  double max_x, double max_y);
PlanEditState with_exclude_region(const PlanEditState& s, double min_x, double min_y,
                                  double max_x, double max_y);
// Out-of-range index is a no-op copy, not an error: a UI that removes the
// row it just removed should not have to care.
PlanEditState without_region(const PlanEditState& s, std::size_t index);
PlanEditState with_regions_cleared(const PlanEditState& s);
PlanRegion normalized_region(const PlanRegion& r);

// --- recompute --------------------------------------------------------------

// The grids for a state: `main` always, `sill` only when the state asks for
// the window check (otherwise it comes back invalid()). Both share a
// lattice.
Status recompute_grids(const PlanInput& in, const PlanEditState& s, OccupancyGrid* main_grid,
                       OccupancyGrid* sill_grid);

// Walls onward from grids the caller already holds. `sill_grid` may be null
// or invalid.
Status recompute_walls(const OccupancyGrid& main_grid, const OccupancyGrid* sill_grid,
                       const PlanEditState& s, PlanModel* out);

// The whole thing. Equivalent to recompute_grids() + recompute_walls(), and
// the function an app calls when it does not want to cache anything.
Status recompute_plan(const PlanInput& in, const PlanEditState& s, PlanModel* out,
                      PlanProgressCallback progress_cb = nullptr,
                      void* progress_user_data = nullptr, PlanCancelToken* cancel = nullptr);

}  // namespace plan
}  // namespace scanengine

#endif  // SCANENGINE_PLAN_PLAN_EDITOR_H
