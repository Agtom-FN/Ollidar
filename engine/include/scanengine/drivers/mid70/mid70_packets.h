// mid70_packets.h — the Livox Mid-70 wire layer, with no Livox SDK in sight.
//
// Owner: A17.
//
// The Mid-70 speaks Livox SDK **v1** (github.com/Livox-SDK/Livox-SDK), not
// the SDK2 protocol the Mid-360 driver mirrors. Same design as
// mid360_packets.h and for the same reason: every byte layout here is
// mirrored from sdk_core/include/livox_def.h and static_asserted, so the
// decode / loss / filter logic runs on all CI legs with no SDK checkout and
// the SDK backend hands datagrams *through* this file rather than around it.
//
// Sources, each fact trusted only where two agree:
//   [S] Livox-SDK v1 source (livox_def.h, SDK 2.3.0 header, pinned by
//       third_party/fetch_sdk1.sh)
//   [M] Livox Mid-70 User Manual v1.2 (2021.02) §5.2 "Output Data"
//   [R] the livox_ros_driver (v2.6.0) decode path, which shipped against
//       real Mid-70 firmware in the FAST-LIO work this port descends from
//   [F] a real Mid-70 recording — captures/mid70_real_30s.livoxdump — once
//       the rig is attached (spikes/s8-mid70-sdk1/REPORT.md lists what is
//       still owed to the fixture)
//
// THREE THINGS THAT DIFFER FROM THE MID-360, EACH OF WHICH BROKE SOMETHING
// IN THE ROS DRIVER BEFORE IT WAS UNDERSTOOD:
//
//  1. No length field and NO sequence counter in the header. Datagram
//     validity is "payload divides evenly into points of the declared
//     type"; packet loss cannot be counted from a counter and is instead
//     inferred from device-timestamp gaps (GapTracker below).
//
//  2. The timestamp's MEANING depends on `timestamp_type`. In NoSync/PPS
//     mode it is nanoseconds since the lidar powered on — a free-running
//     clock ~1.7e9 s from the epoch. In PPS+GPS mode it is a packed UTC
//     date. A driver that assumes one will publish stamps that are wrong by
//     the lidar's uptime (measured: 25,566 s and 65,069 s on two days). The
//     engine's A4 offset estimator maps whichever it is onto engine time;
//     this file only decodes honestly and says which it was.
//
//  3. There is no built-in IMU. `kDataTypeImu` exists in the enum for the
//     Horizon/Avia and is never sent by a Mid-70; the driver treats it as
//     unexpected rather than routing it anywhere.
#ifndef SCANENGINE_DRIVERS_MID70_MID70_PACKETS_H
#define SCANENGINE_DRIVERS_MID70_MID70_PACKETS_H

#include <cstddef>
#include <cstdint>

namespace scanengine {
namespace mid70 {

// --- wire layout ----------------------------------------------------------
//
// 18-byte header, mirrors LivoxEthPacket ([S]) minus its `data[1]` tail.
#pragma pack(push, 1)
struct EthHeader {
  std::uint8_t version;          // packet protocol version (5 on current firmware [R])
  std::uint8_t slot;             // hub slot; 0 for a directly-connected lidar
  std::uint8_t id;               // lidar id within a hub slot
  std::uint8_t rsvd;
  std::uint32_t err_code;        // LidarErrorCode bitfield — see decode_error_code()
  std::uint8_t timestamp_type;   // kTimestampType*
  std::uint8_t data_type;        // kDataType*
  std::uint8_t timestamp[8];     // ns since power-on, OR packed UTC — see timestamp_type
};

// data_type 0 — kCartesian. 100 per datagram ([S] kMaxPointNumber; [R]).
struct RawPoint {
  std::int32_t x, y, z;          // millimetres, lidar frame
  std::uint8_t reflectivity;
};

// data_type 1 — kSpherical.
struct SpherPoint {
  std::uint32_t depth;           // millimetres
  std::uint16_t theta;           // zenith, 0.01 deg, [0, 18000]
  std::uint16_t phi;             // azimuth, 0.01 deg, [0, 36000]
  std::uint8_t reflectivity;
};

// data_type 2 — kExtendCartesian. 96 per datagram. The type current Mid-70
// firmware actually sends after SetCartesianCoordinate() [R].
struct ExtendRawPoint {
  std::int32_t x, y, z;
  std::uint8_t reflectivity;
  std::uint8_t tag;              // see the tag section below
};

// data_type 3 — kExtendSpherical.
struct ExtendSpherPoint {
  std::uint32_t depth;
  std::uint16_t theta;
  std::uint16_t phi;
  std::uint8_t reflectivity;
  std::uint8_t tag;
};

// data_type 4 — kDualExtendCartesian. Two returns per sample, 48 per datagram.
struct DualExtendRawPoint {
  std::int32_t x1, y1, z1;
  std::uint8_t reflectivity1;
  std::uint8_t tag1;
  std::int32_t x2, y2, z2;
  std::uint8_t reflectivity2;
  std::uint8_t tag2;
};

// data_type 6 — kImu. Never sent by a Mid-70; here so parse_packet() can
// name it when a Horizon/Avia is plugged in by mistake.
struct ImuPoint {
  float gyro_x, gyro_y, gyro_z;  // rad/s
  float acc_x, acc_y, acc_z;     // g
};
#pragma pack(pop)

static_assert(sizeof(EthHeader) == 18, "Mid-70 eth header is 18 bytes");
static_assert(sizeof(RawPoint) == 13, "cartesian point is 13 bytes");
static_assert(sizeof(SpherPoint) == 9, "spherical point is 9 bytes");
static_assert(sizeof(ExtendRawPoint) == 14, "extended cartesian point is 14 bytes");
static_assert(sizeof(ExtendSpherPoint) == 10, "extended spherical point is 10 bytes");
static_assert(sizeof(DualExtendRawPoint) == 28, "dual extended cartesian point is 28 bytes");
static_assert(sizeof(ImuPoint) == 24, "IMU sample is 24 bytes");
static_assert(offsetof(EthHeader, err_code) == 4, "err_code sits at offset 4");
static_assert(offsetof(EthHeader, timestamp) == 10, "timestamp sits at offset 10");

// PointDataType [S].
inline constexpr std::uint8_t kDataTypeCartesian = 0;
inline constexpr std::uint8_t kDataTypeSpherical = 1;
inline constexpr std::uint8_t kDataTypeExtendCartesian = 2;
inline constexpr std::uint8_t kDataTypeExtendSpherical = 3;
inline constexpr std::uint8_t kDataTypeDualExtendCartesian = 4;
inline constexpr std::uint8_t kDataTypeDualExtendSpherical = 5;
inline constexpr std::uint8_t kDataTypeImu = 6;

// TimestampType [S]. The values are NOT in "strength" order — PPS-only is 4.
inline constexpr std::uint8_t kTimestampNoSync = 0;
inline constexpr std::uint8_t kTimestampPtp = 1;
inline constexpr std::uint8_t kTimestampRsvd = 2;
inline constexpr std::uint8_t kTimestampPpsGps = 3;
inline constexpr std::uint8_t kTimestampPps = 4;
inline constexpr std::uint8_t kTimestampUnknown = 5;

const char* to_string_timestamp_type(std::uint8_t t) noexcept;

// Points per datagram by type ([S] kMaxPointNumber = 100 for the 13-byte
// type; the extended and dual types are sized to the same ~1.3 KB budget).
inline constexpr std::uint32_t kPointsPerPacketCartesian = 100;
inline constexpr std::uint32_t kPointsPerPacketExtend = 96;
inline constexpr std::uint32_t kPointsPerPacketDual = 48;

inline constexpr std::size_t kPacketBytesCartesian =
    sizeof(EthHeader) + kPointsPerPacketCartesian * sizeof(RawPoint);        // 1318
inline constexpr std::size_t kPacketBytesExtendCartesian =
    sizeof(EthHeader) + kPointsPerPacketExtend * sizeof(ExtendRawPoint);     // 1362
static_assert(kPacketBytesCartesian == 1318, "canonical cartesian datagram is 1318 bytes");
static_assert(kPacketBytesExtendCartesian == 1362,
              "canonical extended cartesian datagram is 1362 bytes");

// Nominal rates ([M] Table 1.2.1: 100,000 points/s single return; 200,000
// dual). Used for the timestamp-gap loss model and health expectations.
inline constexpr double kNominalPointsPerSec = 100000.0;
inline constexpr double kNominalPointsPerSecDual = 200000.0;

// SDK v1 device ports ([S] comm/define.h: the lidar listens for commands on
// 65000; broadcasts arrive on the host's 55000).
inline constexpr std::uint16_t kBroadcastPort = 55000;
inline constexpr std::uint16_t kLidarCmdPort = 65000;

// --- err_code -------------------------------------------------------------
//
// LidarErrorCode [S], carried in EVERY datagram. This is the one place the
// device tells the host whether its clock is actually synchronised — the
// FAST-LIO work spent days inferring `time_sync_status` from stamp
// arithmetic when the lidar had been reporting it in each packet.
struct ErrorCode {
  std::uint8_t temp_status;      // 0 normal, 1 high/low, 2 extreme
  std::uint8_t volt_status;      // 0 normal, 1 high, 2 extremely high
  std::uint8_t motor_status;     // 0 normal, 1 warning, 2 error
  std::uint8_t dirty_warn;       // 0 clean, 1 dirty/blocked
  bool firmware_err;
  bool pps_ok;                   // 1 = PPS signal is OK
  bool device_lifetime_warn;
  bool fan_warn;
  bool self_heating;
  bool ptp_ok;                   // 1 = 1588 signal is OK
  // 0 not started, 1 PTP, 2 GPS, 3 PPS, 4 abnormal (highest-priority source lost)
  std::uint8_t time_sync_status;
  std::uint8_t system_status;    // 0 normal, 1 warning, 2 error
};

ErrorCode decode_error_code(std::uint32_t err_code) noexcept;
const char* to_string_time_sync_status(std::uint8_t s) noexcept;

// --- tag semantics ([M] §5.2, "Tags") ---------------------------------------
//
// DIFFERENT BIT LAYOUT FROM THE MID-360. On the Mid-70:
//   bits 1:0  spatial-position noise   (00 normal, 01 high-confidence noise,
//                                        10 moderate, 11 low)
//   bits 3:2  intensity noise          (00 normal, 01 noise — rain/dust/fog)
//   bits 5:4  return number            (00 return 0, 01 return 1, 10 return 2)
//   bits 7:6  nearby-waveform distortion (00 normal, 01 distorted)
inline constexpr std::uint8_t kTagSpatialNoiseMask = 0x03;
inline constexpr std::uint8_t kTagIntensityNoiseMask = 0x0C;
inline constexpr std::uint8_t kTagReturnNumberMask = 0x30;
inline constexpr std::uint8_t kTagDistortionMask = 0xC0;

inline constexpr std::uint8_t tag_spatial_noise(std::uint8_t tag) {
  return static_cast<std::uint8_t>(tag & kTagSpatialNoiseMask);
}
inline constexpr std::uint8_t tag_intensity_noise(std::uint8_t tag) {
  return static_cast<std::uint8_t>((tag & kTagIntensityNoiseMask) >> 2);
}
inline constexpr std::uint8_t tag_return_number(std::uint8_t tag) {
  return static_cast<std::uint8_t>((tag & kTagReturnNumberMask) >> 4);
}
inline constexpr bool tag_distorted(std::uint8_t tag) {
  return (tag & kTagDistortionMask) != 0;
}

// --- point filtering ------------------------------------------------------
//
// One canonical point for the filter: millimetres + reflectivity + tag.
// Every wire type is widened to this (spherical is converted, non-extended
// types get tag = 0), so the filter has one code path.
struct Point {
  std::int32_t x = 0, y = 0, z = 0;  // mm
  std::uint8_t reflectivity = 0;
  std::uint8_t tag = 0;
};

// Defaults, and where they come from:
//
//   drop_no_return = true
//       A no-return is x == y == z == 0 ([M] §5.2: "the coordinates of the
//       point cloud will be expressed as (0, 0, 0)"). Same failure as the
//       Mid-360: letting these through piles a large fraction of every
//       frame onto the sensor origin.
//
//   tag_reject_mask = kTagSpatialNoiseMask | kTagDistortionMask (0xC3)
//       Spatial-noise and waveform-distortion points are geometrically
//       unreliable per [M]; intensity-noise points (rain/dust) are kept by
//       default — their geometry is usually fine. Non-extended types carry
//       no tag and are never rejected on it.
//
//   min_range_m = 0.1
//       [M] §1: blind zone 0.05 m, "detection precision cannot be
//       guaranteed" inside 0.2 m. 0.1 removes housing self-hits without
//       discarding the near field. The FAST-LIO work measured a 0.5 m gate
//       discarding 94% of a tight-scene frame; that is the odometry's
//       decision (LioConfig::min_range_m), not the driver's.
struct PointFilterConfig {
  bool drop_no_return = true;
  std::uint8_t tag_reject_mask = static_cast<std::uint8_t>(kTagSpatialNoiseMask | kTagDistortionMask);
  float min_range_m = 0.1f;
  float max_range_m = 0.0f;      // 0 = unbounded ([M]: 260 m at 80% reflectivity)
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

bool point_passes(const Point& p, const PointFilterConfig& cfg, FilterStats* stats);

// Spherical → Cartesian, millimetres, [M] §5.2 Fig 5.2.1.1:
//   x = r sinθ cosφ, y = r sinθ sinφ, z = r cosθ  (θ zenith, φ azimuth)
Point from_spherical(std::uint32_t depth_mm, std::uint16_t theta_001deg,
                     std::uint16_t phi_001deg, std::uint8_t reflectivity, std::uint8_t tag);

// --- packet validation ----------------------------------------------------

struct PacketView {
  const EthHeader* header = nullptr;
  const std::uint8_t* payload = nullptr;
  std::size_t payload_bytes = 0;
  std::uint32_t point_count = 0;   // payload_bytes / per_point
  std::uint32_t per_point_bytes = 0;

  bool valid() const { return header != nullptr; }
};

// Validate a received datagram and describe it. With no length field and no
// counter, the checks are: at least a header, a data_type this file knows,
// and a payload that is a whole number of points of that type. A datagram
// that fails is counted as bad by the driver and recorded verbatim by the
// raw sink — never decoded.
PacketView parse_packet(const std::uint8_t* data, std::size_t len);

// Decode the header timestamp according to timestamp_type.
//   NoSync / PTP / PPS : little-endian u64 nanoseconds, as sent.
//   PPS+GPS            : packed UTC ([S] LivoxTimestampPpsGps: year-2000,
//                        month, day, hour, then u32 microseconds within the
//                        hour) → nanoseconds since the Unix epoch, via
//                        timegm (no timezone — the ROS driver had this right).
// Returns false for kTimestampRsvd/kTimestampUnknown or a UTC that does not
// form a date.
bool decode_timestamp_ns(const EthHeader& h, std::int64_t* out_ns) noexcept;

// --- loss accounting without a counter ------------------------------------
//
// The only ordering information a Mid-70 datagram carries is its device
// timestamp. Consecutive datagrams of a type are nominally one
// packet-interval apart (100 points at 100k pts/s = 1.000 ms); a gap of
// k intervals means k−1 datagrams did not arrive. Like the Mid-360's
// udp_cnt model this is blind to a full outage — the device keeps stamping
// while the wire is down and the first packet after resume just looks like
// a long gap — so a gap beyond `reset_threshold_ns` is reported as
// unattributable (reset / outage / timestamp mode change) and handed to the
// watchdog, not counted as loss.
class GapTracker {
 public:
  enum class Step : std::uint8_t {
    kFirst = 0,
    kInSequence = 1,
    kDuplicate = 2,        // same or earlier stamp: retransmit, or clock reset to 0
    kLoss = 3,
    kUnattributable = 4,
  };

  // `interval_ns` is the nominal spacing between datagrams of the type in
  // play; the driver derives it from point count and the nominal rate.
  explicit GapTracker(std::int64_t interval_ns = 1000000, std::int64_t reset_threshold_ns = 2000000000);

  Step observe(std::int64_t t_device_ns, std::uint32_t* lost_out = nullptr);
  void reset();
  void set_interval_ns(std::int64_t ns) { interval_ns_ = ns; }

  std::uint64_t packets() const { return packets_; }
  std::uint64_t lost() const { return lost_; }
  std::uint64_t duplicates() const { return duplicates_; }
  std::uint64_t resets() const { return resets_; }
  bool has_previous() const { return have_prev_; }
  std::int64_t interval_ns() const { return interval_ns_; }
  double loss_fraction() const;

 private:
  std::int64_t interval_ns_;
  std::int64_t reset_threshold_ns_;
  bool have_prev_ = false;
  std::int64_t prev_ = 0;
  std::uint64_t packets_ = 0;
  std::uint64_t lost_ = 0;
  std::uint64_t duplicates_ = 0;
  std::uint64_t resets_ = 0;
};

}  // namespace mid70
}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_MID70_MID70_PACKETS_H
