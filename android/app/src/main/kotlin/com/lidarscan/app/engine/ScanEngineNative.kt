package com.lidarscan.app.engine

import java.nio.ByteBuffer

/**
 * JNI surface over `engine/capi/scanengine_c.h`, bound by
 * `android/app/src/main/cpp/scanengine_jni.cpp`. This is a thin 1:1
 * transliteration of the C ABI (engine handles become `Long`, `scan_error_t`
 * becomes `Int`, strings/buffers marshal as documented per-method) — no
 * business logic lives here. [RealEngineBridge] is the layer that turns this
 * into the `EngineBridge` contract.
 *
 * Pinned against `SCAN_ABI_VERSION` 1 as of B2 (checked again natively in
 * `nativeCreateEngine`); see android/NOTES.md's "C ABI gaps" section for
 * what the C ABI does *not* expose yet.
 */
object ScanEngineNative {

    /** True once [System.loadLibrary] for `scanengine_jni` has succeeded. Checked once, at class init. */
    val isAvailable: Boolean = try {
        System.loadLibrary("scanengine_jni")
        true
    } catch (e: UnsatisfiedLinkError) {
        false
    } catch (e: SecurityException) {
        false
    }

    // --- engine lifecycle ----------------------------------------------------
    external fun nativeAbiVersion(): Int
    external fun nativeVersionString(): String
    external fun nativeLastError(): String
    external fun nativeErrorStr(code: Int): String

    external fun nativeCreateEngine(
        appName: String?,
        logLevel: Int,
        pageCapacity: Int,
        maxPages: Int,
        eventQueueCapacity: Int,
    ): Long

    external fun nativeDestroyEngine(handle: Long)

    // B4: `liveSlam` binds scan_session_config.live_slam (present in the C
    // ABI as of SCAN_ABI_VERSION 2, the version this task is pinned against —
    // see android/NOTES.md's B2 section for the ABI-1 gap this closes).
    external fun nativeStartSession(
        handle: Long,
        lscanDir: String?,
        profile: String?,
        record: Boolean,
        liveSlam: Boolean,
    ): Int
    external fun nativeStopSession(handle: Long): Int
    external fun nativeEngineState(handle: Long): Int

    // --- devices ---------------------------------------------------------------
    /** Returns the device id (>= 0), or -1 on failure (see [nativeLastError]). */
    external fun nativeAddD6Device(
        handle: Long,
        serialPortName: String?,
        baud: Int,
        sendStartStop: Boolean,
        writer: SerialWriter?,
    ): Int

    external fun nativeRemoveDevice(handle: Long, deviceId: Int): Int
    external fun nativeDeviceHealth(handle: Long, deviceId: Int): NativeDeviceHealth?

    /** `buffer` must be `ByteBuffer.allocateDirect(...)` — zero-copy across the JNI boundary. */
    external fun nativePushSerialBytes(handle: Long, deviceId: Int, buffer: ByteBuffer, len: Int, tMonoNs: Long): Int

    // --- events ----------------------------------------------------------------
    external fun nativeStartEventPump(handle: Long, listener: EngineEventListener): Boolean
    external fun nativeStopEventPump(handle: Long)

    // --- B4: point pages (live capture engine) ----------------------------------
    // Minimal JNI added for B4's Filament pipeline: enumerate + read pages
    // from the *live* `scan_engine*` this handle already owns, mirroring
    // desktop's PagedCloudRenderer::sync() poll loop one layer down (see
    // com.lidarscan.app.render.LiveEngineCloudSource).
    external fun nativePageCount(handle: Long): Int
    external fun nativePageIdAt(handle: Long, index: Int): Int
    external fun nativeGetPointPage(handle: Long, pageId: Int): NativePointPage?
    external fun nativeTotalPoints(handle: Long): Long

    // --- B4: replay engine (synthetic-capture acceptance path) ------------------
    // A *separate* handle space from the live `scan_engine*` above — see
    // replay_engine.h's header comment for why replay needs its own
    // scanengine::Engine rather than reusing the C-ABI one. `lscanDir` must
    // be a real filesystem path (the bundled asset already extracted there —
    // see com.lidarscan.app.replay.SyntheticReplayAssets), not an APK asset
    // path.
    external fun nativeReplayCreate(): Long
    external fun nativeReplayDestroy(handle: Long)
    external fun nativeReplayStart(handle: Long, lscanDir: String, speed: Double): Boolean
    external fun nativeReplayStop(handle: Long)
    external fun nativeReplayIsRunning(handle: Long): Boolean
    external fun nativeReplayLastError(handle: Long): String
    external fun nativeReplayStats(handle: Long): NativeReplayStats?
    external fun nativeReplayDeviceHealth(handle: Long): NativeDeviceHealth?
    external fun nativeReplayPageCount(handle: Long): Int
    external fun nativeReplayPageIdAt(handle: Long, index: Int): Int
    external fun nativeReplayGetPointPage(handle: Long, pageId: Int): NativePointPage?
    external fun nativeReplayTotalPoints(handle: Long): Long

    // --- B7: ARCore poses in (A8's "poses in" C-ABI block) ----------------------
    //
    // Bound in cpp/arcore_jni.cpp. Called once per ARCore frame from the AR
    // thread — safe concurrently with the D6 reader thread's
    // [nativePushSerialBytes], per scanengine_c.h's own note on push_pose.
    //
    // `confidence` < 0 means "derive it from quality/tracking_lost", which is
    // what [com.lidarscan.app.ar.CaptureArController] passes: ARCore reports a
    // tracking STATE and a failure REASON, not a scalar, and inventing one
    // here would be worse than letting the engine derive it.
    external fun nativePushPose(
        handle: Long,
        tMonoNs: Long,
        px: Double,
        py: Double,
        pz: Double,
        qx: Double,
        qy: Double,
        qz: Double,
        qw: Double,
        positionSigmaM: Float,
        orientationSigmaDeg: Float,
        quality: Int,
        trackingLost: Boolean,
        confidence: Float,
    ): Int

    /** Returns the `SCAN_POSE_GATE_*` value at [tMonoNs] (see [PoseGate]), or -1 if the call itself failed. */
    external fun nativePoseGateAt(handle: Long, tMonoNs: Long): Int

    // --- B7: D6 pushbroom + mount extrinsics -----------------------------------
    /** `phoneFromLidar` must be a **row-major** 4x4; the engine rejects a column-major one outright. */
    external fun nativeSetMountExtrinsics(handle: Long, phoneFromLidar: DoubleArray): Int
    external fun nativePushbroomEnable(handle: Long, on: Boolean): Int
    external fun nativePushbroomFlush(handle: Long): Int
    external fun nativePushbroomStats(handle: Long): NativePushbroomStats?

    // --- B7: the mount-calibration solver (a standalone handle, no engine) ------
    external fun nativeMountCalibCreate(): Long
    external fun nativeMountCalibDestroy(handle: Long)

    /**
     * One wizard pose. `(nx, ny, nz, d)` is the board plane as the CAMERA
     * measured it, in the camera frame; `xyz` is a flat `x,y,z` array of the
     * lidar returns segmented onto the board, in the SENSOR frame; `sigmaM` is
     * their 1-sigma range noise, which whitens the residual.
     */
    external fun nativeMountCalibAddObservation(
        handle: Long,
        nx: Double,
        ny: Double,
        nz: Double,
        d: Double,
        xyz: FloatArray,
        sigmaM: Double,
    ): Int

    /** `cad` is the bracket's row-major CAD nominal `phone_from_lidar`. Null on failure (including the < 3-observation refusal). */
    external fun nativeMountCalibSolve(handle: Long, cad: DoubleArray): NativeMountCalibResult?

    // --- B8: the frames.idx keyframe writer -------------------------------------
    //
    // NOT a `scan_engine_record_keyframe` call — that C-ABI entry point does
    // not exist at SCAN_ABI_VERSION 3 (A11 §8.2 asks for it; nothing has
    // landed it). cpp/keyframe_writer.{h,cpp} wraps the engine's own
    // `color::KeyframeIndexWriter` instead; see that header for why it is the
    // correct writer rather than a second FileRecordWriter.
    //
    // The JPEG file itself is written by Kotlin ([com.lidarscan.app.ar.KeyframeRecorder]);
    // this records the index entry naming it.
    external fun nativeKeyframeWriterOpen(lscanDir: String): Long

    /** Returns null on success, or a human-readable validation error (naming the offending field). */
    external fun nativeKeyframeWriterAdd(
        handle: Long,
        tEngineNs: Long,
        exposureNs: Long,
        px: Double,
        py: Double,
        pz: Double,
        qx: Double,
        qy: Double,
        qz: Double,
        qw: Double,
        fx: Float,
        fy: Float,
        cx: Float,
        cy: Float,
        distortion: FloatArray,
        width: Int,
        height: Int,
        rowTimeNs: Float,
        positionSigmaM: Float,
        orientationSigmaDeg: Float,
        poseQuality: Int,
        trackingLost: Boolean,
        poseSource: Int,
        flags: Int,
        iso: Float,
        angularRateRadPerS: Float,
        linearSpeedMPerS: Float,
        imageBytes: Int,
        imageName: String,
    ): String?

    external fun nativeKeyframeWriterRecords(handle: Long): Int
    external fun nativeKeyframeWriterFlush(handle: Long)
    external fun nativeKeyframeWriterClose(handle: Long)

    /**
     * Engine → app write callback for D6's start/stop command bytes
     * (`scan_serial_write_cb` in the C ABI). Implemented in
     * [com.lidarscan.app.usb.D6SerialConnection] over the actual
     * `UsbSerialPort`. Return `SCAN_OK` (0) or an `SCAN_ERR_*` value.
     */
    fun interface SerialWriter {
        fun write(data: ByteArray): Int
    }

    /**
     * Delivered from the native event-pump thread (see scanengine_jni.cpp's
     * `event_pump_loop`) — do not block, and do not call back into
     * [ScanEngineNative] synchronously from here (mirrors the C ABI's own
     * "do not call back into the engine" rule for push-mode callbacks).
     * `i0..i4`/`d0` are interpreted per [type] — see [EventType] and
     * scanengine_jni.cpp's marshalling switch for the mapping.
     */
    fun interface EngineEventListener {
        fun onEvent(type: Int, sequence: Int, tMonoNs: Long, i0: Long, i1: Long, i2: Long, i3: Long, i4: Long, d0: Double)
    }

    /** Mirrors `scan_error_t` (`scanengine_c.h`). */
    object ErrorCode {
        const val OK = 0
        const val UNKNOWN = 1
        const val INVALID_ARGUMENT = 2
        const val INVALID_STATE = 3
        const val NOT_FOUND = 4
        const val ALREADY_EXISTS = 5
        const val NOT_SUPPORTED = 6
        const val UNIMPLEMENTED = 7
        const val OUT_OF_MEMORY = 8
        const val CANCELLED = 9
        const val TIMEOUT = 10
        const val BUSY = 11
        const val AGAIN = 12
        const val CAPACITY_EXCEEDED = 13
        const val IO = 20
        const val DISCONNECTED = 21
        const val PERMISSION_DENIED = 22
        const val NETWORK = 23
        const val DEVICE_NOT_RESPONDING = 30
        const val PROTOCOL = 31
        const val CHECKSUM = 32
        const val DEVICE_FAULT = 33
        const val CORRUPT_DATA = 40
        const val VERSION_MISMATCH = 41
        const val FILE = 42
    }

    /** Mirrors `SCAN_DEVICE_*` (`scanengine_c.h`). */
    object DeviceKind {
        const val UNKNOWN = 0
        const val D6 = 1
        const val MID360 = 2
        const val RTK_ROVER = 3
    }

    /** Mirrors `SCAN_DEV_*` device state (`scanengine_c.h`) — what the health panel's "state" label reads. */
    object DeviceState {
        const val DISCONNECTED = 0
        const val IDLE = 1
        const val STARTING = 2
        const val STREAMING = 3
        const val DEGRADED = 4
        const val STOPPING = 5
        const val FAULT = 6

        fun label(state: Int): String = when (state) {
            DISCONNECTED -> "Disconnected"
            IDLE -> "Idle"
            STARTING -> "Starting"
            STREAMING -> "Streaming"
            DEGRADED -> "Degraded"
            STOPPING -> "Stopping"
            FAULT -> "Fault"
            else -> "Unknown ($state)"
        }
    }

    /** Mirrors `SCAN_POSE_QUALITY_*` (`scanengine_c.h`) — what [nativePushPose] takes. */
    object PoseQuality {
        const val INVALID = 0
        const val POOR = 1
        const val FAIR = 2
        const val GOOD = 3
    }

    /** Mirrors `SCAN_POSE_GATE_*` — the five outcomes §3.3's "flagged and excluded by default" needs distinguished. */
    object PoseGate {
        const val OK = 0
        const val NO_DATA = 1
        const val BEFORE_FIRST = 2
        const val FUTURE = 3
        const val STALE = 4
        const val TRACKING_LOST = 5
        const val LOW_CONFIDENCE = 6
    }

    /** Mirrors `SCAN_STREAM_*`. */
    object StreamId {
        const val POSE_AR = 4
        const val CAMERA_FRAMES = 6
    }

    /** Mirrors `kKeyframeFlag*` (`engine/include/scanengine/color/colorize.h`). */
    object KeyframeFlags {
        const val MOTION_VALID = 1
        const val EXPOSURE_VALID = 2
        const val TRACKING_LOST = 4
        const val AUTO_EXPOSURE_LOCKED = 8
    }

    /** Mirrors `SCAN_EVENT_*` (`scanengine_c.h`). */
    object EventType {
        const val NONE = 0
        const val EVENTS_DROPPED = 1
        const val ENGINE_STATE = 10
        const val SESSION_STATE = 11
        const val DEVICE_STATE = 20
        const val DEVICE_HEALTH = 21
        const val POINTS_AVAILABLE = 30
        const val ROTATION = 31
        const val POSE_UPDATE = 40
        const val GNSS_FIX = 50
        const val JOB_PROGRESS = 60
        const val ERROR = 90
    }
}
