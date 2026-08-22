package com.lidarscan.app.engine

import com.lidarscan.app.render.LiveEngineCloudSource
import com.lidarscan.app.render.NativePointCloudProvider
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineEvent
import com.lidarscan.core.capture.PointCountTally
import com.lidarscan.core.capture.PointStreamRole
import com.lidarscan.core.engine.EngineTarget
import com.lidarscan.core.engine.SerialLidarBaud
import com.lidarscan.core.model.SensorType
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * JNI-backed [EngineBridge] over `libscanengine`'s C ABI (B2). D6/USB-serial
 * is wired; other [SensorType]s fail [connect] cleanly (arrives with
 * B3/B9 — Mid-360/RTK). Selected by [com.lidarscan.app.di.AppContainer] in
 * place of [com.lidarscan.core.engine.FakeEngineBridge] once
 * [ScanEngineNative.isAvailable].
 *
 * Ownership/threading: one `scan_engine*` handle for this bridge's lifetime
 * (created lazily on first successful [connect], destroyed in [disconnect]).
 * The event pump is the native thread `ScanEngineNative.nativeStartEventPump`
 * starts; [onEvent] below is called *from that thread* — never block there.
 * The D6 reader thread lives in [com.lidarscan.app.usb.D6SerialConnection].
 * Health polling runs on [scope] via a plain coroutine ticker (device health
 * has no corresponding push event on the wire — see android/NOTES.md's
 * "C ABI gaps").
 */
class RealEngineBridge(
    private val connectionRegistry: D6UsbConnectionRegistry,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
    /**
     * ROUND 6 (owner item 21): how big the engine's live `PageStore` is on THIS
     * phone. B2 through 0.2.1 passed `0, 0` here, i.e. "use the engine's
     * defaults" — which are a desktop's 16 MB pages and a 1 GB ceiling, and
     * which a two-stream D6 capture exhausts in about a minute of walking. See
     * [com.lidarscan.core.render.LivePageStoreSizing] for the full mechanism.
     */
    private val pageStoreSizing: com.lidarscan.core.render.LivePageStoreSizing =
        com.lidarscan.core.render.LivePageStoreSizing.forTier(com.lidarscan.core.capture.DeviceTier.STANDARD),
) : EngineBridge, ScanEngineNative.EngineEventListener, NativePointCloudProvider {

    /** ROUND 6: the live-store ceiling the capture screen watches for, in pages. */
    val livePageBudget: Int get() = pageStoreSizing.maxPages

    /** ROUND 6: the sizing actually handed to `scan_engine_create`, for the inline "map is full" note. */
    val livePageStoreSizing: com.lidarscan.core.render.LivePageStoreSizing get() = pageStoreSizing

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    override val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _captureState = MutableStateFlow(CaptureState.IDLE)
    override val captureState: StateFlow<CaptureState> = _captureState.asStateFlow()

    private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 64)
    override val events: SharedFlow<EngineEvent> = _events.asSharedFlow()

    private val _deviceHealth = MutableStateFlow<DeviceHealth?>(null)
    override val deviceHealth: StateFlow<DeviceHealth?> = _deviceHealth.asStateFlow()

    private var engineHandle: Long = 0L
    private var deviceId: Int = -1
    private var healthPollJob: Job? = null
    private var recordingStartNs = 0L

    /**
     * ROUND 11 — was `pointsSinceStart`, a single Long summing every
     * `POINTS_AVAILABLE` event regardless of which stream it came from. The
     * engine's one PageStore carries the raw sensor-frame preview AND the
     * resolved pushbroom map during a D6 capture, so that sum was roughly
     * double the truth (scan-020: 584,315 shown, 293,166 real). See
     * [PointCountTally] for the whole story and for why the roles rather than
     * the stream ids cross into `:core`.
     */
    private val tally = PointCountTally()

    /** ROUND 11: the honest count, for the status line and for the ViewModel. */
    val pointsThisSession: Long get() = tally.points
    private var lastDevicePath: String? = null

    /** B3: `"<lidarIp>|<hostIp>"` while a Mid-360 is the connected device, else null. */
    private var mid360Endpoint: String? = null

    override suspend fun connect(target: EngineTarget): Result<Unit> = withContext(Dispatchers.IO) {
        when (target.sensor) {
            SensorType.MID360 -> return@withContext connectMid360(target)
            SensorType.COIN_D6 -> Unit // falls through to the serial path below
            // ROUND 25 item 119: the STL-27L is the same transport, the same
            // push-bytes loop and the same phone-tracked pose as the D6. The
            // only two things that differ — the engine's device kind and the
            // baud — are parameters of that path, not a second path, so it
            // takes this one deliberately rather than by omission.
            SensorType.STL27L -> Unit
        }
        if (!ScanEngineNative.isAvailable) {
            _connectionState.value = ConnectionState.ERROR
            return@withContext Result.failure(IllegalStateException("scanengine_jni native library not loaded"))
        }
        val sensorName = target.sensor.displayName
        val devicePath = target.transportHint
            ?: return@withContext Result.failure(
                IllegalArgumentException("$sensorName connect needs a device path"),
            )
        val conn = connectionRegistry.get(devicePath)
            ?: return@withContext Result.failure(
                IllegalStateException(
                    "No open USB connection for $devicePath — the connect wizard must open+permission it first",
                ),
            )

        _connectionState.value = ConnectionState.CONNECTING
        _events.emit(EngineEvent.StatusMessage("Connecting to $sensorName on $devicePath…"))

        if (engineHandle == 0L) {
            engineHandle = createEngineHandle()
            if (engineHandle == 0L) {
                _connectionState.value = ConnectionState.ERROR
                val message = "scan_engine_create failed: ${ScanEngineNative.nativeLastError()}"
                _events.emit(EngineEvent.Fault("CREATE_FAILED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
        }

        // ROUND 25 item 119 — the three things that differ between the two
        // serial lidars, each read from a named source rather than typed here:
        //
        //  * `kind`   — SCAN_DEVICE_D6 vs SCAN_DEVICE_STL27L. A new VALUE of an
        //               existing enum field, so the C ABI is still 12.
        //  * `baud`   — 230400 vs 921600, from `SerialLidarBaud`, which is the
        //               SAME object `D6UsbConnectionRegistry.open` reads. They
        //               must agree: the engine derives per-point timing from
        //               this number, so a wrong one does not fail, it silently
        //               mis-times every return.
        //  * `writer` — the LD-series free-runs on power and has no command
        //               channel, so the STL-27L gets a null writer and
        //               sendStartStop=false. Building a trampoline it would
        //               never use would only leave a JVM global ref alive.
        val kind = deviceKindOf(target.sensor)
        // ROUND 32 item 178(b): the target's own rate wins when it has one.
        // The probe that opened this port at 230 400 has already proved the
        // sensor answers there; deriving 921 600 from the sensor type at this
        // point would hand the engine a divisor the wire is not using, which
        // does not fail — it mis-times every return.
        if (target.serialBaud == null && !SerialLidarBaud.isSerial(target.sensor)) {
            return@withContext Result.failure(
                IllegalStateException("$sensorName is not a serial sensor"),
            )
        }
        val baud = target.serialBaud
            ?: SerialLidarBaud.forSensorOrNull(target.sensor)
            ?: return@withContext Result.failure(
                IllegalStateException("$sensorName is not a serial sensor"),
            )
        val hasCommandChannel = target.sensor == SensorType.COIN_D6
        val writer = if (hasCommandChannel) ScanEngineNative.SerialWriter { data -> conn.write(data) } else null
        val id = ScanEngineNative.nativeAddSerialLidarDevice(
            engineHandle,
            kind,
            devicePath,
            baud,
            hasCommandChannel,
            writer,
        )
        if (id < 0) {
            _connectionState.value = ConnectionState.ERROR
            val message = "scan_engine_add_device failed: ${ScanEngineNative.nativeLastError()}"
            _events.emit(EngineEvent.Fault("ADD_DEVICE_FAILED", message))
            return@withContext Result.failure(IllegalStateException(message))
        }
        deviceId = id
        lastDevicePath = devicePath

        ScanEngineNative.nativeStartEventPump(engineHandle, this@RealEngineBridge)
        startHealthPolling()

        conn.startReading { buffer, len, tMonoNs ->
            val err = ScanEngineNative.nativePushSerialBytes(engineHandle, id, buffer, len, tMonoNs)
            if (err != ScanEngineNative.ErrorCode.OK && err != ScanEngineNative.ErrorCode.AGAIN) {
                // Deliberately not emitted through `_events` from here — this
                // callback runs on the USB reader thread and EngineEvent is a
                // SharedFlow, which tryEmit handles fine, but frequent byte-chunk
                // errors would flood the UI; last_error is still queryable.
            }
        }

        _connectionState.value = ConnectionState.CONNECTED
        _events.emit(EngineEvent.StatusMessage("Connected to $sensorName on $devicePath"))
        Result.success(Unit)
    }

    /**
     * ROUND 25 item 119: [SensorType] → [ScanEngineNative.DeviceKind].
     *
     * Written out rather than derived from an ordinal, because the two enums
     * are maintained in different languages and their orders are not a
     * contract. The Mid-360 branch exists so this `when` stays exhaustive and
     * the next sensor breaks the build here — it is never reached, because
     * `connect` has already routed a Mid-360 to [connectMid360] by this point.
     */
    private fun deviceKindOf(sensor: SensorType): Int = when (sensor) {
        SensorType.COIN_D6 -> ScanEngineNative.DeviceKind.D6
        SensorType.STL27L -> ScanEngineNative.DeviceKind.STL27L
        SensorType.MID360 -> ScanEngineNative.DeviceKind.MID360
    }

    /**
     * B3: the Mid-360 connect path. Adds the device to **this bridge's own**
     * `scan_engine*` — the same engine the capture session runs on — so its
     * points land in the session's PageStore, its raw datagrams reach the
     * session's `.lscan` recorder (the engine installs a `raw_sink` shim for
     * exactly that, engine.cpp's "Record-always for a driver that owns its
     * own sockets"), and live SLAM can see them.
     *
     * That is why this goes through the C ABI rather than through B3's
     * `Mid360Probe`: the probe owns a *separate* engine, which is right for a
     * transport check and wrong for a capture.
     *
     * [EngineTarget.transportHint] carries `"<lidarIp>|<hostIp>"`. Both are
     * required — see [ScanEngineNative.nativeAddMid360Device]. The engine is
     * created here if it does not exist yet, exactly as the D6 path does.
     */
    private suspend fun connectMid360(target: EngineTarget): Result<Unit> {
        if (!ScanEngineNative.isAvailable) {
            _connectionState.value = ConnectionState.ERROR
            return Result.failure(IllegalStateException("scanengine_jni native library not loaded"))
        }
        val hint = target.transportHint
            ?: return Result.failure(IllegalArgumentException("Mid-360 connect needs \"<lidarIp>|<hostIp>\""))
        val parts = hint.split('|')
        if (parts.size != 2 || parts.any { it.isBlank() }) {
            return Result.failure(
                IllegalArgumentException(
                    "Mid-360 connect needs both a lidar IP and a host IP (\"<lidarIp>|<hostIp>\"); got \"$hint\"",
                ),
            )
        }
        val (lidarIp, hostIp) = parts

        _connectionState.value = ConnectionState.CONNECTING
        _events.emit(EngineEvent.StatusMessage("Adding Mid-360 $lidarIp → host $hostIp…"))

        if (engineHandle == 0L) {
            engineHandle = createEngineHandle()
            if (engineHandle == 0L) {
                _connectionState.value = ConnectionState.ERROR
                val message = "scan_engine_create failed: ${ScanEngineNative.nativeLastError()}"
                _events.emit(EngineEvent.Fault("CREATE_FAILED", message))
                return Result.failure(IllegalStateException(message))
            }
        }

        val id = ScanEngineNative.nativeAddMid360Device(engineHandle, lidarIp, hostIp)
        if (id < 0) {
            _connectionState.value = ConnectionState.ERROR
            val message = "scan_engine_add_device(Mid-360) failed: ${ScanEngineNative.nativeLastError()}"
            _events.emit(EngineEvent.Fault("ADD_DEVICE_FAILED", message))
            return Result.failure(IllegalStateException(message))
        }
        deviceId = id
        lastDevicePath = null // no USB connection backs a Mid-360
        mid360Endpoint = hint

        ScanEngineNative.nativeStartEventPump(engineHandle, this@RealEngineBridge)
        startHealthPolling()

        _connectionState.value = ConnectionState.CONNECTED
        _events.emit(EngineEvent.StatusMessage("Mid-360 $lidarIp added as device $id"))
        return Result.success(Unit)
    }

    /** True once a Mid-360 has been added — changes what pause/resume can mean (see [pauseCapture]). */
    val isMid360: Boolean get() = mid360Endpoint != null

    override suspend fun disconnect() {
        stopCapture()
        mid360Endpoint = null
        healthPollJob?.cancel()
        healthPollJob = null

        val devicePath = lastDevicePath
        if (devicePath != null) connectionRegistry.close(devicePath)
        lastDevicePath = null

        if (engineHandle != 0L && deviceId >= 0) {
            ScanEngineNative.nativeRemoveDevice(engineHandle, deviceId)
        }
        deviceId = -1
        if (engineHandle != 0L) {
            ScanEngineNative.nativeStopEventPump(engineHandle)
        }
        _connectionState.value = ConnectionState.DISCONNECTED
        _deviceHealth.value = null
        _events.emit(EngineEvent.StatusMessage("Disconnected"))
    }

    override suspend fun startCapture(projectDirectory: String, liveSlam: Boolean, profile: String): Result<Unit> =
        withContext(Dispatchers.IO) {
            if (_connectionState.value != ConnectionState.CONNECTED) {
                val message = "Cannot start capture: not connected"
                _events.emit(EngineEvent.Fault("NOT_CONNECTED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
            // Record-always (Tech Spec §3, key rule 2): `record` is always true
            // here, independent of `liveSlam`. `liveSlam` now DOES bind
            // scan_session_config.live_slam (B4, ABI 2 — see
            // android/NOTES.md's B2 section for the ABI-1-era gap this
            // closes; B2 could only record it for status text).
            //
            // B5: `profile` is now the PROJECT's, not the hardcoded
            // "quickscan" B2 had to pass — see EngineBridge.startCapture's
            // KDoc and CaptureDefaults.engineProfileString().
            //
            // ── ROUND 7 (field bug: "scan-009 sealed points=0 elapsedMs=0") ──
            // **Re-arm the transport before the session starts.**
            //
            // [pauseCapture] and [stopCapture] both implement their half of the
            // session by telling the D6 reader thread to stop forwarding bytes
            // into `push_serial_bytes` — the C ABI has no pause/resume, so that
            // is where a pause lives. Only [resumeCapture] ever turned
            // forwarding back ON, and it is reachable only from the Pause
            // button.
            //
            // So the FIRST Stop of a connect session latched `forwarding =
            // false` on the open [com.lidarscan.app.usb.D6SerialConnection] and
            // nothing ever cleared it: the second Start created a healthy
            // `.lscan`, started a healthy engine session, got `SCAN_OK` back —
            // and then received **not one byte** for the rest of the connect,
            // because the reader thread was still dropping every chunk. Zero
            // packets means zero POINTS_AVAILABLE events, which means
            // `CaptureStats` never fires, which is exactly the field log's
            // `sealed OK … points=0 elapsedMs=0` two minutes after a
            // 216,653-point scan on the same cable.
            //
            // Neither hardware-free path could see it: `ReplayEngineBridge` has
            // no serial connection and `FakeEngineBridge` has no transport at
            // all. It is real-USB state, and it is now re-armed on every start
            // rather than only on a Pause→Resume.
            activeConnection()?.resumeForwarding()
            // ── ROUND 10 (owner item 38) ────────────────────────────────────
            // Empty the live window BEFORE the session opens, so the first
            // frame of capture #2 is capture #2. `Engine::start_session()` does
            // this itself — but only when the store is in kEvictOldest, which
            // is opt-in and which this app enables in createEngineHandle(). The
            // explicit call is kept anyway: it is one JNI hop, it is the only
            // thing standing between the operator and someone else's scan, and
            // it must not silently depend on a policy flag set somewhere else.
            ScanEngineNative.nativeRecycleLivePages(engineHandle)
            val err = ScanEngineNative.nativeStartSession(
                engineHandle, projectDirectory, profile, true, liveSlam,
            )
            if (err != ScanEngineNative.ErrorCode.OK) {
                val message = "scan_engine_start failed: ${ScanEngineNative.nativeErrorStr(err)}"
                _events.emit(EngineEvent.Fault("START_FAILED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
            recordingStartNs = System.nanoTime()
            tally.reset()
            _captureState.value = CaptureState.RECORDING
            _events.emit(
                EngineEvent.StatusMessage(
                    "Recording to $projectDirectory (${if (liveSlam) "live SLAM" else "record-only"})",
                ),
            )
            Result.success(Unit)
        }

    /**
     * ROUND 10 (owner item 38). See [EngineBridge.resetLiveView]. Never fails a
     * caller: with no engine handle there is nothing to empty, which is success.
     */
    override suspend fun resetLiveView(): Result<Unit> = withContext(Dispatchers.IO) {
        if (engineHandle == 0L) return@withContext Result.success(Unit)
        val err = ScanEngineNative.nativeRecycleLivePages(engineHandle)
        if (err != ScanEngineNative.ErrorCode.OK) {
            _events.emit(
                EngineEvent.StatusMessage(
                    "Live view reset returned ${ScanEngineNative.nativeErrorStr(err)} " +
                        "(the recording is unaffected)",
                ),
            )
        }
        Result.success(Unit)
    }

    override suspend fun pauseCapture(): Result<Unit> {
        if (_captureState.value != CaptureState.RECORDING) return Result.success(Unit)
        if (isMid360) {
            // B2's pause trick does not transfer. The D6 pauses by having the
            // app's reader thread stop forwarding bytes into
            // push_serial_bytes — but the Mid-360 owns its own sockets and
            // the app never touches its bytes, so there is nothing app-side
            // to hold back.
            //
            // Desktop C2 pauses a Mid-360 by stopping the recording session
            // and starting a preview one, then resuming with a *new*
            // recording session into the same directory. That is NOT
            // replicated here, because `FileRecordWriter::open()` creates its
            // stream files with `std::fopen(path, "wb")` — a resume would
            // **truncate everything recorded before the pause**. Silently
            // destroying the first half of a capture is far worse than not
            // offering the button, so this fails cleanly and CaptureScreen
            // hides Pause for a Mid-360 session (the same shape as B4's
            // replay path, which also refuses rather than faking a resume).
            //
            // A real fix is an append/resume mode in the recorder, or a
            // pause/resume pair in the C ABI — noted in android/NOTES.md.
            val message =
                "Pause is not available for a Mid-360: the driver owns its own sockets, and restarting " +
                    "the session to resume would truncate the recording already written. Use Stop."
            _events.emit(EngineEvent.StatusMessage(message, EngineEvent.StatusMessage.Level.WARNING))
            return Result.failure(UnsupportedOperationException(message))
        }
        // No scan_engine_pause/resume in the C ABI (start/stop only — see
        // android/NOTES.md). Pause is implemented one layer down: the D6
        // reader thread keeps the USB port open but stops forwarding bytes
        // into push_serial_bytes, so the .lscan session simply receives
        // nothing while paused; resume flips it back on. This needs no ABI
        // change.
        activeConnection()?.pauseForwarding()
        _captureState.value = CaptureState.PAUSED
        _events.emit(EngineEvent.StatusMessage("Capture paused"))
        return Result.success(Unit)
    }

    override suspend fun resumeCapture(): Result<Unit> {
        if (_captureState.value != CaptureState.PAUSED) return Result.success(Unit)
        activeConnection()?.resumeForwarding()
        _captureState.value = CaptureState.RECORDING
        _events.emit(EngineEvent.StatusMessage("Capture resumed"))
        return Result.success(Unit)
    }

    override suspend fun stopCapture(): Result<Unit> = withContext(Dispatchers.IO) {
        if (_captureState.value == CaptureState.IDLE) return@withContext Result.success(Unit)
        _captureState.value = CaptureState.STOPPING
        activeConnection()?.pauseForwarding()
        val err = ScanEngineNative.nativeStopSession(engineHandle)
        _captureState.value = CaptureState.IDLE
        if (err != ScanEngineNative.ErrorCode.OK) {
            val message = "scan_engine_stop failed: ${ScanEngineNative.nativeErrorStr(err)}"
            _events.emit(EngineEvent.Fault("STOP_FAILED", message))
            return@withContext Result.failure(IllegalStateException(message))
        }
        _events.emit(EngineEvent.StatusMessage("Capture stopped — ${tally.logSuffix()}"))
        Result.success(Unit)
    }

    /**
     * ROUND 11: which of the store's simultaneous point streams an event came
     * from. `SLAM_MAP` is the registered world-frame cloud (A6's live map and
     * A8's pushbroom both publish there — INT24-wiring.md §2), which is the one
     * the operator means by "points"; the sensor streams are the raw preview
     * fan of the same returns.
     */
    private fun streamRole(stream: Int): PointStreamRole = when (stream) {
        ScanEngineNative.StreamId.SLAM_MAP -> PointStreamRole.RESOLVED_MAP
        ScanEngineNative.StreamId.LIDAR_D6,
        ScanEngineNative.StreamId.LIDAR_MID360,
        -> PointStreamRole.RAW_SENSOR
        else -> PointStreamRole.OTHER
    }

    /** Called from scanengine_jni's native event-pump thread — must not block or call back into ScanEngineNative. */
    override fun onEvent(
        type: Int,
        sequence: Int,
        tMonoNs: Long,
        i0: Long,
        i1: Long,
        i2: Long,
        i3: Long,
        i4: Long,
        d0: Double,
    ) {
        when (type) {
            ScanEngineNative.EventType.POINTS_AVAILABLE -> {
                // ROUND 11: i3 is `payload.points.stream`, and it has been in
                // the payload since B2 — the counter simply never read it. The
                // mapping from the C ABI's numeric stream space to a role is
                // here, in `:app`, because that space belongs to the ABI (see
                // EngineBridge's own KDoc).
                tally.add(streamRole(i3.toInt()), i2)
                val elapsedMs = ((System.nanoTime() - recordingStartNs) / 1_000_000L).coerceAtLeast(1L)
                _events.tryEmit(EngineEvent.CaptureStats(tally.points, elapsedMs))
            }
            ScanEngineNative.EventType.DEVICE_STATE -> {
                val errorCode = i4.toInt() // payload.device.error
                if (errorCode != ScanEngineNative.ErrorCode.OK) {
                    _events.tryEmit(EngineEvent.Fault("DEVICE_ERROR", ScanEngineNative.nativeErrorStr(errorCode)))
                }
            }
            ScanEngineNative.EventType.ERROR -> {
                _events.tryEmit(EngineEvent.Fault("ENGINE_ERROR", ScanEngineNative.nativeErrorStr(i0.toInt())))
            }
            else -> Unit
        }
    }

    /**
     * B4: the Capture screen's live 3D view reads pages through this — a
     * thin [LiveEngineCloudSource] over whatever `engineHandle` currently
     * is (re-read on every call via the lambda, since it can go from 0 to a
     * real handle across a [connect]).
     */
    override fun currentPointCloudSource(): PointCloudSource? =
        if (engineHandle != 0L) LiveEngineCloudSource { engineHandle } else null

    /**
     * B7: the raw `scan_engine*` for the calls that have no place on the
     * `EngineBridge` interface — `scan_engine_push_pose` (driven from the
     * ARCore thread, not from a coroutine), `set_mount_extrinsics` and
     * `pushbroom_enable`. Deliberately NOT added to `:core`'s `EngineBridge`:
     * that interface is the platform-neutral seam, and a native handle is the
     * one thing it must never carry.
     */
    fun engineHandleOrZero(): Long = engineHandle

    /**
     * B7: hands the ARCore controller this bridge's engine handle so its
     * poses land in the live session, and applies a mount extrinsic when one
     * is available. Called by the Capture screen at session start.
     */
    fun attachPoseSink(setHandle: (Long) -> Unit) {
        setHandle(engineHandle)
    }

    /**
     * ROUND 6 (owner item 21): one call site for `scan_engine_create`, so the
     * phone-sized page store cannot be applied on the D6 path and forgotten on
     * the Mid-360 one (which is exactly the shape the two copies of this call
     * had before).
     */
    private fun createEngineHandle(): Long {
        val handle = ScanEngineNative.nativeCreateEngine(
            "lidarscan-android",
            LOG_LEVEL_INFO,
            pageStoreSizing.pageCapacityPoints,
            pageStoreSizing.maxPages,
            0,
        )
        if (handle != 0L) {
            // ROUND 10 (owner item 38). Two things this buys, and the second is
            // the one that was actually hurting:
            //
            //  1. A live window that RECYCLES its oldest page instead of
            //     dead-ending. `LivePageStoreSizing`'s KDoc still says
            //     "eviction is an engine change and the engine tree is
            //     read-only for this task" — that was true when it was written
            //     and stopped being true at ABI 7. The note it left behind
            //     (`fullNote`) has been the app's answer ever since.
            //  2. `Engine::start_session()` resets the store between sessions
            //     ONLY when this policy is on. That reset is what makes capture
            //     #2 start from an empty map.
            ScanEngineNative.nativeSetLivePageEviction(handle, true)
        }
        return handle
    }

    private fun activeConnection() = lastDevicePath?.let { connectionRegistry.get(it) }

    private fun startHealthPolling() {
        healthPollJob?.cancel()
        healthPollJob = scope.launch {
            while (true) {
                val id = deviceId
                val handle = engineHandle
                if (id >= 0 && handle != 0L) {
                    val h = ScanEngineNative.nativeDeviceHealth(handle, id)
                    if (h != null) {
                        _deviceHealth.value = DeviceHealth(
                            id = h.id,
                            kind = h.kind,
                            state = h.state,
                            lastError = h.lastError,
                            bytesIn = h.bytesIn,
                            packetsOk = h.packetsOk,
                            packetsBad = h.packetsBad,
                            pointsOut = h.pointsOut,
                            drops = h.drops,
                            pointsPerSec = h.pointsPerSec,
                            rotationHz = h.rotationHz,
                            checksumPassRate = h.checksumPassRate,
                            tLastDataNs = h.tLastDataNs,
                        )
                    }
                }
                delay(HEALTH_POLL_INTERVAL_MS)
            }
        }
    }

    private companion object {
        const val DEFAULT_PROFILE = "quickscan"
        const val LOG_LEVEL_INFO = 2
        const val HEALTH_POLL_INTERVAL_MS = 500L
    }
}
