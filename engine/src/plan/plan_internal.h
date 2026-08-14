// plan_internal.h — declarations shared between the .cpp files of src/plan/
// only. NOT a public header: nothing outside this directory includes it
// (DESIGN.md §1, "nothing in src/ is included across module boundaries").
#ifndef SCANENGINE_SRC_PLAN_PLAN_INTERNAL_H
#define SCANENGINE_SRC_PLAN_PLAN_INTERNAL_H

#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/plan/plan_model.h"

namespace scanengine {
namespace plan {

// wall_extract.cpp. The public extract_walls() is this with a null token.
Status extract_walls_impl(const OccupancyGrid& grid, const OccupancyGrid* sill,
                          const PlanOptions& opts, PlanModel* out, PlanCancelToken* cancel);

// floor_plan.cpp. The public extract_floor_plan() is this with no regions;
// plan_editor.cpp is the caller that has any.
Status extract_floor_plan_with_regions(const PlanInput& in, const PlanOptions& opts,
                                       Span<const PlanRegion> regions, PlanModel* out,
                                       PlanProgressCallback cb, void* ud,
                                       PlanCancelToken* cancel);

// rooms.cpp — planar-face room detection over a trimmed, welded wall network.
Status detect_rooms(const std::vector<WallSegment>& walls, const RoomOptions& opts,
                    std::vector<Room>* out);

// plan_model.cpp — the wall footprint polygons BOTH writers draw: the wall's
// two faces, with its openings punched out, so a doorway is a hole in the
// wall rather than a line drawn on top of a solid one. Returns one polygon
// per solid piece (a wall with two doors yields three).
std::vector<std::vector<Vec2>> wall_footprints(const WallSegment& w,
                                               const std::vector<Opening>& openings);

// plan_text.cpp — locale-independent number formatting, used by BOTH writers.
//
// std::snprintf("%f") and std::ostringstream both honour LC_NUMERIC, so an
// app running under a de_DE locale would write "1,234" into a DXF coordinate
// and produce a file no CAD package can read. src/gnss/nmea.cpp hit exactly
// this and documents it; these do the conversion by integer arithmetic and
// cannot be affected by a locale at all. They are also the reason two runs
// produce byte-identical output.
std::string fmt_fixed(double v, int decimals);
std::string fmt_int(long long v);
// Trims trailing zeros (and a trailing point) after fmt_fixed — for labels,
// never for coordinates.
std::string fmt_trim(double v, int decimals);

}  // namespace plan
}  // namespace scanengine

#endif  // SCANENGINE_SRC_PLAN_PLAN_INTERNAL_H
