// plan_writers.h — DXF and PDF output for a PlanModel (§3.6). Owner: A12.
//
// Both writers are hand-rolled against the standard library only, for the
// same reason A9's LAS/PLY/PCD writers are: a dependency that has to build on
// five CI legs (including the macOS universal overlay triplet and the Android
// NDK) is a much larger cost than a few hundred lines of well-documented
// format code, and neither format needs anything clever here.
//
// They live in plan/, not export/. export/ writes point clouds streamed out
// of a PageStore; these write a PlanModel, which is already in memory and is
// a completely different shape. `ExportFormat::kDxf` / `kPdf` stay declared
// in export/exporter.h as the app-facing enum values — routing those two to
// these functions is the job layer's (A15) one-line dispatch, not a reason
// to give export/ a dependency on plan/.
//
// BOTH WRITERS TAKE THE SAME MODEL. There is no "DXF plan" and "PDF plan":
// what is drawn, and where, is decided once by the extractor. The writers
// only choose representation.
//
//   DXF  model space, 1 drawing unit = 1 metre, R12 ASCII (AC1009).
//   PDF  a scaled sheet: A4/A3, portrait/landscape, a scale off the
//        standard architectural ladder, scale bar, north arrow, title block.
#ifndef SCANENGINE_PLAN_PLAN_WRITERS_H
#define SCANENGINE_PLAN_PLAN_WRITERS_H

#include <cstdint>
#include <string>

#include "scanengine/core/error.h"
#include "scanengine/plan/plan_model.h"

namespace scanengine {
namespace plan {

// --- DXF --------------------------------------------------------------------
//
// R12 (AC1009) and ASCII, deliberately. R12 is the last DXF revision that
// every CAD, GIS and drafting tool on earth reads without argument; it has
// no LWPOLYLINE (so polylines are POLYLINE + VERTEX + SEQEND), no object
// section, no handles required, and no class table. Nothing in a floor plan
// needs anything newer. ASCII rather than binary so a failure is diffable and
// so the test's independent reader is a group-code tokenizer rather than a
// second binary decoder.
struct DxfOptions {
  std::string layer_walls = "WALLS";
  std::string layer_openings = "OPENINGS";
  std::string layer_rooms = "ROOMS";
  std::string layer_dimensions = "DIMENSIONS";

  // Walls as their footprint (a closed 4-vertex polyline through the two
  // faces) rather than as a bare centerline. This is what reads as a wall in
  // CAD. Setting it false emits centerlines, which is what a downstream tool
  // that wants to re-derive its own thickness would prefer.
  bool wall_footprint = true;
  bool wall_centerlines = false;  // additionally emit the centerline
  bool room_polygons = true;
  bool room_labels = true;    // room name + area TEXT, on the dimensions layer
  bool opening_labels = true; // "DOOR 0.90" etc., on the openings layer
  bool overall_dimensions = true;  // bounding-box dimension lines + text

  double text_height_m = 0.22;
  int decimals = 5;  // coordinate precision written to the file
};

// Writes `path` (truncating). Returns kInvalidArgument for an empty path,
// kFileError if the file cannot be opened or written.
Status write_dxf(const PlanModel& model, const DxfOptions& opts, const std::string& path);
// The same bytes, in memory. Everything write_dxf() does goes through here.
Status build_dxf(const PlanModel& model, const DxfOptions& opts, std::string* out);

// --- PDF --------------------------------------------------------------------

enum class SheetSize : std::uint8_t { kA4 = 0, kA3 = 1 };
enum class SheetOrientation : std::uint8_t { kPortrait = 0, kLandscape = 1 };

const char* to_string(SheetSize s) noexcept;

struct PdfOptions {
  SheetSize sheet = SheetSize::kA4;
  SheetOrientation orientation = SheetOrientation::kLandscape;

  // 0 = pick the largest scale off the standard ladder (1:20, 1:25, 1:50,
  // 1:100, 1:200, 1:500, 1:1000, 1:2000) at which the plan still fits the
  // drawing area. Non-zero pins it, and the plan is clipped if it does not
  // fit (a pinned scale is a deliberate instruction, not a hint).
  int scale_denominator = 0;

  // Title block. Every string is caller-supplied; NOTHING is derived from
  // the clock. A writer that stamps std::time() cannot be tested for
  // determinism, and a floor plan's date belongs to the survey, not to the
  // moment someone re-exported it.
  bool title_block = true;
  std::string title = "Floor plan";
  std::string project;
  std::string date;      // e.g. "2026-08-15"; empty rows are omitted
  std::string drawn_by;
  std::string reference;  // session id / job id

  bool scale_bar = true;
  // North arrow. PLACEHOLDER, and labelled as one on the sheet: the engine
  // has no heading for an ungeoreferenced indoor session, so this draws the
  // arrow along +y and marks it "N?" unless `north_known` is set. A10's
  // georeferencing is what turns it into a real bearing — set
  // `north_angle_deg` (clockwise from plan +y) and `north_known` then.
  bool north_arrow = true;
  bool north_known = false;
  double north_angle_deg = 0.0;

  bool room_labels = true;
  bool opening_marks = true;
  bool wall_fill = true;   // solid-fill the wall footprints (poche)
  double margin_mm = 12.0;
  double line_width_mm = 0.35;
};

Status write_pdf(const PlanModel& model, const PdfOptions& opts, const std::string& path);
Status build_pdf(const PlanModel& model, const PdfOptions& opts, std::string* out);

// The ladder entry `auto` would choose. Exposed so a UI can show the scale
// before the export runs, and so the tests can assert on it directly.
int auto_scale_denominator(const PlanBounds& bounds, const PdfOptions& opts);
// Sheet size in PostScript points (1/72 inch), after orientation.
void sheet_size_pt(const PdfOptions& opts, double* width_pt, double* height_pt);

}  // namespace plan
}  // namespace scanengine

#endif  // SCANENGINE_PLAN_PLAN_WRITERS_H
