// Synthetic LDROBOT STL-27L packet builder for the unit tests and for
// engine_cli's --synth/--replay.
//
// Every packet here is assembled BYTE BY BYTE at the offsets the protocol
// note gives, and the CRC is computed by an INDEPENDENT bitwise routine —
// deliberately NOT by calling stl27l::crc8(), so the tests cross-check two
// implementations of the same specification rather than checking one
// implementation against itself. (Same discipline as tests/packet_builder.h,
// which replays the COIN-D6 vendor SDK's checksum state machine instead of
// calling d6::checksum().)
//
// The bitwise routine below IS the vendor's table generator, unrolled: the
// LDROBOT SDK ships `CrcTable[256]` and does `crc = CrcTable[crc ^ byte]`,
// which is exactly "XOR the byte in, then shift the register eight times
// through poly 0x4D". Writing it out is what makes the table checkable.
#ifndef STL27L_TEST_PACKET_BUILDER_H
#define STL27L_TEST_PACKET_BUILDER_H

#include <cmath>
#include <cstdint>
#include <vector>

namespace stl27ltest {

inline constexpr std::size_t kPointsPerPacket = 12;
inline constexpr std::size_t kPacketBytes = 47;

struct Sample {
  uint16_t distance_mm = 0;
  uint8_t  intensity   = 0;
};

// CRC8, computed bit by bit: poly 0x4D, init 0x00, MSB-first, no reflection,
// no final XOR. No table, no dependency on the parser.
inline uint8_t crc8_bitwise(const uint8_t* p, std::size_t n) {
  uint8_t crc = 0x00;
  for (std::size_t i = 0; i < n; ++i) {
    crc = static_cast<uint8_t>(crc ^ p[i]);
    for (int b = 0; b < 8; ++b) {
      crc = static_cast<uint8_t>((crc & 0x80) ? ((crc << 1) ^ 0x4D) : (crc << 1));
    }
  }
  return crc;
}

// Degrees -> the on-wire 0.01-degree field, wrapped into one revolution.
inline uint16_t encode_angle(double deg) {
  long v = std::lround(deg * 100.0);
  while (v < 0) v += 36000;
  while (v >= 36000) v -= 36000;
  return static_cast<uint16_t>(v);
}

enum class CrcMode { kGood, kCorrupt };

struct PacketSpec {
  uint16_t speed_dps = 3600;      // 10 Hz
  double first_angle_deg = 0.0;
  double last_angle_deg = 2.0;    // 12 points at the datasheet rate span 2 deg
  uint16_t timestamp_ms = 0;
  std::vector<Sample> samples;    // padded/truncated to 12
  CrcMode crc_mode = CrcMode::kGood;
  uint8_t force_header = 0x54;    // framing tests
  uint8_t force_ver_len = 0x2C;   // framing tests (wrong point count / version)
};

inline void push_le16(std::vector<uint8_t>* v, uint16_t x) {
  v->push_back(static_cast<uint8_t>(x & 0xFF));
  v->push_back(static_cast<uint8_t>(x >> 8));
}

// Assemble one 47-byte packet at the documented offsets:
//   0 header | 1 ver_len | 2..3 speed | 4..5 start_angle
//   6..41  12 x (u16 distance, u8 intensity)
//   42..43 end_angle | 44..45 timestamp | 46 crc8
inline std::vector<uint8_t> build(const PacketSpec& ps) {
  std::vector<uint8_t> pkt;
  pkt.reserve(kPacketBytes);
  pkt.push_back(ps.force_header);
  pkt.push_back(ps.force_ver_len);
  push_le16(&pkt, ps.speed_dps);
  push_le16(&pkt, encode_angle(ps.first_angle_deg));
  for (std::size_t i = 0; i < kPointsPerPacket; ++i) {
    const Sample s = i < ps.samples.size() ? ps.samples[i] : Sample{};
    push_le16(&pkt, s.distance_mm);
    pkt.push_back(s.intensity);
  }
  push_le16(&pkt, encode_angle(ps.last_angle_deg));
  push_le16(&pkt, ps.timestamp_ms);
  uint8_t crc = crc8_bitwise(pkt.data(), pkt.size());  // over the 46 preceding bytes
  if (ps.crc_mode == CrcMode::kCorrupt) crc = static_cast<uint8_t>(crc ^ 0x5A);
  pkt.push_back(crc);
  return pkt;
}

inline void append(std::vector<uint8_t>* dst, const std::vector<uint8_t>& src) {
  dst->insert(dst->end(), src.begin(), src.end());
}

// A full synthetic revolution: `packets` packets of 12 points each, evenly
// spread over 360 degrees, with a device clock advancing at the implied rate
// and wrapping at 30000 ms like the real one.
//
// `t0_ms` is the device timestamp of the FIRST packet; `spin_hz` sets both the
// `speed` field and the timestamp advance, so the two agree the way they must
// on real hardware.
inline std::vector<uint8_t> build_revolution(int packets, uint16_t distance_mm = 1000,
                                             uint8_t intensity = 128, double spin_hz = 10.0,
                                             uint16_t t0_ms = 0, int revolutions = 1) {
  std::vector<uint8_t> out;
  const double step = 360.0 / static_cast<double>(packets * static_cast<int>(kPointsPerPacket));
  const double packet_ms = 1000.0 / (spin_hz * static_cast<double>(packets));
  double t_ms = static_cast<double>(t0_ms);
  for (int rev = 0; rev < revolutions; ++rev) {
    for (int k = 0; k < packets; ++k) {
      PacketSpec ps;
      ps.speed_dps = static_cast<uint16_t>(std::lround(spin_hz * 360.0));
      const double a0 = step * static_cast<double>(k * static_cast<int>(kPointsPerPacket));
      ps.first_angle_deg = a0;
      ps.last_angle_deg = a0 + step * static_cast<double>(kPointsPerPacket - 1);
      t_ms += packet_ms;
      ps.timestamp_ms = static_cast<uint16_t>(std::fmod(t_ms, 30000.0));
      for (std::size_t i = 0; i < kPointsPerPacket; ++i) {
        ps.samples.push_back(Sample{distance_mm, intensity});
      }
      append(&out, build(ps));
    }
  }
  return out;
}

}  // namespace stl27ltest

#endif  // STL27L_TEST_PACKET_BUILDER_H
