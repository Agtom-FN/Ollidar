// loop_closure.h — point-to-plane ICP, the gate every Scan Context candidate
// has to pass before it becomes a pose-graph edge.
//
// Tech Spec §3.3 lists "Scan Context loop candidates" and then optimization.
// The word doing the work is CANDIDATE. Scan Context cannot distinguish two
// parallel corridors, two identical stairwells, or the same room entered from
// opposite ends — it was never meant to; it is a shortlist generator with a
// 20-float key. A false loop edge is the single most destructive thing that
// can happen to a pose graph: unlike odometry drift, which is smooth and
// bounded, a wrong loop folds the map and every downstream product (floor
// plan, export, merge) inherits the fold.
//
// So a candidate becomes an edge only if a point-to-plane ICP of the query
// keyframe against a LOCAL SUBMAP around the match converges, keeps enough
// inliers, and lands within a sane distance of where Scan Context said it
// would. The submap rather than the single matched keyframe matters: one
// Mid-360 keyframe is a fraction of a second of a non-repetitive scan
// pattern, and two such clouds of the same place overlap much less than
// either overlaps the accumulated local map.
//
// Point-to-plane (not point-to-point) because the residual is then invariant
// to sliding along a surface, which is exactly the ambiguity a lidar revisit
// has: the same wall sampled twice gives no information about where along the
// wall you are, and a point-to-point ICP invents some anyway.
//
// Owner: A7.
#ifndef SCANENGINE_SLAM_POST_LOOP_CLOSURE_H
#define SCANENGINE_SLAM_POST_LOOP_CLOSURE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/slam/post/progress.h"

namespace scanengine {
namespace post {

struct IcpConfig {
  // Correspondence gate. A6 measured that TIGHTENING this makes things worse
  // (docs/A6-lio.md §7.3): a small gate keeps only the matches that already
  // agree with the current estimate and stops constraining it. Same effect
  // here, with the same fix — start wide and let the plane fit reject.
  double max_correspondence_m = 1.0;
  // Plane fit, FAST-LIO2's parameters, as A6 uses them.
  std::uint32_t plane_points = 5;
  double plane_thickness_m = 0.15;
  double max_planarity_ratio = 0.1;
  // Huber threshold on the point-to-plane residual, metres. Bounded influence
  // per correspondence; 0 disables.
  double huber_m = 0.10;

  std::uint32_t max_iterations = 30;
  double converge_rot_rad = 1e-5;
  double converge_trans_m = 1e-4;
  // Grid pitch for the target index. Equal to max_correspondence_m keeps the
  // 3x3x3 neighbour probe exact.
  double target_cell_m = 1.0;
};

struct IcpResult {
  bool converged = false;
  std::uint32_t iterations = 0;
  // target_from_source, refined.
  double q[4] = {0.0, 0.0, 0.0, 1.0};
  double p[3] = {0.0, 0.0, 0.0};
  // The initialization it started from, echoed back so the acceptance gate can
  // measure how far ICP had to MOVE rather than how far the loop is from
  // identity. Those are different numbers, and only the first one is evidence:
  // a corridor revisited in the opposite direction is a legitimate 180-degree
  // loop, and a gate on absolute rotation would throw exactly those away.
  double init_q[4] = {0.0, 0.0, 0.0, 1.0};
  double init_p[3] = {0.0, 0.0, 0.0};
  std::uint64_t inliers = 0;       // correspondences in the final iteration
  double inlier_ratio = 0.0;       // inliers / source points
  double rms_m = 0.0;              // RMS point-to-plane residual, final
  double fitness_m = 0.0;          // mean |residual|, final
};

// One point-to-plane ICP. `init_q`/`init_p` are the initial target_from_source
// (from Scan Context's yaw, or from odometry). The target is indexed
// internally; for repeated queries against the same target the cost is
// dominated by the source size, not the rebuild.
IcpResult icp_point_to_plane(Span<const PointVertex> source, Span<const PointVertex> target,
                             const double init_q[4], const double init_p[3],
                             const IcpConfig& cfg = {}, CancelToken* cancel = nullptr);

// --- acceptance -------------------------------------------------------------

struct LoopAcceptConfig {
  // How far ICP is allowed to move the estimate away from its initialization.
  // A revisit that has to be dragged further than this was not a revisit —
  // this is the guard against a Scan Context match in a repetitive building.
  // Deliberately generous, because the whole point of a loop closure is to
  // correct drift that has grown large; the inlier and RMS gates are the sharp
  // ones.
  double max_translation_m = 10.0;
  double max_rotation_deg = 45.0;
  std::uint64_t min_inliers = 150;
  double min_inlier_ratio = 0.30;
  double max_rms_m = 0.25;
};

// True when `r` clears every gate in `cfg`. `reason` (optional) receives a
// stable, loggable string naming the first gate that failed.
bool loop_is_acceptable(const IcpResult& r, const LoopAcceptConfig& cfg, const char** reason);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_LOOP_CLOSURE_H
