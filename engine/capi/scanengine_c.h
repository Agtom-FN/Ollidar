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
/* 3 -> 4 (INT-34): A11's colorization and A15's jobs.
 *   * scan_engine_record_keyframe + scan_keyframe — B8's capture-side write
 *     path for streams/frames/frames.idx.
 *   * the scan_colorizer_* handle — create/configure/load/run/cancel/stats,
 *     with a progress callback.
 *   * scan_clock_sweep_estimate + scan_rate_sample/scan_clock_sweep_result —
 *     B7's wizard sweep.
 *   * SCAN_EVENT_JOB_PROGRESS finally has a real union case (it used to cross
 *     as zeroed opaque bytes — docs/A15-jobs.md §7.1) and the SCAN_JOB_*
 *     state mirror behind it.
 *   * scan_event.payload.points gained `update_kind`, so a recolour is
 *     distinguishable from an append (docs/A11-color.md §8.1).
 * The scan_event union changed layout, so per DESIGN.md §6 item 9 this and
 * scanengine::kEngineAbiVersion move together. */
/* 4 -> 5 (INT-FINAL): the Android capture seam and the CRS escape hatch.
 *   * scan_device_config's Mid-360 half is no longer lidar_ip + host_ip. It
 *     carries the BACKEND selector, the two PRE-BOUND descriptors (points and
 *     IMU), all ten ports, the receive-buffer request, the point filter, the
 *     live decimation budget and the SDK config path. android/NOTES.md §8
 *     finding 1 is the reason: `UdpConfig::prebound_fd` is the field
 *     udp_source.h documents AS the Android seam and it was unreachable from
 *     C, so "the capture session cannot use a pre-bound socket at all". It can
 *     now. Findings 2 (one fd is not enough for a two-socket backend) and 4
 *     (sdk_config_path, which forced a TMPDIR work-around) are the same
 *     struct's other two holes and are closed with it.
 *   * scan_engine_set_crs() — docs/INT29-wiring.md §7 item 5: a C consumer
 *     needing a national grid had no way to supply the WKT its authority
 *     publishes, and the engine ships no PROJ to derive one.
 * scan_device_config changed layout, so per DESIGN.md §6 item 9 this and
 * scanengine::kEngineAbiVersion move together. Every OTHER struct, every
 * function signature and every enum value from ABI 4 is unchanged. */
/* 5 -> 6 (A16): device auto-discovery and the single-instance guard. The
 * owner requirement from the first real-hardware session, verbatim: manual
 * IP/port entry defeats the GUI.
 *   * scan_discover_mid360() + scan_mid360_beacon — the 1 Hz broadcast
 *     heartbeat, parsed: SN, firmware, the lidar's own IP/mask/gateway and
 *     the HOST IP it has persisted and will stream to.
 *   * scan_host_check() + scan_host_check_result — "the lidar expects
 *     192.168.1.5 and this machine is not 192.168.1.5", which is the exact
 *     way the field session's first Mid-360 session failed.
 *   * scan_probe_d6() / scan_probe_um982() + their result structs — identify
 *     a serial device by its WIRE SIGNATURE, never by its /dev name.
 *   * scan_enumerate_serial() + scan_serial_port.
 *   * scan_instance_acquire() / scan_instance_release() — the advisory
 *     single-instance lock (owner round-4 item 6).
 * Every addition is a NEW symbol or a NEW struct: no existing struct layout,
 * function signature or enum value from ABI 5 changed. An ABI-5 consumer
 * relinks against this header unmodified. */
/* 6 -> 7 (live page eviction): the fix for the 2026-08-17 field bug "live view
 * not moving even i move the lidar". The page store is bounded, and when it
 * filled it dropped EVERY subsequent point for the rest of the run — a Mid-360
 * at ~200k pts/s fills the default 64 x 1 M pages during a PREVIEW, and the
 * live view then froze at that instant while the log took one warn per
 * revolution (1400 in one session).
 *   * scan_engine_set_live_page_eviction() — opt a LIVE CAPTURE's store into
 *     recycling its oldest page instead of dead-ending. OFF by default: an
 *     offline/post-processing store must still hard-cap and say so.
 *   * scan_engine_page_stats() + scan_page_stats — pages, the ceiling,
 *     resident/evicted points and `evicting`, so an app can say "the live map
 *     shows the most recent N points; the recording has all of them".
 *   * scan_engine_recycle_live_pages() — empty the live window without
 *     freeing a buffer a renderer may be reading.
 *   * SCAN_PAGE_UPDATE_EVICTED — a new VALUE of the existing
 *     scan_event.payload.points.update_kind field (no layout change): the
 *     page is gone. A consumer that ignores update_kind asks for the page,
 *     gets SCAN_ERR_NOT_FOUND, and skips — which is what it already does for
 *     any page that vanished.
 * Every addition is a NEW symbol, a NEW struct or a NEW enum value: no
 * existing struct layout or function signature from ABI 6 changed, and the
 * default behaviour of every existing call is byte-for-byte what it was. An
 * ABI-6 consumer relinks against this header unmodified. */
/* 7 -> 8 (ROUND 9 item 35): the phone's IMU becomes an input to the geometry.
 * The owner's words: "lidar data and the imu position data need sync the
 * frequency". ARCore delivers ~30 poses/s and the COIN-D6 delivers 4000
 * returns/s, so ~133 consecutive returns share one pose bracket and are placed
 * on the chord between its ends — which cannot represent the 5-15 Hz band a
 * handheld rig actually moves in. The phone's own gyro runs at 200-400 Hz and
 * does. On the engine's fixture (1.5 deg of 12 Hz jitter) the wall's plane-fit
 * RMS goes from 0.739 cm to 0.021 cm.
 *   * scan_engine_push_imu() — one gyro+accel sample, `SensorEvent.timestamp`
 *     verbatim (it is already CLOCK_BOOTTIME, the same clock ARCore stamps
 *     poses with, so nothing maps it). Feeds the densifier AND is recorded as
 *     a ChunkType::kPhoneImu chunk, because since this round the gyro decides
 *     where most returns go and a container without it can only be re-resolved
 *     to a coarser cloud than the operator watched.
 *   * scan_engine_set_imu_extrinsics() — the IMU->camera rotation. NOT
 *     identity on a real phone: Android's sensor frame is defined against the
 *     display and ARCore's against the camera image, and the two are usually
 *     90 degrees apart (CameraCharacteristics.SENSOR_ORIENTATION). Only the
 *     app can know it, which is why it must cross the ABI.
 *   * scan_engine_imu_densify_stats() + scan_imu_densify_stats — was the gyro
 *     path used, and if not, which of the four guards refused it.
 *   * SCAN_STREAM_IMU_PHONE — a new VALUE of the existing SCAN_STREAM_* mirror
 *     (no layout change). Deliberately not SCAN_STREAM_IMU, which is the
 *     Mid-360's IMU and is what two offline pipelines read as "this is a
 *     Mid-360 project".
 * Every addition is a NEW symbol, a NEW struct or a NEW enum value: no existing
 * struct layout or function signature from ABI 7 changed. An ABI-7 consumer
 * relinks against this header unmodified and, pushing no IMU, gets
 * byte-for-byte the trajectory and the cloud it got before. */

/* --- ABI 9 (ROUND 10 item 36): the lidar -> pose time offset -------------
 *
 * ONE new pair of functions, no struct or signature changed:
 *
 *   * scan_engine_set_pose_time_offset_ns() / scan_engine_pose_time_offset_ns()
 *     — the constant delay between the clock a D6 return is dated in and the
 *     clock ARCore stamps its poses in. Zero is the ABI-8 behaviour exactly,
 *     so an ABI-8 consumer that never calls it gets the cloud it got before.
 *
 * WHY IT HAS TO CROSS THE ABI. The offset is a property of the phone's USB
 * stack and its reader thread, not of the sensor, so the engine cannot know
 * it and cannot derive it; only the app (which owns the transport and the
 * user-facing calibration) can supply it. Same argument that put
 * scan_engine_set_imu_extrinsics() here in ABI 8. */
/* --- ABI 10 (ROUND 13): reprocess a sealed container, and the mount check --
 *
 * TWO new functions and two new PODs. Nothing existing changed size, order or
 * meaning, so an ABI-9 consumer that never calls them is byte-compatible.
 *
 *   * scan_lscan_reprocess_d6() — take a SEALED .lscan whose capture broke
 *     into sections, resolve it offline with section stitching on, and leave
 *     the corrected cloud in `processed/map_stitched.bin` beside a
 *     `processed/stitch.json` recording what moved. The sealed streams are
 *     never touched, so "replay == capture" still holds over the raw data and
 *     deleting the two derived files restores exactly what the phone wrote.
 *
 *   * scan_lscan_mount_check() — has the puck been rotated since the mount
 *     reference was set? Judged from where the returns LAND, because the fan's
 *     own attitude is unobservable from the fan (every return leaves the
 *     formula at z == 0). Warns; never refuses.
 *
 * WHY THEY CROSS THE ABI. The Android app reaches the engine's processing
 * through processing_engine.cpp, which links `scanengine` directly — so on
 * that platform these would not need to be here. The desktop shells and every
 * future consumer go through this header, and a correction that only one
 * platform can apply is a correction that will silently diverge. Same argument
 * that put scan_colorizer_run() here. */
/* --- ABI 11 (ROUND 15): live healing, the floor plan, and the ruler --------
 *
 * FIVE new functions, three new PODs, and NOTHING existing changed — in
 * particular `scan_reprocess_options` and `scan_reprocess_result` keep their
 * exact ABI-10 layout, which is why the self-check arrives through a NEW
 * entry point taking a SECOND out-struct rather than through two more fields
 * on the old one. Appending to a POD a caller allocates is not an additive
 * change; adding a function is.
 *
 *   * scan_engine_heal_live_frame() — apply ROUND 13's analytic re-anchor
 *     transform to the LIVE map the instant a break is detected, so the
 *     display stays continuous. The RECORDED pose stream is untouched and an
 *     offline re-resolve is bit-identical either way; see
 *     Engine::heal_live_frame() for the direction argument.
 *   * scan_engine_clear_live_correction() / scan_engine_live_heal_stats().
 *   * scan_lscan_reprocess_d6_ex() — scan_lscan_reprocess_d6() plus the
 *     ROUND 12 self-consistency ruler over the cloud the run just produced.
 *   * scan_lscan_floor_plan() — A12's floor-plan extractor over a sealed
 *     container, writing DXF + PDF + a rendered PNG. */
#define SCAN_ABI_VERSION 12u

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
  SCAN_STREAM_POSE_LIO = 9,
  SCAN_STREAM_IMU_PHONE = 10 /* ROUND 9: the PHONE's gyro/accel. Distinct from
                              * SCAN_STREAM_IMU, which is the Mid-360's — the
                              * offline pipelines route on that distinction. */
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

/* --- colorization: mirrors of color/ (A11), ABI 4 ------------------------
 *
 * The go/no-go input. Mirror of timesync/offset_estimator.h's SyncQuality,
 * and it FAILS CLOSED: SCAN_SYNC_UNKNOWN is what an unconverged estimator
 * reports and what a caller who never wired A4 gets, and the colorizer
 * refuses both (SCAN_ERR_NOT_SUPPORTED). Gate on this, NEVER on a jitter
 * figure — docs/A4-timesync.md §7. */
enum {
  SCAN_SYNC_UNKNOWN = 0, /* not converged: refuse */
  SCAN_SYNC_GOOD = 1,    /* <= 5 ms  : colorize, gate 30 deg/s */
  SCAN_SYNC_GATED = 2,   /* <= 15 ms : colorize, gate 15 deg/s */
  SCAN_SYNC_POOR = 3     /* > 15 ms  : refuse unless allow_poor_sync */
};

/* What happened to one point's colour. */
enum {
  SCAN_COVERAGE_NONE = 0,           /* no acceptable view; keeps its old colour */
  SCAN_COVERAGE_COLORIZED = 1,
  SCAN_COVERAGE_LOW_CONFIDENCE = 2  /* only fast-turn frames saw it (S6 §6.3) */
};

/* Which frame the recorded keyframe poses are in. */
enum {
  SCAN_KEYFRAME_POSE_CAMERA = 0,    /* world_from_camera — what B8 records */
  SCAN_KEYFRAME_POSE_LIDAR_BODY = 1 /* world_from_lidar_body — A6/A7 output */
};

/* Keyframe flags, bit-identical to the frames.idx record's. */
enum {
  SCAN_KEYFRAME_MOTION_VALID = 1,
  SCAN_KEYFRAME_EXPOSURE_VALID = 2,
  SCAN_KEYFRAME_TRACKING_LOST = 4,
  SCAN_KEYFRAME_AUTO_EXPOSURE_LOCKED = 8
};

/* Why the wizard's clock sweep was refused. SCAN_SWEEP_ACCEPTED is the only
 * value that means "use the offset"; the rest are things the wizard must SAY
 * to the operator, which is why they are a verdict and not an error code. */
enum {
  SCAN_SWEEP_ACCEPTED = 0,
  SCAN_SWEEP_TOO_SHORT = 1,
  SCAN_SWEEP_TOO_FEW_SAMPLES = 2,
  SCAN_SWEEP_NO_MOTION = 3,        /* the rig did not move */
  SCAN_SWEEP_NO_SWEEP = 4,         /* moved, but not back and forth */
  SCAN_SWEEP_WEAK_CORRELATION = 5,
  SCAN_SWEEP_AMBIGUOUS = 6,        /* a whole period fits in the search window */
  SCAN_SWEEP_AT_SEARCH_EDGE = 7
};

/* --- jobs: mirror of jobs/job_types.h's JobState (A15), ABI 4 -------------
 *
 * FIVE states, not six. A cancelled job settles into SCAN_JOB_FAILED with
 * error == SCAN_ERR_CANCELLED, the same convention Status/SCAN_TRY use
 * everywhere else in the engine — a UI does not need a second code path to
 * notice a cancellation, it reads the error. This is the encoding
 * scan_event.payload.job.state carries. */
enum {
  SCAN_JOB_QUEUED = 0,
  SCAN_JOB_RUNNING = 1,
  SCAN_JOB_CANCELLING = 2, /* cancel() landed; the cooperative unwind is in flight */
  SCAN_JOB_DONE = 3,
  SCAN_JOB_FAILED = 4
};

/* How a page's points changed (mirror of cloud/point_page.h's PageUpdateKind,
 * ABI 4). A consumer that ignores this is still correct — both kinds carry
 * the same [first, first+count) range and both want a re-upload — but one
 * that caches geometry apart from colour can skip the position upload on a
 * RECOLOURED. */
enum {
  SCAN_PAGE_UPDATE_APPENDED = 0,  /* the range is NEW */
  SCAN_PAGE_UPDATE_RECOLOURED = 1, /* the range existed; only r/g/b/a changed */
  /* ABI 7. The PAGE IS GONE: a live store recycled its oldest page to make
   * room for newer points. `first` is 0 and `count` is how many points left
   * the live window. The page id is never reused, and
   * scan_engine_get_point_page() answers SCAN_ERR_NOT_FOUND for it from now
   * on — so a consumer that ignores update_kind behaves correctly by
   * accident, and one that reads it can free its GPU buffers immediately. */
  SCAN_PAGE_UPDATE_EVICTED = 2
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
      uint8_t update_kind; /* SCAN_PAGE_UPDATE_* (ABI 4) */
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
    /* SCAN_EVENT_JOB_PROGRESS — A15's queue, one update per progress tick of
     * every job kind. `state` is SCAN_JOB_*. Before ABI 4 this event crossed
     * with a ZEROED payload (docs/A15-jobs.md §7.1): the type constant and
     * the static_assert existed, the union case did not. */
    struct {
      uint64_t job_id;
      float progress; /* 0..1, monotone within one run */
      uint8_t state;  /* SCAN_JOB_* */
    } job;
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

/* --- Mid-360 link state: mirror of drivers/mid360/mid360_driver.h, ABI 5 ---
 *
 * Orthogonal to SCAN_DEV_*: a SILENT link shows up as SCAN_DEV_DEGRADED, and a
 * link still silent through a forced re-init as SCAN_DEV_FAULT. */
enum {
  SCAN_MID360_LINK_DOWN = 0,           /* not started, or stopped */
  SCAN_MID360_LINK_WAITING = 1,        /* started; no packet has ever arrived */
  SCAN_MID360_LINK_UP = 2,             /* data within the watchdog window */
  SCAN_MID360_LINK_SILENT = 3,         /* watchdog fired; may still self-heal */
  SCAN_MID360_LINK_REINITIALIZING = 4  /* tearing the SDK down and back up */
};

/* The Mid-360's own counters (ABI 5). scan_device_health answers "is data
 * arriving"; this answers the question a flaky bench session raises — how many
 * watchdog trips, clean resumes and FORCED SDK RE-INITS this capture has had.
 * android/NOTES.md §8 finding 5 and desktop/NOTES.md §8.3 both name this gap: the
 * Engine had no concrete-driver accessor, so these were unreachable from an
 * app even in C++.
 *
 * The two loss figures are NOT the same number: `loss_pct_window` is the last
 * health window's and snaps back after a transient burst, `loss_pct_total` is
 * the session's and does not. A gauge should say which it is showing. */
typedef struct scan_mid360_stats {
  uint8_t link;  /* SCAN_MID360_LINK_* */
  uint8_t state; /* SCAN_DEV_* */

  uint64_t point_packets;
  uint64_t imu_packets;
  uint64_t points_received;    /* before filtering */
  uint64_t points_kept;        /* after filtering, before decimation */
  uint64_t points_appended;    /* what actually reached the page store */
  uint64_t points_dropped_store;
  uint64_t bad_packets;
  uint64_t imu_dropped;

  uint64_t packets_lost;       /* the free-running udp_cnt model */
  uint64_t packets_duplicated;
  uint64_t counter_resets;

  double points_per_sec;
  double points_appended_per_sec;
  double imu_hz;
  double loss_pct_window;
  double loss_pct_total;

  /* The two failure modes S2 separated: a cable pull the SDK recovers from on
   * its own, and a power-cycle it never recovers from. */
  uint64_t watchdog_trips;
  uint64_t clean_resumes;
  uint64_t forced_reinits;
  uint64_t reinit_failures;

  int64_t t_last_point_ns;
  int64_t t_last_imu_ns;
  int64_t t_last_heartbeat_ns; /* the SDK's 1 Hz push-state, a SECOND outage signal */
  int64_t t_silent_since_ns;   /* 0 unless link != UP */

  /* Copied in, NUL-terminated, never a pointer into the engine. */
  char device_sn[32];
  char device_ip[48];
} scan_mid360_stats;

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

/* Which layer owns the Mid-360's sockets. Mirror of
 * drivers/mid360/mid360_driver.h's Mid360Backend, ABI 5.
 *
 * SDK2 (0, the default and what a zeroed struct selects) is the only backend
 * that can bring an OUT-OF-THE-BOX device up: discovery, the host-IP
 * configuration push and the heartbeat live in its command state machine.
 * RAW_UDP is listen-only against a device that is ALREADY configured to stream
 * at this host — it is the backend the pre-bound descriptors below belong to.
 * INJECT owns no transport at all. */
enum {
  SCAN_MID360_BACKEND_SDK2 = 0,
  SCAN_MID360_BACKEND_RAW_UDP = 1,
  SCAN_MID360_BACKEND_INJECT = 2
};

typedef struct scan_device_config {
  int32_t kind; /* SCAN_DEVICE_* */

  /* D6 (serial) */
  const char* serial_port_name;
  uint32_t serial_baud;
  scan_serial_write_cb serial_write; /* may be NULL: engine sends no commands */
  void* serial_write_user_data;
  uint8_t send_start_stop_commands;

  /* Mid-360 (UDP) — A3. Both are REQUIRED: the device is TOLD where to stream
   * and never discovers its host, and there is no broadcast discovery on
   * macOS. */
  const char* lidar_ip;
  const char* host_ip;

  /* --- Mid-360, ABI 5 (android/NOTES.md §8 findings 1, 2 and 4) ------------
   *
   * ZERO IS THE ABI-4 BEHAVIOUR for every field below, with the two
   * exceptions that carry an explicit `_set` flag (a zero there is a real
   * value a caller may want). A memset(0) config is therefore exactly what it
   * was: SDK2, engine-created sockets, the A3 default ports, the A3 default
   * filter and 40k pts/s of live decimation. */
  int32_t mid360_backend; /* SCAN_MID360_BACKEND_* */

  /* THE ANDROID SEAM. A socket the app has ALREADY created, sized
   * (SO_RCVBUF — a pre-bound socket bypasses the engine's own sizing), bound
   * to the host address and bound to the USB-Ethernet Network object
   * (ConnectivityManager TRANSPORT_ETHERNET + Network.bindSocket), because the
   * engine cannot reach ConnectivityManager. -1 (and 0, which is never a
   * socket an app hands over) means "the engine creates its own".
   *
   * OWNERSHIP: the engine NEVER closes a descriptor it did not create. The app
   * closes both AFTER scan_engine_remove_device()/scan_engine_stop() has
   * returned — no receive thread may still be inside recvfrom on them.
   *
   * BOTH are needed for a full capture: SCAN_MID360_BACKEND_RAW_UDP opens two
   * sockets (points and IMU), and one descriptor read by two receive threads
   * has them steal each other's datagrams. Supplying the point fd with no IMU
   * fd while the IMU is enabled is REFUSED (SCAN_ERR_INVALID_ARGUMENT) rather
   * than silently creating an unrouted socket; for a deliberately point-only
   * capture set mid360_publish_imu_set = 1 with mid360_publish_imu = 0.
   * They are only consulted by the raw-UDP backend — SDK2 creates its own
   * sockets inside the vendored SDK (android/NOTES.md §8 finding 3). */
  int32_t mid360_prebound_fd;
  int32_t mid360_prebound_imu_fd;

  /* Ports. 0 = the A3 default for that port. Device-side first (where the
   * lidar listens / sends from), then the host-side ports the device is told
   * to stream to. */
  uint16_t mid360_point_port;      /* 56300 */
  uint16_t mid360_imu_port;        /* 56400 */
  uint16_t mid360_cmd_port;        /* 56100 */
  uint16_t mid360_push_port;       /* 56200 */
  uint16_t mid360_log_port;        /* 56500 */
  uint16_t mid360_host_cmd_port;   /* 56101 */
  uint16_t mid360_host_push_port;  /* 56201 */
  uint16_t mid360_host_point_port; /* 56301 */
  uint16_t mid360_host_imu_port;   /* 56401 */
  uint16_t mid360_host_log_port;   /* 56501 */

  /* SO_RCVBUF request for sockets the ENGINE creates, bytes. 0 = the A3
   * default (4 MB ~ 1.4 s of slack at the Mid-360's ~23 Mbit/s). Ignored for a
   * pre-bound socket: the app sized that one when it bound it. */
  int32_t mid360_recv_buffer_bytes;

  /* Live decimation budget, points/second into the PageStore (Tech Spec §3.3:
   * ~40k of the sensor's 200k). Because 0 legitimately means "no decimation"
   * (post-processing / replay), this one needs the flag. */
  uint8_t mid360_live_points_per_sec_set;
  uint32_t mid360_live_points_per_sec;

  /* IMU publication. Flagged for the same reason: 0 means "point-only", which
   * is a real choice (see the pre-bound descriptors above). */
  uint8_t mid360_publish_imu_set;
  uint8_t mid360_publish_imu;

  /* Point filter (drivers/mid360/mid360_packets.h's PointFilterConfig). The
   * defaults come from real Livox recordings, so a caller that does not care
   * should leave all of this zeroed. */
  uint8_t mid360_filter_set;           /* 0 = keep every default below */
  uint8_t mid360_drop_no_return;       /* default 1 */
  uint8_t mid360_tag_reject_mask;      /* default: the spatial-noise mask */
  uint8_t mid360_min_reflectivity;     /* default 0 */
  float mid360_min_range_m;            /* default 0.10 */
  float mid360_max_range_m;            /* 0 = unbounded (~70 m sensor) */

  /* Verify the packet CRC32 in software. The SDK already did it; the raw-UDP
   * backend has nobody else to trust, so a caller on that path usually wants
   * this on. Flagged because 0 is the default AND a real choice. */
  uint8_t mid360_verify_crc_set;
  uint8_t mid360_verify_crc;

  /* SDK2 only. SDK2's entry point takes a config FILE path; with this empty
   * the driver writes one into the temp directory. On Android there is no
   * writable temp directory unless the app sets TMPDIR (android/NOTES.md §8
   * finding 4 — the work-around this field removes the need for), so an
   * Android caller should point this at a path inside its own cacheDir. NULL
   * or "" keeps the ABI-4 behaviour. */
  const char* mid360_sdk_config_path;
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
/* ABI 5. SCAN_ERR_NOT_FOUND for an unknown id, SCAN_ERR_INVALID_ARGUMENT for a
 * device that is not a Mid-360. Always writes *out (zeroed) first. */
SCAN_API scan_error_t scan_engine_mid360_stats(scan_engine* engine, uint32_t device_id,
                                               scan_mid360_stats* out);

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

/* --- the live point window (ABI 7) ---------------------------------------
 *
 * The store is bounded (scan_engine_config.max_pages). Two things can happen
 * when it fills, and the app chooses:
 *
 *   eviction OFF (default, and what every previous ABI did): the store refuses
 *   every further point and scan_device_health.dropped_page_full climbs. Right
 *   for an OFFLINE store — a post-processing pass that overruns has a bug and
 *   must say so — and catastrophic for a live view, which simply freezes.
 *
 *   eviction ON: the OLDEST page is recycled to make room, so the live view is
 *   a moving window over the newest points and never stops advancing. Memory
 *   is unchanged (one spare page's worth on top of max_pages, and no buffer is
 *   ever freed under a renderer). RECORDING IS A SEPARATE PATH and keeps every
 *   point: turn this on for a live capture, and only for a live capture.
 *
 * Turning eviction on also makes scan_engine_start_session() reset the live
 * window, because live SLAM restarts at the origin every session and the
 * previous session's points are in a frame that no longer exists. */
SCAN_API scan_error_t scan_engine_set_live_page_eviction(scan_engine* engine, uint8_t enabled);

typedef struct scan_page_stats {
  uint32_t pages;             /* pages live right now */
  uint32_t max_pages;         /* the ceiling they are counted against */
  uint64_t resident_points;   /* points readable right now */
  uint64_t total_points;      /* points ever appended */
  uint64_t dropped_points;    /* eviction OFF: points refused at the ceiling */
  uint64_t evicted_pages;     /* eviction ON: pages recycled */
  uint64_t evicted_points;    /* eviction ON: points that left the window */
  uint8_t evicting;           /* 1 once the window started recycling */
  uint8_t eviction_enabled;   /* 1 if this store recycles instead of refusing */
} scan_page_stats;

SCAN_API scan_error_t scan_engine_page_stats(scan_engine* engine, scan_page_stats* out);

/* Empty the live window without freeing a buffer a renderer may be reading
 * (every page is retired exactly as an eviction retires one, with a
 * SCAN_PAGE_UPDATE_EVICTED event each). The lifetime counters above are NOT
 * reset — they are what a support log adds up. */
SCAN_API scan_error_t scan_engine_recycle_live_pages(scan_engine* engine);

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

/* --- the phone IMU in (ABI 8, ROUND 9 item 35) ---------------------------
 *
 * One decoded sample, in the IMU's own frame and SI units — i.e. an Android
 * `SensorEvent` for TYPE_GYROSCOPE fused with the matching TYPE_ACCELEROMETER
 * event, passed through unchanged. `t_mono_ns` is `SensorEvent.timestamp`
 * VERBATIM: it is already CLOCK_BOOTTIME, the same domain ARCore stamps poses
 * in, so the engine maps nothing and a caller that "helpfully" converts it
 * will break the correlation this entire feature depends on.
 *
 * Safe from the SensorEventListener thread while points decode on another. A
 * non-finite sample, or one older than the newest already held, is rejected
 * with SCAN_ERR_INVALID_ARGUMENT rather than corrupting the ring — Android
 * genuinely delivers out-of-order events across sensor types, so this is an
 * expected return, not a bug indicator.
 *
 * Pushing nothing is always legal: with an empty ring the engine's trajectory
 * is exactly the ABI-7 one. */
SCAN_API scan_error_t scan_engine_push_imu(scan_engine* engine, int64_t t_mono_ns,
                                           const float gyro_rad_s[3],
                                           const float accel_m_s2[3]);

/* The rotation taking a vector in the IMU's frame to the frame ARCore reports
 * its pose in, as a unit quaternion (x, y, z, w). The identity is correct ONLY
 * for a synthetic stream or a device where the sensor and camera frames
 * coincide; on a real phone they are usually 90 degrees apart
 * (CameraCharacteristics.SENSOR_ORIENTATION), and only the app knows which way.
 *
 * A wrong value cannot corrupt the pose knots — those stay pinned to ARCore —
 * but it distorts the interpolated path between them, which is the whole value
 * being added. Non-finite or zero-norm is SCAN_ERR_INVALID_ARGUMENT.
 *
 * Applying it REBUILDS the densifier. ROUND 18: the buffered IMU samples are
 * CARRIED ACROSS the rebuild — they are raw sensor-frame measurements and the
 * extrinsic is applied at integration time, so they are valid under the new
 * value, and dropping them shortened the tracking-gap bridge's reach by
 * exactly the samples pushed before the app applied the extrinsic (~24 ms
 * after start on the owner's captures, and the whole preview on a future
 * caller). The estimated gyro bias is still reset — it was learned under the
 * old extrinsic. Call it once during setup, before scan_engine_start(),
 * exactly like the mount extrinsic. */
SCAN_API scan_error_t scan_engine_set_imu_extrinsics(scan_engine* engine,
                                                     const double quat_xyzw[4]);

/* Mirror of scanengine::ImuDensifyStats. The four `fallback_*` counters are
 * the field diagnosis and they mean different things to an operator:
 * `no_imu` = nothing is pushing samples, `gap` = the sensor is stuttering,
 * `bracket` = ARCore dropped poses, `closing` = the gyro and ARCore disagree
 * by more than any real rig should, which almost always means the extrinsic
 * above is wrong.
 *
 * ROUND 14: these four do NOT sum to `fallbacks`, and on a real capture they
 * account for well under half of it — the rest is the wrapped pose source
 * having nothing usable at that instant, which is a trajectory problem rather
 * than an IMU one. The C++ ImuDensifyStats splits that out as
 * `fallback_no_pose` / `fallback_gate`; they are deliberately NOT mirrored
 * here, because this struct's size is part of a frozen ABI. A C consumer that
 * needs the whole accounting should read `fallbacks` as the total and treat
 * the difference as "no usable pose". */
typedef struct scan_imu_densify_stats {
  uint64_t samples_in;
  uint64_t samples_rejected; /* non-finite, or out of order */
  uint64_t queries;
  uint64_t densified;        /* placed on the gyro path */
  uint64_t fallbacks;        /* fell through to plain interpolation */
  uint64_t fallback_no_imu;
  uint64_t fallback_gap;
  uint64_t fallback_bracket;
  uint64_t fallback_closing;
  uint64_t bias_updates;
  double bias_rad_s[3];      /* the estimated gyro bias, IMU frame */
  double worst_closing_deg;  /* worst gyro-vs-ARCore disagreement over a bracket */
  double mean_closing_deg;
} scan_imu_densify_stats;

SCAN_API scan_error_t scan_engine_imu_densify_stats(scan_engine* engine,
                                                    scan_imu_densify_stats* out);

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

/* --- the lidar -> pose time offset (ABI 9, ROUND 10 item 36) --------------
 *
 * Nanoseconds ADDED to a return's own timestamp before its pose is looked up.
 * POSITIVE means "the return was really taken later than its stamp says, pair
 * it with a later pose", which is the sign a transport delay takes: the D6
 * has no clock of its own, so a return is dated when the phone's reader
 * thread sees its bytes, which is after the mirror actually swept past.
 *
 * WHAT IT COSTS TO GET WRONG, and why it hides. A constant offset `dt` moves
 * the cloud `v*dt` along the walk — 2 cm at 1 m/s and 20 ms, the same shift
 * for every point, so walls stay straight and nothing looks wrong — and
 * rotates it `omega*dt` about the operator, which at a 60 deg/s turn and
 * 20 ms is 1.2 deg, i.e. 6 cm of tangential error at 3 m, with the SIGN
 * FOLLOWING THE TURN. So it is invisible walking in a straight line and
 * obvious the moment the operator turns around, which is exactly how the
 * owner described it.
 *
 * Settable at any time, including mid-capture: it takes effect for every
 * point resolved after the call, including points still pending. There is no
 * reconnect and no session restart, because the app exposes this as a live
 * calibration and losing a capture to change a number would be absurd.
 *
 * Zero is the ABI-8 behaviour. `engine/tools/engine_cli.cpp --d6-timesweep`
 * measures the right value from a real capture. */
SCAN_API scan_error_t scan_engine_set_pose_time_offset_ns(scan_engine* engine,
                                                          int64_t offset_ns);
/* Writes the current offset. SCAN_ERR_INVALID_ARGUMENT if `out` is NULL. */
SCAN_API scan_error_t scan_engine_pose_time_offset_ns(scan_engine* engine, int64_t* out);

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

/* The survey profile's EPSG picker (Tech Spec §3.4), ABI 5.
 *
 * The engine can render a WKT for WGS 84 and the UTM zones and NOTHING else —
 * it ships no PROJ and no proj.db (gnss/crs.h explains the trade). A national
 * grid (HK1980 EPSG:2326, OSGB36 EPSG:27700, RD New EPSG:28992, ...) is
 * therefore only expressible if the CALLER supplies the WKT its own geodetic
 * authority publishes. This is the way in, and before ABI 5 there was none
 * from C at all (docs/INT29-wiring.md §7 item 5).
 *
 *   scan_engine_set_crs(e, "EPSG:2326", wkt)  label exports with that CRS
 *   scan_engine_set_crs(e, "EPSG:32650", "")  an EPSG the engine can render
 *   scan_engine_set_crs(e, NULL, NULL)        clear; back to auto-UTM
 *
 * Both strings are COPIED (convention 3). NULL is treated as "". VALIDATED:
 * SCAN_ERR_INVALID_ARGUMENT for an EPSG string that does not parse, for a WKT
 * that is not one (a PROJ.4 string, a bare code, a truncated paste), and for
 * an EPSG the engine cannot render supplied with no WKT — that combination
 * silently produces an unlabelled export, which is the failure this call
 * exists to prevent.
 *
 * It changes WHAT label a georeferenced cloud gets, never WHETHER an
 * ungeoreferenced one may be labelled: scan_engine_crs_wkt() and
 * scan_engine_crs_epsg() stay EMPTY until the georef transform converges
 * either way. */
SCAN_API scan_error_t scan_engine_set_crs(scan_engine* engine, const char* epsg,
                                          const char* wkt);

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

/* --- camera keyframes (A11), ABI 4 ---------------------------------------
 *
 * ONE record of `streams/frames/frames.idx`, the index B8's capture path
 * writes and any platform reads. docs/A11-color.md §3 is the format; this
 * struct is its in-memory face.
 *
 * `t_engine_ns` is the exposure time of image ROW 0 (not the frame centre),
 * on the engine clock — that convention is what makes the rolling-shutter
 * model need no second reference point: row r is exposed at
 * t_engine_ns + r * rolling_shutter_row_time_ns.
 *
 * `position`/`orientation` are world_from_camera; the quaternion is x, y, z, w
 * and must be unit length. `image_name` is RELATIVE TO streams/frames/ (a bare
 * "kf_000123.jpg"), forward slashes, no "..", not absolute — the same
 * zip-slip class zip_import() defends against, because an index is just as
 * attacker-supplied as an archive. Convention 3 holds: the string is copied
 * before this call returns. */
typedef struct scan_keyframe {
  int64_t t_engine_ns;
  double position[3];
  double orientation[4]; /* x, y, z, w — unit */
  float fx, fy, cx, cy;
  float distortion[5];   /* k1, k2, p1, p2, k3 — OpenCV/ARCore order */
  uint32_t width, height;
  float rolling_shutter_row_time_ns; /* 0 = global shutter */
  float position_sigma_m, orientation_sigma_deg;
  uint8_t pose_quality;   /* SCAN_POSE_QUALITY_* */
  uint8_t tracking_lost;  /* 0/1 */
  uint8_t pose_source;    /* SCAN_STREAM_* */
  uint32_t flags;         /* SCAN_KEYFRAME_* */
  int64_t exposure_ns;
  float iso, angular_rate_rad_s, linear_speed_m_s;
  uint32_t image_bytes;   /* JPEG size on disk; 0 = unknown */
  const char* image_name; /* NUL-terminated UTF-8; NULL is refused */
} scan_keyframe;

/* Encodes `kf` and writes it to the session's recorder as a
 * kCameraFrameIndex chunk — exactly encode_keyframe_record() +
 * write_chunk(), which is what makes B8 a capture task rather than a format
 * task (docs/A11-color.md §3.1). Requires a running session that is
 * recording; SCAN_ERR_INVALID_STATE otherwise. A keyframe that could not be
 * projected (non-unit quaternion, principal point outside the image, an
 * absolute name or one with a ".." component, ...) is SCAN_ERR_INVALID_ARGUMENT and is
 * NOT written: an unusable record on disk can only be skipped silently
 * later. */
SCAN_API scan_error_t scan_engine_record_keyframe(scan_engine* engine, const scan_keyframe* kf);

/* --- colorization (A11), ABI 4 -------------------------------------------
 *
 * The knobs a caller actually chooses. Everything else in
 * color::ColorizeConfig keeps its C++ default — those are S6-derived
 * constants (the 75 deg incidence cut, the 1/8 depth scale, the slope bias,
 * the scoring weights), not per-session choices, and exposing them here would
 * invite a JNI caller to tune away a measured result.
 *
 * ZERO-INITIALIZING THIS STRUCT IS SAFE AND REFUSES: sync_quality 0 is
 * SCAN_SYNC_UNKNOWN, which fails closed (docs/A11-color.md §2). Non-zero
 * motion_gate_deg_s / motion_reject_deg_s override the policy the sync
 * quality selects; 0 takes the policy's. */
typedef struct scan_colorize_config {
  uint8_t sync_quality;    /* SCAN_SYNC_* — from scan_engine timesync/A4 */
  uint8_t allow_poor_sync; /* 0/1 — the operator override behind SCAN_SYNC_POOR */
  uint8_t pose_frame;      /* SCAN_KEYFRAME_POSE_* */
  float motion_gate_deg_s;
  float motion_reject_deg_s;
  int64_t camera_clock_offset_ns; /* t_engine = t_camera + this */
  float min_range_m;              /* 0 = keep the default (0.30) */
  float max_range_m;              /* 0 = keep the default (30.0) */
  uint8_t occlusion_test;         /* 0/1 */
  uint8_t rolling_shutter;        /* 0/1 */
  uint8_t estimate_normals;       /* 0/1 */
  uint8_t low_confidence_alpha;   /* 0 = leave alpha alone (A8/A14 own it) */
  uint8_t uncovered_alpha;        /* 0 = leave alpha alone */
} scan_colorize_config;

typedef struct scan_colorize_stats {
  uint64_t points_total;
  uint64_t points_colorized;
  uint64_t points_low_confidence;
  uint64_t points_uncovered;
  uint32_t keyframes_total;
  uint32_t keyframes_used;
  uint32_t keyframes_rejected_motion;
  uint32_t keyframes_rejected_pose;
  uint32_t keyframes_outside_cloud;
  uint32_t keyframes_image_failed;
  double ms_total;
  float coverage_fraction;
} scan_colorize_stats;

/* Progress, 0..1, monotone within one run. Invoked ON THE THREAD INSIDE
 * scan_colorizer_run() — which is the caller's thread, since the run is
 * blocking. Quick, no re-entry, and on Android attach before touching the
 * JVM. */
typedef void (*scan_colorize_progress_cb)(float fraction, void* user_data);

typedef struct scan_colorizer scan_colorizer;

/* A NULL `cfg` means "all defaults", which refuses (SCAN_SYNC_UNKNOWN). */
SCAN_API scan_error_t scan_colorizer_create(scan_colorizer** out,
                                            const scan_colorize_config* cfg);
SCAN_API void scan_colorizer_destroy(scan_colorizer* c);

/* Loads every keyframe from <lscan_dir>/streams/frames/frames.idx and installs
 * a file-backed image source rooted at lscan_dir. SCAN_ERR_NOT_FOUND means the
 * session has no camera — Tech Spec §3.5's "gracefully unavailable", which a
 * caller should REPORT, not treat as a failure. */
SCAN_API scan_error_t scan_colorizer_load_keyframes(scan_colorizer* c, const char* lscan_dir);

/* The mount extrinsic, ROW-MAJOR 4x4 and rigid. Used when
 * pose_frame == SCAN_KEYFRAME_POSE_LIDAR_BODY; validated (and refused if
 * non-rigid) either way — a column-major matrix handed across JNI produces a
 * plausible-looking mirrored cloud nobody notices until export. */
SCAN_API scan_error_t scan_colorizer_set_extrinsics(scan_colorizer* c,
                                                    const double camera_from_lidar[16]);

SCAN_API scan_error_t scan_colorizer_set_progress_callback(scan_colorizer* c,
                                                           scan_colorize_progress_cb cb,
                                                           void* user_data);

/* BLOCKING: colours every page of `engine`'s PageStore in place, on the
 * CALLING thread, and then notifies the store's subscribers that the colours
 * changed so a live renderer re-uploads. Call it from a worker thread — an
 * app that wants a queue should drive a Colorize job through the C++
 * jobs::JobQueue instead (there is no C surface for the queue at ABI 4).
 *
 * SCAN_ERR_NOT_SUPPORTED when the sync gate refuses — before any image is
 * decoded and any point is touched. SCAN_ERR_CANCELLED after
 * scan_colorizer_cancel(), with the points left exactly as they were. */
SCAN_API scan_error_t scan_colorizer_run(scan_colorizer* c, scan_engine* engine);

/* Safe from another thread while run() is in flight — that is the whole
 * point. Sticky: a colorizer cancelled once stays cancelled. */
SCAN_API scan_error_t scan_colorizer_cancel(scan_colorizer* c);
SCAN_API float scan_colorizer_progress(scan_colorizer* c);
SCAN_API scan_error_t scan_colorizer_stats(scan_colorizer* c, scan_colorize_stats* out);

/* --- the wizard's clock sweep (A11, for B7), ABI 4 ------------------------
 *
 * One sample of "how fast the thing is turning", on that sensor's own clock.
 * The units are arbitrary and are normalised away: in the wizard these are
 * the target's bearing rate in the camera against its bearing rate in the
 * lidar; on a bench they can be ARCore's gyro against the Mid-360's IMU. */
typedef struct scan_rate_sample {
  int64_t t_ns;
  double value;
} scan_rate_sample;

/* SIGN CONVENTION, asserted by a test rather than a comment:
 *     t_engine_ns = t_camera_ns + offset_ns
 * i.e. a positive offset means the camera clock reads EARLY relative to the
 * lidar's. Feed it back as scan_colorize_config::camera_clock_offset_ns. */
typedef struct scan_clock_sweep_result {
  int64_t offset_ns;
  double correlation;
  double rival_correlation;
  double sigma_ns; /* honest to an order of magnitude — the wizard's "+-2 ms" */
  uint32_t grid_samples;
  uint32_t zero_crossings;
  int64_t overlap_ns;
  uint8_t accepted;
  uint8_t verdict; /* SCAN_SWEEP_* */
} scan_clock_sweep_result;

/* Both tracks must be sorted by t_ns and finite. Returns SCAN_OK with
 * `accepted == 0` and a verdict when the CAPTURE was not good enough — "the
 * user did not sweep" is something the wizard must say, not an error to
 * propagate. SCAN_ERR_INVALID_ARGUMENT is reserved for structurally bad
 * input (unsorted, empty, non-finite). Uses the estimator's default config
 * (+-100 ms search, 2 ms grid, 4 s minimum overlap). */
SCAN_API scan_error_t scan_clock_sweep_estimate(const scan_rate_sample* camera, uint32_t n_camera,
                                                const scan_rate_sample* lidar, uint32_t n_lidar,
                                                scan_clock_sweep_result* out);

/* --- device auto-discovery (A16), ABI 6 -----------------------------------
 *
 * The mirror of discovery/discovery.h. Nothing here needs an engine handle:
 * discovery runs BEFORE there is a session to configure, which is the whole
 * point. Every one of these calls BLOCKS for up to the timeout it is given —
 * call them from a worker, never from a UI thread.
 *
 * Convention 4 does not apply to these structs: they carry FIXED CHAR ARRAYS,
 * copied by value, so a JNI caller can hold them past the next engine call. A
 * string longer than its array is truncated, never overflowed. */

/* Where the Mid-360 broadcasts its heartbeat. Mirrored from
 * discovery::kMid360PushPort and static_asserted against it. */
#define SCAN_MID360_PUSH_PORT 56201u
#define SCAN_MID360_PUSH_PORT_ALT 56200u

typedef struct scan_mid360_beacon {
  char sn[32];              /* "ARMCP7K0034759" */
  char dev_type[32];        /* "Mid-360" */
  char fw_version[32];      /* "35.1.1.8" */
  char fw_version_text[32]; /* "35010108", the firmware's own spelling */
  char build_time[32];
  char mac[24];
  char lidar_ip[16];
  char netmask[16];
  char gateway[16];
  /* The address the lidar HAS PERSISTED and will stream to. Feed it to
   * scan_host_check() before doing anything else with it. */
  char persisted_host_ip[16];
  char source_ip[16];  /* who sent the datagram */
  uint16_t persisted_point_port;
  uint16_t persisted_imu_port;
  uint16_t push_port_seen; /* the local port it arrived on */
  uint16_t key_count;
  int64_t t_last_seen_ns;
  uint32_t beacons_seen;
  uint8_t crc_ok;    /* header CRC16 and payload CRC32 both verified */
  uint8_t heuristic; /* recovered by the fallback scan, treat as advisory */
} scan_mid360_beacon;

/* Listen for `timeout_ms` and write at most `capacity` records, dedup'd by
 * serial number. *out_count is always the TRUE number found, so a truncated
 * call (SCAN_ERR_CAPACITY_EXCEEDED) can be retried with a bigger array — the
 * same contract as scan_ntrip_fetch_sourcetable.
 *
 * SCAN_OK with *out_count == 0 means "nothing is broadcasting", which is an
 * answer, not an error. SCAN_ERR_BUSY means the listen port could not be
 * bound at all — another LidarScan, or Livox Viewer, is holding it. */
SCAN_API scan_error_t scan_discover_mid360(uint32_t timeout_ms, scan_mid360_beacon* out,
                                           uint32_t capacity, uint32_t* out_count);

#define SCAN_HOST_CHECK_MAX_CANDIDATES 8

typedef struct scan_host_check_result {
  uint8_t host_ip_is_local;  /* this machine holds the persisted host address */
  uint8_t on_lidar_subnet;   /* ...or at least is on the lidar's subnet */
  uint32_t candidate_count;  /* how many of `candidates` are filled */
  char candidates[SCAN_HOST_CHECK_MAX_CANDIDATES][16];
  char suggested_host_ip[16];
  char suggested_interface[64];
  char note[512]; /* one operator-readable sentence; always non-empty */
} scan_host_check_result;

/* Enumerates this machine's interfaces and compares. Never fails because the
 * answer is bad news — a machine on the wrong network gets SCAN_OK and a note
 * that says so. */
SCAN_API scan_error_t scan_host_check(const scan_mid360_beacon* beacon,
                                      scan_host_check_result* out);

typedef struct scan_serial_port {
  char path[256]; /* "/dev/cu.usbserial-21130", "COM7" */
} scan_serial_port;

/* Never fails: a machine with no serial ports reports zero of them.
 * SCAN_ERR_CAPACITY_EXCEEDED if `capacity` was too small; *out_count is the
 * true total either way. */
SCAN_API scan_error_t scan_enumerate_serial(scan_serial_port* out, uint32_t capacity,
                                            uint32_t* out_count);

typedef struct scan_d6_probe {
  char port[256];
  uint32_t baud; /* always 230400 — the D6 has one rate */
  uint32_t packets_ok;
  uint32_t packets_bad_checksum;
  uint8_t used_start_command; /* the probe had to ask; see discovery.h */
} scan_d6_probe;

typedef struct scan_um982_probe {
  char port[256];
  uint32_t baud; /* whatever the sweep found — 230400 on the real unit */
  uint32_t sentences_ok;
  uint32_t sentences_bad;
  uint8_t has_heading; /* a dual-antenna heading sentence was seen */
} scan_um982_probe;

/* Probe each of `n_ports` paths for `per_port_ms` and report the FIRST match.
 * SCAN_ERR_NOT_FOUND means none of them is that device — a normal answer, and
 * the reason these do not return SCAN_ERR_IO for an unopenable port. A port
 * that is busy is skipped silently.
 *
 * scan_probe_d6 may WRITE the D6's 4-byte start command to a port that stayed
 * silent AND did not look like a text protocol, and always writes the stop
 * command afterwards. scan_probe_um982 never writes. */
SCAN_API scan_error_t scan_probe_d6(const char* const* ports, uint32_t n_ports,
                                    uint32_t per_port_ms, scan_d6_probe* out);
SCAN_API scan_error_t scan_probe_um982(const char* const* ports, uint32_t n_ports,
                                       uint32_t per_port_ms, scan_um982_probe* out);

/* --- single-instance guard (A16, owner round-4 item 6), ABI 6 --------------
 *
 * PROCESS-GLOBAL, like the log callback: one claim per process, released by
 * scan_instance_release() or by process exit. `app_id` may be NULL for the
 * default ("lidarscan"); `lock_path` may be NULL to derive it from app_id in
 * the temp directory.
 *
 * SCAN_ERR_BUSY means another LidarScan holds it; *out_holder_pid (optional)
 * is that process, and scan_engine_last_error() is the sentence to show:
 * "another LidarScan is running (pid 4242)". Calling acquire twice in one
 * process is SCAN_OK — see instance_guard.h on why that is not a violation. */
SCAN_API scan_error_t scan_instance_acquire(const char* app_id, const char* lock_path,
                                            int64_t* out_holder_pid);
SCAN_API void scan_instance_release(void);
SCAN_API int64_t scan_current_process_id(void);

/* Logging is process-global, not per engine. */
SCAN_API void scan_engine_set_log_callback(scan_log_cb cb, void* user_data, int32_t min_level);

/* ======================================================================== */
/* ROUND 13 (ABI 10): reprocess a sealed container + the mount watchdog      */
/* ======================================================================== */

/* Mirrors post::SeamDecision. STABLE, APPEND-ONLY. */
enum {
  SCAN_SEAM_ANALYTIC = 0,
  SCAN_SEAM_REFINED = 1,
  SCAN_SEAM_NO_TRAJECTORY = 2,
  SCAN_SEAM_THIN_SUBMAP = 3,
  SCAN_SEAM_UNOBSERVABLE = 4,
  SCAN_SEAM_NOT_CONVERGED = 5,
  SCAN_SEAM_REFINEMENT_TOO_BIG = 6,
  SCAN_SEAM_MAP_GOT_WORSE = 7
};

/* Mirrors post::MountWatchVerdict. STABLE, APPEND-ONLY. */
enum {
  SCAN_MOUNT_OK = 0,
  SCAN_MOUNT_NOT_MEASURABLE = 1,
  SCAN_MOUNT_SUSPECT = 2,
  SCAN_MOUNT_MISMATCH = 3
};

typedef struct scan_reprocess_options {
  uint8_t stitch_sections; /* 1 = put the sections back into one frame */
  uint8_t close_loops;     /* runs AFTER stitching; off by default */
  uint8_t densify_with_phone_imu;
  uint8_t refine_seams; /* the translation-only, rotation-locked solve */
  double max_refine_translation_m; /* <= 0 uses the engine default (0.30) */
} scan_reprocess_options;

typedef struct scan_reprocess_result {
  uint8_t ran;         /* the pipeline resolved */
  uint8_t map_written; /* processed/map_stitched.bin was (re)written */
  uint32_t sections;   /* 1 = the capture never broke; nothing was moved */
  uint32_t seams;
  uint32_t seams_refined; /* the rest kept the analytic transform, by name */
  uint64_t points;
  uint64_t poses;
  uint64_t poses_untracked; /* recorded with no world frame at all */

  /* What the correction moved the FIRST section by, into the last section's
   * frame. This is the "your map was this far apart" number. */
  double first_section_moved_m;
  double first_section_moved_deg;

  /* The check that does NOT come from the same measurement: the operator
   * walks on a flat floor, so this must SHRINK. */
  double vertical_extent_before_m;
  double vertical_extent_after_m;

  /* Start-to-end gap. Before stitching this compares two points in different
   * world frames and means nothing; after, it is ARCore's own drift. */
  double end_gap_before_m;
  double end_gap_after_m;

  /* The mount watchdog, run for free on the same cloud. */
  int32_t mount_verdict; /* SCAN_MOUNT_* */
  double mount_impossible_fraction;
  double mount_revolution_extent_m;
} scan_reprocess_result;

typedef struct scan_mount_check_result {
  int32_t verdict; /* SCAN_MOUNT_* */
  uint32_t revolutions;
  uint64_t points;
  double median_revolution_extent_m;
  double impossible_fraction;
  double median_range_m;
} scan_mount_check_result;

/* Progress, 0..1, called on the caller's thread inside the run. May be NULL.
 * Returning 0 cancels; returning non-zero continues. */
typedef int32_t (*scan_reprocess_progress_cb)(float fraction, void* user_data);

/* Resolve `lscan_dir` offline with section stitching and write the corrected
 * cloud into `processed/`. `opts` may be NULL for the defaults (stitch on,
 * refine on, loops off). Tens of seconds on a phone for a one-minute walk. */
SCAN_API scan_error_t scan_lscan_reprocess_d6(const char* lscan_dir,
                                              const scan_reprocess_options* opts,
                                              scan_reprocess_result* out,
                                              scan_reprocess_progress_cb progress,
                                              void* user_data);

/* True (1) when the container already carries a stitched cloud. */
SCAN_API int32_t scan_lscan_has_stitched_cloud(const char* lscan_dir);

/* The mount watchdog over the first `window_seconds` of a container (0 = the
 * whole capture). Judged from where the returns land; warns, never refuses. */
SCAN_API scan_error_t scan_lscan_mount_check(const char* lscan_dir, double window_seconds,
                                             scan_mount_check_result* out);

/* ======================================================================== */
/* ROUND 15 (ABI 11): live re-anchor healing, the ruler, and the floor plan  */
/* ======================================================================== */

/* --- item 54: live healing ---------------------------------------------- */

/* Fold ARCore's own re-anchor transform into the LIVE world frame so the map
 * on screen does not shatter. `before` and `after` are the two poses that
 * straddle the jump, as the caller's detector found them.
 *
 * The recorded pose stream is NOT affected: scan_engine_push_pose() records
 * what you pushed, before any correction, so a replay of the container
 * reproduces the capture exactly. Section bookkeeping is unaffected, so the
 * offline stitch still runs and still improves on this.
 *
 * SCAN_ERR_INVALID_ARGUMENT — with the correction left exactly as it was —
 * when the pair cannot define a rigid transform (a pose the tracker disowned,
 * a degenerate rotation, out-of-order stamps). That is the case in which the
 * operator SHOULD be told, because nothing could be done about it. */
SCAN_API scan_error_t scan_engine_heal_live_frame(scan_engine* engine,
                                                  const scan_pose* before,
                                                  const scan_pose* after);

/* Back to identity. Also done automatically by scan_engine_start_session(). */
SCAN_API scan_error_t scan_engine_clear_live_correction(scan_engine* engine);

typedef struct scan_live_heal_stats {
  uint32_t applied;  /* breaks folded into the live frame */
  uint32_t refused;  /* breaks with no usable pose bracket — cue these */
  uint8_t active;
  /* What the accumulated correction moves a point by, i.e. how far apart the
   * live map WOULD have been without it. */
  double translation_m;
  double rotation_deg;
  double matrix[16]; /* row-major 4x4 */
} scan_live_heal_stats;

SCAN_API scan_error_t scan_engine_live_heal_stats(scan_engine* engine,
                                                  scan_live_heal_stats* out);

/* --- item 57: the ROUND 12 ruler, on the reprocess path ------------------ */

typedef struct scan_selfcheck_result {
  uint8_t measurable;        /* 0 = nothing could be compared; see `blocker` */
  uint32_t windows;
  uint32_t nearest_separation;
  uint32_t cells;            /* shared planar cells that voted */
  double window_seconds;
  double nearest_offset_m;   /* THE number: how far the map disagrees with itself */
  double p90_offset_m;
  double self_floor_m;       /* the measurement's own floor on this capture */
  char blocker[96];          /* "" when measurable */
} scan_selfcheck_result;

/* scan_lscan_reprocess_d6() plus the self-consistency measurement over the
 * cloud this run produced. `selfcheck` may be NULL (then it is not computed,
 * and the call is exactly scan_lscan_reprocess_d6()). */
SCAN_API scan_error_t scan_lscan_reprocess_d6_ex(const char* lscan_dir,
                                                 const scan_reprocess_options* opts,
                                                 scan_reprocess_result* out,
                                                 scan_selfcheck_result* selfcheck,
                                                 scan_reprocess_progress_cb progress,
                                                 void* user_data);

/* --- item 56: the floor plan --------------------------------------------- */

/* Which drawing came out. STABLE, APPEND-ONLY. */
enum {
  SCAN_PLAN_MODE_WALLS = 0,   /* walls were fitted */
  SCAN_PLAN_MODE_DENSITY = 1  /* nothing fitted; the returns themselves are the map */
};

typedef struct scan_plan_options {
  double slice_min_m; /* 0 = engine default (1.0) */
  double slice_max_m; /* 0 = engine default (1.5) */
  double grid_res_m;  /* 0 = engine default (0.02) */
  uint8_t up_axis;    /* 0 = Z, 1 = Y (ARCore, and the default for 0), 2 = X */
  uint8_t write_dxf;
  uint8_t write_pdf;
  uint8_t write_png;
  uint32_t png_max_px; /* 0 = 1600 */
  const char* out_dir;   /* NULL = <lscan_dir>/processed */
  const char* base_name; /* NULL = "floorplan" */
  const char* title;     /* NULL = "" */
} scan_plan_options;

typedef struct scan_plan_result {
  uint8_t ran;
  uint8_t mode;                 /* SCAN_PLAN_MODE_* */
  uint8_t walls_from_floor_map; /* the 1.2 m cut was too sparse; see below */
  uint8_t no_room_closed;

  uint64_t cloud_points;
  uint64_t band_points;   /* points inside the plan slice */
  uint64_t map_points;    /* points inside the floor-map band */
  uint32_t occupied_cells;
  uint32_t map_cells;

  uint32_t walls;
  uint32_t walls_paired; /* both faces scanned -> MEASURED thickness */
  uint32_t openings;
  uint32_t doors;
  uint32_t windows;
  uint32_t rooms;
  double total_wall_length_m;
  double total_room_area_m2;
  double largest_room_area_m2;
  double extent_x_m;
  double extent_y_m;
  double png_px_per_m;
  double png_scale_bar_m;
  uint32_t png_w;
  uint32_t png_h;

  char png_path[512];
  char pdf_path[512];
  char dxf_path[512];
  char cloud_source[64];
} scan_plan_result;

/* Extract A12's floor plan from a SEALED container and write the requested
 * files. Prefers `processed/map_stitched.bin` when the container has been
 * processed, then the live map cache, then a full re-resolve.
 *
 * A capture whose plan slice fits no wall is NOT an error: the call returns
 * SCAN_OK with `mode == SCAN_PLAN_MODE_DENSITY` and a PNG of the occupancy at
 * a stated metric scale. DXF and PDF are written only when something was
 * fitted — the corresponding path is empty otherwise. */
SCAN_API scan_error_t scan_lscan_floor_plan(const char* lscan_dir,
                                            const scan_plan_options* opts,
                                            scan_plan_result* out);


/* ======================================================================== */
/* ROUND 17 (ABI 12): why a re-anchor was healed, or refused                */
/* ======================================================================== */

/* ADDITIVE. scan_engine_heal_live_frame() is unchanged in signature and its
 * meaning is unchanged in the case that matters — a short gap still folds
 * ARCore's own transform into the live frame. What changed underneath it is
 * the LONG gap: across the owner's scan-040 the tracker was blind for 6.065 s,
 * and the 66.21 deg it had left over when it came back was the leftover of a
 * 145 deg turn the operator really made, not a frame correction. Applying it
 * rotated his room. poses/reanchor.h has the measurement.
 *
 * So heal_live_frame() now bridges a long gap with the recorded gyro and heals
 * only the residual, and refuses outright when the two witnesses disagree by
 * more than a re-anchor can be. A Status can carry "refused"; it cannot carry
 * the six numbers that justify the refusal, and those numbers belong in the
 * capture log while the walk is still happening. This is where they are.
 *
 * Cleared by scan_engine_clear_live_correction() and by starting a session. */

typedef enum scan_gap_verdict {
  SCAN_GAP_SNAP = 0,           /* short gap: ARCore's transform, applied */
  SCAN_GAP_BRIDGED = 1,        /* long gap, gyro agreed: residual applied */
  SCAN_GAP_NEGLIGIBLE = 2,     /* long gap: the jump was the operator. NOT a refusal */
  SCAN_GAP_REFUSED_NO_GYRO = 3,
  SCAN_GAP_REFUSED_TOO_LONG = 4,
  SCAN_GAP_REFUSED_DISAGREE = 5, /* scan-040 */
  SCAN_GAP_REFUSED_DEGENERATE = 6
} scan_gap_verdict;

typedef struct scan_reanchor_info {
  uint8_t valid;      /* 0 = no re-anchor has been resolved this session */
  uint8_t gyro_used;
  int32_t verdict;    /* scan_gap_verdict */
  double gap_s;
  double reported_translation_m;
  double reported_rotation_deg;
  double gyro_rotation_deg;      /* what the operator actually turned */
  double residual_translation_m; /* what was (or would have been) healed */
  double residual_rotation_deg;
  double walk_bound_m;           /* how far a walk could have carried them */
} scan_reanchor_info;

SCAN_API scan_error_t scan_engine_last_reanchor(scan_engine* engine, scan_reanchor_info* out);

/* One word for a verdict, for a log line. Never NULL. */
SCAN_API const char* scan_gap_verdict_str(int32_t verdict);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SCANENGINE_C_H */
