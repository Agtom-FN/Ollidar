// floor_plan.h — floor-plan extraction (§3.6). Owner: A12.
//
// Pipeline, fixed by the spec and implemented in src/plan/:
//
//   gravity-aligned cloud
//     -> horizontal slice band (default 1.0-1.5 m, configurable)   occupancy.h
//     -> 2D occupancy grid (default 2 cm)                          occupancy.h
//     -> sequential RANSAC line extraction over occupied cells     wall_extract.cpp
//     -> orthogonality snapping to the two dominant directions     wall_extract.cpp
//     -> face pairing (two scanned faces -> one wall + thickness)  wall_extract.cpp
//     -> collinear merge + gap analysis -> WallSegment + Opening   wall_extract.cpp
//     -> corner intersection trimming/joining                      wall_extract.cpp
//     -> planar-face room detection + inset + area                 rooms.cpp
//     -> DXF / PDF                                                 plan_writers.h
//
// TWO ENTRY POINTS, ONE CODE PATH. `extract_floor_plan()` is the whole
// pipeline. `extract_walls()` takes a grid you already built, which is what
// the editor's slice-height slider and its include/exclude rectangles use so
// that only the parts that actually changed get recomputed
// (plan/plan_editor.h). The former calls the latter.
//
// DETERMINISM IS A REQUIREMENT, NOT A NICETY. Two runs over the same cloud
// with the same options produce a byte-identical PlanModel, DXF and PDF —
// the same property A6/A7 established for the cloud. That is why the RANSAC
// uses this module's own seeded 64-bit PRNG rather than <random>'s
// distributions (whose output is not specified across implementations), why
// nothing here iterates a hash container, and why the PDF writer has no
// timestamp the caller did not supply.
#ifndef SCANENGINE_PLAN_FLOOR_PLAN_H
#define SCANENGINE_PLAN_FLOOR_PLAN_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/plan/plan_model.h"

namespace scanengine {
namespace plan {

// --- options ----------------------------------------------------------------

// The A1 seam struct, extended. The first five fields keep their A1 names,
// types and meanings (tests/test_headers.cpp asserts the §3.6 default band),
// with one deliberate change: `snap_tolerance_deg` moves 5 -> 7 degrees,
// which is the figure the A12 task specifies and the one that actually
// covers the drift a hand-carried scan puts into a nominally square room.
struct SliceOptions {
  float z_min_m = 1.0f;
  float z_max_m = 1.5f;
  float grid_res_m = 0.02f;
  bool snap_orthogonal = true;
  float snap_tolerance_deg = 7.0f;

  // --- A12 additions ---
  UpAxis up = UpAxis::kZ;
  // Points per cell before a cell counts as occupied. THREE, not one: at the
  // default 2 cm grid a wall face carrying the ~2 cm range noise of a Mid-360
  // spreads over about five cells, and the outer two collect roughly one
  // point each. Accepting those tails widens every face to ~5 cells, which is
  // wider than the 150 mm partition the pipeline is trying to resolve into
  // two faces, and RANSAC then fits a third line down the middle of the wall.
  // Three keeps the fitted band at ~3 cells. A sparse or heavily decimated
  // cloud may need 2 or 1 — it is the one knob that has to track point
  // density, and PlanStats::occupied_cells is how you see that it is wrong.
  std::uint32_t min_cell_points = 3;
  bool outlier_filter = false;
  double outlier_std_dev_mul = 1.5;

  // Window sill re-slice. A second band BELOW the main one: a window has
  // solid wall under it, a door does not. Disabled automatically (reported
  // as SillCheck::kNoData, never guessed) when the lower band has no
  // coverage on the wall in question.
  bool window_sill_check = true;
  float sill_z_min_m = 0.35f;
  float sill_z_max_m = 0.80f;
};

struct WallOptions {
  // RANSAC. `inlier_m` must stay below half the smallest wall thickness you
  // expect to resolve, or the two faces of a partition fuse into one line
  // through the middle of the wall.
  double inlier_m = 0.035;
  std::uint32_t iterations = 600;   // per line, before the adaptive early-out
  std::uint32_t max_lines = 256;
  double min_sample_separation_m = 0.40;
  // A line is only worth fitting if it could be a wall: this is expressed in
  // cells so it tracks the grid resolution.
  double min_wall_length_m = 0.60;
  // Runs along a fitted line are split at gaps wider than this, then
  // re-merged by the opening logic below. 0.10 m = 5 cells at the default
  // resolution, comfortably above the noise-induced dropout of a 2 cm grid.
  double run_gap_m = 0.10;
  double min_run_m = 0.12;  // shorter runs on a line are spurs, not wall

  // Collinear merge.
  double collinear_offset_m = 0.05;
  double collinear_angle_deg = 4.0;

  // Orthogonality snapping is in SliceOptions (it is the field A1 put
  // there); this is the extra knob the estimator needs.
  double dominant_min_length_m = 0.80;  // walls shorter than this do not vote

  // Face pairing -> measured thickness.
  bool pair_faces = true;
  double thickness_min_m = 0.06;
  double thickness_max_m = 0.40;
  double pair_angle_deg = 3.0;
  double pair_min_overlap_frac = 0.45;
  double default_thickness_m = 0.10;

  // Corner intersection trimming/joining. An endpoint within this distance
  // of a non-parallel wall's line is pulled onto the intersection.
  double corner_join_m = 0.45;
  double corner_min_angle_deg = 20.0;
  double weld_m = 0.03;  // endpoints closer than this become one vertex

  // Deterministic RANSAC seed. Change it and you get a different (equally
  // valid) sampling order; leave it and two runs are identical.
  std::uint64_t seed = 0x9E3779B97F4A7C15ull;
};

struct OpeningOptions {
  bool enabled = true;
  // Gaps below this are noise/occlusion and are simply closed.
  double min_gap_m = 0.15;
  // Gaps above this are NOT bridged: the wall genuinely stays broken, which
  // is the honest answer for a 3 m opening between a kitchen and a living
  // room. Raise it and unrelated walls start fusing.
  double max_bridge_m = 2.00;
  double door_min_m = 0.60;
  double door_max_m = 1.20;
  // Sill-band occupancy fractions that decide door vs window.
  double sill_solid_frac = 0.55;
  double sill_open_frac = 0.25;
  // The wall must have at least this much sill-band support outside the gap
  // for the check to mean anything; below it the verdict is kNoData.
  double sill_wall_support_frac = 0.35;
};

struct RoomOptions {
  bool enabled = true;
  double min_area_m2 = 1.00;
  // Inset each room edge by the wall's room_inset_m() so the polygon lands
  // on the interior face — a floor plan's area is net internal area.
  bool inset_by_thickness = true;
  // Vertices closer than this are the same corner; edges are split where
  // another wall's endpoint lands on them within this distance. Both are
  // what turns a pile of segments into a planar graph.
  double weld_m = 0.02;
};

struct PlanOptions {
  SliceOptions slice;
  WallOptions walls;
  OpeningOptions openings;
  RoomOptions rooms;
};

// --- progress / cancellation ------------------------------------------------
//
// Same shape as A9's export callbacks (a plain function pointer plus a void*,
// no allocation, C-ABI friendly) rather than std::function, and a poll-based
// flag rather than a callback, for the same reasons that header gives. It is
// deliberately NOT export/'s type: plan/ does not depend on export/.
using PlanProgressCallback = void (*)(float fraction, void* user_data);

class PlanCancelToken {
 public:
  void request_cancel() noexcept { flag_.store(true, std::memory_order_release); }
  bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }
  void reset() noexcept { flag_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> flag_{false};
};

inline bool cancelled(const PlanCancelToken* t) noexcept {
  return t != nullptr && t->cancelled();
}

// --- the pipeline -----------------------------------------------------------

// Walls + openings + rooms from a grid you already have. `sill` may be null
// (then every Opening reports SillCheck::kNotChecked); when given it must be
// on the SAME lattice as `grid` (BandOptions::lattice) or the call returns
// kInvalidArgument.
Status extract_walls(const OccupancyGrid& grid, const OccupancyGrid* sill,
                     const PlanOptions& opts, PlanModel* out);

// The whole of §3.6 from a cloud. Slices the main band, slices the sill band
// when `opts.slice.window_sill_check` is set, and runs extract_walls().
Status extract_floor_plan(const PlanInput& in, const PlanOptions& opts, PlanModel* out,
                          PlanProgressCallback progress_cb = nullptr,
                          void* progress_user_data = nullptr,
                          PlanCancelToken* cancel = nullptr);

// Convert the A1 slice-options struct into the band the grid builder wants.
BandOptions main_band(const SliceOptions& s, Span<const PlanRegion> regions = {});
BandOptions sill_band(const SliceOptions& s, const OccupancyGrid& lattice,
                      Span<const PlanRegion> regions = {});

// --- the A1 seam ------------------------------------------------------------
//
// Kept verbatim in shape; `extract()` runs the pipeline above with default
// options and flattens the result with to_polylines(). Nothing in the engine
// needs the virtual — it exists because A1 published it — so the concrete
// pipeline is the free function and this is a thin adapter.
class FloorPlanExtractor {
 public:
  virtual ~FloorPlanExtractor() = default;
  virtual Status extract(const PageStore& points, const SliceOptions& opts, FloorPlan* out) = 0;
  virtual float progress() const = 0;
};

std::unique_ptr<FloorPlanExtractor> make_floor_plan_extractor();

}  // namespace plan

using plan::FloorPlanExtractor;
using plan::PlanModel;
using plan::SliceOptions;

}  // namespace scanengine

#endif  // SCANENGINE_PLAN_FLOOR_PLAN_H
