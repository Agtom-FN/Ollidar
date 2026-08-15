// colorizer.h — the colorization pass (A11). Tech Spec §3.5:
//
//   "per point, best-view keyframe selection (angle/distance/occlusion
//    z-buffer), color sample, RGB into final cloud"
//
// plus the three mitigations S6 §7.1 made mandatory, two of which are this
// module's:
//
//   2. a ROLLING-SHUTTER per-row time model in the projection   (−6.8 px)
//   3. MOTION-GATED keyframe selection on angular rate  (15 ms: fail → pass)
//
// (1, the constant clock offset, is `color/clock_sweep.h`, and its result
// arrives here as `ColorizeConfig::camera_clock_offset_ns`.)
//
// --- where it runs ----------------------------------------------------------
//
// Post-processing, on any platform, from a `.lscan`: the same posture as A7's
// post pipeline, which is where the cloud comes from. Capture is Android-only
// (§3.5) but nothing here is: it reads keyframes off disk (`frames_idx.h`),
// pixels through an `ImageSource`, and writes RGB into `PointVertex`.
//
// --- what it does NOT do ----------------------------------------------------
//
// It does not decide whether the session may be coloured at all. That is
// `policy_for(SyncQuality)` in `clock_sweep.h`, driven by A4's
// `TimeSync::quality()`, and the caller passes the answer in. A4 §7 is
// explicit that consumers gate on `sync_quality()`, never on `jitter_ns`.
//
// --- determinism ------------------------------------------------------------
//
// Single-threaded, fixed iteration order (keyframes in index order, points in
// store order), ties broken by the lower keyframe index. The same inputs
// produce byte-identical output — asserted by
// `color/colorize_is_deterministic` — which is what makes a re-run in the
// cloud comparable to a re-run on the phone.
//
// Owner: A11.
#ifndef SCANENGINE_COLOR_COLORIZER_H
#define SCANENGINE_COLOR_COLORIZER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/color/colorize.h"
#include "scanengine/color/clock_sweep.h"
#include "scanengine/color/frames_idx.h"
#include "scanengine/color/image_source.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/slam/post/progress.h"

namespace scanengine {
namespace color {

// What happened to one point's colour. Parallel to the point array; also
// summarised in `ColorizeStats`.
enum class ColorCoverage : std::uint8_t {
  // No acceptable view. The point KEEPS the colour it arrived with — the
  // intensity-derived ramp the drivers wrote — and is flagged so a UI can
  // show a coverage map and a user can go back and photograph the gap.
  kNone = 0,
  kColorized = 1,
  // Coloured, but only from a keyframe above the motion gate, or through a
  // marginal incidence angle. S6 §6.3: "points that can only be seen from
  // fast-turn frames should be coloured anyway but flagged low-confidence —
  // the same treatment §3.3 already gives ARCore tracking-loss points."
  kLowConfidence = 2,
};

const char* to_string(ColorCoverage c) noexcept;

// Which frame `Keyframe::pose` is expressed in.
enum class KeyframePoseFrame : std::uint8_t {
  // ARCore's camera pose, `world_from_camera`. The default, and what B8
  // records: A8 §3.1 established that the phone frame and the camera frame
  // are one frame, so a cloud assembled as
  // `world_from_phone · phone_from_lidar · p_lidar` is already in this world.
  kCamera = 0,
  // `world_from_lidar_body` — a trajectory whose poses are the LIDAR's, as
  // A6/A7 produce. The colorizer then composes the mount extrinsic itself:
  // `world_from_camera = world_from_body · inverse(camera_from_lidar)`.
  kLidarBody = 1,
};

enum class ColorStage : std::uint8_t {
  kIdle = 0,
  kPreparing = 1,   // validate keyframes, resolve motion/velocity, bounds
  kNormals = 2,     // PCA surface normals for the incidence term
  kSelecting = 3,   // per keyframe: z-buffer + best-view competition
  kDone = 4,
  kCancelled = 5,
  kFailed = 6,
};

const char* to_string(ColorStage s) noexcept;

struct ColorProgress {
  ColorStage stage = ColorStage::kIdle;
  const char* label = "idle";
  float fraction = 0.f;        // overall, 0..1, monotone non-decreasing
  float stage_fraction = 0.f;  // within the stage
  std::uint64_t done = 0;      // units depend on the stage (points, keyframes)
  std::uint64_t total = 0;
};

// Invoked on the thread inside colorize(). Quick, and must not re-enter the
// colorizer — the same rule A7's `PostProgressFn` follows.
using ColorProgressFn = std::function<void(const ColorProgress&)>;

// The motion gate's input: mean |gyro| over a window ending at `t_engine_ns`,
// in rad/s. Deliberately a callable rather than an `ImuIngest&`, so the
// colorizer has no dependency on a live IMU ring and the tests can drive the
// gate analytically. The production adapter is one line:
//
//   c.set_angular_rate_fn([&imu](std::int64_t t, double* r) {
//     return imu.angular_rate_at(t, 250'000'000, r);
//   });
//
// Returning false means "unknown at this time"; the colorizer then falls back
// to the keyframe's own recorded `angular_rate_rad_s` (frames.idx), and if
// that is absent too, treats the frame as unGATED and counts it.
using AngularRateFn = std::function<bool(std::int64_t t_engine_ns, double* rad_per_s)>;

// `world_from_camera` at an arbitrary time, row-major 4x4 — what the
// rolling-shutter model needs, since row `r` is exposed
// `r · row_time` after row 0. The production source is A7's optimized
// trajectory (`PostSlamPipeline::trajectory()`, a `PoseSource`).
//
// When no function is supplied, the colorizer estimates a constant angular
// and linear velocity per keyframe by differencing its NEIGHBOURING
// keyframes, which is enough for a 20–30 ms readout and needs nothing but
// frames.idx. See docs/A11-color.md §5.
using PoseAtFn = std::function<bool(std::int64_t t_engine_ns, double world_from_camera[16])>;

struct ColorizeConfig {
  // --- gating (S6 §1, §6.3) ------------------------------------------------
  //
  // From `TimeSync::quality(StreamId::kLidarMid360)`. The default fails
  // CLOSED: `kUnknown` means "not converged", and A4 §4 is explicit that a
  // consumer reading jitter before convergence must refuse.
  SyncQuality sync_quality = SyncQuality::kUnknown;
  bool allow_poor_sync = false;
  // 0 = take the gate from `policy_for(sync_quality)`. Non-zero overrides it
  // (the wizard may know better after a successful clock sweep).
  float motion_gate_deg_s = 0.f;
  float motion_reject_deg_s = 0.f;

  // The wizard's 8-second sweep result: t_engine = t_camera + offset.
  std::int64_t camera_clock_offset_ns = 0;

  // --- geometry ------------------------------------------------------------
  KeyframePoseFrame pose_frame = KeyframePoseFrame::kCamera;
  float min_range_m = 0.30f;   // closer than this is the operator's hand
  float max_range_m = 30.0f;   // S6's budget is quoted at 3 m; 30 m is generous
  // Points seen edge-on smear: their colour comes from a footprint many
  // pixels long. 75° is the standard photogrammetric cut.
  float max_incidence_deg = 75.f;
  // Reject projections nearer than this to the image border: lens
  // distortion is worst there, and a point that lands on the very edge is
  // usually about to leave the frame during the exposure.
  std::uint32_t edge_margin_px = 8;
  // Prefer keyframes that see the point from about this range. The distance
  // term is `ref / (ref + range)`, so it is smooth, monotone, and never zero.
  float distance_ref_m = 2.0f;

  // --- occlusion z-buffer --------------------------------------------------
  //
  // The depth image is rendered from the cloud itself at a reduced
  // resolution: a point cloud has no surfaces, so a full-res depth buffer is
  // mostly holes and would occlude nothing. 1/8 of a 4032×3024 frame is
  // 504×378, at which a 5 cm-spaced cloud at 3 m covers every pixel.
  float depth_scale = 0.125f;
  std::uint32_t splat_radius_px = 1;   // in REDUCED pixels; 1 → a 3x3 splat
  // A point is visible when its depth is within tolerance of the buffer:
  //   depth <= zbuf + (abs + rel * depth)
  // The relative term is the honest one — depth error scales with range —
  // and the absolute term covers the splat's own footprint.
  float depth_tolerance_m = 0.05f;
  float depth_relative_tolerance = 0.01f;
  // Slope-scaled bias, the same idea shadow mapping uses and for the same
  // reason. One depth cell covers a patch of the world whose DEPTH SPREAD
  // grows with the surface's obliquity: seen at 60°, a cell 20 cm wide spans
  // 35 cm of depth, so the far half of a perfectly visible wall is "behind"
  // the near half and a fixed tolerance rejects it — a measured 10 % of the
  // points in the synthetic room, in stripes, before this term existed. The
  // bias added is `factor * cell_width_at_this_depth * tan(incidence)`, so it
  // is zero on a surface facing the camera and only ever loosens the test
  // where the buffer genuinely cannot resolve the difference.
  float depth_slope_bias = 1.0f;
  bool occlusion_test = true;

  // --- rolling shutter (S6 §7.1 item 2) ------------------------------------
  bool rolling_shutter = true;
  // The row a point lands on depends on the pose at that row's time, which
  // depends on the row. Three fixed-point iterations settle a 20 ms readout
  // to well under a pixel; the first iteration does almost all the work.
  std::uint32_t rolling_shutter_iterations = 3;

  // --- surface normals (the incidence term) --------------------------------
  //
  // PointVertex carries no normal, so the incidence angle has to be
  // estimated. PCA over a voxel neighbourhood costs one pass and ~12 bytes
  // per point; with normals off, the incidence term is neutral (1.0) and
  // selection falls back to distance × motion, which is measurably worse on
  // grazing surfaces (docs/A11-color.md §4).
  bool estimate_normals = true;
  float normal_radius_m = 0.15f;
  std::uint32_t normal_min_neighbors = 6;

  // --- scoring weights -----------------------------------------------------
  // score = incidence^w_inc * distance^w_dist * motion^w_motion
  float w_incidence = 1.0f;
  float w_distance = 1.0f;
  float w_motion = 2.0f;  // S6 §6.1: sync × turn rate is the dominant term

  // --- output --------------------------------------------------------------
  // Alpha is A8's flag channel and A14's LOD/selection channel, so it is left
  // alone by default. Non-zero writes this alpha into low-confidence points,
  // the way `PushbroomConfig::flagged_alpha` marks flagged ones.
  std::uint8_t low_confidence_alpha = 0;
  std::uint8_t uncovered_alpha = 0;

  // How many points between progress callbacks / cancellation polls.
  std::uint32_t progress_point_interval = 65536;
};

struct ColorizeStats {
  // --- input ---------------------------------------------------------------
  std::uint64_t points_total = 0;
  std::uint32_t keyframes_total = 0;
  std::uint32_t keyframes_used = 0;          // contributed at least one colour
  std::uint32_t keyframes_rejected_motion = 0;
  std::uint32_t keyframes_rejected_pose = 0;  // tracking lost / invalid quality
  std::uint32_t keyframes_outside_cloud = 0;  // frustum missed the bounds
  std::uint32_t keyframes_image_failed = 0;

  // --- output --------------------------------------------------------------
  std::uint64_t points_colorized = 0;
  std::uint64_t points_low_confidence = 0;
  std::uint64_t points_uncovered = 0;

  // --- why points were refused (counted per point×keyframe attempt) --------
  std::uint64_t rejected_behind = 0;
  std::uint64_t rejected_outside = 0;    // off image / inside the edge margin
  std::uint64_t rejected_range = 0;
  std::uint64_t rejected_incidence = 0;
  std::uint64_t rejected_occluded = 0;
  std::uint64_t accepted_samples = 0;

  // --- normals -------------------------------------------------------------
  std::uint64_t normals_estimated = 0;
  std::uint64_t normals_missing = 0;

  // --- timing (wall clock, ms) ---------------------------------------------
  double ms_normals = 0.0;
  double ms_decode = 0.0;
  double ms_depth = 0.0;
  double ms_select = 0.0;
  double ms_total = 0.0;

  float coverage_fraction() const {
    return points_total == 0
               ? 0.f
               : static_cast<float>(static_cast<double>(points_colorized) /
                                    static_cast<double>(points_total));
  }
};

// The A11 implementation of `Colorizer`.
//
// Threading: `colorize()` is blocking and single-threaded. `cancel()`,
// `progress()` and `stats()` are safe from another thread while it runs;
// nothing else is.
class PointColorizer final : public Colorizer {
 public:
  explicit PointColorizer(const ColorizeConfig& cfg = ColorizeConfig());
  ~PointColorizer() override;
  PointColorizer(const PointColorizer&) = delete;
  PointColorizer& operator=(const PointColorizer&) = delete;

  // --- colorize.h seam -----------------------------------------------------
  //
  // `camera_from_lidar` is the mount extrinsic A8's wizard recovers. It is
  // USED when `pose_frame == kLidarBody` (to turn a lidar-body trajectory
  // into camera poses) and validated but otherwise unused when the keyframe
  // poses are already ARCore camera poses. Rejects a non-rigid matrix —
  // A8 §4.4's column-major-across-JNI trap.
  Status set_extrinsics(const double camera_from_lidar[16]) override;
  Status add_keyframe(const Keyframe& kf) override;
  // Colours every page in the store IN PLACE. See docs/A11-color.md §7 for
  // the one seam this needs from `cloud/`: the store exposes only const
  // views today.
  Status colorize(PageStore* points) override;
  float progress() const override;
  // A15 §7.6's two abstract-seam hooks, now real overrides (INT-34). The
  // token is not owned; null restores this object's own internal token, so
  // cancel() below keeps working either way. set_progress_fn() is the
  // coarse, interface-level form of set_progress_callback() and is
  // implemented in terms of it — a caller wanting the stage/label/counts
  // still uses the richer one, and setting either replaces the other.
  void set_cancel_token(post::CancelToken* token) override;
  void set_progress_fn(ColorizeProgressFn cb) override;

  // --- A11 additions -------------------------------------------------------
  // The core. `points` are in the session's world frame; only the r/g/b (and
  // optionally a) bytes are written.
  Status colorize_points(Span<PointVertex> points);

  // Loads every keyframe from `<lscan_dir>/streams/frames/frames.idx` and
  // installs a `FileImageSource` rooted at `lscan_dir` (unless one was set).
  // kNotFound when the session has no camera — §3.5's "gracefully
  // unavailable" case, which a caller should report, not treat as a failure.
  Status load_keyframes(const std::string& lscan_dir, FrameIndexStats* stats = nullptr);

  void set_image_source(ImageSource* src);      // not owned
  void set_angular_rate_fn(AngularRateFn fn);
  void set_pose_fn(PoseAtFn fn);
  void set_progress_callback(ColorProgressFn cb);
  void cancel();

  ColorStage stage() const;
  const ColorizeStats& stats() const;
  const ColorizeConfig& config() const;
  const std::vector<Keyframe>& keyframes() const;
  // Per-point coverage, parallel to the last colorized array. Empty before
  // the first run.
  Span<const ColorCoverage> coverage() const;
  // The gate actually applied, after `policy_for()` and any override.
  const ColorizationPolicy& policy() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace color
}  // namespace scanengine

#endif  // SCANENGINE_COLOR_COLORIZER_H
