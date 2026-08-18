// mount_watch.h — ROUND 13 (owner item 48). Catch a lidar whose PHYSICAL
// orientation no longer matches the stored mount trim, within the first
// seconds of a capture, before the operator walks a room for nothing.
//
// --- WHAT IS AND IS NOT OBSERVABLE, WHICH IS THE WHOLE DESIGN --------------
//
// The brief asked for a "fan-vs-gravity" check. That specific check CANNOT
// work, and saying so is more useful than shipping something that looks like
// it does. Every COIN-D6 return leaves the fan formula with `z == 0` exactly
// (d6_fan.h), so the returns lie in the assumed plane BY CONSTRUCTION. Rotate
// the mount extrinsic however you like and the fan plane in the phone frame
// is `M * z_hat` — a function of the assumed mount and of nothing measured.
// Comparing the fan plane to gravity therefore compares an assumption with
// itself, and a rotation about the puck's own spin axis is invisible to any
// test of this kind: it slides every return along the fan and changes nothing
// about the plane.
//
// What IS observable is where the returns LAND. Hold a phone upright with the
// fan vertical and one revolution paints floor to ceiling: with the sensor at
// about 1.4 m in a 2.6 m room, returns run from roughly -1.4 m to +1.2 m
// about the sensor along gravity, and NOTHING can be further from the sensor
// vertically than the room is tall. Tilt the fan out of that plane and the
// same ranges start arriving at elevations the building does not have.
//
// --- THE DISCRIMINATOR, MEASURED ON THE OWNER'S OWN CAPTURES ---------------
//
// scan-026 is the contaminated one (the owner rotated the puck mid-mount).
// Per-revolution vertical extent of returns relative to the sensor, and the
// fraction of returns more than 2.5 m above or below it:
//
//   capture     median rev. vertical extent    |dY| > 2.5 m    median range
//   scan-020            2.43 m                     0.0 %          1.31 m
//   scan-026            4.88 m                    15.2 %          1.37 m
//   scan-028            2.55 m                     0.0 %          1.37 m
//   scan-029            2.61 m                     0.0 %          1.38 m
//   scan-030            2.45 m                     0.0 %          1.39 m
//
// The four good captures are within 7 % of each other and the bad one is
// double. The impossible-elevation fraction is EXACTLY ZERO on all four and
// 15.2 % on the bad one, at the same median return range — so this is not a
// bigger room, it is the same ranges arriving in the wrong places.
//
// `impossible_fraction` is therefore the gate and the extent is the
// supporting evidence, not the other way round. A ratio between two measured
// quantities beats a tuned threshold: 2.5 m above a sensor held at chest
// height is a point inside the ceiling of any room a person walks through,
// and a genuinely tall space (an atrium, a warehouse) is the documented false
// positive — which is why the verdict is a WARNING the operator can dismiss
// and never a refusal to capture.
//
// Deterministic: fixed traversal order, no clock, no RNG, no Eigen.
//
// Owner: ROUND 13.
#ifndef SCANENGINE_SLAM_POST_MOUNT_WATCH_H
#define SCANENGINE_SLAM_POST_MOUNT_WATCH_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/span.h"
#include "scanengine/slam/post/trajectory_loop.h"

namespace scanengine {
namespace post {

struct MountWatchConfig {
  // Which world axis is up. ARCore's world is gravity-aligned with +Y up.
  int up_axis = 1;

  // Only the opening of a capture is judged, because the point is to stop the
  // operator BEFORE the walk. Zero means "the whole capture", which is what
  // an offline audit wants.
  double window_seconds = 6.0;

  // One D6 revolution is 100 ms at the device's 10 Hz spin.
  double revolution_seconds = 0.10;
  std::size_t min_points_per_revolution = 40;
  std::size_t min_revolutions = 10;

  // A return this far above or below the sensor is outside the building.
  double impossible_elevation_m = 2.5;
  // Returns closer than this carry no elevation information worth judging
  // (a return 0.3 m away cannot be 2.5 m above anything).
  double min_range_m = 0.30;

  // THE GATE. 2 % against a measured 0.0 % on four good captures and 15.2 %
  // on the bad one — two orders of margin either side.
  double max_impossible_fraction = 0.02;

  // Supporting evidence only. 3.5 m sits above the 2.61 m worst good capture
  // and below the 4.88 m bad one; a genuinely tall space can exceed it
  // honestly, which is why it does not fire on its own.
  double warn_vertical_extent_m = 3.5;
};

enum class MountWatchVerdict {
  kOk = 0,
  kNotMeasurable,  // too few revolutions with enough returns
  kSuspect,        // the extent is large but no impossible elevations — say so, do not shout
  kMismatch,       // impossible elevations: the physical mount does not match the trim
};

const char* to_string(MountWatchVerdict v);

struct MountWatchReport {
  MountWatchVerdict verdict = MountWatchVerdict::kNotMeasurable;
  const char* reason = "";
  std::size_t revolutions = 0;
  std::size_t points = 0;
  double median_revolution_extent_m = 0.0;
  double impossible_fraction = 0.0;
  double median_range_m = 0.0;
  double window_seconds = 0.0;
  // The one sentence the operator gets. Never null.
  const char* operator_message = "";
};

// `cloud` / `point_times` are the D6ResolveConfig::out_point_times pairing;
// `poses` supplies the sensor position at each point's own time.
MountWatchReport check_mount_consistency(const std::vector<TrajPose>& poses,
                                         Span<const PointVertex> cloud,
                                         Span<const std::int64_t> point_times,
                                         const MountWatchConfig& cfg = {});

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_MOUNT_WATCH_H
