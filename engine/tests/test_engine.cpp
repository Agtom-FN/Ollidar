// Engine lifecycle + the D6 driver end to end: synthetic S1 capture bytes in,
// decoded points out through the PageStore and the EventBus.
//
// Since INT-24 this file also covers the two integrations the Engine owns
// rather than a module: A8's pushbroom (poses in over the C++ API, world
// points out on StreamId::kSlamMap) and A6's live SLAM (Mid-360 datagrams in
// through the kInject backend, a registered map out on the same stream).
#include <cstddef>
#include <cstdint>
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
  // 2: INT-24's poses + pushbroom + mount calibration + live-SLAM knobs.
  CHECK(kEngineAbiVersion == 2);
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

  DeviceConfig rtk;
  rtk.kind = DeviceKind::kRtkRover;
  CHECK(e.add_device(rtk).error() == ScanError::kUnimplemented);  // A10

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
