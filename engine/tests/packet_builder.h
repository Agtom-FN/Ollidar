// Synthetic COIN-D6 packet builder for the unit tests.
//
// The checksum here is computed by *replaying the vendor SDK's byte-stream
// state machine* (Lidar_Data_Processing::waitPackage in
// sdk/lidar_data_processing.cpp), deliberately NOT by calling d6::checksum(),
// so the tests cross-check two independent implementations.
#ifndef D6_TEST_PACKET_BUILDER_H
#define D6_TEST_PACKET_BUILDER_H

#include <cmath>
#include <cstdint>
#include <vector>

namespace d6test {

struct Sample {
  uint16_t distance_mm = 0;
  uint8_t  intensity   = 0;
  bool     high_refl   = false;
};

// Encode one 3-byte sample so that the documented formulas invert it:
//   Distance  = Si_H*64 + (Si_2nd>>2)
//   Intensity = (Si_2nd&0x03)*64 + (Si_L>>2)
//   HighRefl  = Si_L & 0x01
inline void encode_sample(const Sample& s, uint8_t out[3]) {
  const uint16_t d = static_cast<uint16_t>(s.distance_mm & 0x3FFF);
  out[2] = static_cast<uint8_t>(d >> 6);                                   // Si_H
  out[1] = static_cast<uint8_t>(((d & 0x3F) << 2) | ((s.intensity >> 6) & 0x03));
  out[0] = static_cast<uint8_t>(((s.intensity & 0x3F) << 2) | (s.high_refl ? 1 : 0));
}

// Angle in degrees -> the on-wire 16-bit FSA/LSA field (check bit set).
inline uint16_t encode_angle(double deg) {
  int deg64 = static_cast<int>(std::lround(deg * 64.0));
  while (deg64 < 0) deg64 += 360 * 64;
  while (deg64 >= 360 * 64) deg64 -= 360 * 64;
  return static_cast<uint16_t>((static_cast<uint16_t>(deg64) << 1) | 0x0001);
}

// --- vendor checksum, replayed byte by byte ---------------------------------
// Header: CheckSumCal starts at PH (0x55AA); FSA and LSA are XORed as raw LE
// words; (M&T | LSN<<8) and LSA are folded in at the end (XOR is commutative).
// Samples: recvPos%3==0 -> XOR the byte zero-extended;
//          recvPos%3==1 -> latch as the low byte;
//          recvPos%3==2 -> XOR (byte<<8 | latched).
inline uint16_t vendor_checksum_replay(const std::vector<uint8_t>& pkt, uint8_t lsn) {
  uint16_t cs = 0x55AA;
  const uint16_t sample_num_and_ct =
      static_cast<uint16_t>(pkt[2] | (pkt[3] << 8));
  const uint16_t fsa = static_cast<uint16_t>(pkt[4] | (pkt[5] << 8));
  const uint16_t lsa = static_cast<uint16_t>(pkt[6] | (pkt[7] << 8));
  cs ^= fsa;
  uint16_t latch = 0;
  const size_t body = static_cast<size_t>(lsn) * 3;
  for (size_t i = 0; i < body; ++i) {
    const uint8_t b = pkt[10 + i];
    switch (i % 3) {
      case 0:
        latch = b;
        cs ^= static_cast<uint16_t>(b);
        break;
      case 1:
        latch = b;
        break;
      default:
        latch = static_cast<uint16_t>(latch + b * 0x100);
        cs ^= latch;
        break;
    }
  }
  cs ^= sample_num_and_ct;
  cs ^= lsa;
  return cs;
}

// The literal reading of the spec text ("the third byte of Si is zero-extended").
inline uint16_t spec_checksum_replay(const std::vector<uint8_t>& pkt, uint8_t lsn) {
  uint16_t cs = 0x55AA;
  cs ^= static_cast<uint16_t>(pkt[2] | (pkt[3] << 8));
  cs ^= static_cast<uint16_t>(pkt[4] | (pkt[5] << 8));
  cs ^= static_cast<uint16_t>(pkt[6] | (pkt[7] << 8));
  for (uint8_t i = 0; i < lsn; ++i) {
    const uint8_t* s = &pkt[10 + static_cast<size_t>(i) * 3];
    cs ^= static_cast<uint16_t>(s[0] | (s[1] << 8));
    cs ^= static_cast<uint16_t>(s[2]);
  }
  return cs;
}

enum class CsMode { kVendor, kSpec, kCorrupt };

struct PacketSpec {
  bool   start_packet = false;
  uint8_t scan_freq   = 0;      // M field, only meaningful in a start packet
  double first_angle_deg = 0.0;
  double last_angle_deg  = 0.0;
  std::vector<Sample> samples;
  CsMode cs_mode = CsMode::kVendor;
  bool   break_fsa_check_bit = false;
  bool   break_lsa_check_bit = false;
  int    force_lsn = -1;        // override the LSN field (framing tests)
};

inline std::vector<uint8_t> build(const PacketSpec& ps) {
  const uint8_t lsn = static_cast<uint8_t>(
      ps.force_lsn >= 0 ? ps.force_lsn : ps.samples.size());
  std::vector<uint8_t> pkt;
  pkt.reserve(10 + ps.samples.size() * 3);
  pkt.push_back(0xAA);
  pkt.push_back(0x55);
  pkt.push_back(static_cast<uint8_t>((ps.scan_freq << 1) | (ps.start_packet ? 1 : 0)));
  pkt.push_back(lsn);
  uint16_t fsa = encode_angle(ps.first_angle_deg);
  uint16_t lsa = encode_angle(ps.last_angle_deg);
  if (ps.break_fsa_check_bit) fsa &= 0xFFFE;
  if (ps.break_lsa_check_bit) lsa &= 0xFFFE;
  pkt.push_back(static_cast<uint8_t>(fsa & 0xFF));
  pkt.push_back(static_cast<uint8_t>(fsa >> 8));
  pkt.push_back(static_cast<uint8_t>(lsa & 0xFF));
  pkt.push_back(static_cast<uint8_t>(lsa >> 8));
  pkt.push_back(0);  // CS placeholder
  pkt.push_back(0);
  for (const auto& s : ps.samples) {
    uint8_t enc[3];
    encode_sample(s, enc);
    pkt.push_back(enc[0]);
    pkt.push_back(enc[1]);
    pkt.push_back(enc[2]);
  }
  uint16_t cs = 0;
  switch (ps.cs_mode) {
    case CsMode::kVendor:  cs = vendor_checksum_replay(pkt, lsn); break;
    case CsMode::kSpec:    cs = spec_checksum_replay(pkt, lsn); break;
    case CsMode::kCorrupt: cs = static_cast<uint16_t>(vendor_checksum_replay(pkt, lsn) ^ 0x0040); break;
  }
  pkt[8] = static_cast<uint8_t>(cs & 0xFF);
  pkt[9] = static_cast<uint8_t>(cs >> 8);
  return pkt;
}

inline void append(std::vector<uint8_t>* dst, const std::vector<uint8_t>& src) {
  dst->insert(dst->end(), src.begin(), src.end());
}

// A full synthetic revolution: `packets` point-cloud packets of `per_packet`
// samples each, evenly spread over 360 degrees, preceded by a start packet.
inline std::vector<uint8_t> build_revolution(int packets, int per_packet,
                                             uint16_t distance_mm = 1000,
                                             uint8_t intensity = 128,
                                             uint8_t scan_freq = 10) {
  std::vector<uint8_t> out;
  const double step = 360.0 / static_cast<double>(packets * per_packet);
  {
    PacketSpec sp;
    sp.start_packet = true;
    sp.scan_freq = scan_freq;
    sp.first_angle_deg = 0.0;
    sp.last_angle_deg = 0.0;
    sp.samples = {Sample{distance_mm, intensity, false}};
    append(&out, build(sp));
  }
  for (int k = 0; k < packets; ++k) {
    PacketSpec sp;
    const double a0 = step * static_cast<double>(k * per_packet);
    sp.first_angle_deg = a0;
    sp.last_angle_deg = a0 + step * static_cast<double>(per_packet - 1);
    for (int i = 0; i < per_packet; ++i)
      sp.samples.push_back(Sample{distance_mm, intensity, false});
    append(&out, build(sp));
  }
  return out;
}

}  // namespace d6test

#endif  // D6_TEST_PACKET_BUILDER_H
