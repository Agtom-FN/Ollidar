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

static int g_rtcm_frames;

/* The rover write. In an app this is bluetoothSocket.write(); here it just
 * counts, because the point is that the signature is callable from C. */
static void on_rtcm(const uint8_t* data, size_t len, void* user) {
  (void)data;
  (void)user;
  if (len > 0) ++g_rtcm_frames;
}

/* NMEA 0183 checksum: XOR of everything between '$' and '*'. Computed here
 * rather than hard-coded so a mistyped sentence cannot pass silently. */
static void nmea_gga(char* out, size_t cap, int second) {
  char body[192];
  unsigned char cs = 0;
  size_t i;
  snprintf(body, sizeof(body),
           "GNGGA,0000%02d.00,2216.98%04d,N,11409.51%04d,E,4,22,0.6,50.00,M,-2.0,M,,",
           second, second, second);
  for (i = 0; body[i] != '\0'; ++i) cs = (unsigned char)(cs ^ (unsigned char)body[i]);
  snprintf(out, cap, "$%s*%02X\r\n", body, cs);
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
  /* --- A10 (INT-29) ------------------------------------------------------ */
  scan_gnss_fix fix;
  scan_gnss_stats gnss;
  scan_georef_solution georef;
  scan_ntrip* ntrip = NULL;
  scan_ntrip_config ntrip_cfg;
  scan_ntrip_stats ntrip_st;
  scan_ntrip_source sources[4];
  uint32_t source_count = 0;
  int32_t ntrip_state = -1;
  uint32_t rover_id = 0;
  char burst[256];

  g_events_seen = 0;
  g_points_events = 0;
  g_log_lines = 0;
  g_rtcm_frames = 0;

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

  /* --- GNSS / RTK (A10) ---------------------------------------------------
   *
   * The rover call sequence, from C: add the device, push its NMEA, read the
   * fix and the timeline, and ask for the export CRS. */
  memset(&device, 0, sizeof(device));
  device.kind = SCAN_DEVICE_RTK_ROVER;
  if (scan_engine_add_device(engine, &device, &rover_id) != SCAN_OK) return 68;
  if (rover_id == 0) return 69;

  /* Nothing pushed yet: "no fix" is a state, not an error. */
  if (scan_engine_last_fix(engine, &fix) != SCAN_OK) return 70;
  if (fix.fix != SCAN_FIX_NONE) return 71;
  if (scan_engine_crs_wkt(engine) == NULL) return 72;
  if (scan_engine_crs_wkt(engine)[0] != '\0') return 73; /* nothing to label yet */

  /* A D6 device is not a rover, and is refused rather than fed NMEA. */
  if (scan_engine_push_nmea(engine, device_id, (const uint8_t*)"$GNGGA\r\n", 8, 1) !=
      SCAN_ERR_INVALID_ARGUMENT) {
    return 74;
  }

  /* Three epochs; each closes when the next arrives. Built with nmea_checksum()
   * above rather than hard-coded, so a mistyped digit cannot pass silently. */
  for (i = 0; i < 3; ++i) {
    nmea_gga(burst, sizeof(burst), (int)i);
    if (scan_engine_push_nmea(engine, rover_id, (const uint8_t*)burst, strlen(burst),
                              5000000000LL + (int64_t)i * 1000000000LL) != SCAN_OK) {
      return 75;
    }
  }
  if (scan_engine_last_fix(engine, &fix) != SCAN_OK) return 76;
  if (fix.fix != SCAN_FIX_RTK_FIXED) return 77;
  if (fix.satellites != 22) return 78;
  if (fix.t_mono_ns <= 0) return 79;
  /* alt_m is orthometric and height_ellipsoid_m is what the geodesy uses: the
   * two must differ by exactly the geoid separation the receiver reported. */
  if (fix.has_geoid_sep == 0) return 80;
  if (fabs(fix.height_ellipsoid_m - (fix.alt_m + fix.geoid_sep_m)) > 1e-9) return 81;

  if (scan_engine_gnss_stats(engine, &gnss) != SCAN_OK) return 82;
  if (gnss.epochs != 2) return 83;
  if (gnss.by_fix[SCAN_FIX_RTK_FIXED] != 2) return 84;
  if (gnss.checksum_failed != 0) return 85;
  if (gnss.has_origin == 0) return 86;

  /* Two fixes cannot converge a 4-parameter transform, and the solution says so
   * instead of inventing one. */
  if (scan_engine_georef_solution(engine, &georef) != SCAN_OK) return 87;
  if (georef.converged != 0) return 88;
  if (scan_engine_crs_epsg(engine)[0] != '\0') return 89;

  /* --- NTRIP (A10) -------------------------------------------------------- */
  if (scan_ntrip_create(engine, &ntrip) != SCAN_OK || ntrip == NULL) return 90;
  if (scan_ntrip_get_state(ntrip, &ntrip_state) != SCAN_OK) return 91;
  if (ntrip_state != SCAN_NTRIP_IDLE) return 92;
  if (scan_ntrip_get_stats(ntrip, &ntrip_st) != SCAN_OK) return 93;
  if (ntrip_st.connect_attempts != 0) return 94;
  if (ntrip_st.correction_age_s >= 0.0f) return 95; /* -1 = unknown, not fresh */
  if (scan_ntrip_set_rtcm_callback(ntrip, on_rtcm, NULL) != SCAN_OK) return 96;
  /* An unconfigured caster is refused without touching a socket. */
  memset(&ntrip_cfg, 0, sizeof(ntrip_cfg));
  if (scan_ntrip_connect(ntrip, &ntrip_cfg) != SCAN_ERR_INVALID_ARGUMENT) return 97;
  if (scan_ntrip_fetch_sourcetable(&ntrip_cfg, sources, 4, &source_count) !=
      SCAN_ERR_INVALID_ARGUMENT) {
    return 98;
  }
  if (source_count != 0) return 99;
  if (scan_ntrip_disconnect(ntrip) != SCAN_OK) return 100; /* never connected: a no-op */
  if (scan_ntrip_set_rtcm_callback(ntrip, NULL, NULL) != SCAN_OK) return 101;
  scan_ntrip_destroy(ntrip);
  ntrip = NULL;
  /* Nothing connected, so nothing was ever forwarded to the rover. */
  if (g_rtcm_frames != 0) return 102;

  /* Push mode: installing a callback discards the queue and delivers inline. */
  if (scan_engine_set_event_callback(engine, on_event, NULL) != SCAN_OK) return 31;
  if (scan_engine_stop(engine) != SCAN_OK) return 32;
  if (g_events_seen == 0) return 33;

  if (scan_engine_set_event_callback(engine, NULL, NULL) != SCAN_OK) return 34;
  if (scan_engine_poll_event(engine, &ev) != SCAN_ERR_AGAIN) return 35; /* empty, not an error */

  if (scan_engine_device_health(engine, 4242u, &health) != SCAN_ERR_NOT_FOUND) return 36;
  if (scan_engine_last_error()[0] == '\0') return 37;

  /* --- ABI 4 (INT-34): the keyframe write path (B8), while the engine is
   * still alive. No recording session is open in this smoke run, so this is
   * the honest refusal rather than a write; what is being proved is that
   * scan_keyframe is fillable and the call linkable from C11. */
  {
    scan_keyframe kf;
    memset(&kf, 0, sizeof kf);
    kf.t_engine_ns = 1700000000000000000LL;
    kf.orientation[3] = 1.0; /* x, y, z, w */
    kf.fx = 1200.0f;
    kf.fy = 1200.0f;
    kf.cx = 960.0f;
    kf.cy = 540.0f;
    kf.width = 1920;
    kf.height = 1080;
    kf.pose_quality = SCAN_POSE_QUALITY_GOOD;
    kf.pose_source = SCAN_STREAM_POSE_AR;
    kf.flags = SCAN_KEYFRAME_MOTION_VALID;
    kf.angular_rate_rad_s = 0.1f;
    kf.image_name = "kf_000001.jpg";
    if (scan_engine_record_keyframe(engine, &kf) != SCAN_ERR_INVALID_STATE) return 103;
    if (scan_engine_record_keyframe(NULL, &kf) != SCAN_ERR_INVALID_ARGUMENT) return 104;
    if (scan_engine_record_keyframe(engine, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 105;
    kf.image_name = NULL;
    if (scan_engine_record_keyframe(engine, &kf) != SCAN_ERR_INVALID_ARGUMENT) return 106;

    /* The colorizer is a SECOND handle, and it runs against the engine's page
     * store — so its lifecycle is exercised here, while there is one. */
    {
      scan_colorize_config cc;
      scan_colorizer* col = NULL;
      memset(&cc, 0, sizeof cc);
      /* A zero-initialized config FAILS CLOSED (SCAN_SYNC_UNKNOWN): a caller
       * who forgets the sync gate must be refused, never silently trusted. */
      if (cc.sync_quality != SCAN_SYNC_UNKNOWN) return 107;
      if (scan_colorizer_create(&col, &cc) != SCAN_OK) return 108;
      if (col == NULL) return 109;
      if (scan_colorizer_create(NULL, &cc) != SCAN_ERR_INVALID_ARGUMENT) return 110;
      /* No camera in this session: kNotFound, §3.5's "gracefully
       * unavailable", which a UI reports rather than fails on. */
      if (scan_colorizer_load_keyframes(col, ".") != SCAN_ERR_NOT_FOUND) return 111;
      {
        /* Either "no keyframes" (this session has no camera) or "no points"
         * (this smoke run was given no D6 bytes to decode). Both are refusals
         * with a real code; what must never happen is SCAN_OK or a crash. */
        const scan_error_t rc = scan_colorizer_run(col, engine);
        if (rc != SCAN_ERR_NOT_FOUND && rc != SCAN_ERR_INVALID_ARGUMENT) return 112;
      }
      scan_colorizer_destroy(col);
      scan_colorizer_destroy(NULL); /* must be a no-op */
    }
  }

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

  /* --- ABI 4 (INT-34): the wizard's clock sweep (B7) ---------------------
   *
   * Engine-free, like the mount calibration above and for the same reason:
   * the two tracks come from the app's own detection. */
  {
    scan_clock_sweep_result sweep;
    scan_rate_sample cam[240];
    scan_rate_sample lid[1600];
    scan_colorizer* col2 = NULL;
    scan_colorize_config cc2;
    scan_colorize_stats cstats;
    double ident[16];
    const int64_t truth_ns = 23000000;

    /* A colorizer with no engine in sight: the handle is standalone. */
    memset(&cc2, 0, sizeof cc2);
    cc2.sync_quality = SCAN_SYNC_GOOD;
    if (scan_colorizer_create(&col2, &cc2) != SCAN_OK) return 113;
    for (i = 0; i < 16; ++i) ident[i] = 0.0;
    ident[0] = ident[5] = ident[10] = ident[15] = 1.0;
    if (scan_colorizer_set_extrinsics(col2, ident) != SCAN_OK) return 114;
    if (scan_colorizer_set_progress_callback(col2, NULL, NULL) != SCAN_OK) return 115;
    if (scan_colorizer_cancel(col2) != SCAN_OK) return 116;
    if (scan_colorizer_stats(col2, &cstats) != SCAN_OK) return 117;
    if (cstats.points_colorized != 0) return 118;
    if (scan_colorizer_progress(col2) != 0.0f) return 119;
    scan_colorizer_destroy(col2);

    /* 8 s at ~1 Hz, camera at 30 Hz against lidar at 200 Hz, the camera track
     * shifted by the truth and scaled by 1.7 so the normalised correlation
     * has something to normalise away. */
    for (i = 0; i < 240; ++i) {
      int64_t t = (int64_t)i * 33333333LL;
      cam[i].t_ns = t - truth_ns;
      cam[i].value = sin(2.0 * 3.14159265358979 * (double)t * 1e-9);
    }
    for (i = 0; i < 1600; ++i) {
      int64_t t = (int64_t)i * 5000000LL;
      lid[i].t_ns = t;
      lid[i].value = 1.7 * sin(2.0 * 3.14159265358979 * (double)t * 1e-9);
    }
    memset(&sweep, 0, sizeof sweep);
    if (scan_clock_sweep_estimate(cam, 240u, lid, 1600u, &sweep) != SCAN_OK) return 120;
    if (!sweep.accepted) return 121;
    if (sweep.verdict != SCAN_SWEEP_ACCEPTED) return 122;
    if (sweep.correlation < 0.9) return 123;
    if (sweep.offset_ns - truth_ns > 1000000LL) return 124;  /* the +-1 ms target */
    if (truth_ns - sweep.offset_ns > 1000000LL) return 125;
    if (scan_clock_sweep_estimate(cam, 240u, lid, 1600u, NULL) != SCAN_ERR_INVALID_ARGUMENT) {
      return 126;
    }
    if (scan_clock_sweep_estimate(NULL, 5u, lid, 1600u, &sweep) != SCAN_ERR_INVALID_ARGUMENT) {
      return 127;
    }
  }

  /* --- ABI 5 (INT-FINAL): the Android capture seam ------------------------
   *
   * The reference call sequence for android/NOTES.md §8 finding 1: the app
   * binds its own sockets to the USB-Ethernet Network, hands BOTH descriptors
   * down, and the capture session — the same scan_engine* everything else
   * runs on — receives points AND IMU through them. That is the whole thing
   * B3 could not do and had to work around with a standalone C++ engine.
   *
   * The descriptors here are deliberately bogus: add_device does not open
   * anything (no session is running), and a smoke test must not depend on a
   * network. The point is that the FIELDS exist and convert. */
  {
    scan_device_config mid;
    scan_engine_config c5;
    scan_engine* e5 = NULL;
    uint32_t mid_id = 0;

    /* A fresh engine: the one above was destroyed at the end of the ABI-3
     * block, and these two ABI-5 surfaces need no session. */
    memset(&c5, 0, sizeof c5);
    c5.app_name = "capi-smoke-abi5";
    c5.log_level = SCAN_LOG_WARN;
    if (scan_engine_create(&c5, &e5) != SCAN_OK || e5 == NULL) return 128;

    memset(&mid, 0, sizeof mid);
    mid.kind = SCAN_DEVICE_MID360;
    mid.lidar_ip = "192.168.1.100";
    mid.host_ip = "192.168.1.5";
    mid.mid360_backend = SCAN_MID360_BACKEND_RAW_UDP;
    mid.mid360_prebound_fd = 1001;      /* Os.socket() + Network.bindSocket() */
    mid.mid360_prebound_imu_fd = 1002;  /* the SECOND one: finding 2 */
    mid.mid360_host_point_port = 56301;
    mid.mid360_host_imu_port = 56401;
    mid.mid360_recv_buffer_bytes = 4 * 1024 * 1024;
    mid.mid360_live_points_per_sec_set = 1;
    mid.mid360_live_points_per_sec = 40000;
    mid.mid360_publish_imu_set = 1;
    mid.mid360_publish_imu = 1;
    mid.mid360_filter_set = 1;
    mid.mid360_drop_no_return = 1;
    mid.mid360_min_range_m = 0.1f;
    mid.mid360_sdk_config_path = "/data/user/0/app/cache/mid360.json"; /* finding 4 */
    if (scan_engine_add_device(e5, &mid, &mid_id) != SCAN_OK) return 129;
    if (scan_engine_remove_device(e5, mid_id) != SCAN_OK) return 130;

    /* A pre-bound descriptor cannot reach SDK2 (finding 3), and saying so at
     * add_device is the difference between a clear message and a bench
     * session spent wondering why the seam does nothing. */
    mid.mid360_backend = SCAN_MID360_BACKEND_SDK2;
    if (scan_engine_add_device(e5, &mid, &mid_id) != SCAN_ERR_INVALID_ARGUMENT) return 131;

    /* A zeroed Mid-360 config is still exactly the ABI-4 device. */
    memset(&mid, 0, sizeof mid);
    mid.kind = SCAN_DEVICE_MID360;
    mid.lidar_ip = "192.168.1.100";
    mid.host_ip = "192.168.1.5";
    if (scan_engine_add_device(e5, &mid, &mid_id) != SCAN_OK) return 132;

    /* The Mid-360's OWN counters (finding 5). Nothing has streamed, so what is
     * asserted is that the question is ASKABLE and that the wrong device kind
     * is refused rather than silently zeroed. */
    {
      scan_mid360_stats ms;
      memset(&ms, 0xAB, sizeof ms);
      if (scan_engine_mid360_stats(e5, mid_id, &ms) != SCAN_OK) return 133;
      if (ms.link != SCAN_MID360_LINK_DOWN) return 134;
      if (ms.forced_reinits != 0 || ms.watchdog_trips != 0) return 135;
      if (ms.device_sn[0] != '\0') return 136;
      if (scan_engine_mid360_stats(e5, 4242u, &ms) != SCAN_ERR_NOT_FOUND) return 137;
      if (scan_engine_mid360_stats(e5, mid_id, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 138;
    }
    if (scan_engine_remove_device(e5, mid_id) != SCAN_OK) return 139;
    scan_engine_destroy(e5);
  }

  /* --- ABI 5 (INT-FINAL): the CRS escape hatch ---------------------------- */
  {
    scan_engine_config c6;
    scan_engine* e6 = NULL;
    /* EPSG:2326 is Hong Kong 1980 Grid — a national grid the engine has no
     * table entry for, which is exactly why the WKT has to come from here. */
    static const char* const hk_wkt =
        "PROJCS[\"Hong Kong 1980 Grid System\",GEOGCS[\"Hong Kong 1980\","
        "DATUM[\"Hong_Kong_1980\",SPHEROID[\"International 1924\",6378388,297]],"
        "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
        "PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"scale_factor\",1],"
        "UNIT[\"metre\",1],AUTHORITY[\"EPSG\",\"2326\"]]";
    memset(&c6, 0, sizeof c6);
    c6.app_name = "capi-smoke-crs";
    c6.log_level = SCAN_LOG_WARN;
    if (scan_engine_create(&c6, &e6) != SCAN_OK || e6 == NULL) return 140;
    if (scan_engine_set_crs(e6, "EPSG:2326", hk_wkt) != SCAN_OK) return 141;
    /* The code alone is refused: the engine cannot render a WKT for it, and
     * an unlabelled export is the failure this call exists to prevent. */
    if (scan_engine_set_crs(e6, "EPSG:2326", "") != SCAN_ERR_INVALID_ARGUMENT) return 142;
    /* A UTM zone it CAN render needs no WKT. */
    if (scan_engine_set_crs(e6, "EPSG:32650", NULL) != SCAN_OK) return 143;
    if (scan_engine_set_crs(e6, "not-an-epsg", hk_wkt) != SCAN_ERR_INVALID_ARGUMENT) {
      return 144;
    }
    /* Still gated on convergence: nothing here georeferenced anything. */
    if (scan_engine_crs_wkt(e6)[0] != '\0') return 145;
    if (scan_engine_set_crs(e6, NULL, NULL) != SCAN_OK) return 146;  /* clear */
    scan_engine_destroy(e6);
  }

  /* --- ABI 6 (A16): device auto-discovery --------------------------------
   *
   * None of this needs an engine, hardware, or a network — which is the
   * property that makes it smoke-testable at all. What is asserted is that
   * every new entry point is CALLABLE FROM C with the argument shapes JNI
   * will use, that the two-call capacity protocol works, and that a machine
   * with no lidar and no adapters gets answers rather than errors. */
  {
    scan_mid360_beacon beacons[2];
    scan_host_check_result hc;
    scan_serial_port ports[4];
    scan_d6_probe d6p;
    scan_um982_probe gnssp;
    const char* bogus[2];
    uint32_t found = 0xFFFFFFFFu;
    int64_t holder = -1;
    scan_error_t e;

    /* Serial enumeration, capacity-0 form: how many are there? */
    e = scan_enumerate_serial(NULL, 0, &found);
    if (e != SCAN_OK && e != SCAN_ERR_CAPACITY_EXCEEDED) return 150;
    if (found == 0xFFFFFFFFu) return 150;
    /* ...then the real call. A machine with more than 4 ports reports
     * SCAN_ERR_CAPACITY_EXCEEDED and still fills the array. */
    memset(ports, 0, sizeof ports);
    e = scan_enumerate_serial(ports, 4, &found);
    if (e != SCAN_OK && e != SCAN_ERR_CAPACITY_EXCEEDED) return 151;
    if (scan_enumerate_serial(NULL, 4, &found) != SCAN_ERR_INVALID_ARGUMENT) return 152;
    if (scan_enumerate_serial(ports, 4, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 153;

    /* Discovery with a zero timeout: binds, finds nothing, returns. SCAN_ERR_BUSY
     * is equally correct on a machine where Livox Viewer holds 56201. */
    memset(beacons, 0, sizeof beacons);
    found = 0xFFFFFFFFu;
    e = scan_discover_mid360(0u, beacons, 2, &found);
    if (e != SCAN_OK && e != SCAN_ERR_BUSY && e != SCAN_ERR_CAPACITY_EXCEEDED) return 154;
    if (e == SCAN_OK && found != 0) return 155;
    if (scan_discover_mid360(0u, beacons, 2, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 156;

    /* The host check, on the beacon the field session actually saw. The point
     * of the assertion is that a machine WITHOUT 192.168.1.5 still gets a
     * usable sentence — that is the whole feature. */
    memset(&beacons[0], 0, sizeof beacons[0]);
    strcpy(beacons[0].sn, "ARMCP7K0034759");
    strcpy(beacons[0].lidar_ip, "192.168.1.159");
    strcpy(beacons[0].netmask, "255.255.255.0");
    strcpy(beacons[0].gateway, "192.168.1.1");
    strcpy(beacons[0].persisted_host_ip, "192.168.1.5");
    memset(&hc, 0, sizeof hc);
    if (scan_host_check(&beacons[0], &hc) != SCAN_OK) return 157;
    if (hc.note[0] == '\0') return 158;
    if (strstr(hc.note, "192.168.1.5") == NULL) return 159;
    if (hc.candidate_count > SCAN_HOST_CHECK_MAX_CANDIDATES) return 160;
    if (scan_host_check(NULL, &hc) != SCAN_ERR_INVALID_ARGUMENT) return 161;
    if (scan_host_check(&beacons[0], NULL) != SCAN_ERR_INVALID_ARGUMENT) return 162;

    /* The probes. No ports offered, and one that cannot exist: both are
     * SCAN_ERR_NOT_FOUND, not an I/O error — "that device is not here" is an
     * answer a picker displays. */
    memset(&d6p, 0, sizeof d6p);
    memset(&gnssp, 0, sizeof gnssp);
    if (scan_probe_d6(NULL, 0, 50u, &d6p) != SCAN_ERR_NOT_FOUND) return 163;
    if (scan_probe_um982(NULL, 0, 50u, &gnssp) != SCAN_ERR_NOT_FOUND) return 164;
    bogus[0] = "/dev/lidarscan-capi-smoke-not-a-port";
    bogus[1] = "/dev/lidarscan-capi-smoke-not-a-port-either";
    if (scan_probe_d6(bogus, 2, 50u, &d6p) != SCAN_ERR_NOT_FOUND) return 165;
    if (scan_probe_um982(bogus, 2, 50u, &gnssp) != SCAN_ERR_NOT_FOUND) return 166;
    if (scan_probe_d6(bogus, 2, 50u, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 167;
    if (scan_probe_um982(bogus, 2, 50u, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 168;
    bogus[0] = NULL;
    if (scan_probe_d6(bogus, 2, 50u, &d6p) != SCAN_ERR_INVALID_ARGUMENT) return 169;

    /* The single-instance guard. A dedicated app_id so this never fights a
     * real LidarScan on the developer's machine. */
    if (scan_current_process_id() <= 0) return 170;
    e = scan_instance_acquire("lidarscan-capi-smoke", NULL, &holder);
    if (e != SCAN_OK && e != SCAN_ERR_FILE) return 171;
    if (e == SCAN_OK) {
      if (holder != scan_current_process_id()) return 172;
      /* Twice in one process is OK: this is a library being initialized
       * twice, not a second LidarScan. */
      if (scan_instance_acquire("lidarscan-capi-smoke", NULL, &holder) != SCAN_OK) return 173;
      scan_instance_release();
      scan_instance_release(); /* idempotent */
    }
  }

  /* --- ABI 8 (ROUND 9 item 35): the phone IMU -----------------------------
   *
   * The Android call sequence, from C: set the IMU->camera rotation once, then
   * pump SensorEvent triples. What is asserted is that every entry point is
   * callable with the argument shapes JNI will use, that the guards fire on
   * the two mistakes a sensor listener actually makes (out-of-order and
   * non-finite events, both of which Android really delivers), and that an
   * engine which is never given an IMU reports honest zeroes rather than
   * failing. */
  {
    scan_engine_config c8;
    scan_engine* e8 = NULL;
    scan_imu_densify_stats istats;
    /* SENSOR_ORIENTATION = 90 deg about +z: the usual phone, where the sensor
     * frame is defined against the display and ARCore's against the camera. */
    const double cam_from_imu[4] = {0.0, 0.0, 0.70710678118654752, 0.70710678118654752};
    const double zero_quat[4] = {0.0, 0.0, 0.0, 0.0};
    float gyro[3];
    float accel[3];
    int64_t t;
    int k;

    memset(&c8, 0, sizeof c8);
    c8.app_name = "capi-smoke-abi8";
    c8.log_level = SCAN_LOG_WARN;
    if (scan_engine_create(&c8, &e8) != SCAN_OK || e8 == NULL) return 180;

    /* Before anything is pushed: askable, and all zero. This is the state every
     * ABI-7 consumer stays in forever, and it must not be an error. */
    memset(&istats, 0xAB, sizeof istats);
    if (scan_engine_imu_densify_stats(e8, &istats) != SCAN_OK) return 181;
    if (istats.samples_in != 0 || istats.densified != 0) return 182;

    if (scan_engine_set_imu_extrinsics(e8, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 183;
    if (scan_engine_set_imu_extrinsics(e8, zero_quat) != SCAN_ERR_INVALID_ARGUMENT) return 184;
    if (scan_engine_set_imu_extrinsics(e8, cam_from_imu) != SCAN_OK) return 185;

    accel[0] = 0.0f;
    accel[1] = 0.0f;
    accel[2] = 9.81f;
    for (k = 0; k < 40; ++k) {
      t = 2000000000LL + (int64_t)k * 2500000LL; /* 400 Hz */
      gyro[0] = 0.01f * (float)k;
      gyro[1] = -0.02f;
      gyro[2] = 0.0f;
      if (scan_engine_push_imu(e8, t, gyro, accel) != SCAN_OK) return 186;
    }
    /* Out of order is rejected, not silently reordered — an Android
     * SensorEventListener genuinely delivers these across sensor types. */
    if (scan_engine_push_imu(e8, 2000000000LL, gyro, accel) != SCAN_ERR_INVALID_ARGUMENT) {
      return 187;
    }
    /* And so is a non-finite one. */
    gyro[1] = (float)NAN;
    if (scan_engine_push_imu(e8, 2100000000LL, gyro, accel) != SCAN_ERR_INVALID_ARGUMENT) {
      return 188;
    }
    if (scan_engine_push_imu(e8, 2100000000LL, NULL, accel) != SCAN_ERR_INVALID_ARGUMENT) {
      return 189;
    }
    if (scan_engine_push_imu(NULL, 2100000000LL, gyro, accel) != SCAN_ERR_INVALID_ARGUMENT) {
      return 190;
    }

    memset(&istats, 0, sizeof istats);
    if (scan_engine_imu_densify_stats(e8, &istats) != SCAN_OK) return 191;
    if (istats.samples_in != 42) return 192;      /* 40 good + the two refused */
    if (istats.samples_rejected != 2) return 193;
    if (scan_engine_imu_densify_stats(e8, NULL) != SCAN_ERR_INVALID_ARGUMENT) return 194;
    if (scan_engine_imu_densify_stats(NULL, &istats) != SCAN_ERR_INVALID_ARGUMENT) return 195;

    /* Re-applying the extrinsic rebuilds the densifier. ROUND 18 item 68: the
     * ring's samples are CARRIED ACROSS the rebuild (they are raw sensor-frame
     * measurements; the extrinsic is applied at integration time), because
     * dropping them shortened the gap bridge's reach by exactly the samples
     * pushed before the app applied the extrinsic. The 40 good samples
     * survive; the 2 refused ones were never in the ring. Asserting it here
     * keeps the header honest, in its new wording. */
    if (scan_engine_set_imu_extrinsics(e8, cam_from_imu) != SCAN_OK) return 196;
    memset(&istats, 0xAB, sizeof istats);
    if (scan_engine_imu_densify_stats(e8, &istats) != SCAN_OK) return 197;
    if (istats.samples_in != 40) return 198;

    scan_engine_destroy(e8);
  }

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
