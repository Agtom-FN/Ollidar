// extrinsics_solver.h — `ExtrinsicsSolver`, implemented (A8 §6 / A11 §8.3).
//
// `color/colorize.h` declares the seam and says what it must be:
//
//   "A8 shipped the solver this seam describes — MountCalibrationSolver in
//    slam/pushbroom/mount_calibration.h, with the split-half gate — and A8 §6
//    asks A11 to WRAP it rather than reimplement it: turn a keyframe's
//    checkerboard detection into the plane (n, d) the solver's residual wants
//    and delegate."
//
// This is that wrapper. It was left as a seam until B7 existed to feed it
// (docs/INT34-wiring.md §9 item 1); B7 now ships a real detector and a real
// homography-based plane estimator (android/NOTES.md §B7:
// `calib/CheckerboardDetector.kt`, `calib/TargetPlane.kt`,
// `calib/BoardSegmentation.kt`), so both halves of one observation exist and
// only the adapter was missing.
//
// WHAT THE ADAPTER ACTUALLY ADDS, given that both types already exist:
//
//  1. **It pairs the two halves by time.** The camera half (a plane) and the
//     lidar half (returns segmented onto the board) are produced by different
//     subsystems on different threads; the keyframe is what ties them to one
//     instant. `add_detection()` takes the camera half stamped on the ENGINE
//     clock, and the seam's `add_observation(kf, board_points)` matches it to
//     the keyframe within `match_tolerance_ns`. An unmatched keyframe is
//     kNotFound, never a silently dropped pose — the wizard has to tell the
//     operator that shot did not count.
//
//  2. **It measures the gate in the RIGHT camera's pixels.** `MountCalibConfig`
//     defaults to S6's model camera (4032x3024, fx 2912). The split-half gate
//     is quoted in pixels at 3 m, so evaluating it against a model camera when
//     the keyframes came from a different one silently rescales the verdict.
//     The adapter takes `CalibCamera` from the FIRST keyframe's own
//     `CameraIntrinsics` and refuses a later keyframe from a visibly different
//     camera — one calibration, one camera.
//
//  3. **It reports physical units.** A8 §6 and WIZARD.md §2 are explicit:
//     "±5 mm at 3 m — Good", never pixels. `px · range / fx` is the
//     conversion, and `split_half_mm_at()` is it, so the wizard does not have
//     to re-derive fx.
//
// WHAT IT DOES NOT DO: detect a checkerboard, or solve a homography. A8 §4.1
// and this file's whole reason for existing is that detection lives on the app
// side where the image already is (OpenCV on desktop, B7's Kotlin on Android).
// The keyframe's POSE is not used at all — the point-on-plane residual is
// between two sensor frames at one instant, and a wrong ARCore pose cannot
// corrupt it. That is worth knowing when a wizard shot is taken during
// tracking loss: it is still a usable calibration observation.
//
// Owner: A11 (this wrapper) over A8's solver.
#ifndef SCANENGINE_COLOR_EXTRINSICS_SOLVER_H
#define SCANENGINE_COLOR_EXTRINSICS_SOLVER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "scanengine/color/colorize.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/slam/pushbroom/mount_calibration.h"

namespace scanengine {
namespace color {

// The camera half of one wizard pose, as the app's detector measured it: the
// target plane `n · X = d` in the CAMERA frame, with `n` unit and `d > 0`
// metres. This is exactly `PlaneObservation`'s camera half — A8 §4.1's "that
// is all the residual needs, and it keeps checkerboard detection + PnP on the
// app side".
struct BoardDetection {
  // The engine-clock stamp of the IMAGE this was detected in, i.e. the
  // keyframe's `t_mono_ns`. This is the only field the pairing uses.
  std::int64_t t_engine_ns = 0;

  double normal[3] = {0.0, 0.0, 1.0};
  double d = 0.0;

  // 1-sigma range noise of the LIDAR returns that will be paired with this
  // plane (A8's whitening term). < 0 takes ExtrinsicsSolverConfig's default.
  double sigma_m = -1.0;

  // Diagnostics the wizard already has and a bench log wants; not used by the
  // solve. `corners` is how many board corners the detection matched,
  // `rms_reproj_px` the homography's own residual (-1 = not reported).
  std::uint32_t corners = 0;
  double rms_reproj_px = -1.0;
};

struct ExtrinsicsSolverConfig {
  // Passed straight to A8. `camera` is overwritten from the keyframes when
  // `camera_from_keyframes` is set (which is the default and the point).
  MountCalibConfig calib{};

  // Used for a detection that does not carry its own. 20 mm is the Mid-360's
  // figure; a D6 capture must set 30 mm (S6's open question A).
  double default_sigma_m = 0.02;

  // How far a detection's stamp may sit from the keyframe's and still be the
  // same instant. 2 ms is well inside a 2-5 fps keyframe cadence and well
  // outside the stamping jitter of a detector that ran on the same frame.
  std::int64_t match_tolerance_ns = 2'000'000;

  // Take CalibCamera from the keyframes' own intrinsics (see the header
  // comment, point 2). Off keeps `calib.camera` exactly as supplied, which is
  // what a bench harness reproducing an S6 table wants.
  bool camera_from_keyframes = true;

  // The bracket's CAD nominal, ROW-MAJOR and rigid. `solve()` starts here.
  // Identity is legal but is not a real bracket: S6's experiments start
  // ~4 degrees and 25 mm from truth precisely because a designed bracket
  // always has a nominal, and starting at identity is a harder problem than
  // the wizard actually has.
  double cad_camera_from_lidar[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

class MountExtrinsicsSolver final : public ExtrinsicsSolver {
 public:
  explicit MountExtrinsicsSolver(const ExtrinsicsSolverConfig& cfg = ExtrinsicsSolverConfig());
  ~MountExtrinsicsSolver() override;

  MountExtrinsicsSolver(const MountExtrinsicsSolver&) = delete;
  MountExtrinsicsSolver& operator=(const MountExtrinsicsSolver&) = delete;

  // --- the camera half, from the app's detector ----------------------------
  //
  // Rejects a non-unit or non-finite normal and a non-positive `d` here rather
  // than at solve time, so a bad detection is attributable to the shot that
  // produced it. Two detections at the same stamp: the later one REPLACES the
  // earlier (a wizard that re-runs its detector on one frame must not end up
  // with two observations of it).
  Status add_detection(const BoardDetection& det);
  std::size_t pending_detections() const;

  // --- colorize.h's seam ---------------------------------------------------
  //
  // `board_points` are the lidar returns segmented onto the board, in the
  // SENSOR frame. The plane comes from the detection matching `kf.t_mono_ns`;
  // kNotFound when there is none, which is the honest answer for "the detector
  // did not find the board in this frame".
  Status add_observation(const Keyframe& kf, Span<const PointVertex> board_points) override;

  // The same thing with the halves already paired — the desktop/OpenCV route,
  // and what a test uses. `det.t_engine_ns` is ignored here.
  Status add_observation(const Keyframe& kf, const BoardDetection& det,
                         Span<const PointVertex> board_points);

  // Runs A8's two-stage solve from the CAD nominal and writes the row-major
  // 4x4. The full result — the gate, the split-half figure, the diagnostics
  // that must never be gated on — stays available through result().
  // kInvalidState with no observations; a degenerate solve still returns kOk
  // with `gate == kReject`, because "not accurate enough, redo the capture" is
  // a verdict the wizard shows, not an error it propagates.
  Status solve(double camera_from_lidar[16]) override;

  // -1 before solve(), or when MountCalibConfig::compute_split_half is off.
  float split_half_agreement_px() const override;

  // --- what the wizard actually displays -----------------------------------
  //
  // The gate in millimetres at `range_m` (WIZARD.md: never pixels). This is
  // A8 §6's `px · range / fx`, with fx the camera the gate was measured in.
  // -1 when there is no gate figure yet.
  double split_half_mm_at(double range_m) const;

  const MountCalibResult& result() const;
  CalibGate gate() const;
  std::size_t observation_count() const;
  const MountCalibrationSolver& solver() const;
  const ExtrinsicsSolverConfig& config() const;

  // Forgets observations, detections, the fixed camera and the last result.
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace color
}  // namespace scanengine

#endif  // SCANENGINE_COLOR_EXTRINSICS_SOLVER_H
