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

    // --- B3: Livox Mid-360 (mid360_jni.cpp) -------------------------------------

    /**
     * Points `TMPDIR` at a writable directory (the app's `cacheDir`) for the
     * whole process. **Call this once before adding any Mid-360 device.**
     *
     * Not cosmetic: the engine's SDK2 backend generates the config file
     * `LivoxLidarSdkInit()` requires into `std::filesystem::temp_directory_path()`,
     * which on Android resolves to nothing usable — libc++ consults
     * `TMPDIR`/`TMP`/`TEMP`/`TEMPDIR` then falls back to `/tmp`, and an
     * Android device has none of those and no `/tmp`; the engine's own
     * fallback of `"."` is the app's CWD, which is `/`. Without this call the
     * Mid-360 fails to start with a file error. See mid360_jni.cpp's header
     * comment for the full write-up and for why this is preferable to an ABI
     * change exposing `Mid360Config::sdk_config_path`.
     *
     * Returns false if `setenv` failed (nothing in the app should be able to
     * cause that; it is reported rather than swallowed).
     */
    external fun nativeSetTempDir(path: String): Boolean

    /**
     * Adds a Mid-360 to the live capture engine over the C ABI. Returns the
     * device id (>= 0), or -1 on failure (see [nativeLastError]).
     *
     * Both IPs are REQUIRED, per A3 §3 ("Explicit IP is mandatory,
     * everywhere") — `host_ip` because the device is *told* where to stream
     * via the SDK's `0x0100` configuration push and never discovers its host,
     * so a wrong one means it streams into the void, silently.
     *
     * `scan_device_config` carries **only** these two fields for a Mid-360;
     * the backend selector, every port, and `UdpConfig::prebound_fd` are not
     * in the C ABI at all. That is why the wizard's checks go through
     * [nativeMid360ProbeStart] instead — see NativeMid360Probe.kt.
     */
    external fun nativeAddMid360Device(handle: Long, lidarIp: String, hostIp: String): Int

    /** Allocates a `lidarscan_jni::Mid360Probe`. Always pair with [nativeMid360ProbeDestroy]. */
    external fun nativeMid360ProbeCreate(): Long
    external fun nativeMid360ProbeDestroy(handle: Long)

    /**
     * Starts the probe's standalone engine + Mid-360 device.
     *
     * @param backend 0 = SDK2 (full bring-up: discovery, handshake, host-IP
     *   config push — the only backend that can start an out-of-the-box
     *   device), 1 = raw UDP (listen-only, for a device already configured to
     *   stream here, and the only backend that can take [preboundPointFd]).
     * @param preboundPointFd a descriptor already bound to the Ethernet
     *   `Network` via `Network.bindSocket` and detached with
     *   `ParcelFileDescriptor.dup(...).detachFd()`, or -1. **Ownership
     *   transfers to native**: `UdpSource` never closes a pre-bound fd ("the
     *   app owns it"), so `Mid360Probe::stop()` closes it. Only meaningful
     *   with `backend = 1`; with a pre-bound fd the probe forces IMU off (one
     *   `prebound_fd` field, two `UdpSource`s — see mid360_probe.h).
     */
    external fun nativeMid360ProbeStart(
        handle: Long,
        lidarIp: String,
        hostIp: String,
        backend: Int,
        devicePointPort: Int,
        deviceImuPort: Int,
        deviceCmdPort: Int,
        hostPointPort: Int,
        hostImuPort: Int,
        hostCmdPort: Int,
        preboundPointFd: Int,
        publishImu: Boolean,
    ): Boolean

    external fun nativeMid360ProbeStop(handle: Long)
    external fun nativeMid360ProbeLastError(handle: Long): String
    external fun nativeMid360ProbeSnapshot(handle: Long): NativeMid360Probe?

    /**
     * True while any SDK2-backed probe holds the Livox SDK's process-wide
     * singleton (`LivoxLidarSdkInit`/`Uninit` and the callback registrations
     * are global — A3 §3: "A second kSdk2 driver gets kBusy"). The wizard
     * gates capture start on this so the failure is a sentence, not a kBusy
     * from somewhere the user cannot see.
     */
    external fun nativeMid360Sdk2Active(): Boolean

    // --- B6/B11/B12: the processing engine (jobs, floor plan, merge) ------------
    //
    // A SEPARATE handle space from the live `scan_engine*`, and separate on
    // purpose: A15's job queue, A12's plan extractor and A13's merger have **no
    // C-ABI surface at all** at SCAN_ABI_VERSION 4 — `scanengine_c.h` says so
    // explicitly for the queue ("an app that wants a queue should drive a
    // Colorize job through the C++ jobs::JobQueue instead; there is no C
    // surface for the queue at ABI 4") — so `cpp/processing_engine.{h,cpp}`
    // links the engine's C++ API directly, exactly like B4's replay engine and
    // B3's Mid-360 probe. It runs from a `.lscan` on disk, so it needs no
    // devices and no session and is unrelated to whatever the capture engine is
    // doing.
    //
    // There is deliberately NO `nativeProcSubmitCloudSubmit`: A15 has a
    // kCloudSubmit job kind, but the Android cloud client is Kotlin
    // (`com.lidarscan.core.cloud`) so that the upload has one retry policy and
    // one size cap rather than two. See android/NOTES.md.
    external fun nativeProcCreate(): Long
    external fun nativeProcDestroy(handle: Long)
    external fun nativeProcLastError(handle: Long): String

    /**
     * Returns the job id, or 0 on a submit-time refusal ([nativeProcLastError] says why).
     *
     * ROUND 8: [mountPhoneFromLidar] is the ROW-MAJOR 4x4 `phone_from_lidar`,
     * or null for "read it out of the container's own manifest". It is ignored
     * for a Mid-360 project (A7's pipeline estimates its own trajectory) and
     * load-bearing for a COIN-D6 one, whose returns are range/angle pairs in
     * the lidar's own frame and are not geometry until that matrix is applied
     * — see `engine/include/scanengine/slam/post/d6_resolve.h`.
     */
    external fun nativeProcSubmitPostProcess(
        handle: Long,
        lscanDir: String,
        mountPhoneFromLidar: DoubleArray?,
    ): Long

    /**
     * ROUND 8 (owner item 27c) — what a `.lscan` on disk actually contains,
     * read from the bytes rather than from the app's sidecar manifest.
     *
     * A bitfield, decoded by [ProjectProbe.of]. One JNI call because the Review
     * screen makes it on every open.
     */
    external fun nativeProcProbeProject(handle: Long, lscanDir: String): Long

    /**
     * Loads the container's CACHED resolved cloud (the `kPointsXyzRgba` chunks
     * a 0.5.0+ capture writes) into the processing engine's PageStore, clearing
     * whatever project was there before, and returns the point count. 0 means
     * there is no cache — the caller should run a post-process instead.
     */
    external fun nativeProcOpenRecordedCloud(handle: Long, lscanDir: String): Long

    /** Retires every page in the processing engine's store. */
    external fun nativeProcClearCloud(handle: Long)

    /**
     * @param chainFrom a finished post-process job whose `PageStore` is the
     *   cloud to paint, or 0 for the engine's own store.
     * @param cameraFromLidar ROW-MAJOR rigid 4x4. A column-major matrix is
     *   refused rather than producing a plausible-looking mirrored result.
     * @param syncQuality `SCAN_SYNC_*`. **Fails closed at 0** — an unconverged
     *   estimator and a caller who never wired A4 both land there, and the
     *   colorizer refuses both before decoding an image.
     */
    external fun nativeProcSubmitColorize(
        handle: Long,
        chainFrom: Long,
        lscanDir: String,
        cameraFromLidar: DoubleArray,
        syncQuality: Int,
        allowPoorSync: Boolean,
        clockOffsetNs: Long,
    ): Long

    external fun nativeProcSubmitExport(
        handle: Long,
        chainFrom: Long,
        format: Int,
        outputPath: String,
        crsWkt: String,
        crsEpsg: String,
    ): Long

    external fun nativeProcSubmitTransferExport(
        handle: Long,
        projectDir: String,
        zipPath: String,
        includeResults: Boolean,
    ): Long

    external fun nativeProcCancelJob(handle: Long, jobId: Long): Boolean
    external fun nativeProcJobs(handle: Long): Array<NativeJob>?

    /** Delivered on the JobQueue worker thread (via `EventType::kJobProgress`). Do not block. */
    fun interface JobProgressListener {
        fun onJobProgress(jobId: Long, progress: Float, state: Int)
    }

    external fun nativeProcSetJobProgressListener(handle: Long, listener: JobProgressListener?): Boolean

    // The processing engine's own PageStore — where every produced cloud lands,
    // read with exactly the same NativePointPage marshalling as the live and
    // replay paths, so PointCloudRenderer needs no new code path.
    external fun nativeProcPageCount(handle: Long): Int
    external fun nativeProcPageIdAt(handle: Long, index: Int): Int
    external fun nativeProcGetPointPage(handle: Long, pageId: Int): NativePointPage?
    external fun nativeProcTotalPoints(handle: Long): Long

    external fun nativeProcMergedPageCount(handle: Long): Int
    external fun nativeProcMergedPageIdAt(handle: Long, index: Int): Int
    external fun nativeProcMergedGetPointPage(handle: Long, pageId: Int): NativePointPage?
    external fun nativeProcMergedTotalPoints(handle: Long): Long

    // --- B11: A12 floor plan ----------------------------------------------------
    //
    // BLOCKING — call from a coroutine on Dispatchers.Default, never the main
    // thread. The result is held natively and read back through the getters
    // below, so the arrays are always one consistent extraction.
    external fun nativeProcRunPlan(
        handle: Long,
        zMinM: Float,
        zMaxM: Float,
        gridResM: Float,
        snapOrthogonal: Boolean,
        snapToleranceDeg: Float,
        minCellPoints: Int,
        windowSillCheck: Boolean,
        detectRooms: Boolean,
        detectOpenings: Boolean,
    ): Boolean

    external fun nativeProcCancelPlan(handle: Long)
    external fun nativeProcPlanProgress(handle: Long): Float

    /** Stride 8: ax, ay, bx, by, thicknessM, rmsResidualM, coverage, confidence. */
    external fun nativeProcPlanWallsD(handle: Long): DoubleArray

    /** Stride 4: id, evidence, supportCells, snapped. */
    external fun nativeProcPlanWallsI(handle: Long): IntArray

    /** Stride 6: ax, ay, bx, by, widthM, confidence. */
    external fun nativeProcPlanOpeningsD(handle: Long): DoubleArray

    /** Stride 4: id, wallId, kind, sill. */
    external fun nativeProcPlanOpeningsI(handle: Long): IntArray

    /** Stride 5: areaM2, perimeterM, centroidX, centroidY, confidence. */
    external fun nativeProcPlanRoomsD(handle: Long): DoubleArray

    /** Stride 3: id, fullyMeasured, polygonVertexCount. */
    external fun nativeProcPlanRoomsI(handle: Long): IntArray

    /** Every room's polygon vertices concatenated as x, y — walked with the vertex counts above. */
    external fun nativeProcPlanRoomPolygons(handle: Long): DoubleArray
    external fun nativeProcPlanRoomLabels(handle: Long): Array<String>

    /** 19 doubles — see `NativePlanArrays.SUMMARY_*` for the index names. */
    external fun nativeProcPlanSummary(handle: Long): DoubleArray

    external fun nativeProcWritePlanDxf(handle: Long, path: String): Boolean
    external fun nativeProcWritePlanPdf(handle: Long, path: String, title: String, project: String, date: String): Boolean

    // --- B12: A13 georeferenced auto-merge --------------------------------------

    fun interface MergeProgressListener {
        fun onMergeProgress(fraction: Float, label: String)
    }

    /**
     * BLOCKING. `georef` is 23 doubles per session — see
     * `MergeRepository.encodeGeoref` for the layout, which is asserted by a
     * `:core` test rather than trusted.
     */
    external fun nativeProcRunMerge(
        handle: Long,
        lscanDirs: Array<String>,
        provenanceIds: Array<String>,
        chainFromJobs: LongArray,
        georef: DoubleArray,
        outPlyPath: String,
        listener: MergeProgressListener?,
    ): NativeMergeSummary?

    external fun nativeProcCancelMerge(handle: Long)

    // --- B9: GNSS / RTK (gnss_jni.cpp, entirely over the C ABI) -----------------
    //
    // A10 §9.2 listed what B9 needs and INT-29 landed all of it, so — unlike
    // the processing surface above — none of this needs C++ linkage. The rover
    // goes on the SAME `scan_engine*` the capture session owns, which is what
    // makes `scan_engine_push_nmea`'s record-always guarantee apply: the bytes
    // hit the `.lscan` as kGnssNmea chunks BEFORE they are parsed.

    /** Returns the device id (>= 0) or -1. */
    external fun nativeAddRtkRoverDevice(handle: Long): Int

    /** `buffer` must be direct. Any chunk size is fine — the framer handles SPP's 20–990-byte fragments. */
    external fun nativePushNmea(handle: Long, deviceId: Int, buffer: java.nio.ByteBuffer, len: Int, tMonoNs: Long): Int

    /** 22 doubles — see [NativeGnssLayout.FIX_*]. Null only if the call itself failed. */
    external fun nativeLastFix(handle: Long): DoubleArray?

    /** 19 doubles — see [NativeGnssLayout.STATS_*]. */
    external fun nativeGnssStats(handle: Long): DoubleArray?

    /** 27 doubles — see [NativeGnssLayout.GEOREF_*]. */
    external fun nativeGeorefSolution(handle: Long): DoubleArray?

    /** A10's stable reason string; empty when the transform has converged. */
    external fun nativeGeorefBlocker(handle: Long): String

    /** **Empty until the georef transform converges** — A9's documented local-frame-placeholder input. */
    external fun nativeCrsWkt(handle: Long): String
    external fun nativeCrsEpsg(handle: Long): String

    external fun nativeNtripCreate(engineHandle: Long): Long
    external fun nativeNtripDestroy(handle: Long)

    /** Performs the first handshake **synchronously** — call off the main thread. Returns a `SCAN_ERR_*`. */
    external fun nativeNtripConnect(
        handle: Long,
        host: String,
        port: Int,
        mountpoint: String,
        username: String,
        password: String,
        ntripVersion: Int,
        allowV1Fallback: Boolean,
        ggaIntervalMs: Int,
        autoReconnect: Boolean,
    ): Int

    external fun nativeNtripDisconnect(handle: Long): Int

    /** 18 doubles — see [NativeGnssLayout.NTRIP_*]. */
    external fun nativeNtripStats(handle: Long): DoubleArray?

    /**
     * Whole, CRC-valid RTCM3 frames only, delivered on the NTRIP receive thread
     * with no client lock held. Must be quick and must not call back into the
     * engine — this is where the app writes to the rover's Bluetooth socket.
     */
    fun interface RtcmSink {
        fun write(data: ByteArray)
    }

    external fun nativeNtripSetRtcmSink(handle: Long, sink: RtcmSink?): Boolean

    /** Stride 4 strings: mountpoint, identifier, format, country. */
    external fun nativeNtripSourcetableText(host: String, port: Int, username: String, password: String, capacity: Int): Array<String>?

    /** Stride 6 doubles: latDeg, lonDeg, needsGga, fee, carrier, solution. */
    external fun nativeNtripSourcetableNumbers(host: String, port: Int, username: String, password: String, capacity: Int): DoubleArray?

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
        const val UNKNOWN = 0
        const val LIDAR_D6 = 1
        const val LIDAR_MID360 = 2
        const val IMU = 3
        const val POSE_AR = 4
        const val GNSS = 5
        const val CAMERA_FRAMES = 6
        const val POSE_FUSED = 7

        /**
         * Registered world-frame points: A6's live-SLAM map **and** A8's
         * assembled pushbroom cloud, which INT24-wiring.md §2 deliberately
         * routes here rather than back onto `LIDAR_D6` ("kSlamMap *is* the
         * registered-world-frame stream"). B3's [com.lidarscan.app.render.StreamFilter]
         * is what stops it being drawn on top of the sensor-frame preview.
         */
        const val SLAM_MAP = 8
        const val POSE_LIO = 9
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

        /** B9: the corrections link changed state (A10 §8's kStreaming → kReconnecting → kStreaming). */
        const val NTRIP_STATE = 51

        /** B9: the moment the session became (or stopped being) exportable in a real CRS. */
        const val GEOREF_CONVERGED = 52

        /**
         * A15's queue. **Note this does not fire for the processing engine** —
         * that one is driven through the C++ `Engine::jobs()` and delivers
         * progress on its own EventBus subscription
         * ([JobProgressListener]); this constant is here because the capture
         * engine's C-ABI pump would carry it if anything ever submitted a job
         * on that engine. Before ABI 4 it crossed with a zeroed payload.
         */
        const val JOB_PROGRESS = 60
        const val ERROR = 90
    }
}
