// gap_rescue.h — ROUND 19 item 73. Register the two sides of a REFUSED gap
// against each other, with the rotation LOCKED to what the gyro witnessed and
// the translation solved from the walls.
//
// --- THE GAPS THIS EXISTS FOR ----------------------------------------------
//
// After ROUND 17/18 the pipeline is honest about long tracking losses, and
// honesty left three of the owner's captures with a fold nobody would fix:
//
//     scan-046   6.897 s blind   tracker says 72.28 deg   gyro says 178.63
//     scan-050   6.398 s blind   tracker says 29.94 deg   gyro says 115.63
//     scan-040   6.065 s blind   tracker says 66.21 deg   gyro says 142.75
//
// Every one is refused-gyro-disagrees, and the refusal is CORRECT: the
// tracker's frame restarted somewhere during the blind window, so the pose
// jump is not a frame change and applying it (or bridging its residual) would
// rotate the room by a number nobody can justify. But refusing is not the end
// of the evidence. The D6 kept painting through every one of those losses,
// the gyro kept measuring at 399 Hz, and the operator was pacing a small flat
// — the walls on the far side of the gap are largely the SAME walls as on the
// near side. Two rigid maps of one room, and a witness to the rotation
// between them: that is a registration problem with one unknown vector, not
// a lost cause.
//
// --- WHY THE ROTATION IS LOCKED, NOT SOLVED --------------------------------
//
// ROUND 12 measured what happens when a pushbroom cloud is given a free
// rotation to spend: point-to-plane ICP does not fail in its null space, it
// WANDERS, and it proposed 14-19 deg rotations against a gyro that had
// measured under 1.1 deg. That lesson is now structural in three modules
// (section_stitch, loop_end, and this one): the rotation was never the
// unknown. Here it is even less so — the gyro personally witnessed the turn
// the tracker slept through, to the drift ROUND 17 measured on these very
// captures (median 0.11-0.47 deg against ARCore over any clean second). So:
//
//     q_pred = q_before * q_gyro          (where the operator really ended up)
//     R_T    = R(q_after) * R(q_pred)^T   (the frame change, by construction)
//
// R_T is applied, never estimated, and the solver is handed exactly three
// unknowns: the translation between the two frames.
//
// --- HOW THE TRANSLATION IS FOUND ------------------------------------------
//
// The initial transform anchors the prediction at p_before — "the operator
// ended where he lost tracking" — which is wrong by however far he actually
// walked, so the search has to be able to reach across that. Two stages, both
// deterministic:
//
//  1. A COARSE GRID over the translation (default +-2.4 m horizontally at
//     0.4 m, +-0.6 m vertically at 0.3 m — a blind walk in a flat, not a
//     hallway sprint), scored by how many pre-gap points find a post-gap
//     partner. Ties prefer the smaller offset, so a direction the geometry
//     does not constrain stays at zero instead of drifting to the grid edge.
//     If no cell finds a real overlap, the rescue refuses: the two sides do
//     not paint any shared surface, and there is nothing to register.
//  2. ROUND 13's point-to-plane refinement from the best cell — rotation
//     frozen, 3x3 normal equations — with one addition: the step is solved
//     IN THE OBSERVABLE SUBSPACE of the system matrix. Each iteration the
//     eigenvectors whose eigenvalue ratio clears the ROUND-11/13/16 gate
//     (0.05) take a step; a weak direction takes none and is reported by
//     name. Fewer than two observable directions is a refusal, not a guess —
//     ROUND 16's plane_radius lesson (a radius that cannot reach across the
//     error excludes exactly the surfaces that measure it) sets the 0.60 m
//     neighbourhood here too.
//
// --- AND THE RULER VOTES LAST ----------------------------------------------
//
// Same seventh-gate doctrine as loop_end.h, same reason: every gate above is
// computed from the two submaps the solver chose, and a translation that
// slides a cloud onto SOME nearby surface always flatters its own residual.
// ROUND 12's whole-map self-consistency is measured before and after; a
// rescue that makes the number on the summary card worse is refused however
// plausible its geometry looked.
//
// A rescue that survives is a world-frame rigid transform T taking every
// point and pose BEFORE the gap into the after-gap frame — exactly the shape
// of a section seam, applied the same way, composed the same way when a
// capture has more than one. OFFLINE ONLY, through the processed/ provenance
// channel: streams/ stays byte-identical, and deleting processed/ returns the
// container to what the phone sealed. The same rule close_loops,
// stitch_sections and close_loop_end live by.
//
// Deterministic: no Eigen, no RNG, no clock; fixed grid order, fixed
// iteration counts, points visited in point order.
//
// Owner: ROUND 19 item 73.
#ifndef SCANENGINE_SLAM_POST_GAP_RESCUE_H
#define SCANENGINE_SLAM_POST_GAP_RESCUE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/span.h"
#include "scanengine/poses/reanchor.h"
#include "scanengine/slam/post/map_consistency.h"
#include "scanengine/slam/post/trajectory_loop.h"

namespace scanengine {
namespace post {

struct GapRescueConfig {
  // The witness. REQUIRED: a rescue without the gyro would be ROUND 12's
  // free-rotation ICP wearing a new name, and refuses by that name.
  const reanchor::GyroBridge* gyro = nullptr;

  // --- the submaps ---------------------------------------------------------
  //
  // TWICE section_stitch's 6 s, deliberately: a stitch seam compares two
  // halves of one continuous painting, but a rescue has a 6-7 s hole punched
  // between its two sides, and the far side needs long enough to paint the
  // shared walls again before the comparison means anything.
  double submap_half_window_s = 12.0;
  std::size_t min_submap_points = 600;
  std::size_t max_submap_points = 40000;

  // --- stage 1: the coarse search ------------------------------------------
  double search_radius_m = 2.4;
  double search_step_m = 0.4;
  double search_vertical_m = 0.6;
  double search_vertical_step_m = 0.3;
  // ARCore's world is gravity-aligned with +Y up; the vertical radius is
  // smaller because both frames share gravity and a phone stays in a hand.
  int up_axis = 1;
  // The pre-gap submap is decimated to this many points for the grid scan
  // (deterministic stride), and a candidate cell scores by how many of them
  // find a partner within `coarse_gate_m`.
  std::size_t coarse_source_points = 2000;
  double coarse_gate_m = 0.40;
  // The best cell must pair at least this fraction of the decimated points,
  // or the two sides simply do not see the same walls: refuse, by name.
  double min_coarse_overlap = 0.25;

  // --- stage 2: the refinement (ROUND 13's solve at ROUND 16's radius) -----
  double max_correspondence_m = 0.80;
  double huber_m = 0.10;
  std::uint32_t max_iterations = 40;
  double converge_translation_m = 1e-3;
  std::size_t min_pairs = 200;
  double plane_radius_m = 0.60;
  double max_planarity_ratio = 0.10;
  // The ROUND-11/13/16 gate, applied PER DIRECTION: an eigenvector of the
  // system matrix below this ratio takes no step. Fewer than two observable
  // directions refuses outright.
  double min_translation_observability = 0.05;

  // --- gates ---------------------------------------------------------------
  //
  // The search radius plus refinement headroom. A rescue past this is two
  // places that are not the same place.
  double max_rescue_translation_m = 3.5;
  double min_mismatch_improvement_m = 0.0;
  // Gate: the ruler (ROUND 12's whole-map self-consistency) votes last.
  bool require_self_consistency = true;
  double self_consistency_tolerance_m = 0.0;
  MapConsistencyConfig consistency{};
};

enum class GapRescueDecision {
  kRescued = 0,
  kNoGyro,            // the gyro does not cover the gap — rotation cannot be locked
  kNoAnchor,          // no tracked pose on one side of the gap to anchor a frame to
  kThinSubmap,        // not enough points on one side
  kNoOverlap,         // stage 1: the two sides paint no shared surface within reach
  kUnobservable,      // fewer than two directions the walls can measure
  kNotConverged,      // the refinement did not settle
  kCorrectionTooBig,  // past max_rescue_translation_m
  kNoImprovement,     // the two sides did not come together
  kRulerSaysWorse,    // the whole map agreed with itself LESS afterwards
  kDegenerate,        // a pose or the gyro's rotation is not usable
};

const char* to_string(GapRescueDecision d);

struct GapRescueReport {
  GapRescueDecision decision = GapRescueDecision::kDegenerate;
  const char* reason = "";

  // The gap, restated so the report reads on its own.
  std::size_t pose_before = 0;
  std::size_t pose_after = 0;
  std::int64_t t_before_ns = 0;
  std::int64_t t_after_ns = 0;
  double gap_s = 0.0;

  // What the gyro witnessed, and the rotation the lock therefore applies.
  double gyro_rotation_deg = 0.0;
  double rotation_applied_deg = 0.0;

  // Stage 1's verdict.
  double coarse_dt[3] = {0.0, 0.0, 0.0};
  double coarse_overlap = 0.0;

  // Stage 2's verdict. `solved_axes` is how many directions the geometry
  // could actually see (3 = full; 2 = the weak axis, reported below, was left
  // at the coarse value and stage 1's tie-break biases that toward zero).
  double dt[3] = {0.0, 0.0, 0.0};
  double translation_m = 0.0;
  int solved_axes = 0;
  double observability = 0.0;
  double weak_axis[3] = {0.0, 0.0, 0.0};
  std::size_t pairs = 0;
  std::uint32_t iterations = 0;
  std::size_t submap_before_points = 0;
  std::size_t submap_after_points = 0;

  // The two sides, refused-state vs rescued, mean nearest-neighbour on the
  // POINTS. `mismatch_identity_pairs` can legitimately be near zero — a 106
  // degree fold shares almost nothing — in which case the improvement gate
  // abstains and the ruler below is the whole verdict.
  double mismatch_identity_m = 0.0;
  std::size_t mismatch_identity_pairs = 0;
  double mismatch_rescued_m = 0.0;
  std::size_t mismatch_pairs = 0;

  // The ruler, whole cloud, before and after.
  bool self_check_checked = false;
  double self_check_before_m = 0.0;
  double self_check_after_m = 0.0;

  // Row-major 4x4, world-frame left-multiplication taking everything at or
  // before the gap into the after-gap frame. Identity unless kRescued, so a
  // caller that applies without looking is wasteful, never wrong.
  double correction[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// Attempt to rescue ONE refused gap. `poses`, `cloud` and `point_times` are
// the CURRENT state (any stitch correction already applied); `pose_before` /
// `pose_after` are indices into `poses` — the last tracked pose before the
// blindness and the first after it, i.e. SectionSeam::pose_before/pose_after
// of the refused gap.
//
// NOTHING IS MUTATED. The caller applies `correction` to every point and pose
// stamped at or before `t_before_ns`... strictly: with stamp < `t_after_ns`,
// matching SectionCorrection::section_of (a point AT the seam belongs to the
// later frame).
GapRescueReport rescue_gap(const std::vector<TrajPose>& poses, Span<const PointVertex> cloud,
                           Span<const std::int64_t> point_times, std::size_t pose_before,
                           std::size_t pose_after, const GapRescueConfig& cfg);

// --- ROUND 19 item 74: the points painted DURING the blindness --------------
//
// Once a gap is bridged (ROUND 18) or rescued (above), the two ends of the
// blind window are finally in ONE frame, and the returns captured inside it —
// excluded by PushbroomConfig::exclude_flagged, ~10k per long loss at the
// D6's ~1.5k resolved points/s — can be re-resolved instead of thrown away:
// orientation from the gyro (integrated across the window, closing error
// distributed linearly, exactly the densifier's model), position linearly
// interpolated between the two trusted endpoints. The ruler votes on the
// result per gap; d6_resolve.cpp owns that loop. What lives here is the
// per-gap accounting, so the sidecar and the CLI can report what was
// recovered and what was refused in the same breath as the rescue itself.
struct GapRecovery {
  std::int64_t t_before_ns = 0;
  std::int64_t t_after_ns = 0;
  std::uint64_t candidates = 0;   // flagged returns inside the window, in range
  std::uint64_t no_gyro = 0;      // skipped: the gyro did not cover [t_before, t]
  std::uint64_t admitted = 0;     // re-resolved AND kept by the ruler
  bool ruler_vetoed = false;      // candidates > 0 but admitting them read worse
  double self_check_before_m = 0.0;
  double self_check_after_m = 0.0;
  const char* reason = "";
};

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_GAP_RESCUE_H
