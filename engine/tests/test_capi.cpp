// C ABI tests. The heavy lifting is in capi_smoke.c, which is compiled as
// C — this file feeds it a synthetic D6 capture and adds the checks that are
// easier to express in C++ (mirror-struct sizes, event conversion).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"
#include "scanengine/color/frames_idx.h"
#include "scanengine/core/event.h"
#include "scanengine/jobs/job_types.h"

extern "C" {
#include "scanengine_c.h"
int scan_capi_smoke_run(const uint8_t* d6_bytes, size_t d6_len);
}

namespace scanengine {
namespace capi_test {
// Defined in capi/scanengine_c.cpp — see the comment there. Not part of the
// ABI and not declared in scanengine_c.h.
void convert_event_for_test(const Event& in, scan_event* out);
}  // namespace capi_test
}  // namespace scanengine

namespace {

std::string capi_temp_dir(const char* tag) {
  static std::atomic<long long> counter{0};
  const auto n = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() /
      (std::string("capi_int34_") + tag + "_" + std::to_string(now) + "_" + std::to_string(n));
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  return p.string();
}

}  // namespace

TEST_CASE("capi/smoke_sequence_from_C_with_a_synthetic_D6_capture") {
  const std::vector<std::uint8_t> bytes =
      d6test::build_revolution(10, 40, /*distance_mm=*/1500, /*intensity=*/200);
  const int rc = scan_capi_smoke_run(bytes.data(), bytes.size());
  INFO("failing step: " << rc << " (" << scan_engine_last_error() << ")");
  CHECK(rc == 0);
}

TEST_CASE("capi/abi_version_and_error_strings") {
  CHECK(scan_engine_abi_version() == SCAN_ABI_VERSION);
  CHECK(SCAN_ABI_VERSION == 6u);  // moves with scanengine::kEngineAbiVersion
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

// ===========================================================================
// INT-29: the A10 GNSS/RTK surface, from C
// ===========================================================================

TEST_CASE("capi/gnss_and_ntrip_entry_points_reject_null_handles") {
  scan_gnss_fix fix;
  scan_gnss_stats gstats;
  scan_georef_solution sol;
  scan_ntrip_stats nstats;
  scan_ntrip_config ncfg;
  scan_ntrip_source sources[2];
  std::uint32_t count = 0;
  std::int32_t state = -1;
  const std::uint8_t nmea[] = {'$', 'G', 'N', 'G', 'G', 'A'};

  CHECK(scan_engine_push_nmea(nullptr, 1, nmea, sizeof(nmea), 0) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_last_fix(nullptr, &fix) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_gnss_stats(nullptr, &gstats) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_georef_solution(nullptr, &sol) == SCAN_ERR_INVALID_ARGUMENT);
  // The string accessors cannot fail — they return "", never NULL, so a JNI
  // NewStringUTF() on the result is always safe.
  CHECK(std::string(scan_engine_crs_wkt(nullptr)).empty());
  CHECK(std::string(scan_engine_crs_epsg(nullptr)).empty());

  CHECK(scan_ntrip_create(nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_ntrip_connect(nullptr, &ncfg) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_ntrip_disconnect(nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_ntrip_get_state(nullptr, &state) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_ntrip_get_stats(nullptr, &nstats) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_ntrip_set_rtcm_callback(nullptr, nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_ntrip_fetch_sourcetable(nullptr, sources, 2, &count) == SCAN_ERR_INVALID_ARGUMENT);
  scan_ntrip_destroy(nullptr);  // must be a no-op

  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(nullptr, &e) == SCAN_OK);
  CHECK(scan_engine_last_fix(e, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_gnss_stats(e, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_engine_georef_solution(e, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  // No such device, and no rover at all: an error, not a silent no-op.
  CHECK(scan_engine_push_nmea(e, 99, nmea, sizeof(nmea), 1) == SCAN_ERR_NOT_FOUND);

  // A D6 device is refused rather than being fed NMEA, which would otherwise
  // read as a stream of malformed D6 packets and degrade the wrong device.
  scan_device_config dev;
  std::memset(&dev, 0, sizeof(dev));
  dev.kind = SCAN_DEVICE_D6;
  dev.serial_port_name = "capi";
  std::uint32_t d6 = 0;
  REQUIRE(scan_engine_add_device(e, &dev, &d6) == SCAN_OK);
  CHECK(scan_engine_push_nmea(e, d6, nmea, sizeof(nmea), 1) == SCAN_ERR_INVALID_ARGUMENT);

  // An unconfigured caster is an argument error, and it never touches a socket.
  std::memset(&ncfg, 0, sizeof(ncfg));
  scan_ntrip* client = nullptr;
  REQUIRE(scan_ntrip_create(e, &client) == SCAN_OK);
  REQUIRE(client != nullptr);
  CHECK(scan_ntrip_connect(client, &ncfg) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_ntrip_fetch_sourcetable(&ncfg, sources, 2, &count) == SCAN_ERR_INVALID_ARGUMENT);
  scan_ntrip_destroy(client);
  scan_engine_destroy(e);
}

TEST_CASE("capi/rtk_fixes_and_the_georef_solution_cross_the_abi") {
  scan_engine_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "capi-gnss";
  cfg.log_level = SCAN_LOG_OFF;
  cfg.page_capacity = 4096;
  cfg.max_pages = 4;
  cfg.event_queue_capacity = 4096;

  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(&cfg, &e) == SCAN_OK);

  scan_device_config dev;
  std::memset(&dev, 0, sizeof(dev));
  dev.kind = SCAN_DEVICE_RTK_ROVER;
  std::uint32_t rover = 0;
  REQUIRE(scan_engine_add_device(e, &dev, &rover) == SCAN_OK);

  scan_session_config session;
  std::memset(&session, 0, sizeof(session));
  session.record = 0;
  REQUIRE(scan_engine_start(e, &session) == SCAN_OK);

  // No fix yet is a NORMAL state a status strip renders, not an error.
  scan_gnss_fix fix;
  REQUIRE(scan_engine_last_fix(e, &fix) == SCAN_OK);
  CHECK(fix.fix == SCAN_FIX_NONE);
  CHECK(std::string(scan_engine_crs_wkt(e)).empty());

  scan_georef_solution sol;
  REQUIRE(scan_engine_georef_solution(e, &sol) == SCAN_OK);
  CHECK(sol.converged == 0);

  // Two RTK-Fixed epochs; the first closes when the second arrives.
  static const char* kEpochs[] = {
      "$GNGGA,000000.00,2216.980000,N,11409.510000,E,4,22,0.6,50.00,M,-2.0,M,,*64\r\n"
      "$GNGST,000000.00,0.020,0.020,0.016,0.0,0.020,0.020,0.030*4D\r\n",
      "$GNGGA,000001.00,2216.981000,N,11409.511000,E,4,21,0.7,50.10,M,-2.0,M,,*66\r\n"
      "$GNGST,000001.00,0.020,0.020,0.016,0.0,0.020,0.020,0.030*4C\r\n",
      "$GNGGA,000002.00,2216.982000,N,11409.512000,E,4,20,0.8,50.20,M,-2.0,M,,*68\r\n"};
  std::int64_t t = 5'000'000'000LL;
  for (const char* burst : kEpochs) {
    REQUIRE(scan_engine_push_nmea(e, rover, reinterpret_cast<const std::uint8_t*>(burst),
                                  std::strlen(burst), t) == SCAN_OK);
    t += 1'000'000'000LL;
  }

  REQUIRE(scan_engine_last_fix(e, &fix) == SCAN_OK);
  CHECK(fix.fix == SCAN_FIX_RTK_FIXED);
  CHECK(fix.satellites == 21);
  CHECK(fix.quality_raw == 4);
  CHECK(fix.sigma_from_gst == 1);
  CHECK(fix.sigma_horizontal_m == doctest::Approx(0.02f).epsilon(0.01));
  CHECK(fix.lat_deg == doctest::Approx(22.28301667).epsilon(1e-6));
  CHECK(fix.has_geoid_sep == 1);
  CHECK(fix.height_ellipsoid_m == doctest::Approx(fix.alt_m + fix.geoid_sep_m));
  CHECK(fix.t_mono_ns > 0);

  scan_gnss_stats st;
  REQUIRE(scan_engine_gnss_stats(e, &st) == SCAN_OK);
  CHECK(st.epochs == 2);
  CHECK(st.fixes_published == 2);
  CHECK(st.checksum_failed == 0);
  CHECK(st.checksum_pass_rate == doctest::Approx(1.0));
  CHECK(st.by_fix[SCAN_FIX_RTK_FIXED] == 2);
  CHECK(st.gst_epochs == 2);
  CHECK(st.has_origin == 1);
  CHECK(st.origin_lat_deg == doctest::Approx(22.283).epsilon(1e-6));

  // The fix event, through the union case this ABI version added.
  int fix_events = 0;
  scan_event ev;
  while (scan_engine_poll_event(e, &ev) == SCAN_OK) {
    if (ev.type != SCAN_EVENT_GNSS_FIX) continue;
    ++fix_events;
    CHECK(ev.payload.gnss.fix_type == SCAN_FIX_RTK_FIXED);
    CHECK(ev.payload.gnss.satellites >= 21);
    CHECK(ev.payload.gnss.sigma_h_m == doctest::Approx(0.02f).epsilon(0.01));
    CHECK(ev.payload.gnss.lat_deg > 22.0);
  }
  CHECK(fix_events == 2);

  // Two fixes is far below the convergence floor, so the transform says so
  // rather than inventing one, and the export CRS stays empty with it.
  REQUIRE(scan_engine_georef_solution(e, &sol) == SCAN_OK);
  CHECK(sol.converged == 0);
  CHECK(std::string(scan_engine_crs_wkt(e)).empty());
  CHECK(std::string(scan_engine_crs_epsg(e)).empty());

  CHECK(scan_engine_stop(e) == SCAN_OK);
  scan_engine_destroy(e);
}

TEST_CASE("capi/an_engine_backed_ntrip_handle_shares_the_engines_client") {
  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(nullptr, &e) == SCAN_OK);

  scan_ntrip* a = nullptr;
  scan_ntrip* b = nullptr;
  REQUIRE(scan_ntrip_create(e, &a) == SCAN_OK);
  REQUIRE(scan_ntrip_create(nullptr, &b) == SCAN_OK);  // standalone

  std::int32_t state = -1;
  REQUIRE(scan_ntrip_get_state(a, &state) == SCAN_OK);
  CHECK(state == SCAN_NTRIP_IDLE);
  REQUIRE(scan_ntrip_get_state(b, &state) == SCAN_OK);
  CHECK(state == SCAN_NTRIP_IDLE);

  scan_ntrip_stats st;
  REQUIRE(scan_ntrip_get_stats(a, &st) == SCAN_OK);
  CHECK(st.state == SCAN_NTRIP_IDLE);
  CHECK(st.connect_attempts == 0);
  CHECK(st.receiving == 0);
  // -1, not 0: "no frame yet" and "fresh" are different claims and only one of
  // them is reassuring (docs/A10-gnss.md §3).
  CHECK(st.correction_age_s < 0.0f);

  // Installing and clearing the rover sink is legal on both flavours, and
  // disconnect on a client that never connected is a no-op, not an error.
  CHECK(scan_ntrip_set_rtcm_callback(a, nullptr, nullptr) == SCAN_OK);
  CHECK(scan_ntrip_set_rtcm_callback(b, nullptr, nullptr) == SCAN_OK);
  CHECK(scan_ntrip_disconnect(a) == SCAN_OK);
  CHECK(scan_ntrip_disconnect(b) == SCAN_OK);

  // Destroying the borrowed handle must not take the engine's client with it,
  // nor leave it holding a dangling callback.
  scan_ntrip_destroy(a);
  scan_ntrip* again = nullptr;
  REQUIRE(scan_ntrip_create(e, &again) == SCAN_OK);
  REQUIRE(scan_ntrip_get_state(again, &state) == SCAN_OK);
  CHECK(state == SCAN_NTRIP_IDLE);
  scan_ntrip_destroy(again);
  scan_ntrip_destroy(b);
  scan_engine_destroy(e);
}

// ===========================================================================
// INT-34: ABI 4 — A11's colorization + keyframes, and A15's job progress
// ===========================================================================

TEST_CASE("capi/abi4_entry_points_reject_null_handles") {
  // Same rule every ABI wave follows: an error, never a crash, on a null
  // handle. JNI passes 0 for an unopened engine more often than anyone would
  // like, and a colorizer is a second handle to get wrong.
  CHECK(scan_engine_record_keyframe(nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_create(nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_load_keyframes(nullptr, "x") == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_set_extrinsics(nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_set_progress_callback(nullptr, nullptr, nullptr) ==
        SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_run(nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_cancel(nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_stats(nullptr, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  CHECK(scan_colorizer_progress(nullptr) == 0.f);
  CHECK(scan_clock_sweep_estimate(nullptr, 0, nullptr, 0, nullptr) == SCAN_ERR_INVALID_ARGUMENT);
  scan_colorizer_destroy(nullptr);  // must be a no-op, not a crash

  // A null engine handle with a valid keyframe is still an argument error,
  // and a valid engine with a null image_name is too — the string is the one
  // field with no safe default.
  scan_keyframe kf;
  std::memset(&kf, 0, sizeof(kf));
  CHECK(scan_engine_record_keyframe(nullptr, &kf) == SCAN_ERR_INVALID_ARGUMENT);
}

TEST_CASE("capi/keyframes_cross_the_abi_into_frames_idx") {
  const std::string dir = capi_temp_dir("kf") + "/session.lscan";

  scan_engine_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "capi-keyframes";
  cfg.log_level = SCAN_LOG_OFF;
  cfg.page_capacity = 1024;
  cfg.max_pages = 4;
  scan_engine* e = nullptr;
  REQUIRE(scan_engine_create(&cfg, &e) == SCAN_OK);

  scan_keyframe kf;
  std::memset(&kf, 0, sizeof(kf));
  kf.t_engine_ns = 1'700'000'000'000'000'000LL;
  kf.position[0] = 2.5;
  kf.position[1] = 0.5;
  kf.position[2] = 1.25;
  kf.orientation[3] = 1.0;  // identity, x y z w
  kf.fx = kf.fy = 1200.f;
  kf.cx = 960.f;
  kf.cy = 540.f;
  kf.width = 1920;
  kf.height = 1080;
  kf.rolling_shutter_row_time_ns = 18518.f;
  kf.pose_quality = SCAN_POSE_QUALITY_GOOD;
  kf.pose_source = SCAN_STREAM_POSE_AR;
  kf.flags = SCAN_KEYFRAME_MOTION_VALID | SCAN_KEYFRAME_EXPOSURE_VALID;
  kf.exposure_ns = 5'000'000;
  kf.iso = 400.f;
  kf.angular_rate_rad_s = 0.21f;
  kf.linear_speed_m_s = 0.65f;
  kf.image_bytes = 987654;
  kf.image_name = "kf_000001.jpg";

  // No recording session yet.
  CHECK(scan_engine_record_keyframe(e, &kf) == SCAN_ERR_INVALID_STATE);
  kf.image_name = nullptr;
  CHECK(scan_engine_record_keyframe(e, &kf) == SCAN_ERR_INVALID_ARGUMENT);
  kf.image_name = "kf_000001.jpg";

  scan_session_config session;
  std::memset(&session, 0, sizeof(session));
  session.record = 1;
  session.lscan_dir = dir.c_str();
  REQUIRE(scan_engine_start(e, &session) == SCAN_OK);

  REQUIRE(scan_engine_record_keyframe(e, &kf) == SCAN_OK);
  kf.t_engine_ns += 250'000'000LL;
  kf.image_name = "kf_000002.jpg";
  REQUIRE(scan_engine_record_keyframe(e, &kf) == SCAN_OK);

  // Refused, and not written: a non-unit quaternion cannot be projected, and
  // an escaping name is the zip-slip class inside an index.
  scan_keyframe bad = kf;
  bad.orientation[3] = 0.25;
  CHECK(scan_engine_record_keyframe(e, &bad) == SCAN_ERR_INVALID_ARGUMENT);
  bad = kf;
  bad.image_name = "../../../etc/passwd";
  CHECK(scan_engine_record_keyframe(e, &bad) == SCAN_ERR_INVALID_ARGUMENT);

  CHECK(scan_engine_stop(e) == SCAN_OK);
  scan_engine_destroy(e);

  // Read back with the C++ reader: the C path produces the same records the
  // C++ path does, byte for byte, because it IS the same encoder.
  std::vector<scanengine::Keyframe> back;
  scanengine::color::FrameIndexStats st;
  REQUIRE(scanengine::color::read_frame_index(dir, &back, &st).ok());
  CHECK(st.records == 2);
  CHECK(st.rejected_records == 0);
  REQUIRE(back.size() == 2);
  CHECK(back[0].image_path == "streams/frames/kf_000001.jpg");
  CHECK(back[1].image_path == "streams/frames/kf_000002.jpg");
  CHECK(back[1].intrinsics.width == 1920);
  CHECK(back[1].intrinsics.cx == doctest::Approx(960.f));
  CHECK(back[1].pose.position[0] == doctest::Approx(2.5));
  CHECK(back[1].iso == doctest::Approx(400.f));
  CHECK(back[1].image_bytes == 987654);
  CHECK(back[1].pose.quality == scanengine::PoseQuality::kGood);

  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(dir).parent_path(), ec);
}

TEST_CASE("capi/the_colorizer_handle_refuses_an_unsynchronised_session") {
  scan_engine_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "capi-colorize";
  cfg.log_level = SCAN_LOG_OFF;
  cfg.page_capacity = 256;
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

  // A .lscan with a real frames.idx and no JPEGs. That is enough: the gate
  // refusal below happens BEFORE any image is decoded, which is exactly the
  // property being asserted.
  const std::string dir = capi_temp_dir("cam");
  {
    scanengine::color::KeyframeIndexWriter w;
    REQUIRE(w.open(dir).ok());
    for (int i = 0; i < 2; ++i) {
      scanengine::Keyframe kf;
      kf.t_mono_ns = 12345 + i * 100'000'000LL;
      kf.image_path = "streams/frames/kf_" + std::to_string(i) + ".jpg";
      kf.pose.t_mono_ns = kf.t_mono_ns;
      kf.pose.position[2] = -3.0;
      kf.pose.orientation[3] = 1.0;
      kf.pose.quality = scanengine::PoseQuality::kGood;
      kf.intrinsics.fx = kf.intrinsics.fy = 300.f;
      kf.intrinsics.cx = 160.f;
      kf.intrinsics.cy = 120.f;
      kf.intrinsics.width = 320;
      kf.intrinsics.height = 240;
      REQUIRE(w.add(kf).ok());
    }
    REQUIRE(w.close().ok());
  }

  // A ZERO-INITIALIZED config is the "caller forgot to wire A4" case, and it
  // must REFUSE — docs/A11-color.md §2. This is the single most important
  // property of the whole colorization surface: the alternative is a
  // silently mis-registered cloud.
  scan_colorize_config cc;
  std::memset(&cc, 0, sizeof(cc));
  CHECK(cc.sync_quality == SCAN_SYNC_UNKNOWN);
  scan_colorizer* c = nullptr;
  REQUIRE(scan_colorizer_create(&c, &cc) == SCAN_OK);
  REQUIRE(scan_colorizer_load_keyframes(c, dir.c_str()) == SCAN_OK);
  CHECK(scan_colorizer_run(c, e) == SCAN_ERR_NOT_SUPPORTED);

  scan_colorize_stats cs;
  REQUIRE(scan_colorizer_stats(c, &cs) == SCAN_OK);
  // Refused BEFORE any work: no image decoded, no keyframe considered, no
  // point written (docs/A11-color.md §2's
  // gate/an_unsynchronised_session_is_refused_before_any_work).
  CHECK(cs.points_colorized == 0);
  CHECK(cs.keyframes_used == 0);
  CHECK(cs.keyframes_image_failed == 0);
  CHECK(scan_colorizer_progress(c) == 0.f);

  // The extrinsic is validated even when it is not used: A8 §4.4's
  // column-major-across-JNI trap produces a mirrored cloud nobody notices
  // until export.
  double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CHECK(scan_colorizer_set_extrinsics(c, identity) == SCAN_OK);
  double shear[16] = {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CHECK(scan_colorizer_set_extrinsics(c, shear) != SCAN_OK);

  // A session with no camera at all is kNotFound, not corruption — §3.5's
  // "gracefully unavailable", which a caller reports rather than fails on.
  const std::string empty_dir = capi_temp_dir("nocam");
  CHECK(scan_colorizer_load_keyframes(c, empty_dir.c_str()) == SCAN_ERR_NOT_FOUND);

  CHECK(scan_colorizer_cancel(c) == SCAN_OK);
  CHECK(scan_colorizer_set_progress_callback(c, nullptr, nullptr) == SCAN_OK);
  scan_colorizer_destroy(c);

  // With a real gate the run reaches the pipeline. The JPEGs do not exist, so
  // every keyframe fails to decode — which is counted, not fatal, and leaves
  // every point wearing its intensity-derived colour and flagged uncovered.
  std::memset(&cc, 0, sizeof(cc));
  cc.sync_quality = SCAN_SYNC_GOOD;
  cc.occlusion_test = 1;
  cc.rolling_shutter = 1;
  scan_colorizer* c2 = nullptr;
  REQUIRE(scan_colorizer_create(&c2, &cc) == SCAN_OK);
  REQUIRE(scan_colorizer_load_keyframes(c2, dir.c_str()) == SCAN_OK);

  int progress_calls = 0;
  REQUIRE(scan_colorizer_set_progress_callback(
              c2, [](float, void* u) { ++*static_cast<int*>(u); }, &progress_calls) == SCAN_OK);

  CHECK(scan_colorizer_run(c2, e) == SCAN_OK);
  REQUIRE(scan_colorizer_stats(c2, &cs) == SCAN_OK);
  CHECK(cs.points_total == 81);
  CHECK(cs.points_uncovered == 81);
  CHECK(cs.points_colorized == 0);
  CHECK(cs.keyframes_total == 2);
  CHECK(cs.coverage_fraction == 0.f);
  CHECK(progress_calls > 0);  // the C trampoline fired, no function-pointer cast
  CHECK(scan_colorizer_progress(c2) == doctest::Approx(1.f));

  // A recolour of a real page reaches the C consumer as a POINTS_AVAILABLE
  // event carrying SCAN_PAGE_UPDATE_RECOLOURED — the distinction A14 needs to
  // re-upload a GPU page without re-uploading its geometry, and the ABI half
  // of docs/A11-color.md §8.1.
  int recoloured = 0, appended = 0;
  scan_event ev;
  while (scan_engine_poll_event(e, &ev) == SCAN_OK) {
    if (ev.type != SCAN_EVENT_POINTS_AVAILABLE) continue;
    if (ev.payload.points.update_kind == SCAN_PAGE_UPDATE_RECOLOURED) ++recoloured;
    if (ev.payload.points.update_kind == SCAN_PAGE_UPDATE_APPENDED) ++appended;
  }
  CHECK(appended > 0);
  CHECK(recoloured > 0);

  scan_colorizer_destroy(c2);
  CHECK(scan_engine_stop(e) == SCAN_OK);
  scan_engine_destroy(e);

  std::error_code ec;
  std::filesystem::remove_all(empty_dir, ec);
  std::filesystem::remove_all(dir, ec);
}

TEST_CASE("capi/the_wizard_clock_sweep_crosses_the_abi") {
  // WIZARD.md screen 3: ~1 sweep per second for 8 s, both sensors watching the
  // same motion. The camera track is the lidar's, shifted and scaled — the
  // normalised correlation removes the scale, which is what makes the
  // estimator signal-agnostic.
  const std::int64_t truth_ns = 23'000'000;  // camera reads 23 ms early
  std::vector<scan_rate_sample> cam, lid;
  for (int i = 0; i < 240; ++i) {  // 30 Hz for 8 s
    const std::int64_t t = static_cast<std::int64_t>(i) * 33'333'333LL;
    scan_rate_sample s;
    s.t_ns = t - truth_ns;  // on the camera's own clock
    s.value = std::sin(2.0 * 3.14159265358979 * 1.0 * static_cast<double>(t) * 1e-9);
    cam.push_back(s);
  }
  for (int i = 0; i < 1600; ++i) {  // 200 Hz for 8 s
    const std::int64_t t = static_cast<std::int64_t>(i) * 5'000'000LL;
    scan_rate_sample s;
    s.t_ns = t;
    s.value = 1.7 * std::sin(2.0 * 3.14159265358979 * 1.0 * static_cast<double>(t) * 1e-9);
    lid.push_back(s);
  }

  scan_clock_sweep_result r;
  REQUIRE(scan_clock_sweep_estimate(cam.data(), static_cast<std::uint32_t>(cam.size()),
                                    lid.data(), static_cast<std::uint32_t>(lid.size()),
                                    &r) == SCAN_OK);
  CHECK(r.accepted == 1);
  CHECK(r.verdict == SCAN_SWEEP_ACCEPTED);
  CHECK(r.correlation > 0.9);
  CHECK(std::llabs(r.offset_ns - truth_ns) < 1'000'000);  // the +-1 ms target
  CHECK(r.zero_crossings >= 4);
  MESSAGE("clock sweep over the ABI: " << r.offset_ns << " ns (truth " << truth_ns
                                       << "), sigma " << r.sigma_ns << " ns");

  // A capture that did not move is a VERDICT, not an error: the wizard has to
  // tell the operator what went wrong, and SCAN_OK + accepted == 0 is how.
  std::vector<scan_rate_sample> flat = lid;
  for (auto& s : flat) s.value = 1.0;
  scan_clock_sweep_result r2;
  CHECK(scan_clock_sweep_estimate(cam.data(), static_cast<std::uint32_t>(cam.size()),
                                  flat.data(), static_cast<std::uint32_t>(flat.size()),
                                  &r2) == SCAN_OK);
  CHECK(r2.accepted == 0);
  CHECK(r2.verdict == SCAN_SWEEP_NO_MOTION);

  // Structurally bad input IS an error.
  scan_clock_sweep_result r3;
  CHECK(scan_clock_sweep_estimate(nullptr, 5, lid.data(),
                                  static_cast<std::uint32_t>(lid.size()),
                                  &r3) == SCAN_ERR_INVALID_ARGUMENT);
}

TEST_CASE("capi/job_progress_no_longer_crosses_the_abi_as_zeroed_bytes") {
  // docs/A15-jobs.md §7.1: SCAN_EVENT_JOB_PROGRESS existed and was
  // static_assert'ed since A1, but convert_event() had no case and
  // scan_event had no `job` member, so every job-progress event reached a C
  // consumer with a zeroed payload. ABI 4 closes it, and this is the case
  // that would fail if the union member were ever dropped again.
  scanengine::Event in;
  in.type = scanengine::EventType::kJobProgress;
  in.sequence = 77;
  in.t_mono_ns = 4242;
  in.payload.job.job_id = 0x0123456789ABCDEFull;
  in.payload.job.progress = 0.625f;
  in.payload.job.state = static_cast<std::uint8_t>(scanengine::jobs::JobState::kRunning);

  scan_event out;
  scanengine::capi_test::convert_event_for_test(in, &out);
  CHECK(out.type == SCAN_EVENT_JOB_PROGRESS);
  CHECK(out.sequence == 77);
  CHECK(out.payload.job.job_id == 0x0123456789ABCDEFull);
  CHECK(out.payload.job.progress == doctest::Approx(0.625f));
  CHECK(out.payload.job.state == SCAN_JOB_RUNNING);
}
