// SyntheticBuilding.h — C5 verification fixture: a two-room-plus-corridor
// building, ported from engine/tests/test_plan.cpp's own `make_building()` /
// `default_cloud()` (task goal 5: "generate via A12's test-fixture approach —
// read its test to mirror the generator"). engine/ is read-only for this
// task and that generator is file-local to a test binary, so this is the
// desktop's own copy of the exact same recipe, not a re-derivation:
//
//   south/east/north/west exterior walls (inner face only — a scanner inside
//   never sees the outside), a horizontal partition with two doors, a
//   vertical partition (both faces, so its thickness is measured), a window
//   in the north wall, a low table, a cabinet and floor/ceiling clutter, and
//   400 speckle points. True room areas: corridor 11.2000 m2, room A
//   13.2825 m2, room B 13.8000 m2 (engine/docs/A12-plan.md §6).
//
// Owner: C5.
#pragma once

#include <vector>

#include "scanengine/cloud/point_page.h"

namespace lidarscan {

// The default fixture (BuildOpts{} in the engine test: clutter on, no
// shelf-against-wall, no diagonal wall, no rotation).
std::vector<scanengine::PointVertex> buildSyntheticBuildingPoints();

}  // namespace lidarscan
