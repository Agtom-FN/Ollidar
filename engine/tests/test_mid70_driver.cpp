// Mid-70 driver + wire layer. NO LIVOX SDK IS LINKED BY THIS FILE.
//
// Same contract as tests/test_mid360_driver.cpp: the SDK is fetched, not
// committed, so every CI leg that never runs fetch_sdk1.sh must still prove
// the parts that can actually be wrong. For a Mid-70 those are different
// parts than for a Mid-360, and this file is organised around the three
// things mid70_packets.h warns about:
//
//   • no length field and NO sequence counter — validity is "the payload
//     divides evenly into points of the declared type", and loss is inferred
//     from device-timestamp gaps;
//   • the timestamp's MEANING depends on timestamp_type (raw nanoseconds vs
//     a packed UTC date), which is the bug that publishes stamps wrong by
//     the lidar's uptime;
//   • there is no IMU, so a kDataTypeImu datagram is a mistake to be counted,
//     not a stream to be routed.
//
// Every datagram here is assembled with std::memcpy out of the mid70 structs.
// Nothing casts a byte buffer to a packed struct and writes through it: the
// buffers are heap vectors with no alignment guarantee, and the decoder under
// test does not assume one either.
//
// The reconnect tests drive Mid70Driver::tick() with a scripted clock rather
// than sleeping, so they assert the state machine itself instead of racing it.
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/drivers/mid70/mid70_driver.h"
#include "scanengine/drivers/mid70/mid70_packets.h"

using namespace scanengine;
using namespace scanengine::mid70;

namespace {

// --- a scripted clock -----------------------------------------------------
//
// ClockFn is a plain function pointer (so replay is reproducible and the C
// ABI can carry it), hence the file-scope current time.
std::int64_t g70_now_ns = 0;
TimePoint fake_clock() { return TimePoint{g70_now_ns}; }
void advance_ms(std::int64_t ms) { g70_now_ns += ms * 1000000LL; }

// --- synthetic datagrams --------------------------------------------------

// The 18-byte header, built as a struct and copied in. `ts_raw` is written
// verbatim into the 8 timestamp bytes, which is what a NoSync/PTP/PPS device
// sends; the PPS+GPS tests overwrite those bytes by hand.
std::vector<std::uint8_t> make_header(std::uint8_t data_type, std::uint8_t ts_type,
                                      std::uint64_t ts_raw, std::uint32_t err_code = 0) {
  EthHeader h{};
  h.version = 5;
  h.slot = 0;
  h.id = 1;
  h.rsvd = 0;
  h.err_code = err_code;
  h.timestamp_type = ts_type;
  h.data_type = data_type;
  std::memcpy(h.timestamp, &ts_raw, sizeof(ts_raw));

  std::vector<std::uint8_t> b(sizeof(EthHeader));
  std::memcpy(b.data(), &h, sizeof(h));
  return b;
}

template <typename P>
void put_point(std::vector<std::uint8_t>& b, std::uint32_t i, const P& p) {
  std::memcpy(b.data() + sizeof(EthHeader) + static_cast<std::size_t>(i) * sizeof(P), &p,
              sizeof(p));
}

// The canonical 1362-byte datagram: what current Mid-70 firmware sends after
// SetCartesianCoordinate(). Point i sits at (1000 + i, 2000, −500) mm, so a
// metric conversion error is visible in the first decimal place.
std::vector<std::uint8_t> make_extend_packet(std::int64_t t_dev_ns,
                                             std::uint32_t n = kPointsPerPacketExtend,
                                             std::uint32_t err_code = 0,
                                             std::uint8_t ts_type = kTimestampNoSync) {
  std::vector<std::uint8_t> b =
      make_header(kDataTypeExtendCartesian, ts_type, static_cast<std::uint64_t>(t_dev_ns),
                  err_code);
  b.resize(sizeof(EthHeader) + static_cast<std::size_t>(n) * sizeof(ExtendRawPoint), 0);
  for (std::uint32_t i = 0; i < n; ++i) {
    ExtendRawPoint p{};
    p.x = 1000 + static_cast<std::int32_t>(i);
    p.y = 2000;
    p.z = -500;
    p.reflectivity = 100;
    p.tag = 0;
    put_point(b, i, p);
  }
  return b;
}

ExtendRawPoint get_extend_point(const std::vector<std::uint8_t>& b, std::uint32_t i) {
  ExtendRawPoint p{};
  std::memcpy(&p, b.data() + sizeof(EthHeader) + static_cast<std::size_t>(i) * sizeof(p),
              sizeof(p));
  return p;
}

// data_type 0: 13-byte points, 100 per datagram.
std::vector<std::uint8_t> make_cartesian_packet(std::int64_t t_dev_ns,
                                                std::uint32_t n = kPointsPerPacketCartesian) {
  std::vector<std::uint8_t> b =
      make_header(kDataTypeCartesian, kTimestampNoSync, static_cast<std::uint64_t>(t_dev_ns));
  b.resize(sizeof(EthHeader) + static_cast<std::size_t>(n) * sizeof(RawPoint), 0);
  for (std::uint32_t i = 0; i < n; ++i) {
    RawPoint p{};
    p.x = 3000;
    p.y = 0;
    p.z = 0;
    p.reflectivity = 77;
    put_point(b, i, p);
  }
  return b;
}

// data_type 4: two returns per sample, 48 samples per datagram.
std::vector<std::uint8_t> make_dual_packet(std::int64_t t_dev_ns,
                                           std::uint32_t n = kPointsPerPacketDual) {
  std::vector<std::uint8_t> b = make_header(kDataTypeDualExtendCartesian, kTimestampNoSync,
                                            static_cast<std::uint64_t>(t_dev_ns));
  b.resize(sizeof(EthHeader) + static_cast<std::size_t>(n) * sizeof(DualExtendRawPoint), 0);
  for (std::uint32_t i = 0; i < n; ++i) {
    DualExtendRawPoint p{};
    p.x1 = 4000;
    p.y1 = 0;
    p.z1 = 0;
    p.reflectivity1 = 50;
    p.tag1 = 0x00;  // return 0
    p.x2 = 9000;
    p.y2 = 0;
    p.z2 = 0;
    p.reflectivity2 = 20;
    p.tag2 = 0x10;  // return 1
    put_point(b, i, p);
  }
  return b;
}

// data_type 6 — a Horizon/Avia on a Mid-70's wire. Never valid geometry.
std::vector<std::uint8_t> make_imu_packet() {
  std::vector<std::uint8_t> b =
      make_header(kDataTypeImu, kTimestampNoSync, 5000000000ULL);
  b.resize(sizeof(EthHeader) + sizeof(ImuPoint), 0);
  ImuPoint p{};
  p.gyro_x = 0.25f;
  p.acc_z = 1.0f;
  put_point(b, 0, p);
  return b;
}

// --- a driver wired to a real PageStore + EventBus ------------------------

struct Rig {
  EventBus bus;
  PageStore points;
  DriverContext ctx;

  Rig() : points(PageStoreConfig{4096, 64}) {
    ctx.bus = &bus;
    ctx.points = &points;
    ctx.clock = &fake_clock;
  }
};

Mid70Config inject_config() {
  Mid70Config c;
  c.backend = Mid70Backend::kInject;
  c.internal_supervisor_thread = false;  // tests drive tick() themselves
  c.live_points_per_sec = 0;             // no decimation: count every point
  c.max_batch_points = 96;               // one packet per flush, easy to reason about
  return c;
}

// One extended datagram's nominal spacing: 96 points at 100k pts/s.
constexpr std::int64_t kExtendIntervalNs = 960000;

}  // namespace

// ===========================================================================
// Wire layout — the [S]/[M] cross-check, machine-checked.
// ===========================================================================

TEST_CASE("mid70/wire_layout_matches_the_sdk_v1_definition") {
  CHECK(sizeof(EthHeader) == 18);
  CHECK(sizeof(RawPoint) == 13);
  CHECK(sizeof(SpherPoint) == 9);
  CHECK(sizeof(ExtendRawPoint) == 14);
  CHECK(sizeof(ExtendSpherPoint) == 10);
  CHECK(sizeof(DualExtendRawPoint) == 28);
  CHECK(sizeof(ImuPoint) == 24);
  // The two offsets a decoder gets wrong when it copies the Mid-360's
  // 36-byte header shape by reflex.
  CHECK(offsetof(EthHeader, err_code) == 4);
  CHECK(offsetof(EthHeader, timestamp_type) == 8);
  CHECK(offsetof(EthHeader, data_type) == 9);
  CHECK(offsetof(EthHeader, timestamp) == 10);

  CHECK(kPacketBytesCartesian == 1318);
  CHECK(kPacketBytesExtendCartesian == 1362);
  CHECK(kPointsPerPacketCartesian == 100);
  CHECK(kPointsPerPacketExtend == 96);
  CHECK(kPointsPerPacketDual == 48);

  // SDK v1 ports: the host LISTENS on 55000 for the device's broadcast; the
  // device listens for commands on 65000. Nothing here is a Mid-360 port.
  CHECK(kBroadcastPort == 55000);
  CHECK(kLidarCmdPort == 65000);
}

TEST_CASE("mid70/parse_accepts_and_rejects") {
  const std::vector<std::uint8_t> good = make_extend_packet(0);
  CHECK(good.size() == kPacketBytesExtendCartesian);
  const PacketView v = parse_packet(good.data(), good.size());
  REQUIRE(v.valid());
  CHECK(v.point_count == 96);
  CHECK(v.per_point_bytes == 14);
  CHECK(v.payload_bytes == 96 * sizeof(ExtendRawPoint));

  // Shorter than the header.
  CHECK_FALSE(parse_packet(good.data(), 10).valid());
  CHECK_FALSE(parse_packet(good.data(), sizeof(EthHeader) - 1).valid());
  // Header only: no points is not a point packet.
  CHECK_FALSE(parse_packet(good.data(), sizeof(EthHeader)).valid());
  // WITH NO LENGTH FIELD this is the only structural check there is: a
  // payload that is not a whole number of points. One byte short of 96
  // extended points leaves 1343 bytes, and 1343 % 14 != 0.
  CHECK_FALSE(parse_packet(good.data(), good.size() - 1).valid());
  // A data_type this file has no layout for — including the dual-spherical
  // type (5) the driver never requests.
  std::vector<std::uint8_t> odd = good;
  odd[offsetof(EthHeader, data_type)] = 5;
  CHECK_FALSE(parse_packet(odd.data(), odd.size()).valid());
  odd[offsetof(EthHeader, data_type)] = 9;
  CHECK_FALSE(parse_packet(odd.data(), odd.size()).valid());

  // More points than the type can carry in one datagram: 200 cartesian
  // points divides evenly by 13 but is twice what the device sends, so the
  // layout is not the one we think it is.
  const std::vector<std::uint8_t> too_many = make_cartesian_packet(0, 200);
  CHECK_FALSE(parse_packet(too_many.data(), too_many.size()).valid());

  // Every type we do decode.
  const std::vector<std::uint8_t> cart = make_cartesian_packet(0);
  CHECK(cart.size() == kPacketBytesCartesian);
  CHECK(parse_packet(cart.data(), cart.size()).point_count == 100);
  const std::vector<std::uint8_t> dual = make_dual_packet(0);
  CHECK(parse_packet(dual.data(), dual.size()).point_count == 48);

  // An IMU datagram PARSES — it is a well-formed SDK v1 packet. Whether it
  // belongs on a Mid-70's wire is the driver's judgement, not the parser's.
  const std::vector<std::uint8_t> imu = make_imu_packet();
  const PacketView iv = parse_packet(imu.data(), imu.size());
  REQUIRE(iv.valid());
  CHECK(iv.header->data_type == kDataTypeImu);
  CHECK(iv.point_count == 1);
}

// ===========================================================================
// Timestamps — the field whose MEANING depends on another field.
// ===========================================================================

TEST_CASE("mid70/timestamp_nosync_ptp_and_pps_are_raw_nanoseconds") {
  // A real measurement from the FAST-LIO work: 25,566 s of uptime. Reading
  // this as an epoch time puts the cloud in 1970.
  const std::uint64_t uptime_ns = 25566ULL * 1000000000ULL + 123456789ULL;
  for (std::uint8_t type : {kTimestampNoSync, kTimestampPtp, kTimestampPps}) {
    const std::vector<std::uint8_t> b =
        make_header(kDataTypeExtendCartesian, type, uptime_ns);
    EthHeader h{};
    std::memcpy(&h, b.data(), sizeof(h));
    std::int64_t ns = -1;
    CHECK(decode_timestamp_ns(h, &ns));
    CHECK(ns == static_cast<std::int64_t>(uptime_ns));
  }

  // Reserved and unknown decode to nothing at all. The driver must fall back
  // to arrival time rather than publish a number it cannot justify.
  for (std::uint8_t type : {kTimestampRsvd, kTimestampUnknown, static_cast<std::uint8_t>(200)}) {
    const std::vector<std::uint8_t> b = make_header(kDataTypeExtendCartesian, type, uptime_ns);
    EthHeader h{};
    std::memcpy(&h, b.data(), sizeof(h));
    std::int64_t ns = -1;
    CHECK_FALSE(decode_timestamp_ns(h, &ns));
  }
}

TEST_CASE("mid70/timestamp_pps_gps_is_a_packed_utc_date") {
  // [S] LivoxTimestampPpsGps: year-since-2000, month, day, hour, then u32
  // microseconds INSIDE that hour. UTC, so timegm — the local-time trap here
  // costs a whole timezone offset and is invisible in Greenwich.
  std::vector<std::uint8_t> b =
      make_header(kDataTypeExtendCartesian, kTimestampPpsGps, 0);
  b[offsetof(EthHeader, timestamp) + 0] = 26;  // 2026
  b[offsetof(EthHeader, timestamp) + 1] = 3;   // March
  b[offsetof(EthHeader, timestamp) + 2] = 14;
  b[offsetof(EthHeader, timestamp) + 3] = 9;   // 09:00 UTC
  const std::uint32_t us_in_hour = 1234567u;   // 00:01.234567 into the hour
  std::memcpy(b.data() + offsetof(EthHeader, timestamp) + 4, &us_in_hour, sizeof(us_in_hour));

  EthHeader h{};
  std::memcpy(&h, b.data(), sizeof(h));
  std::int64_t ns = 0;
  REQUIRE(decode_timestamp_ns(h, &ns));

  std::tm expect{};
  expect.tm_year = 2026 - 1900;
  expect.tm_mon = 2;  // March, 0-based
  expect.tm_mday = 14;
  expect.tm_hour = 9;
  expect.tm_isdst = 0;
  const std::int64_t epoch_s = static_cast<std::int64_t>(timegm(&expect));
  CHECK(ns == (epoch_s * 1000000LL + us_in_hour) * 1000LL);
  // Sanity on the magnitude: this is an epoch time, not an uptime.
  CHECK(ns > 1700000000LL * 1000000000LL);

  // A date the calendar does not contain is refused rather than normalised.
  b[offsetof(EthHeader, timestamp) + 1] = 13;  // month 13
  std::memcpy(&h, b.data(), sizeof(h));
  CHECK_FALSE(decode_timestamp_ns(h, &ns));
  b[offsetof(EthHeader, timestamp) + 1] = 3;
  b[offsetof(EthHeader, timestamp) + 3] = 24;  // hour 24
  std::memcpy(&h, b.data(), sizeof(h));
  CHECK_FALSE(decode_timestamp_ns(h, &ns));
}

TEST_CASE("mid70/timestamp_type_names") {
  CHECK(std::string(to_string_timestamp_type(kTimestampNoSync)) == "nosync");
  CHECK(std::string(to_string_timestamp_type(kTimestampPtp)) == "ptp");
  CHECK(std::string(to_string_timestamp_type(kTimestampPpsGps)) == "pps+gps");
  CHECK(std::string(to_string_timestamp_type(kTimestampPps)) == "pps");
}

// ===========================================================================
// err_code — the device's own opinion of its clock, in every packet.
// ===========================================================================

TEST_CASE("mid70/error_code_bit_positions") {
  // Each field on its own, at the bit position [S] puts it.
  CHECK(decode_error_code(0x2u).temp_status == 2);
  CHECK(decode_error_code(0x1u << 2).volt_status == 1);
  CHECK(decode_error_code(0x2u << 4).motor_status == 2);
  CHECK(decode_error_code(0x1u << 6).dirty_warn == 1);
  CHECK(decode_error_code(0x1u << 8).firmware_err);
  CHECK(decode_error_code(0x1u << 9).pps_ok);
  CHECK(decode_error_code(0x1u << 10).device_lifetime_warn);
  CHECK(decode_error_code(0x1u << 11).fan_warn);
  CHECK(decode_error_code(0x1u << 12).self_heating);
  CHECK(decode_error_code(0x1u << 13).ptp_ok);
  CHECK(decode_error_code(0x2u << 14).time_sync_status == 2);  // GPS
  CHECK(decode_error_code(0x2u << 30).system_status == 2);

  // An all-clear word says nothing is wrong anywhere.
  const ErrorCode ok = decode_error_code(0u);
  CHECK(ok.temp_status == 0);
  CHECK_FALSE(ok.firmware_err);
  CHECK_FALSE(ok.pps_ok);
  CHECK(ok.time_sync_status == 0);
  CHECK(ok.system_status == 0);

  // ...and the fields do not leak into each other when several are set.
  const std::uint32_t word = 0x1u |          // temp warning
                             (0x1u << 9) |   // PPS OK
                             (0x3u << 14) |  // synced from PPS
                             (0x1u << 30);   // system warning
  const ErrorCode c = decode_error_code(word);
  CHECK(c.temp_status == 1);
  CHECK(c.volt_status == 0);
  CHECK(c.motor_status == 0);
  CHECK(c.pps_ok);
  CHECK_FALSE(c.ptp_ok);
  CHECK(c.time_sync_status == 3);
  CHECK(c.system_status == 1);
  CHECK(std::string(to_string_time_sync_status(3)) == "pps");
  CHECK(std::string(to_string_time_sync_status(4)) == "abnormal");
}

// ===========================================================================
// Tags — a DIFFERENT bit layout from the Mid-360's.
// ===========================================================================

TEST_CASE("mid70/tag_accessors_use_the_mid70_layout") {
  CHECK(tag_spatial_noise(0x03) == 3);     // bits 1:0
  CHECK(tag_intensity_noise(0x0C) == 3);   // bits 3:2
  CHECK(tag_return_number(0x30) == 3);     // bits 5:4
  CHECK(tag_distorted(0x40));              // bits 7:6
  CHECK(tag_distorted(0xC0));

  // Each field reads zero when only the others are set — the cross-leak that
  // makes a Mid-360 tag decoder silently reject good Mid-70 points.
  CHECK(tag_spatial_noise(0xFC) == 0);
  CHECK(tag_intensity_noise(0xF3) == 0);
  CHECK(tag_return_number(0xCF) == 0);
  CHECK_FALSE(tag_distorted(0x3F));

  CHECK(tag_return_number(0x10) == 1);     // second return
  CHECK(tag_intensity_noise(0x04) == 1);   // rain/dust/fog
}

// ===========================================================================
// The point filter.
// ===========================================================================

TEST_CASE("mid70/filter_defaults") {
  PointFilterConfig cfg;
  CHECK(cfg.drop_no_return);
  CHECK(cfg.tag_reject_mask == (kTagSpatialNoiseMask | kTagDistortionMask));
  CHECK(cfg.min_range_m == doctest::Approx(0.1f));

  FilterStats st;
  const Point good{1000, 2000, -500, 100, 0};
  CHECK(point_passes(good, cfg, &st));

  // A no-return is (0,0,0) [M] §5.2. Letting these through piles a large
  // fraction of every frame onto the sensor origin.
  CHECK_FALSE(point_passes(Point{0, 0, 0, 0, 0}, cfg, &st));
  CHECK(st.dropped_no_return == 1);

  // Spatial-position noise and nearby-waveform distortion are geometrically
  // unreliable, so both are rejected by default...
  CHECK_FALSE(point_passes(Point{1000, 0, 0, 100, 0x01}, cfg, &st));
  CHECK_FALSE(point_passes(Point{1000, 0, 0, 100, 0x02}, cfg, &st));
  CHECK_FALSE(point_passes(Point{1000, 0, 0, 100, 0x40}, cfg, &st));
  CHECK(st.dropped_tag == 3);

  // ...and intensity noise (rain, dust, fog) is deliberately KEPT: its
  // geometry is usually fine, only its reflectivity is not. Return-number
  // bits are never a rejection reason either.
  CHECK(point_passes(Point{1000, 0, 0, 100, 0x04}, cfg, &st));
  CHECK(point_passes(Point{1000, 0, 0, 100, 0x08}, cfg, &st));
  CHECK(point_passes(Point{1000, 0, 0, 100, 0x10}, cfg, &st));

  // 0.1 m removes housing self-hits without discarding the near field.
  CHECK_FALSE(point_passes(Point{50, 0, 0, 100, 0}, cfg, &st));
  CHECK(point_passes(Point{150, 0, 0, 100, 0}, cfg, &st));
  CHECK(st.dropped_range == 1);

  CHECK(st.seen == 10);
  CHECK(st.kept == 5);
  CHECK(st.keep_fraction() == doctest::Approx(0.5));

  // A diagnostic / post-processing run can ask for everything.
  PointFilterConfig raw;
  raw.drop_no_return = false;
  raw.tag_reject_mask = 0;
  raw.min_range_m = 0.f;
  CHECK(point_passes(Point{0, 0, 0, 0, 0xFF}, raw, nullptr));
}

TEST_CASE("mid70/filter_range_and_reflectivity_are_configurable") {
  PointFilterConfig cfg;
  cfg.min_range_m = 0.5f;
  cfg.max_range_m = 10.0f;
  cfg.min_reflectivity = 20;
  FilterStats st;

  CHECK_FALSE(point_passes(Point{100, 0, 0, 200, 0}, cfg, &st));    // 0.1 m
  CHECK_FALSE(point_passes(Point{20000, 0, 0, 200, 0}, cfg, &st));  // 20 m
  CHECK_FALSE(point_passes(Point{2000, 0, 0, 5, 0}, cfg, &st));     // too dim
  CHECK(point_passes(Point{2000, 0, 0, 200, 0}, cfg, &st));
  CHECK(st.dropped_range == 2);
  CHECK(st.dropped_reflectivity == 1);
  CHECK(st.kept == 1);
}

TEST_CASE("mid70/from_spherical_matches_the_manual_formula") {
  // [M] §5.2 Fig 5.2.1.1: x = r sinθ cosφ, y = r sinθ sinφ, z = r cosθ, with
  // θ the ZENITH angle (not elevation — swapping them mirrors the cloud
  // through the horizontal plane and still looks plausible on screen).
  const Point p = from_spherical(5000, 6000, 3000, 88, 0x10);
  CHECK(p.x == doctest::Approx(3750).epsilon(0.001));  // 5000·sin60·cos30
  CHECK(p.y == doctest::Approx(2165).epsilon(0.001));  // 5000·sin60·sin30
  CHECK(p.z == doctest::Approx(2500).epsilon(0.001));  // 5000·cos60
  CHECK(p.reflectivity == 88);
  CHECK(p.tag == 0x10);  // the tag rides through untouched

  // Straight up the z axis: θ = 0.
  const Point up = from_spherical(1234, 0, 0, 1, 0);
  CHECK(up.x == 0);
  CHECK(up.y == 0);
  CHECK(up.z == 1234);

  // Round trip: cartesian → spherical → cartesian, to the millimetre the
  // wire quantises to.
  const double x = 1234.0, y = -2345.0, z = 3456.0;
  const double r = std::sqrt(x * x + y * y + z * z);
  const double theta_deg = std::acos(z / r) * 180.0 / 3.14159265358979323846;
  double phi_deg = std::atan2(y, x) * 180.0 / 3.14159265358979323846;
  if (phi_deg < 0.0) phi_deg += 360.0;
  const Point back = from_spherical(static_cast<std::uint32_t>(std::lround(r)),
                                    static_cast<std::uint16_t>(std::lround(theta_deg * 100.0)),
                                    static_cast<std::uint16_t>(std::lround(phi_deg * 100.0)), 0,
                                    0);
  CHECK(back.x == doctest::Approx(x).epsilon(0.002));
  CHECK(back.y == doctest::Approx(y).epsilon(0.002));
  CHECK(back.z == doctest::Approx(z).epsilon(0.002));
}

// ===========================================================================
// Loss with no counter: the timestamp-gap model.
// ===========================================================================

TEST_CASE("mid70/gap_tracker_counts_missing_intervals") {
  GapTracker t(1000000);  // 1.000 ms = 100 cartesian points at 100k pts/s
  CHECK(t.interval_ns() == 1000000);
  CHECK_FALSE(t.has_previous());

  std::uint32_t lost = 0;
  CHECK(t.observe(10000000, &lost) == GapTracker::Step::kFirst);
  CHECK(lost == 0);
  CHECK(t.has_previous());

  CHECK(t.observe(11000000, &lost) == GapTracker::Step::kInSequence);
  CHECK(lost == 0);
  // A little jitter is still one interval — the device's own is well under
  // half of one, so 1.5 intervals is the boundary.
  CHECK(t.observe(12400000, &lost) == GapTracker::Step::kInSequence);
  CHECK(lost == 0);

  // One datagram did not arrive: a two-interval gap.
  CHECK(t.observe(14400000, &lost) == GapTracker::Step::kLoss);
  CHECK(lost == 1);
  CHECK(t.lost() == 1);

  // Three did.
  CHECK(t.observe(18400000, &lost) == GapTracker::Step::kLoss);
  CHECK(lost == 3);
  CHECK(t.lost() == 4);

  CHECK(t.packets() == 5);
}

TEST_CASE("mid70/gap_tracker_duplicate_and_backwards_stamps_are_not_loss") {
  GapTracker t(1000000);
  std::uint32_t lost = 0;
  CHECK(t.observe(10000000, &lost) == GapTracker::Step::kFirst);
  // The same stamp again: a retransmit, or a replay looping.
  CHECK(t.observe(10000000, &lost) == GapTracker::Step::kDuplicate);
  CHECK(lost == 0);
  // Backwards: the device clock reset (power-cycle in NoSync mode, or a PPS
  // edge resetting the counter — [M] §5.2.2). Never negative loss.
  CHECK(t.observe(500000, &lost) == GapTracker::Step::kDuplicate);
  CHECK(lost == 0);
  CHECK(t.duplicates() == 2);
  CHECK(t.lost() == 0);
  // ...and it keeps counting from the new base.
  CHECK(t.observe(1500000, &lost) == GapTracker::Step::kInSequence);
}

TEST_CASE("mid70/gap_tracker_calls_a_long_gap_unattributable") {
  // The model's blind spot, stated rather than hidden: the device keeps
  // stamping while the wire is down, so the first packet after a real outage
  // shows a gap of the whole outage. Calling that "3,125 packets lost" would
  // be a fabrication; the watchdog is what sees an outage.
  GapTracker t(1000000, 2000000000);
  std::uint32_t lost = 0;
  CHECK(t.observe(0, &lost) == GapTracker::Step::kFirst);
  CHECK(t.observe(1999999999, &lost) == GapTracker::Step::kLoss);  // just under
  CHECK(lost == 1999);
  t.reset();

  CHECK(t.observe(0, &lost) == GapTracker::Step::kFirst);
  CHECK(t.observe(2000000000, &lost) == GapTracker::Step::kUnattributable);  // at the line
  CHECK(lost == 0);
  CHECK(t.lost() == 0);
  CHECK(t.resets() == 1);
  CHECK(t.observe(2001000000, &lost) == GapTracker::Step::kInSequence);
}

TEST_CASE("mid70/gap_tracker_loss_fraction") {
  // Drop every 50th datagram out of 500, one interval apart.
  GapTracker t(1000000);
  std::uint64_t sent = 0;
  for (int i = 0; i < 500; ++i) {
    if (i % 50 == 49) continue;  // this one never arrives
    (void)t.observe(static_cast<std::int64_t>(i) * 1000000LL);
    ++sent;
  }
  CHECK(t.packets() == sent);
  CHECK(sent == 490);
  // Nine gaps are observable; the tenth drop is the last datagram of the run
  // and has no successor to reveal it.
  CHECK(t.lost() == 9);
  CHECK(t.loss_fraction() == doctest::Approx(9.0 / 499.0).epsilon(0.001));

  t.reset();
  CHECK(t.packets() == 0);
  CHECK(t.lost() == 0);
  CHECK_FALSE(t.has_previous());
  CHECK(t.loss_fraction() == doctest::Approx(0.0));
}

// ===========================================================================
// The driver: decode → PageStore.
// ===========================================================================

TEST_CASE("mid70/driver_converts_mm_to_metres_and_fills_the_page_store") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());
  CHECK(d.state() == DeviceState::kStarting);

  const std::vector<std::uint8_t> p = make_extend_packet(1000000);
  d.on_point_packet(p.data(), p.size(), fake_clock());

  CHECK(d.state() == DeviceState::kStreaming);  // first data promotes
  const Mid70Stats st = d.stats();
  CHECK(st.point_packets == 1);
  CHECK(st.points_received == 96);
  CHECK(st.points_kept == 96);
  CHECK(st.points_appended == 96);
  CHECK(st.bad_packets == 0);
  CHECK(st.unexpected_imu_packets == 0);

  const auto ids = rig.points.page_ids();
  REQUIRE(ids.size() == 1);
  const PageView v = rig.points.page_view(ids[0]);
  REQUIRE(v.valid());
  CHECK(v.stream == StreamId::kLidarMid70);  // provenance survives to export
  CHECK(v.count == 96);
  CHECK(v.data[0].x == doctest::Approx(1.0f));
  CHECK(v.data[0].y == doctest::Approx(2.0f));
  CHECK(v.data[0].z == doctest::Approx(-0.5f));
  CHECK(v.data[95].x == doctest::Approx(1.095f));
  CHECK(v.data[0].r == 100);  // reflectivity carried as greyscale
  CHECK(v.data[0].g == 100);
  CHECK(v.data[0].b == 100);
  CHECK(v.data[0].a == 255);

  // bytes_in is MEASURED, not points × a constant.
  CHECK(d.health().bytes_in == kPacketBytesExtendCartesian);
  CHECK(d.health().kind == DeviceKind::kMid70);
  CHECK(d.health().rotation_hz == doctest::Approx(0.0));  // no revolutions, no IMU
  CHECK(d.health().points_out == 96);

  CHECK(d.stop().ok());
}

TEST_CASE("mid70/driver_decodes_every_supported_data_type") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg = inject_config();
  cfg.max_batch_points = 8192;
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  // data_type 0: 100 points, no tag byte at all.
  const std::vector<std::uint8_t> cart = make_cartesian_packet(1000000);
  d.on_point_packet(cart.data(), cart.size(), fake_clock());
  CHECK(d.stats().points_received == 100);
  CHECK(d.stats().points_kept == 100);

  // data_type 4: two returns per sample, both real geometry.
  const std::vector<std::uint8_t> dual = make_dual_packet(2000000);
  d.on_point_packet(dual.data(), dual.size(), fake_clock());
  CHECK(d.stats().points_received == 100 + 96);
  CHECK(d.stats().points_kept == 100 + 96);

  CHECK(d.stop().ok());
  CHECK(d.stats().points_appended == 196);
  CHECK(rig.points.total_points() == 196);

  // The near return at 4 m and the far return at 9 m both landed.
  const auto ids = rig.points.page_ids();
  REQUIRE(ids.size() == 1);
  const PageView v = rig.points.page_view(ids[0]);
  REQUIRE(v.valid());
  CHECK(v.data[0].x == doctest::Approx(3.0f));    // cartesian packet
  CHECK(v.data[100].x == doctest::Approx(4.0f));  // dual, return 0
  CHECK(v.data[101].x == doctest::Approx(9.0f));  // dual, return 1
}

TEST_CASE("mid70/driver_drops_no_returns_out_of_the_cloud") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());

  std::vector<std::uint8_t> p = make_extend_packet(1000000);
  for (std::uint32_t i = 0; i < 96; ++i) {
    ExtendRawPoint pt = get_extend_point(p, i);
    if (i % 3 == 0) {  // 32 no-returns, the real-data failure mode
      pt.x = 0;
      pt.y = 0;
      pt.z = 0;
    }
    if (i == 1) pt.tag = 0x01;  // one high-confidence spatial-noise flag
    if (i == 2) pt.tag = 0x40;  // one distorted-waveform flag
    if (i == 4) pt.tag = 0x04;  // intensity noise — KEPT by default
    put_point(p, i, pt);
  }
  d.on_point_packet(p.data(), p.size(), fake_clock());

  const Mid70Stats st = d.stats();
  CHECK(st.points_received == 96);
  CHECK(st.filter.dropped_no_return == 32);
  CHECK(st.filter.dropped_tag == 2);  // 0x01 and 0x40; 0x04 survived
  CHECK(st.points_kept == 62);
  // 62 survivors is under the 96-point batch threshold, so nothing has
  // reached the store yet — batching is what keeps ~1,000 packets a second
  // from becoming ~1,000 PageStore locks.
  CHECK(st.points_appended == 0);
  CHECK(d.stop().ok());  // ...and stop() flushes the remainder.
  CHECK(d.stats().points_appended == 62);
  CHECK(rig.points.total_points() == 62);
}

TEST_CASE("mid70/driver_decimates_deterministically_to_the_live_budget") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg = inject_config();
  cfg.live_points_per_sec = 20000;  // 1-in-5 of the 100k nominal rate
  cfg.max_batch_points = 8192;
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  for (int k = 0; k < 5; ++k) {
    const std::vector<std::uint8_t> p = make_extend_packet(k * kExtendIntervalNs);
    d.on_point_packet(p.data(), p.size(), fake_clock());
  }
  CHECK(d.stats().points_received == 480);
  CHECK(d.stats().points_kept == 480);
  CHECK(d.stop().ok());
  CHECK(d.stats().points_appended == 96);  // exactly 1-in-5, phase-continuous

  // Deterministic, not sampled: the survivors are points 0, 5, 10 … of the
  // filtered sequence, which is what makes a replay reproduce a cloud.
  const auto ids = rig.points.page_ids();
  REQUIRE(ids.size() == 1);
  const PageView v = rig.points.page_view(ids[0]);
  REQUIRE(v.valid());
  CHECK(v.data[0].x == doctest::Approx(1.000f));
  CHECK(v.data[1].x == doctest::Approx(1.005f));
  CHECK(v.data[2].x == doctest::Approx(1.010f));

  // Feeding the identical bytes to a second driver gives the identical cloud.
  Rig rig2;
  Mid70Driver e(2, cfg, rig2.ctx);
  REQUIRE(e.start().ok());
  for (int k = 0; k < 5; ++k) {
    const std::vector<std::uint8_t> p = make_extend_packet(k * kExtendIntervalNs);
    e.on_point_packet(p.data(), p.size(), fake_clock());
  }
  CHECK(e.stop().ok());
  CHECK(e.stats().points_appended == 96);
}

TEST_CASE("mid70/unexpected_imu_datagrams_are_counted_not_decoded") {
  // A Mid-70 HAS NO IMU. kDataTypeImu on this wire means a Horizon or an
  // Avia is plugged into a Mid-70 slot, or the wrong capture is being
  // replayed. It is recorded (the raw sink still sees it) and counted, and it
  // reaches neither the cloud nor the gap tracker — its 200 Hz cadence would
  // manufacture loss against a 1 kHz interval.
  g70_now_ns = 1000000000;
  Rig rig;
  static std::vector<std::size_t> sunk;
  sunk.clear();
  Mid70Config cfg = inject_config();
  cfg.raw_sink = [](const std::uint8_t*, std::size_t n, std::int64_t, void*) {
    sunk.push_back(n);
  };
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  const std::vector<std::uint8_t> imu = make_imu_packet();
  for (int i = 0; i < 5; ++i) d.on_point_packet(imu.data(), imu.size(), fake_clock());

  const Mid70Stats st = d.stats();
  CHECK(st.unexpected_imu_packets == 5);
  CHECK(st.point_packets == 0);
  CHECK(st.bad_packets == 0);
  CHECK(st.points_received == 0);
  CHECK(st.packets_lost == 0);
  CHECK(rig.points.total_points() == 0);
  CHECK(rig.points.page_count() == 0);
  // Recorded regardless: record-always is not conditional on being useful.
  CHECK(sunk.size() == 5);
  CHECK(sunk[0] == sizeof(EthHeader) + sizeof(ImuPoint));
  CHECK(d.health().bytes_in == 5 * (sizeof(EthHeader) + sizeof(ImuPoint)));
  // The device never promoted to streaming: no geometry ever arrived.
  CHECK(d.state() == DeviceState::kStarting);
  CHECK(d.stop().ok());
}

TEST_CASE("mid70/bad_packets_are_counted_not_decoded") {
  g70_now_ns = 1000000000;
  Rig rig;
  static std::vector<std::size_t> sunk;
  sunk.clear();
  Mid70Config cfg = inject_config();
  cfg.raw_sink = [](const std::uint8_t*, std::size_t n, std::int64_t, void*) {
    sunk.push_back(n);
  };
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  const std::vector<std::uint8_t> good = make_extend_packet(1000000);
  d.on_point_packet(good.data(), good.size(), fake_clock());

  const std::uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  d.on_point_packet(garbage, sizeof(garbage), fake_clock());
  // A payload that is not a whole number of points.
  d.on_point_packet(good.data(), good.size() - 1, fake_clock());
  // A type with no layout in this file.
  std::vector<std::uint8_t> odd = good;
  odd[offsetof(EthHeader, data_type)] = 9;
  d.on_point_packet(odd.data(), odd.size(), fake_clock());

  const Mid70Stats st = d.stats();
  CHECK(st.point_packets == 1);
  CHECK(st.bad_packets == 3);
  CHECK(st.points_appended == 96);
  // Every datagram reached the raw sink, valid or not, exactly once — a
  // recording you cannot replay is a recording of nothing.
  CHECK(sunk.size() == 4);
  CHECK(d.health().bytes_in == good.size() + 8 + (good.size() - 1) + odd.size());
  CHECK(d.stop().ok());
}

TEST_CASE("mid70/driver_records_what_the_device_says_about_itself") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());

  const std::uint32_t err = (0x1u << 9) | (0x3u << 14) | 0x1u;  // PPS ok, PPS sync, temp warn
  const std::vector<std::uint8_t> p = make_extend_packet(7654321, kPointsPerPacketExtend, err);
  d.on_point_packet(p.data(), p.size(), fake_clock());

  const Mid70Stats st = d.stats();
  CHECK(st.data_type == kDataTypeExtendCartesian);
  CHECK(st.timestamp_type == kTimestampNoSync);
  CHECK(st.err_code_raw == err);
  CHECK(st.err.pps_ok);
  CHECK(st.err.time_sync_status == 3);
  CHECK(st.err.temp_status == 1);
  CHECK(st.device_stamp_decodable);
  CHECK(st.t_device_last_ns == 7654321);

  // A timestamp_type this build cannot decode is reported as such rather
  // than guessed at, and does not disturb the last good stamp's meaning.
  const std::vector<std::uint8_t> q =
      make_extend_packet(999, kPointsPerPacketExtend, 0, kTimestampRsvd);
  d.on_point_packet(q.data(), q.size(), fake_clock());
  CHECK_FALSE(d.stats().device_stamp_decodable);
  CHECK(d.stats().t_device_last_ns == 7654321);
  CHECK(d.stats().point_packets == 2);
  CHECK(d.stats().packets_lost == 0);
  CHECK(d.stop().ok());
}

TEST_CASE("mid70/push_bytes_is_for_the_inject_backend_only") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());

  const std::vector<std::uint8_t> p = make_extend_packet(1000000);
  CHECK(d.push_bytes(ByteSpan(p.data(), p.size()), fake_clock()).ok());
  CHECK(d.stats().point_packets == 1);

  const std::vector<std::uint8_t> imu = make_imu_packet();
  CHECK(d.push_bytes(ByteSpan(imu.data(), imu.size()), fake_clock()).ok());
  CHECK(d.stats().unexpected_imu_packets == 1);

  // Not a datagram at all: refused by name, and counted.
  CHECK(d.push_bytes(ByteSpan(p.data(), 4), fake_clock()).error() == ScanError::kProtocolError);
  CHECK(d.stats().bad_packets == 1);
  CHECK(d.stop().ok());

  // ...and refused outright on a socket-owning backend.
  Mid70Config raw = inject_config();
  raw.backend = Mid70Backend::kRawUdp;
  Mid70Driver e(2, raw, rig.ctx);
  CHECK(e.push_bytes(ByteSpan(p.data(), p.size()), fake_clock()).error() ==
        ScanError::kNotSupported);
}

// ===========================================================================
// Health and the reconnect state machine.
// ===========================================================================

TEST_CASE("mid70/health_window_reports_rates_and_loss") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg = inject_config();
  cfg.health_period_ms = 1000;
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  // A second of traffic at 1/5th the real packet rate, with a 5-packet burst
  // never arriving. Device stamps advance one nominal interval per datagram
  // whether or not the datagram reaches us — that is the whole basis of the
  // gap model.
  std::int64_t t_dev = 1000000;
  for (int i = 0; i < 208; ++i) {
    if (i < 100 || i >= 105) {
      const std::vector<std::uint8_t> p = make_extend_packet(t_dev);
      d.on_point_packet(p.data(), p.size(), fake_clock());
    }
    t_dev += kExtendIntervalNs;
    advance_ms(5);
  }
  d.tick(fake_clock());

  const Mid70Stats st = d.stats();
  CHECK(st.point_packets == 203);
  CHECK(st.packets_lost == 5);
  CHECK(st.packets_duplicated == 0);
  CHECK(st.counter_resets == 0);
  CHECK(st.points_per_sec > 0.0);
  CHECK(st.points_appended_per_sec > 0.0);
  CHECK(st.loss_pct_window > 2.0);
  CHECK(st.loss_pct_window < 3.0);
  CHECK(st.loss_pct_total > 2.0);
  // Sustained loss above max_loss_pct demotes, and says why.
  CHECK(d.state() == DeviceState::kDegraded);
  CHECK(d.health().last_error == ScanError::kNetworkError);
  CHECK(d.health().checksum_pass_rate < 1.0);
  CHECK(d.health().checksum_pass_rate > 0.95);
  CHECK(d.health().packets_ok == 203);
  CHECK(d.stop().ok());
}

TEST_CASE("mid70/watchdog_is_the_only_thing_that_can_see_a_link_drop") {
  // The gap model shares the Mid-360 counter's blind spot: the device keeps
  // stamping while the wire is down, so a real outage arrives as one gap far
  // past the reset threshold and is booked as unattributable, not as loss.
  // The wall-clock watchdog is the primary outage signal.
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg = inject_config();
  cfg.reconnect.data_timeout_ms = 1000;
  cfg.reconnect.reinit_after_silence_ms = 5000;
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  std::int64_t t_dev = 1000000;
  for (int i = 0; i < 10; ++i) {
    const std::vector<std::uint8_t> p = make_extend_packet(t_dev);
    d.on_point_packet(p.data(), p.size(), fake_clock());
    t_dev += kExtendIntervalNs;
    advance_ms(50);
    d.tick(fake_clock());
  }
  CHECK(d.link_state() == Mid70LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);

  // --- the wire goes away for 3 s; the device keeps stamping --------------
  advance_ms(1500);
  t_dev += 1500000000LL;
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid70LinkState::kSilent);
  CHECK(d.state() == DeviceState::kDegraded);
  CHECK(d.health().last_error == ScanError::kDeviceNotResponding);
  CHECK(d.stats().watchdog_trips == 1);

  advance_ms(1500);
  t_dev += 1500000000LL;
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid70LinkState::kSilent);  // not yet re-init time

  // --- the cable is plugged back in --------------------------------------
  const std::vector<std::uint8_t> p = make_extend_packet(t_dev);
  d.on_point_packet(p.data(), p.size(), fake_clock());
  d.tick(fake_clock());

  CHECK(d.link_state() == Mid70LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);
  CHECK(d.stats().clean_resumes == 1);
  CHECK(d.stats().forced_reinits == 0);  // a cable pull needs NO re-init
  // And the punchline: the timestamp gap saw an outage it refused to name.
  CHECK(d.stats().packets_lost == 0);
  CHECK(d.stats().counter_resets == 1);
  CHECK(d.stop().ok());
}

TEST_CASE("mid70/prolonged_silence_forces_a_full_reinit") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg = inject_config();
  cfg.reconnect.data_timeout_ms = 1000;
  cfg.reconnect.reinit_after_silence_ms = 5000;
  cfg.reconnect.reinit_backoff_initial_ms = 1000;
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  const std::vector<std::uint8_t> p = make_extend_packet(1000000);
  d.on_point_packet(p.data(), p.size(), fake_clock());
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid70LinkState::kUp);

  // Silence past the watchdog...
  advance_ms(1200);
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid70LinkState::kSilent);
  CHECK(d.stats().forced_reinits == 0);

  // ...and on past the re-init threshold.
  advance_ms(4000);
  d.tick(fake_clock());
  CHECK(d.stats().forced_reinits == 1);
  CHECK(d.link_state() == Mid70LinkState::kReinitializing);
  CHECK(d.state() == DeviceState::kDegraded);

  // Still dead: the next attempt waits for the backoff window rather than
  // spinning socket setup once per tick.
  advance_ms(1000);
  d.tick(fake_clock());
  CHECK(d.stats().forced_reinits == 1);

  advance_ms(5000);
  d.tick(fake_clock());
  CHECK(d.stats().forced_reinits == 2);

  // The device comes back after the re-init with its clock reset to zero —
  // the power-cycle signature. The tracker was reset with the socket, so
  // that reads as a fresh first packet, not as one enormous duplicate.
  const std::vector<std::uint8_t> q = make_extend_packet(0);
  d.on_point_packet(q.data(), q.size(), fake_clock());
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid70LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);
  CHECK(d.stats().clean_resumes == 0);  // NOT a cable-class recovery
  CHECK(d.stats().forced_reinits == 2);
  CHECK(d.stats().packets_duplicated == 0);
  CHECK(d.stop().ok());
}

TEST_CASE("mid70/reconnect_can_be_capped_and_then_faults") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg = inject_config();
  cfg.reconnect.data_timeout_ms = 500;
  cfg.reconnect.reinit_after_silence_ms = 1000;
  cfg.reconnect.reinit_backoff_initial_ms = 100;
  cfg.reconnect.max_reinits = 2;
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  const std::vector<std::uint8_t> p = make_extend_packet(1000000);
  d.on_point_packet(p.data(), p.size(), fake_clock());
  d.tick(fake_clock());

  for (int i = 0; i < 8; ++i) {
    advance_ms(1500);
    d.tick(fake_clock());
  }
  CHECK(d.stats().forced_reinits == 2);
  CHECK(d.state() == DeviceState::kFault);
  CHECK(d.stop().ok());
}

TEST_CASE("mid70/watchdog_can_be_disabled") {
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg = inject_config();
  cfg.reconnect.enabled = false;
  Mid70Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());
  const std::vector<std::uint8_t> p = make_extend_packet(1000000);
  d.on_point_packet(p.data(), p.size(), fake_clock());
  d.tick(fake_clock());
  advance_ms(60000);
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid70LinkState::kUp);
  CHECK(d.stats().watchdog_trips == 0);
  CHECK(d.stats().forced_reinits == 0);
  CHECK(d.stop().ok());
}

// ===========================================================================
// Configuration guard rails.
// ===========================================================================

TEST_CASE("mid70/raw_udp_needs_a_host_and_a_port_but_sdk1_does_not") {
  g70_now_ns = 1000000000;
  Rig rig;

  // The listen-only backend binds a host socket and cannot guess which one.
  Mid70Config cfg;
  cfg.backend = Mid70Backend::kRawUdp;
  cfg.internal_supervisor_thread = false;
  Mid70Driver d(1, cfg, rig.ctx);
  CHECK(d.start().error() == ScanError::kInvalidArgument);
  CHECK(std::string(last_error_message()).find("host_ip") != std::string::npos);
  CHECK(d.state() == DeviceState::kFault);

  cfg.udp.host_ip = "192.168.20.2";
  cfg.udp.host_point_port = 0;
  cfg.udp.bind_port = 0;
  Mid70Driver e(2, cfg, rig.ctx);
  CHECK(e.start().error() == ScanError::kInvalidArgument);
  CHECK(std::string(last_error_message()).find("bind_port") != std::string::npos);
}

TEST_CASE("mid70/sdk1_backend_reports_its_absence_usefully") {
  // AND, by omission, proves the Mid-70's headline difference from the
  // Mid-360: no lidar_ip and no host_ip are demanded here. SDK v1 discovery
  // is a device broadcast the host merely listens to, so an out-of-the-box
  // device needs no addressing from the connect wizard at all.
  g70_now_ns = 1000000000;
  Rig rig;
  Mid70Config cfg;
  cfg.backend = Mid70Backend::kSdk1;
  cfg.internal_supervisor_thread = false;
  Mid70Driver d(1, cfg, rig.ctx);

  const Status s = d.start();
  if (s.ok()) {
    // Built WITH the SDK and it came up: there is no absence to report, and
    // no device to talk to either. That is the kc_m4 bring-up path, not a CI
    // leg — every CI leg runs without any SDK checkout.
    MESSAGE("sdk1 backend present and Init() succeeded; absence assertions skipped");
    CHECK(d.stop().ok());
    return;
  }
  const std::string msg = last_error_message();
  if (s.error() == ScanError::kNotSupported) {
    // Built without the SDK: the message must name the way out.
    CHECK(msg.find("fetch_sdk1.sh") != std::string::npos);
  } else {
    // Built WITH the SDK but unable to claim it in this environment.
    MESSAGE("sdk1 backend present; start() said: " << msg);
  }
  CHECK(d.state() == DeviceState::kFault);
}

TEST_CASE("mid70/to_string_covers_every_enumerator") {
  CHECK(std::string(to_string(Mid70Backend::kSdk1)) == "sdk1");
  CHECK(std::string(to_string(Mid70Backend::kRawUdp)) == "raw-udp");
  CHECK(std::string(to_string(Mid70Backend::kInject)) == "inject");
  CHECK(std::string(to_string(Mid70LinkState::kDown)) == "down");
  CHECK(std::string(to_string(Mid70LinkState::kWaiting)) == "waiting");
  CHECK(std::string(to_string(Mid70LinkState::kUp)) == "up");
  CHECK(std::string(to_string(Mid70LinkState::kSilent)) == "silent");
  CHECK(std::string(to_string(Mid70LinkState::kReinitializing)) == "reinitializing");
}
