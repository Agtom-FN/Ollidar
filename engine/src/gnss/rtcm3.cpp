#include "scanengine/gnss/rtcm3.h"

#include <cstring>

namespace scanengine {
namespace rtcm3 {
namespace {

// 256-entry CRC-24Q table, built once at first use. A bitwise loop costs
// ~8 shifts per byte; a caster at 5 kB/s is nothing either way, but the
// framer also re-CRCs every candidate frame while resyncing through garbage,
// and that is the case worth being cheap in.
struct Crc24Table {
  std::uint32_t t[256];
  Crc24Table() {
    for (int i = 0; i < 256; ++i) {
      std::uint32_t crc = static_cast<std::uint32_t>(i) << 16;
      for (int b = 0; b < 8; ++b) {
        crc <<= 1;
        if (crc & 0x01000000u) crc ^= kCrc24qPoly;
      }
      t[i] = crc & 0x00FFFFFFu;
    }
  }
};

const Crc24Table& crc_table() {
  static const Crc24Table table;
  return table;
}

}  // namespace

std::uint32_t crc24q(const std::uint8_t* data, std::size_t n, std::uint32_t crc) noexcept {
  const Crc24Table& tb = crc_table();
  crc &= 0x00FFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t idx = static_cast<std::uint8_t>((crc >> 16) ^ data[i]);
    crc = ((crc << 8) ^ tb.t[idx]) & 0x00FFFFFFu;
  }
  return crc;
}

bool validate_frame(const std::uint8_t* data, std::size_t n, FrameInfo* out) noexcept {
  if (out) *out = FrameInfo{};
  if (data == nullptr || n < kHeaderBytes) return false;
  if (data[0] != kPreamble) return false;
  // The 6 bits above the 10-bit length are reserved and must be zero. A
  // non-zero value is the single cheapest way to reject a 0xD3 that happened
  // to appear inside a payload, before paying for a CRC.
  if ((data[1] & 0xFCu) != 0) return false;
  const std::size_t len = (static_cast<std::size_t>(data[1] & 0x03u) << 8) | data[2];
  const std::size_t total = kHeaderBytes + len + kCrcBytes;
  if (n < total) return false;  // need more bytes — not a failure

  const std::uint32_t computed = crc24q(data, kHeaderBytes + len);
  const std::uint32_t received = (static_cast<std::uint32_t>(data[kHeaderBytes + len]) << 16) |
                                 (static_cast<std::uint32_t>(data[kHeaderBytes + len + 1]) << 8) |
                                 static_cast<std::uint32_t>(data[kHeaderBytes + len + 2]);
  if (out) {
    out->offset = 0;
    out->payload_len = len;
    out->total_len = total;
    out->crc_ok = (computed == received);
    if (len >= 2) {
      // DF002: the first 12 bits of the payload.
      out->message_type = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(data[kHeaderBytes]) << 4) | (data[kHeaderBytes + 1] >> 4));
    }
  }
  return true;
}

std::size_t build_frame(const std::uint8_t* payload, std::size_t payload_len,
                        std::uint8_t* out) noexcept {
  if (out == nullptr || payload_len > kMaxPayloadBytes) return 0;
  if (payload_len > 0 && payload == nullptr) return 0;
  out[0] = kPreamble;
  out[1] = static_cast<std::uint8_t>((payload_len >> 8) & 0x03u);
  out[2] = static_cast<std::uint8_t>(payload_len & 0xFFu);
  if (payload_len > 0) std::memcpy(out + kHeaderBytes, payload, payload_len);
  const std::uint32_t crc = crc24q(out, kHeaderBytes + payload_len);
  out[kHeaderBytes + payload_len] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
  out[kHeaderBytes + payload_len + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
  out[kHeaderBytes + payload_len + 2] = static_cast<std::uint8_t>(crc & 0xFF);
  return kHeaderBytes + payload_len + kCrcBytes;
}

Rtcm3Framer::Rtcm3Framer() { buf_.reserve(4 * kMaxFrameBytes); }

void Rtcm3Framer::reset() {
  buf_.clear();
  stats_ = Rtcm3Stats{};
}

void Rtcm3Framer::clear_buffer() { buf_.clear(); }

void Rtcm3Framer::note_type_(std::uint16_t type) {
  stats_.last_message_type = type;
  for (std::size_t i = 0; i < stats_.type_slots_used; ++i) {
    if (stats_.types[i].type == type) {
      ++stats_.types[i].count;
      return;
    }
  }
  if (stats_.type_slots_used < kMessageTypeSlots) {
    stats_.types[stats_.type_slots_used].type = type;
    stats_.types[stats_.type_slots_used].count = 1;
    ++stats_.type_slots_used;
  } else {
    ++stats_.types_overflow;
  }
}

void Rtcm3Framer::push(ByteSpan bytes, std::int64_t t_ns) {
  stats_.bytes_in += bytes.size();
  buf_.insert(buf_.end(), bytes.begin(), bytes.end());

  std::size_t pos = 0;
  while (pos < buf_.size()) {
    if (buf_[pos] != kPreamble) {
      ++pos;
      ++stats_.resync_bytes;
      continue;
    }
    FrameInfo info;
    if (!validate_frame(buf_.data() + pos, buf_.size() - pos, &info)) {
      // Either not enough bytes yet, or the reserved bits are non-zero. The
      // first case must WAIT; the second must resync. `validate_frame`
      // returns false for both, so distinguish by whether a plausible header
      // is even complete.
      if (buf_.size() - pos >= kHeaderBytes && (buf_[pos + 1] & 0xFCu) != 0) {
        ++pos;
        ++stats_.resync_bytes;
        continue;
      }
      break;  // need more bytes
    }
    const ByteSpan frame(buf_.data() + pos, info.total_len);
    if (info.crc_ok) {
      ++stats_.frames_ok;
      stats_.payload_bytes += info.payload_len;
      if (stats_.frames_ok == 1) stats_.t_first_frame_ns = t_ns;
      stats_.t_last_frame_ns = t_ns;
      note_type_(info.message_type);
      if (handler_) handler_(frame, info, t_ns);
      pos += info.total_len;
    } else {
      ++stats_.frames_crc_failed;
      if (bad_) bad_(frame, info, t_ns);
      // Resync by ONE byte, not by the declared length: a corrupt length
      // field would otherwise skip over the next good frame. This is the
      // same rule the S5 spike's iter_frames applies.
      ++pos;
      ++stats_.resync_bytes;
    }
  }

  if (pos > 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
  // A stream that never yields a frame must not grow without bound. Anything
  // longer than one maximum frame cannot be the prefix of a valid frame.
  if (buf_.size() > kMaxFrameBytes) {
    const std::size_t drop = buf_.size() - kMaxFrameBytes;
    stats_.resync_bytes += drop;
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
  }
}

double Rtcm3Framer::age_s(std::int64_t now_ns) const {
  if (stats_.frames_ok == 0) return -1.0;
  const std::int64_t d = now_ns - stats_.t_last_frame_ns;
  return d > 0 ? static_cast<double>(d) * 1e-9 : 0.0;
}

}  // namespace rtcm3
}  // namespace scanengine
