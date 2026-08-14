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
#include <stdio.h>
#include <string.h>

#include "scanengine_c.h"

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
  uint32_t device_id = 0;
  uint32_t page_count = 0;
  uint32_t page_id = 0;
  uint64_t total_points = 0;
  int32_t state = -1;
  unsigned char first_written = 0;
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
