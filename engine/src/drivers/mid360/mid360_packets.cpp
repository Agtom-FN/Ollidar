#include "scanengine/drivers/mid360/mid360_packets.h"

#include <cstring>

namespace scanengine {
namespace mid360 {
namespace {

// Range comparisons in mm², so the 200k pts/s path never calls sqrt.
inline std::int64_t range_sq_mm(const CartesianHigh& p) {
  const std::int64_t x = p.x, y = p.y, z = p.z;
  return x * x + y * y + z * z;
}

inline std::int64_t metres_to_mm_sq(float m) {
  const std::int64_t mm = static_cast<std::int64_t>(static_cast<double>(m) * 1000.0);
  return mm * mm;
}

}  // namespace

bool point_passes(const CartesianHigh& p, const PointFilterConfig& cfg, FilterStats* stats) {
  if (stats != nullptr) ++stats->seen;

  // No-return first: it is by far the most common rejection (35–68% of real
  // returns) and it is the cheapest test.
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

// --- LossTracker ----------------------------------------------------------

LossTracker::Step LossTracker::observe(std::uint16_t udp_cnt, std::uint32_t* lost_out) {
  if (lost_out != nullptr) *lost_out = 0;
  ++packets_;

  if (!have_prev_) {
    have_prev_ = true;
    prev_ = udp_cnt;
    return Step::kFirst;
  }

  // Deliberately unsigned 16-bit arithmetic: this is what makes the
  // free-running counter's wrap at 65535 → 0 a gap of 1 rather than a
  // 65,535-packet "loss" event.
  const std::uint16_t gap = static_cast<std::uint16_t>(udp_cnt - prev_);
  prev_ = udp_cnt;

  if (gap == 0) {
    ++duplicates_;
    return Step::kDuplicate;
  }
  if (gap == 1) return Step::kInSequence;
  if (gap < kResetThreshold) {
    const std::uint32_t lost = gap - 1u;
    lost_ += lost;
    if (lost_out != nullptr) *lost_out = lost;
    return Step::kLoss;
  }
  // A jump this large is a counter reset (the documented per-frame model, or
  // a device reboot, or a replay looping back to the top of a file). It is
  // NOT attributable to packet loss, and counting it as loss is precisely
  // the mistake the published protocol table invites.
  ++resets_;
  return Step::kUnattributable;
}

void LossTracker::reset() {
  have_prev_ = false;
  prev_ = 0;
  packets_ = 0;
  lost_ = 0;
  duplicates_ = 0;
  resets_ = 0;
}

double LossTracker::loss_fraction() const {
  const std::uint64_t total = packets_ + lost_;
  return total == 0 ? 0.0 : static_cast<double>(lost_) / static_cast<double>(total);
}

// --- parsing --------------------------------------------------------------

PacketView parse_packet(const std::uint8_t* data, std::size_t len) {
  PacketView v;
  if (data == nullptr || len < sizeof(DataHeader)) return v;

  DataHeader h{};
  std::memcpy(&h, data, sizeof(h));  // the datagram is not guaranteed aligned

  // `length` is the whole datagram. A device that disagrees with the socket
  // is either a firmware we do not understand or a truncated read; either
  // way the payload bounds below cannot be trusted.
  if (h.length != len) return v;

  const std::size_t payload_bytes = len - sizeof(DataHeader);
  std::size_t per_point = 0;
  switch (h.data_type) {
    case kDataTypeImu: per_point = sizeof(ImuRaw); break;
    case kDataTypeCartesianHigh: per_point = sizeof(CartesianHigh); break;
    case kDataTypeCartesianLow: per_point = sizeof(CartesianLow); break;
    default: return v;  // spherical / debug types we never request
  }
  if (h.dot_num == 0) return v;
  if (static_cast<std::size_t>(h.dot_num) * per_point > payload_bytes) return v;

  v.header = reinterpret_cast<const DataHeader*>(data);
  v.payload = data + sizeof(DataHeader);
  v.payload_bytes = payload_bytes;
  v.point_count = h.dot_num;
  return v;
}

std::uint32_t crc32_iso_hdlc(const std::uint8_t* data, std::size_t len) {
  // Bit-reflected CRC-32/ISO-HDLC (the zlib/Ethernet polynomial 0xEDB88320),
  // which is what the SDK's bundled FastCRC computes and therefore what the
  // device's header.crc32 field holds. Table built once, lazily; 1 KiB.
  static std::uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

bool crc32_ok(const PacketView& v) {
  if (!v.valid()) return false;
  // The device's CRC covers [timestamp .. end of payload] — i.e. everything
  // from offset 28 on, not the whole header (S2 cross-check: SDK source,
  // published table and a real recording all agree).
  const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(v.header) +
                             offsetof(DataHeader, timestamp);
  const std::size_t n = sizeof(std::uint64_t) + v.payload_bytes;
  std::uint32_t stored = 0;
  std::memcpy(&stored, reinterpret_cast<const std::uint8_t*>(v.header) + offsetof(DataHeader, crc32),
              sizeof(stored));
  return crc32_iso_hdlc(base, n) == stored;
}

}  // namespace mid360
}  // namespace scanengine
