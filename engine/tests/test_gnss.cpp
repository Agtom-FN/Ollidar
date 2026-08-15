// test_gnss.cpp — A10: NMEA, RTCM3, CRS, GNSS source, georef fusion, NTRIP.
//
// Header-first and alone, so this file doubles as the self-containment check
// tests/test_headers.cpp performs for the A1 seams (that file is not A10's to
// edit — see docs/A10-gnss.md §9).
#include "scanengine/gnss/georef.h"

#include "scanengine/gnss/crs.h"
#include "scanengine/gnss/gnss.h"
#include "scanengine/gnss/gnss_source.h"
#include "scanengine/gnss/nmea.h"
#include "scanengine/gnss/ntrip_client.h"
#include "scanengine/gnss/rtcm3.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

using namespace scanengine;

namespace {

// Deterministic PRNG. Same reasoning as test_timesync.cpp: <random>'s
// distributions are not specified by the standard, so the five CI legs would
// disagree on the numbers this file asserts.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  // Box–Muller, written out: no std::normal_distribution.
  double gauss() {
    const double u1 = std::max(1e-12, uniform());
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.14159265358979323846 * u2);
  }
};

std::string with_checksum(const std::string& body) {
  char cs[8];
  std::snprintf(cs, sizeof(cs), "*%02X", nmea::checksum_of(body));
  return "$" + body + cs + "\r\n";
}

// A minimal, standards-shaped sentence builder mirroring the S5 simulator's
// field layout, so the offline cases here and the live sim cases below exercise
// the same parser paths.
struct SimEpoch {
  double sod = 0.0;
  double lat = 22.2830, lon = 114.1585, alt = 50.0;
  int quality = 4;
  int sats = 22;
  double hdop = 0.6, pdop = 0.9, vdop = 0.8;
  double sigma_h = 0.02, sigma_v = 0.03;
  double course = 90.0, speed_mps = 1.0;
  const char* mode = "R";
  int gsa_fix = 3;
  bool emit_gst = true;
};

std::string latf(double lat) {
  const double a = std::fabs(lat);
  char b[32];
  std::snprintf(b, sizeof(b), "%02d%09.6f", static_cast<int>(a), (a - static_cast<int>(a)) * 60.0);
  return b;
}
std::string lonf(double lon) {
  const double a = std::fabs(lon);
  char b[32];
  std::snprintf(b, sizeof(b), "%03d%09.6f", static_cast<int>(a), (a - static_cast<int>(a)) * 60.0);
  return b;
}
std::string timef(double sod) {
  const int hh = static_cast<int>(sod / 3600.0);
  const int mm = static_cast<int>((sod - hh * 3600.0) / 60.0);
  const double ss = sod - hh * 3600.0 - mm * 60.0;
  char b[32];
  std::snprintf(b, sizeof(b), "%02d%02d%05.2f", hh, mm, ss);
  return b;
}

std::string sim_burst(const SimEpoch& e) {
  char buf[512];
  std::string out;

  std::snprintf(buf, sizeof(buf), "GNGGA,%s,%s,%c,%s,%c,%d,%02d,%.1f,%.2f,M,-2.0,M,,",
                timef(e.sod).c_str(), latf(e.lat).c_str(), e.lat < 0 ? 'S' : 'N',
                lonf(e.lon).c_str(), e.lon < 0 ? 'W' : 'E', e.quality, e.sats, e.hdop, e.alt);
  out += with_checksum(buf);

  std::snprintf(buf, sizeof(buf), "GNRMC,%s,%c,%s,%c,%s,%c,%.2f,%.1f,010126,,,%s",
                timef(e.sod).c_str(), e.quality > 0 ? 'A' : 'V', latf(e.lat).c_str(),
                e.lat < 0 ? 'S' : 'N', lonf(e.lon).c_str(), e.lon < 0 ? 'W' : 'E',
                e.speed_mps * 1.9438444924, e.course, e.mode);
  out += with_checksum(buf);

  if (e.emit_gst) {
    std::snprintf(buf, sizeof(buf), "GNGST,%s,%.3f,%.3f,%.3f,0.0,%.3f,%.3f,%.3f",
                  timef(e.sod).c_str(), e.sigma_h, e.sigma_h, e.sigma_h * 0.8, e.sigma_h,
                  e.sigma_h, e.sigma_v);
    out += with_checksum(buf);
  }

  std::snprintf(buf, sizeof(buf), "GNGSA,A,%d,1,2,3,4,5,6,7,8,9,10,11,12,%.1f,%.1f,%.1f",
                e.gsa_fix, e.pdop, e.hdop, e.vdop);
  out += with_checksum(buf);

  std::snprintf(buf, sizeof(buf), "GNVTG,%.1f,T,,M,%.2f,N,%.2f,K,%s", e.course,
                e.speed_mps * 1.9438444924, e.speed_mps * 3.6, e.mode);
  out += with_checksum(buf);
  return out;
}

void push_str(GnssSource& s, const std::string& text, std::int64_t t_ns) {
  s.push_nmea(ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), t_ns)
      .ok();
}

int quality_digit(FixType f) {
  switch (f) {
    case FixType::kRtkFixed: return 4;
    case FixType::kRtkFloat: return 5;
    case FixType::kDgps: return 2;
    case FixType::kSingle: return 1;
    case FixType::kNone: break;
  }
  return 0;
}

const char* mode_char(FixType f) {
  switch (f) {
    case FixType::kRtkFixed: return "R";
    case FixType::kRtkFloat: return "F";
    case FixType::kDgps: return "D";
    case FixType::kSingle: return "A";
    case FixType::kNone: break;
  }
  return "N";
}

}  // namespace

// ===========================================================================
// 1. NMEA parsing
// ===========================================================================

TEST_CASE("gnss/nmea/checksum_is_the_standard_xor") {
  // Cross-checked against the S5 spike's nmea_sim.nmea_checksum, which is an
  // independent implementation: both give 0x64 for this body.
  const std::string body = "GNGGA,000000.00,2216.9800,N,11409.5100,E,4,22,0.6,50.00,M,-2.0,M,,";
  CHECK(nmea::checksum_of(body) == 0x64);
  CHECK(nmea::checksum_of("") == 0x00);
  CHECK(nmea::checksum_of("A") == 'A');
}

TEST_CASE("gnss/nmea/valid_sentence_parses_into_fields") {
  const std::string line = "$GNGGA,013245.50,2216.9800,N,11409.5100,E,4,22,0.6,50.00,M,-2.0,M,1.2,0135*4A";
  nmea::Sentence s;
  // Deliberately wrong checksum in the literal above: fix it, then assert.
  const std::string body = line.substr(1, line.find('*') - 1);
  const std::string good = with_checksum(body);
  const std::string trimmed = good.substr(0, good.size() - 2);

  CHECK(nmea::parse_sentence(trimmed, &s) == nmea::NmeaError::kOk);
  CHECK(s.talker == "GN");
  CHECK(s.type == "GGA");
  CHECK(s.id == nmea::SentenceId::kGga);
  CHECK(s.checksum_present);
  CHECK(s.field_count == 14);
  CHECK(s.field(5) == "4");

  nmea::GgaData g;
  REQUIRE(nmea::decode_gga(s, &g));
  CHECK(g.has_time);
  CHECK(g.utc_sod_s == doctest::Approx(1 * 3600 + 32 * 60 + 45.5));
  CHECK(g.has_position);
  CHECK(g.lat_deg == doctest::Approx(22.2830).epsilon(1e-9));
  CHECK(g.lon_deg == doctest::Approx(114.1585).epsilon(1e-9));
  CHECK(g.fix == FixType::kRtkFixed);
  CHECK(g.quality_raw == 4);
  CHECK(g.satellites == 22);
  CHECK(g.hdop == doctest::Approx(0.6));
  CHECK(g.alt_msl_m == doctest::Approx(50.0));
  CHECK(g.geoid_sep_m == doctest::Approx(-2.0));
  CHECK(g.has_dgps_age);
  CHECK(g.dgps_age_s == doctest::Approx(1.2));
  CHECK(g.station_id == 135);
}

TEST_CASE("gnss/nmea/malformed_sentences_are_classified_not_crashed") {
  nmea::Sentence s;
  struct Case {
    const char* line;
    nmea::NmeaError want;
  };
  // A survey rover on Bluetooth SPP produces all of these; every one must be
  // a counted rejection rather than a bad position.
  const Case cases[] = {
      {"", nmea::NmeaError::kEmpty},
      {"GNGGA,1,2*00", nmea::NmeaError::kNoStart},
      {"$GN*4A", nmea::NmeaError::kTooShort},
      {"$GNGGA,1,2", nmea::NmeaError::kNoChecksum},
      {"$GNGGA,1,2*ZZ", nmea::NmeaError::kBadChecksumHex},
      {"$GNGGA,1,2*4", nmea::NmeaError::kBadChecksumHex},
      {"$GNGGA,013245.50,2216.9800,N*00", nmea::NmeaError::kBadChecksum},
  };
  for (const Case& c : cases) {
    CHECK(nmea::parse_sentence(c.line, &s) == c.want);
  }

  // A byte lost from a binary UBX message that leaves a plausible '$'.
  std::string binary = "$GNGGA,1,";
  binary.push_back(static_cast<char>(0x82));
  binary += "2*00";
  CHECK(nmea::parse_sentence(binary, &s) == nmea::NmeaError::kBadCharacter);

  // Oversize.
  std::string big = "$GNGGA";
  for (int i = 0; i < 400; ++i) big += ",1";
  big += "*00";
  CHECK(nmea::parse_sentence(big, &s) == nmea::NmeaError::kTooLong);

  // A checksum-less sentence is accepted when the caller opts in.
  nmea::ParseOptions opt;
  opt.allow_missing_checksum = true;
  CHECK(nmea::parse_sentence("$GNGGA,1,2", &s, opt) == nmea::NmeaError::kOk);
}

TEST_CASE("gnss/nmea/no_fix_epoch_has_empty_position_fields") {
  // The S5 simulator's NONE state: quality 0, every position field empty.
  const std::string raw = with_checksum("GNGGA,000000.00,,,,,0,00,99.9,,M,-2.0,M,,");
  // NOTE: `Sentence` holds string_views INTO this buffer (gnss/nmea.h §2), so
  // the trimmed line must outlive the parse. Passing a temporary substr() here
  // is a dangling read, and it is the mistake this named local exists to avoid.
  const std::string line = raw.substr(0, raw.size() - 2);
  nmea::Sentence s;
  REQUIRE(nmea::parse_sentence(line, &s) == nmea::NmeaError::kOk);
  nmea::GgaData g;
  REQUIRE(nmea::decode_gga(s, &g));
  CHECK_FALSE(g.has_position);
  CHECK(g.fix == FixType::kNone);
  CHECK(g.quality_raw == 0);
  CHECK(g.has_hdop);
}

TEST_CASE("gnss/nmea/gga_quality_digits_map_to_the_spec_vocabulary") {
  CHECK(nmea::fix_from_gga_quality(0) == FixType::kNone);
  CHECK(nmea::fix_from_gga_quality(1) == FixType::kSingle);
  CHECK(nmea::fix_from_gga_quality(2) == FixType::kDgps);
  CHECK(nmea::fix_from_gga_quality(3) == FixType::kSingle);   // PPS
  CHECK(nmea::fix_from_gga_quality(4) == FixType::kRtkFixed);
  CHECK(nmea::fix_from_gga_quality(5) == FixType::kRtkFloat);
  // Dead reckoning and manual input are NOT GNSS observations.
  CHECK(nmea::fix_from_gga_quality(6) == FixType::kNone);
  CHECK(nmea::fix_from_gga_quality(7) == FixType::kNone);
  CHECK(nmea::fix_from_gga_quality(8) == FixType::kSingle);   // simulator
  CHECK(nmea::fix_from_gga_quality(99) == FixType::kNone);

  CHECK(nmea::fix_from_mode_char('R') == FixType::kRtkFixed);
  CHECK(nmea::fix_from_mode_char('F') == FixType::kRtkFloat);
  CHECK(nmea::fix_from_mode_char('D') == FixType::kDgps);
  CHECK(nmea::fix_from_mode_char('A') == FixType::kSingle);
  CHECK(nmea::fix_from_mode_char('N') == FixType::kNone);
  CHECK(nmea::fix_from_mode_char('E') == FixType::kNone);
  CHECK(nmea::fix_from_mode_char('\0') == FixType::kNone);

  CHECK(fix_at_least(FixType::kRtkFixed, FixType::kRtkFloat));
  CHECK_FALSE(fix_at_least(FixType::kDgps, FixType::kRtkFloat));
  CHECK(default_sigma_for_fix(FixType::kRtkFixed) == doctest::Approx(0.02));
  CHECK(default_sigma_for_fix(FixType::kRtkFloat) == doctest::Approx(0.30));
  CHECK(default_sigma_for_fix(FixType::kSingle) == doctest::Approx(2.00));
}

TEST_CASE("gnss/nmea/rmc_gst_gsa_vtg_decode") {
  SimEpoch e;
  e.sod = 3661.25;
  e.sigma_h = 0.021;
  e.sigma_v = 0.034;
  const std::string burst = sim_burst(e);

  nmea::NmeaFramer f;
  nmea::RmcData rmc{};
  nmea::GstData gst{};
  nmea::GsaData gsa{};
  nmea::VtgData vtg{};
  int seen = 0;
  f.set_handler([&](std::string_view, const nmea::Sentence& s, std::int64_t) {
    ++seen;
    if (s.id == nmea::SentenceId::kRmc) nmea::decode_rmc(s, &rmc);
    if (s.id == nmea::SentenceId::kGst) nmea::decode_gst(s, &gst);
    if (s.id == nmea::SentenceId::kGsa) nmea::decode_gsa(s, &gsa);
    if (s.id == nmea::SentenceId::kVtg) nmea::decode_vtg(s, &vtg);
  });
  f.push(ByteSpan(reinterpret_cast<const std::uint8_t*>(burst.data()), burst.size()), 0);
  CHECK(seen == 5);

  CHECK(rmc.valid);
  CHECK(rmc.has_date);
  CHECK(rmc.year == 2026);
  CHECK(rmc.month == 1);
  CHECK(rmc.day == 1);
  CHECK(rmc.mode == 'R');
  CHECK(rmc.fix == FixType::kRtkFixed);
  CHECK(rmc.utc_sod_s == doctest::Approx(3661.25));
  CHECK(rmc.speed_knots == doctest::Approx(1.94).epsilon(0.01));

  CHECK(gst.has_sigmas);
  CHECK(gst.lat_sigma_m == doctest::Approx(0.021));
  CHECK(gst.alt_sigma_m == doctest::Approx(0.034));
  CHECK(gst.has_ellipse);

  CHECK(gsa.fix_type == 3);
  CHECK(gsa.satellites_used == 12);
  CHECK(gsa.pdop == doctest::Approx(0.9));
  CHECK(gsa.vdop == doctest::Approx(0.8));

  CHECK(vtg.has_course_true);
  CHECK(vtg.course_true_deg == doctest::Approx(90.0));
  CHECK(vtg.mode == 'R');
}

TEST_CASE("gnss/nmea/framer_survives_arbitrary_chunking_and_garbage") {
  std::string stream;
  for (int i = 0; i < 20; ++i) {
    SimEpoch e;
    e.sod = i;
    stream += sim_burst(e);
  }
  const std::size_t total_sentences = 100;

  // Same treatment the S1/A2 replay harness gives the D6 parser: tear the
  // stream at every chunk size from 1 to 64 bytes and demand identical output.
  for (std::size_t chunk = 1; chunk <= 64; chunk *= 3) {
    nmea::NmeaFramer f;
    std::size_t n = 0;
    f.set_handler([&](std::string_view, const nmea::Sentence&, std::int64_t) { ++n; });
    for (std::size_t i = 0; i < stream.size(); i += chunk) {
      const std::size_t len = std::min(chunk, stream.size() - i);
      f.push(ByteSpan(reinterpret_cast<const std::uint8_t*>(stream.data() + i), len),
             static_cast<std::int64_t>(i));
    }
    CHECK(n == total_sentences);
    CHECK(f.stats().sentences_ok == total_sentences);
    CHECK(f.stats().checksum_failed == 0);
  }

  // Binary noise (a UBX message, an RTCM frame) interleaved between sentences.
  std::string noisy;
  for (int i = 0; i < 10; ++i) {
    SimEpoch e;
    e.sod = i;
    noisy += sim_burst(e);
    noisy += std::string("\xB5\x62\x01\x07\x5C\x00", 6);
    noisy.push_back(static_cast<char>(0xD3));
  }
  nmea::NmeaFramer f2;
  std::size_t n2 = 0;
  f2.set_handler([&](std::string_view, const nmea::Sentence&, std::int64_t) { ++n2; });
  f2.push(ByteSpan(reinterpret_cast<const std::uint8_t*>(noisy.data()), noisy.size()), 0);
  CHECK(n2 == 50);
  CHECK(f2.stats().dropped_bytes >= 70);
  CHECK(f2.stats().malformed == 0);
}

TEST_CASE("gnss/nmea/corrupted_sentences_are_counted_and_skipped") {
  std::string stream;
  int corrupted = 0;
  for (int i = 0; i < 50; ++i) {
    SimEpoch e;
    e.sod = i;
    std::string b = sim_burst(e);
    if (i % 5 == 0) {
      // Flip one character in the GGA body — the checksum must catch it.
      b[10] = (b[10] == '9') ? '8' : '9';
      ++corrupted;
    }
    stream += b;
  }
  nmea::NmeaFramer f;
  std::size_t ok = 0;
  std::vector<nmea::NmeaError> errs;
  f.set_handler([&](std::string_view, const nmea::Sentence&, std::int64_t) { ++ok; });
  f.set_error_handler(
      [&](std::string_view, nmea::NmeaError e, std::int64_t) { errs.push_back(e); });
  f.push(ByteSpan(reinterpret_cast<const std::uint8_t*>(stream.data()), stream.size()), 0);

  CHECK(ok == 250u - static_cast<std::size_t>(corrupted));
  CHECK(errs.size() == static_cast<std::size_t>(corrupted));
  for (nmea::NmeaError e : errs) CHECK(e == nmea::NmeaError::kBadChecksum);
  CHECK(f.stats().checksum_pass_rate() == doctest::Approx(240.0 / 250.0));
}

TEST_CASE("gnss/nmea/utc_to_unix_and_gga_builder_round_trip") {
  std::int64_t ns = 0;
  REQUIRE(nmea::utc_to_unix_ns(1970, 1, 1, 0.0, &ns));
  CHECK(ns == 0);
  REQUIRE(nmea::utc_to_unix_ns(2000, 1, 1, 0.0, &ns));
  CHECK(ns == 946684800LL * 1000000000LL);
  REQUIRE(nmea::utc_to_unix_ns(2026, 8, 15, 12 * 3600 + 34 * 60 + 56.5, &ns));
  // 2026-08-15T12:34:56.5Z, cross-checked with Python datetime.
  CHECK(ns == 1786797296500000000LL);
  CHECK_FALSE(nmea::utc_to_unix_ns(2026, 13, 1, 0.0, &ns));
  CHECK_FALSE(nmea::utc_to_unix_ns(2026, 1, 1, -1.0, &ns));

  nmea::GgaBuilderInput in;
  in.utc_sod_s = 3661.5;
  in.lat_deg = -33.8688;
  in.lon_deg = 151.2093;
  in.alt_msl_m = 12.34;
  in.geoid_sep_m = 22.5;
  in.quality = 4;
  in.satellites = 21;
  in.hdop = 0.7;
  const std::string gga = nmea::build_gga(in);
  REQUIRE(gga.size() > 10);
  CHECK(gga.substr(gga.size() - 2) == "\r\n");

  const std::string gga_line = gga.substr(0, gga.size() - 2);
  nmea::Sentence s;
  REQUIRE(nmea::parse_sentence(gga_line, &s) == nmea::NmeaError::kOk);
  nmea::GgaData g;
  REQUIRE(nmea::decode_gga(s, &g));
  CHECK(g.lat_deg == doctest::Approx(-33.8688).epsilon(1e-7));
  CHECK(g.lon_deg == doctest::Approx(151.2093).epsilon(1e-7));
  CHECK(g.fix == FixType::kRtkFixed);
  CHECK(g.satellites == 21);
  CHECK(g.alt_msl_m == doctest::Approx(12.34));
  CHECK(g.geoid_sep_m == doctest::Approx(22.5));
  CHECK(g.utc_sod_s == doctest::Approx(3661.5));
}

// ===========================================================================
// 2. RTCM3 framing
// ===========================================================================

TEST_CASE("gnss/rtcm3/crc24q_matches_the_independent_spike_implementation") {
  // spikes/s5-rtk-sim/rtcm_tool.py re-derived CRC-24Q from the RTCM generator
  // polynomial independently of this table-driven version. Both give EDEDD6
  // for this frame, and rtcm_tool.build_frame(4C E0 00 80) produces exactly
  // the bytes below.
  const std::uint8_t frame[] = {0xD3, 0x00, 0x04, 0x4C, 0xE0, 0x00, 0x80, 0xED, 0xED, 0xD6};
  CHECK(rtcm3::crc24q(frame, 7) == 0xEDEDD6u);
  CHECK(rtcm3::crc24q(nullptr, 0) == 0u);

  rtcm3::FrameInfo info;
  REQUIRE(rtcm3::validate_frame(frame, sizeof(frame), &info));
  CHECK(info.crc_ok);
  CHECK(info.payload_len == 4);
  CHECK(info.total_len == 10);
  CHECK(info.message_type == 1230);  // 0x4CE >> ... = DF002 of this payload

  // Build/validate round trip over the whole legal length range.
  std::vector<std::uint8_t> payload(rtcm3::kMaxPayloadBytes);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 7);
  std::vector<std::uint8_t> out(rtcm3::kMaxFrameBytes);
  for (std::size_t len : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(255),
                          std::size_t(256), rtcm3::kMaxPayloadBytes}) {
    const std::size_t n = rtcm3::build_frame(payload.data(), len, out.data());
    CHECK(n == len + 6);
    rtcm3::FrameInfo fi;
    REQUIRE(rtcm3::validate_frame(out.data(), n, &fi));
    CHECK(fi.crc_ok);
    CHECK(fi.payload_len == len);
  }
  CHECK(rtcm3::build_frame(payload.data(), rtcm3::kMaxPayloadBytes + 1, out.data()) == 0);
}

TEST_CASE("gnss/rtcm3/framer_resyncs_counts_and_ages") {
  auto make = [](std::uint16_t type, std::size_t extra, std::uint8_t seed) {
    std::vector<std::uint8_t> p(2 + extra);
    p[0] = static_cast<std::uint8_t>(type >> 4);
    p[1] = static_cast<std::uint8_t>((type & 0xF) << 4);
    for (std::size_t i = 2; i < p.size(); ++i) {
      p[i] = static_cast<std::uint8_t>(seed + i * 31);
    }
    std::vector<std::uint8_t> f(rtcm3::kMaxFrameBytes);
    const std::size_t n = rtcm3::build_frame(p.data(), p.size(), f.data());
    f.resize(n);
    return f;
  };

  std::vector<std::uint8_t> stream;
  const std::uint16_t types[] = {1005, 1077, 1087, 1097, 1127, 1230};
  for (int i = 0; i < 30; ++i) {
    // Stray bytes between frames, including a decoy 0xD3 with non-zero
    // reserved bits.
    stream.push_back(0x00);
    stream.push_back(0xD3);
    stream.push_back(0xFF);
    const auto f = make(types[i % 6], 20 + static_cast<std::size_t>(i), static_cast<std::uint8_t>(i));
    stream.insert(stream.end(), f.begin(), f.end());
  }
  // One frame with a corrupted CRC.
  auto bad = make(1005, 10, 9);
  bad[bad.size() - 1] ^= 0xFF;
  stream.insert(stream.end(), bad.begin(), bad.end());
  // A truncated trailing frame: must be buffered, not counted as an error.
  auto trunc = make(1077, 40, 3);
  stream.insert(stream.end(), trunc.begin(), trunc.begin() + 10);

  for (std::size_t chunk : {std::size_t(1), std::size_t(7), std::size_t(64), std::size_t(4096)}) {
    rtcm3::Rtcm3Framer f;
    std::size_t frames = 0, bad_frames = 0;
    f.set_handler([&](ByteSpan fr, const rtcm3::FrameInfo& i, std::int64_t) {
      ++frames;
      CHECK(i.crc_ok);
      CHECK(fr.size() == i.total_len);
      CHECK(fr[0] == rtcm3::kPreamble);
    });
    f.set_bad_frame_handler(
        [&](ByteSpan, const rtcm3::FrameInfo& i, std::int64_t) {
          ++bad_frames;
          CHECK_FALSE(i.crc_ok);
        });
    for (std::size_t i = 0; i < stream.size(); i += chunk) {
      const std::size_t len = std::min(chunk, stream.size() - i);
      f.push(ByteSpan(stream.data() + i, len), 1000000000LL);
    }
    CHECK(frames == 30);
    CHECK(bad_frames == 1);
    CHECK(f.stats().frames_ok == 30);
    CHECK(f.stats().frames_crc_failed == 1);
    CHECK(f.stats().resync_bytes >= 90);
    CHECK(f.stats().type_slots_used == 6);
    CHECK(f.stats().count_of(1005) == 5);
    CHECK(f.stats().count_of(1230) == 5);
    CHECK(f.stats().count_of(9999) == 0);
    CHECK(f.stats().crc_pass_rate() == doctest::Approx(30.0 / 31.0));
    CHECK(f.buffered() == 10);  // the truncated tail is still waiting
    CHECK(f.age_s(3000000000LL) == doctest::Approx(2.0));
  }

  rtcm3::Rtcm3Framer empty;
  CHECK(empty.age_s(0) == -1.0);  // "unknown", not "fresh"
}

// ===========================================================================
// 3. CRS
// ===========================================================================

TEST_CASE("gnss/crs/geodetic_ecef_is_exact_at_the_defining_points") {
  using namespace crs;
  // These three are not "reference values" from anywhere — they are the
  // ellipsoid's definition, so they are the strongest possible check.
  Ecef e = geodetic_to_ecef(Geodetic{0.0, 0.0, 0.0});
  CHECK(e.x == doctest::Approx(kWgs84.a).epsilon(1e-15));
  CHECK(std::fabs(e.y) < 1e-6);
  CHECK(std::fabs(e.z) < 1e-6);

  e = geodetic_to_ecef(Geodetic{0.0, 90.0, 0.0});
  CHECK(std::fabs(e.x) < 1e-6);
  CHECK(e.y == doctest::Approx(kWgs84.a).epsilon(1e-15));

  e = geodetic_to_ecef(Geodetic{90.0, 0.0, 0.0});
  CHECK(e.z == doctest::Approx(kWgs84.b()).epsilon(1e-15));
  CHECK(kWgs84.b() == doctest::Approx(6356752.314245).epsilon(1e-12));

  // Round trip over a global grid, including extreme heights.
  double worst_pos = 0.0, worst_h = 0.0;
  for (int ilat = -89; ilat <= 89; ilat += 7) {
    for (int ilon = -180; ilon < 180; ilon += 23) {
      for (double h : {-400.0, 0.0, 120.0, 9000.0}) {
        const Geodetic g{static_cast<double>(ilat), static_cast<double>(ilon), h};
        const Geodetic b = ecef_to_geodetic(geodetic_to_ecef(g));
        const double dlat_m = std::fabs(b.lat_deg - g.lat_deg) * 111320.0;
        const double dlon_m = std::fabs(b.lon_deg - g.lon_deg) * 111320.0;
        worst_pos = std::max(worst_pos, std::max(dlat_m, dlon_m));
        worst_h = std::max(worst_h, std::fabs(b.height_m - g.height_m));
      }
    }
  }
  MESSAGE("geodetic<->ECEF worst round-trip: " << worst_pos * 1e9 << " nm horizontal, "
                                               << worst_h * 1e9 << " nm vertical");
  CHECK(worst_pos < 1e-6);
  CHECK(worst_h < 1e-6);

  // The polar-axis degenerate input a fuzzer will eventually produce.
  const Geodetic pole = ecef_to_geodetic(Ecef{0.0, 0.0, kWgs84.b()});
  CHECK(pole.lat_deg == doctest::Approx(90.0));
  CHECK(std::fabs(pole.height_m) < 1e-6);
}

TEST_CASE("gnss/crs/enu_frame_is_orthonormal_and_round_trips") {
  using namespace crs;
  const Geodetic origin{22.2830, 114.1585, 50.0};
  const EnuFrame f = make_enu_frame(origin);
  REQUIRE(f.valid);

  // Columns of ecef_from_enu must be an orthonormal right-handed triad.
  auto col = [&](int c, double v[3]) {
    v[0] = f.ecef_from_enu[0 * 3 + c];
    v[1] = f.ecef_from_enu[1 * 3 + c];
    v[2] = f.ecef_from_enu[2 * 3 + c];
  };
  double E[3], N[3], U[3];
  col(0, E);
  col(1, N);
  col(2, U);
  auto dot = [](const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  CHECK(dot(E, E) == doctest::Approx(1.0).epsilon(1e-14));
  CHECK(dot(N, N) == doctest::Approx(1.0).epsilon(1e-14));
  CHECK(dot(U, U) == doctest::Approx(1.0).epsilon(1e-14));
  CHECK(std::fabs(dot(E, N)) < 1e-14);
  CHECK(std::fabs(dot(E, U)) < 1e-14);
  CHECK(std::fabs(dot(N, U)) < 1e-14);

  CHECK(std::fabs(geodetic_to_enu(f, origin).e) < 1e-6);
  CHECK(std::fabs(geodetic_to_enu(f, origin).n) < 1e-6);
  CHECK(std::fabs(geodetic_to_enu(f, origin).u) < 1e-6);

  // A metre north/east must be a metre, to the accuracy of the local radii.
  const double mlat = meters_per_deg_lat(origin.lat_deg);
  const double mlon = meters_per_deg_lon(origin.lat_deg);
  const Enu one_deg_n = geodetic_to_enu(f, Geodetic{origin.lat_deg + 1e-4, origin.lon_deg, 50.0});
  CHECK(one_deg_n.n == doctest::Approx(mlat * 1e-4).epsilon(1e-4));
  const Enu one_deg_e = geodetic_to_enu(f, Geodetic{origin.lat_deg, origin.lon_deg + 1e-4, 50.0});
  CHECK(one_deg_e.e == doctest::Approx(mlon * 1e-4).epsilon(1e-4));

  double worst = 0.0;
  Rng rng(0xA10);
  for (int i = 0; i < 500; ++i) {
    const Enu p{(rng.uniform() - 0.5) * 4000.0, (rng.uniform() - 0.5) * 4000.0,
                (rng.uniform() - 0.5) * 200.0};
    const Enu b = geodetic_to_enu(f, enu_to_geodetic(f, p));
    worst = std::max(worst, std::max(std::fabs(b.e - p.e),
                                     std::max(std::fabs(b.n - p.n), std::fabs(b.u - p.u))));
  }
  MESSAGE("ENU<->WGS84 worst round-trip over a 4 km box: " << worst * 1e9 << " nm");
  CHECK(worst < 1e-7);
}

TEST_CASE("gnss/crs/meridian_arc_matches_numerical_integration") {
  using namespace crs;
  // Independent reference: Simpson integration of the meridian radius of
  // curvature M(phi) = a(1-e^2)/(1-e^2 sin^2 phi)^{3/2}. No external data, no
  // shared code with the Krüger series under test.
  auto integrate = [](const Ellipsoid& el, double phi) {
    const int n = 20000;
    const double h = phi / n;
    auto g = [&](double p) {
      const double s = std::sin(p);
      const double w = 1.0 - el.e2() * s * s;
      return el.a * (1.0 - el.e2()) / (w * std::sqrt(w));
    };
    double sum = g(0.0) + g(phi);
    for (int i = 1; i < n; ++i) sum += (i % 2 ? 4.0 : 2.0) * g(i * h);
    return sum * h / 3.0;
  };

  double worst = 0.0;
  for (double lat = 0.0; lat <= 84.0; lat += 6.0) {
    const double ref = integrate(kWgs84, lat * kDeg);
    const double got = meridian_arc(kWgs84, lat * kDeg);
    worst = std::max(worst, std::fabs(got - ref));
  }
  MESSAGE("meridian arc vs numerical integration, worst over 0..84 deg: " << worst * 1000.0
                                                                         << " mm");
  CHECK(worst < 1e-4);

  // Snyder's own worked example uses Clarke 1866 at 40.5 deg; the numerical
  // integration puts it at 4 484 837.671 m. (Map Projections — A Working
  // Manual, PP 1395, p. 269 prints 4 484 837.0; that digit is the book's
  // rounding of its truncated series, and the integration is the arbiter.)
  CHECK(meridian_arc(kClarke1866, 40.5 * kDeg) ==
        doctest::Approx(integrate(kClarke1866, 40.5 * kDeg)).epsilon(1e-10));
}

TEST_CASE("gnss/crs/transverse_mercator_matches_snyders_published_example") {
  using namespace crs;
  // Snyder, "Map Projections — A Working Manual", USGS Professional Paper
  // 1395, pp. 269–270: Transverse Mercator on the Clarke 1866 ellipsoid,
  // k0 = 0.9996, central meridian 75°W, origin at the equator, for
  // 40°30'N 73°30'W. Published: x = 127 106.5 m, y = 4 484 124.4 m,
  // k = 0.9997989. Stated to 0.1 m / 7 digits, which is the tolerance below.
  TmParams p;
  p.lon0_deg = -75.0;
  p.lat0_deg = 0.0;
  p.k0 = 0.9996;
  p.false_easting = 0.0;
  p.false_northing = 0.0;

  double x = 0, y = 0, k = 0, gamma = 0;
  tm_forward(kClarke1866, p, 40.5, -73.5, &x, &y, &k, &gamma);
  MESSAGE("Snyder TM: x=" << x << " (127106.5)  y=" << y << " (4484124.4)  k=" << k
                          << " (0.9997989)");
  // Snyder prints to 0.1 m, so 0.05 m is agreement at his stated precision.
  CHECK(std::fabs(x - 127106.5) < 0.05);
  CHECK(std::fabs(y - 4484124.4) < 0.05);
  CHECK(k == doctest::Approx(0.9997989).epsilon(1e-7));
  CHECK(gamma > 0.0);  // east of the central meridian in the northern hemisphere

  double lat = 0, lon = 0;
  tm_inverse(kClarke1866, p, 127106.5, 4484124.4, &lat, &lon);
  CHECK(std::fabs(lat - 40.5) * 111320.0 < 0.05);
  CHECK(std::fabs(lon + 73.5) * 111320.0 < 0.05);
}

TEST_CASE("gnss/crs/utm_zone_selection_including_the_two_irregularities") {
  using namespace crs;
  CHECK(utm_zone_for(0.0, 0.5).zone == 31);
  CHECK(utm_zone_for(0.0, -179.0).zone == 1);
  CHECK(utm_zone_for(0.0, 179.0).zone == 60);
  CHECK(utm_zone_for(22.283, 114.1585).zone == 50);   // Hong Kong
  CHECK(utm_zone_for(22.283, 114.1585).north);
  CHECK(utm_zone_for(-33.87, 151.21).zone == 56);     // Sydney
  CHECK_FALSE(utm_zone_for(-33.87, 151.21).north);

  // Bergen, Norway: zone 32 is widened westwards. Without this, a survey
  // lands in zone 31 and ~300 km of easting away from the truth.
  CHECK(utm_zone_for(60.39, 5.32).zone == 32);
  CHECK(utm_zone_for(55.9, 5.32).zone == 31);   // just south of the exception
  CHECK(utm_zone_for(64.1, 5.32).zone == 31);   // just north of it

  // Svalbard: 31/33/35/37 only.
  CHECK(utm_zone_for(78.0, 5.0).zone == 31);
  CHECK(utm_zone_for(78.0, 15.0).zone == 33);
  CHECK(utm_zone_for(78.0, 25.0).zone == 35);
  CHECK(utm_zone_for(78.0, 35.0).zone == 37);
  CHECK(utm_zone_for(71.0, 15.0).zone == 33);  // below 72 deg: the normal rule agrees
}

TEST_CASE("gnss/crs/utm_round_trips_and_agrees_with_ellipsoidal_distance") {
  using namespace crs;
  // (a) Round trip across a whole zone, at every latitude the product can
  //     plausibly be used at.
  double worst_m = 0.0;
  for (double lat = -80.0; lat <= 84.0; lat += 4.0) {
    for (double dlon = -2.9; dlon <= 2.9; dlon += 0.5) {
      const Geodetic g{lat, 117.0 + dlon, 30.0};
      const UtmCoord u = geodetic_to_utm(g);
      REQUIRE(u.zone == 50);
      const Geodetic b = utm_to_geodetic(u, g.height_m);
      const double dn = std::fabs(b.lat_deg - g.lat_deg) * meters_per_deg_lat(lat);
      const double de = std::fabs(b.lon_deg - g.lon_deg) * meters_per_deg_lon(lat);
      worst_m = std::max(worst_m, std::max(dn, de));
    }
  }
  MESSAGE("UTM round trip worst error over zone 50, lat -80..84: " << worst_m * 1e6 << " um");
  CHECK(worst_m < 1e-6);

  // (b) An INDEPENDENT consistency check that needs no reference data: over a
  //     short baseline the grid distance divided by the point scale factor
  //     must equal the true ellipsoidal distance (computed here in ECEF,
  //     which shares no code with the projection).
  const Geodetic a{22.2830, 114.1585, 0.0};
  const Geodetic b{22.2830 + 0.0090, 114.1585 + 0.0097, 0.0};  // ~1.5 km diagonal
  const UtmCoord ua = geodetic_to_utm(a);
  const UtmCoord ub = geodetic_to_utm_zone(b, UtmZone{ua.zone, ua.north});
  const double grid = std::sqrt((ub.easting - ua.easting) * (ub.easting - ua.easting) +
                                (ub.northing - ua.northing) * (ub.northing - ua.northing));
  const Ecef ea = geodetic_to_ecef(a), eb = geodetic_to_ecef(b);
  const double chord = std::sqrt((eb.x - ea.x) * (eb.x - ea.x) + (eb.y - ea.y) * (eb.y - ea.y) +
                                 (eb.z - ea.z) * (eb.z - ea.z));
  const double kmean = 0.5 * (ua.scale + ub.scale);
  MESSAGE("grid " << grid << " m / k " << kmean << " = " << grid / kmean << " vs ECEF chord "
                  << chord << " m");
  CHECK(std::fabs(grid / kmean - chord) < 0.01);  // 1 cm over 1.5 km

  // (c) Scale is exactly k0 on the central meridian, and grows away from it.
  const UtmCoord cm = geodetic_to_utm(Geodetic{22.283, 117.0, 0.0});
  CHECK(cm.scale == doctest::Approx(0.9996).epsilon(1e-12));
  CHECK(cm.easting == doctest::Approx(500000.0).epsilon(1e-12));
  CHECK(std::fabs(cm.convergence_deg) < 1e-9);
  CHECK(geodetic_to_utm(Geodetic{22.283, 114.0, 0.0}).scale > 0.9996);

  // (d) Southern hemisphere false northing.
  const UtmCoord s = geodetic_to_utm(Geodetic{-33.8688, 151.2093, 0.0});
  CHECK_FALSE(s.north);
  CHECK(s.northing > 6000000.0);
  CHECK(s.northing < 10000000.0);
  const Geodetic sb = utm_to_geodetic(s);
  CHECK(sb.lat_deg == doctest::Approx(-33.8688).epsilon(1e-9));
  CHECK(sb.lon_deg == doctest::Approx(151.2093).epsilon(1e-9));
}

TEST_CASE("gnss/crs/epsg_wkt_and_proj_strings") {
  using namespace crs;
  CHECK(utm_epsg(UtmZone{50, true}) == 32650);
  CHECK(utm_epsg(UtmZone{56, false}) == 32756);
  CHECK(utm_epsg(UtmZone{0, true}) == 0);
  CHECK(utm_zone_from_epsg(32650).zone == 50);
  CHECK(utm_zone_from_epsg(32650).north);
  CHECK(utm_zone_from_epsg(32756).zone == 56);
  CHECK_FALSE(utm_zone_from_epsg(32756).north);
  CHECK_FALSE(utm_zone_from_epsg(4326).valid());

  CHECK(epsg_string(32650) == "EPSG:32650");
  CHECK(parse_epsg_string("EPSG:32650") == 32650);
  CHECK(parse_epsg_string("32650") == 32650);
  CHECK(parse_epsg_string("nonsense") == 0);
  CHECK(crs_name_for_epsg(32650) == "WGS 84 / UTM zone 50N");
  CHECK(crs_name_for_epsg(32756) == "WGS 84 / UTM zone 56S");

  const std::string w = utm_wkt1(UtmZone{50, true});
  // Structure a LAS/QGIS/CloudCompare reader actually keys off.
  CHECK(w.find("PROJCS[\"WGS 84 / UTM zone 50N\"") == 0);
  CHECK(w.find("GEOGCS[\"WGS 84\"") != std::string::npos);
  CHECK(w.find("SPHEROID[\"WGS 84\",6378137,298.257223563") != std::string::npos);
  CHECK(w.find("PROJECTION[\"Transverse_Mercator\"]") != std::string::npos);
  CHECK(w.find("PARAMETER[\"central_meridian\",117]") != std::string::npos);
  CHECK(w.find("PARAMETER[\"scale_factor\",0.9996]") != std::string::npos);
  CHECK(w.find("PARAMETER[\"false_easting\",500000]") != std::string::npos);
  CHECK(w.find("PARAMETER[\"false_northing\",0]") != std::string::npos);
  CHECK(w.find("AUTHORITY[\"EPSG\",\"32650\"]") != std::string::npos);
  CHECK(w.find("UNIT[\"metre\",1") != std::string::npos);
  // Balanced brackets: a malformed WKT in a LAS VLR makes the file
  // unopenable, and it is the kind of thing a template edit breaks silently.
  int depth = 0;
  for (char c : w) {
    if (c == '[') ++depth;
    if (c == ']') --depth;
    CHECK(depth >= 0);
  }
  CHECK(depth == 0);

  CHECK(utm_wkt1(UtmZone{56, false}).find("PARAMETER[\"false_northing\",1e+07]") !=
        std::string::npos);
  CHECK(wkt1_for_epsg(32650) == w);
  CHECK(wkt1_for_epsg(4326) == wgs84_geographic_wkt1());
  CHECK(wkt1_for_epsg(2193).empty());  // not ours: caller supplies its own

  CHECK(utm_proj_string(UtmZone{50, true}) ==
        "+proj=utm +zone=50 +datum=WGS84 +units=m +no_defs +type=crs");
  CHECK(utm_proj_string(UtmZone{56, false}).find("+south") != std::string::npos);
  CHECK(proj_string_for_epsg(4326).find("+proj=longlat") == 0);
}

TEST_CASE("gnss/crs/geoid_seam") {
  crs::ConstantGeoidModel g(2.5);
  double n = 0.0;
  CHECK(g.undulation(22.28, 114.15, &n));
  CHECK(n == doctest::Approx(2.5));
  CHECK(std::string(g.name()) == "constant");
}

// ===========================================================================
// 4. GnssSource
// ===========================================================================

TEST_CASE("gnss/source/epoch_assembly_sigmas_and_origin") {
  GnssSourceConfig cfg;
  cfg.min_speed_for_heading_mps = 0.5;
  GnssSource src(cfg);
  REQUIRE(src.start().ok());

  std::vector<GnssFix> seen;
  src.set_fix_callback([&](const GnssFix& f) { seen.push_back(f); });

  SimEpoch e;
  e.sod = 0.0;
  e.sigma_h = 0.021;
  e.sigma_v = 0.033;
  push_str(src, sim_burst(e), 1000000000LL);
  // Nothing published yet: the epoch is still open, waiting for GST-bearing
  // sentences that share this UTC.
  CHECK(seen.empty());

  e.sod = 1.0;
  e.lat += 0.00001;
  push_str(src, sim_burst(e), 2000000000LL);
  REQUIRE(seen.size() == 1);

  const GnssFix& f = seen[0];
  CHECK(f.fix == FixType::kRtkFixed);
  CHECK(f.lat_deg == doctest::Approx(22.2830).epsilon(1e-9));
  CHECK(f.alt_m == doctest::Approx(50.0));
  CHECK(f.geoid_sep_m == doctest::Approx(-2.0));
  CHECK(f.has_geoid_sep);
  CHECK(f.height_ellipsoid_m == doctest::Approx(48.0));
  CHECK(f.satellites == 22);
  CHECK(f.hdop == doctest::Approx(0.6f));
  CHECK(f.pdop == doctest::Approx(0.9f));
  CHECK(f.vdop == doctest::Approx(0.8f));
  CHECK(f.fix_dimension == 3);
  CHECK(f.sigma_from_gst);
  CHECK(f.sigma_north_m == doctest::Approx(0.021f));
  CHECK(f.sigma_up_m == doctest::Approx(0.033f));
  CHECK(f.sigma_horizontal_m == doctest::Approx(0.021f).epsilon(1e-4));
  CHECK(f.has_course);
  CHECK(f.course_deg == doctest::Approx(90.0f));
  CHECK(f.speed_mps == doctest::Approx(1.0f).epsilon(0.01));
  CHECK(f.utc_unix_ns != 0);
  CHECK(f.t_arrival_ns == 1000000000LL);
  // No TimeSync configured: the engine stamp is the arrival stamp.
  CHECK(f.t_mono_ns == 1000000000LL);

  crs::Geodetic o;
  REQUIRE(src.origin(&o));
  CHECK(o.lat_deg == doctest::Approx(22.2830).epsilon(1e-9));
  CHECK(o.height_m == doctest::Approx(48.0));

  // A course of 90 deg (due east) is a yaw of 0 in an ENU local frame.
  Pose p;
  REQUIRE(src.pose_at(1000000000LL, &p).ok());
  CHECK(std::fabs(p.position[0]) < 1e-6);
  CHECK(std::fabs(p.position[1]) < 1e-6);
  CHECK(p.orientation[2] == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(p.orientation[3] == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(p.quality == PoseQuality::kGood);
  CHECK(p.source == StreamId::kGnss);

  CHECK_FALSE(src.last_gga_sentence().empty());
  CHECK(src.last_gga_sentence().find("$GNGGA") == 0);

  // Fallback sigma when the receiver sends no GST.
  GnssSource plain((GnssSourceConfig()));
  std::vector<GnssFix> pf;
  plain.set_fix_callback([&](const GnssFix& x) { pf.push_back(x); });
  SimEpoch e2;
  e2.emit_gst = false;
  e2.quality = 5;
  e2.mode = "F";
  e2.hdop = 1.0;
  e2.sod = 0.0;
  push_str(plain, sim_burst(e2), 0);
  e2.sod = 1.0;
  push_str(plain, sim_burst(e2), 1000000000LL);
  REQUIRE(pf.size() == 1);
  CHECK(pf[0].fix == FixType::kRtkFloat);
  CHECK_FALSE(pf[0].sigma_from_gst);
  CHECK(pf[0].sigma_horizontal_m == doctest::Approx(0.30f).epsilon(1e-4));
}

TEST_CASE("gnss/source/fix_timeline_and_gating_over_a_scripted_scenario") {
  // The S5 default scenario: RTK Fixed 60 s -> Float 20 s -> Single 10 s ->
  // Fixed, at 1 Hz, which is what B9's status strip and the §3.4 capture gate
  // are specified against.
  GnssSourceConfig cfg;
  cfg.min_fix_for_pose = FixType::kSingle;
  cfg.min_fix_for_origin = FixType::kRtkFixed;
  GnssSource src(cfg);
  REQUIRE(src.start().ok());

  auto state_at = [](int t) {
    if (t < 60) return FixType::kRtkFixed;
    if (t < 80) return FixType::kRtkFloat;
    if (t < 90) return FixType::kSingle;
    return FixType::kRtkFixed;
  };

  Rng rng(1234);
  for (int t = 0; t <= 120; ++t) {
    const FixType f = state_at(t);
    SimEpoch e;
    e.sod = t;
    e.quality = quality_digit(f);
    e.mode = mode_char(f);
    e.sigma_h = default_sigma_for_fix(f);
    e.sigma_v = e.sigma_h * 1.5;
    e.lat = 22.2830 + (t * 1.0 + rng.gauss() * e.sigma_h) / 111320.0;
    e.lon = 114.1585;
    push_str(src, sim_burst(e), static_cast<std::int64_t>(t) * 1000000000LL);
  }
  src.flush();

  const GnssStats st = src.stats();
  CHECK(st.epochs == 121);
  CHECK(st.fixes_published == 121);
  CHECK(st.by_fix[static_cast<int>(FixType::kRtkFixed)] == 91);
  CHECK(st.by_fix[static_cast<int>(FixType::kRtkFloat)] == 20);
  CHECK(st.by_fix[static_cast<int>(FixType::kSingle)] == 10);
  CHECK(st.fix_fraction(FixType::kRtkFixed) == doctest::Approx(91.0 / 121.0));
  CHECK(st.nmea.sentences_ok == 121 * 5);
  CHECK(st.nmea.checksum_failed == 0);
  CHECK(st.gst_epochs == 121);

  // Interpolation between two epochs, and the gates.
  const PoseSample mid = src.sample_at(30500000000LL);
  CHECK(mid.gate == PoseGate::kOk);
  CHECK(mid.has_pose);
  CHECK(mid.bracket_gap_ns == 1000000000LL);
  CHECK(mid.confidence == doctest::Approx(1.0f));

  const PoseSample degraded = src.sample_at(85000000000LL);
  CHECK(degraded.pose.quality == PoseQuality::kPoor);
  CHECK(degraded.confidence == doctest::Approx(0.35f));

  CHECK(src.sample_at(-1).gate == PoseGate::kBeforeFirst);
  CHECK(src.sample_at(999000000000LL).gate == PoseGate::kFuture);
  CHECK(src.pose_at(999000000000LL, nullptr).error() == ScanError::kAgain);

  std::int64_t a = 0, b = 0;
  REQUIRE(src.time_span(&a, &b));
  CHECK(a == 0);
  CHECK(b == 120000000000LL);
}

TEST_CASE("gnss/source/loss_of_fix_gates_the_interpolation_across_the_gap") {
  GnssSourceConfig cfg;
  cfg.min_fix_for_pose = FixType::kSingle;
  GnssSource src(cfg);
  REQUIRE(src.start().ok());

  for (int t = 0; t <= 10; ++t) {
    SimEpoch e;
    e.sod = t;
    if (t == 5) {
      // A no-fix epoch: empty position fields, quality 0.
      const std::string line = with_checksum("GNGGA,000005.00,,,,,0,00,99.9,,M,-2.0,M,,") +
                               with_checksum("GNRMC,000005.00,V,,,,,0.00,0.0,010126,,,N");
      push_str(src, line, static_cast<std::int64_t>(t) * 1000000000LL);
      continue;
    }
    e.lat = 22.2830 + t * 1e-5;
    push_str(src, sim_burst(e), static_cast<std::int64_t>(t) * 1000000000LL);
  }
  src.flush();

  const GnssStats st = src.stats();
  CHECK(st.by_fix[static_cast<int>(FixType::kNone)] == 1);
  CHECK(st.epochs_no_position == 1);

  // Interpolating ACROSS the outage must be gated, not silently smoothed.
  CHECK(src.sample_at(4500000000LL).gate == PoseGate::kTrackingLost);
  CHECK(src.sample_at(5500000000LL).gate == PoseGate::kTrackingLost);
  CHECK(src.sample_at(3500000000LL).gate == PoseGate::kOk);
  CHECK(src.sample_at(7500000000LL).gate == PoseGate::kOk);
}

TEST_CASE("gnss/source/utc_is_correlated_through_A4") {
  TimeSync ts;
  GnssSourceConfig cfg;
  cfg.timesync = &ts;
  cfg.correlate_utc = true;
  GnssSource src(cfg);
  REQUIRE(src.start().ok());

  // The receiver's UTC is truth; arrival is late by a Bluetooth link with a
  // 120 ms floor and up to 60 ms of extra jitter. A4's min-delay estimator
  // must recover the constant part, so the mapped engine stamps advance at
  // exactly 1 Hz with the jitter removed.
  Rng rng(77);
  std::int64_t utc0 = 0;
  REQUIRE(nmea::utc_to_unix_ns(2026, 1, 1, 0.0, &utc0));
  const std::int64_t engine0 = 5'000'000'000LL;  // arbitrary monotonic origin

  std::vector<GnssFix> fixes;
  src.set_fix_callback([&](const GnssFix& f) { fixes.push_back(f); });

  const int kEpochs = 180;
  for (int t = 0; t <= kEpochs; ++t) {
    SimEpoch e;
    e.sod = t;
    e.lat = 22.2830 + t * 1e-5;
    const std::int64_t jitter = static_cast<std::int64_t>(rng.uniform() * 60e6);
    const std::int64_t arrival = engine0 + static_cast<std::int64_t>(t) * 1000000000LL +
                                 120000000LL + jitter;
    push_str(src, sim_burst(e), arrival);
  }
  src.flush();
  REQUIRE(fixes.size() >= static_cast<std::size_t>(kEpochs));

  const GnssStats st = src.stats();
  CHECK(st.utc_pairs == fixes.size());
  CHECK(st.utc_unavailable == 0);
  CHECK(st.time_converged);

  // Engine stamps must land 1 s apart — the arrival jitter is gone. Compared
  // against the RAW arrival spacing, which carries the full 60 ms of it.
  double worst = 0.0, worst_late = 0.0, worst_raw = 0.0;
  for (std::size_t i = 1; i < fixes.size(); ++i) {
    const double dt = static_cast<double>(fixes[i].t_mono_ns - fixes[i - 1].t_mono_ns) * 1e-9;
    const double raw =
        static_cast<double>(fixes[i].t_arrival_ns - fixes[i - 1].t_arrival_ns) * 1e-9;
    worst = std::max(worst, std::fabs(dt - 1.0));
    worst_raw = std::max(worst_raw, std::fabs(raw - 1.0));
    // The last third of the session, by which point the min-delay envelope has
    // seen enough samples to stop improving (docs/A4-timesync.md §6: a 1 Hz
    // stream needs ~16 s, and the envelope keeps tightening after that).
    if (i > fixes.size() * 2 / 3) worst_late = std::max(worst_late, std::fabs(dt - 1.0));
  }
  MESSAGE("A4-mapped GNSS epoch spacing: worst " << worst * 1e3 << " ms, worst in the last "
                                                 << "third " << worst_late * 1e3
                                                 << " ms, raw arrival spacing " << worst_raw * 1e3
                                                 << " ms");
  CHECK(worst_raw > 0.03);            // the jitter really is in the arrival stamps
  CHECK(worst < worst_raw * 0.5);     // …and the mapping removes most of it
  CHECK(worst_late < worst);          // …increasingly, as the envelope fills in
  CHECK(worst_late < 0.010);
  const double mean_dt =
      static_cast<double>(fixes.back().t_mono_ns - fixes.front().t_mono_ns) * 1e-9 /
      static_cast<double>(fixes.size() - 1);
  // The residual rate error is A4's drift estimate on a 1 Hz stream, which
  // docs/A4-timesync.md §6 warns is the slowest-converging case in the engine.
  CHECK(mean_dt == doctest::Approx(1.0).epsilon(1e-4));
  // …and the offset is the true one plus the minimum transport delay, which
  // is exactly what docs/A4-timesync.md §3 says is recoverable.
  const std::int64_t expect = engine0 + 120000000LL;
  const std::int64_t got =
      fixes.back().t_mono_ns - static_cast<std::int64_t>(kEpochs) * 1000000000LL;
  CHECK(std::fabs(static_cast<double>(got - expect)) < 5e6);

  CHECK(fixes[0].utc_unix_ns == utc0);
}

TEST_CASE("gnss/source/push_pose_is_refused_and_ring_wraps") {
  GnssSourceConfig cfg;
  cfg.capacity = 8;
  GnssSource src(cfg);
  REQUIRE(src.start().ok());
  CHECK(src.push_pose(Pose{}).error() == ScanError::kNotSupported);

  for (int t = 0; t < 30; ++t) {
    SimEpoch e;
    e.sod = t;
    e.lat = 22.2830 + t * 1e-5;
    push_str(src, sim_burst(e), static_cast<std::int64_t>(t) * 1000000000LL);
  }
  src.flush();
  CHECK(src.fix_count() == 8);
  CHECK(src.stats().overwritten == 22);
  CHECK(src.fixes().size() == 8);
  // The oldest surviving epoch is t=22, so anything earlier is unresolvable.
  CHECK(src.sample_at(10000000000LL).gate == PoseGate::kBeforeFirst);

  // set_origin() after the origin is anchored on a fix is refused: moving it
  // would change what every already-published coordinate means.
  CHECK(src.set_origin(crs::Geodetic{0, 0, 0}).error() == ScanError::kAlreadyExists);
}

// ===========================================================================
// 5. Georeferencing fusion
// ===========================================================================

namespace {

struct GeorefScenario {
  // Ground truth: the transform that carries local -> global.
  double true_yaw_deg = 37.0;
  double true_t[3] = {123.0, -45.0, 7.5};
  double speed_mps = 1.2;
  int seconds = 240;
  bool add_slam_drift = false;
};

// A 40 x 25 m rectangular walking loop, in the GLOBAL (ENU) frame.
void loop_point(double dist_m, double* x, double* y) {
  const double per = 2.0 * (40.0 + 25.0);
  double d = std::fmod(dist_m, per);
  if (d < 40.0) { *x = d; *y = 0.0; }
  else if (d < 65.0) { *x = 40.0; *y = d - 40.0; }
  else if (d < 105.0) { *x = 40.0 - (d - 65.0); *y = 25.0; }
  else { *x = 0.0; *y = 25.0 - (d - 105.0); }
}

// Builds time-aligned (local, global) observations for a scripted fix-quality
// timeline. The LOCAL frame is the truth rotated/translated by the inverse of
// the transform we then ask the estimator to recover.
std::vector<GeorefObservation> make_observations(const GeorefScenario& sc,
                                                 const std::vector<FixType>& timeline,
                                                 std::uint64_t seed) {
  Rng rng(seed);
  std::vector<GeorefObservation> out;
  const double cy = std::cos(sc.true_yaw_deg * crs::kDeg);
  const double sy = std::sin(sc.true_yaw_deg * crs::kDeg);
  for (int t = 0; t < sc.seconds; ++t) {
    double gx = 0.0, gy = 0.0;
    loop_point(sc.speed_mps * t, &gx, &gy);
    const double gz = 2.0;
    // global = R(yaw) * local + t   =>   local = R(-yaw) * (global - t)
    const double dx = gx - sc.true_t[0], dy = gy - sc.true_t[1], dz = gz - sc.true_t[2];
    double lx = cy * dx + sy * dy;
    double ly = -sy * dx + cy * dy;
    double lz = dz;
    if (sc.add_slam_drift) {
      // A slow, monotone SLAM drift: ~0.15 % of distance travelled, which is
      // the order A6's LIO reports over a few hundred metres. This is what the
      // residual and the inflated covariance are supposed to expose.
      lx += 0.0020 * t;
      ly += 0.0010 * t;
    }
    const FixType f = timeline[static_cast<std::size_t>(t) % timeline.size()];
    if (f == FixType::kNone) continue;
    const double sh = default_sigma_for_fix(f);
    GeorefObservation o;
    o.t_ns = static_cast<std::int64_t>(t) * 1000000000LL;
    o.local[0] = lx;
    o.local[1] = ly;
    o.local[2] = lz;
    o.global[0] = gx + rng.gauss() * sh;
    o.global[1] = gy + rng.gauss() * sh;
    o.global[2] = gz + rng.gauss() * sh * 1.6;
    o.sigma_h_m = sh;
    o.sigma_v_m = sh * 1.6;
    o.fix = f;
    out.push_back(o);
  }
  return out;
}

struct GeorefError {
  double yaw_deg = 0.0;
  double translation_m = 0.0;
  double worst_point_m = 0.0;
};

GeorefError measure(const GeorefScenario& sc, const GeorefSolution& s) {
  GeorefError e;
  double dy = s.yaw_deg - sc.true_yaw_deg;
  while (dy > 180.0) dy -= 360.0;
  while (dy < -180.0) dy += 360.0;
  e.yaw_deg = std::fabs(dy);
  e.translation_m = std::sqrt((s.translation[0] - sc.true_t[0]) * (s.translation[0] - sc.true_t[0]) +
                              (s.translation[1] - sc.true_t[1]) * (s.translation[1] - sc.true_t[1]));
  // What actually matters: where a point on the far side of the loop lands.
  const double cy_t = std::cos(sc.true_yaw_deg * crs::kDeg);
  const double sy_t = std::sin(sc.true_yaw_deg * crs::kDeg);
  const double cy_e = std::cos(s.yaw_rad), sy_e = std::sin(s.yaw_rad);
  const double corners[4][2] = {{0, 0}, {40, 0}, {40, 25}, {0, 25}};
  for (const auto& c : corners) {
    const double dx = c[0] - sc.true_t[0], dyy = c[1] - sc.true_t[1];
    const double lx = cy_t * dx + sy_t * dyy;
    const double ly = -sy_t * dx + cy_t * dyy;
    const double px = s.scale * (cy_e * lx - sy_e * ly) + s.translation[0];
    const double py = s.scale * (sy_e * lx + cy_e * ly) + s.translation[1];
    e.worst_point_m = std::max(e.worst_point_m, std::sqrt((px - c[0]) * (px - c[0]) +
                                                          (py - c[1]) * (py - c[1])));
  }
  return e;
}

}  // namespace

TEST_CASE("gnss/georef/noise_free_recovery_is_exact") {
  GeorefScenario sc;
  SimilarityEstimatorConfig cfg;
  cfg.min_fix = FixType::kSingle;
  WeightedSimilarityEstimator est(cfg);
  const auto obs = make_observations(sc, {FixType::kRtkFixed}, 1);
  for (GeorefObservation o : obs) {
    // Strip the noise: the exactness of the closed form is the claim here.
    double gx = 0, gy = 0;
    loop_point(sc.speed_mps * static_cast<double>(o.t_ns / 1000000000LL), &gx, &gy);
    o.global[0] = gx;
    o.global[1] = gy;
    o.global[2] = 2.0;
    est.add(o);
  }
  GeorefSolution s;
  REQUIRE(est.solve(&s));
  CHECK(s.converged);
  CHECK(s.yaw_deg == doctest::Approx(37.0).epsilon(1e-9));
  CHECK(s.translation[0] == doctest::Approx(123.0).epsilon(1e-9));
  CHECK(s.translation[1] == doctest::Approx(-45.0).epsilon(1e-9));
  CHECK(s.translation[2] == doctest::Approx(7.5).epsilon(1e-9));
  CHECK(s.scale == doctest::Approx(1.0).epsilon(1e-12));
  CHECK(s.residual_rms_m < 1e-9);
  CHECK(s.gravity_residual_m < 1e-9);
  CHECK(s.inliers == obs.size());
  CHECK(s.rejected == 0);
  CHECK(s.span_m > 40.0);

  // A 173 deg misalignment is not a harder problem: the solution is closed
  // form, not a linearisation around an initial guess.
  GeorefScenario far = sc;
  far.true_yaw_deg = 173.0;
  WeightedSimilarityEstimator est2(cfg);
  for (GeorefObservation o : make_observations(far, {FixType::kRtkFixed}, 1)) {
    double gx = 0, gy = 0;
    loop_point(far.speed_mps * static_cast<double>(o.t_ns / 1000000000LL), &gx, &gy);
    o.global[0] = gx;
    o.global[1] = gy;
    o.global[2] = 2.0;
    est2.add(o);
  }
  GeorefSolution s2;
  REQUIRE(est2.solve(&s2));
  CHECK(s2.yaw_deg == doctest::Approx(173.0).epsilon(1e-9));
}

TEST_CASE("gnss/georef/accuracy_tracks_fix_quality_and_the_report_is_honest") {
  const GeorefScenario sc;
  struct Mix {
    const char* name;
    std::vector<FixType> timeline;
  };
  const Mix mixes[] = {
      {"all Fixed", {FixType::kRtkFixed}},
      {"all Float", {FixType::kRtkFloat}},
      {"all Single", {FixType::kSingle}},
      {"Fixed 3 : Float 1", {FixType::kRtkFixed, FixType::kRtkFixed, FixType::kRtkFixed,
                             FixType::kRtkFloat}},
      {"Float 3 : Single 1", {FixType::kRtkFloat, FixType::kRtkFloat, FixType::kRtkFloat,
                              FixType::kSingle}},
  };

  double prev_reported = 0.0;
  std::vector<double> reported, actual;
  for (const Mix& m : mixes) {
    SimilarityEstimatorConfig cfg;
    cfg.min_fix = FixType::kSingle;  // accept everything: the point is the weighting
    WeightedSimilarityEstimator est(cfg);
    for (const GeorefObservation& o : make_observations(sc, m.timeline, 0xC0FFEE)) est.add(o);
    GeorefSolution s;
    REQUIRE(est.solve(&s));
    const GeorefError e = measure(sc, s);

    MESSAGE(std::string(m.name) << ": yaw err " << e.yaw_deg << " deg, translation err "
                                << e.translation_m << " m, worst corner " << e.worst_point_m
                                << " m | reported sigma_h " << s.horizontal_sigma_m
                                << " m, CEP95 " << s.cep95_m << " m, residual RMS "
                                << s.residual_rms_h_m << " m, yaw sigma " << s.yaw_sigma_deg
                                << " deg");

    reported.push_back(s.horizontal_sigma_m);
    actual.push_back(e.worst_point_m);

    // The reported 95 % figure must actually bound the error it is a bound
    // on. This is the property the whole uncertainty model exists for.
    CHECK(s.cep95_m > e.worst_point_m);
    CHECK(s.converged);
    CHECK(s.dominant_fix == m.timeline[0]);
    (void)prev_reported;
  }

  // Fixed < Fixed-mostly < Float < Float-mostly < Single, in BOTH the actual
  // error and the reported uncertainty. Degradation is visible, not hidden.
  CHECK(actual[0] < actual[1]);
  CHECK(actual[1] < actual[2]);
  CHECK(reported[0] < reported[1]);
  CHECK(reported[1] < reported[2]);
  CHECK(reported[0] < reported[3]);   // pure Fixed beats Fixed+Float
  CHECK(reported[3] < reported[1]);   // …and Fixed+Float beats pure Float
  CHECK(reported[1] < reported[4]);
  CHECK(reported[4] < reported[2]);

  // The magnitudes: RTK Fixed georeferencing should land in centimetres and
  // Single in metres. If these bounds ever move, §5 of docs/A10-gnss.md is
  // the place to explain why.
  CHECK(actual[0] < 0.05);
  CHECK(actual[1] < 0.50);
  CHECK(actual[2] < 3.00);
  CHECK(reported[0] < 0.05);
  CHECK(reported[2] > 1.0);
  CHECK(reported[2] / reported[0] > 20.0);
}

TEST_CASE("gnss/georef/gross_outliers_are_rejected") {
  const GeorefScenario sc;
  SimilarityEstimatorConfig cfg;
  cfg.min_fix = FixType::kSingle;
  WeightedSimilarityEstimator clean(cfg), dirty(cfg);

  auto obs = make_observations(sc, {FixType::kRtkFixed}, 5);
  for (const GeorefObservation& o : obs) clean.add(o);
  GeorefSolution sclean;
  REQUIRE(clean.solve(&sclean));

  // 8 % of the fixes are multipath jumps of 5–30 m that still claim RTK
  // Fixed accuracy — the failure mode a plain least-squares fit cannot
  // survive, because their tiny sigma makes them the HEAVIEST samples.
  int injected = 0;
  for (std::size_t i = 0; i < obs.size(); ++i) {
    if (i % 12 == 3) {
      obs[i].global[0] += 5.0 + static_cast<double>(i % 25);
      obs[i].global[1] -= 4.0;
      ++injected;
    }
    dirty.add(obs[i]);
  }
  GeorefSolution sdirty;
  REQUIRE(dirty.solve(&sdirty));
  const GeorefError ec = measure(sc, sclean);
  const GeorefError ed = measure(sc, sdirty);
  MESSAGE("outliers: " << injected << " injected; clean worst corner " << ec.worst_point_m
                       << " m, robust worst corner " << ed.worst_point_m << " m, rejected "
                       << sdirty.rejected);
  CHECK(sdirty.rejected >= static_cast<std::size_t>(injected));
  CHECK(sdirty.rejected < obs.size() / 4);
  CHECK(ed.worst_point_m < 0.10);
  CHECK(sdirty.converged);
}

TEST_CASE("gnss/georef/unobservable_heading_is_reported_not_invented") {
  SimilarityEstimatorConfig cfg;
  cfg.min_fix = FixType::kSingle;

  // A rover standing still: the position is observable, the heading is not.
  WeightedSimilarityEstimator est(cfg);
  Rng rng(9);
  for (int t = 0; t < 120; ++t) {
    GeorefObservation o;
    o.t_ns = static_cast<std::int64_t>(t) * 1000000000LL;
    o.local[0] = rng.gauss() * 0.01;
    o.local[1] = rng.gauss() * 0.01;
    o.local[2] = 0.0;
    o.global[0] = 100.0 + rng.gauss() * 0.02;
    o.global[1] = 50.0 + rng.gauss() * 0.02;
    o.global[2] = 3.0;
    o.sigma_h_m = 0.02;
    o.sigma_v_m = 0.03;
    o.fix = FixType::kRtkFixed;
    est.add(o);
  }
  GeorefSolution s;
  CHECK_FALSE(est.solve(&s));
  CHECK_FALSE(s.converged);
  CHECK(std::string(s.blocker) == "trajectory too short to observe heading");
  CHECK(s.span_m < 1.0);
  MESSAGE("stationary rover: yaw sigma " << s.yaw_sigma_deg << " deg over a "
                                         << s.span_m << " m baseline");

  // Too few samples.
  WeightedSimilarityEstimator few(cfg);
  for (int i = 0; i < 3; ++i) {
    GeorefObservation o;
    o.t_ns = i;
    o.local[0] = i * 10.0;
    o.global[0] = i * 10.0;
    o.sigma_h_m = 0.02;
    o.fix = FixType::kRtkFixed;
    few.add(o);
  }
  GeorefSolution sf;
  CHECK_FALSE(few.solve(&sf));
  CHECK(std::string(sf.blocker) == "not enough usable fixes");

  // Nothing at all.
  WeightedSimilarityEstimator none(cfg);
  GeorefSolution sn;
  CHECK_FALSE(none.solve(&sn));
  CHECK(std::string(sn.blocker) == "no observations");
}

TEST_CASE("gnss/georef/slam_drift_inflates_the_reported_uncertainty") {
  GeorefScenario clean;
  GeorefScenario drifting;
  drifting.add_slam_drift = true;

  SimilarityEstimatorConfig cfg;
  cfg.min_fix = FixType::kSingle;
  WeightedSimilarityEstimator a(cfg), b(cfg);
  for (const GeorefObservation& o : make_observations(clean, {FixType::kRtkFixed}, 42)) a.add(o);
  for (const GeorefObservation& o : make_observations(drifting, {FixType::kRtkFixed}, 42)) {
    b.add(o);
  }
  GeorefSolution sa, sb;
  REQUIRE(a.solve(&sa));
  REQUIRE(b.solve(&sb));
  MESSAGE("no drift: residual " << sa.residual_rms_h_m << " m, sigma_h " << sa.horizontal_sigma_m
                                << " m | with drift: residual " << sb.residual_rms_h_m
                                << " m, sigma_h " << sb.horizontal_sigma_m << " m");
  // The rigid model cannot absorb a drift, so it shows up in the residual…
  CHECK(sb.residual_rms_h_m > sa.residual_rms_h_m * 1.5);
  // …and the covariance inflation carries it into what the UI reports.
  CHECK(sb.translation_sigma_h_m > sa.translation_sigma_h_m);
}

TEST_CASE("gnss/georef/scale_is_locked_by_default_and_recoverable_when_unlocked") {
  GeorefScenario sc;
  SimilarityEstimatorConfig locked;
  locked.min_fix = FixType::kSingle;
  SimilarityEstimatorConfig freed = locked;
  freed.lock_scale = false;

  // A local frame with a 3 % scale error (what a mis-scaled VIO produces).
  auto scaled = [&](std::vector<GeorefObservation> obs) {
    for (GeorefObservation& o : obs) {
      o.local[0] *= 1.03;
      o.local[1] *= 1.03;
      o.local[2] *= 1.03;
    }
    return obs;
  };
  const auto obs = scaled(make_observations(sc, {FixType::kRtkFixed}, 3));

  WeightedSimilarityEstimator a(locked), b(freed);
  for (const GeorefObservation& o : obs) { a.add(o); b.add(o); }
  GeorefSolution sa, sb;
  REQUIRE(a.solve(&sa));
  REQUIRE(b.solve(&sb));

  CHECK(sa.scale == doctest::Approx(1.0));
  CHECK(sb.scale == doctest::Approx(1.0 / 1.03).epsilon(1e-3));
  // With the scale locked the error has nowhere to go but the residual —
  // which is exactly why the residual is reported rather than swallowed.
  CHECK(sa.residual_rms_h_m > 0.15);
  CHECK(sb.residual_rms_h_m < 0.05);
}

TEST_CASE("gnss/georef/fusion_end_to_end_from_a_gnss_source") {
  // The real wiring: NMEA bytes -> GnssSource -> fixes; a synthetic local
  // trajectory in an ExternalPoseSource-shaped interpolator; GeorefFusion
  // pairing them by time and producing a CRS.
  const crs::Geodetic origin{22.2830, 114.1585, 48.0};
  const crs::EnuFrame frame = crs::make_enu_frame(origin);

  GnssSourceConfig gcfg;
  gcfg.min_fix_for_pose = FixType::kSingle;
  GnssSource src(gcfg);
  REQUIRE(src.set_origin(origin).ok());
  REQUIRE(src.start().ok());

  GeorefConfig fcfg;
  fcfg.min_fix = FixType::kRtkFloat;
  fcfg.min_interval_ns = 0;
  fcfg.resolve_interval_ns = 0;
  fcfg.estimator.min_fix = FixType::kRtkFloat;
  GeorefFusion fusion(fcfg);
  REQUIRE(fusion.set_enu_frame(frame).ok());

  const double yaw = 37.0 * crs::kDeg;
  const double cy = std::cos(yaw), sy = std::sin(yaw);
  const double tx = 12.0, ty = -8.0, tz = 1.25;

  Rng rng(2026);
  for (int t = 0; t < 200; ++t) {
    double gx = 0, gy = 0;
    loop_point(1.2 * t, &gx, &gy);
    const double gz = 2.0;

    // Emit the fix.
    const crs::Geodetic g = crs::enu_to_geodetic(
        frame, crs::Enu{gx + rng.gauss() * 0.02, gy + rng.gauss() * 0.02, gz});
    SimEpoch e;
    e.sod = t;
    e.lat = g.lat_deg;
    e.lon = g.lon_deg;
    e.alt = g.height_m + 2.0;  // MSL = ellipsoidal - geoid_sep(-2.0)
    push_str(src, sim_burst(e), static_cast<std::int64_t>(t) * 1000000000LL);
    if (t == 0) continue;
    src.flush();

    const GnssFix f = src.last_fix();
    // The local trajectory: ground truth mapped through the inverse of the
    // transform we are about to recover.
    const double dx = gx - tx, dy = gy - ty, dz = gz - tz;
    const double local[3] = {cy * dx + sy * dy, -sy * dx + cy * dy, dz};
    REQUIRE(fusion.add_pair(f.t_mono_ns, local, f).ok());
  }
  REQUIRE(fusion.solve());

  const GeorefSolution s = fusion.solution();
  MESSAGE("end-to-end: yaw " << s.yaw_deg << " deg (37), t = (" << s.translation[0] << ", "
                             << s.translation[1] << ", " << s.translation[2] << ") vs (12, -8, "
                             << "1.25), CEP95 " << s.cep95_m << " m over " << s.inliers
                             << " fixes");
  CHECK(s.converged);
  CHECK(std::fabs(s.yaw_deg - 37.0) < 0.2);
  CHECK(std::fabs(s.translation[0] - tx) < 0.05);
  CHECK(std::fabs(s.translation[1] - ty) < 0.05);
  CHECK(std::fabs(s.translation[2] - tz) < 0.10);
  CHECK(s.cep95_m < 0.20);

  // --- the outputs A9 and the viewer consume ---------------------------
  CHECK(fusion.epsg() == 32650);
  CHECK(fusion.epsg_string() == "EPSG:32650");
  CHECK(fusion.crs_wkt().find("PROJCS[\"WGS 84 / UTM zone 50N\"") == 0);
  CHECK(fusion.proj_string() == "+proj=utm +zone=50 +datum=WGS84 +units=m +no_defs +type=crs");

  crs::Geodetic o;
  REQUIRE(fusion.origin_wgs84(&o));
  CHECK(o.lat_deg == doctest::Approx(22.2830).epsilon(1e-12));
  crs::UtmCoord ou;
  REQUIRE(fusion.origin_utm(&ou));
  CHECK(ou.zone == 50);

  // A local point at a known place must land back on its true global one.
  const double corner_global[3] = {40.0, 25.0, 2.0};
  const double dx = corner_global[0] - tx, dy = corner_global[1] - ty, dz = corner_global[2] - tz;
  const double corner_local[3] = {cy * dx + sy * dy, -sy * dx + cy * dy, dz};
  double enu[3] = {0, 0, 0};
  REQUIRE(fusion.to_global_point(corner_local, enu).ok());
  CHECK(std::fabs(enu[0] - 40.0) < 0.05);
  CHECK(std::fabs(enu[1] - 25.0) < 0.05);

  crs::Geodetic wgs;
  REQUIRE(fusion.to_wgs84(corner_local, &wgs).ok());
  const crs::Enu check = crs::geodetic_to_enu(frame, wgs);
  CHECK(std::fabs(check.e - enu[0]) < 1e-6);
  crs::UtmCoord utm;
  REQUIRE(fusion.to_utm(corner_local, &utm).ok());
  CHECK(utm.zone == 50);
  CHECK(std::fabs(utm.easting - crs::geodetic_to_utm(wgs).easting) < 1e-6);

  // Pose transform, including the uncertainty it adds.
  Pose lp;
  lp.position[0] = corner_local[0];
  lp.position[1] = corner_local[1];
  lp.position[2] = corner_local[2];
  lp.orientation[3] = 1.0;
  lp.position_sigma_m = 0.01f;
  Pose gp;
  REQUIRE(fusion.to_global(lp, &gp).ok());
  CHECK(std::fabs(gp.position[0] - 40.0) < 0.05);
  CHECK(gp.position_sigma_m > lp.position_sigma_m);
  // Yaw-only rotation: the quaternion must stay in the xy-plane's z axis.
  CHECK(std::fabs(gp.orientation[0]) < 1e-12);
  CHECK(std::fabs(gp.orientation[1]) < 1e-12);
  CHECK(gp.orientation[2] == doctest::Approx(std::sin(s.yaw_rad * 0.5)).epsilon(1e-9));

  // A whole page of points.
  std::vector<PointVertex> pts(1000);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    pts[i].x = static_cast<float>(corner_local[0]);
    pts[i].y = static_cast<float>(corner_local[1]);
    pts[i].z = static_cast<float>(corner_local[2]);
  }
  std::vector<double> out(pts.size() * 3);
  REQUIRE(fusion.to_global_points(pts.data(), pts.size(), out.data()).ok());
  CHECK(std::fabs(out[0] - 40.0) < 0.05);
  CHECK(std::fabs(out[3 * 999 + 1] - 25.0) < 0.05);

  PageView page;
  page.data = pts.data();
  page.count = static_cast<std::uint32_t>(pts.size());
  CHECK(fusion.to_global_points(page, out.data(), 10).error() == ScanError::kCapacityExceeded);
  REQUIRE(fusion.to_global_points(page, out.data(), out.size()).ok());

  const GeorefFusion::Stats st = fusion.stats();
  CHECK(st.accepted == 199);
  CHECK(st.skipped_fix_quality == 0);
}

TEST_CASE("gnss/georef/unconverged_transform_refuses_to_be_applied") {
  GeorefConfig cfg;
  cfg.min_fix = FixType::kSingle;
  GeorefFusion f(cfg);
  const double p[3] = {1, 2, 3};
  double out[3];
  CHECK(f.to_global_point(p, out).error() == ScanError::kInvalidState);
  CHECK(f.crs_wkt().empty());
  CHECK(f.epsg() == 0);
  f.set_allow_unconverged(true);
  CHECK(f.to_global_point(p, out).ok());  // identity: a live preview's choice
  CHECK(out[0] == doctest::Approx(1.0));
}

TEST_CASE("gnss/georef/estimator_is_a_swappable_seam") {
  // The A7 hand-off: a factor graph implements GeorefEstimator and nothing
  // downstream changes. Proven here with a stub that returns a canned
  // transform.
  struct StubEstimator final : public GeorefEstimator {
    std::size_t n = 0;
    const char* name() const override { return "stub"; }
    void add(const GeorefObservation&) override { ++n; }
    void clear() override { n = 0; }
    std::size_t size() const override { return n; }
    bool solve(GeorefSolution* out) override {
      GeorefSolution s;
      s.converged = n >= 3;
      s.yaw_rad = 0.0;
      s.translation[0] = 1000.0;
      s.scale = 1.0;
      s.samples = s.inliers = n;
      s.horizontal_sigma_m = 0.5;
      double R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      double t[3] = {1000.0, 0.0, 0.0};
      // mat4_from_rt is se3.h's; the estimator side owns the matrix it fills.
      for (int i = 0; i < 16; ++i) s.global_from_local[i] = (i % 5 == 0) ? 1.0 : 0.0;
      s.global_from_local[3] = t[0];
      (void)R;
      *out = s;
      return s.converged;
    }
  };

  GeorefFusion f{GeorefConfig{}};
  REQUIRE(f.set_estimator(std::unique_ptr<GeorefEstimator>(new StubEstimator())).ok());
  GnssFix fix;
  fix.fix = FixType::kRtkFixed;
  fix.sigma_horizontal_m = 0.02f;
  const double local[3] = {0, 0, 0};
  REQUIRE(f.set_enu_frame(crs::make_enu_frame(crs::Geodetic{22.283, 114.1585, 0.0})).ok());
  for (int i = 0; i < 5; ++i) {
    fix.t_mono_ns = static_cast<std::int64_t>(i) * 2000000000LL;
    REQUIRE(f.add_pair(fix.t_mono_ns, local, fix).ok());
  }
  CHECK(f.solve());
  CHECK(f.converged());
  double out[3];
  const double p[3] = {0, 0, 0};
  REQUIRE(f.to_global_point(p, out).ok());
  CHECK(out[0] == doctest::Approx(1000.0));

  // Swapping after observations have accumulated is refused.
  CHECK(f.set_estimator(std::unique_ptr<GeorefEstimator>(new StubEstimator())).error() ==
        ScanError::kInvalidState);
}

// ===========================================================================
// 6. NTRIP (offline)
// ===========================================================================

TEST_CASE("gnss/ntrip/sourcetable_parsing") {
  // The record shape the S5 caster emits, plus a real-world RTK2go-style line
  // and the noise a caster puts around them.
  const std::string body =
      "SOURCETABLE 200 OK\r\n"
      "CAS;rtk2go.com;2101;RTK2go;RTK2go;0;USA;41.0;-74.0;0.0.0.0;0;\r\n"
      "NET;RTK2go;SNIP;B;N;http://rtk2go.com;;;\r\n"
      "STR;LIDARSCAN;LidarScan-Sim;RTCM 3.3;1005(1),1077(1),1087(1),1097(1),1127(1),1230(5);"
      "2;GPS+GLO+GAL+BDS;SIM;HKG;22.28;114.15;1;0;NtripCasterSim/0.1;none;B;N;9600;\r\n"
      "STR;HKSC;HongKongSatRef;RTCM 3.2;1004(1),1012(1);2;GPS+GLO;SATREF;HKG;22.32;114.14;"
      "0;0;TRIMBLE NETR9;none;B;Y;19200;far away\r\n"
      "ENDSOURCETABLE\r\n";

  const auto v = TcpNtripClient::parse_sourcetable(body);
  REQUIRE(v.size() == 2);
  CHECK(v[0].mountpoint == "LIDARSCAN");
  CHECK(v[0].identifier == "LidarScan-Sim");
  CHECK(v[0].format == "RTCM 3.3");
  CHECK(v[0].carrier == 2);
  CHECK(v[0].nav_system == "GPS+GLO+GAL+BDS");
  CHECK(v[0].country == "HKG");
  CHECK(v[0].lat_deg == doctest::Approx(22.28));
  CHECK(v[0].lon_deg == doctest::Approx(114.15));
  CHECK(v[0].needs_gga);
  CHECK(v[0].authentication == "B");
  CHECK_FALSE(v[0].fee);
  CHECK(v[0].bitrate == 9600);

  CHECK(v[1].mountpoint == "HKSC");
  CHECK_FALSE(v[1].needs_gga);
  CHECK(v[1].fee);
  CHECK(v[1].misc == "far away");

  // Baseline length is what decides Fixed vs Float, so a picker sorts on it.
  CHECK(v[0].distance_km(22.2830, 114.1585) < 2.0);
  CHECK(v[1].distance_km(22.2830, 114.1585) > 3.0);
  CHECK(v[0].distance_km(51.5, -0.12) > 9000.0);

  CHECK(TcpNtripClient::parse_sourcetable("").empty());
  CHECK(TcpNtripClient::parse_sourcetable("garbage\r\nENDSOURCETABLE\r\n").empty());
}

TEST_CASE("gnss/ntrip/argument_validation_and_lifecycle") {
  TcpNtripClient c;
  CHECK(c.state() == NtripState::kIdle);
  CHECK(c.correction_age_s() < 0.0f);  // unknown, not "fresh"
  CHECK_FALSE(c.receiving());

  NtripConfig cfg;
  CHECK(c.connect(cfg).error() == ScanError::kInvalidArgument);
  cfg.host = "127.0.0.1";
  CHECK(c.connect(cfg).error() == ScanError::kInvalidArgument);

  std::vector<std::string> mounts;
  CHECK(c.list_mountpoints(&mounts).error() == ScanError::kInvalidState);
  CHECK(c.list_mountpoints(nullptr).error() == ScanError::kInvalidArgument);

  // disconnect() on a client that never connected is a no-op, twice.
  CHECK(c.disconnect().ok());
  CHECK(c.disconnect().ok());

  // A closed port must fail fast rather than hanging on the OS SYN timeout.
  cfg.mountpoint = "NOPE";
  cfg.port = 1;  // reserved; nothing listens
  cfg.connect_timeout_ms = 2000;
  cfg.auto_reconnect = false;
  const Status s = c.connect(cfg);
  CHECK_FALSE(s.ok());
  CHECK((s.error() == ScanError::kNetworkError || s.error() == ScanError::kTimeout));
  CHECK(c.state() == NtripState::kFailed);
  CHECK(c.stats().connect_attempts >= 1);
}

TEST_CASE("gnss/ntrip/gga_upload_prefers_the_rovers_own_sentence") {
  // The provider path (the rover's verbatim GGA) and the synthesized
  // fallback both have to produce something a caster will accept, so both are
  // round-tripped through the parser here.
  GnssSourceConfig cfg;
  GnssSource src(cfg);
  REQUIRE(src.start().ok());
  SimEpoch e;
  e.sod = 0;
  push_str(src, sim_burst(e), 0);
  e.sod = 1;
  push_str(src, sim_burst(e), 1000000000LL);

  const std::string own = src.last_gga_sentence();
  REQUIRE(own.size() > 20);
  nmea::Sentence s;
  const std::string line = own.substr(0, own.find('\r'));
  REQUIRE(nmea::parse_sentence(line, &s) == nmea::NmeaError::kOk);
  nmea::GgaData g;
  REQUIRE(nmea::decode_gga(s, &g));
  CHECK(g.fix == FixType::kRtkFixed);

  nmea::GgaBuilderInput in;
  in.lat_deg = 22.2830;
  in.lon_deg = 114.1585;
  in.quality = 5;
  const std::string built = nmea::build_gga(in);
  const std::string built_line = built.substr(0, built.size() - 2);
  nmea::Sentence s2;
  REQUIRE(nmea::parse_sentence(built_line, &s2) == nmea::NmeaError::kOk);
  nmea::GgaData g2;
  REQUIRE(nmea::decode_gga(s2, &g2));
  CHECK(g2.fix == FixType::kRtkFloat);
  CHECK(g2.lat_deg == doctest::Approx(22.2830).epsilon(1e-7));
}

// ===========================================================================
// 7. END-TO-END AGAINST THE S5 SIMULATORS.
//
// These carry doctest::skip(), so a normal `ctest` / `scanengine_tests` run
// reports them skipped and costs nothing. They are driven by:
//
//     scanengine_tests --no-skip "--test-case=gnsssim/*" --success=false
//
// and docs/A10-gnss.md §8 has the eight-line CMake block that registers that
// command as a ctest entry labelled "sim-rtk" (A7 owns engine/CMakeLists.txt
// this wave, so A10 could not add it itself).
//
// They need python3 and spikes/s5-rtk-sim/. Anything missing makes them skip
// LOUDLY ("SKIPPED: …") rather than fail, matching the A3 precedent.
// ===========================================================================

#if !defined(_WIN32)

#include <csignal>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "scanengine/timesync/clock.h"

namespace {

std::string env_or(const char* k, const std::string& d) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? std::string(v) : d;
}

bool is_dir(const std::string& p) {
  struct stat st;
  return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The spike lives at <repo>/spikes/s5-rtk-sim. A ctest run's CWD is the build
// directory, which may be anywhere, so search upwards and honour an override.
std::string spike_dir() {
  const std::string env = env_or("SCANENGINE_S5_SIM_DIR", "");
  if (is_dir(env)) return env;
  char cwd[4096];
  if (::getcwd(cwd, sizeof(cwd)) == nullptr) return std::string();
  std::string p(cwd);
  for (int up = 0; up < 8; ++up) {
    const std::string cand = p + "/spikes/s5-rtk-sim";
    if (is_dir(cand)) return cand;
    const std::size_t slash = p.find_last_of('/');
    if (slash == std::string::npos || slash == 0) break;
    p = p.substr(0, slash);
  }
  return std::string();
}

std::string python_bin(const std::string& spike) {
  const std::string venv = spike + "/.venv/bin/python3";
  struct stat st;
  if (::stat(venv.c_str(), &st) == 0) return venv;
  return env_or("PYTHON", "python3");
}

class Child {
 public:
  ~Child() { terminate(); }
  bool spawn(const std::vector<std::string>& argv, const std::string& cwd,
             const std::string& log) {
    // Remove the log BEFORE forking: a previous run's file is otherwise
    // readable by the parent's readiness poll before the child truncates it,
    // and the test then talks to a server that has not bound yet.
    ::unlink(log.c_str());
    std::vector<char*> c;
    c.reserve(argv.size() + 1);
    for (const std::string& a : argv) c.push_back(const_cast<char*>(a.c_str()));
    c.push_back(nullptr);
    pid_ = ::fork();
    if (pid_ < 0) return false;
    if (pid_ == 0) {
      if (!cwd.empty()) { if (::chdir(cwd.c_str()) != 0) ::_exit(126); }
      // Failure here just means the child's output goes to the parent's
      // stdout instead of the log file; not worth aborting the child over
      // (glibc marks freopen warn_unused_result under _FORTIFY_SOURCE,
      // hence the void-cast).
      (void)::freopen(log.c_str(), "w", stdout);
      ::dup2(1, 2);
      ::execvp(c[0], c.data());
      ::_exit(127);
    }
    return true;
  }
  void terminate() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGTERM);
    int status = 0;
    for (int i = 0; i < 50; ++i) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(pid_, SIGKILL);
    (void)::waitpid(pid_, &status, 0);
    pid_ = -1;
  }

 private:
  pid_t pid_ = -1;
};

std::string read_file(const std::string& p) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return std::string();
  std::string out;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// Per-process paths and ports. Two build trees on one host (or a rerun that
// leaked a child) must not be able to talk to each other's simulator — that
// is a whole class of confusing flakiness for the price of a getpid().
std::string tmp_path(const char* leaf) {
  return env_or("TMPDIR", "/tmp") + "/scanengine_a10_" + std::to_string(::getpid()) + "_" + leaf;
}

int unique_port(int base) {
  return base + static_cast<int>(static_cast<unsigned>(::getpid()) % 900u);
}

// Wait until `pred()` or the deadline. Returns whether it succeeded.
template <typename F>
bool wait_until(F pred, double seconds) {
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return pred();
}

int connect_tcp(const char* host, int port);

}  // namespace

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {
int connect_tcp(const char* host, int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  ::inet_pton(AF_INET, host, &a.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
}  // namespace

TEST_CASE("gnsssim/live_nmea_over_tcp_into_the_gnss_source" * doctest::skip()) {
  const std::string spike = spike_dir();
  if (spike.empty()) {
    MESSAGE("SKIPPED: spikes/s5-rtk-sim not found (set SCANENGINE_S5_SIM_DIR)");
    return;
  }
  const int port = unique_port(39000);
  const std::string log = tmp_path("nmea_sim.log");
  Child sim;
  REQUIRE(sim.spawn({python_bin(spike), spike + "/nmea_sim.py", "--mode", "tcp", "--host",
                     "127.0.0.1", "--port", std::to_string(port), "--rate", "5", "--time-scale",
                     "25", "--scenario", "FIXED:60,FLOAT:20,SINGLE:10,FIXED:0", "--duration",
                     "120", "--seed", "7"},
                    spike, log));

  int fd = -1;
  if (!wait_until([&] { fd = connect_tcp("127.0.0.1", port); return fd >= 0; }, 15.0)) {
    MESSAGE("SKIPPED: nmea_sim.py did not come up: " << read_file(log));
    return;
  }

  GnssSourceConfig cfg;
  cfg.min_fix_for_pose = FixType::kSingle;
  cfg.min_fix_for_origin = FixType::kRtkFixed;
  GnssSource src(cfg);
  REQUIRE(src.start().ok());

  std::uint8_t buf[4096];
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20.0) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    REQUIRE(src.push_nmea(ByteSpan(buf, static_cast<std::size_t>(n)),
                          SteadyClock::now().nanos)
                .ok());
    if (src.fix_count() >= 590) break;
  }
  ::close(fd);
  src.flush();

  const GnssStats st = src.stats();
  MESSAGE("live NMEA: " << st.nmea.sentences_ok << " sentences, " << st.epochs << " epochs, "
                        << st.nmea.checksum_failed << " checksum failures, fix mix "
                        << st.by_fix[4] << "/" << st.by_fix[3] << "/" << st.by_fix[1]
                        << " (Fixed/Float/Single)");

  // The simulator is standards-conformant, so a strict parser must accept
  // 100 % of it.
  CHECK(st.nmea.checksum_failed == 0);
  CHECK(st.nmea.malformed == 0);
  CHECK(st.nmea.sentences_ok > 1000);
  CHECK(st.epochs > 200);
  CHECK(st.gst_epochs == st.epochs);

  // The scripted timeline must be visible: 60 s Fixed, 20 s Float, 10 s
  // Single, then Fixed forever, at 5 Hz.
  CHECK(st.by_fix[static_cast<int>(FixType::kRtkFixed)] > 100);
  CHECK(st.by_fix[static_cast<int>(FixType::kRtkFloat)] > 50);
  CHECK(st.by_fix[static_cast<int>(FixType::kSingle)] > 20);
  CHECK(st.by_fix[static_cast<int>(FixType::kNone)] == 0);

  // Sigmas came from the simulator's GST, and they match the noise it
  // actually injected (S5 REPORT.md: the two are derived from one table).
  double s_fixed = 0.0, s_float = 0.0, s_single = 0.0;
  std::size_t n_fixed = 0, n_float = 0, n_single = 0;
  for (const GnssFix& f : src.fixes()) {
    CHECK(f.sigma_from_gst);
    if (f.fix == FixType::kRtkFixed) { s_fixed += f.sigma_horizontal_m; ++n_fixed; }
    if (f.fix == FixType::kRtkFloat) { s_float += f.sigma_horizontal_m; ++n_float; }
    if (f.fix == FixType::kSingle) { s_single += f.sigma_horizontal_m; ++n_single; }
  }
  REQUIRE(n_fixed > 0);
  REQUIRE(n_float > 0);
  REQUIRE(n_single > 0);
  CHECK(s_fixed / n_fixed == doctest::Approx(0.02).epsilon(0.2));
  CHECK(s_float / n_float == doctest::Approx(0.30).epsilon(0.2));
  CHECK(s_single / n_single == doctest::Approx(2.00).epsilon(0.2));

  // The origin was anchored on an RTK Fixed epoch, and the trajectory is the
  // simulator's 40 x 25 m loop.
  crs::Geodetic o;
  REQUIRE(src.origin(&o));
  CHECK(o.lat_deg == doctest::Approx(22.2830).epsilon(1e-3));
  double min_e = 1e9, max_e = -1e9, min_n = 1e9, max_n = -1e9;
  for (std::int64_t t = 0; t < 1; ++t) { (void)t; }
  std::int64_t first = 0, last = 0;
  REQUIRE(src.time_span(&first, &last));
  for (std::int64_t t = first; t <= last; t += 100000000LL) {
    const PoseSample s = src.sample_at(t);
    if (!s.has_pose) continue;
    min_e = std::min(min_e, s.pose.position[0]);
    max_e = std::max(max_e, s.pose.position[0]);
    min_n = std::min(min_n, s.pose.position[1]);
    max_n = std::max(max_n, s.pose.position[1]);
  }
  MESSAGE("trajectory bounds: E [" << min_e << ", " << max_e << "]  N [" << min_n << ", "
                                   << max_n << "] (sim loop is 40 x 25 m)");
  CHECK(max_e - min_e > 30.0);
  CHECK(max_e - min_e < 55.0);
  CHECK(max_n - min_n > 18.0);
  CHECK(max_n - min_n < 40.0);
}

TEST_CASE("gnsssim/live_ntrip_caster_stream_gga_upload_and_reconnect" * doctest::skip()) {
  const std::string spike = spike_dir();
  if (spike.empty()) {
    MESSAGE("SKIPPED: spikes/s5-rtk-sim not found (set SCANENGINE_S5_SIM_DIR)");
    return;
  }
  const int port = unique_port(40000);
  const std::string log = tmp_path("ntrip_caster.log");
  const std::string gga_log = tmp_path("ntrip_gga.log");
  const std::string driver = tmp_path("caster_driver.py");

  // A tiny driver around the UNMODIFIED spike modules. The caster's CLI keeps
  // the rover's GGA uploads in memory (`MountConfig.gga_log`) and prints
  // nothing, so this is the only way to observe them from another process.
  {
    std::FILE* f = std::fopen(driver.c_str(), "w");
    REQUIRE(f != nullptr);
    std::fprintf(f,
                 "import sys, os, time\n"
                 "sys.path.insert(0, %s)\n"
                 "import rtcm_tool, ntrip_caster_sim as N\n"
                 "fx = os.path.join(%s, 'fixtures', 'canned_rtcm.bin')\n"
                 "os.makedirs(os.path.dirname(fx), exist_ok=True)\n"
                 "if not os.path.exists(fx):\n"
                 "    rtcm_tool.generate_canned_stream(fx, count=300, seed=0)\n"
                 "m = N.MountConfig(name='LIDARSCAN', username='lidarscan',\n"
                 "                  password='s5spike', rtcm_file=fx,\n"
                 "                  frame_interval_s=0.05, drop_after_frames=25, max_drops=1)\n"
                 "c = N.NtripCasterSim([m], host='127.0.0.1', port=%d)\n"
                 "c.start()\n"
                 "print('ready', flush=True)\n"
                 "while True:\n"
                 "    time.sleep(0.25)\n"
                 "    with open(%s, 'w') as g:\n"
                 "        g.write('gga=%%d drops=%%d\\n' %% (len(m.gga_log), m.drops_used))\n"
                 "        for ts, line in m.gga_log:\n"
                 "            g.write(line + '\\n')\n",
                 ("'" + spike + "'").c_str(), ("'" + spike + "'").c_str(), port,
                 ("'" + gga_log + "'").c_str());
    std::fclose(f);
  }

  Child caster;
  REQUIRE(caster.spawn({python_bin(spike), driver}, spike, log));
  if (!wait_until([&] { return read_file(log).find("ready") != std::string::npos; }, 20.0)) {
    MESSAGE("SKIPPED: caster driver did not start: " << read_file(log));
    return;
  }

  // --- sourcetable ------------------------------------------------------
  TcpNtripClient client;
  NtripConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = static_cast<std::uint16_t>(port);
  cfg.mountpoint = "LIDARSCAN";
  cfg.username = "lidarscan";
  cfg.password = "s5spike";
  cfg.gga_interval_ms = 500;
  cfg.reconnect_initial_ms = 200;
  cfg.reconnect_max_ms = 1000;
  cfg.stall_timeout_ms = 8000;

  std::vector<NtripSource> sources;
  REQUIRE(wait_until([&] { return client.fetch_sourcetable(cfg, &sources).ok(); }, 10.0));
  REQUIRE(sources.size() == 1);
  CHECK(sources[0].mountpoint == "LIDARSCAN");
  CHECK(sources[0].format == "RTCM 3.3");
  CHECK(sources[0].needs_gga);

  // --- authentication is enforced --------------------------------------
  {
    TcpNtripClient bad;
    NtripConfig b = cfg;
    b.password = "wrong";
    b.auto_reconnect = false;
    const Status s = bad.connect(b);
    CHECK(s.error() == ScanError::kPermissionDenied);
    CHECK(bad.state() == NtripState::kFailed);
    CHECK(bad.disconnect().ok());
  }
  {
    TcpNtripClient bad;
    NtripConfig b = cfg;
    b.mountpoint = "NOSUCHMOUNT";
    b.auto_reconnect = false;
    CHECK(bad.connect(b).error() == ScanError::kNotFound);
    CHECK(bad.disconnect().ok());
  }

  // --- stream ------------------------------------------------------------
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint64_t> frame_bytes{0};
  std::vector<NtripState> states;
  std::mutex states_m;
  client.set_rtcm_handler([&](ByteSpan f) {
    // What the app forwards to the rover: whole, CRC-valid frames.
    CHECK(f.size() >= 6);
    CHECK(f[0] == rtcm3::kPreamble);
    rtcm3::FrameInfo info;
    CHECK(rtcm3::validate_frame(f.data(), f.size(), &info));
    CHECK(info.crc_ok);
    frames.fetch_add(1);
    frame_bytes.fetch_add(f.size());
  });
  client.set_state_callback([&](NtripState s, ScanError) {
    std::lock_guard<std::mutex> lk(states_m);
    states.push_back(s);
  });
  client.set_gga_provider([&](std::string* out) {
    nmea::GgaBuilderInput in;
    in.lat_deg = 22.2830;
    in.lon_deg = 114.1585;
    in.alt_msl_m = 50.0;
    in.quality = 4;
    in.satellites = 22;
    in.hdop = 0.6;
    *out = nmea::build_gga(in);
    return true;
  });

  REQUIRE(client.connect(cfg).ok());
  CHECK(client.stats().ntrip_version_used == 2);

  // 25 frames, then the caster force-closes once (drop_after_frames=25,
  // max_drops=1). The client must notice, back off, reconnect and keep
  // delivering.
  REQUIRE(wait_until([&] { return frames.load() >= 60; }, 30.0));

  const NtripStats st = client.stats();
  MESSAGE("NTRIP: " << st.rtcm.frames_ok << " valid frames (" << st.rtcm.frames_crc_failed
                    << " CRC failures), " << st.bytes_received << " bytes, " << st.gga_sent
                    << " GGA uploads, " << st.disconnects << " disconnects, " << st.reconnects
                    << " reconnects, corrections age " << client.correction_age_s() << " s");

  CHECK(frames.load() >= 60);
  CHECK(st.rtcm.frames_ok >= 60);
  CHECK(st.rtcm.frames_crc_failed == 0);
  CHECK(st.rtcm.type_slots_used >= 1);
  CHECK(client.receiving());
  CHECK(client.correction_age_s() >= 0.0f);
  CHECK(client.correction_age_s() < 5.0f);

  // The drop was exercised and recovered from.
  CHECK(st.disconnects >= 1);
  CHECK(st.reconnects >= 1);
  CHECK(st.connect_attempts >= 2);
  {
    std::lock_guard<std::mutex> lk(states_m);
    bool saw_reconnecting = false, saw_streaming = false;
    for (NtripState s : states) {
      if (s == NtripState::kReconnecting) saw_reconnecting = true;
      if (s == NtripState::kStreaming) saw_streaming = true;
    }
    CHECK(saw_reconnecting);
    CHECK(saw_streaming);
  }

  // The caster logged our GGA uploads.
  REQUIRE(wait_until(
      [&] {
        const std::string g = read_file(gga_log);
        return g.find("gga=") != std::string::npos && g.find("gga=0 ") == std::string::npos;
      },
      10.0));
  const std::string gl = read_file(gga_log);
  MESSAGE("caster GGA log: " << gl.substr(0, 200));
  CHECK(gl.find("$GNGGA") != std::string::npos);
  CHECK(gl.find("drops=1") != std::string::npos);
  CHECK(st.gga_sent >= 1);

  REQUIRE(client.disconnect().ok());
  CHECK(client.state() == NtripState::kIdle);
  // Reconnecting the same object must work.
  REQUIRE(client.connect(cfg).ok());
  REQUIRE(client.disconnect().ok());
}

#endif  // !_WIN32
