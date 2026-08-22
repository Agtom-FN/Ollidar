package com.lidarscan.core.capture

import com.lidarscan.core.calib.HoldOrientation
import com.lidarscan.core.calib.Quat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * ROUND 30 item 175 — **which stream the attitude instrument listens to, and
 * when it listens at all.**
 *
 * [LiveAttitude] is the filter. This is everything around it that is a
 * decision rather than an Android API: the priority between three possible
 * inputs, the reference count that keeps a sensor off the battery while nothing
 * on screen is drawing a needle, and the `StateFlow` the instrument reads.
 *
 * It lives in `:core` and not beside the `SensorManager` because every one of
 * those is a rule that can be wrong in a way no emulator screenshot would show
 * — an instrument that keeps a listener registered after the screen left, or
 * one that goes deaf when ARCore stops tracking, both look perfect in a
 * photograph. `LiveAttitudeFeedTest` is what says they do not happen.
 *
 * ### The three inputs, in order
 *
 * 1. **[onCameraPose]** — ARCore's `q_world_from_camera`, the best answer there
 *    is while a session is tracking: fused against gyro *and* vision, already
 *    flowing through the hold and the whole recording, and gravity-aligned by
 *    construction. Wins for [POSE_PRIORITY_MS] after each sample.
 * 2. **[onImuAccel]** — a copy of the round-9 phone IMU's accelerometer, which
 *    is registered at 400 Hz on its own thread for the engine's densifier
 *    throughout every recording. Free.
 * 3. **[onGravity]** — a listener this feed asks for, and only when the round-9
 *    stream is not already serving it. That is the hold-still card's case: the
 *    card is up before `startPhoneImu()` runs, so during the one stage whose
 *    entire instruction is *hold still*, nothing else is listening at all.
 *
 * ### Two different kinds of "is something else feeding me?"
 *
 * The IMU's answer is a **flag** ([onImuFeedStarted] / [onImuFeedStopped]),
 * because `PhoneImuRecorder.start`/`stop` are a deterministic pair on the
 * capture's own path — there is a signal to trust, and while it is set this
 * feed asks for no sensor of its own.
 *
 * ARCore's answer is a **deadline**, and it deliberately does not reach the
 * registration decision at all. A tracking session can stop delivering frames
 * at any moment with no notification (round 27 item 142 is an entire item about
 * exactly that), so a listener released because ARCore was talking would go
 * silent the moment ARCore stopped — which is this round's freeze bug wearing a
 * new hat. Pose samples outrank gravity samples; they never silence the sensor.
 */
class LiveAttitudeFeed(
    /** Seam for tests: the millisecond clock every sample is stamped with. */
    private val clock: () -> Long = { System.currentTimeMillis() },
) {

    private val lock = Any()
    private val filter = LiveAttitude()

    private val _attitude = MutableStateFlow<HoldOrientation?>(null)

    /**
     * The live hold, republished at [LiveAttitude.MIN_EMIT_INTERVAL_MS].
     *
     * Null means **no reading**: before the first sample, and again once the
     * last subscriber leaves. The instrument draws its ring with no needle for
     * null, which is round 28's rule that an instrument admitting it cannot
     * read is trusted and one showing a plausible wrong number once is not.
     */
    val attitude: StateFlow<HoldOrientation?> = _attitude.asStateFlow()

    private var subscribers = 0
    private var imuFeeding = false
    private var poseFreshUntilMs = 0L
    private var sensorWanted = false

    /**
     * Called — **outside** the feed's own lock — whenever the answer to "should
     * the platform hold a gravity listener for me?" changes. The Android layer
     * registers on true and unregisters on false, and owns nothing else.
     */
    @Volatile
    var onSensorWantedChanged: ((Boolean) -> Unit)? = null

    /** Whether a listener should currently be registered. */
    val isSensorWanted: Boolean get() = synchronized(lock) { sensorWanted }

    /** How many surfaces currently want the instrument fed. */
    val subscriberCount: Int get() = synchronized(lock) { subscribers }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /** One more surface is drawing a needle. Balanced by [release]. */
    fun acquire() = mutate { subscribers++ }

    /**
     * One fewer. At zero the filter is reset and the reading cleared, so
     * re-entering a screen can never show the needle where it was left rather
     * than where the phone is.
     */
    fun release() = mutate {
        subscribers = (subscribers - 1).coerceAtLeast(0)
        if (subscribers == 0) {
            filter.reset()
            poseFreshUntilMs = 0L
            _attitude.value = null
        }
    }

    /** The round-9 IMU has its own accelerometer registration; this feed needs none. */
    fun onImuFeedStarted() = mutate { imuFeeding = true }

    /** It has stopped; if anything is still watching, take a sensor back. */
    fun onImuFeedStopped() = mutate { imuFeeding = false }

    // ── Samples ────────────────────────────────────────────────────────────

    /**
     * One ARCore frame's camera attitude. Ignored while [tracking] is false: a
     * pose ARCore itself has stopped believing is precisely the stale number
     * this item is about.
     */
    fun onCameraPose(worldFromCamera: Quat, sensorOrientationDeg: Int?, tracking: Boolean) {
        if (!tracking) return
        synchronized(lock) {
            if (subscribers == 0) return
            val now = clock()
            poseFreshUntilMs = now + POSE_PRIORITY_MS
            filter.onCameraPose(worldFromCamera, sensorOrientationDeg, now)?.let { _attitude.value = it }
        }
    }

    /** One accelerometer sample the round-9 IMU stream was handed anyway. */
    fun onImuAccel(x: Double, y: Double, z: Double) =
        feed(x, y, z, LiveAttitude.TAU_RAW_ACCEL_MS)

    /**
     * One sample from this feed's own listener. [fused] is true for
     * `TYPE_GRAVITY` — the platform's gyro-fused estimate, which needs far less
     * smoothing than the raw acceleration a walking operator produces.
     */
    fun onGravity(x: Double, y: Double, z: Double, fused: Boolean) =
        feed(x, y, z, if (fused) LiveAttitude.TAU_FUSED_MS else LiveAttitude.TAU_RAW_ACCEL_MS)

    private fun feed(x: Double, y: Double, z: Double, tauMs: Double) {
        synchronized(lock) {
            if (subscribers == 0) return
            val now = clock()
            if (now < poseFreshUntilMs) return
            filter.onDeviceUp(x, y, z, now, tauMs)?.let { _attitude.value = it }
        }
    }

    /**
     * Every state transition goes through here, so the one rule that decides
     * whether a sensor is held is written once. The callback fires outside the
     * lock: registering a listener can start delivering samples on another
     * thread immediately, and those samples come straight back in here.
     */
    private inline fun mutate(block: () -> Unit) {
        var changedTo: Boolean? = null
        synchronized(lock) {
            block()
            val want = subscribers > 0 && !imuFeeding
            if (want != sensorWanted) {
                sensorWanted = want
                changedTo = want
            }
        }
        changedTo?.let { onSensorWantedChanged?.invoke(it) }
    }

    companion object {
        /**
         * 200 ms. Six ARCore frames at its measured 30 Hz, so one dropped frame
         * never hands the instrument back and forth; short enough that a
         * session which stops delivering is replaced by gravity before the
         * operator's next step.
         */
        const val POSE_PRIORITY_MS = 200L
    }
}
