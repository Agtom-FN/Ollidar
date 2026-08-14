package com.lidarscan.core.engine

import com.lidarscan.core.model.SensorType
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow

/**
 * The seam between the Android app and `libscanengine` (Tech Spec §3, "Engine
 * owns all sensor data" / "UIs pass opaque buffers in ... and get render
 * handles + status events out"). This B1 scaffold only needs
 * connect/startCapture/stopCapture/events, per the task brief — A1 defines
 * the engine's real C ABI and B2 (D6) / B3 (Mid-360) wire the JNI
 * implementation of this same interface against it. Everything below is the
 * *contract* those workstreams build to, not a prediction of the engine's
 * internals.
 *
 * Threading: implementations may do blocking/native work inside the suspend
 * functions off the caller's dispatcher; [events], [connectionState] and
 * [captureState] are safe to collect from Compose (`collectAsStateWithLifecycle`).
 */
interface EngineBridge {

    /** Connection lifecycle, current value first on collection. */
    val connectionState: StateFlow<ConnectionState>

    /** Capture lifecycle, current value first on collection. */
    val captureState: StateFlow<CaptureState>

    /** Point/status/error stream for UI surfaces like the capture status strip and health panels (B2/B4). */
    val events: Flow<EngineEvent>

    /** Opens the sensor transport and brings the engine to [ConnectionState.CONNECTED]. */
    suspend fun connect(target: EngineTarget): Result<Unit>

    /** Closes the sensor transport. Safe to call when already disconnected. */
    suspend fun disconnect()

    /**
     * Starts writing to `<project>/streams/` (Record-always, §3.11) and, when
     * [liveSlam] is true, running the live SLAM preview (§3.3) alongside.
     * [projectDirectory] is the absolute path to the `.lscan` directory.
     */
    suspend fun startCapture(projectDirectory: String, liveSlam: Boolean): Result<Unit>

    suspend fun pauseCapture(): Result<Unit>

    suspend fun resumeCapture(): Result<Unit>

    /** Flushes and closes the active capture's streams. */
    suspend fun stopCapture(): Result<Unit>
}

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR,
}

enum class CaptureState {
    IDLE,
    RECORDING,
    PAUSED,
    STOPPING,
}

/** What to connect to. [transportHint] is a free-form string (e.g. a serial device path or IP) left to B2/B3's connect flows. */
data class EngineTarget(
    val sensor: SensorType,
    val transportHint: String? = null,
)

sealed interface EngineEvent {
    data class StatusMessage(val message: String, val level: Level = Level.INFO) : EngineEvent {
        enum class Level { INFO, WARNING, ERROR }
    }

    data class CaptureStats(
        val pointsCaptured: Long,
        val elapsedMillis: Long,
    ) : EngineEvent

    data class Fault(val code: String, val message: String) : EngineEvent
}
