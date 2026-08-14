// Engine lifecycle + the D6 driver end to end: synthetic S1 capture bytes in,
// decoded points out through the PageStore and the EventBus.
#include <string>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"
#include "scanengine/core/engine.h"

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

}  // namespace

TEST_CASE("engine/create_starts_idle_and_reports_a_version") {
  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  CHECK(e.state() == EngineState::kIdle);
  CHECK_FALSE(e.session_active());
  CHECK(std::string(engine_version_string()).find("scanengine") == 0);
  CHECK(kEngineAbiVersion == 1);
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
