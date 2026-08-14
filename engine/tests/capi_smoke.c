/* capi_smoke.c — the C ABI exercised from ACTUAL C.
 *
 * Compiled as C11 (see CMakeLists: this file's language is C, and the
 * standalone scanengine_capi_smoke target is a pure-C executable). That is
 * the point: if scanengine_c.h ever grows a C++-ism — a default argument, a
 * namespace, `bool` without stdbool, an enum class — this file stops
 * compiling, which is exactly the failure JNI would otherwise hit at
 * integration time.
 *
 * Owner: A1. B-workstream JNI code should read this as the reference call
 * sequence.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "scanengine_c.h"

#define SCAN_SMOKE_BOARD_PTS 64

static int g_events_seen;
static int g_points_events;
static int g_log_lines;

static void on_event(const scan_event* ev, void* user) {
  (void)user;
  ++g_events_seen;
  if (ev->type == SCAN_EVENT_POINTS_AVAILABLE) ++g_points_events;
}

static void on_log(int32_t level, const char* module, const char* message, void* user) {
  (void)level;
  (void)module;
  (void)message;
  (void)user;
  ++g_log_lines;
}

static scan_error_t on_serial_write(const uint8_t* data, size_t len, void* user) {
  unsigned char* first = (unsigned char*)user;
  if (len > 0) *first = data[0];
  return SCAN_OK;
}

static void normalize3(double v[3]) {
  const double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (n <= 0.0) return;
  v[0] /= n;
  v[1] /= n;
  v[2] /= n;
}

/* Returns 0 on success, otherwise the number of the step that failed.
 * `d6_bytes` may be NULL: the point-decoding steps are then skipped. */
int scan_capi_smoke_run(const uint8_t* d6_bytes, size_t d6_len) {
  scan_engine_config cfg;
  scan_session_config session;
  scan_device_config device;
  scan_engine* engine = NULL;
  scan_device_health health;
  scan_point_page page;
  scan_event ev;
  scan_pose pose;
  scan_pose pose_out;
  scan_pushbroom_stats pb;
  scan_mount_calib* calib = NULL;
  scan_mount_calib_result result;
  scan_point_vertex board[SCAN_SMOKE_BOARD_PTS];
  const uint32_t kBoardPts = SCAN_SMOKE_BOARD_PTS;
  /* Identity: the smoke test's bracket puts the lidar exactly at the camera,
   * which makes the expected solve answer checkable without a fixture. */
  const double mount[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  /* A scaled matrix: rigid check must reject it (this is what a column-major
   * or uninitialised buffer looks like from here). */
  const double not_rigid[16] = {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const double bad_normal[3] = {0.0, 0.0, 0.5}; /* not unit length */
  uint32_t device_id = 0;
  uint32_t page_count = 0;
  uint32_t page_id = 0;
  uint64_t total_points = 0;
  int32_t state = -1;
  unsigned char first_written = 0;
  uint8_t gate = 0;
  uint32_t i = 0;
  uint32_t k = 0;
  scan_error_t err;

  g_events_seen = 0;
  g_points_events = 0;
  g_log_lines = 0;

  if (scan_engine_abi_version() != SCAN_ABI_VERSION) return 1;
  if (scan_engine_version_string() == NULL) return 2;
  if (strcmp(scan_error_str(SCAN_ERR_TIMEOUT), "timeout") != 0) return 3;

  scan_engine_set_log_callback(on_log, NULL, SCAN_LOG_INFO);

  memset(&cfg, 0, sizeof(cfg));
  cfg.app_name = "capi-smoke";
  cfg.log_level = SCAN_LOG_INFO;
  cfg.page_capacity = 4096;
  cfg.max_pages = 8;
  cfg.event_queue_capacity = 256;

  err = scan_engine_create(&cfg, &engine);
  if (err != SCAN_OK || engine == NULL) return 4;

  if (scan_engine_state(engine, &state) != SCAN_OK || state != SCAN_ENGINE_IDLE) return 5;

  /* Null-argument handling must be an error, never a crash. */
  if (scan_engine_state(engine, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 6;
  if (scan_engine_last_error() == NULL) return 7;

  memset(&device, 0, sizeof(device));
  device.kind = SCAN_DEVICE_D6;
  device.serial_port_name = "capi-test";
  device.serial_baud = 230400;
  device.serial_write = on_serial_write;
  device.serial_write_user_data = &first_written;
  device.send_start_stop_commands = 1;

  if (scan_engine_add_device(engine, &device, &device_id) != SCAN_OK) return 8;
  if (device_id == 0) return 9;

  memset(&session, 0, sizeof(session));
  session.profile = "quickscan";
  session.record = 0;
  if (scan_engine_start(engine, &session) != SCAN_OK) return 10;
  if (scan_engine_state(engine, &state) != SCAN_OK || state != SCAN_ENGINE_RUNNING) return 11;
  if (first_written != 0xAA) return 12; /* start command reached the app */

  /* Poll: at minimum the engine-state and session-state events are queued. */
  if (scan_engine_poll_event(engine, &ev) != SCAN_OK) return 13;
  if (ev.type != SCAN_EVENT_ENGINE_STATE) return 14;
  if (ev.sequence == 0) return 15;

  if (d6_bytes != NULL && d6_len > 0) {
    if (scan_engine_push_serial_bytes(engine, device_id, d6_bytes, d6_len, 0) != SCAN_OK) {
      return 16;
    }
    if (scan_engine_total_points(engine, &total_points) != SCAN_OK || total_points == 0) {
      return 17;
    }
    if (scan_engine_page_count(engine, &page_count) != SCAN_OK || page_count == 0) return 18;
    if (scan_engine_page_id_at(engine, 0, &page_id) != SCAN_OK) return 19;
    if (scan_engine_page_id_at(engine, page_count, &page_id) != SCAN_ERR_NOT_FOUND) return 20;
    if (scan_engine_page_id_at(engine, 0, &page_id) != SCAN_OK) return 21;

    memset(&page, 0, sizeof(page));
    if (scan_engine_get_point_page(engine, page_id, &page) != SCAN_OK) return 22;
    if (page.data == NULL || page.count == 0) return 23;
    if (page.stream != SCAN_STREAM_LIDAR_D6) return 24;
    if (sizeof(page.data[0]) != 16) return 25; /* the S3 GPU layout */
    if (scan_engine_get_point_page(engine, 999999u, &page) != SCAN_ERR_NOT_FOUND) return 26;

    if (scan_engine_device_health(engine, device_id, &health) != SCAN_OK) return 27;
    if (health.state != SCAN_DEV_STREAMING) return 28;
    if (health.points_out == 0) return 29;
    if (health.checksum_pass_rate < 0.99) return 30;
  }

  /* --- poses (A8) --------------------------------------------------------
   *
   * The ARCore call sequence, from C: push a short trajectory, then ask for a
   * pose between two of them and check the gate. */
  memset(&pose, 0, sizeof(pose));
  pose.orientation[3] = 1.0;
  pose.source = SCAN_STREAM_POSE_AR;
  pose.quality = SCAN_POSE_QUALITY_GOOD;

  /* Before any pose exists, the gate says so and nothing crashes. */
  gate = 0xFF;
  if (scan_engine_pose_at(engine, 1000, &pose_out, &gate) != SCAN_ERR_NOT_FOUND) return 39;
  if (gate != SCAN_POSE_GATE_NO_DATA) return 40;

  for (i = 0; i < 5; ++i) {
    pose.t_mono_ns = 1000000000LL + (int64_t)i * 33000000LL; /* ~30 Hz */
    pose.position[0] = 0.1 * (double)i;
    if (scan_engine_push_pose(engine, &pose, -1.0f) != SCAN_OK) return 41; /* derive confidence */
  }
  /* Out of order is rejected, not silently reordered. */
  pose.t_mono_ns = 1000000000LL;
  if (scan_engine_push_pose(engine, &pose, 0.9f) != SCAN_ERR_INVALID_ARGUMENT) return 42;

  gate = 0xFF;
  if (scan_engine_pose_at(engine, 1000000000LL + 16500000LL, &pose_out, &gate) != SCAN_OK) {
    return 43;
  }
  if (gate != SCAN_POSE_GATE_OK) return 44;
  if (pose_out.position[0] < 0.04 || pose_out.position[0] > 0.06) return 45; /* lerped to 0.05 */
  if (pose_out.orientation[3] < 0.999) return 46;
  /* Past the newest pose: "ask me again later", not an error. */
  if (scan_engine_pose_at(engine, 2000000000LL, &pose_out, &gate) != SCAN_ERR_AGAIN) return 47;
  if (gate != SCAN_POSE_GATE_FUTURE) return 48;

  /* --- pushbroom (A8) ---------------------------------------------------- */
  if (scan_engine_pushbroom_enable(engine, 1) != SCAN_ERR_INVALID_STATE) return 49; /* no mount */
  /* A column-major matrix (the classic JNI trap) is not rigid: rejected. */
  if (scan_engine_set_mount_extrinsics(engine, not_rigid) != SCAN_ERR_INVALID_ARGUMENT) return 50;
  if (scan_engine_set_mount_extrinsics(engine, mount) != SCAN_OK) return 51;
  if (scan_engine_pushbroom_enable(engine, 1) != SCAN_OK) return 52;
  if (scan_engine_pushbroom_flush(engine) != SCAN_OK) return 53;
  if (scan_engine_pushbroom_stats(engine, &pb) != SCAN_OK) return 54;
  if (pb.points_pending != 0) return 55;
  if (scan_engine_pushbroom_enable(engine, 0) != SCAN_OK) return 56;

  /* Push mode: installing a callback discards the queue and delivers inline. */
  if (scan_engine_set_event_callback(engine, on_event, NULL) != SCAN_OK) return 31;
  if (scan_engine_stop(engine) != SCAN_OK) return 32;
  if (g_events_seen == 0) return 33;

  if (scan_engine_set_event_callback(engine, NULL, NULL) != SCAN_OK) return 34;
  if (scan_engine_poll_event(engine, &ev) != SCAN_ERR_AGAIN) return 35; /* empty, not an error */

  if (scan_engine_device_health(engine, 4242u, &health) != SCAN_ERR_NOT_FOUND) return 36;
  if (scan_engine_last_error()[0] == '\0') return 37;

  scan_engine_destroy(engine);
  scan_engine_set_log_callback(NULL, NULL, SCAN_LOG_INFO);
  if (g_log_lines == 0) return 38;

  scan_engine_destroy(NULL); /* must be a no-op */

  /* --- mount calibration (A8) --------------------------------------------
   *
   * Engine-free by design: the observations come from the app's checkerboard
   * detection, so the wizard can solve with no session running. */
  if (scan_mount_calib_create(&calib) != SCAN_OK || calib == NULL) return 57;
  /* Non-unit normal and non-positive sigma are refused. */
  if (scan_mount_calib_add_observation(calib, bad_normal, 1.0, board, kBoardPts, 0.02) !=
      SCAN_ERR_INVALID_ARGUMENT) {
    return 58;
  }
  for (i = 0; i < 6; ++i) {
    double n[3];
    double d;
    /* Six board orientations, tilted about two axes so the six unknowns are
     * actually constrained (WIZARD.md: roll variation is load-bearing). */
    n[0] = 0.30 * (double)(i % 3) - 0.30;
    n[1] = 0.30 * (double)(i / 3) - 0.15;
    n[2] = -1.0;
    normalize3(n);
    d = 1.2 + 0.1 * (double)i;
    for (k = 0; k < kBoardPts; ++k) {
      /* A grid on the plane, expressed in the lidar frame, which for this
       * smoke test IS the camera frame (identity ground truth). */
      double u = 0.05 * (double)(k % 8) - 0.175;
      double v = 0.05 * (double)(k / 8) - 0.175;
      double t;
      board[k].x = (float)u;
      board[k].y = (float)v;
      /* Solve n . (u, v, z) = d for z. */
      t = (d - n[0] * u - n[1] * v) / n[2];
      board[k].z = (float)t;
      board[k].r = board[k].g = board[k].b = 128;
      board[k].a = 255;
    }
    if (scan_mount_calib_add_observation(calib, n, d, board, kBoardPts, 0.02) != SCAN_OK) {
      return 59;
    }
  }
  if (scan_mount_calib_solve(calib, mount, &result) != SCAN_OK) return 60;
  if (!result.converged) return 61;
  if (result.degenerate) return 62;
  if (result.observations != 6) return 63;
  if (result.gate != SCAN_CALIB_GATE_GOOD) return 64; /* noise-free data */
  if (result.split_half_px < 0.0) return 65;
  /* Identity ground truth: the solve must come back to it. */
  if (result.camera_from_lidar[3] > 0.01 || result.camera_from_lidar[3] < -0.01) return 66;
  if (result.camera_from_lidar[0] < 0.999) return 67;
  scan_mount_calib_destroy(calib);
  scan_mount_calib_destroy(NULL); /* must be a no-op */

  return 0;
}

#ifdef SCAN_CAPI_SMOKE_MAIN
int main(void) {
  const int rc = scan_capi_smoke_run(NULL, 0);
  if (rc != 0) {
    printf("C ABI smoke test FAILED at step %d: %s\n", rc, scan_engine_last_error());
    return 1;
  }
  printf("C ABI smoke test passed (%s)\n", scan_engine_version_string());
  return 0;
}
#endif
