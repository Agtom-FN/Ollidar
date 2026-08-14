// external_pose_source.h — trajectory pushed in from OUTSIDE the engine.
//
// This is the ARCore path (Tech Spec §3.3/§3.4): the Android app owns the
// ARCore session, and once per camera frame it pushes the VIO pose, its
// tracking state and its confidence across the C ABI (B7 adds the C entry
// points listed in engine/docs/A8-pushbroom.md §7; the C++ API is here now).
// It is deliberately not ARCore-specific — a desktop Qt capture driven by an
// RTK trajectory, a replayed .lscan pose stream, and A6's LIO output all push
// the same way.
//
// What this class adds over a plain ring of poses:
//
//  1. **SE(3) interpolation.** Position lerps, orientation SLERPs on the
//     shortest arc (poses/se3.h). ARCore delivers ~30 Hz; the D6 emits
//     ~4000 points/s, so 99 % of points land BETWEEN two poses and the
//     interpolation is not a nicety — it is where most of the geometry
//     comes from.
//
//  2. **Time mapping through A4.** Poses arrive on the app's clock. `Pose`
//     carries `t_mono_ns` in whatever domain the pusher used; when a
//     `TimeSync` is configured the stamp is mapped with
//     `TimeSync::to_engine_time(stream, t)` on the way in, so everything
//     stored here is already in engine time. For `StreamId::kPoseAr` A4
//     installs a passthrough estimator (ARCore is already CLOCK_BOOTTIME =
//     the engine domain), so this is an identity today — but it is the seam
//     that makes a pose stream with its own clock work without touching any
//     consumer.
//
//  3. **Staleness and confidence gating.** See poses/pose_interpolator.h for
//     why the five outcomes are distinguished, and §3.3 for why tracking-loss
//     points must be flagged rather than silently kept.
//
// Threading: every method is safe from any thread. Poses are pushed from the
// app's ARCore/JNI thread; `sample_at()` is called from the driver thread
// that decodes D6 packets. One mutex, held for a binary search — the D6's
// 4 kpts/s and ARCore's 30 Hz are nowhere near contending.
//
// Owner: A8.
#ifndef SCANENGINE_POSES_EXTERNAL_POSE_SOURCE_H
#define SCANENGINE_POSES_EXTERNAL_POSE_SOURCE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/poses/pose_source.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

// Default confidence for a pose whose pusher did not supply one. ARCore has
// no scalar confidence — it has a TrackingState and a TrackingFailureReason —
// so the app maps those onto PoseQuality and this table turns that into the
// number the gate compares. A pusher that DOES have a scalar (RTK fix
// quality, A6's ESKF covariance) should call the two-argument push_pose().
float pose_confidence(const Pose& p) noexcept;

struct ExternalPoseConfig {
  StreamId stream = StreamId::kPoseAr;

  // Ring capacity. 8192 poses is ~4.5 minutes at ARCore's 30 Hz; the
  // assembler only ever looks a fraction of a second back, so this is sized
  // for "the app stopped pulling points for a while", not for history.
  std::size_t capacity = 8192;

  // Staleness: if the two poses bracketing `t` are further apart than this,
  // the interpolation between them is fiction and the sample is gated
  // kStale. 200 ms is ~6 ARCore frames — long enough to ride out a dropped
  // frame or two, short enough that a walk at 1 m/s cannot cut a corner by
  // more than ~20 cm inside the window.
  std::int64_t max_gap_ns = 200'000'000;

  // How far past the newest pose a sample may be EXTRAPOLATED (held at the
  // last pose, not linearly projected — projecting a VIO pose forward is how
  // you turn a stationary rig into a rocket). 0 disables it, which is the
  // default: a point newer than the newest pose is `kFuture`, and the
  // assembler buffers it until the pose arrives. Non-zero is for a live
  // preview that would rather show a slightly wrong point than nothing.
  std::int64_t max_extrapolation_ns = 0;

  // Confidence floor. Below it a sample is gated kLowConfidence and the
  // assembler flags/excludes the point.
  float min_confidence = 0.35f;

  // Quality floor. kInvalid is ALWAYS rejected regardless of this value.
  PoseQuality min_quality = PoseQuality::kPoor;

  // Gate a sample kTrackingLost when either bracketing pose carries
  // Pose::tracking_lost. §3.3's "flagged and excluded by default" is the
  // combination of this and PushbroomConfig::exclude_flagged.
  bool gate_tracking_lost = true;

  // When set, incoming t_mono_ns is mapped with
  // TimeSync::to_engine_time(stream, t). Null = stamps are already engine
  // time (the D6/desktop/replay case).
  TimeSync* timesync = nullptr;
};

struct ExternalPoseStats {
  std::uint64_t pushed = 0;
  std::uint64_t rejected_out_of_order = 0;  // t <= newest t
  std::uint64_t rejected_invalid = 0;       // non-finite, unnormalizable quaternion
  std::uint64_t overwritten = 0;            // dropped off the back of the ring
  std::uint64_t tracking_lost_poses = 0;
  std::uint64_t queries = 0;
  std::uint64_t queries_gated = 0;
  std::size_t held = 0;
  std::int64_t t_first_ns = 0;
  std::int64_t t_last_ns = 0;
};

class ExternalPoseSource final : public PoseSource, public PoseInterpolator {
 public:
  explicit ExternalPoseSource(const ExternalPoseConfig& cfg = {});
  ~ExternalPoseSource() override;

  // --- PoseSource -------------------------------------------------------
  const char* name() const override { return "external"; }
  StreamId stream() const override { return cfg_.stream; }
  Status start() override;
  Status stop() override;
  bool running() const override;

  // Confidence is derived with pose_confidence(). Rejects a non-finite or
  // zero-norm quaternion (kInvalidArgument) and an out-of-order stamp
  // (kInvalidArgument, counted in stats) — a VIO trajectory is monotone by
  // construction, and silently reordering it would corrupt every
  // interpolation that follows.
  Status push_pose(const Pose& pose) override;

  // Explicit confidence in [0, 1]. Values outside are clamped.
  Status push_pose(const Pose& pose, float confidence);

  void set_callback(PoseCallback cb) override;

  // kOk / kNotFound (nothing usable) / kAgain (t is in the future). The
  // narrow PoseSource contract; prefer sample_at() when the reason matters.
  Status pose_at(std::int64_t t_mono_ns, Pose* out) const override;

  // --- PoseInterpolator -------------------------------------------------
  PoseSample sample_at(std::int64_t t_mono_ns) const override;
  bool time_span(std::int64_t* first_ns, std::int64_t* last_ns) const override;

  // --- diagnostics / lifecycle ------------------------------------------
  ExternalPoseStats stats() const;
  std::size_t size() const;
  void clear();
  const ExternalPoseConfig& config() const { return cfg_; }

 private:
  struct Entry {
    Pose pose{};
    float confidence = 0.0f;
  };

  // Caller holds m_. Index of the last entry with t <= t_query, or -1.
  std::ptrdiff_t upper_index_locked_(std::int64_t t) const;
  const Entry& at_locked_(std::size_t i) const;

  ExternalPoseConfig cfg_;
  mutable std::mutex m_;
  std::vector<Entry> ring_;
  std::size_t head_ = 0;  // index of the OLDEST entry
  std::size_t count_ = 0;
  bool running_ = false;
  PoseCallback cb_;
  mutable ExternalPoseStats stats_{};
};

}  // namespace scanengine

#endif  // SCANENGINE_POSES_EXTERNAL_POSE_SOURCE_H
