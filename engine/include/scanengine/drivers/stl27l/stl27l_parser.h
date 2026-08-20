// LDROBOT STL-27L 2D lidar packet parser — portable C++17, no dependencies
// beyond the standard library and no platform code (serial I/O stays in the
// apps, exactly as it does for the COIN-D6).
//
// Byte stream in (feed), decoded points out (callback or internal queue).
// Deliberately a structural MIRROR of drivers/d6/d6_parser.h: same feed/queue
// seam, same Stats/Config shape, same per-sample timestamp contract, so the
// pushbroom assembler and the driver layer see one kind of 2-D profile source
// and not two.
//
// ---------------------------------------------------------------------------
// WIRE FORMAT (LD-series: LD06 / LD19 / STL-27L share it byte for byte; the
// STL-27L differs from an LD06 only in rate and range, not in framing)
// ---------------------------------------------------------------------------
//
// 921600 baud, 8N1, over a USB-serial bridge (CP210x / CH340 class). Every
// packet is FIXED at 47 bytes, little-endian throughout:
//
//   off  size  field
//    0    1    header  = 0x54
//    1    1    ver_len = 0x2C   (low 5 bits = 12 measurement points)
//    2    2    speed        [deg/s]
//    4    2    start_angle  [0.01 deg]
//    6   36    12 x { u16 distance_mm ; u8 intensity }
//   42    2    end_angle    [0.01 deg]
//   44    2    timestamp    [ms, wraps at 30000]
//   46    1    crc8         over bytes [0, 46)
//
// Rates, from the datasheet: ~21,600 points/s at a ~10 Hz spin (2160 points
// per revolution, 180 packets per revolution, 1800 packets/s = 84.6 kB/s,
// which is ~92% duty on a 921600 8N1 link). Range 0.02–25 m. A distance of 0
// is "no return", not "a wall at the origin", and is dropped.
//
// ---------------------------------------------------------------------------
// UNVERIFIED — READ THIS BEFORE TRUSTING A NUMBER OUT OF HERE
// ---------------------------------------------------------------------------
// No STL-27L hardware exists on this project yet. Everything in this file is
// PROTOCOL-DERIVED from the public LD-series references, not observed:
//   * the 47-byte frame and its field order,
//   * the CRC8 parameters (see crc8() for the table provenance),
//   * the direction the angle sweeps (see stl27l_driver.h's fan note),
//   * the 30000 ms timestamp wrap.
// The synthetic fixtures in tests/test_stl27l.cpp prove this parser against
// ITSELF and against an independent CRC implementation. They cannot prove the
// spec. First contact with real hardware settles it — the counters below
// (packets_bad_crc, packets_malformed, resyncs) are what to read when it does.
#ifndef STL27L_PARSER_H
#define STL27L_PARSER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stl27l {

// --- wire constants ---------------------------------------------------------
inline constexpr uint8_t  kHeaderByte   = 0x54;  // first byte of every packet
inline constexpr uint8_t  kVerLen       = 0x2C;  // version 1 (bits 5-7), 12 points (bits 0-4)
inline constexpr uint8_t  kVerLenPointMask = 0x1F;
inline constexpr size_t   kPointsPerPacket = 12;
inline constexpr size_t   kPacketBytes  = 47;    // header .. crc8 inclusive
inline constexpr size_t   kCrcOffset    = 46;    // index of the crc8 byte
inline constexpr uint8_t  kCrcPoly      = 0x4D;  // MSB-first, init 0x00
inline constexpr uint32_t kTimestampWrapMs = 30000;
inline constexpr uint32_t kDefaultBaud   = 921600;

// 0.01-degree units, one full revolution.
inline constexpr int kCentiDegFull = 360 * 100;  // 36000

// Datasheet rates, used as the fallback when a packet cannot supply its own.
inline constexpr double kNominalSampleHz   = 21600.0;  // points/s
inline constexpr double kNominalSpinHz     = 10.0;
inline constexpr double kWireBytesPerSec   = 92160.0;  // 921600 8N1 = 10 bits/byte

// --- decoded point ----------------------------------------------------------
//
// Field-for-field the D6's `d6::Point` minus the two fields the LD protocol has
// no analogue for (high_reflectivity, from_start_packet), plus the device
// clock the D6 does not have. `t_sample_ns` carries the SAME contract as the
// D6's: see Config::per_sample_timestamps.
struct Point {
  float    angle_deg   = 0.f;  // [0, 360)
  uint16_t distance_mm = 0;    // 0 = no return
  uint8_t  intensity   = 0;    // 0..255
  bool     new_rotation = false;  // first point of a new revolution
  uint64_t t_rx_ns     = 0;    // host receive time of the carrying packet

  // The estimated instant this return was TAKEN, in the HOST monotonic clock,
  // i.e. the same clock d6::Point::t_sample_ns is in and the only clock the
  // pushbroom assembler can bracket a pose with. 0 when unavailable.
  uint64_t t_sample_ns = 0;

  // The DEVICE's own clock, nanoseconds, unwrapped past the 30000 ms rollover
  // and interpolated across the packet. Diagnostics and drift measurement
  // only — it has no fixed relationship to host time, so nothing downstream
  // may place geometry with it. 0 before the second packet (one packet alone
  // cannot say how long its 12 samples took).
  uint64_t t_device_ns = 0;
};

// One decoded packet, before it is turned into points. Exposed because a
// framing test, a bench tool and the discovery sniffer all want the header
// fields without paying for point expansion.
struct Packet {
  uint16_t speed_dps       = 0;   // degrees per second
  float    start_angle_deg = 0.f; // [0, 360)
  float    end_angle_deg   = 0.f; // [0, 360)
  uint16_t timestamp_ms    = 0;   // device clock, wraps at 30000
  uint16_t distance_mm[kPointsPerPacket] = {};
  uint8_t  intensity[kPointsPerPacket] = {};
};

// --- statistics -------------------------------------------------------------
struct Stats {
  uint64_t bytes_in          = 0;
  uint64_t bytes_discarded   = 0;  // garbage dropped while hunting for a header
  uint64_t packets_ok        = 0;
  uint64_t packets_bad_crc   = 0;
  uint64_t packets_malformed = 0;  // header sanity failed (wrong ver_len)
  uint64_t resyncs           = 0;  // contiguous runs of dropped garbage
  uint64_t rotations         = 0;
  uint64_t points            = 0;
  uint64_t points_zero_range = 0;
  uint64_t timestamp_wraps   = 0;  // device clock rolled 30000 -> 0

  // Rolling rates, refreshed roughly once per second.
  double points_per_sec = 0.0;
  double rotation_hz    = 0.0;
  // Last packet's `speed` field, degrees/second (1800-3600 in the wild).
  uint16_t speed_dps = 0;

  // Sample-timing diagnostics, same meaning as d6::Stats'.
  double   sample_hz_est = 0.0;
  uint64_t sample_rate_warnings = 0;
  uint64_t sample_clock_resyncs = 0;

  double crc_pass_rate() const {
    const uint64_t tot = packets_ok + packets_bad_crc;
    return tot ? static_cast<double>(packets_ok) / static_cast<double>(tot) : 0.0;
  }
};

// --- configuration ----------------------------------------------------------
struct Config {
  // Drop points with distance 0 (no return) instead of emitting them. ON by
  // default, unlike the D6's: the LD protocol has no separate validity flag,
  // so a zero here is unambiguously "nothing came back" and a zero-range
  // return placed in the fan frame is a fake point at the sensor origin.
  bool drop_zero_range = true;

  // Emit points from packets that failed the CRC (diagnostics only).
  bool emit_bad_crc_points = false;

  // On a CRC failure, consume the whole 47-byte packet rather than restarting
  // the header hunt one byte later.
  //
  // DEFAULT DIFFERS FROM THE D6 ON PURPOSE. The D6 carries an explicit length
  // field and a 16-bit checksum, so a header that survives the sanity check is
  // almost certainly a real packet boundary and consuming it keeps alignment.
  // The LD frame has only a TWO-BYTE sync (0x54 0x2C) and no length, and 0x54
  // is a perfectly ordinary distance byte — a false header inside the payload
  // is expected, not exotic. Consuming 47 bytes on one would eat the real
  // packet that followed. Dropping a single byte costs at most 46 extra hunt
  // steps and cannot destroy a good frame, so that is the default.
  bool consume_packet_on_bad_crc = false;

  // --- per-sample timestamps (the pushbroom contract) ----------------------
  //
  // Identical in meaning to d6::Config::per_sample_timestamps, and it must
  // stay identical: the assembler interpolates a pose PER POINT and cannot
  // tell which driver produced the point it is placing.
  //
  // The model, in the same three parts the D6 uses:
  //   1. Inside a packet, the 12 returns are spaced at the SAMPLING period,
  //      not the wire period. The period is derived from the device itself —
  //      preferentially from the delta between consecutive `timestamp` fields
  //      (the device's own millisecond clock), otherwise from this packet's
  //      angle span against its `speed` field — and only falls back to
  //      `nominal_sample_hz` when neither is usable.
  //   2. The anchor is the packet's FIRST byte: transmission starts right
  //      after the last of its 12 samples is taken, so
  //          t_last_sample ~= t_last_byte - kPacketBytes / wire_bytes_per_sec
  //      and the eleven earlier returns are back-dated from there.
  //   3. The wire-rate model still locates the packet inside a read burst.
  //
  // Point index 11 sits ON the anchor and index 0 is eleven periods earlier —
  // the same "last sample owns the anchor" convention d6_parser.cpp uses, and
  // the reason this comment insists on it is that the two must not drift
  // apart: a rig that swaps a D6 for an STL-27L must not need a different
  // pose-time offset.
  bool per_sample_timestamps = true;

  // Drive the intra-packet SPACING from the device's millisecond timestamp
  // delta instead of from the angle/speed derivation.
  //
  // OFF by default, and the reason is arithmetic rather than taste: at the
  // datasheet rate a packet spans ~555 us, so consecutive `timestamp` fields
  // differ by 0 or 1 tick and a spacing read off them is quantised by ~80%.
  // The angle/speed derivation — this packet's own angle span against its own
  // `speed` field — is exact, has no quantisation, and is the direct analogue
  // of what d6_parser.cpp does with FSA/LSA against the reported scan
  // frequency. Turn this ON for a unit spun slowly enough that a packet
  // covers several ticks, or to cross-check the two on real hardware.
  //
  // `Point::t_device_ns` is interpolated from the timestamp field either way —
  // that field is the only device clock there is, and it is worth carrying
  // even when it is too coarse to set the spacing with.
  bool use_device_timestamp_spacing = false;

  // Link capacity in bytes/s. 921600 baud 8N1 = 10 bits per byte.
  double wire_bytes_per_sec = kWireBytesPerSec;

  // Datasheet sampling rate, used until the device supplies a better one.
  double nominal_sample_hz = kNominalSampleHz;

  // Run the min-delay sample clock on top of the wire anchor (d6_parser.cpp
  // derives this at length: the anchor is biased LATE by the burst duty
  // cycle, so propagating the previous packet forward and taking the earlier
  // of the two removes most of the bias).
  bool sample_clock_anchor = true;

  // How far the wire anchor may run ahead of the propagated sample clock
  // before the chain is declared broken and re-seeded. 50 ms is ~1080 samples
  // at 21.6 kHz.
  double sample_clock_resync_ns = 50e6;

  // Relative tolerance on the derived sample rate before it is counted as a
  // drift warning (Stats::sample_rate_warnings) and the nominal period is
  // used instead. 0.5 = +/-50% of `nominal_sample_hz`; wider than the D6's
  // 0.25 because the STL-27L's spin is PWM-controlled and legitimately runs
  // anywhere from ~5 to ~13 Hz.
  double sample_rate_tolerance = 0.5;

  // Hard cap on the internal reassembly buffer.
  size_t max_buffered_bytes = 1 << 16;
};

// --- free functions ---------------------------------------------------------

// Monotonic host clock in nanoseconds (steady_clock; portable).
uint64_t now_ns();

// CRC8 over `n` bytes.
//
// PARAMETERS: poly 0x4D, init 0x00, MSB-first (no input or output
// reflection), no final XOR. In Rocksoft notation: width=8 poly=0x4D init=0x00
// refin=false refout=false xorout=0x00.
//
// TABLE PROVENANCE. The LDROBOT LD06/LD19/STL-27L SDK and the vendor's
// protocol note both ship this as a 256-entry constant table, `CrcTable[]`,
// used as `crc = CrcTable[(crc ^ *p++) & 0xff]`. We do NOT paste that table:
// a pasted table is a 256-line assertion nobody can check. It is instead
// GENERATED at namespace scope from the polynomial above (see
// stl27l_parser.cpp), which reproduces the published table exactly — its first
// sixteen entries are 00 4d 9a d7 79 34 e3 ae f2 bf 68 25 8b c6 11 5c, and
// tests/test_stl27l.cpp asserts that prefix plus a full byte-wise recompute
// against the vendor table's own generator loop. If a future firmware ever
// disagrees, the disagreement will land on those assertions and not silently
// on the pass rate.
uint8_t crc8(const uint8_t* p, size_t n);

// Verify + decode a complete 47-byte packet. Returns false for a wrong header
// byte, a wrong ver_len, or a CRC mismatch; `out` may be null (verify only).
// `p` must address at least kPacketBytes bytes.
bool decode_packet(const uint8_t* p, Packet* out);
// Same, without the CRC check — for a diagnostic that wants the fields of a
// packet it already knows is corrupt.
void decode_packet_unchecked(const uint8_t* p, Packet* out);

// Per-point angle by linear interpolation from `start_deg` to `end_deg` over
// `n` points, handling the wrap when the packet spans 0/360 (end < start ⇒ the
// end is a revolution later). Result is wrapped into [0, 360).
//
// step = (end - start) / (n - 1), i.e. the FIRST point sits exactly on
// start_angle and the LAST exactly on end_angle. That is the LD-series
// convention and it is not the only one a vendor could have picked; it is
// asserted directly in tests/test_stl27l.cpp.
float point_angle_deg(float start_deg, float end_deg, size_t i, size_t n);

// The unsigned device-clock delta between two `timestamp` fields, in
// milliseconds, accounting for the 30000 ms wrap. A delta that would be
// negative is read as a wrap, never as time going backwards.
uint32_t timestamp_delta_ms(uint16_t prev_ms, uint16_t now_ms);

// --- parser -----------------------------------------------------------------
class Parser {
 public:
  using PointCallback = std::function<void(const Point&)>;
  // Called once per accepted packet, after its points.
  using PacketCallback = std::function<void(const Packet&)>;

  Parser() = default;
  explicit Parser(const Config& cfg) : cfg_(cfg) {}

  void set_config(const Config& cfg) { cfg_ = cfg; }
  const Config& config() const { return cfg_; }

  // If set, points are delivered here; otherwise they queue internally and are
  // retrieved with take_points().
  void set_point_callback(PointCallback cb) { on_point_ = std::move(cb); }
  void set_packet_callback(PacketCallback cb) { on_packet_ = std::move(cb); }

  // Feed raw bytes. Safe with arbitrary chunk boundaries: packets torn across
  // calls are reassembled. `t_rx_ns` = host time these bytes arrived
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
  void emit_packet(const Packet& pkt, uint64_t t_rx_ns);
  void emit(const Point& pt);
  void update_rates(uint64_t t_ns);

  Config cfg_{};
  Stats  stats_{};
  std::vector<uint8_t> buf_;
  size_t head_ = 0;

  PointCallback  on_point_;
  PacketCallback on_packet_;
  std::vector<Point> queue_;

  // Timing state.
  double   prev_sample_ns_ = 0.0;   // previous packet's final sample time (host)
  bool     have_prev_ts_ = false;
  uint16_t prev_timestamp_ms_ = 0;
  double   device_ns_ = 0.0;        // unwrapped device clock at prev packet
  // Rotation detection.
  bool  pending_new_rotation_ = false;
  float prev_angle_deg_ = -1.f;
  // Resync bookkeeping.
  bool  in_garbage_run_ = false;
  // Rate window.
  uint64_t rate_t0_ns_ = 0;
  uint64_t rate_points0_ = 0;
  uint64_t rate_rotations0_ = 0;
};

}  // namespace stl27l

#endif  // STL27L_PARSER_H
