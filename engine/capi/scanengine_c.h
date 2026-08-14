/* scanengine_c.h — flat C ABI over the engine.
 *
 * WHO USES THIS: Android/JNI (Tech Spec §3 key rule 1) and any other FFI
 * consumer. The Qt desktop app does NOT use it — it links the C++ API
 * directly, in-process, no marshalling.
 *
 * CONVENTIONS (fixed by A1; every later addition must follow them)
 *   1. Every function that can fail returns scan_error_t. Out-parameters
 *      come last. Functions that cannot fail return void or a plain value.
 *   2. Handles are opaque pointers. The caller owns nothing behind them and
 *      frees them only with the matching *_destroy.
 *   3. No struct in this header contains a pointer the callee keeps.
 *      Buffers passed in are consumed before the call returns.
 *   4. Strings out are UTF-8, NUL-terminated, owned by the engine, and valid
 *      until the next engine call ON THE SAME THREAD. Copy them.
 *   5. No C++ exception ever crosses this boundary: every entry point is
 *      wrapped in a catch-all that converts to SCAN_ERR_UNKNOWN.
 *   6. Enum values mirror the C++ enums exactly and are checked with
 *      static_assert in scanengine_c.cpp — a mismatch is a build error, not
 *      a runtime surprise.
 *   7. The ABI is versioned: call scan_engine_abi_version() and compare
 *      against SCAN_ABI_VERSION from the header you compiled against.
 *      JNI must do this once at library load.
 *
 * THREADING: every function is safe to call from any thread. Events are
 * either polled (scan_engine_poll_event, on your thread) or pushed
 * (scan_engine_set_event_callback, on the ENGINE'S publishing thread — do
 * not call back into the engine from it, and do not touch the JVM without
 * attaching the thread first).
 */
#ifndef SCANENGINE_C_H
#define SCANENGINE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(SCANENGINE_BUILD_SHARED)
#define SCAN_API __declspec(dllexport)
#elif defined(SCANENGINE_USE_SHARED)
#define SCAN_API __declspec(dllimport)
#else
#define SCAN_API
#endif
#else
#define SCAN_API __attribute__((visibility("default")))
#endif

#define SCAN_ABI_VERSION 1u

/* --- errors: mirror of scanengine::ScanError --------------------------- */
typedef int32_t scan_error_t;

enum {
  SCAN_OK = 0,
  SCAN_ERR_UNKNOWN = 1,
  SCAN_ERR_INVALID_ARGUMENT = 2,
  SCAN_ERR_INVALID_STATE = 3,
  SCAN_ERR_NOT_FOUND = 4,
  SCAN_ERR_ALREADY_EXISTS = 5,
  SCAN_ERR_NOT_SUPPORTED = 6,
  SCAN_ERR_UNIMPLEMENTED = 7,
  SCAN_ERR_OUT_OF_MEMORY = 8,
  SCAN_ERR_CANCELLED = 9,
  SCAN_ERR_TIMEOUT = 10,
  SCAN_ERR_BUSY = 11,
  SCAN_ERR_AGAIN = 12,
  SCAN_ERR_CAPACITY_EXCEEDED = 13,
  SCAN_ERR_IO = 20,
  SCAN_ERR_DISCONNECTED = 21,
  SCAN_ERR_PERMISSION_DENIED = 22,
  SCAN_ERR_NETWORK = 23,
  SCAN_ERR_DEVICE_NOT_RESPONDING = 30,
  SCAN_ERR_PROTOCOL = 31,
  SCAN_ERR_CHECKSUM = 32,
  SCAN_ERR_DEVICE_FAULT = 33,
  SCAN_ERR_CORRUPT_DATA = 40,
  SCAN_ERR_VERSION_MISMATCH = 41,
  SCAN_ERR_FILE = 42
};

/* --- enums mirrored from core/types.h ---------------------------------- */
enum {
  SCAN_DEVICE_UNKNOWN = 0,
  SCAN_DEVICE_D6 = 1,
  SCAN_DEVICE_MID360 = 2,
  SCAN_DEVICE_RTK_ROVER = 3
};

enum {
  SCAN_DEV_DISCONNECTED = 0,
  SCAN_DEV_IDLE = 1,
  SCAN_DEV_STARTING = 2,
  SCAN_DEV_STREAMING = 3,
  SCAN_DEV_DEGRADED = 4,
  SCAN_DEV_STOPPING = 5,
  SCAN_DEV_FAULT = 6
};

enum {
  SCAN_ENGINE_IDLE = 0,
  SCAN_ENGINE_STARTING = 1,
  SCAN_ENGINE_RUNNING = 2,
  SCAN_ENGINE_STOPPING = 3,
  SCAN_ENGINE_FAULTED = 4
};

enum {
  SCAN_STREAM_UNKNOWN = 0,
  SCAN_STREAM_LIDAR_D6 = 1,
  SCAN_STREAM_LIDAR_MID360 = 2,
  SCAN_STREAM_IMU = 3,
  SCAN_STREAM_POSE_AR = 4,
  SCAN_STREAM_GNSS = 5,
  SCAN_STREAM_CAMERA_FRAMES = 6,
  SCAN_STREAM_POSE_FUSED = 7
};

enum {
  SCAN_LOG_TRACE = 0,
  SCAN_LOG_DEBUG = 1,
  SCAN_LOG_INFO = 2,
  SCAN_LOG_WARN = 3,
  SCAN_LOG_ERROR = 4,
  SCAN_LOG_OFF = 5
};

/* --- events: mirror of core/event.h ------------------------------------ */
enum {
  SCAN_EVENT_NONE = 0,
  SCAN_EVENT_EVENTS_DROPPED = 1,
  SCAN_EVENT_ENGINE_STATE = 10,
  SCAN_EVENT_SESSION_STATE = 11,
  SCAN_EVENT_DEVICE_STATE = 20,
  SCAN_EVENT_DEVICE_HEALTH = 21,
  SCAN_EVENT_POINTS_AVAILABLE = 30,
  SCAN_EVENT_ROTATION = 31,
  SCAN_EVENT_POSE_UPDATE = 40,
  SCAN_EVENT_GNSS_FIX = 50,
  SCAN_EVENT_JOB_PROGRESS = 60,
  SCAN_EVENT_ERROR = 90
};

typedef struct scan_event {
  uint16_t type;
  uint32_t sequence;
  int64_t t_mono_ns;

  union {
    struct { uint64_t count; uint64_t total; } dropped;
    struct { uint8_t state; uint8_t previous; } engine_state;
    struct { uint8_t recording; uint64_t session_id; uint64_t bytes_written; } session;
    struct {
      uint32_t device;
      uint8_t kind;
      uint8_t state;
      uint8_t previous;
      int32_t error;
    } device;
    struct {
      uint32_t page;
      uint32_t first;
      uint32_t count;
      uint8_t stream;
      uint8_t page_created;
    } points;
    struct {
      uint32_t device;
      uint64_t rotation_index;
      uint32_t points_in_rotation;
      double rotation_hz;
    } rotation;
    struct { int32_t error; uint32_t device; uint8_t stream; } error;
    uint8_t raw[64];
  } payload;
} scan_event;

/* --- point pages: mirror of cloud/point_page.h -------------------------- */
typedef struct scan_point_vertex {
  float x, y, z;
  uint8_t r, g, b, a;
} scan_point_vertex; /* 16 bytes — the S3-proven GPU layout */

typedef struct scan_point_page {
  uint32_t id;
  uint8_t stream;
  const scan_point_vertex* data; /* stable for the page's lifetime */
  uint32_t count;
  uint32_t capacity;
  int64_t t_first_ns;
  int64_t t_last_ns;
  float bounds_min[3];
  float bounds_max[3];
} scan_point_page;

typedef struct scan_device_health {
  uint32_t id;
  uint8_t kind;
  uint8_t state;
  int32_t last_error;
  uint64_t bytes_in;
  uint64_t packets_ok;
  uint64_t packets_bad;
  uint64_t points_out;
  uint64_t drops;
  double points_per_sec;
  double rotation_hz;
  double checksum_pass_rate;
  int64_t t_last_data_ns;
} scan_device_health;

/* --- handles and callbacks ---------------------------------------------- */
typedef struct scan_engine scan_engine;

typedef void (*scan_event_cb)(const scan_event* ev, void* user_data);
typedef void (*scan_log_cb)(int32_t level, const char* module, const char* message,
                            void* user_data);
/* Host → device write, implemented by the app (JNI shim / QSerialPort).
 * Return SCAN_OK or an SCAN_ERR_* value. */
typedef scan_error_t (*scan_serial_write_cb)(const uint8_t* data, size_t len, void* user_data);

typedef struct scan_engine_config {
  const char* app_name;          /* copied; may be NULL */
  int32_t log_level;             /* SCAN_LOG_* */
  uint32_t page_capacity;        /* points per page; 0 = default 1<<20 */
  uint32_t max_pages;            /* 0 = default */
  uint32_t event_queue_capacity; /* 0 = default 1024 */
} scan_engine_config;

typedef struct scan_session_config {
  const char* lscan_dir; /* may be NULL/empty = do not record */
  const char* profile;   /* survey | floorplan | research | quickscan */
  uint8_t record;
} scan_session_config;

typedef struct scan_device_config {
  int32_t kind; /* SCAN_DEVICE_* */

  /* D6 (serial) */
  const char* serial_port_name;
  uint32_t serial_baud;
  scan_serial_write_cb serial_write; /* may be NULL: engine sends no commands */
  void* serial_write_user_data;
  uint8_t send_start_stop_commands;

  /* Mid-360 (UDP) — A3 */
  const char* lidar_ip;
  const char* host_ip;
} scan_device_config;

/* --- engine ------------------------------------------------------------- */
SCAN_API uint32_t scan_engine_abi_version(void);
SCAN_API const char* scan_engine_version_string(void);
SCAN_API const char* scan_error_str(scan_error_t err);
/* Detail for the last failing call ON THIS THREAD. Never NULL. */
SCAN_API const char* scan_engine_last_error(void);

SCAN_API scan_error_t scan_engine_create(const scan_engine_config* cfg, scan_engine** out);
SCAN_API void scan_engine_destroy(scan_engine* engine);

SCAN_API scan_error_t scan_engine_start(scan_engine* engine, const scan_session_config* cfg);
SCAN_API scan_error_t scan_engine_stop(scan_engine* engine);
SCAN_API scan_error_t scan_engine_state(scan_engine* engine, int32_t* out_state);

SCAN_API scan_error_t scan_engine_add_device(scan_engine* engine,
                                             const scan_device_config* cfg,
                                             uint32_t* out_device_id);
SCAN_API scan_error_t scan_engine_remove_device(scan_engine* engine, uint32_t device_id);
SCAN_API scan_error_t scan_engine_device_health(scan_engine* engine, uint32_t device_id,
                                                scan_device_health* out);

/* Bytes from the app's serial reader. t_mono_ns 0 = "stamp on arrival". */
SCAN_API scan_error_t scan_engine_push_serial_bytes(scan_engine* engine, uint32_t device_id,
                                                    const uint8_t* data, size_t len,
                                                    int64_t t_mono_ns);

/* Events. poll returns SCAN_ERR_AGAIN when the queue is empty; wait blocks
 * up to timeout_ms (negative = forever). Installing a callback switches the
 * built-in subscription to push mode and discards anything queued. */
SCAN_API scan_error_t scan_engine_poll_event(scan_engine* engine, scan_event* out);
SCAN_API scan_error_t scan_engine_wait_event(scan_engine* engine, scan_event* out,
                                             int32_t timeout_ms);
SCAN_API scan_error_t scan_engine_set_event_callback(scan_engine* engine, scan_event_cb cb,
                                                     void* user_data);

/* Points. page_count + page_id_at enumerate; get_point_page hands back a
 * pointer straight into the engine's buffer for GPU upload. */
SCAN_API scan_error_t scan_engine_page_count(scan_engine* engine, uint32_t* out_count);
SCAN_API scan_error_t scan_engine_page_id_at(scan_engine* engine, uint32_t index,
                                             uint32_t* out_page_id);
SCAN_API scan_error_t scan_engine_get_point_page(scan_engine* engine, uint32_t page_id,
                                                 scan_point_page* out);
SCAN_API scan_error_t scan_engine_total_points(scan_engine* engine, uint64_t* out_points);

/* Logging is process-global, not per engine. */
SCAN_API void scan_engine_set_log_callback(scan_log_cb cb, void* user_data, int32_t min_level);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SCANENGINE_C_H */
