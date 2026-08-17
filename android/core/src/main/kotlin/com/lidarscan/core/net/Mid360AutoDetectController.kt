package com.lidarscan.core.net

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * AUTO-DETECT wizard state: what the "Auto-detect" step of the Mid-360
 * connect wizard shows.
 *
 * [hostMatches] is null until a heartbeat is found — it is computed against
 * whatever local Ethernet addresses the caller passes to
 * [Mid360AutoDetectController.start], not stored redundantly here.
 */
data class Mid360AutoDetectState(
    val status: Status = Status.IDLE,
    val elapsedMs: Long = 0,
    val timeoutMs: Long = Mid360AutoDetectController.DEFAULT_TIMEOUT_MS,
    val found: Mid360Heartbeat? = null,
    val hostMatches: Boolean? = null,
    val message: String? = null,
) {
    enum class Status { IDLE, LISTENING, FOUND, TIMED_OUT, ERROR }

    val progress: Float
        get() = if (timeoutMs <= 0) 0f else (elapsedMs.toFloat() / timeoutMs.toFloat()).coerceIn(0f, 1f)
}

/**
 * Pure-Kotlin state machine driving the Mid-360 connect wizard's
 * "Auto-detect" step (Tech Spec §3.1 Android row — owner-added AUTO-DETECT
 * requirement). Same split as [D6ConnectController] one file over: no
 * Android dependency here, so it is JVM-testable against a fake
 * [Mid360Detector]; the real detector
 * (`com.lidarscan.app.net.UdpMid360Detector`, `:app`) is a plain
 * `DatagramSocket` and is exercised only by instrumentation/manual test.
 */
class Mid360AutoDetectController(
    private val detector: Mid360Detector,
    private val scope: CoroutineScope,
) {
    private val _state = MutableStateFlow(Mid360AutoDetectState())
    val state: StateFlow<Mid360AutoDetectState> = _state.asStateFlow()

    private var job: Job? = null

    /**
     * Starts listening. [localAddresses] is a supplier (not a snapshot) so
     * the freshest Ethernet-interface addresses are read at the moment a
     * heartbeat actually arrives, not whatever they were when `start` was
     * called — the interface can pick up its address seconds into the
     * listen window.
     */
    fun start(timeoutMs: Long = DEFAULT_TIMEOUT_MS, localAddresses: () -> List<LocalAddress>) {
        job?.cancel()
        _state.value = Mid360AutoDetectState(status = Mid360AutoDetectState.Status.LISTENING, timeoutMs = timeoutMs)
        job = scope.launch {
            val result = detector.detect(timeoutMs) { elapsed ->
                _state.value = _state.value.copy(elapsedMs = elapsed)
            }
            _state.value = when (result) {
                is Mid360DetectionResult.Found -> {
                    val matches = localAddresses().any { it.ip == result.heartbeat.persistedHostIp }
                    _state.value.copy(
                        status = Mid360AutoDetectState.Status.FOUND,
                        elapsedMs = _state.value.timeoutMs,
                        found = result.heartbeat,
                        hostMatches = matches,
                    )
                }

                Mid360DetectionResult.TimedOut ->
                    _state.value.copy(status = Mid360AutoDetectState.Status.TIMED_OUT)

                is Mid360DetectionResult.Error ->
                    _state.value.copy(status = Mid360AutoDetectState.Status.ERROR, message = result.message)
            }
        }
    }

    /** Cancels an in-flight listen. Safe to call at any state. */
    fun cancel() {
        job?.cancel()
        job = null
        if (_state.value.status == Mid360AutoDetectState.Status.LISTENING) {
            _state.value = _state.value.copy(status = Mid360AutoDetectState.Status.IDLE)
        }
    }

    /** Back to the initial, un-run state — used by "Enter manually" and by leaving the screen. */
    fun reset() {
        cancel()
        _state.value = Mid360AutoDetectState()
    }

    companion object {
        /**
         * ~5 s (owner brief). The self-test window one card down
         * ([com.lidarscan.core.net.Mid360SelfTest.WINDOW_MS]) is 8 s for a
         * *configured* device's first packet after `add_device`+`start`; a
         * heartbeat needs no handshake and the device broadcasts one every
         * second, so 5 s is roomy (covers a miss of the first one or two).
         */
        const val DEFAULT_TIMEOUT_MS = 5_000L
    }
}
