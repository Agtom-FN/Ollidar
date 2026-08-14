// floor_plan.cpp — the §3.6 pipeline's front door, plus the A1 seam adapter.
#include "scanengine/plan/floor_plan.h"

#include <algorithm>
#include <atomic>
#include <memory>

#include "plan_internal.h"

namespace scanengine {
namespace plan {
namespace {

void report(PlanProgressCallback cb, void* ud, float f) {
  if (cb != nullptr) cb(f, ud);
}

}  // namespace

BandOptions main_band(const SliceOptions& s, Span<const PlanRegion> regions) {
  BandOptions b;
  b.z_min_m = static_cast<double>(std::min(s.z_min_m, s.z_max_m));
  b.z_max_m = static_cast<double>(std::max(s.z_min_m, s.z_max_m));
  b.res_m = static_cast<double>(s.grid_res_m);
  b.min_points = s.min_cell_points;
  b.regions = regions;
  b.outlier_filter = s.outlier_filter;
  b.outlier_std_dev_mul = s.outlier_std_dev_mul;
  return b;
}

BandOptions sill_band(const SliceOptions& s, const OccupancyGrid& lattice,
                      Span<const PlanRegion> regions) {
  BandOptions b = main_band(s, regions);
  b.z_min_m = static_cast<double>(std::min(s.sill_z_min_m, s.sill_z_max_m));
  b.z_max_m = static_cast<double>(std::max(s.sill_z_min_m, s.sill_z_max_m));
  b.lattice = &lattice;
  // The sill band is only ever consulted as "is there wall here", so it uses
  // the same occupancy threshold as the main band; a different one would make
  // the two grids answer different questions.
  return b;
}

Status extract_walls(const OccupancyGrid& grid, const OccupancyGrid* sill,
                     const PlanOptions& opts, PlanModel* out) {
  return extract_walls_impl(grid, sill, opts, out, nullptr);
}

Status extract_floor_plan_with_regions(const PlanInput& in, const PlanOptions& opts,
                                       Span<const PlanRegion> regions, PlanModel* out,
                                       PlanProgressCallback cb, void* ud,
                                       PlanCancelToken* cancel) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "plan: extract_floor_plan(out == null)");
  }
  *out = PlanModel{};
  report(cb, ud, 0.f);

  OccupancyGrid grid;
  PlanStats stats;
  SCAN_TRY(build_occupancy(in, main_band(opts.slice, regions), &grid, &stats));
  report(cb, ud, 0.35f);
  if (cancelled(cancel)) return ScanError::kCancelled;

  OccupancyGrid sill;
  const bool want_sill = opts.slice.window_sill_check && grid.valid();
  if (want_sill) {
    PlanStats ignored;
    SCAN_TRY(build_occupancy(in, sill_band(opts.slice, grid, regions), &sill, &ignored));
  }
  report(cb, ud, 0.55f);
  if (cancelled(cancel)) return ScanError::kCancelled;

  SCAN_TRY(extract_walls_impl(grid, want_sill ? &sill : nullptr, opts, out, cancel));

  // The grid builder knows the point counts; the wall stage knows the rest.
  out->stats.points_considered = stats.points_considered;
  out->stats.points_in_band = stats.points_in_band;
  out->stats.points_after_filter = stats.points_after_filter;
  out->up = in.up;
  report(cb, ud, 1.f);
  return kOkStatus;
}

Status extract_floor_plan(const PlanInput& in, const PlanOptions& opts, PlanModel* out,
                          PlanProgressCallback progress_cb, void* progress_user_data,
                          PlanCancelToken* cancel) {
  return extract_floor_plan_with_regions(in, opts, Span<const PlanRegion>{}, out, progress_cb,
                                         progress_user_data, cancel);
}

// --- the A1 seam ------------------------------------------------------------

namespace {

class DefaultFloorPlanExtractor final : public FloorPlanExtractor {
 public:
  Status extract(const PageStore& points, const SliceOptions& opts, FloorPlan* out) override {
    if (out == nullptr) {
      return set_last_error(ScanError::kInvalidArgument,
                            "plan: FloorPlanExtractor::extract(out == null)");
    }
    progress_.store(0.f, std::memory_order_relaxed);
    PlanInput in;
    in.store = &points;
    in.up = opts.up;
    PlanOptions po;
    po.slice = opts;
    PlanModel model;
    const Status st = extract_floor_plan(in, po, &model, &on_progress, this);
    progress_.store(1.f, std::memory_order_relaxed);
    if (!st.ok()) return st;
    *out = to_polylines(model);
    return kOkStatus;
  }

  float progress() const override { return progress_.load(std::memory_order_relaxed); }

 private:
  static void on_progress(float f, void* ud) {
    static_cast<DefaultFloorPlanExtractor*>(ud)->progress_.store(f, std::memory_order_relaxed);
  }
  std::atomic<float> progress_{0.f};
};

}  // namespace

std::unique_ptr<FloorPlanExtractor> make_floor_plan_extractor() {
  return std::unique_ptr<FloorPlanExtractor>(new DefaultFloorPlanExtractor());
}

}  // namespace plan
}  // namespace scanengine
