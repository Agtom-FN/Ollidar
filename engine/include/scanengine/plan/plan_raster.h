// plan_raster.h — ROUND 15. The floor plan as a PICTURE.
//
// --- WHY A THIRD WRITER ----------------------------------------------------
//
// A12 gave this project two floor-plan outputs and both are documents: DXF
// for CAD and PDF for paper. Neither can be shown on a phone. Android has no
// system DXF viewer at all, and a PDF opens in whatever the operator happens
// to have installed — which means "Floor plan" as a button could produce a
// file the owner cannot see without leaving the app, and a preview is the
// whole point of a plan on a phone: you glance at it, you decide whether the
// room closed, and you either share it or walk the missing wall again.
//
// So this writes a PNG. It is the same PlanModel the other two writers take
// (plan_writers.h's rule — "what is drawn is decided once by the extractor")
// with one addition that is deliberately NOT available to them:
//
// --- THE DENSITY BACKDROP, AND THE HONEST FALLBACK -------------------------
//
// A12's wall extractor needs two scanned FACES to call something a wall, and
// a hand-carried COIN-D6 walking through a flat paints the near face of every
// wall and almost never the far one. When the slice is too thin for RANSAC to
// fit lines at all, the honest answer is not an empty sheet: the occupancy
// grid ITSELF is a floor map — every occupied cell is a real return at a real
// metric position, and a picture of it at a stated scale is a measurement
// even when nothing has been fitted to it.
//
// Hence two modes, and the mode is stamped on the image rather than inferred:
//
//   kWalls    walls were extracted; they are drawn over a faint backdrop of
//             the cells they were fitted to, so a missing wall is visible as
//             returns with no line through them.
//   kDensity  no wall survived; the occupied cells ARE the drawing. Labelled
//             "SLICE DENSITY - NO WALLS FITTED" on the sheet so nobody can
//             mistake it for a surveyed plan.
//
// --- WHY THE PNG ENCODER IS HAND-ROLLED ------------------------------------
//
// Same reason plan_writers.h gives for DXF and PDF, plus one: this repository
// has no zlib on any leg (the engine's only image dependency is the vendored
// stb_image, which DECODES). A PNG does not need zlib to be written — the
// deflate stream may be a chain of STORED (uncompressed) blocks, which is
// legal, universally readable, and has the property this project actually
// cares about more than size: it is bit-identical run to run with no
// compression-level, no dictionary and no library version in the loop. A
// 1600-px plan lands around 2-5 MB, which is a share-sheet attachment, not a
// problem.
//
// DETERMINISM. No clock (PNG carries no tIME chunk here), no RNG, no float
// formatting through the C library, and the rasterizer visits primitives in
// model order. Two runs over the same PlanModel produce the same bytes.
//
// Owner: ROUND 15.
#ifndef SCANENGINE_PLAN_PLAN_RASTER_H
#define SCANENGINE_PLAN_PLAN_RASTER_H

#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/plan/plan_model.h"

namespace scanengine {
namespace plan {

enum class PlanRenderMode : std::uint8_t {
  kWalls = 0,    // fitted walls carried the drawing
  kDensity = 1,  // nothing was fitted; the slice's occupancy IS the map
};

const char* to_string(PlanRenderMode m) noexcept;

struct PlanRasterOptions {
  // Longest side of the image. The plan is fitted inside this minus margins,
  // and the scale that results is written on the sheet.
  std::uint32_t max_dimension_px = 1600;
  std::uint32_t margin_px = 56;
  bool density_backdrop = true;
  bool draw_walls = true;
  bool draw_rooms = true;
  bool draw_openings = true;
  bool scale_bar = true;
  bool caption = true;
  // Fraction of occupied cells the drawing extent must cover. Below 1.0 the
  // tails are trimmed so a handful of returns through a doorway does not
  // shrink the room the operator walked. Walls are never trimmed.
  double extent_keep_fraction = 0.99;
  // Extra caption line, e.g. the scan name. ASCII; anything the 5x7 font does
  // not carry is drawn as a blank.
  std::string title;
};

struct PlanRasterInfo {
  std::uint32_t width_px = 0;
  std::uint32_t height_px = 0;
  double px_per_m = 0.0;
  double scale_bar_m = 0.0;
  PlanRenderMode mode = PlanRenderMode::kDensity;
  std::uint32_t density_cells_drawn = 0;
};

// Renders into `out_png` (a complete PNG file). `density` may be null; when
// it is, there is no backdrop and a model with no walls renders as an empty
// sheet rather than as kDensity.
Status build_plan_png(const PlanModel& model, const OccupancyGrid* density,
                      const PlanRasterOptions& opts, std::string* out_png,
                      PlanRasterInfo* out_info);

Status write_plan_png(const PlanModel& model, const OccupancyGrid* density,
                      const PlanRasterOptions& opts, const std::string& path,
                      PlanRasterInfo* out_info);

// The encoder, exposed because it is the part with a FORMAT risk and the
// tests read it back independently. `rgb` is w*h*3 bytes, row-major, top row
// first. Returns kInvalidArgument on a zero dimension or a short buffer.
Status encode_png_rgb8(const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h,
                       std::string* out);

}  // namespace plan
}  // namespace scanengine

#endif  // SCANENGINE_PLAN_PLAN_RASTER_H
