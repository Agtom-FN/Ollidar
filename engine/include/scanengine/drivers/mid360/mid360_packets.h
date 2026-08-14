// mid360_packets.h — the Mid-360 wire layer, with no Livox SDK in sight.
//
// Owner: A3.
//
// WHY THIS FILE IS SEPARATE FROM THE SDK. Everything here — the packet
// layout, the loss model, the point filter — is logic we must be able to
// unit-test on all five CI legs, none of which have (or want) a vendored
// SDK2 checkout. So the byte layouts are mirrored from
// third_party/Livox-SDK2/include/livox_lidar_def.h and static_asserted
// against the sizes the S2 spike verified against Livox's own recordings,
// and the driver's SDK backend hands raw datagram bytes *through* this file
// rather than around it. `tests/test_mid360_driver.cpp` therefore exercises
// the real production path with synthetic packets and links no SDK at all.
//
// Sources cross-checked by S2 (spikes/s2-mid360-sim/REPORT.md §2), each
// field trusted only where at least two agreed:
//   [S] Livox-SDK2 source, [D] the published Mid-360 protocol tables,
//   [R] a real Mid-360 recording (Livox's own Indoor_sampledata.lvx2).
#ifndef SCANENGINE_DRIVERS_MID360_MID360_PACKETS_H
#define SCANENGINE_DRIVERS_MID360_MID360_PACKETS_H

#include <cstddef>
#include <cstdint>

namespace scanengine {
namespace mid360 {

// --- wire layout ----------------------------------------------------------
//
// 36-byte header shared by the point and IMU streams. Mirrors the prefix of
// LivoxLidarEthernetPacket ([S]); byte offsets confirmed by [D] and by
// parsing [R] byte-for-byte.
#pragma pack(push, 1)
struct DataHeader {
  std::uint8_t version;         // 0
  std::uint16_t length;         // whole datagram, header included
  std::uint16_t time_interval;  // 0.1 us, last point time − first point time
  std::uint16_t dot_num;        // points in this datagram (96 for a point packet)
  std::uint16_t udp_cnt;        // see LossTracker — FREE-RUNNING on real firmware
  std::uint8_t frame_cnt;       // always 0 on real firmware, contrary to [D]
  std::uint8_t data_type;       // kDataType*
  std::uint8_t time_type;       // 0 = lidar-local (power-on) clock
  std::uint8_t rsvd[12];
  std::uint32_t crc32;          // over timestamp + payload
  std::uint64_t timestamp;      // device clock, ns
};

// data_type == 1. 200k pts/s of these, 96 per datagram.
struct CartesianHigh {
  std::int32_t x, y, z;  // millimetres, lidar frame
  std::uint8_t reflectivity;
  std::uint8_t tag;
};

// data_type == 2. Half the bytes, centimetre resolution. The driver can
// decode it but never asks for it: A6 wants millimetres.
struct CartesianLow {
  std::int16_t x, y, z;  // centimetres
  std::uint8_t reflectivity;
  std::uint8_t tag;
};

// data_type == 0. One sample per datagram at 200 Hz.
struct ImuRaw {
  float gyro_x, gyro_y, gyro_z;  // rad/s
  float acc_x, acc_y, acc_z;     // g  (S2 measured mean |acc| = 1.0000 g)
};
#pragma pack(pop)

static_assert(sizeof(DataHeader) == 36, "Mid-360 data header is 36 bytes");
static_assert(sizeof(CartesianHigh) == 14, "cartesian-high point is 14 bytes");
static_assert(sizeof(CartesianLow) == 8, "cartesian-low point is 8 bytes");
static_assert(sizeof(ImuRaw) == 24, "IMU sample is 24 bytes");
static_assert(offsetof(DataHeader, crc32) == 24, "crc32 sits at offset 24");
static_assert(offsetof(DataHeader, timestamp) == 28, "timestamp sits at offset 28");

inline constexpr std::uint8_t kDataTypeImu = 0;
inline constexpr std::uint8_t kDataTypeCartesianHigh = 1;
inline constexpr std::uint8_t kDataTypeCartesianLow = 2;

inline constexpr std::uint16_t kPointsPerPacket = 96;
inline constexpr std::size_t kPointPacketBytes =
    sizeof(DataHeader) + kPointsPerPacket * sizeof(CartesianHigh);  // 1380
inline constexpr std::size_t kImuPacketBytes = sizeof(DataHeader) + sizeof(ImuRaw);  // 60
static_assert(kPointPacketBytes == 1380, "the canonical Mid-360 point packet is 1380 bytes");

// Nominal rates, used for health thresholds and for the docs' expectations.
inline constexpr double kNominalPointsPerSec = 200000.0;
inline constexpr double kNominalImuHz = 200.0;

// Default Mid-360 UDP ports ([S] comm/define.h, [D] port table).
inline constexpr std::uint16_t kLidarCmdPort = 56100;
inline constexpr std::uint16_t kLidarPushPort = 56200;
inline constexpr std::uint16_t kLidarPointPort = 56300;
inline constexpr std::uint16_t kLidarImuPort = 56400;
inline constexpr std::uint16_t kLidarLogPort = 56500;

// --- tag semantics --------------------------------------------------------
//
// The Mid-360 tag byte packs three fields. Bit numbering per [D]:
//   bits 1:0  return number within a multi-return sample
//   bits 3:2  noise confidence from SPATIAL position (0 = normal / not noise,
//             1 = high confidence it IS noise, 2 = moderate, 3 = low)
//   bits 5:4  noise confidence from RETURN INTENSITY, same scale
//   bits 7:6  reserved
//
// The tag distribution S2 measured over a real 5 s indoor slice
// (spikes/s2-mid360-sim/FIXTURES.md §1, 1,000,128 points):
//   {0: 983484, 1: 5638, 2: 1549, 4: 3959, 5: 34, 6: 9, 8: 573, 9: 1,
//    16: 4607, 18: 1, 32: 273}
// i.e. 0.45% carry a spatial-noise flag (4 = level 1, 8 = level 2) and 0.49%
// an intensity-noise flag (16, 32). The simulator emits tag == 0 for every
// point, which is exactly why this filter is tested against the fixture
// histogram and not against the simulator.
inline constexpr std::uint8_t kTagReturnNumberMask = 0x03;
inline constexpr std::uint8_t kTagSpatialNoiseMask = 0x0C;
inline constexpr std::uint8_t kTagIntensityNoiseMask = 0x30;

inline constexpr std::uint8_t tag_return_number(std::uint8_t tag) {
  return static_cast<std::uint8_t>(tag & kTagReturnNumberMask);
}
inline constexpr std::uint8_t tag_spatial_noise(std::uint8_t tag) {
  return static_cast<std::uint8_t>((tag & kTagSpatialNoiseMask) >> 2);
}
inline constexpr std::uint8_t tag_intensity_noise(std::uint8_t tag) {
  return static_cast<std::uint8_t>((tag & kTagIntensityNoiseMask) >> 4);
}

// --- point filtering ------------------------------------------------------
//
// Every knob here is configurable because "which returns are garbage" is a
// site-and-application question, and because S2 could not answer it: the
// simulator produced 0.0017% no-returns against 34.5–67.8% in real Livox
// recordings. The DEFAULTS are chosen from the real fixtures, not the sim:
//
//   drop_no_return = true
//       A no-return is encoded as x == y == z == 0, i.e. a point at the
//       sensor origin. 35.24% of a real indoor slice and 67.76% of a real
//       outdoor one are no-returns; letting them through would pile a third
//       to two thirds of every capture onto the origin, wrecking both the
//       live view and any voxel map built from it.
//
//   tag_reject_mask = kTagSpatialNoiseMask (0x0C)
//       Drops the 0.45% the device itself flags as geometrically suspect
//       (drag points off edges, retro-reflector bloom). Intensity-noise
//       flags (0x30) are deliberately NOT rejected by default: those are
//       usually genuine bright targets whose intensity is untrustworthy,
//       and the geometry is still good. Set to 0x3C to drop both, or 0 to
//       keep everything (what a post-processing / diagnostic run wants).
//
//   min_range_m = 0.1
//       The Mid-360's own close-range blind zone; anything nearer is the
//       housing or self-hit spray.
struct PointFilterConfig {
  bool drop_no_return = true;
  std::uint8_t tag_reject_mask = kTagSpatialNoiseMask;
  float min_range_m = 0.1f;
  float max_range_m = 0.0f;  // 0 = unbounded (the Mid-360 reaches ~70 m)
  std::uint8_t min_reflectivity = 0;
};

struct FilterStats {
  std::uint64_t seen = 0;
  std::uint64_t kept = 0;
  std::uint64_t dropped_no_return = 0;
  std::uint64_t dropped_tag = 0;
  std::uint64_t dropped_range = 0;
  std::uint64_t dropped_reflectivity = 0;

  double keep_fraction() const {
    return seen == 0 ? 0.0 : static_cast<double>(kept) / static_cast<double>(seen);
  }
};

// True if the point survives the filter. `stats` may be null. Range is
// compared in millimetres-squared so no sqrt runs on the 200k pts/s path.
bool point_passes(const CartesianHigh& p, const PointFilterConfig& cfg, FilterStats* stats);

// --- loss accounting ------------------------------------------------------
//
// THE S2 FINDING THAT THIS CLASS EXISTS FOR. The published protocol table
// says udp_cnt resets at every frame start and frame_cnt increments per
// scan. Measured over 4,224 consecutive packages of Livox's own recording:
// udp_cnt increases monotonically with ZERO resets and frame_cnt is 0 for
// every single package. A detector written from the documentation resets its
// expectation at frame boundaries — a blind spot exactly where bursty loss
// shows up — and S2 quantified the cost: 1.917% detected against 2.0%
// injected under the documented model, versus 1.9612% against 1.961% under
// the rule below (0.0002 pp error).
//
// The rule works against BOTH firmware models, which is the point:
//     gap = (uint16)(udp_cnt − prev)
//     gap == 0            duplicate / stalled sender
//     1 < gap < 1024      gap − 1 packets lost
//     gap >= 1024         counter reset; not attributable to loss
//
// EQUALLY IMPORTANT, AND NOT OBVIOUS: this counter is structurally blind to
// a full link outage. S2's link-fault runs measured 0 lost packets across a
// 15-second cable pull, three times over, because the device's counter keeps
// advancing while the wire is down — so the first packet after resume looks
// like prev+1. Extended outages are the data watchdog's job
// (Mid360Driver's wall-clock timer), never this class's.
class LossTracker {
 public:
  enum class Step : std::uint8_t {
    kFirst = 0,          // no previous packet to compare against
    kInSequence = 1,     // gap == 1, the happy path
    kDuplicate = 2,      // gap == 0
    kLoss = 3,           // 1 < gap < kResetThreshold
    kUnattributable = 4, // gap >= kResetThreshold: a counter reset
  };

  static constexpr std::uint16_t kResetThreshold = 1024;

  // Feed one packet's udp_cnt. `lost_out` (optional) receives the number of
  // packets this step attributes to loss (0 unless Step::kLoss).
  Step observe(std::uint16_t udp_cnt, std::uint32_t* lost_out = nullptr);

  void reset();

  std::uint64_t packets() const { return packets_; }
  std::uint64_t lost() const { return lost_; }
  std::uint64_t duplicates() const { return duplicates_; }
  std::uint64_t resets() const { return resets_; }
  bool has_previous() const { return have_prev_; }
  std::uint16_t previous() const { return prev_; }

  // lost / (received + lost) over the tracker's lifetime.
  double loss_fraction() const;

 private:
  bool have_prev_ = false;
  std::uint16_t prev_ = 0;
  std::uint64_t packets_ = 0;
  std::uint64_t lost_ = 0;
  std::uint64_t duplicates_ = 0;
  std::uint64_t resets_ = 0;
};

// --- packet validation ----------------------------------------------------

struct PacketView {
  const DataHeader* header = nullptr;
  const std::uint8_t* payload = nullptr;
  std::size_t payload_bytes = 0;
  std::uint32_t point_count = 0;  // dot_num, after length validation

  bool valid() const { return header != nullptr; }
};

// Validate a received datagram and describe it. Rejects: too short for a
// header, `length` disagreeing with the datagram, a data_type/dot_num
// combination whose payload does not fit. Deliberately does NOT verify the
// CRC32 — the SDK has already done that by the time its callback fires, and
// re-hashing 1380 bytes 2,083 times a second to learn nothing new is the
// kind of cost that shows up on a phone. `Mid360Config::verify_crc` turns it
// on for the raw-UDP backend, where nobody else has checked.
PacketView parse_packet(const std::uint8_t* data, std::size_t len);

// CRC-32/ISO-HDLC over [timestamp .. end], i.e. the bytes the device's own
// header.crc32 covers. Same polynomial the SDK's FastCRC uses.
std::uint32_t crc32_iso_hdlc(const std::uint8_t* data, std::size_t len);
bool crc32_ok(const PacketView& v);

}  // namespace mid360
}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_MID360_MID360_PACKETS_H
