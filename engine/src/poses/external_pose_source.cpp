#include "scanengine/poses/external_pose_source.h"

#include <cmath>
#include <utility>

#include "scanengine/poses/se3.h"

namespace scanengine {
namespace {
bool finite3(const double v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
}  // namespace

const char* to_string(PoseGate g) noexcept {
  switch (g) {
    case PoseGate::kOk: return "ok";
    case PoseGate::kNoData: return "no-data";
    case PoseGate::kBeforeFirst: return "before-first";
    case PoseGate::kFuture: return "future";
    case PoseGate::kStale: return "stale";
    case PoseGate::kTrackingLost: return "tracking-lost";
    case PoseGate::kLowConfidence: return "low-confidence";
  }
  return "?";
}

float pose_confidence(const Pose& p) noexcept {
  if (p.tracking_lost != 0) return 0.0f;
  switch (p.quality) {
    case PoseQuality::kInvalid: return 0.0f;
    case PoseQuality::kPoor: return 0.25f;
    case PoseQuality::kFair: return 0.60f;
    case PoseQuality::kGood: return 1.00f;
  }
  return 0.0f;
}

ExternalPoseSource::ExternalPoseSource(const ExternalPoseConfig& cfg) : cfg_(cfg) {
  if (cfg_.capacity < 2) cfg_.capacity = 2;
  ring_.resize(cfg_.capacity);
}

ExternalPoseSource::~ExternalPoseSource() = default;

Status ExternalPoseSource::start() {
  std::lock_guard<std::mutex> lock(m_);
  running_ = true;
  return kOkStatus;
}

Status ExternalPoseSource::stop() {
  std::lock_guard<std::mutex> lock(m_);
  running_ = false;
  return kOkStatus;
}

bool ExternalPoseSource::running() const {
  std::lock_guard<std::mutex> lock(m_);
  return running_;
}

void ExternalPoseSource::set_callback(PoseCallback cb) {
  std::lock_guard<std::mutex> lock(m_);
  cb_ = std::move(cb);
}

std::size_t ExternalPoseSource::size() const {
  std::lock_guard<std::mutex> lock(m_);
  return count_;
}

void ExternalPoseSource::clear() {
  std::lock_guard<std::mutex> lock(m_);
  head_ = 0;
  count_ = 0;
  stats_.held = 0;
}

const ExternalPoseSource::Entry& ExternalPoseSource::at_locked_(std::size_t i) const {
  return ring_[(head_ + i) % ring_.size()];
}

Status ExternalPoseSource::push_pose(const Pose& pose) {
  return push_pose(pose, pose_confidence(pose));
}

Status ExternalPoseSource::push_pose(const Pose& pose, float confidence) {
  Pose p = pose;

  // Validate before anything else: a NaN that reaches the ring poisons every
  // interpolation that brackets it, and the failure surfaces thousands of
  // points later as an empty cloud.
  if (!finite3(p.position) || !std::isfinite(p.orientation[0]) ||
      !std::isfinite(p.orientation[1]) || !std::isfinite(p.orientation[2]) ||
      !std::isfinite(p.orientation[3])) {
    std::lock_guard<std::mutex> lock(m_);
    ++stats_.rejected_invalid;
    return set_last_error(ScanError::kInvalidArgument,
                          "poses: non-finite pose pushed on stream %d",
                          static_cast<int>(cfg_.stream));
  }
  if (!se3::quat_normalize(p.orientation)) {
    std::lock_guard<std::mutex> lock(m_);
    ++stats_.rejected_invalid;
    return set_last_error(ScanError::kInvalidArgument,
                          "poses: zero-norm orientation quaternion on stream %d",
                          static_cast<int>(cfg_.stream));
  }

  // A4: map the pusher's clock into engine time. Identity for kPoseAr today
  // (ARCore is already CLOCK_BOOTTIME), meaningful the moment a pose stream
  // with its own clock appears. Done outside the lock — TimeSync has its own.
  if (cfg_.timesync != nullptr) {
    p.t_mono_ns = cfg_.timesync->to_engine_time(cfg_.stream, p.t_mono_ns);
  }
  if (p.source == StreamId::kUnknown) p.source = cfg_.stream;

  if (confidence < 0.0f) confidence = 0.0f;
  if (confidence > 1.0f) confidence = 1.0f;

  PoseCallback cb;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (count_ > 0) {
      const std::int64_t newest = at_locked_(count_ - 1).pose.t_mono_ns;
      if (p.t_mono_ns <= newest) {
        ++stats_.rejected_out_of_order;
        return set_last_error(
            ScanError::kInvalidArgument,
            "poses: out-of-order pose on stream %d (t=%lld <= newest=%lld)",
            static_cast<int>(cfg_.stream), static_cast<long long>(p.t_mono_ns),
            static_cast<long long>(newest));
      }
    }

    Entry e;
    e.pose = p;
    e.confidence = confidence;

    if (count_ == ring_.size()) {
      ring_[head_] = e;
      head_ = (head_ + 1) % ring_.size();
      ++stats_.overwritten;
    } else {
      ring_[(head_ + count_) % ring_.size()] = e;
      ++count_;
    }

    ++stats_.pushed;
    if (p.tracking_lost != 0) ++stats_.tracking_lost_poses;
    stats_.held = count_;
    stats_.t_first_ns = at_locked_(0).pose.t_mono_ns;
    stats_.t_last_ns = at_locked_(count_ - 1).pose.t_mono_ns;
    cb = cb_;
  }

  // Callbacks run on the pushing thread with NO lock held (DESIGN.md §2:
  // callbacks must not re-enter, and holding m_ here would make a subscriber
  // that calls sample_at() deadlock).
  if (cb) cb(p);
  return kOkStatus;
}

std::ptrdiff_t ExternalPoseSource::upper_index_locked_(std::int64_t t) const {
  if (count_ == 0) return -1;
  // Binary search for the last entry with pose.t_mono_ns <= t.
  std::size_t lo = 0, hi = count_;  // answer in [lo, hi)
  if (at_locked_(0).pose.t_mono_ns > t) return -1;
  while (hi - lo > 1) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (at_locked_(mid).pose.t_mono_ns <= t) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return static_cast<std::ptrdiff_t>(lo);
}

PoseSample ExternalPoseSource::sample_at(std::int64_t t) const {
  PoseSample out;
  std::lock_guard<std::mutex> lock(m_);
  ++stats_.queries;

  if (count_ == 0) {
    out.gate = PoseGate::kNoData;
    ++stats_.queries_gated;
    return out;
  }

  const std::int64_t t_first = at_locked_(0).pose.t_mono_ns;
  const std::int64_t t_last = at_locked_(count_ - 1).pose.t_mono_ns;

  if (t < t_first) {
    // Never resolvable: either the point predates the ARCore session, or the
    // pose that would have bracketed it has already rolled off the ring.
    out.gate = PoseGate::kBeforeFirst;
    ++stats_.queries_gated;
    return out;
  }

  const Entry* a = nullptr;
  const Entry* b = nullptr;
  double u = 0.0;

  if (t >= t_last) {
    if (t == t_last) {
      a = b = &at_locked_(count_ - 1);
    } else if (cfg_.max_extrapolation_ns > 0 && (t - t_last) <= cfg_.max_extrapolation_ns) {
      // HOLD the last pose, never project it forward. See the config comment.
      a = b = &at_locked_(count_ - 1);
      out.bracket_gap_ns = t - t_last;
    } else {
      out.gate = PoseGate::kFuture;
      ++stats_.queries_gated;
      return out;
    }
  } else {
    const std::ptrdiff_t i = upper_index_locked_(t);
    a = &at_locked_(static_cast<std::size_t>(i));
    b = &at_locked_(static_cast<std::size_t>(i) + 1);
    const std::int64_t gap = b->pose.t_mono_ns - a->pose.t_mono_ns;
    out.bracket_gap_ns = gap;
    u = (gap > 0) ? static_cast<double>(t - a->pose.t_mono_ns) / static_cast<double>(gap) : 0.0;
  }

  // Interpolate. Position lerps; orientation SLERPs on the shortest arc.
  Pose& p = out.pose;
  p.t_mono_ns = t;
  for (int i = 0; i < 3; ++i) {
    p.position[i] = a->pose.position[i] + (b->pose.position[i] - a->pose.position[i]) * u;
  }
  se3::quat_slerp(a->pose.orientation, b->pose.orientation, u, p.orientation);
  p.source = a->pose.source;
  // Every scalar takes the PESSIMISTIC of the two bracketing samples: an
  // interval is only as trustworthy as its worse end.
  p.position_sigma_m = a->pose.position_sigma_m > b->pose.position_sigma_m
                           ? a->pose.position_sigma_m
                           : b->pose.position_sigma_m;
  p.orientation_sigma_deg = a->pose.orientation_sigma_deg > b->pose.orientation_sigma_deg
                                ? a->pose.orientation_sigma_deg
                                : b->pose.orientation_sigma_deg;
  p.quality = (static_cast<std::uint8_t>(a->pose.quality) <
               static_cast<std::uint8_t>(b->pose.quality))
                  ? a->pose.quality
                  : b->pose.quality;
  p.tracking_lost = (a->pose.tracking_lost != 0 || b->pose.tracking_lost != 0) ? 1 : 0;
  out.confidence = a->confidence < b->confidence ? a->confidence : b->confidence;
  out.has_pose = true;

  // Gate, worst reason first.
  if (p.quality == PoseQuality::kInvalid) {
    out.gate = PoseGate::kLowConfidence;
  } else if (cfg_.gate_tracking_lost && p.tracking_lost != 0) {
    out.gate = PoseGate::kTrackingLost;
  } else if (out.bracket_gap_ns > cfg_.max_gap_ns) {
    out.gate = PoseGate::kStale;
  } else if (out.confidence < cfg_.min_confidence ||
             static_cast<std::uint8_t>(p.quality) <
                 static_cast<std::uint8_t>(cfg_.min_quality)) {
    out.gate = PoseGate::kLowConfidence;
  } else {
    out.gate = PoseGate::kOk;
  }
  if (out.gate != PoseGate::kOk) ++stats_.queries_gated;
  return out;
}

Status ExternalPoseSource::pose_at(std::int64_t t_mono_ns, Pose* out) const {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "poses: pose_at(out == null)");
  }
  const PoseSample s = sample_at(t_mono_ns);
  // The A1 contract (pose_source.h): kNotFound before the first pose, kAgain
  // when `t` is newer than the newest. A gated-but-present pose is still
  // returned — a caller using this narrow API has no way to act on the
  // distinction, and dropping the pose entirely would be worse.
  switch (s.gate) {
    case PoseGate::kNoData:
    case PoseGate::kBeforeFirst:
      return ScanError::kNotFound;
    case PoseGate::kFuture:
      return ScanError::kAgain;
    default:
      break;
  }
  *out = s.pose;
  return kOkStatus;
}

bool ExternalPoseSource::time_span(std::int64_t* first_ns, std::int64_t* last_ns) const {
  std::lock_guard<std::mutex> lock(m_);
  if (count_ == 0) return false;
  if (first_ns != nullptr) *first_ns = at_locked_(0).pose.t_mono_ns;
  if (last_ns != nullptr) *last_ns = at_locked_(count_ - 1).pose.t_mono_ns;
  return true;
}

ExternalPoseStats ExternalPoseSource::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  ExternalPoseStats s = stats_;
  s.held = count_;
  return s;
}

}  // namespace scanengine
