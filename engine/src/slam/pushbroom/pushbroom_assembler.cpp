#include "scanengine/slam/pushbroom/pushbroom_assembler.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/drivers/d6/d6_fan.h"
#include "scanengine/poses/se3.h"

namespace scanengine {
namespace {
constexpr const char* kMod = "pushbroom";
}  // namespace

D6PushbroomAssembler::D6PushbroomAssembler(PageStore* points, const PushbroomConfig& cfg)
    : points_(points), cfg_(cfg) {
  se3::mat4_identity(phone_from_lidar_);
  batch_.reserve(cfg_.batch_points);
}

D6PushbroomAssembler::~D6PushbroomAssembler() = default;

Status D6PushbroomAssembler::set_mount_extrinsics(const double phone_from_lidar[16]) {
  if (phone_from_lidar == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "pushbroom: null mount extrinsic");
  }
  if (!se3::mat4_is_rigid(phone_from_lidar, 1e-4)) {
    // Loud on purpose. The two ways this fails in the field are a
    // column-major matrix crossing JNI and an uninitialised buffer; both
    // produce a plausible-looking but wrong cloud if we accept them.
    return set_last_error(ScanError::kInvalidArgument,
                          "pushbroom: mount extrinsic is not a rigid row-major 4x4 "
                          "(check row/column-major at the caller)");
  }
  for (int i = 0; i < 16; ++i) phone_from_lidar_[i] = phone_from_lidar[i];
  have_extrinsics_ = true;
  return kOkStatus;
}

void D6PushbroomAssembler::get_mount_extrinsics(double phone_from_lidar[16]) const {
  if (phone_from_lidar == nullptr) return;
  for (int i = 0; i < 16; ++i) phone_from_lidar[i] = phone_from_lidar_[i];
}

void D6PushbroomAssembler::set_pose_source(const PoseInterpolator* poses) { poses_ = poses; }

void D6PushbroomAssembler::reset() {
  pending_.clear();
  batch_.clear();
  batch_times_.clear();
  batch_t_ns_ = 0;
  batch_t_first_ns_ = 0;
  overflow_warned_ = false;
  stats_ = PushbroomStats{};
}

Status D6PushbroomAssembler::push_point(const ProfilePoint& p) {
  ++stats_.points_in;
  if (stats_.t_first_ns == 0 || p.t_mono_ns < stats_.t_first_ns) stats_.t_first_ns = p.t_mono_ns;
  if (p.t_mono_ns > stats_.t_last_ns) stats_.t_last_ns = p.t_mono_ns;

  if (!(p.range_m >= cfg_.min_range_m) || !(p.range_m <= cfg_.max_range_m) ||
      !std::isfinite(p.angle_deg)) {
    ++stats_.dropped_range;
    return kOkStatus;
  }

  pending_.push_back(p);
  if (pending_.size() > cfg_.max_pending_points) {
    const std::size_t excess = pending_.size() - cfg_.max_pending_points;
    for (std::size_t i = 0; i < excess; ++i) pending_.pop_front();
    stats_.dropped_overflow += excess;
    // Once per overflow episode, not once per point: the queue sheds one
    // point per push while it is full, and a log line per D6 return would be
    // 4000 lines a second.
    if (!overflow_warned_) {
      overflow_warned_ = true;
      SCAN_LOG_WARN(kMod,
                    "pending queue full (%zu points) — no usable pose for the oldest points; "
                    "shedding oldest. Check that the pose source is running.",
                    cfg_.max_pending_points);
    }
  }
  return kOkStatus;
}

Status D6PushbroomAssembler::push_profile(Span<const ProfilePoint> profile) {
  for (std::size_t i = 0; i < profile.size(); ++i) {
    SCAN_TRY(push_point(profile[i]));
  }
  if (cfg_.drain_on_push) return resolve_(false);
  stats_.points_pending = pending_.size();
  return kOkStatus;
}

Status D6PushbroomAssembler::push_profile(Span<const PointVertex> profile,
                                          std::int64_t t_mono_ns) {
  // The coarse seam: sensor-frame Cartesian points sharing one stamp. Invert
  // D6Driver's polar→Cartesian so the rest of the pipeline has exactly one
  // representation to reason about.
  for (std::size_t i = 0; i < profile.size(); ++i) {
    const PointVertex& v = profile[i];
    ProfilePoint p;
    p.t_mono_ns = t_mono_ns;
    p.range_m = static_cast<float>(std::sqrt(static_cast<double>(v.x) * v.x +
                                             static_cast<double>(v.y) * v.y));
    // The exact inverse of d6::fan_point() (d6_fan.h): theta runs from +y
    // toward -x, so recovering it is atan2(-x, y), not atan2(x, y).
    p.angle_deg = static_cast<float>(
        d6::fan_angle_deg(static_cast<double>(v.x), static_cast<double>(v.y)));
    p.intensity = v.r;
    p.high_reflectivity = (v.g > v.r) ? 1 : 0;
    SCAN_TRY(push_point(p));
  }
  if (cfg_.drain_on_push) return resolve_(false);
  stats_.points_pending = pending_.size();
  return kOkStatus;
}

Status D6PushbroomAssembler::drain() { return resolve_(false); }

Status D6PushbroomAssembler::flush() {
  Status s = resolve_(true);
  const Status f = flush_batch_(batch_t_ns_);
  return s.ok() ? f : s;
}

void D6PushbroomAssembler::emit_(const PointVertex& v) { batch_.push_back(v); }

Status D6PushbroomAssembler::flush_batch_(std::int64_t t_ns) {
  if (batch_.empty()) return kOkStatus;
  if (points_ == nullptr) {
    stats_.points_out += batch_.size();
    // A dry run (points_ == nullptr) still "published" them as far as the
    // stats are concerned, so the time sink has to agree or the two would
    // disagree in length for a caller that set both.
    if (cfg_.out_point_times != nullptr) {
      cfg_.out_point_times->insert(cfg_.out_point_times->end(), batch_times_.begin(),
                                   batch_times_.end());
    }
    batch_.clear();
    batch_times_.clear();
    return kOkStatus;
  }
  std::uint32_t appended = 0;
  const Status s = points_->append(cfg_.out_stream, Span<const PointVertex>(batch_.data(), batch_.size()),
                                   t_ns, &appended);
  stats_.points_out += appended;
  if (appended < batch_.size()) stats_.dropped_page_full += batch_.size() - appended;
  // ROUND 11 item 41: only the points the store actually TOOK. PageStore::append
  // fills from the front, so the first `appended` entries are the survivors —
  // which is what keeps out_point_times[k] the k-th published point rather
  // than the k-th attempted one.
  if (cfg_.out_point_times != nullptr && appended > 0) {
    const std::size_t n = std::min(static_cast<std::size_t>(appended), batch_times_.size());
    cfg_.out_point_times->insert(cfg_.out_point_times->end(), batch_times_.begin(),
                                 batch_times_.begin() + static_cast<std::ptrdiff_t>(n));
  }
  batch_.clear();
  batch_times_.clear();
  batch_t_first_ns_ = 0;
  return s;
}

Status D6PushbroomAssembler::resolve_(bool force) {
  if (poses_ == nullptr || !have_extrinsics_) {
    // Nothing to resolve against yet. Points stay pending (bounded) so a
    // capture that starts before the wizard's extrinsic is applied, or before
    // the first ARCore pose lands, is not silently lost.
    stats_.points_pending = pending_.size();
    return kOkStatus;
  }

  Status result = kOkStatus;
  while (!pending_.empty()) {
    const ProfilePoint p = pending_.front();
    // ROUND 10 item 36: the lidar clock and the pose clock are offset by a
    // constant transport delay. `pose_time_offset_ns` is added HERE and only
    // here, so the point's own stamp (and therefore the recorded stream, the
    // stats and the batch times) keeps meaning "when the bytes were dated" —
    // the correction is a property of the JOIN, not of either stream. See the
    // derivation over PushbroomConfig::pose_time_offset_ns.
    const std::int64_t t_pose_ns = p.t_mono_ns + cfg_.pose_time_offset_ns;
    const PoseSample s = poses_->sample_at(t_pose_ns);

    if (s.retryable() && !force) {
      // The queue is time-ordered, so if the OLDEST point is still in the
      // future every point behind it is too. Stop; the caller will drain
      // again once more poses have arrived.
      break;
    }

    pending_.pop_front();

    if (!s.has_pose) {
      // kBeforeFirst (unresolvable), or kFuture/kNoData at flush().
      ++stats_.dropped_no_pose;
      continue;
    }

    switch (s.gate) {
      case PoseGate::kTrackingLost: ++stats_.flagged_tracking_lost; break;
      case PoseGate::kStale: ++stats_.flagged_stale_pose; break;
      case PoseGate::kLowConfidence: ++stats_.flagged_low_confidence; break;
      default: break;
    }
    const bool flagged = s.flagged();
    if (flagged && cfg_.exclude_flagged) continue;

    // world_from_lidar = world_from_phone(t) · phone_from_lidar
    double world_from_phone[16];
    se3::mat4_from_quat_pos(s.pose.orientation, s.pose.position, world_from_phone);
    double world_from_lidar[16];
    se3::mat4_mul(world_from_phone, phone_from_lidar_, world_from_lidar);

    // ROUND 9 item 34: the ONE definition of the fan frame lives in
    // `drivers/d6/d6_fan.h`. It used to be spelled out here and again in
    // D6Driver, with neither saying which end of the sensor +z came out of —
    // which is how the cloud shipped mirrored. Do not inline it again.
    double p_lidar[3];
    d6::fan_point(static_cast<double>(p.angle_deg), static_cast<double>(p.range_m), p_lidar);
    double p_world[3];
    se3::mat4_apply(world_from_lidar, p_lidar, p_world);

    PointVertex v{};
    v.x = static_cast<float>(p_world[0]);
    v.y = static_cast<float>(p_world[1]);
    v.z = static_cast<float>(p_world[2]);
    // Same intensity/high-reflectivity convention D6Driver's live preview
    // uses, so a colour mode written against one works on the other.
    v.r = p.intensity;
    v.g = p.high_reflectivity != 0 ? static_cast<std::uint8_t>(255) : p.intensity;
    v.b = p.intensity;
    v.a = flagged ? cfg_.flagged_alpha : static_cast<std::uint8_t>(255);
    if (flagged) ++stats_.flagged_emitted;

    if (batch_.empty()) batch_t_first_ns_ = p.t_mono_ns;
    emit_(v);
    if (cfg_.out_point_times != nullptr) batch_times_.push_back(p.t_mono_ns);
    batch_t_ns_ = p.t_mono_ns;
    // ROUND 10 item 36: close the batch on COUNT or on POINT-TIME SPAN,
    // whichever comes first. On a D6 the span is what fires (1,453 points/s
    // means 4096 points is 2.8 s), and on a fast sensor the count still is.
    // Point time only — see the note over PushbroomConfig::max_batch_span_ns
    // for why a wall clock here would break replay == capture.
    const bool full = batch_.size() >= cfg_.batch_points;
    const bool aged = cfg_.max_batch_span_ns > 0 &&
                      (p.t_mono_ns - batch_t_first_ns_) >= cfg_.max_batch_span_ns;
    if (full || aged) {
      const Status st = flush_batch_(batch_t_ns_);
      if (!st.ok() && result.ok()) result = st;
    }
  }

  stats_.points_pending = pending_.size();
  if (force) {
    const Status st = flush_batch_(batch_t_ns_);
    if (!st.ok() && result.ok()) result = st;
  }
  return result;
}

}  // namespace scanengine
