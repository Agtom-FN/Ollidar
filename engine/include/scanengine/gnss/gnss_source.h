// gnss_source.h — NMEA bytes in, fixes and poses out (Tech Spec §2.3, §3.4).
//
// This is `GnssReceiver` made real: the app pushes whatever the Bluetooth SPP
// / USB-serial link handed it, and this class produces
//
//   * a `GnssFix` per epoch — position, fix state, per-axis sigmas, DOPs,
//     corrections age, UTC — for B9's status strip and the §3.4 capture gate;
//   * a `Pose` per epoch on `StreamId::kGnss`, in the session's local ENU
//     frame, so that A8's pushbroom assembler can consume an RTK trajectory
//     through exactly the same `PoseInterpolator` seam it uses for ARCore
//     (§3 key rule 3 — "ARCore indoors, RTK outdoors" is configuration, not a
//     code path). §3.3: "Desktop D6 capture: no ARCore → RTK-trajectory …
//     mode only" is this object being handed to `D6PushbroomAssembler`.
//
// ### Three design points worth stating
//
// **1. Time.** §3.2: "GNSS: NMEA time via arrival correlation". The UTC in a
// sentence is the epoch the position refers to; the arrival stamp is when the
// bytes reached us, late by the receiver's processing plus the SPP link. Both
// go into A4 as an `(t_device, t_arrival)` pair — `TimeSync` already installs
// the min-delay estimator for `StreamId::kGnss` (docs/A4-timesync.md §7) —
// and the fix's `t_mono_ns` is the A4-mapped result. This is what removes the
// Bluetooth latency from the trajectory instead of baking it into the
// geometry. A4 §6 warns that a 1 Hz stream needs ~16 s to converge; until
// then the mapping is the arrival stamp itself, which is exactly the
// pre-A10 behaviour, and `GnssStats::time_converged` says so.
//
// **2. Epoch assembly.** A receiver emits a BURST per epoch — GGA, RMC, GST,
// GSA, VTG — all carrying the same UTC. The sigmas that matter for fusion
// live in GST, which arrives AFTER the GGA that carries the position, so
// emitting on GGA would systematically publish the fallback sigma and never
// the measured one. The epoch is therefore closed when a sentence with a
// DIFFERENT UTC arrives, or on `flush()`, or after `epoch_timeout_ns` of
// silence. Cost: one epoch of latency (200 ms at 5 Hz, 1 s at 1 Hz) on the
// FIX CALLBACK. It costs the trajectory nothing — poses are timestamped with
// the epoch's own UTC-derived engine time, so a late-published pose is still
// correctly placed in time, and A8's assembler already buffers points whose
// pose has not arrived (`PoseGate::kFuture`).
//
// **3. Orientation.** A single-antenna GNSS receiver measures POSITION, not
// attitude. Course-over-ground is a direction of travel, and only while
// actually travelling. So: when speed exceeds `min_speed_for_heading_mps` the
// pose carries a yaw-only quaternion from COG with an honest
// `orientation_sigma_deg`; below it, identity with sigma 180°, meaning "no
// information". A consumer that takes a stationary rover's heading seriously
// will spin the cloud, so the number says not to.
//
// Owner: A10.
#ifndef SCANENGINE_GNSS_GNSS_SOURCE_H
#define SCANENGINE_GNSS_GNSS_SOURCE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/gnss/gnss.h"
#include "scanengine/gnss/nmea.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/timesync/offset_estimator.h"

namespace scanengine {

struct GnssSourceConfig {
  StreamId stream = StreamId::kGnss;

  // Fix ring. 4096 epochs is ~68 min at 1 Hz / 13 min at 5 Hz. The
  // assembler only looks a fraction of a second back; this is sized for
  // "the app stopped draining for a while" and for a post-session review of
  // the fix timeline.
  std::size_t capacity = 4096;

  // A4. Null ⇒ stamps are used as-is (replay, and unit tests that want a
  // deterministic mapping).
  TimeSync* timesync = nullptr;

  // Feed (UTC, arrival) pairs into A4. Requires an RMC date to build an
  // absolute UTC; before the first RMC the source falls back to arrival
  // stamps and counts it.
  bool correlate_utc = true;

  // Below this, no origin is set and no pose is published. kSingle by
  // default so an indoor/urban session still gets a coarse trajectory; §3.4's
  // "capture UX warns/blocks below fix-quality threshold" is B9 raising this
  // to kRtkFixed, not this class deciding for it.
  FixType min_fix_for_pose = FixType::kSingle;

  // The ENU origin is anchored on the first fix at or above this quality and
  // then never moves — a session whose origin drifts has a cloud whose
  // coordinates mean different things at different times. kSingle by default
  // for the same reason as above; set kRtkFixed for a survey profile.
  FixType min_fix_for_origin = FixType::kSingle;

  // Fallback 1-sigma when the receiver sends no GST, scaled by HDOP/1.0 when
  // `scale_fallback_by_hdop`. Defaults come from default_sigma_for_fix().
  bool scale_fallback_by_hdop = true;
  double vertical_sigma_ratio = 1.6;  // GNSS vertical is ~1.5–2x horizontal

  double min_speed_for_heading_mps = 0.5;
  // Yaw sigma is atan(sigma_h / distance_travelled_per_epoch) in principle;
  // in practice a COG at walking speed is good to a few degrees and a
  // stationary one is meaningless. These two bracket it.
  double heading_sigma_moving_deg = 5.0;
  double heading_sigma_static_deg = 180.0;

  // Interpolation gates (mirrors ExternalPoseConfig, deliberately: A8's
  // assembler reads the same PoseSample fields whichever source it holds).
  // 3 s = three missed epochs at 1 Hz.
  std::int64_t max_gap_ns = 3'000'000'000;
  std::int64_t max_extrapolation_ns = 0;
  float min_confidence = 0.0f;  // GNSS confidence is the fix state; see below
  PoseQuality min_quality = PoseQuality::kPoor;

  // Close a pending epoch after this much silence even if no new UTC
  // arrived. 1.5 s covers a 1 Hz receiver's inter-epoch gap.
  std::int64_t epoch_timeout_ns = 1'500'000'000;

  // Constant lever arm from the ANTENNA phase centre to the point whose
  // trajectory we want, in ENU metres. Valid for a pole-mounted antenna
  // directly above the rig (the §2.3 configuration): the offset is then
  // (0, 0, −h) and does not rotate with the rig's yaw. For an antenna offset
  // horizontally from a rotating rig this is wrong and A11's rig calibration
  // has to supply a body-frame lever arm instead — see docs/A10-gnss.md §7.
  double antenna_offset_enu[3] = {0.0, 0.0, 0.0};

  nmea::FramerConfig framer{};
};

struct GnssStats {
  nmea::NmeaStats nmea{};
  std::uint64_t epochs = 0;
  std::uint64_t fixes_published = 0;
  std::uint64_t poses_published = 0;
  std::uint64_t epochs_no_position = 0;   // parsed, but fix quality 0
  std::uint64_t epochs_below_gate = 0;    // below min_fix_for_pose
  std::uint64_t epochs_rejected_time = 0; // non-monotone engine stamp
  std::uint64_t gst_epochs = 0;           // sigmas came from the receiver
  std::uint64_t utc_pairs = 0;            // fed to A4
  std::uint64_t utc_unavailable = 0;      // no RMC date yet
  std::uint64_t overwritten = 0;          // fell off the back of the ring
  std::uint64_t queries = 0, queries_gated = 0;

  bool time_converged = false;
  std::int64_t time_uncertainty_ns = 0;

  // Per-state epoch counts, indexed by FixType. The §3.4 "fix-quality
  // timeline" a session report shows.
  std::uint64_t by_fix[5] = {0, 0, 0, 0, 0};

  double fix_fraction(FixType f) const {
    const std::uint64_t n = by_fix[0] + by_fix[1] + by_fix[2] + by_fix[3] + by_fix[4];
    return n ? static_cast<double>(by_fix[static_cast<std::size_t>(f)]) /
                   static_cast<double>(n)
             : 0.0;
  }
};

class GnssSource final : public GnssReceiver, public PoseInterpolator {
 public:
  explicit GnssSource(const GnssSourceConfig& cfg = {});
  ~GnssSource() override;

  // --- PoseSource -------------------------------------------------------
  const char* name() const override { return "gnss"; }
  StreamId stream() const override { return cfg_.stream; }
  Status start() override;
  Status stop() override;   // flushes the pending epoch
  bool running() const override;

  // GNSS poses are DERIVED here, not pushed. Returns kNotSupported: pushing
  // a pose into a receiver would silently create a trajectory with no fix
  // behind it, which is precisely the confusion §3.4's quality gate exists
  // to prevent. (Replay pushes into an ExternalPoseSource instead.)
  Status push_pose(const Pose& pose) override;

  void set_callback(PoseCallback cb) override;
  Status pose_at(std::int64_t t_mono_ns, Pose* out) const override;

  // --- PoseInterpolator -------------------------------------------------
  PoseSample sample_at(std::int64_t t_mono_ns) const override;
  bool time_span(std::int64_t* first_ns, std::int64_t* last_ns) const override;

  // --- GnssReceiver -----------------------------------------------------
  Status push_nmea(ByteSpan sentence, std::int64_t t_mono_ns) override;
  GnssFix last_fix() const override;

  // --- A10 surface ------------------------------------------------------

  // Called once per closed epoch, on the pushing thread, AFTER the fix and
  // pose are in the ring. B9's status strip and GeorefFusion both hang off
  // this rather than polling.
  using FixCallback = std::function<void(const GnssFix&)>;
  void set_fix_callback(FixCallback cb);

  // Close the pending epoch now. `stop()` does it; a test or a replay that
  // knows the stream ended calls it directly.
  void flush();

  // The session's local ENU frame. Anchored on the first fix at or above
  // `min_fix_for_origin` unless set explicitly BEFORE that fix arrives
  // (kAlreadyExists afterwards — see the header comment on why it may not
  // move).
  Status set_origin(const crs::Geodetic& origin);
  bool origin(crs::Geodetic* out) const;
  bool has_origin() const;
  const crs::EnuFrame& enu_frame() const { return enu_; }

  // The rover's own most recent GGA, verbatim, for NTRIP upload. Empty
  // before the first one. Byte-identical to what the receiver emitted so a
  // VRS caster sees exactly what it would from a stand-alone rover.
  std::string last_gga_sentence() const;

  GnssStats stats() const;
  std::size_t fix_count() const;
  // Newest-last snapshot of the ring. For a session report / the fusion
  // layer's cold start.
  std::vector<GnssFix> fixes() const;

  void clear();

  // ROUND 14 — start of a new capture.
  //
  // Drops the ENU origin this source anchored on its first good fix, and with
  // it every pose in the ring, because those poses are COORDINATES IN THAT
  // FRAME: keeping them across a re-anchor would mix two origins in one ring
  // and interpolate between them. The next fix anchors a new origin, so each
  // capture is expressed about its own local zero — which is what an operator
  // means by "the origin resets when the capture starts".
  //
  // An origin set EXPLICITLY by the app (set_origin(), e.g. a site datum
  // shared by every capture of a job) is kept: it is a decision, not an
  // accident of which fix arrived first, and the ring is then cleared without
  // changing frame. Deliberately does NOT touch the last fix, the NMEA framer
  // or the counters — the rover keeps streaming across Start, and the capture
  // gate in the UI reads exactly those.
  void reset_frame();

  const GnssSourceConfig& config() const { return cfg_; }

  // Exposed for tests and for a diagnostic UI: the quality/confidence mapping
  // the pose gate uses. kRtkFixed→kGood, kRtkFloat/kDgps→kFair,
  // kSingle→kPoor, kNone→kInvalid.
  static PoseQuality quality_for(FixType f) noexcept;
  static float confidence_for(FixType f) noexcept;

 private:
  struct Epoch;
  struct Entry {
    GnssFix fix{};
    Pose pose{};
    bool pose_valid = false;
  };

  void on_sentence_(std::string_view line, const nmea::Sentence& s, std::int64_t t_ns);
  void close_epoch_locked_();
  void publish_locked_(const GnssFix& fix, bool pose_valid, const Pose& pose);
  std::ptrdiff_t upper_index_locked_(std::int64_t t) const;
  const Entry& at_locked_(std::size_t i) const;

  GnssSourceConfig cfg_;
  mutable std::mutex m_;
  nmea::NmeaFramer framer_;
  std::vector<Entry> ring_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  bool running_ = false;

  crs::EnuFrame enu_{};
  bool origin_set_ = false;
  bool origin_explicit_ = false;

  std::unique_ptr<Epoch> pending_;
  // Entries published by the current push, drained AFTER the lock is
  // released: a fix callback that turns around and calls sample_at() (the
  // GeorefFusion path does exactly that) must not deadlock on m_.
  std::vector<Entry> notify_;
  std::string last_gga_;
  GnssFix last_fix_{};
  mutable GnssStats stats_{};

  PoseCallback pose_cb_;
  FixCallback fix_cb_;

  // Date carried forward from the last RMC so a GGA-only epoch still gets a
  // UTC. Rolls over correctly when seconds-of-day goes backwards.
  int year_ = 0, month_ = 0, day_ = 0;
  bool have_date_ = false;
  double last_sod_ = -1.0;
};

}  // namespace scanengine

#endif  // SCANENGINE_GNSS_GNSS_SOURCE_H
