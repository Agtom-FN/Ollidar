package com.lidarscan.app.engine

import android.content.Context
import com.lidarscan.app.render.NativePointCloudProvider
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.app.render.ReplayEngineCloudSource
import com.lidarscan.app.replay.SyntheticReplayAssets
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineEvent
import com.lidarscan.core.engine.EngineTarget
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
 * B4's acceptance path: drives the Capture screen (live 3D view, status
 * strip, session-summary sheet — the works) from the bundled synthetic D6
 * capture instead of live hardware, via `replay_engine.h`'s standalone
 * `ReplayEngine` (see that header for why replay needs its own
 * `scanengine::Engine` rather than reusing `RealEngineBridge`'s
 * `scan_engine*`). Selected by the "Replay synthetic capture" debug-drawer
 * action (`ui/settings/SettingsScreen.kt`) instead of the app-wide
 * [com.lidarscan.core.engine.EngineBridgeProvider] — this is a
 * throwaway, per-session bridge instance, not something other screens ever see.
 *
 * KNOWN GAP, DOCUMENTED NOT WORKED AROUND: [pauseCapture]/[resumeCapture]
 * fail cleanly (`Result.failure`) — see `replay_engine.h`'s header comment
 * for why `ReplaySource` has no pause/seek-and-resume primitive this bridge
 * could bind to without re-implementing engine/'s own pacing loop (out of
 * this task's read-only scope for engine/). `CaptureViewModel.isReplaySession`
 * is what lets the Capture screen hide the Pause button instead of shipping
 * a control that silently does nothing.
 */
class ReplayEngineBridge(
    private val context: Context,
    private val speed: Double = 1.0,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
) : EngineBridge, NativePointCloudProvider {

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    override val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _captureState = MutableStateFlow(CaptureState.IDLE)
    override val captureState: StateFlow<CaptureState> = _captureState.asStateFlow()

    private val _events = MutableSharedFlow<EngineEvent>(extraBufferCapacity = 64)
    override val events: SharedFlow<EngineEvent> = _events.asSharedFlow()

    private val _deviceHealth = MutableStateFlow<DeviceHealth?>(null)
    override val deviceHealth: StateFlow<DeviceHealth?> = _deviceHealth.asStateFlow()

    private var replayHandle: Long = 0L
    private var pollJob: Job? = null
    private var recordingStartNs = 0L

    override suspend fun connect(target: EngineTarget): Result<Unit> = withContext(Dispatchers.IO) {
        if (!ScanEngineNative.isAvailable) {
            _connectionState.value = ConnectionState.ERROR
            return@withContext Result.failure(IllegalStateException("scanengine_jni native library not loaded"))
        }
        _connectionState.value = ConnectionState.CONNECTING
        if (replayHandle == 0L) {
            replayHandle = ScanEngineNative.nativeReplayCreate()
        }
        if (replayHandle == 0L) {
            _connectionState.value = ConnectionState.ERROR
            val message = "replay engine create failed"
            _events.emit(EngineEvent.Fault("REPLAY_CREATE_FAILED", message))
            return@withContext Result.failure(IllegalStateException(message))
        }
        _connectionState.value = ConnectionState.CONNECTED
        _events.emit(EngineEvent.StatusMessage("Synthetic replay engine ready (no hardware — bundled S1 d6synth capture)"))
        Result.success(Unit)
    }

    override suspend fun disconnect() {
        stopCapture()
        if (replayHandle != 0L) {
            ScanEngineNative.nativeReplayDestroy(replayHandle)
            replayHandle = 0L
        }
        _connectionState.value = ConnectionState.DISCONNECTED
        _deviceHealth.value = null
    }

    /** [projectDirectory]/[liveSlam] are ignored — replay reads from the bundled asset and records nothing new. */
    override suspend fun startCapture(projectDirectory: String, liveSlam: Boolean, profile: String): Result<Unit> =
        withContext(Dispatchers.IO) {
            if (_connectionState.value != ConnectionState.CONNECTED) {
                val message = "Cannot start replay: engine not ready"
                _events.emit(EngineEvent.Fault("NOT_CONNECTED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
            val lscanDir = SyntheticReplayAssets.ensureExtracted(context)
            val started = ScanEngineNative.nativeReplayStart(replayHandle, lscanDir.absolutePath, speed)
            if (!started) {
                val message = "nativeReplayStart failed: ${ScanEngineNative.nativeReplayLastError(replayHandle)}"
                _events.emit(EngineEvent.Fault("REPLAY_START_FAILED", message))
                return@withContext Result.failure(IllegalStateException(message))
            }
            recordingStartNs = System.nanoTime()
            _captureState.value = CaptureState.RECORDING
            _events.emit(EngineEvent.StatusMessage("Replaying bundled synthetic capture at ${speed}x"))
            startPolling()
            Result.success(Unit)
        }

    override suspend fun pauseCapture(): Result<Unit> {
        val message = "Pause is not supported during synthetic replay (ReplaySource has no seek/resume primitive — see NOTES.md)"
        _events.emit(EngineEvent.StatusMessage(message, EngineEvent.StatusMessage.Level.WARNING))
        return Result.failure(UnsupportedOperationException(message))
    }

    override suspend fun resumeCapture(): Result<Unit> = pauseCapture()

    override suspend fun stopCapture(): Result<Unit> = withContext(Dispatchers.IO) {
        if (_captureState.value == CaptureState.IDLE) return@withContext Result.success(Unit)
        _captureState.value = CaptureState.STOPPING
        pollJob?.cancel()
        pollJob = null
        if (replayHandle != 0L) ScanEngineNative.nativeReplayStop(replayHandle)
        val stats = if (replayHandle != 0L) ScanEngineNative.nativeReplayStats(replayHandle) else null
        _captureState.value = CaptureState.IDLE
        _events.emit(
            EngineEvent.StatusMessage(
                "Replay stopped — ${stats?.chunksReplayed ?: 0} chunks / ${stats?.bytesReplayed ?: 0} bytes replayed",
            ),
        )
        Result.success(Unit)
    }

    override fun currentPointCloudSource(): PointCloudSource? =
        if (replayHandle != 0L) ReplayEngineCloudSource { replayHandle } else null

    private fun startPolling() {
        pollJob?.cancel()
        pollJob = scope.launch {
            while (true) {
                val handle = replayHandle
                if (handle == 0L) break
                val stats = ScanEngineNative.nativeReplayStats(handle)
                if (stats != null) {
                    val elapsedMs = ((System.nanoTime() - recordingStartNs) / 1_000_000L).coerceAtLeast(1L)
                    val points = ScanEngineNative.nativeReplayTotalPoints(handle)
                    _events.emit(EngineEvent.CaptureStats(points, elapsedMs))
                    if (stats.done) {
                        _events.emit(
                            EngineEvent.StatusMessage(
                                "Replay finished — ${stats.chunksReplayed} chunks, $points pts" +
                                    if (stats.resultError != 0) " (ended with error ${stats.resultError})" else "",
                            ),
                        )
                        _captureState.value = CaptureState.IDLE
                        break
                    }
                }
                val health = handle.let { ScanEngineNative.nativeReplayDeviceHealth(it) }
                if (health != null) {
                    _deviceHealth.value = DeviceHealth(
                        id = health.id,
                        kind = health.kind,
                        state = health.state,
                        lastError = health.lastError,
                        bytesIn = health.bytesIn,
                        packetsOk = health.packetsOk,
                        packetsBad = health.packetsBad,
                        pointsOut = health.pointsOut,
                        drops = health.drops,
                        pointsPerSec = health.pointsPerSec,
                        rotationHz = health.rotationHz,
                        checksumPassRate = health.checksumPassRate,
                        tLastDataNs = health.tLastDataNs,
                    )
                }
                delay(POLL_INTERVAL_MS)
            }
        }
    }

    private companion object {
        const val POLL_INTERVAL_MS = 250L
    }
}
