// COIN-D6 2D lidar packet parser -- portable C++17, no dependencies beyond the
// standard library and no platform code (serial I/O lives in tools/).
//
// Byte stream in (feed), decoded points out (callback or internal queue).
//
// Wire format (spec §2.2, vendor SDK "M1CT_TOF" path = 230400 baud,
// 3-byte samples, intensity enabled):
//
//   offset  0  1   2    3    4    5    6    7    8    9   10 ...
//           AA 55  M&T  LSN  FSAL FSAH LSAL LSAH CSL  CSH  S1_L S1_2nd S1_H ...
//
//   PH   = 0x55AA little-endian, i.e. the bytes AA 55 on the wire
//   M&T  = bit0 : 0 = point-cloud packet, 1 = start packet (new revolution)
//          bits1-7 : scan frequency, only meaningful in a start packet
//   LSN  = number of 3-byte samples in this packet (1 in a start packet)
//   FSA  = angle of the first sample, LE, bit0 is a constant check bit (=1)
//   LSA  = angle of the last sample, same encoding
//   CS   = 16-bit XOR checksum, see checksum() below
//
//   Distance  = Si_H * 64 + (Si_2nd >> 2)               [mm, 14 bit]
//   Intensity = (Si_2nd & 0x03) * 64 + (Si_L >> 2)      [0..255]
//   HighRefl  = Si_L & 0x01
//   Angle_deg = (FSA >> 1) / 64, interpolated to LSA across the packet
//
#ifndef D6_PARSER_H
#define D6_PARSER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace d6 {

// --- wire constants ---------------------------------------------------------
inline constexpr uint8_t  kPh0          = 0xAA;    // first header byte on the wire
inline constexpr uint8_t  kPh1          = 0x55;
inline constexpr uint16_t kPhWord       = 0x55AA;  // header as a 16-bit word
inline constexpr size_t   kHeaderBytes  = 10;      // PH..CS inclusive
inline constexpr size_t   kSampleBytes  = 3;
inline constexpr size_t   kMaxPacketLen = kHeaderBytes + 255 * kSampleBytes;
inline constexpr uint8_t  kAngleCheckBit = 0x01;   // FSA/LSA bit0, constant 1
inline constexpr int      kDeg64Full    = 360 * 64;  // 23040, one revolution

// Speed-adjustment filler bytes emitted before the rotation stabilises (spec §1).
inline constexpr uint8_t kSpeedAdjA = 0xFE;
inline constexpr uint8_t kSpeedAdjB = 0xFF;

// --- checksum variants ------------------------------------------------------
//
// The spec says the packet checksum is a 16-bit XOR whose *order is not byte
// order*, and points at a figure that did not survive into the text-only
// translation. The two readings that survive the text are implemented below;
// kVendorSdk is ground truth (it is what the shipping vendor driver computes)
// and is the default. See REPORT.md.
enum class ChecksumVariant {
  kVendorSdk,    // per sample: (0x00<<8 | Si_L) and (Si_H<<8 | Si_2nd)
  kSpecLiteral,  // per sample: (Si_2nd<<8 | Si_L) and (0x00<<8 | Si_H)
};

// --- decoded point ----------------------------------------------------------
struct Point {
  float    angle_deg        = 0.f;    // [0, 360)
  uint16_t distance_mm      = 0;      // 0 = no return
  uint8_t  intensity        = 0;      // 0..255
  bool     high_reflectivity = false; // Si_L bit0
  bool     new_rotation     = false;  // first point of a new revolution
  bool     from_start_packet = false; // carried by a T=1 packet
  uint64_t t_rx_ns          = 0;      // host receive time of the carrying packet
};

// --- statistics -------------------------------------------------------------
struct Stats {
  uint64_t bytes_in            = 0;
  uint64_t bytes_discarded     = 0;  // garbage dropped while hunting for a header
  uint64_t speed_adjust_bytes  = 0;  // 0xFE / 0xFF filler dropped while hunting
  uint64_t packets_ok          = 0;
  uint64_t packets_bad_checksum = 0;
  uint64_t packets_malformed   = 0;  // header sanity failed (LSN=0, check bit, ...)
  uint64_t resyncs             = 0;  // contiguous runs of dropped garbage
  uint64_t start_packets       = 0;
  uint64_t rotations           = 0;
  uint64_t points              = 0;
  uint64_t points_zero_range   = 0;

  // Bench diagnostics: how many packets each checksum reading would have
  // accepted. On real hardware exactly one of these should track packets_ok.
  uint64_t cs_ok_vendor = 0;
  uint64_t cs_ok_spec   = 0;

  // Rolling rates, refreshed roughly once per second.
  double   points_per_sec = 0.0;
  double   rotation_hz    = 0.0;
  // Scan frequency reported by the device in the last start packet:
  // M&T >> 1. Raw units; the vendor SDK treats it as Hz*10 for its health check.
  uint8_t  scan_freq_raw  = 0;

  double checksum_pass_rate() const {
    const uint64_t tot = packets_ok + packets_bad_checksum;
    return tot ? static_cast<double>(packets_ok) / static_cast<double>(tot) : 0.0;
  }
};

// --- configuration ----------------------------------------------------------
struct Config {
  ChecksumVariant checksum = ChecksumVariant::kVendorSdk;

  // Reject packets whose FSA/LSA low byte does not carry the constant check
  // bit. The vendor driver does this and it is a cheap false-header filter.
  bool require_angle_check_bit = true;

  // Vendor waitPackage() applies a mechanical parallax correction
  //   atan(19.16*(d-90.15)/(90.15*d)) * 64   [1/64 deg]
  // to every point. The D6 spec says the zero-angle offset is already
  // compensated mechanically and should be ignored, so this is OFF by default.
  bool apply_mechanical_angle_correction = false;

  // Emit points from packets that failed the checksum (diagnostics only).
  bool emit_bad_checksum_points = false;

  // Drop points with distance 0 (no return) instead of emitting them.
  bool drop_zero_range = false;

  // On a checksum failure, consume the whole packet (vendor behaviour, keeps
  // stream alignment) rather than restarting the header hunt one byte later.
  bool consume_packet_on_bad_checksum = true;

  // Hard cap on the internal reassembly buffer.
  size_t max_buffered_bytes = 1 << 16;
};

// --- free functions ---------------------------------------------------------

// Monotonic host clock in nanoseconds (steady_clock; portable).
uint64_t now_ns();

// Compute the 16-bit packet checksum over a complete packet `p` of `lsn`
// samples (p must hold kHeaderBytes + lsn*3 bytes). The CS field itself does
// not take part.
uint16_t checksum(const uint8_t* p, uint8_t lsn, ChecksumVariant v);

// Build a decoded sample triplet -- exposed for tests and tooling.
inline uint16_t sample_distance_mm(const uint8_t* s) {
  return static_cast<uint16_t>(s[2] * 64 + (s[1] >> 2));
}
inline uint8_t sample_intensity(const uint8_t* s) {
  return static_cast<uint8_t>((s[1] & 0x03) * 64 + (s[0] >> 2));
}
inline bool sample_high_reflectivity(const uint8_t* s) { return (s[0] & 0x01) != 0; }

// --- parser -----------------------------------------------------------------
class Parser {
 public:
  using PointCallback = std::function<void(const Point&)>;
  // Called once per accepted packet, after its points. Useful for framing.
  using PacketCallback = std::function<void(bool is_start_packet, uint8_t lsn)>;

  Parser() = default;
  explicit Parser(const Config& cfg) : cfg_(cfg) {}

  void set_config(const Config& cfg) { cfg_ = cfg; }
  const Config& config() const { return cfg_; }

  // If set, points are delivered here; otherwise they queue internally and are
  // retrieved with take_points().
  void set_point_callback(PointCallback cb) { on_point_ = std::move(cb); }
  void set_packet_callback(PacketCallback cb) { on_packet_ = std::move(cb); }

  // Feed raw bytes. Safe to call with arbitrary chunk boundaries: packets torn
  // across calls are reassembled. `t_rx_ns` = host time these bytes arrived
  // (0 -> now_ns()).
  void feed(const uint8_t* data, size_t n, uint64_t t_rx_ns = 0);
  void feed(const std::vector<uint8_t>& v, uint64_t t_rx_ns = 0) {
    feed(v.data(), v.size(), t_rx_ns);
  }

  // Move the queued points out (no-op when a callback is installed).
  void take_points(std::vector<Point>* out);
  size_t queued_points() const { return queue_.size(); }

  const Stats& stats() const { return stats_; }
  void reset();

  // Human readable one-liner for the CLI status bar.
  std::string stats_line() const;

 private:
  size_t avail() const { return buf_.size() - head_; }
  const uint8_t* at() const { return buf_.data() + head_; }
  void drop_bytes(size_t n, bool garbage);
  void compact();
  void parse(uint64_t t_rx_ns);
  void emit_packet(const uint8_t* p, uint8_t lsn, bool is_start, uint64_t t_rx_ns);
  void emit(const Point& pt);
  void update_rates(uint64_t t_ns);

  Config cfg_{};
  Stats  stats_{};
  std::vector<uint8_t> buf_;
  size_t head_ = 0;

  PointCallback  on_point_;
  PacketCallback on_packet_;
  std::vector<Point> queue_;

  // Angle interpolation state.
  float last_interval_deg64_ = 0.f;  // vendor's IntervalSampleAngle_LastPackage
  // Rotation detection state.
  bool  saw_start_packet_ = false;
  bool  pending_new_rotation_ = false;
  float prev_angle_deg_ = -1.f;
  // Resync bookkeeping.
  bool  in_garbage_run_ = false;
  // Rate window.
  uint64_t rate_t0_ns_ = 0;
  uint64_t rate_points0_ = 0;
  uint64_t rate_rotations0_ = 0;
};

}  // namespace d6

#endif  // D6_PARSER_H
