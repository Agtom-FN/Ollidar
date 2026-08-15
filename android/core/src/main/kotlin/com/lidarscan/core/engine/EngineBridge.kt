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

    /**
     * Latest known health for the connected device, or `null` before a first
     * sample arrives / after disconnect. Backs B2's connect-wizard health
     * panel (pts/s, rotation Hz, checksum pass rate, state). Mirrors the C
     * ABI's `scan_device_health` field-for-field (see
     * `engine/capi/scanengine_c.h`) — [RealEngineBridge] polls
     * `scan_engine_device_health` on an interval (that struct has no
     * corresponding push event on the wire today; see android/NOTES.md's
     * "C ABI gaps" for why) and republishes it here.
     */
    val deviceHealth: StateFlow<DeviceHealth?>

    /** Opens the sensor transport and brings the engine to [ConnectionState.CONNECTED]. */
    suspend fun connect(target: EngineTarget): Result<Unit>

    /** Closes the sensor transport. Safe to call when already disconnected. */
    suspend fun disconnect()

    /**
     * Starts writing to `<project>/streams/` (Record-always, §3.11) and, when
     * [liveSlam] is true, running the live SLAM preview (§3.3) alongside.
     * [projectDirectory] is the absolute path to the `.lscan` directory.
     */
    /**
     * @param profile the engine's `scan_session_config.profile` string —
     *   `survey | floorplan | research | quickscan`. B5 added it: B2 passed the
     *   literal `"quickscan"` for every project because `EngineBridge` had no
     *   way to carry the project's own choice, and `scanengine_c.h` names the
     *   four values only in a header comment with no enum, so
     *   [com.lidarscan.core.model.CaptureDefaults.engineProfileString] is the
     *   single place the app spells them.
     */
    suspend fun startCapture(projectDirectory: String, liveSlam: Boolean, profile: String = "quickscan"): Result<Unit>

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

/**
 * Field-for-field mirror of `scan_device_health` (`engine/capi/scanengine_c.h`).
 * Deliberately plain `Int`s for [kind]/[state]/[lastError] rather than a
 * `:core`-owned enum — those numeric spaces belong to the C ABI (`SCAN_DEVICE_*`
 * / `SCAN_DEV_*` / `scan_error_t`), which `:core` has no dependency on and
 * should not re-encode. UI code that needs labels maps these ints against
 * the ABI's constants (see `com.lidarscan.app.engine.ScanEngineNative`'s
 * companion constants).
 */
data class DeviceHealth(
    val id: Int,
    val kind: Int,
    val state: Int,
    val lastError: Int,
    val bytesIn: Long,
    val packetsOk: Long,
    val packetsBad: Long,
    val pointsOut: Long,
    val drops: Long,
    val pointsPerSec: Double,
    val rotationHz: Double,
    val checksumPassRate: Double,
    val tLastDataNs: Long,
)
