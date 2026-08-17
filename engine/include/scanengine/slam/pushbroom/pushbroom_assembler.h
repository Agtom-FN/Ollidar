// pushbroom_assembler.h — COIN-D6 profiles + trajectory → a 3-D cloud.
//
// Tech Spec §3.3, "D6 pushbroom": a rigid phone+lidar bracket, the trajectory
// from the pose fusion layer, and a 2-D scanner whose single vertical profile
// only becomes a surface because the rig MOVES. The assembler is the join:
//
//     p_world(t) = world_from_phone(t) · phone_from_lidar · p_lidar
//
// with `world_from_phone(t)` interpolated per point (poses/pose_interpolator.h)
// and `phone_from_lidar` the mount extrinsic that
// slam/pushbroom/mount_calibration.h recovers.
//
// Three properties this file exists to guarantee:
//
//  1. **Per-point time, not per-packet time.** The D6 spins at 10 Hz, so one
//     revolution spans 100 ms. Walking at 1 m/s that is 10 cm of rig travel
//     inside a single revolution and ~3 degrees of yaw at a gentle turn. A
//     profile stamped with one time smears exactly that much. `ProfilePoint`
//     therefore carries its own `t_mono_ns`; the `Span<const PointVertex>`
//     overload inherited from PushbroomAssembler is the coarse path and says
//     so.
//
//  2. **Buffer, never guess.** A point whose pose has not arrived yet is
//     PENDING, not dropped and not extrapolated. ARCore poses arrive at
//     ~30 Hz behind the point stream, so without a pending queue the first
//     points of every batch would be lost. `drain()` resolves what the poses
//     allow; `flush()` gives up on the rest at end of stream.
//
//  3. **Tracking loss is flagged, and excluded by default** (§3.3, and S6
//     REPORT §6.3 which gives the same treatment to fast-turn colorization).
//     `exclude_flagged = true` drops them; setting it false emits them with
//     `flagged_alpha` in the alpha channel so a renderer can grey them out
//     and an exporter can filter them, rather than mixing unmarked garbage
//     into the cloud.
//
// **Replay == capture** (§3 key rule 2). The assembler is a pure function of
// (profile points, pose stream, extrinsic): it never reads a clock, never
// samples wall time, and produces the same world points whether the poses
// arrive interleaved with the points (live) or all up front (offline over a
// replayed .lscan). `pushbroom/assembles_identically_live_and_offline` in
// tests/test_pushbroom.cpp asserts that bit for bit.
//
// Threading: the assembler is NOT internally synchronized and owns no thread.
// It is driven from the one thread that decodes D6 packets — the app's serial
// reader live, the replay thread offline — exactly like D6Driver, whose
// contract already is "one Driver instance is pushed from one thread at a
// time" (DESIGN.md §2). The PoseInterpolator it reads IS thread-safe, so the
// app may push poses from its ARCore thread concurrently.
//
// Owner: A8.
#ifndef SCANENGINE_SLAM_PUSHBROOM_PUSHBROOM_ASSEMBLER_H
#define SCANENGINE_SLAM_PUSHBROOM_PUSHBROOM_ASSEMBLER_H

#include <cstdint>
#include <deque>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/slam/slam.h"

namespace scanengine {

// One D6 return, in the polar form the wire actually carries (d6::Point) plus
// the engine-time stamp A4 mapped it to. Converting to Cartesian is the
// assembler's job, so the sensor-frame convention lives in exactly one place.
//
// Sensor frame (identical to D6Driver's, deliberately — the mount extrinsic is
// defined against it): see `drivers/d6/d6_fan.h`, which is the ONE place the
// convention is written down and the only place it is computed.
//
//     x = -d·sin(theta), y = d·cos(theta), z = 0,  theta = 0 along +y
//
// with +z out of the BASE of the unit. ROUND 9 item 34 corrected the sign of
// the x term: the vendor states its angle convention in a LEFT-handed frame,
// and transcribing it into a right-handed one reverses the sweep, which
// mirrored every resolved cloud left-for-right. d6_fan.h derives it in full.
struct ProfilePoint {
  std::int64_t t_mono_ns = 0;
  float angle_deg = 0.0f;   // [0, 360); wraps freely, no unwrapping required
  float range_m = 0.0f;     // 0 = no return
  std::uint8_t intensity = 0;
  std::uint8_t high_reflectivity = 0;
};

struct PushbroomConfig {
  // Stream the assembled world points are appended under. The raw sensor-frame
  // preview D6Driver writes today also uses kLidarD6; an app that wants both
  // live views at once should give the assembler its own StreamId.
  StreamId out_stream = StreamId::kLidarD6;

  // §3.3: "Points during ARCore tracking loss are flagged and excluded by
  // default." `exclude_flagged` is that default; turning it off keeps the
  // points but marks them with `flagged_alpha`.
  bool exclude_flagged = true;
  std::uint8_t flagged_alpha = 96;

  // COIN-D6 range window (spec §2.1: 0.05–12 m). Returns outside it are not
  // geometry — 0 is "no return" and anything past 12 m is a stray.
  float min_range_m = 0.05f;
  float max_range_m = 12.0f;

  // Pending-queue bound. 200k points is ~50 s of D6 at its 4 kpts/s: far more
  // slack than a pose stream ever needs, and still a hard ceiling so a pose
  // source that dies mid-session cannot grow the queue without limit. When it
  // overflows the OLDEST pending points are dropped (they are the ones whose
  // poses are most overdue) and counted in `dropped_overflow`.
  std::size_t max_pending_points = 200'000;

  // Points handed to PageStore::append() per call. Matches D6Config's
  // batching rationale: ~4096 points is 64 kB, the granularity S3 measured
  // the renderer at.
  std::uint32_t batch_points = 4096;

  // Resolve pending points on every push. Turning it off lets an offline job
  // push an entire capture and call drain() once.
  bool drain_on_push = true;
};

struct PushbroomStats {
  std::uint64_t points_in = 0;
  std::uint64_t points_out = 0;         // reached the PageStore
  std::uint64_t points_pending = 0;     // waiting for a pose right now

  std::uint64_t dropped_range = 0;      // 0 / out-of-window returns
  std::uint64_t dropped_no_pose = 0;    // before the first pose, or unresolved at flush()
  std::uint64_t dropped_overflow = 0;   // pending queue bound hit
  std::uint64_t dropped_page_full = 0;  // PageStore backpressure

  // Broken out per gate, because they mean different things to the user:
  // tracking loss is "walk back and rescan", stale is "your pose stream
  // stuttered", low confidence is "ARCore was struggling".
  std::uint64_t flagged_tracking_lost = 0;
  std::uint64_t flagged_stale_pose = 0;
  std::uint64_t flagged_low_confidence = 0;
  std::uint64_t flagged_emitted = 0;    // flagged AND kept (exclude_flagged == false)

  std::int64_t t_first_ns = 0;
  std::int64_t t_last_ns = 0;

  std::uint64_t flagged_total() const {
    return flagged_tracking_lost + flagged_stale_pose + flagged_low_confidence;
  }
};

class D6PushbroomAssembler final : public PushbroomAssembler {
 public:
  // `points` may be null for a dry run (stats only, e.g. a coverage
  // estimate); `poses` may be set later with set_pose_source().
  D6PushbroomAssembler(PageStore* points, const PushbroomConfig& cfg = {});
  ~D6PushbroomAssembler() override;

  // --- PushbroomAssembler ------------------------------------------------

  // `phone_from_lidar`, ROW-MAJOR 4x4 (poses/se3.h conventions). This is the
  // same transform mount_calibration.h calls `camera_from_lidar`: ARCore's
  // pose IS the camera pose, so the phone frame and the camera frame are one
  // frame. Rejects a matrix that is not rigid (kInvalidArgument) — a
  // column-major matrix handed across JNI otherwise produces a silently
  // mirrored cloud.
  Status set_mount_extrinsics(const double phone_from_lidar[16]) override;

  // Coarse path: sensor-frame Cartesian points that all share one stamp.
  // Provided because it is the A1 seam; prefer push_profile(ProfilePoint) so
  // each point carries its own time (see the file header, point 1).
  Status push_profile(Span<const PointVertex> profile, std::int64_t t_mono_ns) override;

  // --- the path the D6 driver should use ---------------------------------
  Status push_profile(Span<const ProfilePoint> profile);
  Status push_point(const ProfilePoint& p);

  void set_pose_source(const PoseInterpolator* poses);
  const PoseInterpolator* pose_source() const { return poses_; }
  void get_mount_extrinsics(double phone_from_lidar[16]) const;
  bool has_mount_extrinsics() const { return have_extrinsics_; }

  // Resolve every pending point whose pose has arrived. Stops at the first
  // point that is still in the future (the queue is time-ordered, so nothing
  // behind it can be resolvable either).
  Status drain();

  // End of stream: resolve what can be resolved, then discard the rest as
  // `dropped_no_pose`. Call once when the capture stops or the replay ends.
  Status flush();

  PushbroomStats stats() const { return stats_; }
  std::size_t pending() const { return pending_.size(); }
  void reset();

  const PushbroomConfig& config() const { return cfg_; }
  void set_config(const PushbroomConfig& cfg) { cfg_ = cfg; }

 private:
  Status resolve_(bool force);
  void emit_(const PointVertex& v);
  Status flush_batch_(std::int64_t t_ns);

  PageStore* points_;
  const PoseInterpolator* poses_ = nullptr;
  PushbroomConfig cfg_;

  double phone_from_lidar_[16];
  bool have_extrinsics_ = false;

  std::deque<ProfilePoint> pending_;
  std::vector<PointVertex> batch_;
  std::int64_t batch_t_ns_ = 0;
  bool overflow_warned_ = false;
  PushbroomStats stats_{};
};

}  // namespace scanengine

#endif  // SCANENGINE_SLAM_PUSHBROOM_PUSHBROOM_ASSEMBLER_H
