// map_consistency.h — ROUND 12. How far a map disagrees with ITSELF.
//
// --- WHY THIS EXISTS -------------------------------------------------------
//
// ROUND 12 was handed two real captures and asked which one shifted and why.
// Every geometry metric this repository owned turned out to be unable to
// answer, and for a reason worth writing down rather than working around:
//
//   * plane-fit RMS and axis extents are SIGN-BLIND (ROUND 9 found this — a
//     mirrored room has identical planarity);
//   * the ROUND 10 wall-probe thickness needs 200 returns inside a 0.5 m
//     radius cell, which only a 5 cm/s crawl produces. Run on the owner's two
//     walking-pace captures it selects **zero probes** and reports nothing.
//     Every crispness claim this project has ever made came from scan-020, the
//     one capture walked at a twentieth of normal speed;
//   * occupied-voxel counts and entropy compare a cloud only against ITSELF at
//     a different setting, so they can rank two resolves of the same bytes but
//     cannot say whether a map is any good.
//
// None of those can see the error the owner reports, because that error is a
// SURFACE PAINTED TWICE IN TWO PLACES. A wall smeared over 15 cm by trajectory
// drift is still a perfectly flat wall to a plane fit; it is two flat walls
// 15 cm apart, and the fit averages them.
//
// --- WHAT IS MEASURED ------------------------------------------------------
//
// Split the capture into windows, then for every pair of windows that painted
// the same small piece of the room, ask how far apart the two paintings are
// ALONG THE SURFACE NORMAL. Along the normal is the whole trick: the D6's
// returns slide freely ALONG a wall (that is exactly why ROUND 11's trim split
// was invisible to flatness metrics), so any offset measured in three
// dimensions is dominated by where the returns happened to land. Perpendicular
// to the surface there is nothing to hide behind.
//
// Concretely, for a pair of windows (i, j):
//
//   1. voxelize window i at `cell_m` and keep the cells window j also filled;
//   2. in each shared cell, fit a plane to window i's points (the smallest
//      eigenvector of the scatter matrix — hand-rolled 3x3 Jacobi, no Eigen);
//   3. skip cells that are not planar enough to have a normal worth using;
//   4. report the mean |signed distance| of window j's points from that plane.
//
// The result is a distribution, and what this returns is its median, which is
// robust to the handful of cells that hold a door swinging or a person walking
// past. A control is computed the same way from a window against ITSELF, split
// in two, which gives the measurement's own floor on this capture — a number
// that must be small for the rest to mean anything.
//
// --- WHAT IT IS NOT --------------------------------------------------------
//
// It is not accuracy. A map can be perfectly self-consistent and wrong (a
// uniform scale error, or ROUND 9's mirror). Self-consistency is a NECESSARY
// condition, not a sufficient one, and it is the necessary condition the
// owner's complaint is literally about: "when i turn around the scan position
// shifted" is a statement that the map disagrees with itself.
//
// It also cannot separate WHY. Trajectory drift, a mount trim error and a
// clock offset all show up here. Separating them is what the A/B experiments
// in android/NOTES.md ROUND 12 do; this file is the ruler they are measured
// with.
//
// --- DETERMINISM -----------------------------------------------------------
//
// Same doctrine as everything else: no Eigen, no RNG, no clock, no hash-order
// dependence. Cells are collected into a sorted vector and visited in sorted
// order, the eigen-solve is a fixed-iteration symmetric Jacobi, and the median
// is over a sorted vector. The same points in the same order give the same
// number on every platform, which is what lets a test assert one.
#ifndef SCANENGINE_SLAM_POST_MAP_CONSISTENCY_H
#define SCANENGINE_SLAM_POST_MAP_CONSISTENCY_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"

namespace scanengine {
namespace post {

struct MapConsistencyConfig {
  // Edge of the cube a local surface is fitted inside. 25 cm is set by the
  // sensor, not by taste: a D6 puts ~40 returns into that much wall in one
  // pass at walking speed (the same number ROUND 11 sized the coverage lattice
  // from), so a cell holds enough points to have a normal and is small enough
  // that a real wall is flat across it.
  double cell_m = 0.25;

  // A cell is used only if its points look like a surface: smallest eigenvalue
  // over largest below this, i.e. thin in one direction. 0.09 = a 3:1 axis
  // ratio. Corners, clutter and the fan's own thin filaments are excluded, and
  // that exclusion is reported so a caller can tell "the map is crisp" from
  // "almost nothing was measurable".
  double max_planarity_ratio = 0.09;

  // Minimum points from EACH window inside a shared cell.
  std::size_t min_points_per_window = 8;

  // How long a window is, in SECONDS OF POINT TIME. Windows are the unit of
  // "when", and time is used rather than distance so the answer does not
  // change when the operator pauses.
  double window_seconds = 8.0;

  // Pairs are bucketed by |i - j| and reported per separation, up to this many
  // buckets. Separation is the independent variable that matters: an error
  // that appears immediately is a resolve error, one that grows with
  // separation is drift, and one that jumps is a relocalization.
  std::size_t max_separation = 4;
};

struct MapConsistencySeparation {
  std::size_t separation = 0;      // |i - j|, in windows
  double seconds = 0.0;            // separation * window_seconds
  double median_offset_m = 0.0;    // the number
  double p90_offset_m = 0.0;
  std::size_t cells = 0;           // how many shared planar cells voted
};

struct MapConsistencyReport {
  bool measurable = false;         // false when nothing could be compared
  std::size_t windows = 0;
  std::size_t points = 0;
  double window_seconds = 0.0;

  // The floor: one window against itself, split in half. This is what the
  // metric reads on a map with NO disagreement at all, so every number below
  // must be read against it.
  double self_floor_m = 0.0;
  std::size_t self_cells = 0;

  std::vector<MapConsistencySeparation> by_separation;

  // The headline: the smallest separation that had enough evidence. It is the
  // one to quote, because it is the least contaminated by drift accumulated
  // over the rest of the walk.
  double nearest_offset_m = 0.0;
  std::size_t nearest_separation = 0;

  // Why `measurable` is false, or "" when it is true. A stable string.
  const char* blocker = "";
};

// `points` and `point_times_ns` must be the same length and in the pairing
// D6ResolveConfig::out_point_times documents. Times need not be sorted.
MapConsistencyReport measure_map_consistency(const std::vector<PointVertex>& points,
                                             const std::vector<std::int64_t>& point_times_ns,
                                             const MapConsistencyConfig& cfg = {});

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_MAP_CONSISTENCY_H
