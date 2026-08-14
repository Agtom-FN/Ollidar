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
  CHECK(SCAN_ABI_VERSION == 2u);  // moves with scanengine::kEngineAbiVersion
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

// ===========================================================================
// INT-24: the A8 surface, from C
// ===========================================================================

TEST_CASE("capi/pose_and_pushbroom_entry_points_reject_null_handles") {
  // Every new entry point must be an error, never a crash, on a null handle —
  // JNI passes 0 for an unopened engine more often than anyone would like.
  scan_pose p;
  std::memset(&p, 0, sizeof(p));
  scan_pushbroom_stats st;
  const double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  CHECK(scan_engine_push_pose(nullptr, &p, -1.f) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_pose_at(nullptr, 0, &p, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_set_mount_extrinsics(nullptr, m) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_pushbroom_enable(nullptr, 1) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_pushbroom_flush(nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_pushbroom_stats(nullptr, &st) == SCAN_ERR_INVALID_ARGUMENT);

  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(nullptr, &e) == SCAN_OK);
  CHECK(scan_engine_push_pose(e, nullptr, -1.f) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_set_mount_extrinsics(e, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_pushbroom_stats(e, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  scan_engine_destroy(e);

  CHECK(scan_mount_calib_create(nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_mount_calib_add_observation(nullptr, nullptr, 1.0, nullptr, 0, 0.02) ==
        SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_mount_calib_solve(nullptr, m, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
}

TEST_CASE("capi/pushbroom_world_points_cross_the_abi_on_the_slam_map_stream") {
  scan_engine_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "capi-pushbroom";
  cfg.log_level = SCAN_LOG_OFF;
  cfg.page_capacity = 4096;
  cfg.max_pages = 8;

  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(&cfg, &e) == SCAN_OK);

  scan_device_config dev;
  std::memset(&dev, 0, sizeof(dev));
  dev.kind = SCAN_DEVICE_D6;
  dev.serial_port_name = "capi";
  std::uint32_t device_id = 0;
  REQUIRE(scan_engine_add_device(e, &dev, &device_id) == SCAN_OK);

  const double mount[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  REQUIRE(scan_engine_set_mount_extrinsics(e, mount) == SCAN_OK);

  const std::int64_t t = 7'000'000'000LL;
  scan_pose p;
  std::memset(&p, 0, sizeof(p));
  p.orientation[3] = 1.0;
  p.source = SCAN_STREAM_POSE_AR;
  p.quality = SCAN_POSE_QUALITY_GOOD;
  for (int i = -1; i <= 1; ++i) {
    p.t_mono_ns = t + i * 33'000'000LL;
    p.position[2] = 2.0;  // 2 m above the origin for the whole sweep
    REQUIRE(scan_engine_push_pose(e, &p, -1.0f) == SCAN_OK);
  }

  scan_session_config session;
  std::memset(&session, 0, sizeof(session));
  session.record = 0;
  session.pushbroom = 1;
  REQUIRE(scan_engine_start(e, &session) == SCAN_OK);

  const auto bytes = d6test::build_revolution(4, 20, 1000, 90);
  REQUIRE(scan_engine_push_serial_bytes(e, device_id, bytes.data(), bytes.size(), t) == SCAN_OK);
  REQUIRE(scan_engine_pushbroom_flush(e) == SCAN_OK);

  scan_pushbroom_stats st;
  REQUIRE(scan_engine_pushbroom_stats(e, &st) == SCAN_OK);
  CHECK(st.points_in == 81);
  CHECK(st.points_out == 81);
  CHECK(st.points_pending == 0);
  CHECK(st.flagged_tracking_lost == 0);
  CHECK(st.t_first_ns == t);

  // The assembled cloud is reachable through the ordinary page API — no new
  // accessor, because it is an ordinary stream in the ordinary store.
  std::uint32_t pages = 0;
  REQUIRE(scan_engine_page_count(e, &pages) == SCAN_OK);
  std::uint32_t world = 0;
  bool saw_world_page = false;
  for (std::uint32_t i = 0; i < pages; ++i) {
    std::uint32_t page_id = 0;
    REQUIRE(scan_engine_page_id_at(e, i, &page_id) == SCAN_OK);
    scan_point_page page;
    std::memset(&page, 0, sizeof(page));
    REQUIRE(scan_engine_get_point_page(e, page_id, &page) == SCAN_OK);
    if (page.stream != SCAN_STREAM_SLAM_MAP) continue;
    world += page.count;
    if (!saw_world_page && page.count > 0) {
      saw_world_page = true;
      // 1000 mm at 0 degrees, lifted by the pose: (0, 1, 2).
      CHECK(page.data[0].y == doctest::Approx(1.0f).epsilon(0.001));
      CHECK(page.data[0].z == doctest::Approx(2.0f).epsilon(0.001));
    }
  }
  CHECK(saw_world_page);
  CHECK(world == 81);

  // The pose event payload survives conversion (it is the first payload the
  // ABI mirrors that A1 declared and nobody published).
  int pose_events = 0;
  scan_event ev;
  while (scan_engine_poll_event(e, &ev) == SCAN_OK) {
    if (ev.type != SCAN_EVENT_POSE_UPDATE) continue;
    ++pose_events;
    CHECK(ev.payload.pose.source == SCAN_STREAM_POSE_AR);
    CHECK(ev.payload.pose.position[2] == doctest::Approx(2.0f));
    CHECK(ev.payload.pose.quality == 255);  // kGood
  }
  CHECK(pose_events == 3);

  CHECK(scan_engine_stop(e) == SCAN_OK);
  scan_engine_destroy(e);
}

TEST_CASE("capi/mount_calibration_gates_an_undetermined_capture") {
  scan_mount_calib* calib = nullptr;
  REQUIRE(scan_mount_calib_create(&calib) == SCAN_OK);
  REQUIRE(calib != nullptr);

  const double cad[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  scan_mount_calib_result r;

  // Nothing at all: six unknowns against zero constraints.
  CHECK(scan_mount_calib_solve(calib, cad, &r) == SCAN_ERR_INVALID_ARGUMENT);

  // Two observations is still undetermined, not merely ill-conditioned, and
  // the solver refuses rather than returning a confident-looking answer.
  std::vector<scan_point_vertex> pts(16);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    pts[i] = scan_point_vertex{static_cast<float>(0.05 * static_cast<double>(i % 4)),
                               static_cast<float>(0.05 * static_cast<double>(i / 4)), 1.5f,
                               128, 128, 128, 255};
  }
  const double n[3] = {0.0, 0.0, -1.0};
  for (int k = 0; k < 2; ++k) {
    CHECK(scan_mount_calib_add_observation(calib, n, 1.5, pts.data(),
                                           static_cast<std::uint32_t>(pts.size()), 0.02) ==
          SCAN_OK);
  }
  CHECK(scan_mount_calib_solve(calib, cad, &r) == SCAN_ERR_INVALID_ARGUMENT);

  // A non-rigid CAD nominal is the other way this goes wrong in the field.
  const double sheared[16] = {1, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CHECK(scan_mount_calib_solve(calib, sheared, &r) == SCAN_ERR_INVALID_ARGUMENT);

  scan_mount_calib_destroy(calib);
}
