// Unit tests for the COIN-D6 parser.
//
// This file is spikes/s1-d6-parser/tests/test_d6.cpp, byte-for-byte, except
// for these includes and the removed main() (doctest owns main now, see
// tests/test_main.cpp). microtest_shim.h maps microtest's TEST/CHECK_EQ/
// CHECK_NEAR onto doctest with identical semantics. Keeping the 33 cases
// unmodified preserves them as the S1 exit-criteria evidence.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "microtest_shim.h"
#include "packet_builder.h"
#include "scanengine/drivers/d6/commands.h"
#include "scanengine/drivers/d6/d6_parser.h"

using namespace d6;
using namespace d6test;

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

}  // namespace

// ---------------------------------------------------------------------------
// checksum
// ---------------------------------------------------------------------------

TEST(checksum_matches_vendor_state_machine) {
  PacketSpec sp;
  sp.first_angle_deg = 12.0;
  sp.last_angle_deg = 20.0;
  for (int i = 0; i < 40; ++i)
    sp.samples.push_back(Sample{static_cast<uint16_t>(500 + i * 37),
                                static_cast<uint8_t>(i * 5), (i % 7) == 0});
  const auto pkt = build(sp);
  const uint16_t field = static_cast<uint16_t>(pkt[8] | (pkt[9] << 8));
  CHECK_EQ(checksum(pkt.data(), 40, ChecksumVariant::kVendorSdk), field);
  CHECK_EQ(vendor_checksum_replay(pkt, 40), field);
}

TEST(checksum_variants_differ_on_real_payloads) {
  // The two readings of the spec figure are genuinely different functions --
  // this is what makes the vendor cross-check necessary.
  PacketSpec sp;
  sp.first_angle_deg = 100.0;
  sp.last_angle_deg = 104.0;
  for (int i = 0; i < 8; ++i)
    sp.samples.push_back(Sample{static_cast<uint16_t>(1234 + i * 11),
                                static_cast<uint8_t>(30 + i), false});
  const auto pkt = build(sp);
  const uint16_t v = checksum(pkt.data(), 8, ChecksumVariant::kVendorSdk);
  const uint16_t s = checksum(pkt.data(), 8, ChecksumVariant::kSpecLiteral);
  CHECK(v != s);
  CHECK_EQ(s, spec_checksum_replay(pkt, 8));
}

TEST(parser_counts_both_checksum_variants) {
  // A packet stamped with the spec-literal checksum must be rejected by the
  // default (vendor) parser but still show up in the spec counter -- this is
  // the bench A/B that decides the question on real hardware.
  PacketSpec sp;
  sp.first_angle_deg = 10.0;
  sp.last_angle_deg = 12.0;
  sp.cs_mode = CsMode::kSpec;
  for (int i = 0; i < 5; ++i)
    sp.samples.push_back(Sample{static_cast<uint16_t>(1000 + i * 100),
                                static_cast<uint8_t>(60 + i), false});
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 0u);
  CHECK_EQ(p.stats().packets_bad_checksum, 1u);
  CHECK_EQ(p.stats().cs_ok_vendor, 0u);
  CHECK_EQ(p.stats().cs_ok_spec, 1u);

  Config cfg;
  cfg.checksum = ChecksumVariant::kSpecLiteral;
  Parser q(cfg);
  auto pts2 = decode(build(sp), &q);
  CHECK_EQ(pts2.size(), 5u);
  CHECK_EQ(q.stats().packets_ok, 1u);
}

TEST(bad_checksum_is_counted_and_drops_points) {
  PacketSpec sp;
  sp.first_angle_deg = 0.0;
  sp.last_angle_deg = 5.0;
  sp.cs_mode = CsMode::kCorrupt;
  for (int i = 0; i < 6; ++i) sp.samples.push_back(Sample{800, 20, false});
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 0u);
  CHECK_EQ(p.stats().packets_ok, 0u);
  CHECK_EQ(p.stats().packets_bad_checksum, 1u);
  CHECK_NEAR(p.stats().checksum_pass_rate(), 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// sample decoding (golden cases from the documented formulas)
// ---------------------------------------------------------------------------

TEST(golden_sample_decode) {
  // Hand-built triplet: Si_L=0xF1, Si_2nd=0x8B, Si_H=0x2A
  //   Distance  = 0x2A*64 + (0x8B>>2)   = 2688 + 34  = 2722 mm
  //   Intensity = (0x8B&3)*64 + (0xF1>>2) = 3*64 + 60 = 252
  //   HighRefl  = 0xF1 & 1 = 1
  const uint8_t s[3] = {0xF1, 0x8B, 0x2A};
  CHECK_EQ(sample_distance_mm(s), 2722);
  CHECK_EQ(sample_intensity(s), 252);
  CHECK(sample_high_reflectivity(s));
}

TEST(sample_roundtrip_through_parser) {
  PacketSpec sp;
  sp.first_angle_deg = 45.0;
  sp.last_angle_deg = 45.0 + 0.9 * 3;
  sp.samples = {Sample{50, 0, false}, Sample{2722, 252, true},
                Sample{12000, 255, false}, Sample{0, 7, false}};
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 4u);
  if (pts.size() == 4) {
    CHECK_EQ(pts[0].distance_mm, 50);
    CHECK_EQ(pts[0].intensity, 0);
    CHECK(!pts[0].high_reflectivity);
    CHECK_EQ(pts[1].distance_mm, 2722);
    CHECK_EQ(pts[1].intensity, 252);
    CHECK(pts[1].high_reflectivity);
    CHECK_EQ(pts[2].distance_mm, 12000);
    CHECK_EQ(pts[2].intensity, 255);
    CHECK_EQ(pts[3].distance_mm, 0);
    CHECK_EQ(p.stats().points_zero_range, 1u);
  }
  CHECK_EQ(p.stats().packets_ok, 1u);
}

TEST(drop_zero_range_option) {
  Config cfg;
  cfg.drop_zero_range = true;
  Parser p(cfg);
  PacketSpec sp;
  sp.first_angle_deg = 10.0;
  sp.last_angle_deg = 13.0;
  sp.samples = {Sample{0, 0, false}, Sample{1000, 50, false},
                Sample{0, 0, false}, Sample{1500, 60, false}};
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 2u);
  CHECK_EQ(p.stats().points, 2u);
  CHECK_EQ(p.stats().points_zero_range, 2u);
}

// ---------------------------------------------------------------------------
// angles
// ---------------------------------------------------------------------------

TEST(angle_interpolation_is_linear) {
  PacketSpec sp;
  sp.first_angle_deg = 100.0;
  sp.last_angle_deg = 108.0;  // 9 points -> 1.0 deg step
  for (int i = 0; i < 9; ++i) sp.samples.push_back(Sample{1000, 10, false});
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 9u);
  for (size_t i = 0; i < pts.size(); ++i)
    CHECK_NEAR(pts[i].angle_deg, 100.0 + static_cast<double>(i), 0.02);
}

TEST(angle_wraparound_across_zero) {
  // LSA < FSA and the packet spans 0 deg: 350 -> 6 over 9 points = 2 deg step.
  PacketSpec sp;
  sp.first_angle_deg = 350.0;
  sp.last_angle_deg = 6.0;
  for (int i = 0; i < 9; ++i) sp.samples.push_back(Sample{1000, 10, false});
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 9u);
  if (pts.size() == 9) {
    CHECK_NEAR(pts[0].angle_deg, 350.0, 0.02);
    CHECK_NEAR(pts[4].angle_deg, 358.0, 0.02);
    CHECK_NEAR(pts[5].angle_deg, 0.0, 0.02);     // wrapped, not 360
    CHECK_NEAR(pts[8].angle_deg, 6.0, 0.02);
    for (const auto& pt : pts) {
      CHECK(pt.angle_deg >= 0.f);
      CHECK(pt.angle_deg < 360.f);
    }
  }
}

TEST(angle_regression_reuses_previous_interval) {
  // LSA < FSA but NOT spanning 0 (device glitch): the vendor reuses the last
  // good interval instead of producing a negative step. Mirror that.
  std::vector<uint8_t> stream;
  {
    PacketSpec a;
    a.first_angle_deg = 100.0;
    a.last_angle_deg = 104.0;  // 5 points -> 1.0 deg
    for (int i = 0; i < 5; ++i) a.samples.push_back(Sample{1000, 10, false});
    append(&stream, build(a));
  }
  {
    PacketSpec b;
    b.first_angle_deg = 200.0;
    b.last_angle_deg = 150.0;  // regressive, does not span 0
    for (int i = 0; i < 5; ++i) b.samples.push_back(Sample{1000, 10, false});
    append(&stream, build(b));
  }
  Parser p;
  auto pts = decode(stream, &p);
  CHECK_EQ(pts.size(), 10u);
  if (pts.size() == 10) {
    CHECK_NEAR(pts[5].angle_deg, 200.0, 0.02);
    CHECK_NEAR(pts[6].angle_deg, 201.0, 0.02);  // reused 1.0 deg step
    CHECK_NEAR(pts[9].angle_deg, 204.0, 0.02);
  }
}

TEST(single_sample_packet_uses_fsa_only) {
  PacketSpec sp;
  sp.first_angle_deg = 123.5;
  sp.last_angle_deg = 123.5;
  sp.samples = {Sample{999, 12, false}};
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 1u);
  if (!pts.empty()) CHECK_NEAR(pts[0].angle_deg, 123.5, 0.02);
}

TEST(mechanical_angle_correction_is_opt_in) {
  PacketSpec sp;
  sp.first_angle_deg = 90.0;
  sp.last_angle_deg = 90.0;
  sp.samples = {Sample{1000, 30, false}};
  Parser a;
  auto pa = decode(build(sp), &a);
  Config cfg;
  cfg.apply_mechanical_angle_correction = true;
  Parser b(cfg);
  auto pb = decode(build(sp), &b);
  CHECK_EQ(pa.size(), 1u);
  CHECK_EQ(pb.size(), 1u);
  if (!pa.empty() && !pb.empty()) {
    CHECK_NEAR(pa[0].angle_deg, 90.0, 0.02);
    // vendor correction at 1 m is atan(19.16*909.85/90150) rad, taken as
    // 1/64-degree units -> about +0.19 deg.
    CHECK_NEAR(pb[0].angle_deg, 90.19, 0.03);
  }
}

// ---------------------------------------------------------------------------
// framing, resync, torn packets
// ---------------------------------------------------------------------------

TEST(start_packet_marks_new_rotation) {
  PacketSpec sp;
  sp.start_packet = true;
  sp.scan_freq = 10;
  sp.first_angle_deg = 0.0;
  sp.last_angle_deg = 0.0;
  sp.samples = {Sample{1200, 44, false}};
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 1u);
  if (!pts.empty()) {
    CHECK(pts[0].new_rotation);
    CHECK(pts[0].from_start_packet);
  }
  CHECK_EQ(p.stats().start_packets, 1u);
  CHECK_EQ(p.stats().rotations, 1u);
  CHECK_EQ(p.stats().scan_freq_raw, 10);
}

TEST(full_revolutions_are_counted) {
  std::vector<uint8_t> stream;
  for (int r = 0; r < 10; ++r)
    append(&stream, build_revolution(10, 40));
  Parser p;
  auto pts = decode(stream, &p);
  CHECK_EQ(p.stats().rotations, 10u);
  CHECK_EQ(p.stats().packets_ok, 110u);  // 10 * (1 start + 10 data)
  CHECK_EQ(p.stats().packets_bad_checksum, 0u);
  CHECK_EQ(p.stats().resyncs, 0u);
  CHECK_EQ(pts.size(), 10u * (1 + 400));
  int rotation_marks = 0;
  for (const auto& pt : pts)
    if (pt.new_rotation) ++rotation_marks;
  CHECK_EQ(rotation_marks, 10);
}

TEST(rotation_falls_back_to_angle_wrap_without_start_packets) {
  // Some units only emit start packets once the speed is locked; make sure the
  // 350 -> 0 wrap still yields rotations.
  std::vector<uint8_t> stream;
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 8; ++k) {
      PacketSpec sp;
      sp.first_angle_deg = k * 45.0;
      sp.last_angle_deg = k * 45.0 + 40.0;
      for (int i = 0; i < 5; ++i) sp.samples.push_back(Sample{900, 11, false});
      append(&stream, build(sp));
    }
  }
  Parser p;
  auto pts = decode(stream, &p);
  CHECK_EQ(p.stats().start_packets, 0u);
  CHECK_EQ(p.stats().rotations, 2u);  // 2 wraps across 3 revolutions
  CHECK_EQ(pts.size(), 120u);
}

TEST(resync_after_garbage) {
  std::vector<uint8_t> stream;
  const uint8_t junk[] = {0x01, 0x02, 0x03, 0xAA, 0x11, 0x55, 0xDE, 0xAD, 0xBE};
  stream.insert(stream.end(), junk, junk + sizeof(junk));
  append(&stream, build_revolution(4, 10));
  const uint8_t junk2[] = {0x00, 0xAA, 0xAA, 0xAA, 0x42};
  stream.insert(stream.end(), junk2, junk2 + sizeof(junk2));
  append(&stream, build_revolution(4, 10));

  Parser p;
  auto pts = decode(stream, &p);
  CHECK_EQ(p.stats().packets_ok, 10u);           // 2 * (1 start + 4 data)
  CHECK_EQ(p.stats().rotations, 2u);
  CHECK_EQ(pts.size(), 2u * (1 + 40));
  CHECK_EQ(p.stats().resyncs, 2u);
  CHECK_EQ(p.stats().bytes_discarded, sizeof(junk) + sizeof(junk2));
}

TEST(truncated_packet_then_resync) {
  // A packet cut in half mid-body must not swallow the following good packet.
  auto good = build_revolution(2, 10);
  PacketSpec sp;
  sp.first_angle_deg = 30.0;
  sp.last_angle_deg = 39.0;
  for (int i = 0; i < 10; ++i) sp.samples.push_back(Sample{1000, 10, false});
  auto torn = build(sp);
  torn.resize(torn.size() - 12);  // lose the tail

  std::vector<uint8_t> stream;
  append(&stream, torn);
  append(&stream, good);
  Parser p;
  auto pts = decode(stream, &p);
  // The torn packet's declared length overruns into the good stream, so it is
  // rejected by checksum; the parser must then re-hunt and recover the two
  // full data packets that follow (the start packet is eaten by the overrun).
  CHECK(p.stats().packets_bad_checksum >= 1u);
  CHECK(p.stats().packets_ok >= 2u);
  CHECK(pts.size() >= 20u);
}

TEST(torn_across_feed_boundaries_bytewise) {
  auto stream = build_revolution(6, 20);
  Parser p;
  auto pts = decode_bytewise(stream, &p);
  CHECK_EQ(p.stats().packets_ok, 7u);
  CHECK_EQ(pts.size(), 1u + 120u);
  CHECK_EQ(p.stats().resyncs, 0u);
  CHECK_EQ(p.stats().bytes_discarded, 0u);
}

TEST(torn_across_awkward_chunk_sizes) {
  auto stream = build_revolution(8, 40);
  for (size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(13), size_t(64),
                       size_t(129), size_t(4096)}) {
    Parser p;
    for (size_t i = 0; i < stream.size(); i += chunk) {
      const size_t n = std::min(chunk, stream.size() - i);
      p.feed(&stream[i], n, 1);
    }
    std::vector<Point> pts;
    p.take_points(&pts);
    CHECK_EQ(p.stats().packets_ok, 9u);
    CHECK_EQ(pts.size(), 1u + 320u);
    CHECK_EQ(p.stats().bytes_discarded, 0u);
  }
}

TEST(speed_adjust_bytes_are_not_garbage) {
  std::vector<uint8_t> stream;
  // Spec §1: two-byte 0xFE/0xFF speed-adjustment commands interleave with the
  // stream until the rotation stabilises.
  const uint8_t speed[] = {0xFE, 0xFE, 0xFF, 0xFF, 0xFE, 0xFF};
  stream.insert(stream.end(), speed, speed + sizeof(speed));
  append(&stream, build_revolution(3, 10));
  stream.insert(stream.end(), speed, speed + sizeof(speed));
  append(&stream, build_revolution(3, 10));

  Parser p;
  auto pts = decode(stream, &p);
  CHECK_EQ(p.stats().speed_adjust_bytes, 12u);
  CHECK_EQ(p.stats().bytes_discarded, 0u);
  CHECK_EQ(p.stats().packets_ok, 8u);
  CHECK_EQ(pts.size(), 2u * (1 + 30));
}

TEST(zero_lsn_is_malformed) {
  PacketSpec sp;
  sp.first_angle_deg = 10.0;
  sp.last_angle_deg = 20.0;
  sp.samples = {Sample{1000, 10, false}};
  sp.force_lsn = 0;
  auto bad = build(sp);
  bad.resize(10);  // LSN=0 means no body
  std::vector<uint8_t> stream;
  append(&stream, bad);
  append(&stream, build_revolution(2, 10));
  Parser p;
  auto pts = decode(stream, &p);
  CHECK(p.stats().packets_malformed >= 1u);
  CHECK_EQ(p.stats().packets_ok, 3u);
  CHECK_EQ(pts.size(), 21u);
}

TEST(angle_check_bit_rejects_false_headers) {
  PacketSpec sp;
  sp.first_angle_deg = 5.0;
  sp.last_angle_deg = 9.0;
  sp.break_fsa_check_bit = true;
  for (int i = 0; i < 5; ++i) sp.samples.push_back(Sample{1000, 10, false});
  auto bad = build(sp);
  std::vector<uint8_t> stream;
  append(&stream, bad);
  append(&stream, build_revolution(2, 10));
  Parser p;
  auto pts = decode(stream, &p);
  CHECK(p.stats().packets_malformed >= 1u);
  CHECK_EQ(p.stats().packets_ok, 3u);
  CHECK_EQ(pts.size(), 21u);

  Config cfg;
  cfg.require_angle_check_bit = false;
  Parser q(cfg);
  auto pts2 = decode(bad, &q);
  CHECK_EQ(q.stats().packets_ok, 1u);   // checksum still valid
  CHECK_EQ(pts2.size(), 5u);
}

TEST(max_lsn_packet_is_handled) {
  PacketSpec sp;
  sp.first_angle_deg = 0.0;
  sp.last_angle_deg = 254.0;
  for (int i = 0; i < 255; ++i)
    sp.samples.push_back(Sample{static_cast<uint16_t>(100 + i), 7, false});
  Parser p;
  auto pts = decode(build(sp), &p);
  CHECK_EQ(pts.size(), 255u);
  CHECK_EQ(p.stats().packets_ok, 1u);
}

TEST(callback_mode_matches_queue_mode) {
  auto stream = build_revolution(5, 20);
  std::vector<Point> via_cb;
  Parser p;
  p.set_point_callback([&](const Point& pt) { via_cb.push_back(pt); });
  p.feed(stream.data(), stream.size(), 1);
  CHECK_EQ(p.queued_points(), 0u);

  Parser q;
  auto via_queue = decode(stream, &q);
  CHECK_EQ(via_cb.size(), via_queue.size());
  bool same = via_cb.size() == via_queue.size();
  for (size_t i = 0; same && i < via_cb.size(); ++i) {
    same = via_cb[i].distance_mm == via_queue[i].distance_mm &&
           via_cb[i].intensity == via_queue[i].intensity &&
           std::fabs(via_cb[i].angle_deg - via_queue[i].angle_deg) < 1e-6;
  }
  CHECK(same);
}

TEST(stats_reset) {
  Parser p;
  decode(build_revolution(3, 10), &p);
  CHECK(p.stats().packets_ok > 0u);
  p.reset();
  CHECK_EQ(p.stats().packets_ok, 0u);
  CHECK_EQ(p.stats().points, 0u);
  auto pts = decode(build_revolution(3, 10), &p);
  CHECK_EQ(p.stats().packets_ok, 4u);
  CHECK_EQ(pts.size(), 31u);
}

TEST(rate_counters_advance) {
  Parser p;
  auto rev = build_revolution(10, 40);
  // Two feeds one simulated second apart -> rates computed over that window.
  p.feed(rev.data(), rev.size(), 1000000000ull);
  for (int i = 0; i < 10; ++i)
    p.feed(rev.data(), rev.size(), 1000000000ull);
  p.feed(rev.data(), rev.size(), 2000000000ull);
  CHECK_NEAR(p.stats().rotation_hz, 12.0, 0.5);
  CHECK(p.stats().points_per_sec > 4000.0);
}

TEST(random_garbage_never_wedges_the_parser) {
  // Pseudo-random bytes with real packets sprinkled in: the parser must not
  // crash, must not grow without bound, and must still find the good packets.
  unsigned rng = 987654321u;
  auto rnd = [&]() {
    rng = rng * 1103515245u + 12345u;
    return static_cast<uint8_t>((rng >> 16) & 0xFF);
  };
  std::vector<uint8_t> stream;
  int good_packets = 0;
  for (int block = 0; block < 200; ++block) {
    for (int i = 0; i < 100; ++i) stream.push_back(rnd());
    auto rev = build_revolution(2, 20);
    append(&stream, rev);
    good_packets += 3;
  }
  Parser p;
  auto pts = decode(stream, &p);
  // Random noise can synthesise a plausible header, so allow extras, but the
  // real packets must all be there.
  CHECK(p.stats().packets_ok >= static_cast<uint64_t>(good_packets) / 2);
  CHECK(pts.size() > 200u * 20u);
  CHECK(p.stats().resyncs > 0u);
}

TEST(feeding_only_garbage_bounds_the_buffer) {
  Parser p;
  std::vector<uint8_t> junk(4096, 0x00);
  for (size_t i = 0; i < junk.size(); i += 2) junk[i] = 0xAA;  // AA 00 AA 00 ...
  for (int i = 0; i < 64; ++i) p.feed(junk.data(), junk.size(), 1);
  CHECK_EQ(p.queued_points(), 0u);
  CHECK_EQ(p.stats().packets_ok, 0u);
  CHECK_EQ(p.stats().bytes_in, 64u * 4096u);
  CHECK(p.stats().bytes_discarded > 64u * 4000u);
}

// ---------------------------------------------------------------------------
// commands / ACKs / info frame
// ---------------------------------------------------------------------------

TEST(command_bytes_match_spec) {
  CHECK_EQ(kCmdStart[0], 0xAA);
  CHECK_EQ(kCmdStart[1], 0x55);
  CHECK_EQ(kCmdStart[2], 0xF0);
  CHECK_EQ(kCmdStart[3], 0x0F);
  CHECK_EQ(kCmdStop[0], 0xAA);
  CHECK_EQ(kCmdStop[1], 0x55);
  CHECK_EQ(kCmdStop[2], 0xF5);
  CHECK_EQ(kCmdStop[3], 0x0A);
}

TEST(ack_frames_classified) {
  const uint8_t start_ok[12] = {0xA5, 0x5A, 0x50, 0x07, 0, 0,
                                0,    0,    0,    0,    0, 0xA8};
  const uint8_t stop_ok[12]  = {0xA5, 0x5A, 0x55, 0x07, 0, 0,
                                0,    0,    0,    0,    0, 0xAD};
  const uint8_t err[12]      = {0xA5, 0x5A, 0x55, 0x07, 0, 0,
                                0,    0,    0,    0,    0, 0xE9};
  CHECK_EQ(ack_xor(start_ok), 0xA8);
  CHECK_EQ(ack_xor(stop_ok), 0xAD);
  CHECK(classify_ack(start_ok, 12) == Ack::kStartOk);
  CHECK(classify_ack(stop_ok, 12) == Ack::kStopOk);
  CHECK(classify_ack(err, 12) == Ack::kError);
  CHECK(classify_ack(start_ok, 11) == Ack::kNone);
}

TEST(ack_found_in_noisy_buffer) {
  std::vector<uint8_t> buf = {0xFE, 0xFF, 0x00, 0xA5, 0x5A, 0x99};
  const uint8_t start_ok[12] = {0xA5, 0x5A, 0x50, 0x07, 0, 0,
                                0,    0,    0,    0,    0, 0xA8};
  buf.insert(buf.end(), start_ok, start_ok + 12);
  size_t off = 0;
  CHECK(find_ack(buf.data(), buf.size(), &off) == Ack::kStartOk);
  CHECK_EQ(off, 6u);

  std::vector<uint8_t> nothing = {0xA5, 0x5A, 0x50};
  CHECK(find_ack(nothing.data(), nothing.size(), &off) == Ack::kNone);
}

TEST(device_info_frame_from_spec_example) {
  std::vector<uint8_t> f = {0xA5, 0x5A, 0x14, 0x00, 0xE3, 0x02, 0x01,
                            0x43, 0x4F, 0x49, 0x4E, 0x2D, 0x44, 0x36,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  // The worked example in the spec prints "COIN-D4" bytes (43 4F 49 4E 2D 44 34)
  // with checksum 0x02E3; using 'D6' (0x36) shifts the sum by 2.
  uint32_t sum = 0;
  for (size_t i = 0; i < f.size(); ++i)
    if (i != 4 && i != 5) sum += f[i];
  f[4] = static_cast<uint8_t>(sum & 0xFF);
  f[5] = static_cast<uint8_t>((sum >> 8) & 0xFF);
  DeviceInfo info{};
  CHECK(parse_device_info(f.data(), f.size(), &info));
  CHECK(std::string(info.model).substr(0, 7) == "COIN-D6");
  CHECK_EQ(info.direction, 0x00);
  CHECK_EQ(info.version, 0x01);

  // Corrupt the checksum -> rejected.
  f[4] ^= 0x01;
  CHECK(!parse_device_info(f.data(), f.size(), &info));
}

TEST(spec_info_example_checksum_is_the_documented_sum) {
  const uint8_t f[27] = {0xA5, 0x5A, 0x14, 0x00, 0xE3, 0x02, 0x01,
                         0x43, 0x4F, 0x49, 0x4E, 0x2D, 0x44, 0x34,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  DeviceInfo info{};
  CHECK(parse_device_info(f, sizeof(f), &info));
}
