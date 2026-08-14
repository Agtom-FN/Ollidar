// C ABI tests. The heavy lifting is in capi_smoke.c, which is compiled as
// C — this file feeds it a synthetic D6 capture and adds the checks that are
// easier to express in C++ (mirror-struct sizes, event conversion).
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"

extern "C" {
#include "scanengine_c.h"
int scan_capi_smoke_run(const uint8_t* d6_bytes, size_t d6_len);
}

TEST_CASE("capi/smoke_sequence_from_C_with_a_synthetic_D6_capture") {
  const std::vector<std::uint8_t> bytes =
      d6test::build_revolution(10, 40, /*distance_mm=*/1500, /*intensity=*/200);
  const int rc = scan_capi_smoke_run(bytes.data(), bytes.size());
  INFO("failing step: " << rc << " (" << scan_engine_last_error() << ")");
  CHECK(rc == 0);
}

TEST_CASE("capi/abi_version_and_error_strings") {
  CHECK(scan_engine_abi_version() == SCAN_ABI_VERSION);
  CHECK(std::string(scan_error_str(SCAN_OK)) == "ok");
  CHECK(std::string(scan_error_str(SCAN_ERR_CHECKSUM)) == "checksum failed");
  CHECK(std::string(scan_error_str(9999)) == "unrecognized error code");
  CHECK(std::string(scan_engine_version_string()).find("scanengine") == 0);
}

TEST_CASE("capi/create_rejects_a_null_out_handle") {
  CHECK(scan_engine_create(nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  scan_engine* e = nullptr;
  CHECK(scan_engine_create(nullptr, &e) == SCAN_OK);  // NULL config = defaults
  REQUIRE(e != nullptr);
  scan_engine_destroy(e);
}

TEST_CASE("capi/point_vertex_mirror_is_the_S3_layout") {
  CHECK(sizeof(scan_point_vertex) == 16);
  CHECK(offsetof(scan_point_vertex, x) == 0);
  CHECK(offsetof(scan_point_vertex, r) == 12);
}

TEST_CASE("capi/events_survive_conversion_with_their_payloads") {
  scan_engine_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "capi-events";
  cfg.log_level = SCAN_LOG_OFF;
  cfg.page_capacity = 4096;
  cfg.max_pages = 4;

  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(&cfg, &e) == SCAN_OK);

  scan_device_config dev;
  std::memset(&dev, 0, sizeof(dev));
  dev.kind = SCAN_DEVICE_D6;
  dev.serial_port_name = "capi";
  std::uint32_t device_id = 0;
  REQUIRE(scan_engine_add_device(e, &dev, &device_id) == SCAN_OK);

  scan_session_config session;
  std::memset(&session, 0, sizeof(session));
  session.record = 0;
  REQUIRE(scan_engine_start(e, &session) == SCAN_OK);

  const auto bytes = d6test::build_revolution(4, 20, 2000, 90);
  REQUIRE(scan_engine_push_serial_bytes(e, device_id, bytes.data(), bytes.size(), 12345) ==
          SCAN_OK);

  int points_events = 0, device_events = 0;
  std::uint32_t points_total = 0;
  scan_event ev;
  while (scan_engine_poll_event(e, &ev) == SCAN_OK) {
    if (ev.type == SCAN_EVENT_POINTS_AVAILABLE) {
      ++points_events;
      points_total += ev.payload.points.count;
      CHECK(ev.payload.points.stream == SCAN_STREAM_LIDAR_D6);
      CHECK(ev.payload.points.page != 0);
    } else if (ev.type == SCAN_EVENT_DEVICE_STATE) {
      ++device_events;
      CHECK(ev.payload.device.device == device_id);
      CHECK(ev.payload.device.kind == SCAN_DEVICE_D6);
    }
  }
  CHECK(points_events > 0);
  CHECK(points_total == 81);  // 4x20 samples + the start packet's sample
  CHECK(device_events >= 2);

  uint64_t total = 0;
  CHECK(scan_engine_total_points(e, &total) == SCAN_OK);
  CHECK(total == 81);

  CHECK(scan_engine_stop(e) == SCAN_OK);
  scan_engine_destroy(e);
}
