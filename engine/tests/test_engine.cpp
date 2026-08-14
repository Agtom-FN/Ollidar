// Engine lifecycle + the D6 driver end to end: synthetic S1 capture bytes in,
// decoded points out through the PageStore and the EventBus.
//
// Since INT-24 this file also covers the two integrations the Engine owns
// rather than a module: A8's pushbroom (poses in over the C++ API, world
// points out on StreamId::kSlamMap) and A6's live SLAM (Mid-360 datagrams in
// through the kInject backend, a registered map out on the same stream).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"
#include "scanengine/core/engine.h"
#include "scanengine/drivers/mid360/mid360_packets.h"

using namespace scanengine;

namespace {

// One revolution as the S1 packet builder emits it: a start packet plus
// `packets` × `per_packet` samples spread over 360°, checksums computed by
// replaying the vendor SDK's state machine (NOT by calling the engine's own
// checksum function — the two implementations cross-check each other).
std::vector<std::uint8_t> synthetic_revolution(int packets = 10, int per_packet = 40) {
  return d6test::build_revolution(packets, per_packet, /*distance_mm=*/1000,
                                  /*intensity=*/128, /*scan_freq=*/10);
}

EngineConfig small_engine_config() {
  EngineConfig cfg;
  cfg.app_name = "engine-tests";
  cfg.log_level = LogLevel::kOff;
  cfg.points.page_capacity = 4096;
  cfg.points.max_pages = 16;
  return cfg;
}

DeviceConfig d6_config(SerialWriteFn write_fn = nullptr, void* user = nullptr) {
  DeviceConfig dc;
  dc.kind = DeviceKind::kD6;
  dc.d6.serial.port_name = "test";
  dc.d6.serial.write_fn = write_fn;
  dc.d6.serial.write_user_data = user;
  dc.d6.send_start_stop_commands = write_fn != nullptr;
  return dc;
}

// --- Mid-360 synthetic datagrams -----------------------------------------
//
// Same shape as tests/test_mid360_driver.cpp's builders (36-byte header + N
// Cartesian-high points / one ImuRaw). Duplicated rather than shared because
// these are pushed through Engine::push_serial_bytes into a kInject driver,
// which is the engine-level path A5's .lscan replay will also take.
//
// The device timestamp is set EQUAL to the arrival stamp on purpose: A4's
// min-delay estimator then maps device→engine as the identity, so the engine
// times the LIO sees are the times this test chose. Both streams go through
// the one kLidarMid360 estimator, which is the point of the wiring.
std::vector<std::uint8_t> mid360_point_packet(std::uint16_t udp_cnt, std::int64_t t_ns) {
  const std::uint16_t dots = mid360::kPointsPerPacket;
  std::vector<std::uint8_t> b(sizeof(mid360::DataHeader) + dots * sizeof(mid360::CartesianHigh), 0);
  auto* h = reinterpret_cast<mid360::DataHeader*>(b.data());
  h->version = 0;
  h->length = static_cast<std::uint16_t>(b.size());
  h->time_interval = 4750;
  h->dot_num = dots;
  h->udp_cnt = udp_cnt;
  h->data_type = mid360::kDataTypeCartesianHigh;
  h->timestamp = static_cast<std::uint64_t>(t_ns);
  auto* pts = reinterpret_cast<mid360::CartesianHigh*>(b.data() + sizeof(mid360::DataHeader));
  // A crude box around the sensor: enough distinct planes for the odometry to
  // have something to register against, and every point well inside the range
  // gate. Deterministic, so the map is reproducible.
  for (std::uint16_t i = 0; i < dots; ++i) {
    const int face = i % 4;
    const std::int32_t along = 20 * (static_cast<std::int32_t>(i) - dots / 2);
    pts[i].x = face == 0 ? 4000 : (face == 1 ? -4000 : along);
    pts[i].y = face == 2 ? 3000 : (face == 3 ? -3000 : along);
    pts[i].z = 500 + 10 * (static_cast<std::int32_t>(i) % 7);
    pts[i].reflectivity = 100;
    pts[i].tag = 0;
  }
  return b;
}

std::vector<std::uint8_t> mid360_imu_packet(std::uint16_t udp_cnt, std::int64_t t_ns) {
  std::vector<std::uint8_t> b(sizeof(mid360::DataHeader) + sizeof(mid360::ImuRaw), 0);
  auto* h = reinterpret_cast<mid360::DataHeader*>(b.data());
  h->version = 0;
  h->length = static_cast<std::uint16_t>(b.size());
  h->dot_num = 1;
  h->udp_cnt = udp_cnt;
  h->data_type = mid360::kDataTypeImu;
  h->timestamp = static_cast<std::uint64_t>(t_ns);
  auto* s = reinterpret_cast<mid360::ImuRaw*>(b.data() + sizeof(mid360::DataHeader));
  // At rest, +1 g on Z, as the device reports it (in g — A4 converts).
  s->acc_z = 1.0f;
  return b;
}

// --- synthetic NMEA (A10) --------------------------------------------------
//
// A receiver's per-epoch burst, built here rather than shared with
// tests/test_gnss.cpp on purpose: this file must be able to state exactly what
// bytes the ENGINE was handed, and the two implementations cross-check each
// other the same way the D6 packet builder cross-checks the D6 parser.
// Checksums go through nmea::checksum_of, which test_gnss.cpp independently
// validates against the S5 spike's Python implementation.
std::string nmea_line(const std::string& body) {
  char cs[8];
  std::snprintf(cs, sizeof(cs), "*%02X", nmea::checksum_of(body));
  return "$" + body + cs + "\r\n";
}

std::string nmea_dm(double deg, int int_width) {
  const double a = std::fabs(deg);
  char b[32];
  std::snprintf(b, sizeof(b), "%0*d%09.6f", int_width, static_cast<int>(a),
                (a - static_cast<int>(a)) * 60.0);
  return b;
}

// GGA + RMC + GST, one epoch, all sharing the same UTC — which is what makes
// the epoch close and what puts the GST sigmas on the SAME fix as the GGA
// position (docs/A10-gnss.md §7).
std::string nmea_epoch(int sod, double lat, double lon, double alt_msl_m, int quality = 4,
                       double sigma_h = 0.02, double course_deg = 90.0) {
  const int hh = sod / 3600, mm = (sod / 60) % 60, ss = sod % 60;
  char t[16];
  std::snprintf(t, sizeof(t), "%02d%02d%02d.00", hh, mm, ss);
  char buf[256];
  std::string out;

  std::snprintf(buf, sizeof(buf), "GNGGA,%s,%s,%c,%s,%c,%d,22,0.6,%.2f,M,-2.0,M,,", t,
                nmea_dm(lat, 2).c_str(), lat < 0 ? 'S' : 'N', nmea_dm(lon, 3).c_str(),
                lon < 0 ? 'W' : 'E', quality, alt_msl_m);
  out += nmea_line(buf);

  std::snprintf(buf, sizeof(buf), "GNRMC,%s,A,%s,%c,%s,%c,2.33,%.1f,010126,,,R", t,
                nmea_dm(lat, 2).c_str(), lat < 0 ? 'S' : 'N', nmea_dm(lon, 3).c_str(),
                lon < 0 ? 'W' : 'E', course_deg);
  out += nmea_line(buf);

  std::snprintf(buf, sizeof(buf), "GNGST,%s,%.3f,%.3f,%.3f,0.0,%.3f,%.3f,%.3f", t, sigma_h,
                sigma_h, sigma_h * 0.8, sigma_h, sigma_h, sigma_h * 1.5);
  out += nmea_line(buf);
  return out;
}

Status push_nmea(Engine& e, DeviceId id, const std::string& s, std::int64_t t_ns) {
  return e.push_serial_bytes(id, ByteSpan(reinterpret_cast<const std::uint8_t*>(s.data()),
                                          s.size()),
                             TimePoint{t_ns});
}

// The S5 simulator's 40 x 25 m walking loop, as a distance along its perimeter.
void loop_point(double dist_m, double* x, double* y) {
  const double per = 2.0 * (40.0 + 25.0);
  double d = std::fmod(dist_m, per);
  if (d < 40.0) { *x = d; *y = 0.0; }
  else if (d < 65.0) { *x = 40.0; *y = d - 40.0; }
  else if (d < 105.0) { *x = 40.0 - (d - 65.0); *y = 25.0; }
  else { *x = 0.0; *y = 25.0 - (d - 105.0); }
}

// Identity pose, ready to be stamped and placed.
Pose ar_pose(std::int64_t t_ns, double x, double y, double z) {
  Pose p;
  p.t_mono_ns = t_ns;
  p.position[0] = x;
  p.position[1] = y;
  p.position[2] = z;
  p.orientation[3] = 1.0;
  p.source = StreamId::kPoseAr;
  p.quality = PoseQuality::kGood;
  return p;
}

}  // namespace

TEST_CASE("engine/create_starts_idle_and_reports_a_version") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  CHECK(e.state() == EngineState::kIdle);
  CHECK_FALSE(e.session_active());
  CHECK(std::string(engine_version_string()).find("scanengine") == 0);
  // Bumped with SCAN_ABI_VERSION, never on its own (DESIGN §6 item 9).
  // 3: INT-29's GNSS/RTK surface (NMEA in, fix/NTRIP/georef out).
  CHECK(kEngineAbiVersion == 3);
}

TEST_CASE("engine/session_transitions_are_enforced_and_announced") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  SessionConfig sc;
  sc.record = false;
  CHECK(e.start_session(sc).ok());
  CHECK(e.state() == EngineState::kRunning);
  CHECK(e.session_id() == 1);
  CHECK(e.start_session(sc).error() == ScanError::kInvalidState);

  // The state machine was broadcast: starting, running, then the session event.
  std::vector<EventType> types;
  Event ev;
  while (e.events().poll(e.app_subscription(), &ev)) types.push_back(ev.type);
  REQUIRE(types.size() >= 3);
  CHECK(types[0] == EventType::kEngineState);
  CHECK(types[1] == EventType::kEngineState);
  CHECK(types[2] == EventType::kSessionState);

  CHECK(e.stop_session().ok());
  CHECK(e.state() == EngineState::kIdle);
  CHECK(e.stop_session().ok());  // idempotent
}

TEST_CASE("engine/unknown_device_kinds_and_ids_are_rejected") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  DeviceConfig bad;
  CHECK(e.add_device(bad).error() == ScanError::kInvalidArgument);

  // INT-29: kRtkRover is real now (it used to be kUnimplemented), and it is
  // routed to the Engine's one GnssSource with no per-device config at all.
  DeviceConfig rtk;
  rtk.kind = DeviceKind::kRtkRover;
  auto rtk_id = e.add_device(rtk);
  REQUIRE(rtk_id.ok());
  auto rh = e.device_health(rtk_id.value());
  REQUIRE(rh.ok());
  CHECK(rh.value().kind == DeviceKind::kRtkRover);

  const std::uint8_t byte = 0;
  CHECK(e.push_serial_bytes(999, ByteSpan(&byte, 1)).error() == ScanError::kNotFound);
  CHECK(e.device_health(999).error() == ScanError::kNotFound);
  CHECK(e.remove_device(999).error() == ScanError::kNotFound);
}

TEST_CASE("engine/mid360_device_exists_but_start_is_task_A3") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  DeviceConfig dc;
  dc.kind = DeviceKind::kMid360;
  dc.mid360.udp.lidar_ip = "192.168.1.100";
  auto id = e.add_device(dc);
  REQUIRE(id.ok());

  auto h = e.device_health(id.value());
  REQUIRE(h.ok());
  CHECK(h.value().kind == DeviceKind::kMid360);
  // A3 landed: the driver is real, so a freshly added device is healthy-idle
  // rather than kUnimplemented. It still cannot START here, because no
  // host_ip is configured — the device is TOLD where to stream and there is
  // no broadcast discovery on macOS (S2 REPORT.md §3), so an explicit host
  // and lidar IP are mandatory.
  CHECK(h.value().last_error == ScanError::kOk);

  // A failing device does not abort the session.
  SessionConfig sc;
  sc.record = false;
  CHECK(e.start_session(sc).ok());
  CHECK(e.state() == EngineState::kRunning);
}

TEST_CASE("engine/d6_end_to_end_synthetic_capture") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  auto id = e.add_device(d6_config());
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.record = false;
  REQUIRE(e.start_session(sc).ok());

  // Two revolutions: the second start packet is what closes the first
  // rotation, so a single revolution would never raise a kRotation event.
  auto bytes = synthetic_revolution();
  const auto second = synthetic_revolution();
  bytes.insert(bytes.end(), second.begin(), second.end());

  // Push in awkward chunks: the parser must reassemble packets torn across
  // arbitrary transport boundaries (S1 proved it; this proves the engine
  // path preserves that).
  std::size_t off = 0;
  const std::size_t chunks[] = {7, 13, 100, 3, 512};
  int k = 0;
  while (off < bytes.size()) {
    const std::size_t n = std::min(chunks[k++ % 5], bytes.size() - off);
    REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(bytes.data() + off, n)).ok());
    off += n;
  }

  auto h = e.device_health(id.value());
  REQUIRE(h.ok());
  const DeviceHealth& health = h.value();
  CHECK(health.state == DeviceState::kStreaming);
  CHECK(health.packets_ok == 22);  // 2 x (1 start packet + 10 point packets)
  CHECK(health.packets_bad == 0);
  CHECK(health.checksum_pass_rate == doctest::Approx(1.0));
  CHECK(health.bytes_in == bytes.size());
  CHECK(health.points_out == 802);  // 2 x (400 samples + the start packet's sample)
  CHECK(health.drops == 0);

  CHECK(e.points().total_points() == 802);
  const auto ids = e.points().page_ids();
  REQUIRE(ids.size() == 1);
  const PageView pv = e.points().page_view(ids[0]);
  CHECK(pv.stream == StreamId::kLidarD6);
  CHECK(pv.count == 802);
  // 1000 mm at 0° → (0, 1, 0) m in the sensor frame.
  CHECK(pv.data[0].x == doctest::Approx(0.0f).epsilon(0.001));
  CHECK(pv.data[0].y == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(pv.data[0].z == 0.0f);
  CHECK(pv.data[0].a == 255);
  // Everything is 1 m from the sensor, so the page box is the unit circle.
  CHECK(pv.bounds_min[0] == doctest::Approx(-1.0f).epsilon(0.01));
  CHECK(pv.bounds_max[1] == doctest::Approx(1.0f).epsilon(0.01));

  // Events: device state changes, points-available ranges, one rotation.
  int points_events = 0, device_events = 0, rotation_events = 0;
  std::uint32_t points_reported = 0;
  Event ev;
  while (e.events().poll(e.app_subscription(), &ev)) {
    switch (ev.type) {
      case EventType::kPointsAvailable:
        ++points_events;
        points_reported += ev.payload.points.count;
        CHECK(ev.payload.points.stream == StreamId::kLidarD6);
        CHECK(ev.payload.points.page == ids[0]);
        break;
      case EventType::kDeviceState: ++device_events; break;
      case EventType::kRotation: ++rotation_events; break;
      default: break;
    }
  }
  CHECK(points_events > 0);
  CHECK(points_reported == 802);
  CHECK(device_events >= 2);  // idle→starting→streaming
  CHECK(rotation_events == 1);  // one closed revolution

  CHECK(e.stop_session().ok());
}

TEST_CASE("engine/d6_start_and_stop_commands_reach_the_app_writer") {
  struct Writer {
    std::vector<std::uint8_t> written;
    static ScanError write(const std::uint8_t* data, std::size_t len, void* user) {
      auto* self = static_cast<Writer*>(user);
      self->written.insert(self->written.end(), data, data + len);
      return ScanError::kOk;
    }
  } writer;

  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  auto id = e.add_device(d6_config(&Writer::write, &writer));
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.record = false;
  REQUIRE(e.start_session(sc).ok());
  REQUIRE(writer.written.size() == 4);
  CHECK(writer.written[0] == 0xAA);
  CHECK(writer.written[1] == 0x55);
  CHECK(writer.written[2] == 0xF0);  // start
  CHECK(writer.written[3] == 0x0F);

  REQUIRE(e.stop_session().ok());
  REQUIRE(writer.written.size() == 8);
  CHECK(writer.written[6] == 0xF5);  // stop
  CHECK(writer.written[7] == 0x0A);
}

TEST_CASE("engine/record_always_writes_raw_bytes_before_parsing") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  // The engine defaults to the real FileRecordWriter (A5 wiring); this test
  // only checks record-before-parse ordering, so install the no-disk writer.
  e.set_recorder(std::make_unique<lscan::NullRecordWriter>());
  auto id = e.add_device(d6_config());
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.lscan_dir = "/tmp/engine-test.lscan";  // NullRecordWriter touches no disk
  sc.record = true;
  REQUIRE(e.start_session(sc).ok());

  const auto bytes = synthetic_revolution(2, 10);
  REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(bytes.data(), bytes.size())).ok());

  const lscan::RecordStats st = e.recorder().stats();
  CHECK(st.chunks_written == 1);
  CHECK(st.bytes_written == bytes.size() + lscan::kChunkOverheadBytes);
  REQUIRE(e.stop_session().ok());
  CHECK_FALSE(e.recorder().is_open());
}

TEST_CASE("engine/devices_can_be_added_while_a_session_runs") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  SessionConfig sc;
  sc.record = false;
  REQUIRE(e.start_session(sc).ok());

  auto id = e.add_device(d6_config());
  REQUIRE(id.ok());
  const auto bytes = synthetic_revolution(1, 10);
  CHECK(e.push_serial_bytes(id.value(), ByteSpan(bytes.data(), bytes.size())).ok());
  CHECK(e.points().total_points() == 11);
  CHECK(e.device_ids().size() == 1);

  CHECK(e.remove_device(id.value()).ok());
  CHECK(e.device_ids().empty());
}

// ===========================================================================
// INT-24: A8's pushbroom, owned by the Engine
// ===========================================================================

TEST_CASE("engine/pushbroom_turns_d6_profiles_into_world_points") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  auto id = e.add_device(d6_config());
  REQUIRE(id.ok());

  // The mount extrinsic is a property of the bracket, so it is set before any
  // session exists. A non-rigid matrix — the column-major-across-JNI trap — is
  // refused rather than silently producing a mirrored cloud.
  double sheared[16] = {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CHECK(e.set_mount_extrinsics(sheared).error() == ScanError::kInvalidArgument);
  // A pure 0.5 m offset along the phone's +z: the lidar sits half a metre in
  // front of the camera on the bracket.
  double mount[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0.5, 0, 0, 0, 1};
  REQUIRE(e.set_mount_extrinsics(mount).ok());

  // The trajectory arrives on its own thread in the field; here it is pushed
  // up front, which the assembler is required to produce the same answer for
  // (docs/A8-pushbroom.md §3.6, replay == capture).
  const std::int64_t t = 5'000'000'000LL;
  const std::int64_t frame = 33'000'000LL;  // ~30 Hz, ARCore's rate
  for (int i = -2; i <= 2; ++i) {
    REQUIRE(e.push_pose(ar_pose(t + i * frame, 10.0, 20.0, 30.0)).ok());
  }
  // Every accepted pose is announced: A1 declared PoseUpdatePayload and until
  // now nobody published it.
  int pose_events = 0;
  Event ev;
  while (e.events().poll(e.app_subscription(), &ev)) {
    if (ev.type == EventType::kPoseUpdate) {
      ++pose_events;
      CHECK(ev.payload.pose.source == StreamId::kPoseAr);
      CHECK(ev.payload.pose.position[1] == doctest::Approx(20.0f));
      CHECK(ev.payload.pose.quaternion[3] == doctest::Approx(1.0f));
    }
  }
  CHECK(pose_events == 5);

  SessionConfig sc;
  sc.record = false;
  sc.pushbroom = true;
  REQUIRE(e.start_session(sc).ok());
  CHECK(e.pushbroom_enabled());

  const auto bytes = synthetic_revolution();
  REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(bytes.data(), bytes.size()), TimePoint{t}).ok());

  // The assembler batches like every other producer; end of stream is what
  // pushes the tail out (stop_session() would do this too).
  REQUIRE(e.pushbroom_flush().ok());

  const PushbroomStats ps = e.pushbroom_stats();
  CHECK(ps.points_in == 401);   // 400 samples + the start packet's
  CHECK(ps.points_out == 401);  // every one of them had a pose
  CHECK(ps.points_pending == 0);
  CHECK(ps.dropped_no_pose == 0);
  CHECK(ps.flagged_total() == 0);

  // Two streams now live in the store: the sensor-frame preview the driver
  // writes (kLidarD6) and the assembled world cloud (kSlamMap). Pages are
  // single-stream, so provenance survives into an export.
  std::uint32_t d6_points = 0, world_points = 0;
  const PointVertex* first_world = nullptr;
  for (const PageId pid : e.points().page_ids()) {
    const PageView pv = e.points().page_view(pid);
    if (pv.stream == StreamId::kLidarD6) {
      d6_points += pv.count;
    } else if (pv.stream == StreamId::kSlamMap) {
      if (first_world == nullptr && pv.count > 0) first_world = pv.data;
      world_points += pv.count;
    }
  }
  CHECK(d6_points == 401);
  CHECK(world_points == 401);
  REQUIRE(first_world != nullptr);

  // p_world = world_from_phone . phone_from_lidar . p_lidar. The pose is a
  // pure translation and the mount a pure +0.5 m z offset, so the first return
  // (1000 mm at 0 degrees = (0, 1, 0) in the sensor frame) must land at
  // (10, 21, 30.5). Getting the composition order backwards moves z, not y.
  CHECK(first_world[0].x == doctest::Approx(10.0f).epsilon(0.001));
  CHECK(first_world[0].y == doctest::Approx(21.0f).epsilon(0.001));
  CHECK(first_world[0].z == doctest::Approx(30.5f).epsilon(0.001));

  // The renderer hears about the world points the same way it hears about
  // every other producer's: one bridge, one event type.
  int slam_map_events = 0;
  while (e.events().poll(e.app_subscription(), &ev)) {
    if (ev.type == EventType::kPointsAvailable && ev.payload.points.stream == StreamId::kSlamMap) {
      ++slam_map_events;
    }
  }
  CHECK(slam_map_events > 0);

  CHECK(e.stop_session().ok());
  CHECK_FALSE(e.pushbroom_enabled());  // a stopped session assembles nothing
}

TEST_CASE("engine/pushbroom_points_without_a_pose_wait_rather_than_being_dropped") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  auto id = e.add_device(d6_config());
  REQUIRE(id.ok());

  // Enabling without an extrinsic is refused: there is nothing to assemble
  // with, and silently assembling with identity would look plausible.
  CHECK(e.set_pushbroom_enabled(true).error() == ScanError::kInvalidState);
  double mount[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  REQUIRE(e.set_mount_extrinsics(mount).ok());

  SessionConfig sc;
  sc.record = false;
  sc.pushbroom = true;
  REQUIRE(e.start_session(sc).ok());

  // Points first, poses later — the real ordering, since ARCore delivers at
  // 30 Hz behind a 4 kpts/s point stream.
  const std::int64_t t = 9'000'000'000LL;
  const auto bytes = synthetic_revolution(2, 10);
  REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(bytes.data(), bytes.size()), TimePoint{t}).ok());
  CHECK(e.pushbroom_stats().points_pending == 21);
  CHECK(e.pushbroom_stats().points_out == 0);

  // The bracketing pose arrives; the pending points resolve on the next push.
  REQUIRE(e.push_pose(ar_pose(t - 10'000'000LL, 0.0, 0.0, 0.0)).ok());
  REQUIRE(e.push_pose(ar_pose(t + 10'000'000LL, 1.0, 0.0, 0.0)).ok());
  REQUIRE(e.pushbroom_flush().ok());

  const PushbroomStats ps = e.pushbroom_stats();
  CHECK(ps.points_pending == 0);
  CHECK(ps.points_out == 21);
  CHECK(ps.dropped_no_pose == 0);

  // Halfway between the two poses, the interpolated origin is x = 0.5.
  const PoseSample s = e.pose_at(t);
  CHECK(s.ok());
  CHECK(s.has_pose);
  CHECK(s.pose.position[0] == doctest::Approx(0.5));
  // Before the first pose is never resolvable; after the last is "retry".
  CHECK(e.pose_at(t - 1'000'000'000LL).gate == PoseGate::kBeforeFirst);
  CHECK(e.pose_at(t + 1'000'000'000LL).gate == PoseGate::kFuture);

  CHECK(e.stop_session().ok());
}

// ===========================================================================
// INT-24: A6's live SLAM, owned by the Engine
// ===========================================================================

TEST_CASE("engine/live_slam_publishes_a_map_from_a_mid360_session") {
  EngineConfig ec = small_engine_config();
  ec.points.page_capacity = 65536;  // the map plus the raw cloud, in few pages
  ec.points.max_pages = 32;
  auto engine = Engine::create(ec);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  DeviceConfig dc;
  dc.kind = DeviceKind::kMid360;
  dc.mid360.backend = Mid360Backend::kInject;   // no sockets, no SDK
  dc.mid360.internal_supervisor_thread = false; // no watchdog thread in a unit test
  dc.mid360.live_points_per_sec = 0;            // no decimation: count every point
  dc.mid360.max_batch_points = mid360::kPointsPerPacket;  // one packet per append
  auto id = e.add_device(dc);
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.record = false;
  sc.live_slam = true;
  // Inline (the engine's default posture) so the whole test is deterministic:
  // the odometry runs on this thread, inside PageStore::append(). A live
  // capture sets internal_thread = true instead — that thread is A6's, and it
  // is the only one the Engine can cause to exist (DESIGN §2).
  sc.lio.internal_thread = false;
  sc.lio.init_imu_samples = 20;  // 0.1 s at 200 Hz, instead of 0.5 s
  sc.lio.scan_period_s = 0.05;
  sc.lio.live_points_per_sec = 0;
  sc.lio.min_range_m = 0.3f;
  sc.lio.max_range_m = 30.0f;
  REQUIRE(e.start_session(sc).ok());
  REQUIRE(e.live_slam() != nullptr);

  // 0.6 s of engine time: IMU at 200 Hz, points at 100 Hz. The device stamps
  // both from one clock and both are mapped through the one kLidarMid360
  // estimator, so the odometry sees a single coherent timeline.
  const std::int64_t t0 = 2'000'000'000LL;
  const std::int64_t step_ns = 5'000'000LL;  // 5 ms
  std::uint16_t imu_cnt = 0, pt_cnt = 0;
  for (int i = 0; i < 120; ++i) {
    const std::int64_t t = t0 + i * step_ns;
    const auto imu = mid360_imu_packet(imu_cnt++, t);
    REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(imu.data(), imu.size()), TimePoint{t}).ok());
    if (i % 2 == 0) {
      const auto pts = mid360_point_packet(pt_cnt++, t);
      REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(pts.data(), pts.size()), TimePoint{t}).ok());
    }
  }

  const LioStats ls = e.live_slam()->stats();
  CHECK(ls.initialized);
  CHECK_FALSE(ls.diverged);
  CHECK(ls.imu_samples >= 100);
  CHECK(ls.points_in > 0);
  CHECK(ls.scans > 0);
  CHECK(ls.points_mapped > 0);
  // The trajectory is reachable as a PoseSource, which is what A8/A11 and
  // A10's fusion consume it through.
  CHECK(e.live_slam()->poses().size() == ls.scans);
  Pose p;
  CHECK(e.live_slam()->current_pose(&p));
  CHECK(p.t_mono_ns > t0);

  // The map went into the ENGINE's page store, on its own stream, and reached
  // the app through the same kPointsAvailable bridge every other producer uses.
  std::uint64_t raw_points = 0, map_points = 0;
  for (const PageId pid : e.points().page_ids()) {
    const PageView pv = e.points().page_view(pid);
    if (pv.stream == StreamId::kLidarMid360) raw_points += pv.count;
    if (pv.stream == StreamId::kSlamMap) map_points += pv.count;
  }
  CHECK(raw_points == 60ull * mid360::kPointsPerPacket);
  CHECK(map_points > 0);
  CHECK(map_points == ls.points_mapped);

  int map_events = 0, raw_events = 0;
  Event ev;
  while (e.events().poll(e.app_subscription(), &ev)) {
    if (ev.type != EventType::kPointsAvailable) continue;
    if (ev.payload.points.stream == StreamId::kSlamMap) ++map_events;
    if (ev.payload.points.stream == StreamId::kLidarMid360) ++raw_events;
  }
  CHECK(map_events > 0);
  CHECK(raw_events > 0);

  // The IMU never enters the PageStore (DESIGN §6) — it lands in the one
  // ImuIngest the point stream shares an estimator with.
  const ImuIngestStats is = e.imu().stats();
  CHECK(is.samples == 120);
  CHECK(is.rate_hz == doctest::Approx(200.0).epsilon(0.05));
  CHECK(e.imu().stream() == StreamId::kLidarMid360);

  CHECK(e.stop_session().ok());
  // Stopping the session releases the odometry (and, in a live capture, its
  // thread); the map it produced stays in the store.
  CHECK(e.live_slam() == nullptr);
  CHECK(e.points().total_points() >= raw_points + map_points);
}

TEST_CASE("engine/record_only_sessions_run_no_odometry") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  DeviceConfig dc;
  dc.kind = DeviceKind::kMid360;
  dc.mid360.backend = Mid360Backend::kInject;
  dc.mid360.internal_supervisor_thread = false;
  dc.mid360.live_points_per_sec = 0;
  dc.mid360.max_batch_points = mid360::kPointsPerPacket;
  auto id = e.add_device(dc);
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.record = false;
  sc.live_slam = false;  // the spec's Record-only half of the toggle
  REQUIRE(e.start_session(sc).ok());
  CHECK(e.live_slam() == nullptr);

  const std::int64_t t0 = 3'000'000'000LL;
  for (int i = 0; i < 20; ++i) {
    const std::int64_t t = t0 + i * 5'000'000LL;
    const auto imu = mid360_imu_packet(static_cast<std::uint16_t>(i), t);
    REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(imu.data(), imu.size()), TimePoint{t}).ok());
    const auto pts = mid360_point_packet(static_cast<std::uint16_t>(i), t);
    REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(pts.data(), pts.size()), TimePoint{t}).ok());
  }

  // Points and IMU still flow — record-only is not "do less decoding" — but
  // nothing is registered and no kSlamMap page exists.
  CHECK(e.imu().stats().samples == 20);
  for (const PageId pid : e.points().page_ids()) {
    CHECK(e.points().page_view(pid).stream == StreamId::kLidarMid360);
  }
  CHECK(e.points().total_points() == 20ull * mid360::kPointsPerPacket);
  CHECK(e.stop_session().ok());
}

TEST_CASE("engine/corrupt_checksums_degrade_the_device_but_keep_it_running") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  auto cfg = d6_config();
  cfg.d6.health_min_packets = 4;  // rate the health quickly in a unit test
  auto id = e.add_device(cfg);
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.record = false;
  REQUIRE(e.start_session(sc).ok());

  // A few good packets, then corrupted ones.
  std::vector<std::uint8_t> stream;
  for (int i = 0; i < 2; ++i) {
    d6test::PacketSpec sp;
    sp.first_angle_deg = 0.0;
    sp.last_angle_deg = 9.0;
    for (int k = 0; k < 10; ++k) sp.samples.push_back(d6test::Sample{1000, 100, false});
    d6test::append(&stream, d6test::build(sp));
  }
  for (int i = 0; i < 6; ++i) {
    d6test::PacketSpec sp;
    sp.cs_mode = d6test::CsMode::kCorrupt;
    sp.first_angle_deg = 0.0;
    sp.last_angle_deg = 9.0;
    for (int k = 0; k < 10; ++k) sp.samples.push_back(d6test::Sample{1000, 100, false});
    d6test::append(&stream, d6test::build(sp));
  }
  REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(stream.data(), stream.size())).ok());

  auto h = e.device_health(id.value());
  REQUIRE(h.ok());
  CHECK(h.value().packets_ok == 2);
  CHECK(h.value().packets_bad == 6);
  CHECK(h.value().checksum_pass_rate < 0.995);
  CHECK(h.value().state == DeviceState::kDegraded);  // degraded, not faulted
}

TEST_CASE("engine/mid360_record_always_writes_raw_datagrams") {
  // C2/C3 field finding: a Mid-360 capture streamed live but recorded 0
  // chunks, because only D6's push_serial_bytes() reached the recorder. The
  // raw shim now records every datagram, valid or not, exactly once.
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  e.set_recorder(std::make_unique<lscan::NullRecordWriter>());

  DeviceConfig dc;
  dc.kind = DeviceKind::kMid360;
  dc.mid360.backend = Mid360Backend::kInject;
  dc.mid360.internal_supervisor_thread = false;
  dc.mid360.live_points_per_sec = 0;
  auto id = e.add_device(dc);
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.lscan_dir = "/tmp/engine-test-mid360.lscan";  // NullRecordWriter touches no disk
  sc.record = true;
  REQUIRE(e.start_session(sc).ok());

  const std::int64_t t0 = 1'000'000'000LL;
  const auto pkt = mid360_point_packet(0, t0);
  const auto imu = mid360_imu_packet(1, t0 + 5'000'000);
  REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(pkt.data(), pkt.size())).ok());
  REQUIRE(e.push_serial_bytes(id.value(), ByteSpan(imu.data(), imu.size())).ok());

  const lscan::RecordStats st = e.recorder().stats();
  CHECK(st.chunks_written == 2);  // one kMid360Points + one kMid360Imu
  CHECK(st.bytes_written ==
        pkt.size() + imu.size() + 2 * lscan::kChunkOverheadBytes);
  REQUIRE(e.stop_session().ok());
}

// ===========================================================================
// INT-29: A10's GNSS/RTK stack, wired into the engine
// ===========================================================================

TEST_CASE("engine/rtk_rover_session_end_to_end_from_synthetic_nmea") {
  // The whole §3.4 path through the Engine: raw NMEA in on a kRtkRover device,
  // fixes and their quality out as events, the bytes recorded before they are
  // parsed, and the local↔global transform converging against a synthetic SLAM
  // track until the session has a real CRS to export in.
  EngineConfig cfg = small_engine_config();
  // Deterministic, not the default: with min_interval_ns the fusion decimates
  // to 1 Hz (which this stream already is) and resolve_interval_ns would make
  // the assertion depend on when the last solve happened.
  cfg.georef.min_interval_ns = 0;
  cfg.georef.resolve_interval_ns = 0;
  // 200 fixes + 2000 poses + lifecycle: the default 1024-event ring would drop
  // the oldest, and this case asserts on every fix event it published.
  cfg.event_queue_capacity = 8192;
  auto engine = Engine::create(cfg);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  e.set_recorder(std::make_unique<lscan::NullRecordWriter>());

  DeviceConfig dc;
  dc.kind = DeviceKind::kRtkRover;
  auto id = e.add_device(dc);
  REQUIRE(id.ok());

  SessionConfig sc;
  sc.lscan_dir = "/tmp/engine-test-rtk.lscan";  // NullRecordWriter touches no disk
  sc.record = true;
  REQUIRE(e.start_session(sc).ok());

  // Ground truth: a 40 x 25 m loop walked at 1.2 m/s, 1 Hz, near Hong Kong
  // (UTM zone 50N). The LOCAL frame is that truth mapped through the inverse of
  // the transform the fusion then has to recover.
  const crs::Geodetic origin{22.2830, 114.1585, 48.0};
  const crs::EnuFrame frame = crs::make_enu_frame(origin);
  const double yaw = 37.0 * crs::kDeg, cy = std::cos(yaw), sy = std::sin(yaw);
  const double tx = 12.0, ty = -8.0, tz = 1.25;
  const int kEpochs = 200;
  // NOT zero: TimePoint{0} means "stamp on arrival" at the engine boundary
  // (scan_engine_push_serial_bytes' documented contract), so a test that wants
  // deterministic engine times has to start the grid somewhere else.
  const std::int64_t kT0 = 5'000'000'000LL;

  for (int t = 0; t < kEpochs; ++t) {
    const std::int64_t t_ns = kT0 + static_cast<std::int64_t>(t) * 1'000'000'000LL;
    double gx = 0, gy = 0;
    loop_point(1.2 * t, &gx, &gy);
    // Walk AT the reference frame's height: the fusion's ENU frame is the
    // GnssSource's, anchored on the first fix, so keeping the first fix at
    // (0, 0, 0) makes the two frames identical and the expected translation
    // literally (tx, ty, tz).
    const double gz = 0.0;

    // The local track, pushed BEFORE the fix it will be paired with: an epoch
    // closes one epoch late (§7), so by the time the fusion asks for the pose
    // at t it is bracketed on both sides. 10 Hz, not 1 Hz, because
    // ExternalPoseConfig::max_gap_ns is 200 ms — a trajectory sampled at the
    // GNSS rate would make every interpolation kStale and
    // GeorefConfig::require_ungated_local_pose would (correctly) drop it.
    for (int k = 0; k < 10; ++k) {
      double px = 0, py = 0;
      loop_point(1.2 * (t + 0.1 * k), &px, &py);
      const double dx = px - tx, dy = py - ty, dz = gz - tz;
      REQUIRE(e.push_pose(ar_pose(t_ns + k * 100'000'000LL, cy * dx + sy * dy,
                                  -sy * dx + cy * dy, dz))
                  .ok());
    }

    const crs::Geodetic g = crs::enu_to_geodetic(frame, crs::Enu{gx, gy, gz});
    // GGA altitude is ORTHOMETRIC and the sentence says geoid sep = -2.0, so
    // MSL = ellipsoidal + 2.0. Getting this backwards is the classic 30-metre
    // georeferencing bug, and the fusion's vertical residual would show it.
    REQUIRE(push_nmea(e, id.value(), nmea_epoch(t, g.lat_deg, g.lon_deg, g.height_m + 2.0),
                      t_ns)
                .ok());
  }
  // Closes the last pending epoch — the one moment a 1 Hz receiver's final fix
  // would otherwise be lost.
  REQUIRE(e.stop_session().ok());

  // --- the fix stream ------------------------------------------------------
  const GnssStats gs = e.gnss_stats();
  MESSAGE("gnss: " << gs.epochs << " epochs, " << gs.fixes_published << " fixes, "
                   << gs.nmea.sentences_ok << " sentences, " << gs.gst_epochs << " with GST");
  CHECK(gs.epochs == static_cast<std::uint64_t>(kEpochs));
  CHECK(gs.fixes_published == static_cast<std::uint64_t>(kEpochs));
  CHECK(gs.nmea.sentences_ok == 3ull * kEpochs);
  CHECK(gs.nmea.checksum_failed == 0);
  CHECK(gs.nmea.checksum_pass_rate() == doctest::Approx(1.0));
  CHECK(gs.by_fix[static_cast<int>(FixType::kRtkFixed)] == static_cast<std::uint64_t>(kEpochs));
  CHECK(gs.gst_epochs == static_cast<std::uint64_t>(kEpochs));
  CHECK(e.gnss().has_origin());

  const GnssFix last = e.last_fix();
  CHECK(last.fix == FixType::kRtkFixed);
  CHECK(last.satellites == 22);
  CHECK(last.sigma_from_gst);
  CHECK(last.sigma_horizontal_m == doctest::Approx(0.02).epsilon(0.01));
  CHECK(last.has_geoid_sep);
  CHECK(last.geoid_sep_m == doctest::Approx(-2.0));
  // alt_m is orthometric and height_ellipsoid_m is what the geodesy uses; the
  // two must differ by exactly the reported separation.
  CHECK(last.height_ellipsoid_m == doctest::Approx(last.alt_m + last.geoid_sep_m));

  // --- record-always -------------------------------------------------------
  // One chunk per pushed buffer, written BEFORE parsing, exactly like kD6Raw.
  const lscan::RecordStats rs = e.recorder().stats();
  CHECK(rs.chunks_written == static_cast<std::uint64_t>(kEpochs));
  CHECK(rs.bytes_written > 0);
  CHECK(lscan::stream_of(lscan::ChunkType::kGnssNmea) == StreamId::kGnss);

  // --- the device row ------------------------------------------------------
  auto h = e.device_health(id.value());
  REQUIRE(h.ok());
  CHECK(h.value().kind == DeviceKind::kRtkRover);
  CHECK(h.value().packets_ok == 3ull * kEpochs);
  CHECK(h.value().packets_bad == 0);
  CHECK(h.value().checksum_pass_rate == doctest::Approx(1.0));
  CHECK(h.value().bytes_in > 0);

  // --- georeferencing ------------------------------------------------------
  const GeorefSolution sol = e.georef_solution();
  MESSAGE("georef: yaw " << sol.yaw_deg << " deg (37), t = (" << sol.translation[0] << ", "
                         << sol.translation[1] << ", " << sol.translation[2]
                         << ") vs (12, -8, 1.25), CEP95 " << sol.cep95_m << " m over "
                         << sol.inliers << " fixes");
  CHECK(sol.converged);
  CHECK(std::fabs(sol.yaw_deg - 37.0) < 0.2);
  CHECK(std::fabs(sol.translation[0] - tx) < 0.05);
  CHECK(std::fabs(sol.translation[1] - ty) < 0.05);
  CHECK(std::fabs(sol.translation[2] - tz) < 0.10);
  CHECK(sol.gravity_residual_m < 0.05);  // the local frame really was Z-up
  CHECK(sol.cep95_m < 0.20);
  CHECK(sol.inliers > 150);

  // The A9 seam has something real in it now.
  CHECK(e.crs_epsg() == "EPSG:32650");
  CHECK(e.crs_wkt().find("PROJCS[\"WGS 84 / UTM zone 50N\"") == 0);
  CHECK(e.georef().epsg() == 32650);

  // --- the events B9's status strip reads ----------------------------------
  int fix_events = 0, georef_events = 0;
  GnssFixPayload last_fix_ev{};
  GeorefConvergedPayload conv{};
  Event ev;
  while (e.events().poll(e.app_subscription(), &ev)) {
    if (ev.type == EventType::kGnssFix) {
      ++fix_events;
      last_fix_ev = ev.payload.gnss;
    } else if (ev.type == EventType::kGeorefConverged) {
      ++georef_events;
      conv = ev.payload.georef;
    }
  }
  CHECK(fix_events == kEpochs);
  CHECK(last_fix_ev.fix_type == static_cast<std::uint8_t>(FixType::kRtkFixed));
  CHECK(last_fix_ev.satellites == 22);
  CHECK(last_fix_ev.sigma_h_m == doctest::Approx(0.02).epsilon(0.01));
  CHECK(last_fix_ev.hdop == doctest::Approx(0.6f));
  // Convergence is announced once, on the transition — not once per fix.
  CHECK(georef_events == 1);
  CHECK(conv.converged == 1);
  CHECK(conv.epsg == 32650);
  // Announced at the FIRST moment it holds — SimilarityEstimatorConfig's 8
  // sample / 5 m span floor — not at the end of the session. That is the point:
  // it is when the capture UI may tell the operator the scan is exportable.
  CHECK(conv.samples == 8);
  CHECK(conv.cep95_m < 0.20);
}

TEST_CASE("engine/a_rover_below_the_gate_reports_the_blocker_instead_of_a_transform") {
  // The other half of §3.4's quality gate: a Single-fix session still produces
  // a trajectory and a fix timeline, but the fusion refuses to call itself
  // georeferenced, and every to_global*() refuses with it.
  EngineConfig cfg = small_engine_config();
  cfg.georef.min_interval_ns = 0;
  cfg.georef.resolve_interval_ns = 0;
  auto engine = Engine::create(cfg);
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  DeviceConfig dc;
  dc.kind = DeviceKind::kRtkRover;
  auto id = e.add_device(dc);
  REQUIRE(id.ok());
  SessionConfig sc;
  sc.record = false;
  REQUIRE(e.start_session(sc).ok());

  const crs::Geodetic origin{22.2830, 114.1585, 48.0};
  const crs::EnuFrame frame = crs::make_enu_frame(origin);
  const std::int64_t kT0 = 5'000'000'000LL;  // 0 means "stamp on arrival"
  for (int t = 0; t < 40; ++t) {
    const std::int64_t t_ns = kT0 + static_cast<std::int64_t>(t) * 1'000'000'000LL;
    double gx = 0, gy = 0;
    loop_point(1.2 * t, &gx, &gy);
    for (int k = 0; k < 10; ++k) {  // 10 Hz, for max_gap_ns — see the case above
      double px = 0, py = 0;
      loop_point(1.2 * (t + 0.1 * k), &px, &py);
      REQUIRE(e.push_pose(ar_pose(t_ns + k * 100'000'000LL, px, py, 2.0)).ok());
    }
    const crs::Geodetic g = crs::enu_to_geodetic(frame, crs::Enu{gx, gy, 2.0});
    // quality 1 = single point, sigma 2 m — below GeorefConfig::min_fix.
    REQUIRE(push_nmea(e, id.value(),
                      nmea_epoch(t, g.lat_deg, g.lon_deg, g.height_m + 2.0, 1, 2.0), t_ns)
                .ok());
  }
  REQUIRE(e.stop_session().ok());

  const GnssStats gs = e.gnss_stats();
  CHECK(gs.by_fix[static_cast<int>(FixType::kSingle)] == 40);
  CHECK(gs.poses_published == 40);  // a coarse trajectory is still a trajectory

  const GeorefSolution sol = e.georef_solution();
  CHECK_FALSE(sol.converged);
  CHECK(sol.samples == 0);
  // Every fix was offered and every one was refused on quality — not silently
  // down-weighted, which is what would let 2 m fixes drag a survey around.
  CHECK(e.georef().stats().offered == 40);
  CHECK(e.georef().stats().skipped_fix_quality == 40);
  CHECK(e.georef().stats().accepted == 0);

  // The SITE's CRS is known — an origin was anchored on the first Single fix,
  // and the UTM zone is a property of where you are standing...
  CHECK(e.georef().epsg() == 32650);
  CHECK_FALSE(e.georef().crs_wkt().empty());
  // ...but the EXPORT's is not, and that is the stricter gate: A9 gets the
  // empty string, its documented "embed the local-frame placeholder" input,
  // rather than a UTM label on a local-frame cloud.
  CHECK(e.crs_wkt().empty());
  CHECK(e.crs_epsg().empty());

  double enu[3] = {0, 0, 0};
  const double local[3] = {1, 2, 3};
  CHECK(e.georef().to_global_point(local, enu).error() == ScanError::kInvalidState);
}

TEST_CASE("engine/the_rtk_trajectory_drives_the_pushbroom_with_no_arcore") {
  // Tech Spec §3.3: "Desktop D6 capture: no ARCore → RTK-trajectory mode only".
  // No new code path — GnssSource is a PoseInterpolator, so the assembler
  // cannot tell it from the ARCore ring (§3 key rule 3).
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();

  DeviceConfig rover;
  rover.kind = DeviceKind::kRtkRover;
  auto rover_id = e.add_device(rover);
  REQUIRE(rover_id.ok());
  auto d6_id = e.add_device(d6_config());
  REQUIRE(d6_id.ok());

  const double mount[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  REQUIRE(e.set_mount_extrinsics(mount).ok());

  SessionConfig sc;
  sc.record = false;
  sc.pushbroom = true;
  sc.trajectory = TrajectorySource::kGnss;
  REQUIRE(e.start_session(sc).ok());
  CHECK(e.trajectory_source() == TrajectorySource::kGnss);
  // Nothing was ever pushed into the ARCore ring; the assembler is reading the
  // rover instead.
  CHECK(e.pose_at(9'000'000'000LL).gate == PoseGate::kNoData);

  const crs::Geodetic origin{22.2830, 114.1585, 48.0};
  const crs::EnuFrame frame = crs::make_enu_frame(origin);
  const std::int64_t kT0 = 5'000'000'000LL;  // 0 means "stamp on arrival"
  for (int t = 0; t < 8; ++t) {
    const std::int64_t t_ns = kT0 + static_cast<std::int64_t>(t) * 1'000'000'000LL;
    double gx = 0, gy = 0;
    loop_point(1.2 * t, &gx, &gy);
    const crs::Geodetic g = crs::enu_to_geodetic(frame, crs::Enu{gx, gy, 2.0});
    REQUIRE(push_nmea(e, rover_id.value(), nmea_epoch(t, g.lat_deg, g.lon_deg, g.height_m + 2.0),
                      t_ns)
                .ok());
  }
  CHECK(e.gnss_stats().poses_published >= 6);

  // A D6 revolution in the middle of the rover's trajectory, so every return
  // is bracketed by two GNSS poses.
  const std::int64_t t_scan = kT0 + 4'000'000'000LL;
  const auto bytes = d6test::build_revolution(4, 20, /*distance_mm=*/1000, /*intensity=*/90);
  REQUIRE(e.push_serial_bytes(d6_id.value(), ByteSpan(bytes.data(), bytes.size()),
                              TimePoint{t_scan})
              .ok());
  REQUIRE(e.pushbroom_flush().ok());

  const PushbroomStats ps = e.pushbroom_stats();
  MESSAGE("pushbroom on RTK: " << ps.points_in << " in, " << ps.points_out << " out, "
                               << ps.dropped_no_pose << " without a pose");
  CHECK(ps.points_in == 81);
  CHECK(ps.points_out == 81);
  CHECK(ps.dropped_no_pose == 0);
  CHECK(ps.flagged_tracking_lost == 0);

  // World-frame points, in the session's ENU frame, on the registered-map
  // stream — the same pages A9 exports and A14 renders.
  std::uint64_t world = 0;
  const PointVertex* first = nullptr;
  for (const PageId pid : e.points().page_ids()) {
    const PageView pv = e.points().page_view(pid);
    if (pv.stream != StreamId::kSlamMap) continue;
    world += pv.count;
    if (first == nullptr && pv.count > 0) first = pv.data;
  }
  CHECK(world == 81);
  REQUIRE(first != nullptr);

  // 1000 mm at 0 degrees is +1 m on the sensor's y; the rover's own pose at
  // t_scan is where it lands. Reading that pose through the SAME interpolator
  // the assembler used is the check that the two agree.
  const PoseSample s = e.gnss().sample_at(t_scan);
  REQUIRE(s.has_pose);
  CHECK(static_cast<double>(first->x) == doctest::Approx(s.pose.position[0]).epsilon(0.01));
  CHECK(static_cast<double>(first->z) == doctest::Approx(s.pose.position[2]).epsilon(0.01));

  // Switching back mid-session is one call and needs no re-added device.
  REQUIRE(e.set_trajectory_source(TrajectorySource::kExternal).ok());
  CHECK(e.trajectory_source() == TrajectorySource::kExternal);
  REQUIRE(e.stop_session().ok());
}
