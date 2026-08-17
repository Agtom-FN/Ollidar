// Copied unmodified from spikes/s1-d6-parser/d6/d6_parser.cpp (the finished
// S1 artefact) except for this include path. The spike tree stays in place
// as the historical record; this is the shipping copy. Owner: A2.
#include "scanengine/drivers/d6/d6_parser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace d6 {
namespace {

inline uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Wrap a 1/64-degree angle into [0, 23040).
inline float wrap_deg64(float a) {
  while (a < 0.f) a += static_cast<float>(kDeg64Full);
  while (a >= static_cast<float>(kDeg64Full)) a -= static_cast<float>(kDeg64Full);
  return a;
}

}  // namespace

uint64_t now_ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// The XOR is over 16-bit words and is order independent, so only the *grouping*
// of bytes into words matters. Both readings share the four header words and
// differ only in how the 3-byte samples are grouped.
uint16_t checksum(const uint8_t* p, uint8_t lsn, ChecksumVariant v) {
  uint16_t cs = kPhWord;              // PH taken as one 16-bit word
  cs ^= le16(p + 2);                  // (LSN << 8) | M&T
  cs ^= le16(p + 4);                  // FSA, raw (check bit included)
  cs ^= le16(p + 6);                  // LSA, raw
  const uint8_t* s = p + kHeaderBytes;
  if (v == ChecksumVariant::kVendorSdk) {
    for (uint8_t i = 0; i < lsn; ++i, s += kSampleBytes) {
      cs ^= static_cast<uint16_t>(s[0]);                   // Si_L, zero extended
      cs ^= static_cast<uint16_t>(s[1] | (s[2] << 8));     // Si_H:Si_2nd
    }
  } else {
    for (uint8_t i = 0; i < lsn; ++i, s += kSampleBytes) {
      cs ^= static_cast<uint16_t>(s[0] | (s[1] << 8));     // Si_2nd:Si_L
      cs ^= static_cast<uint16_t>(s[2]);                   // Si_H, zero extended
    }
  }
  return cs;
}

void Parser::reset() {
  stats_ = Stats{};
  buf_.clear();
  head_ = 0;
  queue_.clear();
  last_interval_deg64_ = 0.f;
  prev_sample_ns_ = 0.0;
  saw_start_packet_ = false;
  pending_new_rotation_ = false;
  prev_angle_deg_ = -1.f;
  in_garbage_run_ = false;
  rate_t0_ns_ = 0;
  rate_points0_ = 0;
  rate_rotations0_ = 0;
}

void Parser::drop_bytes(size_t n, bool garbage) {
  if (garbage) {
    for (size_t i = 0; i < n; ++i) {
      const uint8_t b = buf_[head_ + i];
      if (b == kSpeedAdjA || b == kSpeedAdjB) {
        ++stats_.speed_adjust_bytes;
      } else {
        ++stats_.bytes_discarded;
      }
    }
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
  // Only shuffle when the dead prefix is worth reclaiming.
  if (head_ >= 4096 || head_ * 2 >= buf_.size()) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(head_));
    head_ = 0;
  }
}

void Parser::feed(const uint8_t* data, size_t n, uint64_t t_rx_ns) {
  if (t_rx_ns == 0) t_rx_ns = now_ns();
  if (rate_t0_ns_ == 0) rate_t0_ns_ = t_rx_ns;
  stats_.bytes_in += n;
  buf_.insert(buf_.end(), data, data + n);
  // Runaway guard: if we are drowning in bytes without finding a packet,
  // throw away the oldest half of the buffer.
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
    // --- hunt for the 0x55AA header (bytes AA 55) --------------------------
    size_t garbage = 0;
    while (head_ + garbage < buf_.size()) {
      const uint8_t b0 = buf_[head_ + garbage];
      if (b0 != kPh0) { ++garbage; continue; }
      if (head_ + garbage + 1 >= buf_.size()) break;   // need the second byte
      if (buf_[head_ + garbage + 1] != kPh1) { ++garbage; continue; }
      break;
    }
    if (garbage) drop_bytes(garbage, true);
    if (avail() < 2) return;                            // wait for more bytes
    if (at()[0] != kPh0 || at()[1] != kPh1) return;     // trailing lone 0xAA

    if (avail() < kHeaderBytes) return;                 // torn header

    const uint8_t* p   = at();
    const uint8_t  mt  = p[2];
    const uint8_t  lsn = p[3];
    const bool is_start = (mt & 0x01) != 0;

    // --- header sanity -----------------------------------------------------
    bool malformed = (lsn == 0);
    if (!malformed && cfg_.require_angle_check_bit) {
      if ((p[4] & kAngleCheckBit) == 0 || (p[6] & kAngleCheckBit) == 0) malformed = true;
    }
    if (malformed) {
      ++stats_.packets_malformed;
      drop_bytes(1, true);   // false header: resume the hunt one byte later
      continue;
    }

    const size_t total = kHeaderBytes + static_cast<size_t>(lsn) * kSampleBytes;
    if (avail() < total) return;                        // torn body, wait

    // --- checksum ----------------------------------------------------------
    const uint16_t cs_field  = le16(p + 8);
    const uint16_t cs_vendor = checksum(p, lsn, ChecksumVariant::kVendorSdk);
    const uint16_t cs_spec   = checksum(p, lsn, ChecksumVariant::kSpecLiteral);
    if (cs_vendor == cs_field) ++stats_.cs_ok_vendor;
    if (cs_spec == cs_field) ++stats_.cs_ok_spec;
    const uint16_t cs_active =
        (cfg_.checksum == ChecksumVariant::kVendorSdk) ? cs_vendor : cs_spec;

    in_garbage_run_ = false;  // we are aligned again

    if (cs_active != cs_field) {
      ++stats_.packets_bad_checksum;
      if (cfg_.consume_packet_on_bad_checksum) {
        if (cfg_.emit_bad_checksum_points) emit_packet(p, lsn, is_start, t_rx_ns);
        head_ += total;
      } else {
        drop_bytes(2, true);
      }
      continue;
    }

    ++stats_.packets_ok;
    emit_packet(p, lsn, is_start, t_rx_ns);
    head_ += total;
  }
}

void Parser::emit_packet(const uint8_t* p, uint8_t lsn, bool is_start,
                         uint64_t t_rx_ns) {
  const uint16_t fsa_raw = le16(p + 4);
  const uint16_t lsa_raw = le16(p + 6);
  const int fsa64 = fsa_raw >> 1;   // 1/64 degree units
  const int lsa64 = lsa_raw >> 1;

  // Per-sample angle step, mirroring the vendor's waitPackage():
  //  - LSN == 1                         -> no step
  //  - LSA < FSA and the packet spans 0 -> add a full revolution
  //  - LSA < FSA otherwise (glitch)     -> reuse the previous packet's step
  float interval64;
  if (lsn <= 1) {
    interval64 = 0.f;
  } else if (lsa64 < fsa64) {
    if (fsa64 > 270 * 64 && lsa64 < 90 * 64) {
      interval64 = static_cast<float>(kDeg64Full + lsa64 - fsa64) /
                   static_cast<float>(lsn - 1);
      last_interval_deg64_ = interval64;
    } else {
      interval64 = last_interval_deg64_;
    }
  } else {
    interval64 = static_cast<float>(lsa64 - fsa64) / static_cast<float>(lsn - 1);
    last_interval_deg64_ = interval64;
  }

  if (is_start) {
    ++stats_.start_packets;
    stats_.scan_freq_raw = static_cast<uint8_t>(p[2] >> 1);  // M = M&T >> 1
    saw_start_packet_ = true;
    pending_new_rotation_ = true;
    ++stats_.rotations;
  }

  // --- ROUND 9: when was each sample actually TAKEN? -------------------------
  //
  // Config::per_sample_timestamps documents the model in full. Briefly: the
  // device blasts a buffered packet at the line rate (~1.7x faster than it
  // samples), so spacing samples at the WIRE rate compresses them. Space them
  // at the SAMPLING rate instead, anchored on the packet's first byte, which
  // is the moment just after its last sample was taken.
  const size_t total_bytes = kHeaderBytes + static_cast<size_t>(lsn) * kSampleBytes;
  double t_last_sample = 0.0;
  double sample_ns = 0.0;
  const bool stamp_samples = cfg_.per_sample_timestamps && t_rx_ns != 0 &&
                             cfg_.wire_bytes_per_sec > 0.0 && cfg_.nominal_sample_hz > 0.0;
  if (stamp_samples) {
    const double byte_ns = 1e9 / cfg_.wire_bytes_per_sec;

    // (3) The wire-rate model, doing the one job it is right for: locating
    // this packet inside the read burst. Bytes still buffered BEHIND this
    // packet arrived after its last byte did.
    const size_t bytes_after = avail() - total_bytes;
    const double t_last_byte = static_cast<double>(t_rx_ns) - static_cast<double>(bytes_after) * byte_ns;

    // (2) The anchor.
    t_last_sample = t_last_byte - static_cast<double>(total_bytes) * byte_ns;

    // (1) The spacing. Prefer the device's own behaviour — this packet's
    // angle span against the reported spin rate — over the datasheet number,
    // so real spin-speed variation is honoured.
    sample_ns = 1e9 / cfg_.nominal_sample_hz;
    if (lsn > 1 && interval64 > 0.f && stats_.scan_freq_raw != 0) {
      const double scan_hz = static_cast<double>(stats_.scan_freq_raw) * cfg_.scan_freq_raw_to_hz;
      if (scan_hz > 1.0 && scan_hz < 100.0) {
        const double deg_per_sample = static_cast<double>(interval64) / 64.0;
        const double hz = 360.0 * scan_hz / deg_per_sample;
        stats_.sample_hz_est = hz;
        // The cross-check the spec asks for: angle-derived rate vs the
        // datasheet's 4 kHz. Inside the band, trust the device. Outside it,
        // this is a corrupt angle span rather than a slow motor — fall back to
        // nominal and say so.
        const double rel = std::fabs(hz - cfg_.nominal_sample_hz) / cfg_.nominal_sample_hz;
        if (rel <= cfg_.sample_rate_tolerance) {
          sample_ns = 1e9 / hz;
        } else {
          ++stats_.sample_rate_warnings;
        }
      }
    }

    // --- the min-delay sample clock ----------------------------------------
    //
    // The wire anchor above is good but BIASED LATE, and the bias is exactly
    // the burst duty cycle. Back-dating `bytes_after` at the LINE rate assumes
    // the link was busy the whole time; at ~60% duty it was idle for 40% of
    // it, so the real elapsed time is ~1.7x what the model says and the anchor
    // lands too late — by up to a hundred milliseconds at the head of a
    // phone-sized 4 KB read. The anchor is therefore an UPPER BOUND on the
    // true sample time, never a lower one.
    //
    // The device's own sampling rate, however, is a superb clock: steady,
    // known to 0.1 Hz from the scan-frequency field, and independent of how
    // the bytes happen to be bunched. So run the classic min-delay estimator
    // over the two: propagate the previous packet's time forward at the
    // sampling period, and take whichever of {propagated, wire anchor} is
    // EARLIER.
    //
    // This converges for free. At the TAIL of every read `bytes_after` is ~0,
    // so that packet's anchor is tight and the minimum takes it; the chain
    // then carries that tight value forward through the head of the NEXT read,
    // where the anchor is loose and the propagation wins. Every packet ends up
    // dated from the nearest tight anchor rather than from the line-rate
    // fiction.
    //
    // It also subsumes what would otherwise be a separate monotonicity clamp:
    // the propagated term is strictly increasing by construction, so the two
    // invariants the assembler needs — non-decreasing stamps, and no stamp in
    // the future of the transport that carried it — both fall out.
    if (cfg_.sample_clock_anchor && prev_sample_ns_ != 0.0) {
      const double propagated = prev_sample_ns_ + static_cast<double>(lsn) * sample_ns;
      if (t_last_sample - propagated > cfg_.sample_clock_resync_ns) {
        // The chain has lost the stream: dropped packets, a stall, or a device
        // restart. Nothing to propagate from — fall back to the anchor and
        // start a new chain.
        ++stats_.sample_clock_resyncs;
      } else {
        t_last_sample = std::min(t_last_sample, propagated);
      }
    }

    // Monotonicity across the packet boundary. The constraint is on this
    // packet's FIRST sample, not its last: the window is (lsn-1) periods wide
    // and must start no earlier than the previous packet's final sample. When
    // the anchor leaves less room than that — a saturated stream, or a
    // re-ordered read — hold the anchor (invariant (ii): never stamp into the
    // future of the transport) and COMPRESS the spacing to fit, which degrades
    // gracefully to ROUND 7's wire-rate spacing rather than lying about it.
    if (prev_sample_ns_ != 0.0 && lsn > 1) {
      const double room = t_last_sample - prev_sample_ns_;
      const double want = static_cast<double>(lsn - 1) * sample_ns;
      if (room < want) {
        sample_ns = room > 0.0 ? room / static_cast<double>(lsn - 1) : 0.0;
        if (room <= 0.0) t_last_sample = prev_sample_ns_;
      }
    }
    prev_sample_ns_ = t_last_sample;
  }

  const uint8_t* s = p + kHeaderBytes;
  for (uint8_t i = 0; i < lsn; ++i, s += kSampleBytes) {
    Point pt;
    pt.distance_mm       = sample_distance_mm(s);
    pt.intensity         = sample_intensity(s);
    pt.high_reflectivity = sample_high_reflectivity(s);
    pt.from_start_packet = is_start;
    pt.t_rx_ns           = t_rx_ns;
    if (stamp_samples) {
      // Sample `lsn-1` sits on the anchor; earlier ones are back-dated at the
      // sampling period.
      const double t = t_last_sample - static_cast<double>(lsn - 1 - i) * sample_ns;
      pt.t_sample_ns = t > 0.0 ? static_cast<uint64_t>(t + 0.5) : 0;
    }

    float a64 = static_cast<float>(fsa64) + interval64 * static_cast<float>(i);
    if (cfg_.apply_mechanical_angle_correction && pt.distance_mm != 0) {
      const double d = pt.distance_mm;
      a64 += static_cast<float>(std::atan(19.16 * (d - 90.15) / (90.15 * d)) * 64.0);
    }
    pt.angle_deg = wrap_deg64(a64) / 64.f;

    // Rotation detection: start packets when the device sends them, otherwise
    // fall back to a 360 -> 0 angle wrap (the vendor's coin-path heuristic).
    if (!saw_start_packet_ && prev_angle_deg_ >= 0.f &&
        prev_angle_deg_ > 270.f && pt.angle_deg < 90.f) {
      pending_new_rotation_ = true;
      ++stats_.rotations;
    }
    prev_angle_deg_ = pt.angle_deg;

    pt.new_rotation = pending_new_rotation_;
    pending_new_rotation_ = false;

    if (pt.distance_mm == 0) {
      ++stats_.points_zero_range;
      if (cfg_.drop_zero_range) {
        // Keep the rotation marker alive for the next emitted point.
        if (pt.new_rotation) pending_new_rotation_ = true;
        continue;
      }
    }
    ++stats_.points;
    emit(pt);
  }
  if (on_packet_) on_packet_(is_start, lsn);
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
  stats_.points_per_sec =
      static_cast<double>(stats_.points - rate_points0_) / secs;
  stats_.rotation_hz =
      static_cast<double>(stats_.rotations - rate_rotations0_) / secs;
  rate_t0_ns_ = t_ns;
  rate_points0_ = stats_.points;
  rate_rotations0_ = stats_.rotations;
}

std::string Parser::stats_line() const {
  char b[320];
  std::snprintf(b, sizeof(b),
                "pkt ok=%llu bad_cs=%llu malformed=%llu resync=%llu | "
                "cs_pass=%.2f%% (vendor=%llu spec=%llu) | "
                "pts=%llu %.0f pts/s | rot=%llu %.2f Hz | "
                "speedadj=%llu garbage=%llu",
                (unsigned long long)stats_.packets_ok,
                (unsigned long long)stats_.packets_bad_checksum,
                (unsigned long long)stats_.packets_malformed,
                (unsigned long long)stats_.resyncs,
                stats_.checksum_pass_rate() * 100.0,
                (unsigned long long)stats_.cs_ok_vendor,
                (unsigned long long)stats_.cs_ok_spec,
                (unsigned long long)stats_.points, stats_.points_per_sec,
                (unsigned long long)stats_.rotations, stats_.rotation_hz,
                (unsigned long long)stats_.speed_adjust_bytes,
                (unsigned long long)stats_.bytes_discarded);
  return std::string(b);
}

}  // namespace d6
