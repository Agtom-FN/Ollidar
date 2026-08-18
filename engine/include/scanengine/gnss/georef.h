// georef.h — local SLAM frame ↔ global (ENU/UTM/WGS84) alignment (§3.4).
//
// "Fusion: factor graph (GTSAM) with LIO/VIO odometry factors + GNSS position
// factors weighted by fix quality; continuously estimates the local↔global
// similarity transform. Output: georeferenced trajectory and cloud in the
// project CRS."
//
// ### What A10 built, and why it is not GTSAM
//
// The spec names GTSAM for the WHOLE fusion problem — odometry factors plus
// GNSS factors, i.e. a joint smoother that also corrects the trajectory. That
// is A7's pose-graph task. What §3.4 asks A10 for, and what this file does,
// is the SECOND half: given a trajectory (from A6's LIO, A8's ARCore poses,
// or a replayed pose stream) and a stream of GNSS fixes, continuously
// estimate the transform that carries the local frame onto the globe.
//
// That sub-problem has **four parameters** — yaw about gravity, a
// 3-translation, and a scale that is locked to 1 for a metric SLAM system —
// and a closed-form weighted solution (Umeyama restricted to a rotation about
// a known axis). It is the same argument A8 wrote down for declining Ceres
// (docs/A8-pushbroom.md §2): the normal equations are 4x4 whether there are
// 30 fixes or 30,000, the weighting is one scalar per observation, and the
// robustness that actually matters is an IRLS/Huber loop, not a sparse
// solver. Adding gtsam to `vcpkg.json` — with its Boost dependency, across
// five CI legs including the macOS universal overlay triplet — to solve a
// 4-parameter problem is not a trade this task should make on A7's behalf.
// A7 had not added it when A10 landed (`vcpkg.json` still lists eigen3
// alone), so the decision was also forced.
//
// **The estimator is therefore behind an interface.** `GeorefEstimator` takes
// weighted (local, global) correspondences and returns a `GeorefSolution`.
// `WeightedSimilarityEstimator` is the hand-rolled default. When A7 lands a
// factor graph, it implements the same interface — including odometry factors
// it can see and this one cannot — and `GeorefFusion::set_estimator()` swaps
// it in with no change to any consumer. That is the same swap
// `TimeSync::set_estimator()` made possible for A4.
//
// ### Why yaw-only
//
// Roll and pitch are OBSERVABLE FROM GRAVITY, continuously, to a small
// fraction of a degree, by the IMU that A6's ESKF is already running. Solving
// for them from GNSS positions instead would mean estimating two extra
// parameters from data that constrains them only through vertical motion —
// on a level walk, not at all. Locking them to the gravity-aligned local
// frame and solving only for the heading about that axis is the difference
// between a well-conditioned 4-parameter fit and a rank-deficient
// 7-parameter one. A local frame that is NOT gravity-aligned must be rotated
// into one before it gets here; `GeorefConfig::local_is_gravity_aligned`
// documents that precondition and `GeorefSolution::gravity_residual_m` is
// the evidence that it held (a tilted local frame shows up as a vertical
// residual that grows with horizontal distance).
//
// ### Honest uncertainty
//
// The reported accuracy is NOT the fit residual. A cloud georeferenced from
// 200 RTK-Fixed samples has a transform whose parameters are known to
// millimetres, and an absolute accuracy that is still bounded by the fixes'
// own 2 cm. `GeorefSolution` reports both, plus the combination, and the
// combination is what a UI should show. §5 of docs/A10-gnss.md tabulates the
// measured behaviour across the fix-quality mixes.
//
// Owner: A10.
#ifndef SCANENGINE_GNSS_GEOREF_H
#define SCANENGINE_GNSS_GEOREF_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/gnss/gnss.h"
#include "scanengine/poses/pose_interpolator.h"

namespace scanengine {

// One time-aligned correspondence: where the local frame says the rig was,
// and where GNSS says it was, in the SAME ENU frame's metres.
struct GeorefObservation {
  std::int64_t t_ns = 0;
  double local[3] = {0.0, 0.0, 0.0};
  double global[3] = {0.0, 0.0, 0.0};
  double sigma_h_m = 1.0;   // 1-sigma horizontal of the GNSS position
  double sigma_v_m = 1.6;
  FixType fix = FixType::kNone;
};

struct GeorefSolution {
  // --- the transform ----------------------------------------------------
  bool converged = false;
  double yaw_rad = 0.0;
  double yaw_deg = 0.0;
  double translation[3] = {0.0, 0.0, 0.0};
  double scale = 1.0;
  // Row-major 4x4, se3.h convention: `global_from_local`. Directly usable
  // with se3::transform_apply(), and directly mirrorable onto the
  // `double transform[16]` seams in slam/slam.h and merge/merge.h.
  double global_from_local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  // --- how well it fits -------------------------------------------------
  std::size_t samples = 0;     // observations in the window
  std::size_t inliers = 0;
  std::size_t rejected = 0;    // hard-rejected outliers
  double residual_rms_m = 0.0;      // 3D, inliers only
  double residual_rms_h_m = 0.0;    // horizontal
  double residual_rms_v_m = 0.0;
  double residual_max_m = 0.0;
  double gravity_residual_m = 0.0;  // |correlation of vertical residual with
                                    // horizontal radius| — a tilted local frame

  // --- how well it is KNOWN --------------------------------------------
  double yaw_sigma_deg = 0.0;           // from the weighted lever arm
  double translation_sigma_h_m = 0.0;   // per-axis, from the weights
  double translation_sigma_v_m = 0.0;
  double lever_arm_rms_m = 0.0;         // RMS horizontal radius of the samples
  double span_m = 0.0;                  // max pairwise horizontal separation

  // --- what a UI should show -------------------------------------------
  //
  // Combined 1-sigma horizontal error of an arbitrary georeferenced point at
  // the working radius: transform-parameter uncertainty ⊕ yaw × lever arm ⊕
  // the GNSS fixes' own accuracy. CEP from the Rayleigh factors
  // (CEP50 = 1.1774σ, CEP95 = 2.4477σ).
  double horizontal_sigma_m = 0.0;
  double vertical_sigma_m = 0.0;
  double cep50_m = 0.0;
  double cep95_m = 0.0;

  double mean_fix_sigma_m = 0.0;
  FixType best_fix = FixType::kNone;
  FixType dominant_fix = FixType::kNone;
  double fix_fraction[5] = {0.0, 0.0, 0.0, 0.0, 0.0};

  std::int64_t t_first_ns = 0, t_last_ns = 0;
  std::uint32_t iterations = 0;

  // Why it is not converged, for a UI that has to say something. Empty when
  // it is.
  const char* blocker = "";
};

// The seam A7's factor graph implements. Deliberately narrow: correspondences
// in, one transform + its quality out. Everything the hand-rolled estimator
// knows how to do (weighting, IRLS, windowing) is an implementation detail
// on this side of the line.
class GeorefEstimator {
 public:
  virtual ~GeorefEstimator() = default;
  virtual const char* name() const = 0;
  virtual void add(const GeorefObservation& obs) = 0;
  virtual void clear() = 0;
  // Recompute from everything currently held. False when the solution is not
  // usable; `out` still carries the diagnostics that say why.
  virtual bool solve(GeorefSolution* out) = 0;
  virtual std::size_t size() const = 0;
};

struct SimilarityEstimatorConfig {
  // Sliding window. 1800 samples = 30 min at 1 Hz. Older correspondences are
  // dropped because SLAM drift makes a 40-minute-old local coordinate a
  // measurement of a DIFFERENT transform — the whole reason §3.4 says
  // "continuously estimates" rather than "estimates once".
  std::size_t window = 1800;

  // Lock scale to 1. Default true: LIO and ARCore are metric, and a free
  // scale absorbs GNSS bias into a systematic stretch of the cloud, which is
  // invisible in the residual and catastrophic in a measurement.
  bool lock_scale = true;

  // Reject a fix worse than this outright. kRtkFloat by default: a 2 m
  // single-point fix pulls a 2 cm-accurate alignment around even when
  // correctly down-weighted, because there are usually many more of them.
  FixType min_fix = FixType::kRtkFloat;

  // IRLS/Huber. `huber_k` is in units of the observation's own sigma, so a
  // 2 cm Fixed sample and a 2 m Single sample are held to the same
  // STATISTICAL standard.
  int max_iterations = 12;
  double huber_k = 2.0;
  double convergence_yaw_rad = 1e-9;
  double convergence_translation_m = 1e-6;

  // Hard rejection after IRLS: |residual| > reject_k × max(sigma, floor).
  // The floor stops a run of 2 cm Fixed samples from rejecting a real 10 cm
  // SLAM excursion as an outlier.
  double reject_k = 5.0;
  double reject_floor_m = 0.10;
  int reject_passes = 2;

  // Convergence gates.
  std::size_t min_samples = 8;
  // Yaw is observable only from a BASELINE. Two fixes a metre apart
  // determine heading to ±atan(sigma/1 m); at 2 cm that is 1.1°, at 2 m it
  // is 63°. 5 m of horizontal span is the minimum this class will call
  // converged, and `yaw_sigma_deg` keeps saying how good it actually is.
  double min_span_m = 5.0;
  double max_yaw_sigma_deg = 15.0;
};

class WeightedSimilarityEstimator final : public GeorefEstimator {
 public:
  explicit WeightedSimilarityEstimator(const SimilarityEstimatorConfig& cfg = {});

  const char* name() const override { return "weighted-similarity"; }
  void add(const GeorefObservation& obs) override;
  void clear() override;
  bool solve(GeorefSolution* out) override;
  std::size_t size() const override { return obs_.size(); }

  const SimilarityEstimatorConfig& config() const { return cfg_; }
  const std::vector<GeorefObservation>& observations() const { return obs_; }

 private:
  SimilarityEstimatorConfig cfg_;
  std::vector<GeorefObservation> obs_;
  std::vector<double> w_;       // IRLS weights, parallel to obs_
  std::vector<double> r_;       // residual magnitudes, parallel to obs_
};

struct GeorefConfig {
  // Precondition, not a request: the local frame's +Z must be up. A6's LIO
  // and ARCore both deliver that. See the header comment for why this is a
  // precondition and not a fifth parameter.
  bool local_is_gravity_aligned = true;

  // Only fixes at or above this contribute. Mirrors
  // SimilarityEstimatorConfig::min_fix and overrides it on construction.
  FixType min_fix = FixType::kRtkFloat;

  // Decimate: at 5 Hz a walking rover produces highly correlated samples;
  // 1 per second is plenty and keeps the window covering 30 minutes.
  std::int64_t min_interval_ns = 1'000'000'000;

  // Re-solve at most this often. solve() is O(n) per IRLS iteration over a
  // 1800-sample window — microseconds — but the cost of doing it per fix at
  // 5 Hz for an hour is still pointless.
  std::int64_t resolve_interval_ns = 1'000'000'000;

  // Drop a correspondence whose local pose was interpolated across a gap
  // longer than the pose source's own staleness gate (PoseGate::kStale) or
  // flagged for tracking loss. Default true: a fabricated local coordinate
  // paired with a real GNSS one biases the transform silently.
  bool require_ungated_local_pose = true;

  SimilarityEstimatorConfig estimator{};
  CrsConfig crs{};
};

// Ties it together: takes fixes from a `GnssSource` (or raw observations),
// pairs each with the local trajectory at the same instant, keeps the
// estimator fed, and turns the resulting transform into the coordinates the
// exporters and the UI want.
//
// Thread-safe: one mutex. `add_fix()` runs on the GNSS push thread,
// `to_global*()` on an export/render thread.
class GeorefFusion {
 public:
  explicit GeorefFusion(const GeorefConfig& cfg = {});
  ~GeorefFusion();

  // Swap in A7's factor graph. Must be called before the first observation;
  // kInvalidState afterwards (the old estimator's window would be lost).
  Status set_estimator(std::unique_ptr<GeorefEstimator> est);

  // Where the local poses come from. A6's LIO, A8's ExternalPoseSource, or a
  // replayed track — anything implementing PoseInterpolator. Null is legal:
  // the caller then feeds add_pair()/add_observation() itself.
  void set_local_source(const PoseInterpolator* src);

  // The ENU frame the GLOBAL coordinates live in. Normally shared with the
  // GnssSource (`GnssSource::enu_frame()`), so both agree on the origin.
  Status set_enu_frame(const crs::EnuFrame& frame);
  bool has_frame() const;

  // --- feeding ----------------------------------------------------------

  // Full path: fix → ENU via the frame, local pose via the interpolator.
  // kAgain when the local pose is not available yet (the caller may retry),
  // kInvalidState with no frame, kOk when it was accepted OR deliberately
  // skipped by a gate (`stats()` says which — a decimated sample is not an
  // error).
  Status add_fix(const GnssFix& fix);

  // Local coordinate supplied by the caller.
  Status add_pair(std::int64_t t_ns, const double local_xyz[3], const GnssFix& fix);

  // Fully explicit; no frame required.
  Status add_observation(const GeorefObservation& obs);

  // Force a re-solve now (otherwise it happens at resolve_interval_ns).
  bool solve();

  // ROUND 14 — start of a new capture. Forgets the ENU frame, the estimator's
  // whole observation window and the solution built from them, keeping the
  // config, the estimator object and the local source wiring.
  //
  // has_frame() is a LATCH: it goes true on the first fix good enough to
  // anchor an origin and nothing ever lowered it, so a second capture in one
  // app run georeferenced itself against the FIRST capture's origin and, worse,
  // against a window of local↔global pairs whose local half came from a
  // trajectory that no longer exists (every capture restarts local coordinates
  // at zero). The similarity fit over that mixture is not merely noisy, it is
  // a fit to two different rooms.
  void reset();

  GeorefSolution solution() const;
  bool converged() const;

  // --- using ------------------------------------------------------------
  //
  // All of these return kInvalidState until converged() — a transform that
  // is not converged must not be applied silently. `allow_unconverged` on
  // the fusion object turns that into a warning for a live preview that
  // would rather show an approximately-placed cloud.
  void set_allow_unconverged(bool v);

  Status to_global(const Pose& local, Pose* out) const;
  Status to_global_point(const double local[3], double enu_out[3]) const;
  // n points, out_xyz must hold 3n doubles. Doubles because a UTM northing
  // is ~5.5e6 m and a float loses 0.5 m there — the exact reason
  // `Pose::position` is double (pose_source.h).
  Status to_global_points(const PointVertex* pts, std::size_t n, double* out_xyz) const;
  Status to_global_points(const PageView& page, double* out_xyz, std::size_t cap) const;

  Status to_wgs84(const double local[3], crs::Geodetic* out) const;
  Status to_utm(const double local[3], crs::UtmCoord* out) const;

  // --- the CRS the session is expressed in -------------------------------
  bool origin_wgs84(crs::Geodetic* out) const;
  bool origin_utm(crs::UtmCoord* out) const;
  // Chosen from the origin when CrsConfig::auto_utm, else from
  // CrsConfig::epsg. 0 before the origin is known.
  int epsg() const;
  std::string epsg_string() const;
  // The A9 seam: hand straight to `ExportOptions::crs_wkt`. Empty when there
  // is no georeferencing yet, which is exactly what A9 documents as "embed
  // the local-frame placeholder" (docs/A9-export.md, "CRS seam").
  std::string crs_wkt() const;
  std::string proj_string() const;

  struct Stats {
    std::uint64_t offered = 0;
    std::uint64_t accepted = 0;
    std::uint64_t skipped_fix_quality = 0;
    std::uint64_t skipped_decimation = 0;
    std::uint64_t skipped_no_pose = 0;
    std::uint64_t skipped_gated_pose = 0;
    std::uint64_t solves = 0;
  };
  Stats stats() const;

  const GeorefConfig& config() const { return cfg_; }

 private:
  bool solve_locked_();

  mutable std::mutex m_;
  GeorefConfig cfg_;
  std::unique_ptr<GeorefEstimator> est_;
  const PoseInterpolator* local_ = nullptr;
  crs::EnuFrame frame_{};
  bool has_frame_ = false;
  bool allow_unconverged_ = false;
  int epsg_ = 0;
  GeorefSolution sol_{};
  Stats stats_{};
  std::int64_t last_accept_ns_ = 0;
  std::int64_t last_solve_ns_ = 0;
  bool dirty_ = false;
};

}  // namespace scanengine

#endif  // SCANENGINE_GNSS_GEOREF_H
