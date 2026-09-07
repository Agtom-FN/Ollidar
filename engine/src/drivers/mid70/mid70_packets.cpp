#include "scanengine/drivers/mid70/mid70_packets.h"

#include <cmath>
#include <cstring>
#include <ctime>

namespace scanengine {
namespace mid70 {
namespace {

inline std::int64_t range_sq_mm(const Point& p) {
  const std::int64_t x = p.x, y = p.y, z = p.z;
  return x * x + y * y + z * z;
}

inline std::int64_t metres_to_mm_sq(float m) {
  const std::int64_t mm = static_cast<std::int64_t>(static_cast<double>(m) * 1000.0);
  return mm * mm;
}

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

}  // namespace

const char* to_string_timestamp_type(std::uint8_t t) noexcept {
  switch (t) {
    case kTimestampNoSync: return "nosync";
    case kTimestampPtp: return "ptp";
    case kTimestampRsvd: return "reserved";
    case kTimestampPpsGps: return "pps+gps";
    case kTimestampPps: return "pps";
    case kTimestampUnknown: return "unknown";
  }
  return "?";
}

const char* to_string_time_sync_status(std::uint8_t s) noexcept {
  switch (s) {
    case 0: return "not-started";
    case 1: return "ptp";
    case 2: return "gps";
    case 3: return "pps";
    case 4: return "abnormal";
  }
  return "?";
}

ErrorCode decode_error_code(std::uint32_t e) noexcept {
  // Bit layout from [S] LidarErrorCode, LSB first:
  //   temp 2 | volt 2 | motor 2 | dirty 2 | fw 1 | pps 1 | life 1 | fan 1 |
  //   self_heat 1 | ptp 1 | time_sync 3 | rsvd 13 | system 2
  ErrorCode c{};
  c.temp_status = static_cast<std::uint8_t>(e & 0x3u);
  c.volt_status = static_cast<std::uint8_t>((e >> 2) & 0x3u);
  c.motor_status = static_cast<std::uint8_t>((e >> 4) & 0x3u);
  c.dirty_warn = static_cast<std::uint8_t>((e >> 6) & 0x3u);
  c.firmware_err = ((e >> 8) & 0x1u) != 0;
  c.pps_ok = ((e >> 9) & 0x1u) != 0;
  c.device_lifetime_warn = ((e >> 10) & 0x1u) != 0;
  c.fan_warn = ((e >> 11) & 0x1u) != 0;
  c.self_heating = ((e >> 12) & 0x1u) != 0;
  c.ptp_ok = ((e >> 13) & 0x1u) != 0;
  c.time_sync_status = static_cast<std::uint8_t>((e >> 14) & 0x7u);
  c.system_status = static_cast<std::uint8_t>((e >> 30) & 0x3u);
  return c;
}

// --- filter ---------------------------------------------------------------

bool point_passes(const Point& p, const PointFilterConfig& cfg, FilterStats* stats) {
  if (stats != nullptr) ++stats->seen;

  if (cfg.drop_no_return && p.x == 0 && p.y == 0 && p.z == 0) {
    if (stats != nullptr) ++stats->dropped_no_return;
    return false;
  }
  if (cfg.tag_reject_mask != 0 && (p.tag & cfg.tag_reject_mask) != 0) {
    if (stats != nullptr) ++stats->dropped_tag;
    return false;
  }
  if (p.reflectivity < cfg.min_reflectivity) {
    if (stats != nullptr) ++stats->dropped_reflectivity;
    return false;
  }
  if (cfg.min_range_m > 0.f || cfg.max_range_m > 0.f) {
    const std::int64_t r2 = range_sq_mm(p);
    if (cfg.min_range_m > 0.f && r2 < metres_to_mm_sq(cfg.min_range_m)) {
      if (stats != nullptr) ++stats->dropped_range;
      return false;
    }
    if (cfg.max_range_m > 0.f && r2 > metres_to_mm_sq(cfg.max_range_m)) {
      if (stats != nullptr) ++stats->dropped_range;
      return false;
    }
  }
  if (stats != nullptr) ++stats->kept;
  return true;
}

Point from_spherical(std::uint32_t depth_mm, std::uint16_t theta_001deg,
                     std::uint16_t phi_001deg, std::uint8_t reflectivity, std::uint8_t tag) {
  Point p;
  const double r = static_cast<double>(depth_mm);
  const double th = static_cast<double>(theta_001deg) * 0.01 * kDegToRad;
  const double ph = static_cast<double>(phi_001deg) * 0.01 * kDegToRad;
  const double st = std::sin(th);
  p.x = static_cast<std::int32_t>(std::lround(r * st * std::cos(ph)));
  p.y = static_cast<std::int32_t>(std::lround(r * st * std::sin(ph)));
  p.z = static_cast<std::int32_t>(std::lround(r * std::cos(th)));
  p.reflectivity = reflectivity;
  p.tag = tag;
  return p;
}

// --- parsing --------------------------------------------------------------

PacketView parse_packet(const std::uint8_t* data, std::size_t len) {
  PacketView v;
  if (data == nullptr || len < sizeof(EthHeader)) return v;

  EthHeader h{};
  std::memcpy(&h, data, sizeof(h));

  std::size_t per_point = 0;
  std::uint32_t max_points = 0;
  switch (h.data_type) {
    case kDataTypeCartesian: per_point = sizeof(RawPoint); max_points = kPointsPerPacketCartesian; break;
    case kDataTypeSpherical: per_point = sizeof(SpherPoint); max_points = kPointsPerPacketCartesian; break;
    case kDataTypeExtendCartesian: per_point = sizeof(ExtendRawPoint); max_points = kPointsPerPacketExtend; break;
    case kDataTypeExtendSpherical: per_point = sizeof(ExtendSpherPoint); max_points = kPointsPerPacketExtend; break;
    case kDataTypeDualExtendCartesian: per_point = sizeof(DualExtendRawPoint); max_points = kPointsPerPacketDual; break;
    case kDataTypeImu: per_point = sizeof(ImuPoint); max_points = 1; break;
    default: return v;  // dual-spherical / triple types we never request
  }

  const std::size_t payload_bytes = len - sizeof(EthHeader);
  if (payload_bytes == 0) return v;
  // No length field: a whole number of points is the only structural check
  // available, so it is applied strictly. A datagram cut short by a socket
  // buffer would fail it; a datagram of a type the firmware sized
  // differently would too, which is the correct outcome for a layout this
  // file does not know.
  if (payload_bytes % per_point != 0) return v;
  const std::size_t n = payload_bytes / per_point;
  if (n == 0 || n > max_points) return v;

  v.header = reinterpret_cast<const EthHeader*>(data);
  v.payload = data + sizeof(EthHeader);
  v.payload_bytes = payload_bytes;
  v.point_count = static_cast<std::uint32_t>(n);
  v.per_point_bytes = static_cast<std::uint32_t>(per_point);
  return v;
}

bool decode_timestamp_ns(const EthHeader& h, std::int64_t* out_ns) noexcept {
  if (out_ns == nullptr) return false;
  switch (h.timestamp_type) {
    case kTimestampNoSync:
    case kTimestampPtp:
    case kTimestampPps: {
      std::uint64_t raw = 0;
      std::memcpy(&raw, h.timestamp, sizeof(raw));  // little-endian u64 ns [S]
      *out_ns = static_cast<std::int64_t>(raw);
      return true;
    }
    case kTimestampPpsGps: {
      // [S] LivoxTimestampPpsGps: year (since 2000), month, day, hour, then
      // u32 microseconds within that hour. The ROS driver used timegm here
      // and that is the only correct choice: the field is UTC.
      std::tm t{};
      t.tm_year = static_cast<int>(h.timestamp[0]) + 100;  // 2000 + x, tm_year is since 1900
      t.tm_mon = static_cast<int>(h.timestamp[1]) - 1;
      t.tm_mday = static_cast<int>(h.timestamp[2]);
      t.tm_hour = static_cast<int>(h.timestamp[3]);
      t.tm_min = 0;
      t.tm_sec = 0;
      t.tm_isdst = 0;
      if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31 || t.tm_hour > 23) {
        return false;
      }
      std::uint32_t us_in_hour = 0;
      std::memcpy(&us_in_hour, h.timestamp + 4, sizeof(us_in_hour));
      const std::time_t epoch_s = timegm(&t);
      if (epoch_s == static_cast<std::time_t>(-1)) return false;
      *out_ns = (static_cast<std::int64_t>(epoch_s) * 1000000LL + us_in_hour) * 1000LL;
      return true;
    }
    default:
      return false;
  }
}

// --- GapTracker -----------------------------------------------------------

GapTracker::GapTracker(std::int64_t interval_ns, std::int64_t reset_threshold_ns)
    : interval_ns_(interval_ns > 0 ? interval_ns : 1), reset_threshold_ns_(reset_threshold_ns) {}

GapTracker::Step GapTracker::observe(std::int64_t t, std::uint32_t* lost_out) {
  if (lost_out != nullptr) *lost_out = 0;
  ++packets_;
  if (!have_prev_) {
    have_prev_ = true;
    prev_ = t;
    return Step::kFirst;
  }
  const std::int64_t gap = t - prev_;
  prev_ = t;

  if (gap <= 0) {
    // Same stamp, or the clock went backwards: a retransmit, or the device
    // clock reset to zero (power-cycle in NoSync mode, or a PPS edge in
    // PPS-only mode resetting the counter — [M] §5.2.2).
    ++duplicates_;
    return Step::kDuplicate;
  }
  if (gap >= reset_threshold_ns_) {
    ++resets_;
    return Step::kUnattributable;
  }
  // Round to the nearest whole interval; anything under 1.5 intervals is
  // in sequence (the device's own jitter is well under half an interval).
  const double k = static_cast<double>(gap) / static_cast<double>(interval_ns_);
  if (k < 1.5) return Step::kInSequence;
  const std::uint32_t lost = static_cast<std::uint32_t>(std::lround(k)) - 1u;
  lost_ += lost;
  if (lost_out != nullptr) *lost_out = lost;
  return Step::kLoss;
}

void GapTracker::reset() {
  have_prev_ = false;
  prev_ = 0;
  packets_ = 0;
  lost_ = 0;
  duplicates_ = 0;
  resets_ = 0;
}

double GapTracker::loss_fraction() const {
  const std::uint64_t total = packets_ + lost_;
  return total == 0 ? 0.0 : static_cast<double>(lost_) / static_cast<double>(total);
}

}  // namespace mid70
}  // namespace scanengine
