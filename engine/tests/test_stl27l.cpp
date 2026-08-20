// ITEM 119 — LDROBOT STL-27L: parser, driver, fan mapping and the discovery
// classifier, against byte-exact synthetic fixtures.
//
// STYLE. Same shape as tests/test_d6_parser.cpp: microtest_shim.h for
// CHECK_EQ/CHECK_NEAR, a separate packet builder header that assembles frames
// BYTE BY BYTE and computes the CRC with an INDEPENDENT implementation, and
// doctest::Approx for the float comparisons. The shim's TEST() macro hard-
// codes a "d6/" case prefix (it exists to keep the S1 spike's file byte-
// identical), so the cases here use TEST_CASE("stl27l/...") directly, which is
// what every non-spike file in this suite does.
//
// WHAT THESE TESTS CAN AND CANNOT PROVE. No STL-27L hardware exists on this
// project. Every fixture below is generated from the PROTOCOL as documented in
// stl27l_parser.h, so what is proved is that the parser implements that
// document and that the two CRC implementations agree. Nothing here is
// evidence about a real device. The three claims that need hardware to settle
// are named at their assertions: the CRC parameters, the direction the angle
// sweeps, and the 30000 ms timestamp wrap.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "microtest_shim.h"
#include "packet_builder.h"  // the COIN-D6 builder, for the mutual-exclusion cases
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/discovery/discovery.h"
#include "scanengine/drivers/d6/d6_fan.h"
#include "scanengine/drivers/stl27l/stl27l_driver.h"
#include "scanengine/drivers/stl27l/stl27l_parser.h"
#include <filesystem>
#include <fstream>

#include "scanengine_c.h"
#include "scanengine/core/engine.h"
#include "scanengine/record/lscan.h"
#include "stl27l_packet_builder.h"

using namespace stl27l;
using namespace stl27ltest;

namespace {

std::vector<Point> decode(const std::vector<uint8_t>& bytes, Parser* p) {
  p->feed(bytes.data(), bytes.size(), 1);
  std::vector<Point> pts;
  p->take_points(&pts);
  return pts;
}

// Feed a stream one byte at a time to prove packets survive any chunking.
std::vector<Point> decode_bytewise(const std::vector<uint8_t>& bytes, Parser* p) {
  for (size_t i = 0; i < bytes.size(); ++i) p->feed(&bytes[i], 1, 1);
  std::vector<Point> pts;
  p->take_points(&pts);
  return pts;
}

// A packet with twelve distinguishable returns.
PacketSpec twelve(double a0, double a1) {
  PacketSpec ps;
  ps.first_angle_deg = a0;
  ps.last_angle_deg = a1;
  for (int i = 0; i < 12; ++i) {
    ps.samples.push_back(Sample{static_cast<uint16_t>(1000 + i * 37),
                                static_cast<uint8_t>(10 + i * 3)});
  }
  return ps;
}

// A plain-function-pointer clock (DriverContext::clock's required shape) over
// a file-local variable a test moves by hand. Copied in spirit from
// tests/test_d6_driver.cpp — nothing here depends on real elapsed time.
std::int64_t g_fake_now_ns = 0;
scanengine::TimePoint fake_clock() { return scanengine::TimePoint{g_fake_now_ns}; }

}  // namespace

// ===========================================================================
// CRC8
// ===========================================================================

TEST_CASE("stl27l/crc8_matches_the_vendor_table_and_a_known_vector") {
  // (1) THE VENDOR-SOURCED VECTOR. The LDROBOT LD06/LD19/STL-27L SDK ships a
  // 256-entry `CrcTable[]` and uses it as `crc = CrcTable[(crc ^ byte)]`, so
  // entry i is by construction the CRC of the single byte i. Its published
  // first sixteen entries are pinned here. stl27l::crc8() generates the table
  // from poly 0x4D rather than pasting it (see the header's provenance note),
  // and this is what makes that generation checkable against the vendor.
  static const uint8_t kVendorTablePrefix[16] = {0x00, 0x4d, 0x9a, 0xd7, 0x79, 0x34,
                                                 0xe3, 0xae, 0xf2, 0xbf, 0x68, 0x25,
                                                 0x8b, 0xc6, 0x11, 0x5c};
  for (int i = 0; i < 16; ++i) {
    const uint8_t one = static_cast<uint8_t>(i);
    CHECK_EQ(crc8(&one, 1), kVendorTablePrefix[i]);
  }
  // The whole table, against the independent bitwise builder in the fixture
  // header — 256 assertions that the generator and the shift register agree.
  for (int i = 0; i < 256; ++i) {
    const uint8_t one = static_cast<uint8_t>(i);
    CHECK_EQ(crc8(&one, 1), crc8_bitwise(&one, 1));
  }

  // (2) THE CATALOGUE-STYLE CHECK VALUE. width=8 poly=0x4D init=0x00
  // refin=false refout=false xorout=0x00 over the ASCII string "123456789".
  const char* nine = "123456789";
  CHECK_EQ(crc8(reinterpret_cast<const uint8_t*>(nine), 9), 0xC3);

  // (3) Init is 0x00 and there is no final XOR, so an all-zero message has an
  // all-zero CRC. Two assertions that between them pin both parameters.
  const std::vector<uint8_t> zeros(46, 0);
  CHECK_EQ(crc8(zeros.data(), zeros.size()), 0x00);
  const uint8_t one_bit = 0x80;
  CHECK_EQ(crc8(&one_bit, 1), 0x7C);   // == table[0x80]
  const uint8_t all_bits = 0xFF;
  CHECK_EQ(crc8(&all_bits, 1), 0xA8);  // == table[0xFF]

  // (4) It is order-sensitive (a XOR-only checksum would not be), so a
  // transposition is caught. This is the property the D6's 16-bit XOR
  // checksum famously does NOT have.
  const uint8_t ab[2] = {0x12, 0x34};
  const uint8_t ba[2] = {0x34, 0x12};
  CHECK(crc8(ab, 2) != crc8(ba, 2));
}

TEST_CASE("stl27l/builder_and_parser_crcs_agree_on_a_real_packet") {
  const auto pkt = build(twelve(12.0, 14.0));
  REQUIRE(pkt.size() == stl27l::kPacketBytes);
  // The builder wrote the CRC at offset 46 over the 46 bytes before it.
  CHECK_EQ(pkt[46], crc8(pkt.data(), 46));
  CHECK_EQ(pkt[46], crc8_bitwise(pkt.data(), 46));
}

// ===========================================================================
// packet layout and decode
// ===========================================================================

TEST_CASE("stl27l/packet_is_47_bytes_at_the_documented_offsets") {
  PacketSpec ps = twelve(30.0, 32.0);
  ps.speed_dps = 3600;
  ps.timestamp_ms = 0x1234;
  const auto pkt = build(ps);

  REQUIRE(pkt.size() == 47u);
  CHECK_EQ(pkt[0], 0x54);              // header
  CHECK_EQ(pkt[1], 0x2C);              // ver_len: version 1, 12 points
  CHECK_EQ(pkt[1] & 0x1F, 12);         // the low five bits ARE the point count
  CHECK_EQ(pkt[2] | (pkt[3] << 8), 3600);        // speed, little-endian
  CHECK_EQ(pkt[4] | (pkt[5] << 8), 3000);        // start_angle, 0.01 deg
  CHECK_EQ(pkt[42] | (pkt[43] << 8), 3200);      // end_angle
  CHECK_EQ(pkt[44] | (pkt[45] << 8), 0x1234);    // timestamp

  // The twelve 3-byte measurement records live at 6..41 inclusive.
  for (int i = 0; i < 12; ++i) {
    const uint8_t* s = pkt.data() + 6 + i * 3;
    CHECK_EQ(s[0] | (s[1] << 8), 1000 + i * 37);
    CHECK_EQ(s[2], 10 + i * 3);
  }

  Packet decoded{};
  REQUIRE(decode_packet(pkt.data(), &decoded));
  CHECK_EQ(decoded.speed_dps, 3600);
  CHECK(decoded.start_angle_deg == doctest::Approx(30.0));
  CHECK(decoded.end_angle_deg == doctest::Approx(32.0));
  CHECK_EQ(decoded.timestamp_ms, 0x1234);
}

TEST_CASE("stl27l/good_packet_decodes_all_twelve_points") {
  Parser p;
  Config cfg;
  cfg.drop_zero_range = false;  // this packet has none, but be explicit
  p.set_config(cfg);

  const auto pts = decode(build(twelve(100.0, 102.0)), &p);
  REQUIRE(pts.size() == 12u);
  CHECK_EQ(p.stats().packets_ok, 1u);
  CHECK_EQ(p.stats().packets_bad_crc, 0u);
  CHECK_EQ(p.stats().points, 12u);

  for (int i = 0; i < 12; ++i) {
    CHECK_EQ(pts[static_cast<size_t>(i)].distance_mm, 1000 + i * 37);
    CHECK_EQ(pts[static_cast<size_t>(i)].intensity, 10 + i * 3);
    // step = (102 - 100) / 11 -- the FIRST point sits on start_angle and the
    // LAST on end_angle, which is the LD-series convention and is asserted
    // rather than assumed.
    const double want = 100.0 + (2.0 / 11.0) * i;
    CHECK(pts[static_cast<size_t>(i)].angle_deg == doctest::Approx(want).epsilon(1e-4));
  }
  CHECK(pts[0].angle_deg == doctest::Approx(100.0));
  CHECK(pts[11].angle_deg == doctest::Approx(102.0));
}

TEST_CASE("stl27l/free_function_angle_interpolation") {
  // The interpolator on its own, including the shape of a one-point packet.
  CHECK(point_angle_deg(10.f, 21.f, 0, 12) == doctest::Approx(10.0));
  CHECK(point_angle_deg(10.f, 21.f, 11, 12) == doctest::Approx(21.0));
  CHECK(point_angle_deg(10.f, 21.f, 5, 12) == doctest::Approx(15.0));
  CHECK(point_angle_deg(10.f, 21.f, 0, 1) == doctest::Approx(10.0));
}

// ===========================================================================
// the 0/360 seam
// ===========================================================================

TEST_CASE("stl27l/angle_wraps_across_360") {
  // start 355, end 2 -- the packet crosses the seam, so the end is a
  // revolution LATER and the twelve points must run forwards through 0, not
  // backwards across the whole circle.
  Parser p;
  const auto pts = decode(build(twelve(355.0, 2.0)), &p);
  REQUIRE(pts.size() == 12u);

  // step = (362 - 355) / 11 = 0.63636 deg
  const double step = 7.0 / 11.0;
  for (int i = 0; i < 12; ++i) {
    double want = 355.0 + step * i;
    if (want >= 360.0) want -= 360.0;
    CHECK(pts[static_cast<size_t>(i)].angle_deg == doctest::Approx(want).epsilon(1e-4));
  }
  CHECK(pts[0].angle_deg == doctest::Approx(355.0));
  CHECK(pts[11].angle_deg == doctest::Approx(2.0));

  // Every angle stays inside [0, 360).
  for (const Point& pt : pts) {
    CHECK(pt.angle_deg >= 0.f);
    CHECK(pt.angle_deg < 360.f);
  }

  // And the free function agrees with the parser, wrap included.
  CHECK(point_angle_deg(355.f, 2.f, 6, 12) ==
        doctest::Approx(static_cast<double>(pts[6].angle_deg)).epsilon(1e-4));
}

TEST_CASE("stl27l/a_full_revolution_is_counted_once") {
  Parser p;
  // 180 packets x 12 points = 2160 returns, the datasheet's revolution.
  const auto bytes = build_revolution(180, 1000, 128, 10.0, 0, 2);
  const auto pts = decode(bytes, &p);
  CHECK_EQ(p.stats().packets_ok, 360u);
  CHECK_EQ(pts.size(), 4320u);
  // Two revolutions of angle; the wrap is only crossed ONCE inside the stream
  // (the first revolution starts at 0 with no predecessor to wrap from).
  CHECK_EQ(p.stats().rotations, 1u);
}

// ===========================================================================
// framing: chunking, resync, rejection
// ===========================================================================

TEST_CASE("stl27l/byte_at_a_time_streaming_reassembles_packets") {
  std::vector<uint8_t> stream;
  append(&stream, build(twelve(0.0, 2.0)));
  append(&stream, build(twelve(2.0, 4.0)));
  append(&stream, build(twelve(4.0, 6.0)));

  Parser one;
  const auto whole = decode(stream, &one);

  Parser bytewise;
  const auto split = decode_bytewise(stream, &bytewise);

  REQUIRE(whole.size() == 36u);
  REQUIRE(split.size() == 36u);
  CHECK_EQ(bytewise.stats().packets_ok, 3u);
  for (size_t i = 0; i < whole.size(); ++i) {
    CHECK_EQ(split[i].distance_mm, whole[i].distance_mm);
    CHECK_EQ(split[i].intensity, whole[i].intensity);
    CHECK(split[i].angle_deg == doctest::Approx(static_cast<double>(whole[i].angle_deg)));
  }
}

TEST_CASE("stl27l/packets_split_across_arbitrary_chunk_boundaries") {
  std::vector<uint8_t> stream;
  for (int k = 0; k < 8; ++k) append(&stream, build(twelve(k * 2.0, k * 2.0 + 2.0)));

  // Every chunk size from 1 to 50 tears the 47-byte frame somewhere
  // different, including exactly on a boundary (47) and one either side.
  for (size_t chunk = 1; chunk <= 50; ++chunk) {
    Parser p;
    for (size_t off = 0; off < stream.size(); off += chunk) {
      const size_t n = std::min(chunk, stream.size() - off);
      p.feed(stream.data() + off, n, 1);
    }
    std::vector<Point> pts;
    p.take_points(&pts);
    CHECK_EQ(p.stats().packets_ok, 8u);
    CHECK_EQ(pts.size(), 96u);
    CHECK_EQ(p.stats().packets_bad_crc, 0u);
  }
}

TEST_CASE("stl27l/resyncs_after_garbage") {
  std::vector<uint8_t> stream;
  // Garbage that includes a lone 0x54 and a 0x54 followed by the WRONG second
  // byte -- both are false-header traps the framer must walk past.
  const uint8_t junk[] = {0x00, 0x54, 0xFF, 0x54, 0x2B, 0x11, 0x22, 0x54};
  stream.insert(stream.end(), junk, junk + sizeof(junk));
  append(&stream, build(twelve(10.0, 12.0)));
  stream.push_back(0xAB);
  stream.push_back(0xCD);
  append(&stream, build(twelve(12.0, 14.0)));

  Parser p;
  const auto pts = decode(stream, &p);
  CHECK_EQ(p.stats().packets_ok, 2u);
  CHECK_EQ(pts.size(), 24u);
  CHECK(p.stats().resyncs >= 1u);
  CHECK(p.stats().bytes_discarded >= sizeof(junk));

  // Bytewise arrival must reach exactly the same verdict.
  Parser q;
  const auto pts2 = decode_bytewise(stream, &q);
  CHECK_EQ(q.stats().packets_ok, 2u);
  CHECK_EQ(pts2.size(), 24u);
}

TEST_CASE("stl27l/a_bad_crc_is_rejected") {
  PacketSpec ps = twelve(20.0, 22.0);
  ps.crc_mode = CrcMode::kCorrupt;

  Parser p;
  const auto pts = decode(build(ps), &p);
  CHECK_EQ(pts.size(), 0u);
  CHECK_EQ(p.stats().packets_ok, 0u);
  CHECK_EQ(p.stats().packets_bad_crc, 1u);
  CHECK_NEAR(p.stats().crc_pass_rate(), 0.0, 1e-9);

  // A corrupt packet must not destroy the frame that follows it: the default
  // resync drops ONE byte, so the next good packet is still found.
  std::vector<uint8_t> stream;
  append(&stream, build(ps));
  append(&stream, build(twelve(22.0, 24.0)));
  Parser q;
  const auto pts2 = decode(stream, &q);
  CHECK_EQ(q.stats().packets_bad_crc, 1u);
  CHECK_EQ(q.stats().packets_ok, 1u);
  CHECK_EQ(pts2.size(), 12u);

  // Every single-bit flip in the 46 protected bytes is caught. (A CRC8 misses
  // 1 in 256 RANDOM corruptions; it misses NO single-bit error, and that is
  // the property worth pinning.)
  const auto good = build(twelve(20.0, 22.0));
  for (size_t byte = 0; byte < 46; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> flipped = good;
      flipped[byte] = static_cast<uint8_t>(flipped[byte] ^ (1u << bit));
      if (byte <= 1) continue;  // flipping the sync makes it a different frame
      CHECK(crc8(flipped.data(), 46) != flipped[46]);
    }
  }
}

TEST_CASE("stl27l/a_wrong_ver_len_is_rejected") {
  // 0x2B would be 11 points; 0x4C would be a different protocol version. Both
  // are refused at the sync, before any CRC work.
  for (uint8_t bad_ver : {uint8_t{0x2B}, uint8_t{0x2D}, uint8_t{0x4C}, uint8_t{0x00}}) {
    PacketSpec ps = twelve(0.0, 2.0);
    ps.force_ver_len = bad_ver;
    std::vector<uint8_t> stream;
    append(&stream, build(ps));
    append(&stream, build(twelve(5.0, 7.0)));  // a good one behind it

    Parser p;
    const auto pts = decode(stream, &p);
    CHECK(p.stats().packets_malformed >= 1u);
    // Only the GOOD packet's twelve points come out.
    CHECK_EQ(p.stats().packets_ok, 1u);
    CHECK_EQ(pts.size(), 12u);
  }

  // A wrong HEADER byte is likewise never a packet.
  PacketSpec ps = twelve(0.0, 2.0);
  ps.force_header = 0x55;
  Parser q;
  const auto pts = decode(build(ps), &q);
  CHECK_EQ(pts.size(), 0u);
  CHECK_EQ(q.stats().packets_ok, 0u);
}

TEST_CASE("stl27l/decode_packet_verifies_before_it_decodes") {
  auto pkt = build(twelve(0.0, 2.0));
  Packet out{};
  CHECK(decode_packet(pkt.data(), &out));
  pkt[46] = static_cast<uint8_t>(pkt[46] ^ 0xFF);
  CHECK_FALSE(decode_packet(pkt.data(), &out));
  pkt[46] = crc8(pkt.data(), 46);
  pkt[1] = 0x2B;
  CHECK_FALSE(decode_packet(pkt.data(), &out));
}

// ===========================================================================
// zero-distance returns
// ===========================================================================

TEST_CASE("stl27l/zero_distance_points_are_dropped") {
  PacketSpec ps;
  ps.first_angle_deg = 40.0;
  ps.last_angle_deg = 42.0;
  for (int i = 0; i < 12; ++i) {
    // Every third return is a no-return.
    ps.samples.push_back(Sample{static_cast<uint16_t>((i % 3 == 0) ? 0 : 1500 + i),
                                static_cast<uint8_t>(50 + i)});
  }

  // The default drops them: a zero-range return has no geometry, and placed
  // in the fan frame it would be a fake point sitting on the sensor origin.
  Parser p;
  const auto kept = decode(build(ps), &p);
  CHECK_EQ(kept.size(), 8u);
  CHECK_EQ(p.stats().points_zero_range, 4u);
  CHECK_EQ(p.stats().points, 8u);
  for (const Point& pt : kept) CHECK(pt.distance_mm != 0);

  // Turning the drop off keeps all twelve, and still counts them.
  Config cfg;
  cfg.drop_zero_range = false;
  Parser q(cfg);
  const auto all = decode(build(ps), &q);
  CHECK_EQ(all.size(), 12u);
  CHECK_EQ(q.stats().points_zero_range, 4u);
}

TEST_CASE("stl27l/a_dropped_zero_range_return_does_not_swallow_a_rotation_marker") {
  // The revolution boundary lands on a no-return: the marker must survive
  // onto the next point that is actually emitted, or the driver loses a
  // rotation event.
  Parser p;
  std::vector<uint8_t> stream;
  {
    PacketSpec a = twelve(350.0, 358.0);
    append(&stream, build(a));
  }
  {
    PacketSpec b;
    b.first_angle_deg = 358.0;
    b.last_angle_deg = 6.0;
    for (int i = 0; i < 12; ++i) {
      // The first point past the seam (index 3 at this spacing) is a
      // no-return.
      const bool seam = (i == 3);
      b.samples.push_back(Sample{static_cast<uint16_t>(seam ? 0 : 2000), 77});
    }
    append(&stream, build(b));
  }
  const auto pts = decode(stream, &p);
  CHECK_EQ(p.stats().rotations, 1u);
  int marked = 0;
  for (const Point& pt : pts) if (pt.new_rotation) ++marked;
  CHECK_EQ(marked, 1);
}

// ===========================================================================
// per-point time
// ===========================================================================

TEST_CASE("stl27l/per_point_time_interpolates_inside_a_packet") {
  // Two packets, so the second one has a predecessor to derive a device-clock
  // span from and a sample chain to propagate.
  std::vector<uint8_t> stream;
  {
    PacketSpec a = twelve(0.0, 2.0);
    a.timestamp_ms = 1000;
    append(&stream, build(a));
  }
  {
    PacketSpec b = twelve(2.0, 4.0);
    b.timestamp_ms = 1006;  // 6 ms later
    append(&stream, build(b));
  }

  Parser p;
  const uint64_t t_rx = 5'000'000'000ull;
  p.feed(stream.data(), stream.size(), t_rx);
  std::vector<Point> pts;
  p.take_points(&pts);
  REQUIRE(pts.size() == 24u);

  // --- host-clock stamps (the ones the pushbroom uses) --------------------
  //
  // The D6's convention, which this parser must match exactly: the LAST
  // return of a packet sits on the anchor and the earlier eleven are
  // back-dated at the sampling period. So inside one packet the stamps are
  // strictly increasing and EVENLY spaced.
  for (size_t i = 0; i < 12; ++i) CHECK(pts[i].t_sample_ns != 0u);
  const double d0 = static_cast<double>(pts[1].t_sample_ns) -
                    static_cast<double>(pts[0].t_sample_ns);
  CHECK(d0 > 0.0);
  for (size_t i = 1; i < 12; ++i) {
    const double d = static_cast<double>(pts[i].t_sample_ns) -
                     static_cast<double>(pts[i - 1].t_sample_ns);
    CHECK_NEAR(d, d0, 2.0);  // integer-ns rounding only
  }
  // The derived rate is the datasheet's: speed 3600 deg/s over a 2-degree,
  // 12-point packet is 3600 / (2/11) = 19800 points/s, i.e. ~50.5 us apart.
  CHECK_NEAR(d0, 1e9 / 19800.0, 50.0);

  // Monotonic across the packet boundary too, and never in the future of the
  // chunk that carried the bytes.
  for (size_t i = 1; i < pts.size(); ++i) {
    CHECK(pts[i].t_sample_ns >= pts[i - 1].t_sample_ns);
  }
  CHECK(pts.back().t_sample_ns <= t_rx);

  // --- the device clock ---------------------------------------------------
  //
  // The first packet has no predecessor, so it carries no device span.
  for (size_t i = 0; i < 12; ++i) CHECK_EQ(pts[i].t_device_ns, 0u);
  // The second interpolates its twelve returns between the previous packet's
  // timestamp (1000 ms) and its own (1006 ms), with the LAST landing exactly
  // on its own timestamp -- the same "last sample owns the stamp" convention
  // as the host side.
  for (size_t i = 0; i < 12; ++i) {
    const double want = 1000e6 + 6e6 * static_cast<double>(i + 1) / 12.0;
    CHECK_NEAR(static_cast<double>(pts[12 + i].t_device_ns), want, 1.0);
  }
  CHECK_EQ(pts[23].t_device_ns, 1006ull * 1000000ull);
}

TEST_CASE("stl27l/device_timestamp_wrap_is_not_time_going_backwards") {
  // 30000 ms is the documented horizon. PROTOCOL-DERIVED: nothing has watched
  // a real unit roll over.
  CHECK_EQ(timestamp_delta_ms(29990, 29995), 5u);
  CHECK_EQ(timestamp_delta_ms(29995, 5), 10u);      // wrapped
  CHECK_EQ(timestamp_delta_ms(0, 0), 0u);
  CHECK_EQ(timestamp_delta_ms(29999, 0), 1u);

  std::vector<uint8_t> stream;
  {
    PacketSpec a = twelve(0.0, 2.0);
    a.timestamp_ms = 29995;
    append(&stream, build(a));
  }
  {
    PacketSpec b = twelve(2.0, 4.0);
    b.timestamp_ms = 29999;
    append(&stream, build(b));
  }
  {
    PacketSpec c = twelve(4.0, 6.0);
    c.timestamp_ms = 3;  // rolled over
    append(&stream, build(c));
  }

  Parser p;
  const auto pts = decode(stream, &p);
  REQUIRE(pts.size() == 36u);
  CHECK_EQ(p.stats().timestamp_wraps, 1u);
  // The device clock is unwrapped, so it keeps increasing straight through.
  for (size_t i = 13; i < pts.size(); ++i) {
    CHECK(pts[i].t_device_ns >= pts[i - 1].t_device_ns);
  }
  // 29999 -> 3 is a 4 ms step, not a -29996 ms one.
  CHECK_EQ(pts[35].t_device_ns - pts[23].t_device_ns, 4ull * 1000000ull);
}

TEST_CASE("stl27l/spacing_can_be_driven_from_the_device_timestamp_instead") {
  // Config::use_device_timestamp_spacing, which is OFF by default because at
  // the datasheet rate the device's 1 ms tick is coarser than a whole packet.
  // Spun slowly enough that a packet covers several ticks, it is exact — so
  // the fixture here is a 1 Hz spin, where twelve returns take 24 ms.
  Config cfg;
  cfg.use_device_timestamp_spacing = true;
  // The derived rate is 12 returns per 24 ms = 500 Hz, which is 2.3% of the
  // 21600 Hz nominal — so the datasheet cross-check has to be told this is a
  // legitimately slow device rather than a corrupt packet.
  cfg.nominal_sample_hz = 500.0;
  Parser p(cfg);

  // Each packet is fed with its OWN arrival time, 24 ms apart, the way a
  // serial reader delivers them. Handing all three to one feed() at one
  // instant would (correctly) make the monotonicity clamp compress the
  // spacing to fit the ~1.5 ms of wire time those bytes actually occupy: the
  // parser never stamps a return later than the transport that carried it,
  // and a fixture that ignores that is testing the clamp, not the spacing.
  const uint64_t t0 = 9'000'000'000ull;
  for (int k = 0; k < 3; ++k) {
    PacketSpec ps = twelve(k * 2.0, k * 2.0 + 2.0);
    ps.speed_dps = 360;                                     // 1 Hz
    ps.timestamp_ms = static_cast<uint16_t>(1000 + k * 24); // 24 ms per packet
    const auto pkt = build(ps);
    p.feed(pkt.data(), pkt.size(), t0 + static_cast<uint64_t>(k) * 24'000'000ull);
  }
  std::vector<Point> pts;
  p.take_points(&pts);
  REQUIRE(pts.size() == 36u);

  // The second packet's returns are 2 ms apart in HOST time, taken straight
  // from the device's own 24 ms packet span.
  for (size_t i = 13; i < 24; ++i) {
    const double d = static_cast<double>(pts[i].t_sample_ns) -
                     static_cast<double>(pts[i - 1].t_sample_ns);
    CHECK_NEAR(d, 2e6, 1e3);
  }
  CHECK_NEAR(p.stats().sample_hz_est, 500.0, 1.0);
  // Exactly one warning, and it belongs to the FIRST packet: with no
  // predecessor it has no device span to use, so it falls to the angle/speed
  // derivation (360 deg/s over 0.1818 deg per return = 1980 Hz) which is far
  // outside the deliberately-lowered 500 Hz nominal this fixture set. The two
  // packets that DO have a predecessor agree with it and warn about nothing.
  CHECK_EQ(p.stats().sample_rate_warnings, 1u);
}

TEST_CASE("stl27l/an_implausible_derived_rate_falls_back_to_nominal_and_says_so") {
  // A corrupt angle span (or a stalled rotor) must not be honoured as a
  // sampling rate: outside Config::sample_rate_tolerance the parser uses the
  // datasheet period and counts a warning, which is the D6's behaviour too.
  PacketSpec ps = twelve(0.0, 0.5);  // a 0.5 deg span at 3600 deg/s => 79200 Hz
  Parser p;
  const auto pkt = build(ps);
  // A realistic arrival stamp: the anchor back-dates a packet by its own wire
  // duration, so a t_rx of a few nanoseconds would put every return before
  // the epoch and the parser would (correctly) publish no estimate at all.
  p.feed(pkt.data(), pkt.size(), 4'000'000'000ull);
  std::vector<Point> pts;
  p.take_points(&pts);
  REQUIRE(pts.size() == 12u);
  CHECK(p.stats().sample_rate_warnings >= 1u);
  const double d = static_cast<double>(pts[1].t_sample_ns) -
                   static_cast<double>(pts[0].t_sample_ns);
  CHECK_NEAR(d, 1e9 / kNominalSampleHz, 2.0);
}

TEST_CASE("stl27l/per_sample_timestamps_can_be_turned_off") {
  Config cfg;
  cfg.per_sample_timestamps = false;
  Parser p(cfg);
  const auto pts = decode(build(twelve(0.0, 2.0)), &p);
  REQUIRE(pts.size() == 12u);
  for (const Point& pt : pts) {
    CHECK_EQ(pt.t_sample_ns, 0u);
    CHECK_EQ(pt.t_rx_ns, 1u);  // the feed() stamp is still carried
  }
}

// ===========================================================================
// fan mapping — the ONE convention, shared with the D6
// ===========================================================================

TEST_CASE("stl27l/fan_mapping_agrees_with_the_d6_convention") {
  // d6_fan.h is the single definition of the sensor frame in this engine:
  //     x = -d*sin(theta), y = d*cos(theta), z = 0
  // The STL-27L driver CALLS it. This case is what would fail if somebody
  // re-inlined the formula here with the ROUND-9 sign bug back in it.
  scanengine::EventBus bus;
  scanengine::PageStore points;
  scanengine::DriverContext ctx;
  ctx.bus = &bus;
  ctx.points = &points;

  scanengine::Stl27lConfig cfg;
  scanengine::Stl27lDriver driver(1, cfg, ctx);
  REQUIRE(driver.start().ok());

  // One packet whose twelve returns are all 2.000 m, starting at 0 degrees.
  PacketSpec ps;
  ps.first_angle_deg = 0.0;
  ps.last_angle_deg = 33.0;  // 3 degrees apart, so the signs are unambiguous
  for (int i = 0; i < 12; ++i) ps.samples.push_back(Sample{2000, 200});
  const auto pkt = build(ps);
  REQUIRE(driver.push_bytes(scanengine::ByteSpan(pkt.data(), pkt.size()),
                            scanengine::TimePoint{0})
              .ok());
  (void)driver.stop();  // flushes the batch into the store

  const auto ids = points.page_ids();
  REQUIRE(!ids.empty());
  const scanengine::PageView pv = points.page_view(ids.front());
  REQUIRE(pv.count >= 12u);
  CHECK(pv.stream == scanengine::StreamId::kLidarStl27l);

  for (int i = 0; i < 12; ++i) {
    const double theta = 3.0 * i;
    double want[3];
    d6::fan_point(theta, 2.0, want);
    CHECK(pv.data[static_cast<size_t>(i)].x == doctest::Approx(want[0]).epsilon(1e-4));
    CHECK(pv.data[static_cast<size_t>(i)].y == doctest::Approx(want[1]).epsilon(1e-4));
    CHECK(pv.data[static_cast<size_t>(i)].z == doctest::Approx(0.0));
  }
  // The sign that ROUND 9 corrected: at +90 degrees the return is at NEGATIVE
  // x. Restated here as a bare arithmetic fact so this case says WHAT it is
  // defending, not just "the two agree".
  double p90[3];
  d6::fan_point(90.0, 1.0, p90);
  CHECK(p90[0] == doctest::Approx(-1.0));
  CHECK_NEAR(p90[1], 0.0, 1e-9);
}

TEST_CASE("stl27l/invert_angle_is_the_one_knob_for_a_reversed_sweep") {
  // If hardware turns out to sweep the other way, this is the fix -- NOT a
  // second copy of the fan formula. See stl27l_driver.h.
  scanengine::EventBus bus;
  scanengine::PageStore points;
  scanengine::DriverContext ctx;
  ctx.bus = &bus;
  ctx.points = &points;

  scanengine::Stl27lConfig cfg;
  cfg.invert_angle = true;
  scanengine::Stl27lDriver driver(1, cfg, ctx);
  REQUIRE(driver.start().ok());

  PacketSpec ps;
  ps.first_angle_deg = 90.0;
  ps.last_angle_deg = 90.0;  // twelve returns all at 90 degrees
  for (int i = 0; i < 12; ++i) ps.samples.push_back(Sample{1000, 100});
  const auto pkt = build(ps);
  REQUIRE(driver.push_bytes(scanengine::ByteSpan(pkt.data(), pkt.size()),
                            scanengine::TimePoint{0})
              .ok());
  (void)driver.stop();

  const auto ids = points.page_ids();
  REQUIRE(!ids.empty());
  const scanengine::PageView pv = points.page_view(ids.front());
  REQUIRE(pv.count >= 1u);
  // 360 - 90 = 270, which the shared formula puts at +x.
  double want[3];
  d6::fan_point(270.0, 1.0, want);
  CHECK(pv.data[0].x == doctest::Approx(want[0]).epsilon(1e-4));
  CHECK(pv.data[0].x == doctest::Approx(1.0).epsilon(1e-4));
}

// ===========================================================================
// the driver
// ===========================================================================

TEST_CASE("stl27l/driver_reaches_streaming_and_reports_health") {
  scanengine::EventBus bus;
  scanengine::PageStore points;
  scanengine::DriverContext ctx;
  ctx.bus = &bus;
  ctx.points = &points;

  scanengine::Stl27lDriver driver(7, scanengine::Stl27lConfig{}, ctx);
  CHECK(driver.kind() == scanengine::DeviceKind::kStl27l);
  CHECK(std::string(driver.name()) == "stl27l");

  REQUIRE(driver.start().ok());
  CHECK(driver.snapshot().phase == scanengine::Stl27lPhase::kStarting);

  const auto bytes = build_revolution(60, 1500, 90, 10.0);
  REQUIRE(driver.push_bytes(scanengine::ByteSpan(bytes.data(), bytes.size()),
                            scanengine::TimePoint{0})
              .ok());

  CHECK(driver.state() == scanengine::DeviceState::kStreaming);
  const scanengine::Stl27lHealthSnapshot snap = driver.snapshot();
  CHECK(snap.phase == scanengine::Stl27lPhase::kStreaming);
  CHECK(snap.stall == scanengine::Stl27lStallKind::kNone);
  CHECK(snap.crc_pass_rate == doctest::Approx(1.0));
  CHECK_EQ(snap.bytes_in, bytes.size());
  CHECK_EQ(snap.speed_dps, 3600);
  CHECK_EQ(snap.packets_bad_crc, 0u);

  const scanengine::DeviceHealth h = driver.health();
  CHECK(h.kind == scanengine::DeviceKind::kStl27l);
  CHECK_EQ(h.packets_ok, 60u);
  CHECK_EQ(h.packets_bad, 0u);
  CHECK(h.points_out > 0u);

  // The transport defaulted to the STL-27L's rate, not the D6's.
  CHECK_EQ(driver.transport().config().baud, 921600u);

  REQUIRE(driver.stop().ok());
  CHECK(driver.state() == scanengine::DeviceState::kIdle);
}

TEST_CASE("stl27l/driver_watchdog_reports_a_silent_device") {
  // A deterministic wall clock, the seam clock.h documents and
  // tests/test_d6_driver.cpp uses for exactly this: state()/health()/
  // snapshot() re-check the watchdog against DriverContext::clock(), so
  // without this the real steady clock would fight the synthetic timestamps.
  const std::int64_t now_ns = 1'000'000'000;
  g_fake_now_ns = now_ns;
  scanengine::EventBus bus;
  scanengine::PageStore points;
  scanengine::DriverContext ctx;
  ctx.bus = &bus;
  ctx.points = &points;
  ctx.clock = &fake_clock;

  scanengine::Stl27lConfig cfg;
  scanengine::Stl27lDriver driver(3, cfg, ctx);
  REQUIRE(driver.start().ok());

  const auto bytes = build_revolution(4, 1000, 80, 10.0);
  REQUIRE(driver.push_bytes(scanengine::ByteSpan(bytes.data(), bytes.size()),
                            scanengine::TimePoint{now_ns})
              .ok());
  CHECK(driver.snapshot().phase == scanengine::Stl27lPhase::kStreaming);

  // Inside the startup grace nothing fires, however long the silence.
  g_fake_now_ns = now_ns + 1'900'000'000;
  driver.check_watchdog(scanengine::TimePoint{now_ns + 1'900'000'000});
  CHECK(driver.snapshot().phase == scanengine::Stl27lPhase::kStreaming);

  // Past the grace and past the silent timeout, it does. No restart is
  // attempted (there is no command channel), so the device parks in kStalled
  // -> kDegraded rather than churning.
  g_fake_now_ns = now_ns + 5'000'000'000;
  driver.check_watchdog(scanengine::TimePoint{now_ns + 5'000'000'000});
  const scanengine::Stl27lHealthSnapshot snap = driver.snapshot();
  CHECK(snap.phase == scanengine::Stl27lPhase::kStalled);
  CHECK(snap.stall == scanengine::Stl27lStallKind::kSilent);
  CHECK(driver.state() == scanengine::DeviceState::kDegraded);

  // Bytes coming back promote it straight to kStreaming again.
  g_fake_now_ns = now_ns + 5'100'000'000;
  REQUIRE(driver.push_bytes(scanengine::ByteSpan(bytes.data(), bytes.size()),
                            scanengine::TimePoint{now_ns + 5'100'000'000})
              .ok());
  CHECK(driver.snapshot().phase == scanengine::Stl27lPhase::kStreaming);
}

TEST_CASE("stl27l/driver_profile_sink_sees_polar_returns_with_per_point_time") {
  struct Cap {
    std::vector<float> angles;
    std::vector<float> ranges;
    std::vector<std::int64_t> times;
  };
  static Cap cap;
  cap = Cap{};

  scanengine::EventBus bus;
  scanengine::PageStore points;
  scanengine::DriverContext ctx;
  ctx.bus = &bus;
  ctx.points = &points;

  scanengine::Stl27lConfig cfg;
  cfg.profile_sink = [](float a, float r, std::uint8_t, std::uint8_t hi, std::int64_t t, void*) {
    CHECK_EQ(hi, 0);  // the LD protocol has no high-reflectivity flag
    cap.angles.push_back(a);
    cap.ranges.push_back(r);
    cap.times.push_back(t);
  };
  scanengine::Stl27lDriver driver(1, cfg, ctx);
  REQUIRE(driver.start().ok());

  PacketSpec ps = twelve(10.0, 12.0);
  const auto pkt = build(ps);
  REQUIRE(driver.push_bytes(scanengine::ByteSpan(pkt.data(), pkt.size()),
                            scanengine::TimePoint{9'000'000'000})
              .ok());

  REQUIRE(cap.angles.size() == 12u);
  CHECK(cap.angles[0] == doctest::Approx(10.0));
  CHECK(cap.angles[11] == doctest::Approx(12.0));
  CHECK(cap.ranges[0] == doctest::Approx(1.0));         // 1000 mm
  CHECK(cap.ranges[11] == doctest::Approx(1.407));      // 1000 + 11*37 mm
  for (size_t i = 1; i < cap.times.size(); ++i) CHECK(cap.times[i] > cap.times[i - 1]);
  CHECK(cap.times.back() <= 9'000'000'000);
}

// ===========================================================================
// discovery: the classifier, and the two sensors not eating each other
// ===========================================================================

TEST_CASE("stl27l/discovery_sniffer_identifies_the_wire_signature") {
  using scanengine::discovery::D6Sniffer;
  using scanengine::discovery::Stl27lSniffer;

  const std::vector<uint8_t> rev = build_revolution(30, 1200, 140, 10.0);

  SUBCASE("a clean stream identifies") {
    Stl27lSniffer s;
    CHECK_FALSE(s.Identified());
    s.Feed(rev.data(), rev.size());
    CHECK(s.Identified());
    CHECK(s.packets_ok() >= Stl27lSniffer::kPacketsToIdentify);
    CHECK_EQ(s.packets_bad_crc(), 0u);
    CHECK_EQ(s.speed_dps(), 3600);
    CHECK_FALSE(s.LooksLikeText());
    CHECK_FALSE(s.LooksLikeD6());
  }

  SUBCASE("torn across arbitrary chunk boundaries") {
    Stl27lSniffer s;
    for (size_t i = 0; i < rev.size(); i += 7) {
      s.Feed(rev.data() + i, std::min<size_t>(7, rev.size() - i));
    }
    CHECK(s.Identified());
  }

  SUBCASE("three packets are not enough") {
    Stl27lSniffer s;
    std::vector<uint8_t> few;
    for (int k = 0; k < 3; ++k) append(&few, build(twelve(k * 2.0, k * 2.0 + 2.0)));
    s.Feed(few.data(), few.size());
    CHECK_EQ(s.packets_ok(), 3u);
    CHECK_FALSE(s.Identified());
  }

  SUBCASE("CRC-valid but implausible headers do not identify") {
    // A stopped rotor (speed 0) with a zero angle span: the CRC is perfect,
    // the frame is not a scanning lidar. The sanity band is what rejects it.
    Stl27lSniffer s;
    std::vector<uint8_t> stream;
    for (int k = 0; k < 8; ++k) {
      PacketSpec ps = twelve(0.0, 0.0);
      ps.speed_dps = 0;
      append(&stream, build(ps));
    }
    s.Feed(stream.data(), stream.size());
    CHECK_EQ(s.packets_ok(), 8u);   // the CRCs really do pass
    CHECK_FALSE(s.Identified());    // and it is still not identified
  }

  SUBCASE("random binary identifies nothing") {
    // 64 KB, which is ~0.7 s of a 921600 link — a whole discovery dwell. The
    // 0x54 0x2C sync turns up about once per 64 KB by chance and then has to
    // survive an 8-bit CRC, so ZERO accepted packets is the expected outcome
    // and four is unreachable. Asserted on the packet counter rather than on
    // Identified() alone, so the claim cannot be satisfied by the text latch
    // firing instead (over this much random data an accidental "$G" is
    // likely, which is a property of the latch and not evidence about the
    // lidar).
    Stl27lSniffer s;
    std::vector<uint8_t> noise(65536);
    std::uint32_t x = 0x12345678u;
    for (uint8_t& c : noise) {
      x = x * 1664525u + 1013904223u;
      c = static_cast<uint8_t>(x >> 24);
    }
    s.Feed(noise.data(), noise.size());
    CHECK(s.packets_ok() < Stl27lSniffer::kPacketsToIdentify);
    CHECK_FALSE(s.Identified());
    CHECK_FALSE(s.LooksLikeD6());
  }

  SUBCASE("a short burst of binary noise latches nothing at all") {
    // 4 KB — the same fixture size tests/test_discovery.cpp uses for the D6
    // sniffer, small enough that an accidental text latch is not expected.
    Stl27lSniffer s;
    std::vector<uint8_t> noise(4096);
    std::uint32_t x = 0x9E3779B9u;
    for (uint8_t& c : noise) {
      x = x * 1664525u + 1013904223u;
      c = static_cast<uint8_t>(x >> 24);
    }
    s.Feed(noise.data(), noise.size());
    CHECK_FALSE(s.Identified());
    CHECK_FALSE(s.LooksLikeText());
    CHECK_FALSE(s.LooksLikeD6());
  }

  SUBCASE("NMEA text latches and blocks identification") {
    Stl27lSniffer s;
    const char* nmea =
        "$GNGGA,000000.00,2216.980000,N,11409.510000,E,4,22,0.6,50.00,M,-2.0,M,,*4A\r\n"
        "$GPTHS,123.4,A*2B\r\n";
    s.Feed(reinterpret_cast<const uint8_t*>(nmea), std::strlen(nmea));
    CHECK(s.LooksLikeText());
    CHECK_FALSE(s.Identified());
  }

  SUBCASE("reset clears everything") {
    Stl27lSniffer s;
    s.Feed(rev.data(), rev.size());
    REQUIRE(s.Identified());
    s.Reset();
    CHECK_FALSE(s.Identified());
    CHECK_EQ(s.packets_ok(), 0u);
    CHECK_EQ(s.speed_dps(), 0);
  }
}

TEST_CASE("stl27l/the_two_serial_lidars_do_not_misidentify_each_other") {
  using scanengine::discovery::D6Sniffer;
  using scanengine::discovery::Stl27lSniffer;

  const std::vector<uint8_t> stl = build_revolution(60, 1200, 140, 10.0);
  const std::vector<uint8_t> d6 = d6test::build_revolution(20, 40, 1000, 128, 10);

  SUBCASE("a COIN-D6 stream is NOT an STL-27L") {
    Stl27lSniffer s;
    s.Feed(d6.data(), d6.size());
    CHECK_FALSE(s.Identified());
    // And it says WHY, rather than merely failing to reach four packets: the
    // explicit cross-check latched. This is the half of the guarantee that
    // does not depend on the two probes opening at different baud rates.
    CHECK(s.LooksLikeD6());
  }

  SUBCASE("an STL-27L stream is NOT a COIN-D6") {
    D6Sniffer d;
    d.Feed(stl.data(), stl.size());
    CHECK_FALSE(d.Identified());
    CHECK_FALSE(d.LooksLikeText());
  }

  SUBCASE("even interleaved, neither claims the other's packets") {
    // Not a real wire condition -- a port carries one device -- but it is the
    // strongest form of the question: with both signatures present, does
    // either classifier accept the other's frames as its own?
    std::vector<uint8_t> mixed;
    for (size_t i = 0; i < 40; ++i) {
      append(&mixed, build(twelve(i * 2.0, i * 2.0 + 2.0)));
      d6test::PacketSpec sp;
      sp.first_angle_deg = static_cast<double>(i);
      sp.last_angle_deg = static_cast<double>(i) + 9.0;
      for (int k = 0; k < 10; ++k) sp.samples.push_back(d6test::Sample{1000, 100, false});
      d6test::append(&mixed, d6test::build(sp));
    }
    Stl27lSniffer s;
    s.Feed(mixed.data(), mixed.size());
    CHECK(s.LooksLikeD6());
    CHECK_FALSE(s.Identified());  // the D6 latch wins, by design

    D6Sniffer d;
    d.Feed(mixed.data(), mixed.size());
    CHECK(d.Identified());  // the D6's own packets are still there and valid
  }
}

TEST_CASE("stl27l/probe_over_no_ports_is_a_clean_empty_answer") {
  // The port-walking probe with nothing to walk: not an error, no I/O, no
  // write. (Mirrors discovery/serial_enumeration_and_empty_probes.)
  const std::vector<std::string> none;
  CHECK_FALSE(scanengine::discovery::ProbeSerialStl27l(none, 10).has_value());
}


// ===========================================================================
// the C ABI — the exact value an app puts in scan_device_config::kind
// ===========================================================================

TEST_CASE("stl27l/capi_selects_the_driver_with_SCAN_DEVICE_STL27L") {
  // THIS IS THE CONTRACT THE ANDROID APP CODES AGAINST. No new function, no
  // struct change, no ABI bump: a new VALUE of an existing enum field, driven
  // through the entry points that already exist.
  CHECK_EQ(SCAN_DEVICE_STL27L, 4);
  CHECK_EQ(SCAN_STREAM_LIDAR_STL27L, 11);
  CHECK_EQ(scan_engine_abi_version(), 12u);  // deliberately NOT bumped

  scan_engine_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "stl27l-capi";
  cfg.log_level = SCAN_LOG_OFF;
  cfg.page_capacity = 4096;
  cfg.max_pages = 8;

  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(&cfg, &e) == SCAN_OK);

  scan_device_config dev;
  std::memset(&dev, 0, sizeof(dev));
  dev.kind = SCAN_DEVICE_STL27L;
  dev.serial_port_name = "capi-stl27l";
  // serial_baud left at 0 on purpose: the driver fills in 921600, NOT the D6's
  // 230400. That default is load-bearing — the baud is what dates every point.
  std::uint32_t device_id = 0;
  REQUIRE(scan_engine_add_device(e, &dev, &device_id) == SCAN_OK);

  scan_session_config session;
  std::memset(&session, 0, sizeof(session));
  session.record = 0;
  REQUIRE(scan_engine_start(e, &session) == SCAN_OK);

  const auto bytes = build_revolution(30, 2000, 90, 10.0);
  REQUIRE(scan_engine_push_serial_bytes(e, device_id, bytes.data(), bytes.size(), 12345) ==
          SCAN_OK);

  int points_events = 0, device_events = 0;
  std::uint32_t points_total = 0;
  scan_event ev;
  while (scan_engine_poll_event(e, &ev) == SCAN_OK) {
    if (ev.type == SCAN_EVENT_POINTS_AVAILABLE) {
      ++points_events;
      points_total += ev.payload.points.count;
      CHECK(ev.payload.points.stream == SCAN_STREAM_LIDAR_STL27L);
    } else if (ev.type == SCAN_EVENT_DEVICE_STATE) {
      ++device_events;
      CHECK(ev.payload.device.kind == SCAN_DEVICE_STL27L);
    }
  }
  CHECK(points_events > 0);
  CHECK_EQ(points_total, 360u);  // 30 packets x 12 returns
  CHECK(device_events >= 2);

  scan_device_health h;
  std::memset(&h, 0, sizeof(h));
  REQUIRE(scan_engine_device_health(e, device_id, &h) == SCAN_OK);
  CHECK_EQ(h.kind, SCAN_DEVICE_STL27L);
  CHECK_EQ(h.state, SCAN_DEV_STREAMING);
  CHECK_EQ(h.packets_ok, 30u);
  CHECK_EQ(h.packets_bad, 0u);

  CHECK(scan_engine_stop(e) == SCAN_OK);
  scan_engine_destroy(e);
}

TEST_CASE("stl27l/capi_rejects_a_device_kind_this_build_does_not_know") {
  // The hardening that had to come with an enum an app may hold a NEWER value
  // of: an out-of-range `kind` used to fall out of Engine::add_device()'s
  // switch with a null driver and be inserted into the device map anyway.
  scan_engine_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "stl27l-capi";
  cfg.log_level = SCAN_LOG_OFF;
  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(&cfg, &e) == SCAN_OK);

  scan_device_config dev;
  std::memset(&dev, 0, sizeof(dev));
  dev.kind = 99;
  std::uint32_t device_id = 0;
  CHECK(scan_engine_add_device(e, &dev, &device_id) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK_EQ(device_id, 0u);
  scan_engine_destroy(e);
}


// ===========================================================================
// record-always: the STL-27L's bytes land in the container as their OWN type
// ===========================================================================

TEST_CASE("stl27l/raw_bytes_are_recorded_as_kStl27lRaw_not_kD6Raw") {
  // The consequence of getting this wrong is silent and expensive: an
  // STL-27L capture recorded as kD6Raw would make
  // post::lscan_is_d6_project() answer YES and hand 47-byte LD frames to the
  // COIN-D6 parser, which decodes nothing and blames the capture.
  namespace fs = std::filesystem;
  const fs::path dir =
      fs::temp_directory_path() / ("stl27l-record-" + std::to_string(static_cast<unsigned long long>(
                                                std::hash<std::string>{}("stl27l-item119"))) + ".lscan");
  std::error_code ec;
  fs::remove_all(dir, ec);

  {
    scanengine::EngineConfig ecfg;
    ecfg.app_name = "stl27l-record";
    ecfg.log_level = scanengine::LogLevel::kOff;
    ecfg.points.page_capacity = 4096;
    ecfg.points.max_pages = 8;
    auto engine = scanengine::Engine::create(ecfg);
    REQUIRE(engine.ok());
    scanengine::Engine& e = *engine.value();

    scanengine::DeviceConfig dc;
    dc.kind = scanengine::DeviceKind::kStl27l;
    dc.stl27l.serial.port_name = "test";
    auto id = e.add_device(dc);
    REQUIRE(id.ok());

    scanengine::SessionConfig sc;
    sc.record = true;
    sc.lscan_dir = dir.string();
    REQUIRE(e.start_session(sc).ok());

    const auto bytes = build_revolution(20, 1000, 128, 10.0);
    REQUIRE(e.push_serial_bytes(id.value(),
                                scanengine::ByteSpan(bytes.data(), bytes.size()))
                .ok());
    REQUIRE(e.stop_session().ok());
  }

  scanengine::lscan::FileRecordReader reader;
  REQUIRE(reader.open(dir.string()).ok());
  scanengine::lscan::ChunkHeader h{};
  std::vector<std::uint8_t> payload;
  int stl_chunks = 0, d6_chunks = 0;
  std::size_t stl_bytes = 0;
  while (reader.next_chunk(&h, &payload).ok()) {
    if (h.type == scanengine::lscan::ChunkType::kStl27lRaw) {
      ++stl_chunks;
      stl_bytes += payload.size();
    } else if (h.type == scanengine::lscan::ChunkType::kD6Raw) {
      ++d6_chunks;
    }
  }
  (void)reader.close();
  CHECK(stl_chunks > 0);
  CHECK_EQ(d6_chunks, 0);
  CHECK_EQ(stl_bytes, 20u * stl27l::kPacketBytes);
  // And the chunk type maps to the STL-27L's own stream, sharing lidar.bin
  // the way kLidarMid360 already does.
  CHECK(scanengine::lscan::stream_of(scanengine::lscan::ChunkType::kStl27lRaw) ==
        scanengine::StreamId::kLidarStl27l);

  fs::remove_all(dir, ec);
}
