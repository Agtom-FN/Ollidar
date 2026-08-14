// pose_source.h — pluggable, fusable trajectory sources (Tech Spec §3 key
// rule 3 and §3.4).
//
// ARCore VIO, GNSS/RTK and LIO all produce poses on the engine clock and all
// feed one fusion layer. Nothing downstream (the D6 pushbroom assembler, the
// colorization projector, the exporters) may care which source a pose came
// from beyond its `source` tag and its quality — that is what makes
// "ARCore indoors, RTK outdoors, blended when both" a configuration rather
// than a code path.
//
// Owner: A1 (interface) / A4 (clock alignment) / A8 (pushbroom consumer,
// ARCore ingestion) / A10 (GNSS/RTK + factor-graph fusion).
#ifndef SCANENGINE_POSES_POSE_SOURCE_H
#define SCANENGINE_POSES_POSE_SOURCE_H

#include <cstdint>
#include <functional>

#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {

enum class PoseQuality : std::uint8_t {
  kInvalid = 0,   // do not use; points stamped here are flagged and excluded
  kPoor = 1,      // ARCore limited tracking / GNSS single fix
  kFair = 2,      // DGPS / RTK float / recovering VIO
  kGood = 3,      // healthy VIO / RTK fixed
};

// Position in the session's local metric frame; orientation as a unit
// quaternion (x, y, z, w). Doubles for position because A10 works in
// projected CRS coordinates where float loses centimetres.
struct Pose {
  std::int64_t t_mono_ns = 0;
  double position[3] = {0.0, 0.0, 0.0};
  double orientation[4] = {0.0, 0.0, 0.0, 1.0};
  float position_sigma_m = 0.0f;
  float orientation_sigma_deg = 0.0f;
  StreamId source = StreamId::kUnknown;
  PoseQuality quality = PoseQuality::kInvalid;
  std::uint8_t tracking_lost = 0;  // ARCore tracking-loss flag (§3.3)
};

using PoseCallback = std::function<void(const Pose&)>;

class PoseSource {
 public:
  virtual ~PoseSource() = default;

  virtual const char* name() const = 0;
  virtual StreamId stream() const = 0;
  virtual Status start() = 0;
  virtual Status stop() = 0;
  virtual bool running() const = 0;

  // Push model, matching the transports: ARCore poses arrive from the app
  // through JNI, GNSS poses from the NMEA parser, LIO poses from A6.
  virtual Status push_pose(const Pose& pose) = 0;

  // Delivered on the pushing thread.
  virtual void set_callback(PoseCallback cb) = 0;

  // Interpolated lookup — what the pushbroom assembler and the colorization
  // projector actually call. kNotFound before the first pose, kAgain when
  // `t` is newer than the newest pose (the caller should buffer and retry).
  virtual Status pose_at(std::int64_t t_mono_ns, Pose* out) const = 0;
};

// The fusion layer (A10): consumes several PoseSources, publishes one fused
// trajectory on StreamId::kPoseFused, and owns the local↔global similarity
// transform that georeferences the session.
class PoseFusion {
 public:
  virtual ~PoseFusion() = default;
  virtual Status add_source(PoseSource* source, float weight) = 0;
  virtual Status pose_at(std::int64_t t_mono_ns, Pose* out) const = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_POSES_POSE_SOURCE_H
