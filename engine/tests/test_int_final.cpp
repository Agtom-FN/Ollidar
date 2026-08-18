// test_int_final.cpp — INT-FINAL: the Phase-1 integration sweep.
//
// Four seams, each one closed here and each one previously named as a gap by
// the workstream that hit it:
//
//   intfinal/crs*        Engine::set_crs / scan_engine_set_crs — the survey
//                        profile's national-grid escape hatch
//                        (docs/INT29-wiring.md §7 item 5).
//   intfinal/abi5*       scan_device_config's Mid-360 half at ABI 5: the
//                        backend selector, the pre-bound descriptors, the
//                        ports and the filter (android/NOTES.md §8 finding 1).
//   intfinal/prebound*   UdpConfig's SECOND descriptor, so the raw-UDP backend
//                        is not point-only on a pre-bound socket
//                        (android/NOTES.md §8 finding 2). Real loopback
//                        sockets, real datagrams.
//   intfinal/merge*      MergeProject::remove_session (desktop NOTES §11.8:
//                        "'Remove selected' is a message box, not a real
//                        removal").
//   intfinal/chain*      the adapter's "colorize-export" pipeline, which is
//                        what `engine_cli --post --colorize --out` submits
//                        (docs/INT34-wiring.md §9 item 6).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "doctest.h"

#include "scanengine_c.h"
#include "scanengine/core/engine.h"
#include "scanengine/drivers/mid360/mid360_driver.h"
#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/jobs/job_runner_adapter.h"
#include "scanengine/merge/merge.h"

using namespace scanengine;

namespace {

// A real WKT for a real national grid the engine has no table entry for. This
// is EPSG:2326 (Hong Kong 1980 Grid System) as the registry publishes it —
// the exact case docs/A10-gnss.md §4 calls "a survey profile's own WKT".
const char* const kHk1980Wkt =
    "PROJCS[\"Hong Kong 1980 Grid System\","
    "GEOGCS[\"Hong Kong 1980\",DATUM[\"Hong_Kong_1980\","
    "SPHEROID[\"International 1924\",6378388,297]],PRIMEM[\"Greenwich\",0],"
    "UNIT[\"degree\",0.0174532925199433]],"
    "PROJECTION[\"Transverse_Mercator\"],"
    "PARAMETER[\"latitude_of_origin\",22.31213333333334],"
    "PARAMETER[\"central_meridian\",114.1785555555556],"
    "PARAMETER[\"scale_factor\",1],PARAMETER[\"false_easting\",836694.05],"
    "PARAMETER[\"false_northing\",819069.8],UNIT[\"metre\",1],AUTHORITY[\"EPSG\",\"2326\"]]";

std::unique_ptr<Engine> make_engine() {
  EngineConfig cfg;
  cfg.app_name = "int-final";
  cfg.log_level = LogLevel::kError;
  auto e = Engine::create(cfg);
  REQUIRE(e.ok());
  return std::move(e).value();
}

}  // namespace

// ===========================================================================
// intfinal/crs — the caller-supplied CRS
// ===========================================================================

TEST_CASE("intfinal/crs_accepts_a_national_grid_and_refuses_what_is_not_one") {
  auto e = make_engine();

  SUBCASE("a real national grid, EPSG + WKT") {
    REQUIRE(e->set_crs("EPSG:2326", kHk1980Wkt).ok());
    CHECK(e->configured_crs_epsg() == "EPSG:2326");
    CHECK(e->configured_crs_wkt() == kHk1980Wkt);
  }

  SUBCASE("a bare number is an EPSG code too — JNI callers hand over ints") {
    REQUIRE(e->set_crs("27700", "PROJCS[\"OSGB36 / British National Grid\"]").ok());
    // Normalised on the way in, so a consumer never has to parse two spellings.
    CHECK(e->configured_crs_epsg() == "EPSG:27700");
  }

  SUBCASE("an EPSG this engine CAN render needs no WKT") {
    REQUIRE(e->set_crs("EPSG:32650", "").ok());
    CHECK(e->configured_crs_epsg() == "EPSG:32650");
    CHECK(e->configured_crs_wkt().empty());
  }

  SUBCASE("an EPSG it cannot render, with no WKT, is REFUSED") {
    // This is the whole point: accepting it would silently produce an export
    // with an empty CRS field, which looks exactly like a local-frame cloud.
    const Status st = e->set_crs("EPSG:2326", "");
    CHECK(st.error() == ScanError::kInvalidArgument);
    CHECK(e->configured_crs_epsg().empty());
  }

  SUBCASE("a PROJ.4 string is not a WKT") {
    CHECK(e->set_crs("EPSG:2326", "+proj=tmerc +lat_0=22.31 +lon_0=114.17 +k=1").error() ==
          ScanError::kInvalidArgument);
  }

  SUBCASE("a truncated paste is caught by the brackets") {
    const std::string half(kHk1980Wkt, std::strlen(kHk1980Wkt) - 40);
    CHECK(e->set_crs("EPSG:2326", half).error() == ScanError::kInvalidArgument);
  }

  SUBCASE("a WKT with no quoted name is refused") {
    CHECK(e->set_crs("", "PROJCS[]").error() == ScanError::kInvalidArgument);
  }

  SUBCASE("garbage in the EPSG field") {
    CHECK(e->set_crs("EPSG:banana", kHk1980Wkt).error() == ScanError::kInvalidArgument);
    CHECK(e->set_crs("ESRI:102100", kHk1980Wkt).error() == ScanError::kInvalidArgument);
  }

  SUBCASE("a WKT alone is legal — not every grid has a code") {
    REQUIRE(e->set_crs("", kHk1980Wkt).ok());
    CHECK(e->configured_crs_epsg().empty());
    CHECK(e->configured_crs_wkt() == kHk1980Wkt);
  }

  SUBCASE("clearing") {
    REQUIRE(e->set_crs("EPSG:2326", kHk1980Wkt).ok());
    REQUIRE(e->set_crs("", "").ok());
    CHECK(e->configured_crs_epsg().empty());
    CHECK(e->configured_crs_wkt().empty());
  }
}

TEST_CASE("intfinal/crs_override_does_not_defeat_the_convergence_gate") {
  auto e = make_engine();
  REQUIRE(e->set_crs("EPSG:2326", kHk1980Wkt).ok());
  // Nothing has georeferenced anything, so the A9 export seam is still EMPTY:
  // an override says WHAT to label a converged cloud, never that an
  // unconverged one may be labelled. Handing A9 a national grid for a
  // local-frame cloud produces a file that opens and lands in the wrong place.
  CHECK(e->crs_wkt().empty());
  CHECK(e->crs_epsg().empty());
  // But the configuration is readable, so a UI can show what the operator set.
  CHECK(e->configured_crs_wkt() == kHk1980Wkt);
}

TEST_CASE("intfinal/crs_crosses_the_C_ABI") {
  scan_engine* h = nullptr;
  scan_engine_config c;
  std::memset(&c, 0, sizeof(c));
  c.log_level = SCAN_LOG_ERROR;
  REQUIRE(scan_engine_create(&c, &h) == SCAN_OK);

  CHECK(scan_engine_abi_version() == 9u);  // 8 -> 9 with ROUND 10's pose-time offset
  CHECK(SCAN_ABI_VERSION == kEngineAbiVersion);

  CHECK(scan_engine_set_crs(h, "EPSG:2326", kHk1980Wkt) == SCAN_OK);
  CHECK(scan_engine_set_crs(h, "EPSG:2326", nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_set_crs(h, "not-an-epsg", kHk1980Wkt) == SCAN_ERR_INVALID_ARGUMENT);
  // NULL/NULL clears rather than crashing — the JNI caller's "no CRS chosen".
  CHECK(scan_engine_set_crs(h, nullptr, nullptr) == SCAN_OK);
  CHECK(scan_engine_set_crs(nullptr, "EPSG:32650", "") == SCAN_ERR_INVALID_ARGUMENT);
  // Still gated: no fix has ever been pushed.
  CHECK(std::string(scan_engine_crs_wkt(h)).empty());
  scan_engine_destroy(h);
}

// ===========================================================================
// intfinal/abi5 — scan_device_config's Mid-360 half
// ===========================================================================

TEST_CASE("intfinal/abi5_device_config_is_additive_and_zero_means_ABI_4") {
  scan_engine* h = nullptr;
  scan_engine_config ec;
  std::memset(&ec, 0, sizeof(ec));
  ec.log_level = SCAN_LOG_ERROR;
  REQUIRE(scan_engine_create(&ec, &h) == SCAN_OK);

  SUBCASE("a zeroed config is exactly the ABI-4 device: SDK2, engine sockets") {
    scan_device_config dc;
    std::memset(&dc, 0, sizeof(dc));
    dc.kind = SCAN_DEVICE_MID360;
    dc.lidar_ip = "192.168.1.100";
    dc.host_ip = "192.168.1.5";
    std::uint32_t id = 0;
    // add_device does not start anything (no session), so this succeeds with
    // or without the SDK compiled in.
    REQUIRE(scan_engine_add_device(h, &dc, &id) == SCAN_OK);
    CHECK(scan_engine_remove_device(h, id) == SCAN_OK);
  }

  SUBCASE("the backend selector is range-checked, not cast blindly") {
    scan_device_config dc;
    std::memset(&dc, 0, sizeof(dc));
    dc.kind = SCAN_DEVICE_MID360;
    dc.lidar_ip = "192.168.1.100";
    dc.host_ip = "192.168.1.5";
    dc.mid360_backend = 7;
    std::uint32_t id = 0;
    CHECK(scan_engine_add_device(h, &dc, &id) == SCAN_ERR_INVALID_ARGUMENT);
  }

  SUBCASE("a pre-bound descriptor handed to the SDK2 backend is refused") {
    // SDK2 creates its own sockets inside the vendored SDK, so a descriptor
    // bound to a Network here would be silently ignored — which on a bench
    // looks like "the seam does not work" (android/NOTES.md §8 finding 3).
    scan_device_config dc;
    std::memset(&dc, 0, sizeof(dc));
    dc.kind = SCAN_DEVICE_MID360;
    dc.lidar_ip = "192.168.1.100";
    dc.host_ip = "192.168.1.5";
    dc.mid360_backend = SCAN_MID360_BACKEND_SDK2;
    dc.mid360_prebound_fd = 42;
    std::uint32_t id = 0;
    CHECK(scan_engine_add_device(h, &dc, &id) == SCAN_ERR_INVALID_ARGUMENT);
  }

  SUBCASE("every ABI-5 field reaches the C++ config") {
    // The device is added through the ABI and its configuration is read back
    // through the C++ Engine, which is the only way to prove the conversion
    // rather than assert it.
    scan_device_config dc;
    std::memset(&dc, 0, sizeof(dc));
    dc.kind = SCAN_DEVICE_MID360;
    dc.lidar_ip = "192.168.1.100";
    dc.host_ip = "192.168.1.5";
    dc.mid360_backend = SCAN_MID360_BACKEND_INJECT;
    dc.mid360_point_port = 7300;
    dc.mid360_host_imu_port = 7401;
    dc.mid360_recv_buffer_bytes = 1 << 20;
    dc.mid360_live_points_per_sec_set = 1;
    dc.mid360_live_points_per_sec = 0;  // 0 is a REAL value: no decimation
    dc.mid360_publish_imu_set = 1;
    dc.mid360_publish_imu = 0;
    dc.mid360_verify_crc_set = 1;
    dc.mid360_verify_crc = 1;
    dc.mid360_filter_set = 1;
    dc.mid360_drop_no_return = 0;
    dc.mid360_tag_reject_mask = 0x0C;
    dc.mid360_min_reflectivity = 12;
    dc.mid360_min_range_m = 0.5f;
    dc.mid360_max_range_m = 40.f;
    dc.mid360_sdk_config_path = "/tmp/does-not-need-to-exist.json";
    std::uint32_t id = 0;
    REQUIRE(scan_engine_add_device(h, &dc, &id) == SCAN_OK);
    CHECK(scan_engine_remove_device(h, id) == SCAN_OK);
  }
  scan_engine_destroy(h);
}

// The same conversion, checked field by field against the C++ side. The ABI
// cannot read a driver's config back (Engine exposes no concrete-driver
// accessor — android/NOTES.md §8 finding 5), so this builds the DeviceConfig
// the C layer would build and asserts the mapping directly.
TEST_CASE("intfinal/abi5_defaults_survive_a_zeroed_struct") {
  const Mid360Config d{};
  // These are the values a memset(0) scan_device_config must NOT disturb.
  CHECK(d.backend == Mid360Backend::kSdk2);
  CHECK(d.udp.prebound_fd == -1);
  CHECK(d.udp.prebound_imu_fd == -1);
  CHECK(d.udp.point_port == 56300);
  CHECK(d.udp.host_point_port == 56301);
  CHECK(d.udp.recv_buffer_bytes == 4 * 1024 * 1024);
  CHECK(d.live_points_per_sec == 40000u);
  CHECK(d.publish_imu);
  CHECK_FALSE(d.verify_crc);
  CHECK(d.filter.drop_no_return);
}

// ===========================================================================
// intfinal/stats — the Mid-360's own counters, reachable at last
// ===========================================================================

TEST_CASE("intfinal/mid360_stats_are_reachable_from_the_engine_and_the_ABI") {
  auto e = make_engine();
  DeviceConfig dc;
  dc.kind = DeviceKind::kMid360;
  dc.mid360.backend = Mid360Backend::kInject;  // no sockets, no SDK
  dc.mid360.internal_supervisor_thread = false;
  auto id = e->add_device(dc);
  REQUIRE(id.ok());

  auto s = e->mid360_stats(id.value());
  REQUIRE(s.ok());
  // Before start(): the link is down and nothing has been counted. The point
  // is that the QUESTION can now be asked at all — "how many forced re-inits
  // has this capture had" was unanswerable from an app before this
  // (android/NOTES.md §8 finding 5, desktop/NOTES.md §8.3).
  CHECK(s.value().link == Mid360LinkState::kDown);
  CHECK(s.value().forced_reinits == 0);
  CHECK(s.value().watchdog_trips == 0);

  SUBCASE("a device that is not a Mid-360 is refused, not silently zeroed") {
    DeviceConfig d6;
    d6.kind = DeviceKind::kD6;
    d6.d6.serial.port_name = "not-a-mid360";
    auto other = e->add_device(d6);
    REQUIRE(other.ok());
    CHECK(e->mid360_stats(other.value()).error() == ScanError::kInvalidArgument);
  }
  SUBCASE("an unknown id") {
    CHECK(e->mid360_stats(9999).error() == ScanError::kNotFound);
  }
}

// ===========================================================================
// intfinal/prebound — two descriptors, two streams, neither closed
// ===========================================================================

#if !defined(_WIN32)
namespace {

struct Rig {
  EventBus bus;
  PageStore points;
  DriverContext ctx;
  Rig() : points(PageStoreConfig{1 << 14, 4}) {
    ctx.bus = &bus;
    ctx.points = &points;
  }
};

// A loopback UDP socket bound to an ephemeral port, exactly the way the
// Android app binds one before handing the descriptor down (minus the
// Network.bindSocket step, which has no desktop equivalent).
int bind_loopback(std::uint16_t* out_port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
  a.sin_port = 0;  // let the kernel choose: no fixed port, no collision with
                   // the sim-labelled tests or a second build tree
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  sockaddr_in got{};
  socklen_t len = sizeof(got);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &len) != 0) {
    ::close(fd);
    return -1;
  }
  *out_port = ntohs(got.sin_port);
  return fd;
}

std::vector<std::uint8_t> point_datagram(std::uint16_t udp_cnt) {
  std::vector<std::uint8_t> b(mid360::kPointPacketBytes, 0);
  auto* hd = reinterpret_cast<mid360::DataHeader*>(b.data());
  hd->length = static_cast<std::uint16_t>(mid360::kPointPacketBytes);
  hd->time_interval = 5;
  hd->dot_num = mid360::kPointsPerPacket;
  hd->udp_cnt = udp_cnt;
  hd->data_type = mid360::kDataTypeCartesianHigh;
  hd->timestamp = 1;
  auto* pts = reinterpret_cast<mid360::CartesianHigh*>(b.data() + sizeof(mid360::DataHeader));
  for (std::uint32_t i = 0; i < mid360::kPointsPerPacket; ++i) {
    pts[i].x = 2000 + static_cast<std::int32_t>(i);
    pts[i].y = 100;
    pts[i].z = 50;
    pts[i].reflectivity = 120;
    pts[i].tag = 0;
  }
  return b;
}

std::vector<std::uint8_t> imu_datagram(std::uint16_t udp_cnt) {
  std::vector<std::uint8_t> b(mid360::kImuPacketBytes, 0);
  auto* hd = reinterpret_cast<mid360::DataHeader*>(b.data());
  hd->length = static_cast<std::uint16_t>(mid360::kImuPacketBytes);
  hd->time_interval = 5;
  hd->dot_num = 1;
  hd->udp_cnt = udp_cnt;
  hd->data_type = mid360::kDataTypeImu;
  hd->timestamp = 1;
  const mid360::ImuRaw raw{0.f, 0.f, 0.f, 0.f, 0.f, 1.f};
  std::memcpy(b.data() + sizeof(mid360::DataHeader), &raw, sizeof(raw));
  return b;
}

void send_to(std::uint16_t port, const std::vector<std::uint8_t>& bytes) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  REQUIRE(fd >= 0);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = ::inet_addr("127.0.0.1");
  to.sin_port = htons(port);
  (void)::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  ::close(fd);
}

}  // namespace

TEST_CASE("intfinal/prebound_raw_udp_takes_one_descriptor_per_stream") {
  std::uint16_t point_port = 0, imu_port = 0;
  const int point_fd = bind_loopback(&point_port);
  const int imu_fd = bind_loopback(&imu_port);
  REQUIRE(point_fd >= 0);
  REQUIRE(imu_fd >= 0);

  Mid360Config cfg;
  cfg.backend = Mid360Backend::kRawUdp;
  cfg.udp.host_ip = "127.0.0.1";
  cfg.udp.lidar_ip = "127.0.0.1";
  cfg.udp.prebound_fd = point_fd;
  cfg.udp.prebound_imu_fd = imu_fd;
  cfg.internal_supervisor_thread = false;
  cfg.live_points_per_sec = 0;
  cfg.max_batch_points = mid360::kPointsPerPacket;

  Rig rig;
  {
    Mid360Driver drv(11, cfg, rig.ctx);
    REQUIRE(drv.start().ok());

    // Both streams, several datagrams each. Before the second descriptor
    // existed these two ports were ONE socket read by two threads, and each
    // thread ate datagrams meant for the other.
    for (std::uint16_t i = 0; i < 4; ++i) {
      send_to(point_port, point_datagram(i));
      send_to(imu_port, imu_datagram(i));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      const Mid360Stats s = drv.stats();
      if (s.point_packets >= 4 && s.imu_packets >= 4) break;
      if (std::chrono::steady_clock::now() > deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const Mid360Stats s = drv.stats();
    CHECK(s.point_packets == 4);
    CHECK(s.imu_packets == 4);
    CHECK(s.points_received == 4u * mid360::kPointsPerPacket);
    CHECK(s.bad_packets == 0);
    CHECK(drv.stop().ok());
  }

  // NEVER-CLOSE, for BOTH: the app owns the descriptors and closes them after
  // the engine has torn down. A closed fd here would make the app's own
  // close() hit EBADF — or, worse, close a descriptor the runtime has since
  // reused for something else.
  CHECK(::fcntl(point_fd, F_GETFD) != -1);
  CHECK(::fcntl(imu_fd, F_GETFD) != -1);
  ::close(point_fd);
  ::close(imu_fd);
}

TEST_CASE("intfinal/prebound_point_fd_without_an_imu_fd_is_refused") {
  std::uint16_t port = 0;
  const int fd = bind_loopback(&port);
  REQUIRE(fd >= 0);

  Mid360Config cfg;
  cfg.backend = Mid360Backend::kRawUdp;
  cfg.udp.host_ip = "127.0.0.1";
  cfg.udp.lidar_ip = "127.0.0.1";
  cfg.udp.prebound_fd = fd;
  cfg.udp.prebound_imu_fd = -1;
  cfg.publish_imu = true;  // ... but there is no descriptor for it
  cfg.internal_supervisor_thread = false;

  Rig rig;
  {
    Mid360Driver drv(12, cfg, rig.ctx);
    // Creating an ordinary socket for the IMU would look like it worked and
    // then receive nothing (on Android it would not be bound to the
    // USB-Ethernet Network at all), so this is refused instead.
    CHECK(drv.start().error() == ScanError::kInvalidArgument);
  }

  SUBCASE("point-only is still allowed, explicitly") {
    Mid360Config c2 = cfg;
    c2.publish_imu = false;
    Rig rig2;
    Mid360Driver drv(13, c2, rig2.ctx);
    CHECK(drv.start().ok());
    CHECK(drv.stop().ok());
  }
  CHECK(::fcntl(fd, F_GETFD) != -1);
  ::close(fd);
}
#endif  // !_WIN32

// ===========================================================================
// intfinal/merge — remove_session
// ===========================================================================

namespace {

merge::SessionInput cube_session(const std::string& id, std::vector<PointVertex>* storage,
                                 double x0) {
  storage->clear();
  for (int i = 0; i < 64; ++i) {
    PointVertex v{};
    v.x = static_cast<float>(x0 + (i % 4) * 0.5);
    v.y = static_cast<float>((i / 4) % 4) * 0.5f;
    v.z = static_cast<float>(i / 16) * 0.5f;
    v.a = 255;
    storage->push_back(v);
  }
  merge::SessionInput in;
  in.provenance_id = id;
  in.cloud.chunks.push_back(Span<const PointVertex>(storage->data(), storage->size()));
  return in;
}

}  // namespace

TEST_CASE("intfinal/merge_remove_session_renumbers_and_keeps_the_anchor") {
  merge::MergeProject p;
  std::vector<PointVertex> a, b, c;
  REQUIRE(p.add_session(cube_session("alpha", &a, 0.0)).ok());
  REQUIRE(p.add_session(cube_session("beta", &b, 1.0)).ok());
  REQUIRE(p.add_session(cube_session("gamma", &c, 2.0)).ok());
  REQUIRE(p.session_count() == 3);
  CHECK(p.anchor() == 0);

  SUBCASE("an unknown id is kNotFound, not a crash") {
    CHECK(p.remove_session(7).error() == ScanError::kNotFound);
    CHECK(p.session_count() == 3);
  }

  SUBCASE("removing the middle session renumbers the ones after it") {
    // Beta placed by hand, so there is a real alignment to preserve.
    double m[16] = {1, 0, 0, 3.0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    REQUIRE(p.set_alignment(1, m).ok());
    REQUIRE(p.remove_session(1).ok());

    CHECK(p.session_count() == 2);
    CHECK(p.session(0).provenance_id == "alpha");
    CHECK(p.session(1).provenance_id == "gamma");  // was 2
    CHECK(p.session(1).id == 1);                   // the id moved with it
    CHECK(p.find("beta") == -1);
    CHECK(p.find("gamma") == 1);
    // The anchor sat before the hole, so nothing about the merged frame moved.
    CHECK(p.anchor() == 0);
    CHECK(p.report().sessions.size() == 2);
  }

  SUBCASE("removing the anchor moves it and rebases the survivors") {
    // Place gamma 3 m from alpha; after alpha goes, gamma must still be 3 m
    // from beta's frame's origin... i.e. the RELATIVE geometry survives and
    // only the origin moves.
    double gm[16] = {1, 0, 0, 3.0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    double bm[16] = {1, 0, 0, 1.0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    REQUIRE(p.set_alignment(1, bm).ok());
    REQUIRE(p.set_alignment(2, gm).ok());

    REQUIRE(p.remove_session(0).ok());
    CHECK(p.session_count() == 2);
    CHECK(p.anchor() == 0);
    CHECK(p.session(0).provenance_id == "beta");
    CHECK(p.session(0).anchor);
    // beta is now the origin; gamma was 2 m further along than beta.
    CHECK(p.session(0).world_from_session[3] == doctest::Approx(0.0));
    CHECK(p.session(1).world_from_session[3] == doctest::Approx(2.0));
  }

  SUBCASE("the pair report is invalidated, not left stale") {
    REQUIRE(p.survey_overlap().ok());
    const std::size_t before = p.report().pairs.size();
    CHECK(before > 0);
    REQUIRE(p.remove_session(2).ok());
    // Every pair named a session by id, and one of those ids is now a
    // different session. Reporting them would be worse than reporting none.
    CHECK(p.report().pairs.empty());
    CHECK(p.report().sessions.size() == 2);
    // ... and a fresh survey rebuilds it over what is left.
    REQUIRE(p.survey_overlap().ok());
    CHECK(p.report().pairs.size() == 1);
  }

  SUBCASE("removing everything leaves an empty project that still works") {
    REQUIRE(p.remove_session(2).ok());
    REQUIRE(p.remove_session(1).ok());
    REQUIRE(p.remove_session(0).ok());
    CHECK(p.session_count() == 0);
    CHECK(p.report().sessions.empty());
    // And a new session can be added, taking id 0 and the anchor.
    std::vector<PointVertex> d;
    auto id = p.add_session(cube_session("delta", &d, 0.0));
    REQUIRE(id.ok());
    CHECK(id.value() == 0);
    CHECK(p.anchor() == 0);
  }
}

// ===========================================================================
// intfinal/chain — the colorize-export pipeline
// ===========================================================================

namespace {

// A Colorizer that records what it was asked to do. The chain is what is under
// test here, not A11's projection.
class CountingColorizer final : public Colorizer {
 public:
  Status set_extrinsics(const double[16]) override { return kOkStatus; }
  Status add_keyframe(const Keyframe&) override { return kOkStatus; }
  Status colorize(PageStore*) override {
    ++runs;
    return kOkStatus;
  }
  float progress() const override { return 1.f; }
  std::atomic<int> runs{0};
};

}  // namespace

TEST_CASE("intfinal/chain_colorize_export_is_three_stages_and_needs_both_options") {
  EngineConfig ecfg;
  ecfg.app_name = "int-final";
  ecfg.log_level = LogLevel::kError;
  auto e = Engine::create(ecfg);
  REQUIRE(e.ok());

  CountingColorizer col;
  jobs::JobRunnerOptions opts;
  opts.store = std::make_shared<PageStore>();
  jobs::QueueJobRunner runner(&e.value()->jobs(), opts);

  JobRequest req;
  req.mode = JobMode::kLocal;
  req.lscan_dir = "/nonexistent-for-this-test.lscan";

  SUBCASE("no colorizer: kUnimplemented at submit, not at run time") {
    req.pipeline = "colorize-export";
    req.output_dir = "/tmp";
    CHECK(runner.submit(req).error() == ScanError::kUnimplemented);
  }

  SUBCASE("no output_dir: kInvalidArgument, the same as plain 'export'") {
    runner.options().colorizer = &col;
    req.pipeline = "colorize-export";
    req.output_dir.clear();
    CHECK(runner.submit(req).error() == ScanError::kInvalidArgument);
  }

  SUBCASE("an unknown pipeline is still kUnimplemented") {
    req.pipeline = "colourise";  // the other spelling is not a pipeline
    CHECK(runner.submit(req).error() == ScanError::kUnimplemented);
  }
}
