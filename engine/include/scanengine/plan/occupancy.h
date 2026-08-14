// occupancy.h — the horizontal slice and the 2D occupancy grid it fills.
//
// Step 1 of §3.6. The grid is exposed publicly rather than hidden inside the
// extractor for three reasons:
//
//   * the §3.6 editor drags a slice-height slider, and re-gridding is the
//     only part that has to re-touch the cloud — an app that caches the grid
//     can re-run wall extraction alone in milliseconds;
//   * the window sill check needs a SECOND grid on the same lattice, and
//     "same lattice" has to be expressible;
//   * the tests can assert on cell counts, which is a much sharper failure
//     signal than asserting on the walls that came out the far end.
//
// LATTICE. Cell (i, j) covers [origin + i*res, origin + (i+1)*res) on each
// axis, and `origin` is snapped to the global multiple-of-res lattice
// (floor(min/res)*res) rather than to the cloud's own minimum. That makes the
// grid stable under the editor: nudging the slice band, or excluding a
// region, moves the extents but never shifts the cells sideways, so two
// grids of the same cloud always agree cell-for-cell where they overlap.
//
// Owner: A12.
#ifndef SCANENGINE_PLAN_OCCUPANCY_H
#define SCANENGINE_PLAN_OCCUPANCY_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/core/types.h"
#include "scanengine/plan/plan_model.h"

namespace scanengine {
namespace plan {

// A cell is "occupied" at >= min_points. One stray point per cell is what a
// 2 cm grid gets from dust, a reflective floor, or a single grazing return;
// two is the cheapest filter that removes it and it costs nothing, unlike
// the O(N*k) statistical outlier filter (slam/post/cloud_filter.h), which is
// available as an option but is not the default — see PlanOptions.
struct OccupancyGrid {
  double origin_x = 0.0;
  double origin_y = 0.0;
  double res_m = 0.02;
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  std::uint32_t min_points = 2;
  std::vector<std::uint32_t> counts;  // row-major, size w*h

  bool valid() const { return w > 0 && h > 0 && counts.size() == std::size_t(w) * h; }
  std::size_t index(std::uint32_t i, std::uint32_t j) const {
    return std::size_t(j) * w + i;
  }
  std::uint32_t count_at(std::uint32_t i, std::uint32_t j) const {
    return counts[index(i, j)];
  }
  bool occupied(std::uint32_t i, std::uint32_t j) const {
    return counts[index(i, j)] >= min_points;
  }
  double cell_center_x(std::uint32_t i) const {
    return origin_x + (static_cast<double>(i) + 0.5) * res_m;
  }
  double cell_center_y(std::uint32_t j) const {
    return origin_y + (static_cast<double>(j) + 0.5) * res_m;
  }
  // Returns false when the point lies outside the grid.
  bool cell_of(double x, double y, std::uint32_t* i, std::uint32_t* j) const;
  std::uint32_t occupied_count() const;
  PlanBounds extent() const;
  std::vector<Vec2> occupied_centers() const;
};

// Where the points come from. Exactly one of `store` / `points` is used:
// `store` streams pages without copying anything (the A9 posture), `points`
// is the flat-span path a test or a post-pipeline result uses. `streams`
// filters the store by StreamId; empty means every page, which is what a
// merged multi-sensor session wants.
struct PlanInput {
  const PageStore* store = nullptr;
  Span<const PointVertex> points{};
  Span<const StreamId> streams{};
  UpAxis up = UpAxis::kZ;

  bool has_source() const { return store != nullptr || !points.empty(); }
};

// Slice parameters for one band. Separated from SliceOptions (floor_plan.h)
// because the sill check reuses the machinery with a different band and the
// SAME lattice.
struct BandOptions {
  double z_min_m = 1.0;
  double z_max_m = 1.5;
  double res_m = 0.02;
  std::uint32_t min_points = 2;
  // When non-null, the new grid adopts this grid's origin, resolution and
  // dimensions instead of computing its own. Points outside are dropped.
  const OccupancyGrid* lattice = nullptr;
  // Include/exclude rectangles (the §3.6 editor). Empty = keep everything.
  Span<const PlanRegion> regions{};
  // Run slam/post's statistical outlier filter over the band before
  // gridding. Off by default: it costs a k-NN pass over every in-band point
  // (A7 measured it as the slowest stage of the post pipeline) and
  // `min_points` already removes the isolated speckle that matters here.
  bool outlier_filter = false;
  double outlier_std_dev_mul = 1.5;
};

// Largest grid this module will allocate, in cells. 40 M cells is 160 MB of
// counts and a 160 x 160 m building at 2 cm. Past that the answer is a
// coarser resolution, not a bigger allocation, so the call fails loudly with
// kCapacityExceeded and a message naming both numbers.
inline constexpr std::size_t kMaxGridCells = 40u * 1000u * 1000u;

// Project a world point into the plan frame for the given up axis.
Vec2 project(float x, float y, float z, UpAxis up);
// The up-axis coordinate of a world point.
double up_coord(float x, float y, float z, UpAxis up);

// Fills `out` (cleared first). `stats` may be null; when given, its
// points_considered / points_in_band / points_after_filter / grid_* /
// occupied_cells fields are written.
Status build_occupancy(const PlanInput& in, const BandOptions& band, OccupancyGrid* out,
                       PlanStats* stats = nullptr);

}  // namespace plan
}  // namespace scanengine

#endif  // SCANENGINE_PLAN_OCCUPANCY_H
