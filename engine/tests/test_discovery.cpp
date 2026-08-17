// test_discovery.cpp — A16: beacon parsing, host reachability, serial probe
// state machines, the discovery timeout, and the single-instance guard.
//
// THE CENTRAL FIXTURE IS REAL. tests/integration/data/mid360_beacon.bin is two
// verbatim 430-byte heartbeats lifted out of captures/mid360_real_30s.livoxdump
// (port 56201, 2026-08-17, SN ARMCP7K0034759 at 192.168.1.159 with host
// 192.168.1.5 persisted). Every offset the parser believes in is asserted
// against those bytes, so a "harmless" refactor that shifts a field by two
// cannot pass.
//
// NO REAL PORTS AND NO REAL LIDAR are required or used. The serial probes are
// tested through their sniffers with injected byte streams; the UDP path is
// tested by sending the captured datagram to a loopback port this test binds
// itself.
//
// Header-first and alone, so this file doubles as a self-containment check on
// the two new public headers.
#include "scanengine/discovery/discovery.h"

#include "scanengine/core/instance_guard.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace scanengine;
using namespace scanengine::discovery;

namespace {

// --- fixtures ---------------------------------------------------------------

std::string repo_root_from_this_file() {
  std::string here = __FILE__;  // .../engine/tests/test_discovery.cpp
  for (char& c : here) {
    if (c == '\\') c = '/';  // MSVC's __FILE__ (see test_e2_replay_golden.cpp)
  }
  const std::size_t cut = here.rfind("engine/tests/");
  return cut != std::string::npos ? here.substr(0, cut) : std::string();
}

std::string data_path(const char* filename) {
  const std::string root = repo_root_from_this_file();
  if (!root.empty()) return root + "engine/tests/integration/data/" + filename;
  return std::string("tests/integration/data/") + filename;  // CWD == engine/
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::vector<std::uint8_t> out;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return out;
  std::uint8_t buf[8192];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

// The fixture is two back-to-back frames; each is self-delimiting via the
// u16 length at offset 2, which is how a recvfrom loop consumes them too.
std::vector<std::vector<std::uint8_t>> beacon_frames() {
  const std::vector<std::uint8_t> all = read_file(data_path("mid360_beacon.bin"));
  std::vector<std::vector<std::uint8_t>> out;
  std::size_t off = 0;
  while (off + 24 <= all.size()) {
    const std::size_t len =
        static_cast<std::size_t>(all[off + 2]) | (static_cast<std::size_t>(all[off + 3]) << 8);
    if (len < 24 || off + len > all.size()) break;
    out.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(off),
                     all.begin() + static_cast<std::ptrdiff_t>(off + len));
    off += len;
  }
  return out;
}

LocalInterface iface(const char* name, const char* ip, const char* mask, bool loop = false,
                     bool up = true) {
  LocalInterface li;
  li.name = name;
  li.ipv4 = ip;
  li.netmask = mask;
  li.is_loopback = loop;
  li.is_up = up;
  return li;
}

std::string temp_lock_path(const char* tag) {
  char buf[256];
#if defined(_WIN32)
  char tmp[MAX_PATH + 1] = {0};
  ::GetTempPathA(sizeof(tmp), tmp);
  std::snprintf(buf, sizeof(buf), "%slidarscan-test-%s-%lld.lock", tmp, tag,
                static_cast<long long>(CurrentProcessId()));
#else
  const char* tmp = std::getenv("TMPDIR");
  std::snprintf(buf, sizeof(buf), "%slidarscan-test-%s-%lld.lock",
                (tmp && tmp[0]) ? tmp : "/tmp/", tag,
                static_cast<long long>(CurrentProcessId()));
#endif
  return buf;
}

void remove_file(const std::string& path) { std::remove(path.c_str()); }

std::int64_t millis_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// ===========================================================================
// The beacon parser, against the real capture
// ===========================================================================

TEST_CASE("discovery/beacon_parses_the_real_capture") {
  const auto frames = beacon_frames();
  REQUIRE(frames.size() == 2);
  CHECK(frames[0].size() == 430);
  CHECK(frames[1].size() == 430);

  Result<Mid360Beacon> r = ParseMid360Beacon(frames[0].data(), frames[0].size());
  REQUIRE(r.ok());
  const Mid360Beacon b = r.value();

  // The identity the field session recorded. The frame's SN field is
  // "ARMCP7K0034759"; FIELD_SESSION_2026-08-17.md quotes its tail,
  // "MCP7K0034759", which is the part printed on the unit — so assert both
  // the exact bytes and that the quoted serial is a suffix of them.
  CHECK(b.sn == "ARMCP7K0034759");
  CHECK(b.sn.find("MCP7K0034759") != std::string::npos);

  // The addresses. These four are the entire reason this module exists.
  CHECK(b.lidar_ip == "192.168.1.159");
  CHECK(b.netmask == "255.255.255.0");
  CHECK(b.gateway == "192.168.1.1");
  CHECK(b.persisted_host_ip == "192.168.1.5");
  CHECK(b.persisted_imu_host_ip == "192.168.1.5");
  CHECK(b.persisted_point_port == 56301);
  CHECK(b.persisted_imu_port == 56401);

  // The heartbeat also tells us where IT is broadcasting — 56201, the port
  // the capture was taken on.
  CHECK(b.push_port_seen == 56201);

  // Identity and firmware.
  CHECK(b.dev_type == "Mid-360");
  CHECK(b.fw_type == "App");
  CHECK(b.fw_version_text == "35010108");
  CHECK(b.fw_version == "35.1.1.8");  // key 0x8002, decoded octet by octet
  CHECK(b.build_time == "2025/06/09");
  CHECK(b.product_info.rfind("DevType:Mid-360", 0) == 0);
  CHECK(b.mac == "ec:72:f7:89:13:5f");

  CHECK(b.key_count == 31);
  CHECK(b.crc_ok);
  CHECK_FALSE(b.heuristic);

  const std::string desc = b.describe();
  CHECK(desc.find("192.168.1.159") != std::string::npos);
  CHECK(desc.find("192.168.1.5") != std::string::npos);
}

TEST_CASE("discovery/beacon_second_record_is_the_same_lidar") {
  const auto frames = beacon_frames();
  REQUIRE(frames.size() == 2);
  // The two captured frames differ (sequence number, core temperature, the
  // device clock) but are the same lidar — which is what dedup relies on.
  CHECK(frames[0] != frames[1]);
  Result<Mid360Beacon> a = ParseMid360Beacon(frames[0].data(), frames[0].size());
  Result<Mid360Beacon> b = ParseMid360Beacon(frames[1].data(), frames[1].size());
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(a.value().sn == b.value().sn);
  CHECK(a.value().lidar_ip == b.value().lidar_ip);
  CHECK(b.value().crc_ok);
}

TEST_CASE("discovery/beacon_crcs_are_the_real_algorithms") {
  const auto frames = beacon_frames();
  REQUIRE(!frames.empty());
  std::vector<std::uint8_t> f = frames[0];
  CHECK(Mid360HeaderCrcOk(f.data(), f.size()));
  CHECK(Mid360PayloadCrcOk(f.data(), f.size()));

  // Corrupt a payload byte: the payload CRC must notice, the header CRC must
  // not care. (If both "passed", the CRC code would be a no-op.)
  std::vector<std::uint8_t> corrupt = f;
  corrupt[200] = static_cast<std::uint8_t>(corrupt[200] ^ 0xFF);
  CHECK(Mid360HeaderCrcOk(corrupt.data(), corrupt.size()));
  CHECK_FALSE(Mid360PayloadCrcOk(corrupt.data(), corrupt.size()));

  // Corrupt the sequence number: header CRC fails.
  std::vector<std::uint8_t> hdr_bad = f;
  hdr_bad[4] = static_cast<std::uint8_t>(hdr_bad[4] ^ 0xFF);
  CHECK_FALSE(Mid360HeaderCrcOk(hdr_bad.data(), hdr_bad.size()));

  // A parse of the corrupt frame still SUCCEEDS (a beacon is advisory) but
  // says so.
  Result<Mid360Beacon> r = ParseMid360Beacon(corrupt.data(), corrupt.size());
  REQUIRE(r.ok());
  CHECK_FALSE(r.value().crc_ok);
  CHECK(r.value().lidar_ip == "192.168.1.159");
}

TEST_CASE("discovery/beacon_rejects_what_is_not_a_beacon") {
  const std::uint8_t empty[4] = {0, 0, 0, 0};
  CHECK(ParseMid360Beacon(nullptr, 100).error() == ScanError::kInvalidArgument);
  CHECK(ParseMid360Beacon(empty, sizeof(empty)).error() == ScanError::kProtocolError);

  // Right size, wrong protocol: a Mid-360 POINT datagram starts 0x00, not
  // 0xAA, and an NMEA line starts '$'. Neither may be mistaken for a beacon.
  std::vector<std::uint8_t> not_beacon(430, 0x00);
  CHECK(ParseMid360Beacon(not_beacon.data(), not_beacon.size()).error() ==
        ScanError::kProtocolError);

  std::vector<std::uint8_t> nmea(430, static_cast<std::uint8_t>('$'));
  CHECK(ParseMid360Beacon(nmea.data(), nmea.size()).error() == ScanError::kProtocolError);

  // 0xAA-framed noise: the sof matches, nothing else does. It must not
  // produce a confident record — with the fallback off it is kCorruptData.
  std::vector<std::uint8_t> framed_noise(430, 0x00);
  framed_noise[0] = 0xAA;
  framed_noise[2] = 0xAE;
  framed_noise[3] = 0x01;
  CHECK(ParseMid360Beacon(framed_noise.data(), framed_noise.size(), false).error() ==
        ScanError::kCorruptData);
  CHECK(ParseMid360Beacon(framed_noise.data(), framed_noise.size(), true).error() ==
        ScanError::kCorruptData);
}

TEST_CASE("discovery/beacon_heuristic_survives_a_broken_key_walk") {
  const auto frames = beacon_frames();
  REQUIRE(!frames.empty());

  // Simulate firmware that renumbered/extended the payload: the declared key
  // count no longer matches the layout, so the walk runs off the end. The
  // TEXT and the IPv4-shaped bytes are untouched — which is exactly the
  // situation the fallback exists for.
  std::vector<std::uint8_t> f = frames[0];
  f[24] = 0xF0;  // key_num = 240, far more than the frame holds
  f[25] = 0x00;

  CHECK(ParseMid360Beacon(f.data(), f.size(), /*allow_heuristic=*/false).error() ==
        ScanError::kCorruptData);

  Result<Mid360Beacon> r = ParseMid360Beacon(f.data(), f.size(), /*allow_heuristic=*/true);
  REQUIRE(r.ok());
  const Mid360Beacon b = r.value();
  CHECK(b.heuristic);
  // The anchor scan recovers the operator-relevant facts:
  CHECK(b.lidar_ip == "192.168.1.159");
  CHECK(b.netmask == "255.255.255.0");
  CHECK(b.gateway == "192.168.1.1");
  CHECK(b.persisted_host_ip == "192.168.1.5");
  CHECK(b.sn == "ARMCP7K0034759");
  CHECK(b.dev_type == "Mid-360");
  CHECK(b.fw_version_text == "35010108");
  CHECK(b.describe().find("heuristic") != std::string::npos);
}

// ===========================================================================
// IPv4 arithmetic
// ===========================================================================

TEST_CASE("discovery/ipv4_helpers") {
  std::uint32_t v = 0;
  CHECK(ParseIpv4("192.168.1.159", &v));
  CHECK(v == 0xC0A8019Fu);
  CHECK(Ipv4ToString(v) == "192.168.1.159");
  CHECK(Ipv4ToString(0) == "0.0.0.0");
  CHECK(Ipv4ToString(0xFFFFFFFFu) == "255.255.255.255");

  CHECK_FALSE(ParseIpv4("", &v));
  CHECK_FALSE(ParseIpv4("192.168.1", &v));
  CHECK_FALSE(ParseIpv4("192.168.1.256", &v));
  CHECK_FALSE(ParseIpv4("192.168.1.1.1", &v));
  CHECK_FALSE(ParseIpv4("not an ip", &v));

  CHECK(SameSubnet("192.168.1.5", "192.168.1.159", "255.255.255.0"));
  CHECK_FALSE(SameSubnet("192.168.2.5", "192.168.1.159", "255.255.255.0"));
  CHECK(SameSubnet("192.168.2.5", "192.168.1.159", "255.255.0.0"));
  // An absent mask falls back to /24 rather than refusing to answer.
  CHECK(SameSubnet("192.168.1.5", "192.168.1.159", ""));

  CHECK(PrefixLen("255.255.255.0") == 24);
  CHECK(PrefixLen("255.255.0.0") == 16);
  CHECK(PrefixLen("255.255.255.252") == 30);
  CHECK(PrefixLen("0.0.0.0") == 0);
  CHECK(PrefixLen("255.0.255.0") == -1);  // non-contiguous
  CHECK(PrefixLen("garbage") == -1);
}

// ===========================================================================
// Host reachability — the field failure, in four shapes
// ===========================================================================

TEST_CASE("discovery/host_check_against_fake_interfaces") {
  const auto frames = beacon_frames();
  REQUIRE(!frames.empty());
  Result<Mid360Beacon> parsed = ParseMid360Beacon(frames[0].data(), frames[0].size());
  REQUIRE(parsed.ok());
  const Mid360Beacon beacon = parsed.value();

  SUBCASE("this machine holds the persisted host address") {
    const std::vector<LocalInterface> ifs = {
        iface("lo0", "127.0.0.1", "255.0.0.0", true),
        iface("en0", "10.0.0.42", "255.255.255.0"),
        iface("en7", "192.168.1.5", "255.255.255.0"),
    };
    const HostCheck hc = CheckHostReachability(beacon, ifs);
    CHECK(hc.host_ip_is_local);
    CHECK(hc.on_lidar_subnet);
    CHECK(hc.suggested_host_ip == "192.168.1.5");
    CHECK(hc.suggested_interface == "en7");
    CHECK(hc.note.rfind("Ready:", 0) == 0);
    CHECK(hc.note.find("en7") != std::string::npos);
  }

  SUBCASE("right wire, wrong address — the exact field failure") {
    const std::vector<LocalInterface> ifs = {
        iface("lo0", "127.0.0.1", "255.0.0.0", true),
        iface("en7", "192.168.1.77", "255.255.255.0"),
    };
    const HostCheck hc = CheckHostReachability(beacon, ifs);
    CHECK_FALSE(hc.host_ip_is_local);
    CHECK(hc.on_lidar_subnet);
    REQUIRE(hc.local_candidates.size() == 1);
    CHECK(hc.local_candidates[0] == "192.168.1.77");
    CHECK(hc.suggested_interface == "en7");
    CHECK(hc.suggested_host_ip == "192.168.1.5");
    // The sentence has to name the address the operator must add, and the
    // interface to add it on.
    CHECK(hc.note.find("expects host 192.168.1.5") != std::string::npos);
    CHECK(hc.note.find("192.168.1.77") != std::string::npos);
    CHECK(hc.note.find("en7") != std::string::npos);
    CHECK(hc.note.find("/24") != std::string::npos);
  }

  SUBCASE("wrong network entirely") {
    const std::vector<LocalInterface> ifs = {
        iface("lo0", "127.0.0.1", "255.0.0.0", true),
        iface("en0", "10.11.12.13", "255.255.255.0"),
    };
    const HostCheck hc = CheckHostReachability(beacon, ifs);
    CHECK_FALSE(hc.host_ip_is_local);
    CHECK_FALSE(hc.on_lidar_subnet);
    CHECK(hc.local_candidates.empty());
    CHECK(hc.note.find("no address on that subnet") != std::string::npos);
    CHECK(hc.note.find("10.11.12.13") != std::string::npos);
  }

  SUBCASE("no interfaces at all") {
    const HostCheck hc = CheckHostReachability(beacon, {});
    CHECK_FALSE(hc.host_ip_is_local);
    CHECK_FALSE(hc.on_lidar_subnet);
    CHECK(hc.note.find("192.168.1.5") != std::string::npos);
  }

  SUBCASE("loopback holding the address does not count") {
    // A lidar cannot reach 127/8. A naive "do I have this IP?" says yes here
    // and produces a session with zero points.
    const std::vector<LocalInterface> ifs = {
        iface("lo0", "192.168.1.5", "255.255.255.0", true),
    };
    const HostCheck hc = CheckHostReachability(beacon, ifs);
    CHECK_FALSE(hc.host_ip_is_local);
    CHECK_FALSE(hc.on_lidar_subnet);
  }

  SUBCASE("a down interface is not a candidate") {
    const std::vector<LocalInterface> ifs = {
        iface("en7", "192.168.1.77", "255.255.255.0", false, /*up=*/false),
    };
    const HostCheck hc = CheckHostReachability(beacon, ifs);
    CHECK_FALSE(hc.on_lidar_subnet);
  }

  SUBCASE("a factory-fresh lidar with no persisted host") {
    Mid360Beacon fresh = beacon;
    fresh.persisted_host_ip.clear();
    const std::vector<LocalInterface> ifs = {
        iface("en7", "192.168.1.77", "255.255.255.0"),
    };
    const HostCheck hc = CheckHostReachability(fresh, ifs);
    CHECK_FALSE(hc.host_ip_is_local);
    CHECK(hc.on_lidar_subnet);
    CHECK(hc.suggested_host_ip == "192.168.1.77");
    CHECK(hc.note.find("no host address configured") != std::string::npos);
  }
}

TEST_CASE("discovery/enumerate_local_interfaces_on_this_machine") {
  Result<std::vector<LocalInterface>> r = EnumerateLocalInterfaces();
  if (!r.ok()) {
    // Only a platform with neither getifaddrs nor GetAdaptersAddresses.
    CHECK(r.error() == ScanError::kNotSupported);
    return;
  }
  const std::vector<LocalInterface> ifs = r.value();
  // Every machine that can run this test has a loopback.
  bool saw_loopback = false;
  for (const LocalInterface& li : ifs) {
    CHECK_FALSE(li.name.empty());
    std::uint32_t v = 0;
    CHECK(ParseIpv4(li.ipv4, &v));
    if (li.is_loopback) saw_loopback = true;
  }
  CHECK(saw_loopback);

  // And the real check must run without a crash whatever this machine's
  // network looks like.
  Mid360Beacon b;
  b.lidar_ip = "192.168.1.159";
  b.netmask = "255.255.255.0";
  b.persisted_host_ip = "192.168.1.5";
  const HostCheck hc = CheckHostReachability(b);
  CHECK_FALSE(hc.note.empty());
}

// ===========================================================================
// Serial probe state machines — injected bytes, no ports
// ===========================================================================

TEST_CASE("discovery/d6_sniffer_identifies_the_wire_signature") {
  const std::vector<std::uint8_t> rev = d6test::build_revolution(6, 20, 1000, 128, 10);

  SUBCASE("a clean revolution identifies") {
    D6Sniffer s;
    CHECK_FALSE(s.Identified());
    s.Feed(rev.data(), rev.size());
    CHECK(s.Identified());
    CHECK(s.packets_ok() >= D6Sniffer::kPacketsToIdentify);
    CHECK(s.packets_bad_checksum() == 0);
    CHECK_FALSE(s.LooksLikeText());
  }

  SUBCASE("torn across arbitrary chunk boundaries") {
    D6Sniffer s;
    for (std::size_t i = 0; i < rev.size(); i += 7) {
      s.Feed(rev.data() + i, std::min<std::size_t>(7, rev.size() - i));
    }
    CHECK(s.Identified());
  }

  SUBCASE("one packet is not enough") {
    D6Sniffer s;
    d6test::PacketSpec ps;
    ps.samples = {d6test::Sample{1000, 128, false}};
    const std::vector<std::uint8_t> one = d6test::build(ps);
    s.Feed(one.data(), one.size());
    CHECK(s.packets_ok() == 1);
    CHECK_FALSE(s.Identified());
  }

  SUBCASE("the spec-literal checksum variant is NOT accepted") {
    // The field session closed this: the device computes the VENDOR variant
    // (2430/2430 vs 143). A probe that accepted both would identify noise.
    D6Sniffer s;
    for (int k = 0; k < 4; ++k) {
      d6test::PacketSpec ps;
      ps.cs_mode = d6test::CsMode::kSpec;
      // SEVEN samples, not eight: the two readings XOR to the same value for
      // an even run of identical samples, so an even count would let a
      // spec-checksummed packet pass the vendor check by coincidence.
      for (int i = 0; i < 7; ++i) ps.samples.push_back(d6test::Sample{1000, 128, false});
      ps.first_angle_deg = 10.0 * k;
      ps.last_angle_deg = 10.0 * k + 5.0;
      const std::vector<std::uint8_t> p = d6test::build(ps);
      s.Feed(p.data(), p.size());
    }
    CHECK_FALSE(s.Identified());
    CHECK(s.packets_bad_checksum() > 0);
  }

  SUBCASE("NMEA text latches the no-write flag") {
    D6Sniffer s;
    const char* nmea =
        "$GNGGA,000000.00,2216.980000,N,11409.510000,E,4,22,0.6,50.00,M,-2.0,M,,*4A\r\n"
        "$GPTHS,123.4,A*2B\r\n";
    s.Feed(reinterpret_cast<const std::uint8_t*>(nmea), std::strlen(nmea));
    CHECK(s.LooksLikeText());
    CHECK_FALSE(s.Identified());
  }

  SUBCASE("a Unicore binary-ish log also latches") {
    D6Sniffer s;
    const char* uni = "#UNIHEADINGA,68,GPS,FINE,2190,375100.000,0,0,18,10;SOL_COMPUTED*a1b2c3d4";
    s.Feed(reinterpret_cast<const std::uint8_t*>(uni), std::strlen(uni));
    CHECK(s.LooksLikeText());
  }

  SUBCASE("random binary identifies nothing") {
    D6Sniffer s;
    std::vector<std::uint8_t> noise(4096);
    std::uint32_t x = 0x12345678u;
    for (std::uint8_t& c : noise) {
      x = x * 1664525u + 1013904223u;
      c = static_cast<std::uint8_t>(x >> 24);
    }
    s.Feed(noise.data(), noise.size());
    CHECK_FALSE(s.Identified());
    CHECK_FALSE(s.LooksLikeText());
  }

  SUBCASE("reset clears everything") {
    D6Sniffer s;
    s.Feed(rev.data(), rev.size());
    REQUIRE(s.Identified());
    s.Reset();
    CHECK_FALSE(s.Identified());
    CHECK(s.packets_ok() == 0);
    CHECK_FALSE(s.LooksLikeText());
  }
}

TEST_CASE("discovery/um982_sniffer_identifies_nmea_and_heading") {
  SUBCASE("the real field capture identifies, with heading") {
    const std::vector<std::uint8_t> nmea = read_file(data_path("field_um982_30s.nmea"));
    REQUIRE(nmea.size() > 100);
    Um982Sniffer s;
    // 64-byte chunks, the way a serial read actually arrives.
    for (std::size_t i = 0; i < nmea.size(); i += 64) {
      s.Feed(nmea.data() + i, std::min<std::size_t>(64, nmea.size() - i));
    }
    CHECK(s.Identified());
    CHECK(s.sentences_ok() > 10);
    // The real unit emits GPTHS — dual-antenna heading is enabled in firmware.
    CHECK(s.has_heading());
    CHECK(s.sentences_bad() == 0);
  }

  SUBCASE("two sentences are the threshold") {
    Um982Sniffer s;
    const char* one = "$GNGGA,,,,,,0,,,,,,,,*78\r\n";
    s.Feed(reinterpret_cast<const std::uint8_t*>(one), std::strlen(one));
    CHECK(s.sentences_ok() == 1);
    CHECK_FALSE(s.Identified());
    s.Feed(reinterpret_cast<const std::uint8_t*>(one), std::strlen(one));
    CHECK(s.Identified());
  }

  SUBCASE("a bad checksum is not a sentence") {
    Um982Sniffer s;
    const char* bad = "$GNGGA,,,,,,0,,,,,,,,*00\r\n$GNGGA,,,,,,0,,,,,,,,*01\r\n";
    s.Feed(reinterpret_cast<const std::uint8_t*>(bad), std::strlen(bad));
    CHECK_FALSE(s.Identified());
    CHECK(s.sentences_bad() == 2);
  }

  SUBCASE("Unicore #-logs identify too") {
    Um982Sniffer s;
    const char* uni =
        "#UNIHEADINGA,68,GPS,FINE,2190,375100.000,0,0,18,10;SOL_COMPUTED*a1b2c3d4\r\n"
        "#VERSIONA,68,GPS,FINE,2190,375100.000,0,0,18,10;UM982,R4.10*deadbeef\r\n";
    s.Feed(reinterpret_cast<const std::uint8_t*>(uni), std::strlen(uni));
    CHECK(s.Identified());
    CHECK(s.has_heading());
  }

  SUBCASE("garbage from the WRONG baud rate identifies nothing") {
    // This is the sweep's safety property: at a mismatched rate the framer
    // sees noise, and two independent valid NMEA checksums do not happen.
    Um982Sniffer s;
    std::vector<std::uint8_t> noise(8192);
    std::uint32_t x = 0xDEADBEEFu;
    for (std::uint8_t& c : noise) {
      x = x * 1664525u + 1013904223u;
      c = static_cast<std::uint8_t>(x >> 24);
    }
    s.Feed(noise.data(), noise.size());
    CHECK_FALSE(s.Identified());
    CHECK_FALSE(s.has_heading());
  }

  SUBCASE("reset clears everything") {
    Um982Sniffer s;
    const char* two = "$GNGGA,,,,,,0,,,,,,,,*78\r\n$GPTHS,,V*0E\r\n";
    s.Feed(reinterpret_cast<const std::uint8_t*>(two), std::strlen(two));
    REQUIRE(s.Identified());
    REQUIRE(s.has_heading());
    s.Reset();
    CHECK_FALSE(s.Identified());
    CHECK_FALSE(s.has_heading());
    CHECK(s.sentences_ok() == 0);
  }
}

TEST_CASE("discovery/serial_enumeration_and_empty_probes") {
  // Enumeration must never throw and never produce a path we would refuse to
  // open. It may legitimately be empty on a CI box with no adapters.
  const std::vector<std::string> ports = EnumerateSerialPorts();
  for (const std::string& p : ports) {
    CHECK_FALSE(p.empty());
#if defined(_WIN32)
    CHECK(p.rfind("COM", 0) == 0);
#else
    CHECK(p.rfind("/dev/", 0) == 0);
#endif
  }

  // No ports offered: both probes answer "not found" immediately, and neither
  // treats that as an error.
  CHECK_FALSE(ProbeSerialD6({}, 50).has_value());
  CHECK_FALSE(ProbeSerialUm982({}, 50).has_value());

  // A path that cannot exist is skipped, not fatal, and must not hang.
  const std::int64_t t0 = millis_now();
  CHECK_FALSE(ProbeSerialD6({"/dev/lidarscan-does-not-exist"}, 200).has_value());
  CHECK_FALSE(ProbeSerialUm982({"/dev/lidarscan-does-not-exist"}, 200).has_value());
  CHECK(millis_now() - t0 < 4000);
}

// ===========================================================================
// The UDP path: bind, receive, parse, dedup, time out
// ===========================================================================

TEST_CASE("discovery/mid360_timeout_and_arguments") {
  CHECK(DiscoverMid360(-1).error() == ScanError::kInvalidArgument);

  DiscoverOptions opt;
  opt.timeout_ms = 250;
  // A port nothing else uses, so this test never fights the real 56201 (or a
  // Livox Viewer on a developer's machine).
  opt.ports = {static_cast<std::uint16_t>(45000 + (CurrentProcessId() % 900))};
  const std::int64_t t0 = millis_now();
  Result<std::vector<Mid360Beacon>> r = DiscoverMid360(opt);
  const std::int64_t dt = millis_now() - t0;
  REQUIRE(r.ok());
  CHECK(r.value().empty());          // nothing is broadcasting on that port
  CHECK(dt >= 200);                  // it really waited
  CHECK(dt < 3000);                  // and really stopped
}

TEST_CASE("discovery/mid360_receives_and_dedups_a_real_heartbeat") {
  const auto frames = beacon_frames();
  REQUIRE(frames.size() == 2);

  const std::uint16_t port = static_cast<std::uint16_t>(46000 + (CurrentProcessId() % 900));

#if defined(_WIN32)
  WSADATA wsa;
  (void)::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  std::atomic<bool> sent{false};
  std::thread sender([&] {
    // Give the listener time to bind. Both frames go out, ~120 ms apart, the
    // way the lidar's 1 Hz beacon would — the dedup must merge them into ONE
    // record, not two.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
#if defined(_WIN32)
    SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) return;
#else
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return;
#endif
    sockaddr_in to;
    std::memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(0x7F000001u);  // 127.0.0.1
    for (const auto& f : frames) {
      (void)::sendto(fd, reinterpret_cast<const char*>(f.data()), static_cast<int>(f.size()), 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to));
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    sent = true;
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
  });

  DiscoverOptions opt;
  opt.timeout_ms = 1500;
  opt.ports = {port};
  Result<std::vector<Mid360Beacon>> r = DiscoverMid360(opt);
  sender.join();

  REQUIRE(r.ok());
  REQUIRE(r.value().size() == 1);  // two heartbeats, ONE lidar
  const Mid360Beacon& b = r.value()[0];
  CHECK(b.sn == "ARMCP7K0034759");
  CHECK(b.lidar_ip == "192.168.1.159");
  CHECK(b.persisted_host_ip == "192.168.1.5");
  CHECK(b.source_ip == "127.0.0.1");
  CHECK(b.push_port_seen == port);  // the port we HEARD it on
  CHECK(b.beacons_seen == 2);
  CHECK(b.t_last_seen_ns > 0);
  CHECK(sent.load());
}

TEST_CASE("discovery/mid360_stop_after_first_device") {
  const auto frames = beacon_frames();
  REQUIRE(!frames.empty());
  const std::uint16_t port = static_cast<std::uint16_t>(47000 + (CurrentProcessId() % 900));

  std::thread sender([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
#if defined(_WIN32)
    SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) return;
#else
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return;
#endif
    sockaddr_in to;
    std::memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(0x7F000001u);
    (void)::sendto(fd, reinterpret_cast<const char*>(frames[0].data()),
                   static_cast<int>(frames[0].size()), 0, reinterpret_cast<sockaddr*>(&to),
                   sizeof(to));
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
  });

  DiscoverOptions opt;
  opt.timeout_ms = 8000;  // long, but stop_after_devices must cut it short
  opt.ports = {port};
  opt.stop_after_devices = 1;
  const std::int64_t t0 = millis_now();
  Result<std::vector<Mid360Beacon>> r = DiscoverMid360(opt);
  const std::int64_t dt = millis_now() - t0;
  sender.join();

  REQUIRE(r.ok());
  CHECK(r.value().size() == 1);
  CHECK(dt < 4000);  // returned on the find, not on the timeout
}

// ===========================================================================
// The single-instance guard
// ===========================================================================

TEST_CASE("instance_guard/two_guards_in_one_process_are_both_ok") {
  const std::string path = temp_lock_path("same-proc");
  remove_file(path);

  InstanceGuardOptions opt;
  opt.lock_path = path;

  InstanceGuard a;
  REQUIRE(a.Acquire(opt).ok());
  CHECK(a.held());
  CHECK_FALSE(a.same_process());
  CHECK(a.holder_pid() == CurrentProcessId());
  CHECK(a.lock_path() == path);

  // The second one JOINS the claim rather than deadlocking against its own
  // process's flock — the case that would otherwise make the test binary, a
  // plugin host or a library initialized twice fail to start.
  InstanceGuard b;
  CHECK(b.Acquire(opt).ok());
  CHECK(b.held());
  CHECK(b.same_process());

  // Idempotent re-acquire.
  CHECK(a.Acquire(opt).ok());

  b.Release();
  CHECK_FALSE(b.held());
  a.Release();
  CHECK_FALSE(a.held());

  // Released: a fresh guard can take it again.
  InstanceGuard c;
  CHECK(c.Acquire(opt).ok());
  CHECK_FALSE(c.same_process());
  c.Release();
  remove_file(path);
}

TEST_CASE("instance_guard/a_foreign_live_holder_is_kBusy") {
  const std::string path = temp_lock_path("foreign");
  remove_file(path);

  // Simulate the other process without spawning one: write its pid into the
  // lockfile and tell the guard that pid is alive. use_os_lock=false because
  // a real flock from THIS process would be indistinguishable from our own.
  const std::int64_t foreign_pid = 424242;
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fprintf(f, "%lld\nlidarscan\n", static_cast<long long>(foreign_pid));
    std::fclose(f);
  }

  InstanceGuardOptions opt;
  opt.lock_path = path;
  opt.use_os_lock = false;
  opt.pid_is_alive = [foreign_pid](std::int64_t pid) { return pid == foreign_pid; };

  InstanceGuard g;
  const Status s = g.Acquire(opt);
  CHECK(s.error() == ScanError::kBusy);
  CHECK_FALSE(g.held());
  CHECK(g.holder_pid() == foreign_pid);

  // The message is the operator-facing one, with the pid in it.
  const std::string msg = last_error_message();
  CHECK(msg.find("another LidarScan is running") != std::string::npos);
  CHECK(msg.find("424242") != std::string::npos);

  remove_file(path);
}

TEST_CASE("instance_guard/a_stale_lockfile_is_taken_over") {
  const std::string path = temp_lock_path("stale");
  remove_file(path);

  // The crash case: a pid recorded by a process that no longer exists. The
  // guard must NOT lock the operator out of their own machine.
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fprintf(f, "999999\nlidarscan\n");
    std::fclose(f);
  }

  InstanceGuardOptions opt;
  opt.lock_path = path;
  opt.use_os_lock = false;
  opt.pid_is_alive = [](std::int64_t) { return false; };

  InstanceGuard g;
  CHECK(g.Acquire(opt).ok());
  CHECK(g.held());
  CHECK(g.holder_pid() == CurrentProcessId());
  g.Release();
  remove_file(path);
}

TEST_CASE("instance_guard/default_path_and_lifecycle") {
  InstanceGuardOptions opt;
  opt.app_id = "lidarscan-unit-test/../..";  // a hostile app_id
  InstanceGuard g;
  const Status s = g.Acquire(opt);
  // Either it claims the default path or the temp dir is not writable; both
  // are legitimate on a locked-down CI box, and neither may crash.
  if (s.ok()) {
    CHECK(g.held());
    // Sanitized: nothing that reads like a traversal survived into the
    // filename — no separators and no "..".
    const std::string p = g.lock_path();
    const std::size_t last_sep = p.find_last_of("/\\");
    const std::string filename = last_sep == std::string::npos ? p : p.substr(last_sep + 1);
    CHECK(filename.rfind("lidarscan-", 0) == 0);
    CHECK(filename.find("..") == std::string::npos);
    CHECK(filename.find('/') == std::string::npos);
    const std::string path_copy = p;
    g.Release();
    CHECK_FALSE(g.held());
    remove_file(path_copy);
  } else {
    CHECK(s.error() == ScanError::kFileError);
  }

  // Release without Acquire, and double Release, are both no-ops.
  InstanceGuard idle;
  idle.Release();
  idle.Release();
  CHECK_FALSE(idle.held());
  CHECK(CurrentProcessId() > 0);
}
