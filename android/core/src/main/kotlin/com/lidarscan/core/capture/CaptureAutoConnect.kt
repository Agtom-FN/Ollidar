package com.lidarscan.core.capture

import com.lidarscan.core.model.SensorType
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * One sensor that answered an auto-detect probe.
 *
 * [transportHint] is what [com.lidarscan.core.engine.EngineTarget.transportHint]
 * needs for this sensor and nothing else: a USB device path for the D6, a
 * `"<lidarIp>|<hostIp>"` pair for the Mid-360. [label] is the one line the
 * capture screen shows ("COIN-D6 · /dev/bus/usb/001/003"), [detail] the quieter
 * second line (a serial number, a firmware string) or null.
 */
data class AutoDetection(
    val sensor: SensorType,
    val transportHint: String?,
    val label: String,
    val detail: String? = null,
)

/**
 * A single kind of auto-detect probe — the D6's serial-signature read, the
 * Mid-360's heartbeat listen. Returns null when nothing of its kind answered
 * inside its own window; it must not throw (the controller catches anyway, but
 * a detector that reports "not found" as an exception loses the distinction
 * between "no device" and "the probe itself broke").
 */
interface SensorAutoDetector {
    val sensor: SensorType
    suspend fun detect(): AutoDetection?
}

/**
 * Round 5's capture flow, as a state machine: **detected → connected → live
 * preview, with no taps in between.**
 *
 * ```
 *   IDLE ──start()──► SEARCHING ──found──► CONNECTING ──ok──► PREVIEW
 *                         │                     │
 *                     nothing               connect failed
 *                         ▼                     ▼
 *                       FAILED  ◄───────────────┘
 * ```
 *
 * [PREVIEW][CaptureAutoConnectState.Phase.PREVIEW] is the state the owner asked
 * for in item 10: the sensor is connected and streaming into the live viewport
 * but **nothing is being recorded yet**, so every display parameter can be tuned
 * against real points before Start creates a project. There is deliberately no
 * self-test phase between CONNECTING and PREVIEW — round 5 item 7 is explicit
 * that "live preview showing points IS the proof a device works".
 *
 * Same split as [com.lidarscan.core.engine.D6ConnectController] and
 * [com.lidarscan.core.net.Mid360AutoDetectController]: no Android dependency, so
 * the whole flow is JVM-testable against fake detectors and a fake connect
 * lambda.
 */
class CaptureAutoConnectController(
    private val detectors: List<SensorAutoDetector>,
    /** Brings the engine up on a detection. Typically `engineBridge::connect` wrapped in an `EngineTarget`. */
    private val connect: suspend (AutoDetection) -> Result<Unit>,
    private val scope: CoroutineScope,
) {
    private val _state = MutableStateFlow(CaptureAutoConnectState())
    val state: StateFlow<CaptureAutoConnectState> = _state.asStateFlow()

    private var job: Job? = null

    /**
     * Runs the whole detect → connect → preview sequence. Safe to call at any
     * phase: an in-flight attempt is cancelled first, so a Retry tap can never
     * leave two probes racing for the same USB port.
     */
    fun start() {
        job?.cancel()
        _state.value = CaptureAutoConnectState(phase = CaptureAutoConnectState.Phase.SEARCHING)
        job = scope.launch {
            val detection = detectFirst()
            if (detection == null) {
                // ROUND 5 (owner mockup review, addition 1): **nothing found
                // flows straight into manual entry** — the fields are already
                // open and filled with the last-known addresses by the time the
                // operator looks up, rather than behind a "Enter manually" tap
                // they have to find first. The auto-detect line above them still
                // says what happened, and Retry is still one tap.
                _state.value = CaptureAutoConnectState(
                    phase = CaptureAutoConnectState.Phase.FAILED,
                    message = NOTHING_FOUND,
                    manualEntryOpen = true,
                )
                return@launch
            }
            _state.value = CaptureAutoConnectState(
                phase = CaptureAutoConnectState.Phase.CONNECTING,
                detection = detection,
            )
            connectTo(detection, manualEntryOpen = false)
        }
    }

    /**
     * ROUND 5: connect to something the operator typed/picked in the manual
     * panel, bypassing detection entirely.
     *
     * Keeps the manual panel open on the way through — a manual connect that
     * fails must leave the fields exactly where the operator can fix them, and a
     * manual connect that succeeds should not make the panel it was driven from
     * vanish underneath the finger that used it.
     */
    fun connectManually(detection: AutoDetection) {
        job?.cancel()
        _state.value = CaptureAutoConnectState(
            phase = CaptureAutoConnectState.Phase.CONNECTING,
            detection = detection,
            manualEntryOpen = true,
        )
        job = scope.launch { connectTo(detection, manualEntryOpen = true) }
    }

    private suspend fun connectTo(detection: AutoDetection, manualEntryOpen: Boolean) {
        val result = runCatching { connect(detection) }.getOrElse { Result.failure(it) }
        _state.value = result.fold(
            onSuccess = {
                CaptureAutoConnectState(
                    phase = CaptureAutoConnectState.Phase.PREVIEW,
                    detection = detection,
                    manualEntryOpen = manualEntryOpen,
                )
            },
            onFailure = { e ->
                CaptureAutoConnectState(
                    phase = CaptureAutoConnectState.Phase.FAILED,
                    detection = detection,
                    message = e.message ?: "could not connect to ${detection.label}",
                    // A failed connect always opens the manual panel, whichever
                    // path got here: auto-detect found a device and the engine
                    // still refused it, so the addresses/port are the next thing
                    // to look at.
                    manualEntryOpen = true,
                )
            },
        )
    }

    /**
     * Opens the manual panel by hand — the "Enter manually" affordance that
     * stays reachable **even after a successful detect** (owner addition 1), for
     * the rig where auto-detect finds the wrong one of two devices.
     */
    fun showManualEntry() {
        _state.value = _state.value.copy(manualEntryOpen = true)
    }

    /** Closes the manual panel without touching the connection. */
    fun hideManualEntry() {
        _state.value = _state.value.copy(manualEntryOpen = false)
    }

    /** Same as [start] — named for what the button says. */
    fun retry() = start()

    /** Cancels an in-flight attempt and drops back to [CaptureAutoConnectState.Phase.IDLE]. */
    fun cancel() {
        job?.cancel()
        job = null
        val phase = _state.value.phase
        if (phase == CaptureAutoConnectState.Phase.SEARCHING || phase == CaptureAutoConnectState.Phase.CONNECTING) {
            _state.value = CaptureAutoConnectState()
        }
    }

    /**
     * The engine reported the transport gone (USB detach, Ethernet down) while
     * we were in preview. Reported rather than silently re-probed: a device that
     * vanished mid-preview is something the operator has to see, and the Retry
     * button is one tap.
     */
    fun onConnectionLost() {
        if (_state.value.phase != CaptureAutoConnectState.Phase.PREVIEW) return
        _state.value = _state.value.copy(
            phase = CaptureAutoConnectState.Phase.FAILED,
            message = "The sensor disconnected. Check the cable and tap Retry.",
            // Same rule as a failed connect: the manual panel comes up with it,
            // so the operator can re-pick a port or re-type an address without
            // hunting for the affordance first.
            manualEntryOpen = true,
        )
    }

    /**
     * Races every detector and returns the first one to answer, cancelling the
     * rest. First-past-the-post rather than "probe the D6, then fall back to the
     * Mid-360" because the two probes touch completely different hardware (a USB
     * serial port, a UDP socket) and running them in sequence would make a
     * Mid-360 operator wait out the D6's window for nothing.
     */
    private suspend fun detectFirst(): AutoDetection? {
        if (detectors.isEmpty()) return null
        return coroutineScope {
            val winner = CompletableDeferred<AutoDetection?>()
            val outstanding = AtomicInteger(detectors.size)
            val probes = detectors.map { detector ->
                launch {
                    val found = runCatching { detector.detect() }.getOrNull()
                    if (found != null) {
                        winner.complete(found)
                    } else if (outstanding.decrementAndGet() == 0) {
                        winner.complete(null)
                    }
                }
            }
            val result = winner.await()
            probes.forEach { it.cancel() }
            result
        }
    }

    companion object {
        /**
         * ROUND 24 item 110(a). Was: "No sensor found. Plug the COIN-D6 into
         * USB-C (or attach the Mid-360's Ethernet adapter) and tap Retry." —
         * eighteen words naming two products and a connector, on the screen a
         * first-run operator meets. The manual panel directly underneath it
         * already says how to plug in each sensor, so this line only has to
         * report and point.
         */
        const val NOTHING_FOUND: String = "No scanner found. Plug it in, then Retry."
    }
}

/** What the Capture tab shows above the live viewport. */
data class CaptureAutoConnectState(
    val phase: Phase = Phase.IDLE,
    val detection: AutoDetection? = null,
    val message: String? = null,
    /**
     * ROUND 5 (owner addition 1): whether the inline manual-entry panel is
     * showing. Set automatically when auto-detect finds nothing or a connect
     * fails — the fallback is a *flow*, not a button the operator has to
     * discover — and settable by hand at any phase, including during a live
     * preview.
     */
    val manualEntryOpen: Boolean = false,
) {
    enum class Phase { IDLE, SEARCHING, CONNECTING, PREVIEW, FAILED }

    val sensor: SensorType? get() = detection?.sensor

    /** True once the sensor is streaming — the only phase in which Start can create a project and record. */
    val isPreviewing: Boolean get() = phase == Phase.PREVIEW

    val isBusy: Boolean get() = phase == Phase.SEARCHING || phase == Phase.CONNECTING

    /** The single status line the screen prints; never null, so the strip is never blank. */
    fun statusLine(): String = when (phase) {
        Phase.IDLE -> "Looking for a sensor…"
        Phase.SEARCHING -> "Searching for a COIN-D6 or Mid-360…"
        Phase.CONNECTING -> "Connecting to ${detection?.label ?: "the sensor"}…"
        Phase.PREVIEW -> "${detection?.label ?: "Sensor"} · live preview (not recording)"
        Phase.FAILED -> message ?: CaptureAutoConnectController.NOTHING_FOUND
    }
}
