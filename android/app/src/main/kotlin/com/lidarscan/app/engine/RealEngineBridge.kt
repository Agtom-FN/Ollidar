package com.lidarscan.app.engine

import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineEvent
import com.lidarscan.core.engine.EngineTarget
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
) : EngineBridge, ScanEngineNative.EngineEventListener {

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
    private var pointsSinceStart = 0L
    private var lastDevicePath: String? = null

    override suspend fun connect(target: EngineTarget): Result<Unit> = withContext(Dispatchers.IO) {
        if (target.sensor != SensorType.COIN_D6) {
            return@withContext Result.failure(
                UnsupportedOperationException("RealEngineBridge: ${target.sensor} connect arrives with B3/B9"),
            )
        }
        if (!ScanEngineNative.isAvailable) {
            _connectionState.value = ConnectionState.ERROR
            return@withContext Result.failure(IllegalStateException("scanengine_jni native library not loaded"))
        }
        val devicePath = target.transportHint
            ?: return@withContext Result.failure(IllegalArgumentException("D6 connect needs a device path"))
        val conn = connectionRegistry.get(devicePath)
            ?: return@withContext Result.failure(
                IllegalStateException(
                    "No open USB connection for $devicePath — the connect wizard must open+permission it first",
                ),
            )

        _connectionState.value = ConnectionState.CONNECTING
        _events.emit(EngineEvent.StatusMessage("Connecting to D6 on $devicePath…"))

        if (engineHandle == 0L) {
            engineHandle = ScanEngineNative.nativeCreateEngine(
                "lidarscan-android", LOG_LEVEL_INFO, 0, 0, 0,
            )
            if (engineHandle == 0L) {
                _connectionState.value = ConnectionState.ERROR
                val message = "scan_engine_create failed: ${ScanEngineNative.nativeLastError()}"
                _events.emit(EngineEvent.Fault("CREATE_FAILED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
        }

        val writer = ScanEngineNative.SerialWriter { data -> conn.write(data) }
        val id = ScanEngineNative.nativeAddD6Device(engineHandle, devicePath, DEFAULT_BAUD, true, writer)
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
        _events.emit(EngineEvent.StatusMessage("Connected to D6 on $devicePath"))
        Result.success(Unit)
    }

    override suspend fun disconnect() {
        stopCapture()
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

    override suspend fun startCapture(projectDirectory: String, liveSlam: Boolean): Result<Unit> =
        withContext(Dispatchers.IO) {
            if (_connectionState.value != ConnectionState.CONNECTED) {
                val message = "Cannot start capture: not connected"
                _events.emit(EngineEvent.Fault("NOT_CONNECTED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
            // Record-always (Tech Spec §3, key rule 2): `record` is always true
            // here, independent of `liveSlam`. `liveSlam` itself has no
            // scan_session_config equivalent in the C ABI today (start/stop
            // only) — see android/NOTES.md's "C ABI gaps"; it is recorded only
            // for this bridge's own status text until that lands.
            val err = ScanEngineNative.nativeStartSession(engineHandle, projectDirectory, DEFAULT_PROFILE, true)
            if (err != ScanEngineNative.ErrorCode.OK) {
                val message = "scan_engine_start failed: ${ScanEngineNative.nativeErrorStr(err)}"
                _events.emit(EngineEvent.Fault("START_FAILED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
            recordingStartNs = System.nanoTime()
            pointsSinceStart = 0L
            _captureState.value = CaptureState.RECORDING
            _events.emit(
                EngineEvent.StatusMessage(
                    "Recording to $projectDirectory (${if (liveSlam) "live SLAM" else "record-only"})",
                ),
            )
            Result.success(Unit)
        }

    override suspend fun pauseCapture(): Result<Unit> {
        if (_captureState.value != CaptureState.RECORDING) return Result.success(Unit)
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
        _events.emit(EngineEvent.StatusMessage("Capture stopped — $pointsSinceStart pts"))
        Result.success(Unit)
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
                pointsSinceStart += i2 // payload.points.count
                val elapsedMs = ((System.nanoTime() - recordingStartNs) / 1_000_000L).coerceAtLeast(1L)
                _events.tryEmit(EngineEvent.CaptureStats(pointsSinceStart, elapsedMs))
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
        const val DEFAULT_BAUD = 230_400
        const val DEFAULT_PROFILE = "quickscan"
        const val LOG_LEVEL_INFO = 2
        const val HEALTH_POLL_INTERVAL_MS = 500L
    }
}
