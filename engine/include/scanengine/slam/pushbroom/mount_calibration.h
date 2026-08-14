// mount_calibration.h — recover the 6-DoF lidar→phone mount extrinsic from a
// planar-checkerboard capture.
//
// This is the S6 spike's solver, shipped. Everything below that looks like a
// magic number traces to `spikes/s6-calibration/REPORT.md` and its
// `results/tables.md`; the constraints S6 discovered the hard way are stated
// here so they are not rediscovered:
//
//  * **Planar checkerboard only.** §3.3 originally proposed "guided
//    corner/doorframe capture" for the D6. S6 §5 shows that is geometrically
//    impossible for a 2-D scanner: its scan plane samples a single 1-D slice,
//    so the "corner" it finds is the bend in that slice, and that bend SLIDES
//    along the corner line as the rig moves. It is not a repeatable world
//    point and cannot be matched to a camera-triangulated vertex. There is
//    therefore only one residual family in this file: point-on-plane.
//    (A bare wall via ARCore plane detection is also not a target — 60–115 px
//    at 3 m, 3–6x the whole colorization budget. The camera side must be a
//    checkerboard.)
//
//  * **No robust kernel in stage 1.** From the bracket's CAD nominal (about
//    4 degrees and 25 mm off truth) EVERY residual looks like an outlier, so
//    a soft-L1 fit down-weights the whole problem and, in S6's scipy
//    prototype, stalled at the starting point (S6 §2.3). Order kept: plain L2
//    first, robust refinement second from the now-close L2 answer.
//
//    Measured note, from `the_robust_stage_earns_its_place_and_never_runs_first`
//    in tests/test_mount_calib.cpp: THIS solver does not reproduce that stall,
//    and the reason is structural. Its damping is Marquardt's
//    `lambda * diag(H)` rather than `lambda * I`, which makes the step
//    invariant to a uniform rescaling of H and g — and a robust kernel applied
//    when every residual is large is, to first order, exactly such a rescale.
//    S6's trust-region-reflective bounds the step in a scaled variable
//    instead, so the same down-weighting shrinks the step. The order is kept
//    regardless: it costs nothing, it is what the study mandates, and it is
//    the only order that stays correct if the damping strategy changes.
//    `MountCalibConfig::robust_first_stage` exists so that test can keep
//    measuring the claim rather than citing it.
//
//  * **The quality gate is split-half agreement, NOT the solver covariance.**
//    S6 action G: with a fixed prescribed pose set the linearised covariance
//    is nearly constant session to session; its measured rank correlation
//    with true error is about 0.1. `sigma_rot_deg` / `sigma_trans_mm` are
//    reported here as diagnostics and must never be used to accept or reject
//    a capture. `split_half_px` is the gate.
//
//  * **A D6 solution is only accurate near its calibration distance.** S6
//    §6.2: a weakly conditioned solve trades rotation against translation so
//    the two partially cancel near the capture distance, and that cancellation
//    breaks down away from it (the D6-at-30 mm error curve RISES again at
//    8 m). Capture at a distance representative of use — 1.2–2 m — and do not
//    extrapolate. `MountCalibResult::gate_range_m` records what the reported
//    gate was measured at.
//
//  * **The D6 needs a bench procedure, not a handheld wizard.** At its
//    specified 30 mm range noise, 12 handheld poses give ~37 px at 3 m;
//    45 tripod poses give ~9 px on an A1 board and ~6 px on an XL board.
//    See engine/docs/A8-pushbroom.md §5 for this engine's measured version of
//    that table. The solver is the same either way — only the pose count is
//    different — which is why one class serves both.
//
// Frames: the solver estimates `camera_from_lidar`, ROW-MAJOR 4x4
// (poses/se3.h). ARCore's pose is the camera pose, so this is exactly the
// `phone_from_lidar` the pushbroom assembler wants and the
// `camera_from_lidar` color/colorize.h wants — one transform, two names,
// no conversion.
//
// Owner: A8 (this solver) / A11 (colorization wrapper, wizard-side clock
// offset) / B7 (the Android wizard UI that captures the observations).
#ifndef SCANENGINE_SLAM_PUSHBROOM_MOUNT_CALIBRATION_H
#define SCANENGINE_SLAM_PUSHBROOM_MOUNT_CALIBRATION_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"

namespace scanengine {

// One wizard/bench pose: the target plane as the CAMERA measured it, plus the
// lidar returns that landed on the target.
//
// The camera side is a plane, not a pose: `n · X = d` for X on the board, with
// `n` a unit normal in the camera frame and `d > 0` metres. That is all the
// residual needs, and it keeps checkerboard detection + PnP on the app side
// (OpenCV on desktop, ARCore/MLKit on Android) where the image already is.
struct PlaneObservation {
  double normal[3] = {0.0, 0.0, 1.0};
  double d = 0.0;

  // Lidar-frame returns segmented onto the board, as x,y,z triples
  // (size == 3 * point count).
  std::vector<double> points_lidar;

  // 1-sigma range noise of those returns, metres. This whitens the residual,
  // so mixing a Mid-360 (20 mm) and a D6 (30 mm) capture — or poses at
  // different ranges — weights them correctly. S6's central open question
  // (action A) is what this number really is for the D6: at 10 mm it behaves
  // like a Mid-360, at 30 mm it does not.
  double sigma_m = 0.02;

  std::size_t point_count() const { return points_lidar.size() / 3; }
};

// The phone camera, for the reprojection metric only — no image is projected
// through this, it just converts an extrinsic disagreement into the pixels
// the S6 budget is written in. Defaults are S6's model: 4032x3024, 26 mm
// equivalent → fx = 2912 px, HFOV 69.4 degrees.
struct CalibCamera {
  double fx = 2912.0;
  double fy = 2912.0;
  double cx = 2016.0;
  double cy = 1512.0;
  std::uint32_t width = 4032;
  std::uint32_t height = 3024;
};

// The user-facing verdict. Thresholds are WIZARD.md §2 screen 4.
enum class CalibGate : std::uint8_t {
  kUnknown = 0,  // not computed (fewer than 4 observations)
  kGood = 1,     // <= 12 px  — "Good, ready to scan"
  kUsable = 2,   // <= 30 px  — "Usable, but colours may smear on edges"
  kReject = 3,   // >  30 px  — "Not accurate enough"; must redo
};

const char* to_string(CalibGate g) noexcept;

struct MountCalibConfig {
  CalibCamera camera{};

  // The gate is evaluated at 3 m, the range the S6 budget is quoted at.
  double gate_range_m = 3.0;
  double gate_good_px = 12.0;
  double gate_reject_px = 30.0;

  // Zhang–Pless needs >= 5 configurations for a plane-only solve; below that
  // the problem is rank-deficient however good the data is. Fewer than this
  // still solves (so a caller can inspect the answer) but is marked
  // `degenerate` and gated kReject.
  std::size_t min_observations = 5;
  std::size_t min_points_per_observation = 3;

  // --- solver ------------------------------------------------------------
  int max_iterations = 100;          // per stage
  double lambda_init = 1e-3;         // Levenberg–Marquardt damping
  double cost_tol = 1e-12;           // relative cost improvement
  double step_tol = 1e-12;           // ||dx||
  bool run_robust_stage = true;      // stage 2 (S6 §2.3)
  double robust_f_scale = 2.5;       // soft-L1 scale, in whitened residual units

  // MUST stay false in production. Exposed only so the test suite can
  // demonstrate the S6 finding rather than cite it — see the file header.
  bool robust_first_stage = false;

  // Split-half is two extra solves; a caller doing a parameter sweep can skip
  // it. Production must not.
  bool compute_split_half = true;
};

struct MountCalibResult {
  double camera_from_lidar[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  bool converged = false;
  bool degenerate = false;  // too few observations, or a rank-deficient normal matrix
  int iterations_l2 = 0;
  int iterations_robust = 0;

  std::size_t observations = 0;
  std::size_t residuals = 0;
  double rms_residual_m = 0.0;
  double final_cost = 0.0;

  // THE GATE (S6 action G). Pixels of disagreement at `gate_range_m` between
  // the two half-solutions. -1 when not computed.
  double split_half_px = -1.0;
  double gate_range_m = 3.0;
  CalibGate gate = CalibGate::kUnknown;

  // DIAGNOSTICS ONLY — never gate on these. See the file header.
  double sigma_rot_deg = 0.0;
  double sigma_trans_mm = 0.0;
  double condition_number = 0.0;
};

class MountCalibrationSolver {
 public:
  explicit MountCalibrationSolver(const MountCalibConfig& cfg = {});
  ~MountCalibrationSolver();

  // Rejects a non-unit / non-finite normal, a non-positive sigma, and an
  // observation with too few points.
  Status add_observation(const PlaneObservation& obs);

  // Convenience for the capture path, whose lidar returns are already
  // PointVertex in the SENSOR frame (D6Driver's live preview, or the
  // pushbroom assembler before the extrinsic is known).
  Status add_observation(const double normal[3], double d,
                         Span<const PointVertex> lidar_points, double sigma_m);

  std::size_t observation_count() const { return obs_.size(); }
  const std::vector<PlaneObservation>& observations() const { return obs_; }
  void clear() { obs_.clear(); }

  // `cad_camera_from_lidar` is the bracket's CAD nominal — a designed bracket
  // always has one, and S6's experiments start 4 degrees / 25 mm away from
  // truth on purpose. Must be a rigid row-major 4x4.
  Result<MountCalibResult> solve(const double cad_camera_from_lidar[16]);

  const MountCalibConfig& config() const { return cfg_; }
  void set_config(const MountCalibConfig& cfg) { cfg_ = cfg; }

 private:
  MountCalibConfig cfg_;
  std::vector<PlaneObservation> obs_;
};

// --- free functions the wizard UI and the tests both need -----------------

// RMS pixel disagreement when a point at `range_m` is projected through
// `a` instead of `b`. This is the S6 metric: sample directions over the image,
// place a point at that range, un-project it through one extrinsic and
// re-project it through the other.
//
// Deterministic (a fixed 24x18 grid over the central 85 % of the frame) where
// S6 used a fixed-seed random sample — same statistic, reproducible across
// the five CI legs, which a <random> distribution would not be.
double reprojection_disagreement_px(const double a[16], const double b[16], double range_m,
                                    const CalibCamera& cam);

CalibGate classify_gate(double split_half_px, const MountCalibConfig& cfg) noexcept;

}  // namespace scanengine

#endif  // SCANENGINE_SLAM_PUSHBROOM_MOUNT_CALIBRATION_H
