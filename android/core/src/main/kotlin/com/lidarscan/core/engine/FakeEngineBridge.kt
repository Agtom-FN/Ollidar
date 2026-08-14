package com.lidarscan.core.engine

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

/**
 * In-memory stand-in for the real JNI-backed [EngineBridge]. Lets B1's
 * Capture-screen stub (and B4 once it lands) exercise the full
 * connect → capture → stop lifecycle and see believable status/point-count
 * events without any native code or hardware attached. Selected by default
 * from [EngineBridgeProvider] until B2/B3/A1 register the real
 * implementation.
 *
 * Behaviour is intentionally simple and deterministic-ish (small simulated
 * delays, a steady fake point rate) — it is a UI-development aid, not a
 * simulator of engine semantics or failure modes.
 */
class FakeEngineBridge(
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
) : EngineBridge {

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    override val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _captureState = MutableStateFlow(CaptureState.IDLE)
    override val captureState: StateFlow<CaptureState> = _captureState.asStateFlow()

    private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 64)
    override val events: SharedFlow<EngineEvent> = _events.asSharedFlow()

    private val _deviceHealth = MutableStateFlow<DeviceHealth?>(null)
    override val deviceHealth: StateFlow<DeviceHealth?> = _deviceHealth.asStateFlow()

    private var statsJob: Job? = null
    private var pointsCaptured = 0L
    private var elapsedMillis = 0L

    override suspend fun connect(target: EngineTarget): Result<Unit> {
        _connectionState.value = ConnectionState.CONNECTING
        _events.emit(EngineEvent.StatusMessage("Connecting to fake ${target.sensor.displayName}…"))
        delay(FAKE_CONNECT_DELAY_MS)
        _connectionState.value = ConnectionState.CONNECTED
        _events.emit(EngineEvent.StatusMessage("Connected (fake engine — no hardware attached)"))
        return Result.success(Unit)
    }

    override suspend fun disconnect() {
        stopCapture()
        _connectionState.value = ConnectionState.DISCONNECTED
        _deviceHealth.value = null
        _events.emit(EngineEvent.StatusMessage("Disconnected"))
    }

    override suspend fun startCapture(projectDirectory: String, liveSlam: Boolean): Result<Unit> {
        if (_connectionState.value != ConnectionState.CONNECTED) {
            val message = "Cannot start capture: not connected"
            _events.emit(EngineEvent.Fault(code = "NOT_CONNECTED", message = message))
            return Result.failure(IllegalStateException(message))
        }
        pointsCaptured = 0
        elapsedMillis = 0
        _captureState.value = CaptureState.RECORDING
        _events.emit(
            EngineEvent.StatusMessage(
                "Recording to $projectDirectory (${if (liveSlam) "live SLAM" else "record-only"})",
            ),
        )
        statsJob?.cancel()
        statsJob = scope.launch { tickStats() }
        return Result.success(Unit)
    }

    override suspend fun pauseCapture(): Result<Unit> {
        if (_captureState.value != CaptureState.RECORDING) return Result.success(Unit)
        _captureState.value = CaptureState.PAUSED
        statsJob?.cancel()
        _events.emit(EngineEvent.StatusMessage("Capture paused"))
        return Result.success(Unit)
    }

    override suspend fun resumeCapture(): Result<Unit> {
        if (_captureState.value != CaptureState.PAUSED) return Result.success(Unit)
        _captureState.value = CaptureState.RECORDING
        statsJob?.cancel()
        statsJob = scope.launch { tickStats() }
        _events.emit(EngineEvent.StatusMessage("Capture resumed"))
        return Result.success(Unit)
    }

    override suspend fun stopCapture(): Result<Unit> {
        if (_captureState.value == CaptureState.IDLE) return Result.success(Unit)
        _captureState.value = CaptureState.STOPPING
        statsJob?.cancel()
        statsJob = null
        _events.emit(EngineEvent.StatusMessage("Capture stopped — $pointsCaptured pts (fake)"))
        _captureState.value = CaptureState.IDLE
        return Result.success(Unit)
    }

    private suspend fun tickStats() {
        while (true) {
            delay(STATS_TICK_MS)
            pointsCaptured += FAKE_POINTS_PER_TICK
            elapsedMillis += STATS_TICK_MS
            _events.emit(EngineEvent.CaptureStats(pointsCaptured, elapsedMillis))
            _deviceHealth.value = DeviceHealth(
                id = 0,
                kind = 1, // SCAN_DEVICE_D6
                state = 3, // SCAN_DEV_STREAMING
                lastError = 0,
                bytesIn = pointsCaptured * 16,
                packetsOk = pointsCaptured / 96,
                packetsBad = 0,
                pointsOut = pointsCaptured,
                drops = 0,
                pointsPerSec = FAKE_POINTS_PER_TICK * (1000.0 / STATS_TICK_MS),
                rotationHz = FAKE_ROTATION_HZ,
                checksumPassRate = FAKE_CHECKSUM_PASS_RATE,
                tLastDataNs = elapsedMillis * 1_000_000L,
            )
        }
    }

    private companion object {
        const val FAKE_CONNECT_DELAY_MS = 400L
        const val STATS_TICK_MS = 500L
        const val FAKE_POINTS_PER_TICK = 20_000L
        const val FAKE_ROTATION_HZ = 10.0
        const val FAKE_CHECKSUM_PASS_RATE = 0.999
    }
}
