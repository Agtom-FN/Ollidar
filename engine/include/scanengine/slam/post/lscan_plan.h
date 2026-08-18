// lscan_plan.h — ROUND 15 item 56. A sealed container in, a floor plan out.
//
// --- WHY THIS GLUE EXISTS --------------------------------------------------
//
// A12 built the whole of Tech Spec §3.6 — slice, occupancy, RANSAC walls,
// face pairing with MEASURED thickness, door/window candidates with a sill
// re-slice, room polygons and areas, DXF R12 and a scaled PDF sheet — and
// every one of its entry points takes a `PlanInput`, i.e. a cloud that is
// already in memory. On the phone nothing had a cloud in memory: the Android
// path holds a DIRECTORY. So the most shareable artifact an indoor scanner
// can produce sat one function call away from the operator for eleven rounds.
//
// This is that function call, and it is deliberately in post/ rather than in
// plan/: it knows about containers, `processed/map_stitched.bin`, D6 resolve
// and the ARCore world frame, none of which plan/ has ever heard of and none
// of which it should.
//
// --- THE UP AXIS IS +Y, AND THAT IS NOT A DETAIL ---------------------------
//
// A12's default is `UpAxis::kZ` because a Mid-360 session is gravity-aligned
// into a Z-up frame. A D6 session's world frame is ARCore's, where **+Y is
// up** — the same axis `SectionStitchReport::up_axis = 1` reads and the same
// one mount_watch.h calls gravity. Slicing a D6 cloud at Z 1.0-1.5 m takes a
// vertical band through the room and produces a "floor plan" of a wall.
//
// --- THE LADDER, AND WHY IT IS HONEST RATHER THAN CLEVER -------------------
//
// A12's `min_cell_points = 3` is sized for a Mid-360's density. A COIN-D6 is
// a 10 Hz single-line scanner: the owner's best capture puts ~220 k points
// into a 26 m walk, of which the 50 cm plan band holds a few thousand, spread
// over a room. At 2 cm cells that is often one point per cell, so a wall that
// is unmistakable to the eye has NO cell that reaches three points and RANSAC
// fits nothing.
//
// So the extraction is retried down a stated ladder — {3,2,1} points per cell
// and, if still nothing, a coarser grid — and the report says which rung
// produced the answer. When no rung fits a wall, the result is NOT an empty
// plan: `PlanRenderMode::kDensity` renders the occupancy grid itself, at a
// stated metric scale, and every output says so on its face. A picture of
// where the returns actually are, with a scale bar, is a measurement. An
// empty sheet labelled "floor plan" is a lie.
//
// Owner: ROUND 15.
#ifndef SCANENGINE_SLAM_POST_LSCAN_PLAN_H
#define SCANENGINE_SLAM_POST_LSCAN_PLAN_H

#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/plan/plan_raster.h"
#include "scanengine/plan/plan_writers.h"

namespace scanengine {
namespace post {

struct LscanPlanOptions {
  // The band, in metres above the session origin. Android seeds these from
  // the project's own planSliceMinM/planSliceMaxM.
  double slice_min_m = 1.0;
  double slice_max_m = 1.5;
  double grid_res_m = 0.02;
  // ARCore's world. See the header note.
  plan::UpAxis up = plan::UpAxis::kY;

  // The ladder. Tried in order until walls come out; the last rung that
  // produced nothing still leaves a grid to draw.
  bool adapt_density = true;
  std::uint32_t min_cell_points = 3;

  // --- the floor MAP, which is not the plan slice --------------------------
  //
  // A COIN-D6's fan is vertical and its 10 Hz revolution paints a LINE, not a
  // sheet. A 50 cm horizontal band therefore holds only where that line
  // happened to cross it — on the owner's 26.6 m walk, 21 k of 220 k points
  // and 1,327 occupied cells over a 14 x 9 m extent. That is enough for
  // RANSAC to find some walls and nowhere near enough to close a room.
  //
  // The picture that IS dense is the whole wall height projected down. So the
  // backdrop is built from its own, much wider band at its own coarser
  // resolution: floor and ceiling excluded (they would fill every cell), and
  // 5 cm cells because 2 cm ones render as dust at phone-screen scale. The
  // fitted walls, when there are any, are drawn ON TOP of it — which makes a
  // missing wall visible as returns with no line through them.
  double map_band_min_m = 0.20;
  double map_band_max_m = 2.40;
  double map_res_m = 0.05;
  // How tall a cell's column of returns has to be before it counts as
  // structure. See wall_likeness_grid() in the .cpp — this one test is what
  // separates a wall from a floor in a downward projection.
  double map_min_span_m = 0.60;
  bool draw_map_backdrop = true;

  bool write_dxf = true;
  bool write_pdf = true;
  bool write_png = true;
  std::uint32_t png_max_px = 1600;
  // Empty = `<lscan_dir>/processed`.
  std::string out_dir;
  // Base name for the three files. Empty = "floorplan".
  std::string base_name;
  // Title-block / caption text. NOTHING is derived from the clock here, for
  // plan_writers.h's reason.
  std::string title;
  std::string project;
  std::string date;
};

struct LscanPlanReport {
  bool ran = false;
  plan::PlanRenderMode mode = plan::PlanRenderMode::kDensity;

  std::uint64_t cloud_points = 0;
  std::uint64_t band_points = 0;
  std::uint32_t occupied_cells = 0;
  // The backdrop, i.e. the floor map. Independent of the plan slice.
  std::uint64_t map_band_points = 0;
  std::uint32_t map_cells = 0;
  std::uint32_t grid_w = 0;
  std::uint32_t grid_h = 0;

  std::uint32_t walls = 0;
  std::uint32_t walls_paired = 0;  // both faces scanned -> MEASURED thickness
  std::uint32_t openings = 0;
  std::uint32_t doors = 0;
  std::uint32_t windows = 0;
  std::uint32_t rooms = 0;
  // True when walls were fitted but none of them bounded a room. Kept
  // separate from `walls == 0` because the two say different things to the
  // operator: "nothing was fitted" vs "the outline never closed".
  bool no_room_closed = false;
  // True when the walls came from the FLOOR MAP grid rather than from the
  // plan slice — i.e. the 1.2 m cut was too sparse and the projection had to
  // carry the geometry. It changes what `thickness_m` means (a projection has
  // no faces to pair), so it is reported rather than hidden.
  bool walls_from_floor_map = false;
  double total_wall_length_m = 0.0;
  double total_room_area_m2 = 0.0;
  double largest_room_area_m2 = 0.0;
  double extent_x_m = 0.0;
  double extent_y_m = 0.0;

  // Which rung of the ladder produced this.
  std::uint32_t min_cell_points_used = 0;
  double grid_res_used_m = 0.0;

  double slice_min_m = 0.0;
  double slice_max_m = 0.0;
  double png_px_per_m = 0.0;
  double png_scale_bar_m = 0.0;
  std::uint32_t png_w = 0;
  std::uint32_t png_h = 0;

  // "" when that output was not requested or could not be written.
  std::string png_path;
  std::string pdf_path;
  std::string dxf_path;

  // Where the cloud came from: "processed/map_stitched.bin",
  // "streams/map.bin" or "re-resolved from raw". A stable string.
  const char* cloud_source = "";
  // One sentence for the operator. A stable string.
  const char* summary = "";
};

// Loads the container's best available cloud (the ROUND 13 stitched map when
// present, else the live map cache, else a full re-resolve), extracts the
// plan, and writes whatever `opts` asked for.
//
// Returns kNotFound when the container holds no points at all. An extraction
// that fits no wall is NOT an error: it returns kOk with
// `mode == kDensity` and a PNG/PDF/DXF of the occupancy.
Status floor_plan_from_lscan(const std::string& lscan_dir, const LscanPlanOptions& opts,
                             LscanPlanReport* out);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_LSCAN_PLAN_H
