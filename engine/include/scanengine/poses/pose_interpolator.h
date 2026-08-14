// pose_interpolator.h — "give me the rig's pose at time t, and tell me
// whether you actually believe it".
//
// poses/pose_source.h's `PoseSource::pose_at()` returns a Status and a Pose.
// That is the right minimal contract for a caller that only wants a number,
// but it is not enough for the D6 pushbroom assembler: Tech Spec §3.3 says
// "points during ARCore tracking loss are flagged and excluded by default",
// which means the consumer must be able to tell apart
//
//   * "no pose exists yet at that time"       → buffer the point and retry
//   * "the pose is in the future"             → buffer the point and retry
//   * "I have a pose but the bracketing
//      samples are 3 seconds apart"           → interpolation is fiction
//   * "I have a good pose but tracking was
//      lost / confidence was low"             → FLAG the point
//   * "all good"                              → emit the point
//
// Collapsing those five into one ScanError loses exactly the distinction
// §3.3 asks for, so the richer answer gets its own tiny interface. A10's
// fusion layer implements it as well as A8's ExternalPoseSource, which is
// what lets the assembler consume "ARCore indoors, RTK outdoors, blended when
// both" without a code path per source (§3 key rule 3).
//
// Owner: A8 (this interface + ExternalPoseSource) / A10 (PoseFusion).
#ifndef SCANENGINE_POSES_POSE_INTERPOLATOR_H
#define SCANENGINE_POSES_POSE_INTERPOLATOR_H

#include <cstdint>

#include "scanengine/poses/pose_source.h"

namespace scanengine {

// Why a sample is or is not usable. Ordered from "usable" to "unusable"; the
// numeric values are stable and append-only (they will be mirrored in the C
// ABI — see engine/docs/A8-pushbroom.md §7).
enum class PoseGate : std::uint8_t {
  kOk = 0,
  kNoData = 1,          // no poses pushed yet
  kBeforeFirst = 2,     // `t` predates the first pose — never resolvable
  kFuture = 3,          // `t` is newer than the newest pose — retry later
  kStale = 4,           // bracketing poses further apart than max_gap_ns
  kTrackingLost = 5,    // one of the bracketing poses carries tracking_lost
  kLowConfidence = 6,   // below the configured confidence / quality floor
};

const char* to_string(PoseGate g) noexcept;

// A sample carries a Pose whenever one could be computed at all, even when
// the gate rejects it — the assembler needs the geometry in order to place a
// FLAGGED point, and a diagnostic UI wants to draw the trajectory through a
// tracking-loss interval rather than teleporting across it.
struct PoseSample {
  Pose pose{};
  float confidence = 0.0f;
  PoseGate gate = PoseGate::kNoData;

  // True when `pose` holds a real interpolated value. False only for
  // kNoData / kBeforeFirst / kFuture, where there is nothing to interpolate.
  bool has_pose = false;

  // Time between the two bracketing samples, 0 when `t` hit a pushed pose
  // exactly. This is what `kStale` is decided on and what a UI should show
  // when it explains a gap.
  std::int64_t bracket_gap_ns = 0;

  bool ok() const { return gate == PoseGate::kOk; }

  // "A pose exists but you must flag the point." Distinguishing this from
  // `!has_pose` is the whole reason this struct exists.
  bool flagged() const { return has_pose && gate != PoseGate::kOk; }

  // "Ask me again later" — the only two gates a caller can fix by waiting.
  bool retryable() const { return gate == PoseGate::kFuture || gate == PoseGate::kNoData; }
};

class PoseInterpolator {
 public:
  virtual ~PoseInterpolator() = default;

  // Thread-safe: called from the point-producing thread while the app thread
  // pushes poses.
  virtual PoseSample sample_at(std::int64_t t_mono_ns) const = 0;

  // [first, last] engine-time range currently held. Returns false when empty.
  virtual bool time_span(std::int64_t* first_ns, std::int64_t* last_ns) const = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_POSES_POSE_INTERPOLATOR_H
