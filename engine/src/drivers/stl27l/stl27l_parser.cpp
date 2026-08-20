// LDROBOT STL-27L packet parser. Structural mirror of
// src/drivers/d6/d6_parser.cpp — see that file for the derivation of the
// per-sample timing model, which this file reproduces rather than reinvents.
//
// Owner: ITEM 119.
#include "scanengine/drivers/stl27l/stl27l_parser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace stl27l {
namespace {

inline uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Wrap a degree value into [0, 360).
inline float wrap_deg(float a) {
  while (a < 0.f) a += 360.f;
  while (a >= 360.f) a -= 360.f;
  return a;
}

// --- the CRC8 table --------------------------------------------------------
//
// GENERATED, not pasted. See crc8()'s doc comment in the header for the
// provenance argument: the LDROBOT SDK ships a 256-entry `CrcTable[]` constant
// and uses it as `crc = CrcTable[(crc ^ byte) & 0xff]`; that table is exactly
// the per-byte CRC of the MSB-first, poly-0x4D, init-0x00 register, which is
// what this builds at static-init time. Its first sixteen entries are
//   00 4d 9a d7 79 34 e3 ae f2 bf 68 25 8b c6 11 5c
// and tests/test_stl27l.cpp pins that prefix so a silent divergence from the
// vendor's published table is impossible.
struct Crc8Table {
  std::array<uint8_t, 256> t{};
  constexpr Crc8Table() {
    for (int i = 0; i < 256; ++i) {
      uint8_t c = static_cast<uint8_t>(i);
      for (int b = 0; b < 8; ++b) {
        c = static_cast<uint8_t>((c & 0x80) ? ((c << 1) ^ kCrcPoly) : (c << 1));
      }
      t[static_cast<size_t>(i)] = c;
    }
  }
};
constexpr Crc8Table kCrc8{};

}  // namespace

uint64_t now_ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t c = 0x00;  // init
  for (size_t i = 0; i < n; ++i) c = kCrc8.t[static_cast<uint8_t>(c ^ p[i])];
  return c;  // no final xor
}

uint32_t timestamp_delta_ms(uint16_t prev_ms, uint16_t now_ms) {
  if (now_ms >= prev_ms) return static_cast<uint32_t>(now_ms - prev_ms);
  // Rolled over the 30000 ms horizon. A device that reports a value at or
  // above the wrap (out of spec, but cheap to survive) still yields a
  // non-negative delta because the arithmetic is done in 32 bits.
  return static_cast<uint32_t>(kTimestampWrapMs + now_ms - prev_ms);
}

float point_angle_deg(float start_deg, float end_deg, size_t i, size_t n) {
  if (n <= 1) return wrap_deg(start_deg);
  float end = end_deg;
  // The packet spans the 0/360 seam: the end is a revolution later, not
  // earlier. Interpolating without this runs the 12 points BACKWARDS across
  // almost the whole circle, which is the single most likely way to get a
  // mirrored-looking scan out of a correct decoder.
  if (end < start_deg) end += 360.f;
  const float step = (end - start_deg) / static_cast<float>(n - 1);
  return wrap_deg(start_deg + step * static_cast<float>(i));
}

void decode_packet_unchecked(const uint8_t* p, Packet* out) {
  if (out == nullptr) return;
  out->speed_dps = le16(p + 2);
  out->start_angle_deg = static_cast<float>(le16(p + 4)) / 100.f;
  out->end_angle_deg = static_cast<float>(le16(p + 42)) / 100.f;
  out->timestamp_ms = le16(p + 44);
  for (size_t i = 0; i < kPointsPerPacket; ++i) {
    const uint8_t* s = p + 6 + i * 3;
    out->distance_mm[i] = le16(s);
    out->intensity[i] = s[2];
  }
}

bool decode_packet(const uint8_t* p, Packet* out) {
  if (p[0] != kHeaderByte || p[1] != kVerLen) return false;
  if (crc8(p, kCrcOffset) != p[kCrcOffset]) return false;
  decode_packet_unchecked(p, out);
  return true;
}

void Parser::reset() {
  stats_ = Stats{};
  buf_.clear();
  head_ = 0;
  queue_.clear();
  prev_sample_ns_ = 0.0;
  have_prev_ts_ = false;
  prev_timestamp_ms_ = 0;
  device_ns_ = 0.0;
  pending_new_rotation_ = false;
  prev_angle_deg_ = -1.f;
  in_garbage_run_ = false;
  rate_t0_ns_ = 0;
  rate_points0_ = 0;
  rate_rotations0_ = 0;
}

void Parser::drop_bytes(size_t n, bool garbage) {
  if (garbage) {
    stats_.bytes_discarded += n;
    if (!in_garbage_run_) {
      in_garbage_run_ = true;
      ++stats_.resyncs;
    }
  }
  head_ += n;
}

void Parser::compact() {
  if (head_ == 0) return;
  if (head_ == buf_.size()) {
    buf_.clear();
    head_ = 0;
    return;
  }
  if (head_ >= 4096 || head_ * 2 >= buf_.size()) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(head_));
    head_ = 0;
  }
}

void Parser::feed(const uint8_t* data, size_t n, uint64_t t_rx_ns) {
  if (t_rx_ns == 0) t_rx_ns = now_ns();
  if (rate_t0_ns_ == 0) rate_t0_ns_ = t_rx_ns;
  stats_.bytes_in += n;
  if (data != nullptr && n != 0) buf_.insert(buf_.end(), data, data + n);
  // Runaway guard, same as the D6's: drowning in bytes without finding a
  // packet throws away the oldest half rather than growing without bound.
  if (buf_.size() - head_ > cfg_.max_buffered_bytes) {
    const size_t drop = (buf_.size() - head_) / 2;
    stats_.bytes_discarded += drop;
    ++stats_.resyncs;
    head_ += drop;
  }
  parse(t_rx_ns);
  compact();
  update_rates(t_rx_ns);
}

void Parser::parse(uint64_t t_rx_ns) {
  for (;;) {
    // --- hunt for the 0x54 0x2C sync ---------------------------------------
    size_t garbage = 0;
    while (head_ + garbage < buf_.size()) {
      if (buf_[head_ + garbage] != kHeaderByte) { ++garbage; continue; }
      if (head_ + garbage + 1 >= buf_.size()) break;   // need the second byte
      if (buf_[head_ + garbage + 1] != kVerLen) {
        // A 0x54 whose successor is not 0x2C. It is still a header candidate
        // failure, not silent garbage: count it so a device speaking a
        // DIFFERENT LD variant (a 0x54 with another point count) shows up as
        // malformed packets rather than as an inexplicably quiet port.
        ++stats_.packets_malformed;
        ++garbage;
        continue;
      }
      break;
    }
    if (garbage) drop_bytes(garbage, true);
    if (avail() < 2) return;                                     // wait
    if (at()[0] != kHeaderByte || at()[1] != kVerLen) return;    // trailing lone 0x54
    if (avail() < kPacketBytes) return;                          // torn packet, wait

    const uint8_t* p = at();
    const uint8_t crc_field = p[kCrcOffset];
    const uint8_t crc_calc = crc8(p, kCrcOffset);

    in_garbage_run_ = false;  // aligned on something that looks like a header

    if (crc_calc != crc_field) {
      ++stats_.packets_bad_crc;
      if (cfg_.emit_bad_crc_points) {
        Packet bad{};
        decode_packet_unchecked(p, &bad);
        emit_packet(bad, t_rx_ns);
      }
      // See Config::consume_packet_on_bad_crc for why one byte is the default
      // here and a whole packet is the default on the D6.
      if (cfg_.consume_packet_on_bad_crc) {
        head_ += kPacketBytes;
      } else {
        drop_bytes(1, true);
      }
      continue;
    }

    Packet pkt{};
    decode_packet_unchecked(p, &pkt);
    ++stats_.packets_ok;
    emit_packet(pkt, t_rx_ns);
    head_ += kPacketBytes;
  }
}

void Parser::emit_packet(const Packet& pkt, uint64_t t_rx_ns) {
  constexpr size_t kN = kPointsPerPacket;
  stats_.speed_dps = pkt.speed_dps;

  // --- the device clock ------------------------------------------------------
  //
  // `timestamp` dates the packet, and the LAST of its twelve returns is the
  // one that sits on it (transmission starts once the packet is full). The
  // eleven earlier returns are interpolated back towards the PREVIOUS
  // packet's timestamp, which is the packet's implied start time. Nothing
  // downstream may place geometry with this — it is the device's clock, not
  // the host's — but it is the only direct measurement of how long a packet's
  // returns actually took, so it is carried and it is what
  // Config::use_device_timestamp_spacing feeds when that is on.
  double device_prev_ns = 0.0;
  double device_now_ns = 0.0;
  bool have_device_span = false;
  double device_sample_ns = 0.0;
  if (have_prev_ts_) {
    const uint32_t dt_ms = timestamp_delta_ms(prev_timestamp_ms_, pkt.timestamp_ms);
    if (pkt.timestamp_ms < prev_timestamp_ms_) ++stats_.timestamp_wraps;
    device_prev_ns = device_ns_;
    device_now_ns = device_ns_ + static_cast<double>(dt_ms) * 1e6;
    have_device_span = true;
    device_sample_ns = (device_now_ns - device_prev_ns) / static_cast<double>(kN);
    device_ns_ = device_now_ns;
  } else {
    device_ns_ = static_cast<double>(pkt.timestamp_ms) * 1e6;
  }
  prev_timestamp_ms_ = pkt.timestamp_ms;
  have_prev_ts_ = true;

  // --- when was each return actually TAKEN, in HOST time? --------------------
  //
  // d6_parser.cpp derives this model in full and this is the same model with
  // the D6's numbers swapped for the STL-27L's. Briefly: the device buffers a
  // packet and blasts it at the line rate, so spacing returns at the WIRE rate
  // compresses them; space them at the SAMPLING rate instead, anchored on the
  // packet's first byte, which is the moment just after its last return.
  double t_last_sample = 0.0;
  double sample_ns = 0.0;
  const bool stamp_samples = cfg_.per_sample_timestamps && t_rx_ns != 0 &&
                             cfg_.wire_bytes_per_sec > 0.0 && cfg_.nominal_sample_hz > 0.0;
  if (stamp_samples) {
    const double byte_ns = 1e9 / cfg_.wire_bytes_per_sec;

    // (3) The wire-rate model locating this packet inside the read burst.
    // Bytes still buffered behind it arrived after its last byte did.
    const size_t bytes_after = avail() - kPacketBytes;
    const double t_last_byte =
        static_cast<double>(t_rx_ns) - static_cast<double>(bytes_after) * byte_ns;

    // (2) The anchor.
    t_last_sample = t_last_byte - static_cast<double>(kPacketBytes) * byte_ns;

    // (1) The spacing, best source first.
    sample_ns = 1e9 / cfg_.nominal_sample_hz;
    double hz = 0.0;
    if (cfg_.use_device_timestamp_spacing && have_device_span && device_sample_ns > 0.0) {
      hz = 1e9 / device_sample_ns;
    } else if (pkt.speed_dps > 0) {
      // The exact, quantisation-free derivation: this packet's own angular
      // step against this packet's own reported spin rate. Direct analogue of
      // the D6's interval64-vs-scan_freq_raw cross-check.
      float end = pkt.end_angle_deg;
      if (end < pkt.start_angle_deg) end += 360.f;
      const double span = static_cast<double>(end - pkt.start_angle_deg);
      const double deg_per_point = span / static_cast<double>(kN - 1);
      if (deg_per_point > 1e-6) hz = static_cast<double>(pkt.speed_dps) / deg_per_point;
    }
    if (hz > 0.0) {
      stats_.sample_hz_est = hz;
      // The datasheet cross-check. Inside the band, trust the device; outside
      // it this is a corrupt span or a stalled clock rather than a slow motor,
      // so fall back to nominal and say so.
      const double rel = std::fabs(hz - cfg_.nominal_sample_hz) / cfg_.nominal_sample_hz;
      if (rel <= cfg_.sample_rate_tolerance) {
        sample_ns = 1e9 / hz;
      } else {
        ++stats_.sample_rate_warnings;
      }
    }

    // The min-delay sample clock (d6_parser.cpp derives why the wire anchor is
    // biased LATE and why taking the earlier of {propagated, anchor} removes
    // almost all of that bias, and why it subsumes the monotonicity clamp).
    if (cfg_.sample_clock_anchor && prev_sample_ns_ != 0.0) {
      const double propagated = prev_sample_ns_ + static_cast<double>(kN) * sample_ns;
      if (t_last_sample - propagated > cfg_.sample_clock_resync_ns) {
        ++stats_.sample_clock_resyncs;  // chain broken: re-seed from the anchor
      } else {
        t_last_sample = std::min(t_last_sample, propagated);
      }
    }

    // Monotonicity across the packet boundary: the window is (N-1) periods
    // wide and must start no earlier than the previous packet's final return.
    // When the anchor leaves less room, hold the anchor and COMPRESS, which
    // degrades to wire-rate spacing rather than lying about it.
    if (prev_sample_ns_ != 0.0) {
      const double room = t_last_sample - prev_sample_ns_;
      const double want = static_cast<double>(kN - 1) * sample_ns;
      if (room < want) {
        sample_ns = room > 0.0 ? room / static_cast<double>(kN - 1) : 0.0;
        if (room <= 0.0) t_last_sample = prev_sample_ns_;
      }
    }
    prev_sample_ns_ = t_last_sample;
  }

  for (size_t i = 0; i < kN; ++i) {
    Point pt;
    pt.distance_mm = pkt.distance_mm[i];
    pt.intensity = pkt.intensity[i];
    pt.t_rx_ns = t_rx_ns;
    pt.angle_deg = point_angle_deg(pkt.start_angle_deg, pkt.end_angle_deg, i, kN);

    if (stamp_samples) {
      // Point kN-1 sits ON the anchor; earlier ones are back-dated at the
      // sampling period. Same convention as d6_parser.cpp — see
      // Config::per_sample_timestamps for why it must stay the same.
      const double t = t_last_sample - static_cast<double>(kN - 1 - i) * sample_ns;
      pt.t_sample_ns = t > 0.0 ? static_cast<uint64_t>(t + 0.5) : 0;
    }
    if (have_device_span) {
      const double td = device_prev_ns +
                        (device_now_ns - device_prev_ns) * static_cast<double>(i + 1) /
                            static_cast<double>(kN);
      pt.t_device_ns = td > 0.0 ? static_cast<uint64_t>(td + 0.5) : 0;
    }

    // Rotation detection. The LD protocol has no start-of-revolution packet
    // (the D6 does), so the 360 -> 0 angle wrap is the only signal there is.
    if (prev_angle_deg_ >= 0.f && prev_angle_deg_ > 270.f && pt.angle_deg < 90.f) {
      pending_new_rotation_ = true;
      ++stats_.rotations;
    }
    prev_angle_deg_ = pt.angle_deg;

    pt.new_rotation = pending_new_rotation_;
    pending_new_rotation_ = false;

    if (pt.distance_mm == 0) {
      ++stats_.points_zero_range;
      if (cfg_.drop_zero_range) {
        // Keep the rotation marker alive for the next emitted point, so a
        // revolution boundary that lands on a no-return is not lost.
        if (pt.new_rotation) pending_new_rotation_ = true;
        continue;
      }
    }
    ++stats_.points;
    emit(pt);
  }
  if (on_packet_) on_packet_(pkt);
}

void Parser::emit(const Point& pt) {
  if (on_point_) {
    on_point_(pt);
  } else {
    queue_.push_back(pt);
  }
}

void Parser::take_points(std::vector<Point>* out) {
  if (!out) { queue_.clear(); return; }
  if (out->empty()) {
    out->swap(queue_);
    queue_.clear();
  } else {
    out->insert(out->end(), queue_.begin(), queue_.end());
    queue_.clear();
  }
}

void Parser::update_rates(uint64_t t_ns) {
  if (rate_t0_ns_ == 0) { rate_t0_ns_ = t_ns; return; }
  const uint64_t dt = t_ns - rate_t0_ns_;
  if (dt < 1000000000ull) return;
  const double secs = static_cast<double>(dt) / 1e9;
  stats_.points_per_sec = static_cast<double>(stats_.points - rate_points0_) / secs;
  stats_.rotation_hz = static_cast<double>(stats_.rotations - rate_rotations0_) / secs;
  rate_t0_ns_ = t_ns;
  rate_points0_ = stats_.points;
  rate_rotations0_ = stats_.rotations;
}

std::string Parser::stats_line() const {
  char b[320];
  std::snprintf(b, sizeof(b),
                "pkt ok=%llu bad_crc=%llu malformed=%llu resync=%llu | "
                "crc_pass=%.2f%% | pts=%llu %.0f pts/s | rot=%llu %.2f Hz | "
                "spin=%u deg/s | garbage=%llu",
                (unsigned long long)stats_.packets_ok,
                (unsigned long long)stats_.packets_bad_crc,
                (unsigned long long)stats_.packets_malformed,
                (unsigned long long)stats_.resyncs, stats_.crc_pass_rate() * 100.0,
                (unsigned long long)stats_.points, stats_.points_per_sec,
                (unsigned long long)stats_.rotations, stats_.rotation_hz,
                (unsigned)stats_.speed_dps, (unsigned long long)stats_.bytes_discarded);
  return std::string(b);
}

}  // namespace stl27l
