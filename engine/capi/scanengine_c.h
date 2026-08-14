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

/* 1 -> 2 (INT-24): poses in (scan_pose, push_pose/pose_at), the D6 pushbroom
 * (mount extrinsics, enable/flush/stats), the mount-calibration solver handle,
 * two new stream ids, the kPoseUpdate event payload, and two session flags.
 * Every addition is at the END of an existing struct or a new symbol, but the
 * struct layouts changed, so DESIGN.md §6 item 9 applies: this and
 * scanengine::kEngineAbiVersion move together. */
/* 2 -> 3 (INT-29): A10's GNSS/RTK stack. NMEA in (scan_engine_push_nmea), the
 * fix and its timeline out (scan_gnss_fix, scan_gnss_stats), the NTRIP client
 * as a handle (scan_ntrip_*), the georeferencing solution and the session's
 * CRS (scan_georef_solution, scan_engine_crs_wkt), two new event types with
 * their payloads, and two new mirrored enums (FixType, NtripState). The
 * scan_event union also gained the DEVICE_HEALTH case it never had. */
#define SCAN_ABI_VERSION 3u

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
  SCAN_STREAM_POSE_FUSED = 7,
  SCAN_STREAM_SLAM_MAP = 8,  /* registered world-frame points: A6's map, A8's
                              * assembled pushbroom cloud */
  SCAN_STREAM_POSE_LIO = 9
};

/* --- poses: mirror of poses/pose_source.h + pose_interpolator.h ---------- */
enum {
  SCAN_POSE_QUALITY_INVALID = 0, /* do not use; points here are flagged */
  SCAN_POSE_QUALITY_POOR = 1,    /* ARCore limited tracking / GNSS single fix */
  SCAN_POSE_QUALITY_FAIR = 2,    /* DGPS / RTK float / recovering VIO */
  SCAN_POSE_QUALITY_GOOD = 3     /* healthy VIO / RTK fixed */
};

/* Why a sample is or is not usable. Collapsing these into one error code
 * loses exactly the distinction Tech Spec §3.3 asks for ("points during
 * tracking loss are flagged and excluded by default"), so scan_engine_pose_at
 * reports the gate alongside the pose. */
enum {
  SCAN_POSE_GATE_OK = 0,
  SCAN_POSE_GATE_NO_DATA = 1,        /* no poses pushed yet */
  SCAN_POSE_GATE_BEFORE_FIRST = 2,   /* predates the stream — never resolvable */
  SCAN_POSE_GATE_FUTURE = 3,         /* newer than the newest pose — retry */
  SCAN_POSE_GATE_STALE = 4,          /* bracketing poses too far apart */
  SCAN_POSE_GATE_TRACKING_LOST = 5,  /* a bracketing pose lost tracking */
  SCAN_POSE_GATE_LOW_CONFIDENCE = 6  /* below the confidence/quality floor */
};

/* Mount-calibration verdict (WIZARD.md screen 4 bands). Gate on this, and on
 * scan_mount_calib_result.split_half_px — NEVER on the reported sigmas. */
enum {
  SCAN_CALIB_GATE_UNKNOWN = 0, /* not computed (fewer than 4 observations) */
  SCAN_CALIB_GATE_GOOD = 1,    /* <= 12 px at 3 m */
  SCAN_CALIB_GATE_USABLE = 2,  /* <= 30 px */
  SCAN_CALIB_GATE_REJECT = 3   /* > 30 px: must redo the capture */
};

/* --- GNSS: mirror of gnss/gnss.h ---------------------------------------
 *
 * ORDERED, and the order is load-bearing: Tech Spec §3.4's capture gate is
 * "at or above", so a UI compares these numerically. This is NOT the GGA
 * quality digit (which numbers RTK-fixed 4 and RTK-float 5) — scan_gnss_fix
 * keeps that separately as `quality_raw`. */
enum {
  SCAN_FIX_NONE = 0,
  SCAN_FIX_SINGLE = 1,
  SCAN_FIX_DGPS = 2,
  SCAN_FIX_RTK_FLOAT = 3,
  SCAN_FIX_RTK_FIXED = 4
};

/* --- NTRIP: mirror of gnss/ntrip_client.h -------------------------------- */
enum {
  SCAN_NTRIP_IDLE = 0,
  SCAN_NTRIP_CONNECTING = 1,
  SCAN_NTRIP_STREAMING = 2,
  SCAN_NTRIP_STALLED = 3,     /* socket open, no RTCM for stall_timeout_ms */
  SCAN_NTRIP_RECONNECTING = 4,
  SCAN_NTRIP_FAILED = 5       /* terminal: auth, bad mountpoint, attempts gone */
};

/* Which trajectory the D6 pushbroom assembles against (§3.3). Same interface
 * either way — this is configuration, not a code path. */
enum {
  SCAN_TRAJECTORY_EXTERNAL = 0, /* pushed poses: ARCore, or a replayed track */
  SCAN_TRAJECTORY_GNSS = 1      /* the RTK rover's own trajectory */
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
  SCAN_EVENT_NTRIP_STATE = 51,
  SCAN_EVENT_GEOREF_CONVERGED = 52,
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
    struct {
      uint32_t device;
      uint8_t state;       /* SCAN_DEV_* */
      uint64_t points_out;
      double points_per_sec;
      double checksum_pass_rate;
    } health;
    struct {
      uint8_t source;      /* scan_stream_id the pose came from */
      float position[3];
      float quaternion[4]; /* x, y, z, w */
      uint8_t quality;     /* 0 = invalid .. 255 = best */
    } pose;
    /* SCAN_EVENT_GNSS_FIX — one closed NMEA epoch. */
    struct {
      uint8_t fix_type;        /* SCAN_FIX_* */
      uint8_t satellites;
      float hdop;
      float correction_age_s;  /* GGA field 13: the ROVER's corrections age */
      float sigma_h_m;         /* 1-sigma horizontal, from GST when available */
      double lat_deg, lon_deg, alt_m; /* alt_m is ORTHOMETRIC, as GGA reports */
    } gnss;
    /* SCAN_EVENT_NTRIP_STATE — the corrections link. `correction_age_s` is the
     * ENGINE's age (time since the last CRC-valid frame off the caster), which
     * is NOT the rover's; -1 means "no frame yet on this connection". */
    struct {
      uint8_t state;    /* SCAN_NTRIP_* */
      int32_t error;
      int32_t backoff_ms;
      uint64_t bytes_received;
      float correction_age_s;
    } ntrip;
    /* SCAN_EVENT_GEOREF_CONVERGED — the session became (or stopped being)
     * exportable in a real CRS. */
    struct {
      double cep95_m;
      double horizontal_sigma_m;
      uint32_t samples;
      int32_t epsg;
      uint8_t converged;
    } georef;
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

/* --- poses: mirror of poses/pose_source.h -------------------------------- */
typedef struct scan_pose {
  int64_t t_mono_ns;
  double position[3];
  double orientation[4]; /* x, y, z, w — unit quaternion */
  float position_sigma_m;
  float orientation_sigma_deg;
  uint8_t source;        /* SCAN_STREAM_* */
  uint8_t quality;       /* SCAN_POSE_QUALITY_* */
  uint8_t tracking_lost; /* 0/1 (§3.3) */
} scan_pose;

/* --- pushbroom: mirror of slam/pushbroom/pushbroom_assembler.h ------------ */
typedef struct scan_pushbroom_stats {
  uint64_t points_in;
  uint64_t points_out;     /* reached the PageStore */
  uint64_t points_pending; /* waiting for a pose right now */

  uint64_t dropped_range;     /* 0 / out-of-window returns */
  uint64_t dropped_no_pose;   /* before the first pose, or unresolved at flush */
  uint64_t dropped_overflow;  /* pending-queue bound hit */
  uint64_t dropped_page_full; /* PageStore backpressure */

  /* Broken out because they mean different things to a user: tracking loss is
   * "walk back and rescan", stale is "your pose stream stuttered", low
   * confidence is "ARCore was struggling". */
  uint64_t flagged_tracking_lost;
  uint64_t flagged_stale_pose;
  uint64_t flagged_low_confidence;
  uint64_t flagged_emitted; /* flagged AND kept */

  int64_t t_first_ns;
  int64_t t_last_ns;
} scan_pushbroom_stats;

/* --- mount calibration: mirror of slam/pushbroom/mount_calibration.h ------ */
typedef struct scan_mount_calib_result {
  double camera_from_lidar[16]; /* ROW-MAJOR; == phone_from_lidar */

  uint8_t converged;
  uint8_t degenerate; /* too few observations, or a rank-deficient solve */
  int32_t iterations_l2;
  int32_t iterations_robust;

  uint64_t observations;
  uint64_t residuals;
  double rms_residual_m;
  double final_cost;

  /* THE GATE. Pixels of disagreement at gate_range_m between two half-solves.
   * -1 when not computed. */
  double split_half_px;
  double gate_range_m;
  uint8_t gate; /* SCAN_CALIB_GATE_* */

  /* DIAGNOSTICS ONLY — the covariance cannot rank sessions (its coefficient of
   * variation is 0.13 while the true error's is 1.83). Never gate on these. */
  double sigma_rot_deg;
  double sigma_trans_mm;
  double condition_number;
} scan_mount_calib_result;

/* --- GNSS: mirror of gnss/gnss.h + gnss/gnss_source.h -------------------- */
typedef struct scan_gnss_fix {
  int64_t t_mono_ns;    /* ENGINE time (A4-mapped), not UTC */
  int64_t t_arrival_ns; /* raw arrival stamp, before the A4 mapping */
  int64_t utc_unix_ns;  /* 0 until an RMC has supplied a date */

  uint8_t fix;          /* SCAN_FIX_* */
  uint8_t satellites;
  uint8_t quality_raw;  /* GGA field 6 verbatim (3 = PPS, 8 = simulator, ...) */
  uint8_t fix_dimension;/* GSA field 2: 1 none, 2 = 2D, 3 = 3D */
  uint16_t station_id;  /* GGA field 14: which base is correcting us */

  double lat_deg, lon_deg;
  double alt_m;              /* ORTHOMETRIC (MSL), exactly as GGA reports it */
  double geoid_sep_m;        /* GGA field 11 */
  double height_ellipsoid_m; /* alt_m + geoid_sep_m — what the geodesy uses */
  uint8_t has_geoid_sep;

  float hdop, pdop, vdop;
  float correction_age_s;

  /* 1-sigma, metres. From GST when the receiver sends it, otherwise the
   * fix-quality fallback table; `sigma_from_gst` says which, and a UI that
   * quotes an accuracy should say so too. */
  float sigma_east_m, sigma_north_m, sigma_up_m;
  float sigma_horizontal_m;
  uint8_t sigma_from_gst;

  float speed_mps;
  float course_deg;
  uint8_t has_course;
} scan_gnss_fix;

/* The §3.4 fix-quality timeline B9's status strip and a session report show. */
typedef struct scan_gnss_stats {
  uint64_t bytes_in;
  uint64_t sentences_ok;
  uint64_t checksum_failed;
  uint64_t malformed;
  double checksum_pass_rate;

  uint64_t epochs;
  uint64_t fixes_published;
  uint64_t poses_published;
  uint64_t epochs_no_position;
  uint64_t epochs_below_gate;
  uint64_t gst_epochs;

  /* Epoch counts indexed by SCAN_FIX_*. */
  uint64_t by_fix[5];

  uint8_t time_converged; /* A4: false for the first ~16 s of a 1 Hz stream */
  int64_t time_uncertainty_ns;
  uint8_t has_origin;
  double origin_lat_deg, origin_lon_deg, origin_height_m;
} scan_gnss_stats;

/* --- NTRIP: mirror of gnss/gnss.h NtripConfig + ntrip_client.h ----------- */
typedef struct scan_ntrip_config {
  const char* host;      /* required */
  uint16_t port;         /* 0 = 2101 */
  const char* mountpoint;/* required for connect(), ignored by fetch_sourcetable */
  const char* username;  /* may be NULL */
  const char* password;  /* may be NULL */
  const char* user_agent;/* may be NULL; forced to start with "NTRIP" */

  int32_t ntrip_version;   /* 0 = default 2 */
  uint8_t allow_v1_fallback_set; /* 0 = keep the default (fallback on) */
  uint8_t allow_v1_fallback;

  int32_t connect_timeout_ms; /* 0 = default */
  int32_t stall_timeout_ms;   /* 0 = default 30000 */
  int32_t gga_interval_ms;    /* <0 disables; 0 = default 10000 */

  uint8_t auto_reconnect_set; /* 0 = keep the default (on) */
  uint8_t auto_reconnect;
  int32_t max_reconnect_attempts; /* 0 = forever */
} scan_ntrip_config;

typedef struct scan_ntrip_stats {
  uint64_t connect_attempts;
  uint64_t connects_ok;
  uint64_t disconnects;
  uint64_t reconnects;
  uint64_t bytes_received;
  uint64_t gga_sent;
  uint64_t stalls;
  uint64_t handshake_failures;

  /* RTCM3 transport framing (gnss/rtcm3.h). `frames_crc_failed` is what
   * separates "bad corrections" from "bad sky" — corruption on the Bluetooth
   * hop leaves the rover silently in Float. */
  uint64_t frames_ok;
  uint64_t frames_crc_failed;
  uint64_t rtcm_bytes;

  int64_t t_connected_ns;
  int64_t t_last_rtcm_ns;
  int32_t backoff_ms;
  int32_t http_status;      /* 200 / 401 / 404 / 0 */
  int32_t ntrip_version_used;
  int32_t last_error;
  uint8_t state;            /* SCAN_NTRIP_* */
  uint8_t receiving;        /* a CRC-valid frame on the CURRENT connection */
  float correction_age_s;   /* -1 before the first frame: unknown != fresh */
} scan_ntrip_stats;

/* One sourcetable STR record. Strings are fixed-size so the array crosses the
 * ABI by value — convention 3: no pointer here is owned by the callee. */
typedef struct scan_ntrip_source {
  char mountpoint[64];
  char identifier[64];
  char format[32];
  char nav_system[64];
  char country[8];
  double lat_deg, lon_deg;
  uint8_t needs_gga; /* a VRS mount that wants our GGA uploaded */
  uint8_t fee;
  int32_t carrier;   /* 0 none, 1 L1, 2 L1+L2 */
  int32_t solution;  /* 0 single base, 1 network */
  int32_t bitrate;
} scan_ntrip_source;

/* --- georeferencing: mirror of gnss/georef.h ----------------------------- */
typedef struct scan_georef_solution {
  uint8_t converged;
  double yaw_deg;
  double translation[3];
  double scale;
  double global_from_local[16]; /* ROW-MAJOR, se3.h convention */

  uint32_t samples, inliers, rejected;
  double residual_rms_m;
  double residual_rms_h_m;
  double gravity_residual_m;

  double yaw_sigma_deg;
  double translation_sigma_h_m;
  double span_m;

  /* What a UI should show. horizontal_sigma_m includes the fixes' OWN accuracy
   * at full strength (they share a base-station error that does not average
   * out), so it is deliberately conservative — docs/A10-gnss.md §5.1. */
  double horizontal_sigma_m;
  double vertical_sigma_m;
  double cep50_m, cep95_m;
  double mean_fix_sigma_m;

  uint8_t best_fix, dominant_fix; /* SCAN_FIX_* */
  int32_t epsg;                   /* 0 until the origin picks a zone */
  int64_t t_first_ns, t_last_ns;

  /* Why it is NOT converged, for a UI that has to say something. Empty when it
   * is. Copied in, not a pointer into the engine. */
  char blocker[64];
} scan_georef_solution;

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
  /* Tech Spec §3.1's capture toggle: 0 = Record-only, 1 = Live-SLAM (one LIO
   * for the session's Mid-360 stream, map on SCAN_STREAM_SLAM_MAP). */
  uint8_t live_slam;
  /* Assemble D6 profiles into world points with the pushed pose stream. Needs
   * scan_engine_set_mount_extrinsics() first. */
  uint8_t pushbroom;
  /* SCAN_TRAJECTORY_* — which pose stream the assembler interpolates against.
   * 0 (external) is the ARCore default; 1 is §3.3's desktop RTK mode. */
  uint8_t trajectory;
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

/* --- poses in (A8) -------------------------------------------------------
 *
 * The ARCore path: once per camera frame, push the VIO pose, its tracking
 * state and its confidence. Safe from the AR thread while points are decoded
 * on another. `confidence` < 0 means "derive it from quality/tracking_lost".
 * An out-of-order or non-finite pose is rejected with
 * SCAN_ERR_INVALID_ARGUMENT rather than silently corrupting every
 * interpolation that follows. Each accepted pose raises
 * SCAN_EVENT_POSE_UPDATE. */
SCAN_API scan_error_t scan_engine_push_pose(scan_engine* engine, const scan_pose* pose,
                                            float confidence);

/* Interpolated lookup. *out_gate is ALWAYS written (SCAN_POSE_GATE_*), even on
 * failure. Returns SCAN_OK whenever a pose could be interpolated at all —
 * including the flagged gates, where the geometry is real but must not be
 * trusted silently — SCAN_ERR_AGAIN when `t` is newer than the newest pose
 * (buffer and retry), and SCAN_ERR_NOT_FOUND when it predates the stream.
 * `out` may be NULL if only the gate is wanted. */
SCAN_API scan_error_t scan_engine_pose_at(scan_engine* engine, int64_t t_mono_ns,
                                          scan_pose* out, uint8_t* out_gate);

/* --- D6 pushbroom (A8) ---------------------------------------------------
 *
 * `phone_from_lidar` is a ROW-MAJOR rigid 4x4. A column-major matrix handed
 * across JNI is REJECTED (SCAN_ERR_INVALID_ARGUMENT) instead of producing a
 * plausible-looking mirrored cloud nobody notices until export. */
SCAN_API scan_error_t scan_engine_set_mount_extrinsics(scan_engine* engine,
                                                       const double phone_from_lidar[16]);
/* Needs an extrinsic first, else SCAN_ERR_INVALID_STATE. */
SCAN_API scan_error_t scan_engine_pushbroom_enable(scan_engine* engine, int on);
/* Resolve every pending point the poses allow and push the batch out. Called
 * automatically by scan_engine_stop(). */
SCAN_API scan_error_t scan_engine_pushbroom_flush(scan_engine* engine);
SCAN_API scan_error_t scan_engine_pushbroom_stats(scan_engine* engine,
                                                  scan_pushbroom_stats* out);

/* --- mount calibration (A8) ----------------------------------------------
 *
 * The wizard's solver, as a standalone handle: it needs no engine and no
 * session, because the observations come from the app's checkerboard
 * detection. Add >= 5 observations (the Zhang-Pless floor; < 3 is refused
 * outright as undetermined), then solve from the bracket's CAD nominal. */
typedef struct scan_mount_calib scan_mount_calib;

SCAN_API scan_error_t scan_mount_calib_create(scan_mount_calib** out);
SCAN_API void scan_mount_calib_destroy(scan_mount_calib* calib);
/* `normal` must be unit length and `d` positive: the target plane AS THE
 * CAMERA MEASURED IT, in the camera frame. `pts` are the lidar returns
 * segmented onto the board, in the SENSOR frame. `sigma_m` is their 1-sigma
 * range noise — it whitens the residual, so a capture mixing sensors or
 * ranges is weighted correctly. */
SCAN_API scan_error_t scan_mount_calib_add_observation(scan_mount_calib* calib,
                                                       const double normal[3], double d,
                                                       const scan_point_vertex* pts, uint32_t n,
                                                       double sigma_m);
SCAN_API scan_error_t scan_mount_calib_solve(scan_mount_calib* calib, const double cad[16],
                                             scan_mount_calib_result* out);

/* --- GNSS / RTK (A10) ----------------------------------------------------
 *
 * The rover's bytes. `device_id` must be a SCAN_DEVICE_RTK_ROVER device. Any
 * chunk of the byte stream is fine — a fragment, a sentence, or several: the
 * framer handles arbitrary chunking, which is what Bluetooth SPP's 20–990-byte
 * MTU fragments require. Record-always applies: the bytes hit the .lscan as
 * kGnssNmea chunks BEFORE they are parsed. t_mono_ns 0 = "stamp on arrival".
 *
 * This is scan_engine_push_serial_bytes() by another name, and both work; this
 * one exists because "push NMEA" is what the JNI caller thinks it is doing, and
 * because it can refuse a device that is not a rover instead of quietly
 * feeding a D6 parser. */
SCAN_API scan_error_t scan_engine_push_nmea(scan_engine* engine, uint32_t device_id,
                                            const uint8_t* data, size_t len, int64_t t_mono_ns);

/* The most recent closed epoch. Zero-filled with fix == SCAN_FIX_NONE before
 * the first one — never an error, because "no fix yet" is a normal state a
 * status strip has to render. */
SCAN_API scan_error_t scan_engine_last_fix(scan_engine* engine, scan_gnss_fix* out);

/* The fix-quality timeline (§3.4) plus the NMEA link health behind it. */
SCAN_API scan_error_t scan_engine_gnss_stats(scan_engine* engine, scan_gnss_stats* out);

/* The local -> global transform and how well it is known. Always writes *out;
 * `converged == 0` with a non-empty `blocker` is the normal pre-convergence
 * answer, not a failure. */
SCAN_API scan_error_t scan_engine_georef_solution(scan_engine* engine,
                                                  scan_georef_solution* out);

/* The A9 export seam. UTF-8, engine-owned, valid until the next engine call ON
 * THIS THREAD (convention 4) — copy it. Never NULL.
 *
 * Both are EMPTY until the georef transform converges. That is stricter than
 * "the site's UTM zone is known", and deliberately so: until the transform
 * converges the cloud is still in the LOCAL frame, and labelling it with a real
 * CRS produces a file that opens fine and lands in the wrong place. Empty is
 * exactly A9's documented "embed the local-frame placeholder" input.
 * scan_engine_crs_epsg() returns "EPSG:32650" or "". */
SCAN_API const char* scan_engine_crs_wkt(scan_engine* engine);
SCAN_API const char* scan_engine_crs_epsg(scan_engine* engine);

/* --- NTRIP client (A10) --------------------------------------------------
 *
 * scan_ntrip_create(engine, &c) BORROWS the engine's own client — the one whose
 * GGA upload is the rover's own last sentence and whose forwarded frames are
 * recorded. That is what an app wants. Passing NULL for `engine` creates a
 * STANDALONE client instead, for a mountpoint picker or a diagnostic tool that
 * has no engine. Either way scan_ntrip_destroy() is required, and it frees only
 * what it owns.
 *
 * connect() performs the first handshake SYNCHRONOUSLY, so a wrong password is
 * SCAN_ERR_PERMISSION_DENIED and an unknown mountpoint is SCAN_ERR_NOT_FOUND
 * from this call — not an infinite reconnect loop under a "connecting..." UI.
 * Both are treated as permanently fatal by the reconnect loop: retrying a
 * rejected password forever is how an account gets banned from a caster. */
typedef struct scan_ntrip scan_ntrip;

/* RTCM3 to the rover: WHOLE, CRC-valid frames only. Runs on the NTRIP receive
 * thread with no client lock held. It must be quick, must not re-enter the
 * client, and (on Android) must attach the thread before touching the JVM. */
typedef void (*scan_rtcm_cb)(const uint8_t* data, size_t len, void* user_data);

SCAN_API scan_error_t scan_ntrip_create(scan_engine* engine, scan_ntrip** out);
SCAN_API void scan_ntrip_destroy(scan_ntrip* client);
SCAN_API scan_error_t scan_ntrip_connect(scan_ntrip* client, const scan_ntrip_config* cfg);
SCAN_API scan_error_t scan_ntrip_disconnect(scan_ntrip* client);
SCAN_API scan_error_t scan_ntrip_get_state(scan_ntrip* client, int32_t* out_state);
SCAN_API scan_error_t scan_ntrip_get_stats(scan_ntrip* client, scan_ntrip_stats* out);
SCAN_API scan_error_t scan_ntrip_set_rtcm_callback(scan_ntrip* client, scan_rtcm_cb cb,
                                                   void* user_data);

/* The mountpoint picker. A separate short-lived connection, so it works before
 * connect() and while streaming. Writes at most `capacity` records and always
 * reports the caster's true total in *out_count, so a caller can retry with a
 * bigger array; SCAN_ERR_CAPACITY_EXCEEDED says it was truncated. Only `host`,
 * `port` and the credentials of `cfg` are used. */
SCAN_API scan_error_t scan_ntrip_fetch_sourcetable(const scan_ntrip_config* cfg,
                                                   scan_ntrip_source* out, uint32_t capacity,
                                                   uint32_t* out_count);

/* Logging is process-global, not per engine. */
SCAN_API void scan_engine_set_log_callback(scan_log_cb cb, void* user_data, int32_t min_level);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SCANENGINE_C_H */
