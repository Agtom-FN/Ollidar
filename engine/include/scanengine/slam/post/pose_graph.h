// pose_graph.h — SE(3) pose-graph optimization, hand-rolled.
//
// Tech Spec §3.3 names GTSAM for this step. A7 does NOT take it, and this
// header is where that decision has to survive contact with a reader, so the
// reasoning is here rather than in a commit message. Full version in
// engine/docs/A7-post.md §4; the short form:
//
//   * THE PROBLEM IS ONE FACTOR TYPE. A Mid-360 post graph is N SE(3)
//     variables, N-1 odometry between-factors in a chain, and a handful of
//     loop between-factors. That is ~250 lines of analytic Jacobian and one
//     sparse solve. GTSAM earns its keep on *heterogeneous* graphs —
//     landmarks, IMU preintegration factors, incremental iSAM2 relinearization
//     — none of which appear here.
//   * THE DEPENDENCY IS NOT LOCAL. vcpkg.json's onboarding note is explicit:
//     every new port must build on all five CI legs, and gtsam pulls Boost
//     (serialization, thread, date-time, regex, timer, chrono, system) through
//     the macOS *universal* overlay triplet, which compiles two architectures
//     in one pass and therefore breaks any port whose portfile runs configure
//     checks. That is a five-legged risk taken on for one factor type.
//   * DETERMINISM IS A REQUIREMENT, NOT A PREFERENCE. A6 established that the
//     engine promises bit-identical output for identical input, and bought it
//     by controlling every reduction order (docs/A6-lio.md §3.6, §4). A7 must
//     keep that promise through the optimizer. Every loop below reduces in a
//     fixed index order; the ordering heuristic (RCM) breaks ties on node
//     index; nothing iterates a hash container on a path that affects a
//     result.
//   * A8 SET THE PRECEDENT. The mount-extrinsics solver declined Ceres for
//     the same shape of argument and shipped a 120-line LM with a hand-written
//     6x6 LDL^T (docs/A8-pushbroom.md §2).
//
// WHERE GTSAM BECOMES THE RIGHT ANSWER — the crossover, stated up front so
// the next task does not have to re-litigate it:
//
//   1. Landmarks or a second sensor modality in the same graph (visual
//      features from A11's colorization keyframes, plane/line landmarks).
//      Schur complement machinery is real work and GTSAM has it.
//   2. INCREMENTAL optimization — re-solving every few keyframes during a
//      long capture instead of once at the end. That is iSAM2, and iSAM2 is
//      not a weekend.
//   3. Switchable/max-mixture loop constraints, or any robust formulation
//      beyond the Huber IRLS implemented here.
//   4. Marginalization (fixed-lag smoothing) for a bounded-memory session.
//
//   NOT on that list: A10. GNSS georeferencing needs UNARY factors on
//   position, weighted by fix quality (§3.4), added to exactly this graph.
//   add_position_prior() below is that seam, implemented and tested now, so
//   A10 adds a call and not a solver. A10's remaining piece — the local↔global
//   *similarity* transform — is 7 extra parameters shared by every node and
//   belongs in a wrapper around this class, not inside it.
//
// --- conventions ----------------------------------------------------------
//
// A node is `world_from_body` as (quaternion (x,y,z,w), position), the same
// pair poses/pose_source.h's Pose carries and the same order poses/se3.h uses.
//
// The 6-vector ordering is [0:3) ROTATION (axis-angle, rad), [3:6)
// TRANSLATION (m) — for the error, for the increment, and for the 6x6
// information matrix. It is the order GTSAM's Pose3 uses, so a future port to
// GTSAM does not have to permute anything.
//
// The retraction is RIGHT-multiplicative and DECOUPLED:
//     R <- R * Exp(dtheta)      p <- p + R * dp
// Decoupled (rather than the full SE(3) exponential) because the residual is
// then also decoupled — e = (Log(Rz^T Ri^T Rj), Rz^T(Ri^T(pj-pi)) - tz) —
// which makes the translation block of the information matrix mean "metres",
// exactly what a GNSS sigma or an ICP RMS is quoted in. With the full SE(3)
// log the translation residual is mixed through the left Jacobian and a
// "5 cm" sigma stops meaning 5 cm.
//
// Owner: A7.
#ifndef SCANENGINE_SLAM_POST_POSE_GRAPH_H
#define SCANENGINE_SLAM_POST_POSE_GRAPH_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/slam/post/progress.h"

namespace scanengine {
namespace post {

inline constexpr std::uint32_t kInvalidNode = 0xFFFFFFFFu;

// One variable: world_from_body.
struct PoseNode {
  double q[4] = {0.0, 0.0, 0.0, 1.0};  // (x, y, z, w)
  double p[3] = {0.0, 0.0, 0.0};
  // A fixed node is not a variable: it is removed from the linear system
  // entirely rather than pinned with a large prior. Fixing exactly one node
  // is how the gauge freedom is removed; a big prior would do the same job
  // while quietly costing the solver a decade of condition number.
  bool fixed = false;
};

// z_j = T_i^-1 * T_j, as measured.
struct BetweenFactor {
  std::uint32_t i = kInvalidNode;
  std::uint32_t j = kInvalidNode;
  double q[4] = {0.0, 0.0, 0.0, 1.0};
  double p[3] = {0.0, 0.0, 0.0};
  // Row-major 6x6, [rot | trans]. Must be symmetric positive semi-definite;
  // a zero block simply contributes nothing (that is how a position-only
  // measurement is expressed).
  double information[36] = {0.0};
  // Huber threshold on sqrt(chi2) of this factor, in "sigmas". 0 disables.
  // Applied by IRLS: a residual past `huber_delta` sigmas gets its weight
  // scaled by delta/sqrt(chi2), so one bad loop bends the graph instead of
  // breaking it. Odometry edges leave this at 0 on purpose — a robust kernel
  // on the chain hides real odometry failures.
  double huber_delta = 0.0;
  // Provenance only; the solver does not branch on it. Reports and tests do.
  bool loop = false;
};

// z = T_i, as measured. The A10 seam: a GNSS position fix is this with a
// rotation information block of zero.
struct PriorFactor {
  std::uint32_t i = kInvalidNode;
  double q[4] = {0.0, 0.0, 0.0, 1.0};
  double p[3] = {0.0, 0.0, 0.0};
  double information[36] = {0.0};
  double huber_delta = 0.0;
};

struct PoseGraphOptions {
  std::uint32_t max_iterations = 30;
  // Levenberg damping. lambda multiplies the diagonal (Marquardt scaling);
  // it shrinks by `lambda_down` on an accepted step and grows by `lambda_up`
  // on a rejected one. A pose graph initialized from odometry is close enough
  // that lambda usually collapses to the floor within two iterations, but a
  // graph with a wrong loop is not, and pure Gauss-Newton diverges there.
  double initial_lambda = 1e-6;
  double min_lambda = 1e-12;
  double max_lambda = 1e12;
  double lambda_down = 0.1;
  double lambda_up = 10.0;
  // Stop when the relative chi2 improvement of an accepted step drops below
  // this, or when the largest component of the increment is smaller than
  // these (rad / metres).
  double relative_chi2_tol = 1e-8;
  double converge_rot_rad = 1e-9;
  double converge_trans_m = 1e-9;
  // Re-weight robust factors from the current residuals at the start of each
  // outer iteration (IRLS). False freezes the weights computed at the initial
  // estimate, which is occasionally what you want when comparing two runs.
  bool robust_reweight = true;
};

struct PoseGraphSummary {
  std::uint32_t iterations = 0;      // accepted steps
  std::uint32_t rejected_steps = 0;  // LM steps that increased chi2
  double initial_chi2 = 0.0;
  double final_chi2 = 0.0;
  double final_lambda = 0.0;
  double max_rot_step_rad = 0.0;
  double max_trans_step_m = 0.0;
  bool converged = false;
  // Solver shape, for the report and for the "did the ordering help" test.
  std::uint32_t variables = 0;        // non-fixed nodes
  std::uint64_t envelope_scalars = 0; // stored entries of the skyline factor
  std::uint32_t bandwidth_blocks = 0; // max (i - first[i]) after ordering
};

// A sparse SE(3) pose graph and its Gauss-Newton/Levenberg solver.
//
// The linear system is assembled in a SKYLINE (profile) layout over 6x6
// blocks, ordered by reverse Cuthill-McKee. That combination is what makes
// the hand-rolled solver actually viable rather than merely small:
//
//   * A pose graph's Hessian is a chain plus a few long-range loop edges.
//     In the natural (time) ordering a single loop from keyframe 0 to
//     keyframe N-1 gives the matrix full bandwidth, and a profile factorization
//     degrades to dense O(N^3).
//   * RCM reorders a cycle into 0, 1, N-1, 2, N-2, ... — bandwidth 2. It is
//     the textbook answer to exactly this graph, it is ~50 deterministic lines
//     (BFS, neighbours sorted by degree then index, reversed), and it needs no
//     symbolic elimination-tree analysis.
//   * Cholesky of a symmetric matrix never creates a nonzero outside the
//     envelope. So the profile computed from the ordering IS the exact fill,
//     with no analysis pass and no dynamic allocation during factorization.
//
// Threading: not thread-safe. One graph, one thread. optimize() polls the
// cancel token once per outer iteration and once per 4,096 factorization rows.
class PoseGraph {
 public:
  PoseGraph();
  ~PoseGraph();
  PoseGraph(const PoseGraph&) = delete;
  PoseGraph& operator=(const PoseGraph&) = delete;

  // --- building -----------------------------------------------------------

  // Returns the new node's index. Nodes are indexed in insertion order.
  std::uint32_t add_node(const double q[4], const double p[3]);
  Status set_fixed(std::uint32_t index, bool fixed);

  Status add_between(const BetweenFactor& f);
  Status add_prior(const PriorFactor& f);

  // Convenience: an odometry or loop edge with isotropic sigmas.
  // `sigma_rot_rad` and `sigma_trans_m` become 1/sigma^2 on the diagonal.
  Status add_between(std::uint32_t i, std::uint32_t j, const double q[4], const double p[3],
                     double sigma_rot_rad, double sigma_trans_m, double huber_delta = 0.0,
                     bool loop = false);

  // THE A10 SEAM. A unary factor on POSITION ONLY: the rotation block of the
  // information matrix is zero, so a GNSS fix constrains where a node is and
  // says nothing about which way it points. `sigma_m` is the fix's horizontal
  // /vertical sigma; weight by fix quality by passing a larger one (§3.4).
  // `huber_delta` is what keeps one multipath fix from dragging the map.
  Status add_position_prior(std::uint32_t i, const double xyz[3], double sigma_m,
                            double huber_delta = 0.0);

  // --- solving ------------------------------------------------------------

  Result<PoseGraphSummary> optimize(const PoseGraphOptions& opts = {},
                                    CancelToken* cancel = nullptr);

  // Sum of e^T * Omega * e over every factor, with robust weights applied
  // exactly as optimize() applies them. This is the number the tests compare.
  double chi2() const;

  // --- reading ------------------------------------------------------------

  std::size_t node_count() const;
  std::size_t between_count() const;
  std::size_t prior_count() const;
  std::size_t loop_count() const;
  const PoseNode& node(std::uint32_t index) const;
  const std::vector<PoseNode>& nodes() const;
  const std::vector<BetweenFactor>& between_factors() const;
  const std::vector<PriorFactor>& prior_factors() const;

  // Overwrite an estimate (used to restore a rejected LM step, and by tests).
  Status set_node(std::uint32_t index, const double q[4], const double p[3]);

  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// --- helpers shared with the pipeline and the tests -------------------------

// Row-major 6x6 diagonal information from isotropic sigmas.
void isotropic_information(double sigma_rot_rad, double sigma_trans_m, double out[36]);

// e = (Log(Rz^T Ri^T Rj), Rz^T (Ri^T (pj - pi)) - tz), the residual the
// solver minimizes. Exposed because a test that cannot recompute the residual
// independently is not testing the Jacobian.
void between_error(const double qi[4], const double pi[3], const double qj[4],
                   const double pj[3], const double qz[4], const double pz[3], double out[6]);

// Absolute trajectory error (RMS and max of the position difference) between
// two equally-long pose lists, WITHOUT alignment: both are already in the same
// local frame because node 0 is fixed. `n` poses, positions only.
struct AteResult {
  double rms_m = 0.0;
  double max_m = 0.0;
  double final_m = 0.0;  // error of the last pose
  std::size_t count = 0;
};
AteResult absolute_trajectory_error(const std::vector<PoseNode>& estimate,
                                    const std::vector<PoseNode>& truth);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_POSE_GRAPH_H
