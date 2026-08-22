package com.lidarscan.core.capture

import com.lidarscan.core.calib.HoldOrientation
import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.StartOrientation
import com.lidarscan.core.calib.Vec3
import kotlin.math.exp
import kotlin.math.sqrt

/**
 * ROUND 30 item 175 — **the attitude indicator's missing half: a source.**
 *
 * Round 28 built [AttitudeIndicator]'s angle mapping and unit tested it against
 * literal angles, and it was right. What it was never given was a moving
 * number. The screen fed it `startOrientation.screenUpAngleDeg` — round 26 item
 * 125(b)'s **property of the capture**, written once when the mount hold
 * settles and explicitly documented as "left alone for the rest of the
 * capture". So the instrument was fed a constant: null while the hold-still
 * card was up (nothing has settled yet, so the ring drew with no needle at all)
 * and one frozen value for the whole walk afterwards. The owner's report — "it
 * does not move" — is that sentence in the field.
 *
 * This class is the thing that moves. It is a **direction filter**, not an
 * angle filter, and everything else about it follows from that choice.
 *
 * ### Why the low-pass runs on the vector and not on the angle
 *
 * Smoothing degrees means smoothing a quantity that wraps. A phone drifting
 * across the ±180° seam — which a landscape hold is one hand-tremble away from
 * doing — would make an angle filter sweep the needle the long way round the
 * dial, once per wobble. There is no wrap in a unit vector. The filter holds a
 * smoothed **world-up direction in the device frame**, and the angle is derived
 * from the smoothed direction afterwards by
 * [StartOrientation.fromDeviceUp] — which is round 26's own quadrant decoder,
 * reused rather than restated, so the live instrument and the logged start
 * orientation can never disagree about what "landscape-left" means.
 *
 * That reuse is also what satisfies the orientation requirement without a
 * branch: `fromDeviceUp` returns the quadrant AND the `confident` flag (the
 * phone too flat for a screen-plane angle to mean anything), and
 * [AttitudeIndicator.deviationFromSquareDeg] already reads deviation from the
 * NEAREST square hold. A landscape hold's "level" is landscape-level, in all
 * four quadrants, because nothing here knows which quadrant it is in.
 *
 * ### Two time constants, because there are two kinds of input
 *
 * `TYPE_GRAVITY` and an ARCore camera pose are both already fused with the
 * gyro upstream: they are smooth, and over-filtering them only adds lag to an
 * instrument the operator is meant to react to. A raw `TYPE_ACCELEROMETER`
 * stream is not — a walking gait puts one to two m/s² of linear acceleration
 * into it at about 2 Hz, which is the exact frequency an under-damped needle
 * would render as a twitch per footfall. Hence [TAU_FUSED_MS] and
 * [TAU_RAW_ACCEL_MS], chosen per call site rather than averaged into one
 * compromise that is wrong for both.
 *
 * ### The throttle is here and not in the UI
 *
 * The fastest input is the round-9 phone IMU at 400 Hz. Compose state written
 * 400 times a second is 400 recompositions a second of a canvas whose needle
 * moves less than a degree between frames. [MIN_EMIT_INTERVAL_MS] is 50 ms —
 * 20 Hz, inside the 15–30 Hz the instrument is specified at and comfortably
 * under a display refresh — and every sample is still *filtered*; only its
 * publication is throttled. The counters are exact, the visibility is not,
 * which is the same rule `PhoneImuRecorder.publishStatusOccasionally` follows
 * for the same reason.
 *
 * Not thread-safe by construction: samples can arrive from the sensor
 * HandlerThread and from the ARCore GL thread, and the caller that owns both is
 * the one place a lock belongs. `DeviceAttitudeSource` is that caller.
 */
class LiveAttitude {

    private var upX = 0.0
    private var upY = 0.0
    private var upZ = 0.0

    /** False until a usable sample has arrived; the instrument reads UNKNOWN until then. */
    private var seeded = false

    private var lastSampleMs = 0L
    private var lastEmitMs = 0L

    /** The smoothed hold, or null when nothing usable has arrived yet. */
    val hold: HoldOrientation?
        get() = if (!seeded) null else StartOrientation.fromDeviceUp(Vec3(upX, upY, upZ))

    /** The smoothed hold as the instrument reads it. [AttitudeIndicator.UNKNOWN] before the first sample. */
    val reading: AttitudeIndicator.Reading
        get() = hold?.let { AttitudeIndicator.reading(it.screenUpAngleDeg, it.confident) }
            ?: AttitudeIndicator.UNKNOWN

    /** Forgets the filter state. Called when the feed is released, so a re-entry never shows a stale needle. */
    fun reset() {
        seeded = false
        lastSampleMs = 0L
        lastEmitMs = 0L
    }

    /**
     * Feed one **world-up direction in the DEVICE frame** — the vector an
     * accelerometer at rest reports, and the vector
     * [StartOrientation.worldUpInDevice] returns for a camera pose.
     *
     * @return the smoothed hold when [MIN_EMIT_INTERVAL_MS] has passed since
     *   the last publication (the caller should write it to its state), or null
     *   when the sample was filtered in but is not due to be published — or was
     *   not usable at all.
     */
    fun onDeviceUp(x: Double, y: Double, z: Double, tMillis: Long, tauMs: Double = TAU_FUSED_MS): HoldOrientation? {
        val norm = sqrt(x * x + y * y + z * z)
        // A vector this short is free fall, a dropped phone, or a sensor that
        // has stopped reporting; there is no direction in it to filter towards.
        if (!norm.isFinite() || norm < MIN_MAGNITUDE) return null
        val nx = x / norm
        val ny = y / norm
        val nz = z / norm

        if (!seeded) {
            upX = nx
            upY = ny
            upZ = nz
            seeded = true
            lastSampleMs = tMillis
            lastEmitMs = tMillis
            // The first reading is published immediately: an instrument that
            // waits 50 ms to admit it can read is indistinguishable, to the
            // operator opening the card, from one that cannot.
            return hold
        }

        // Exponential smoothing with a TIME constant rather than a fixed
        // per-sample weight: the same filter has to behave identically at the
        // IMU's 400 Hz, the gravity sensor's 50 Hz and ARCore's 30 Hz, and a
        // fixed alpha would be three different filters on those three streams.
        val dtMs = (tMillis - lastSampleMs).coerceAtLeast(0L).toDouble()
        lastSampleMs = tMillis
        val alpha = if (tauMs <= 0.0) 1.0 else (1.0 - exp(-dtMs / tauMs)).coerceIn(0.0, 1.0)
        upX += alpha * (nx - upX)
        upY += alpha * (ny - upY)
        upZ += alpha * (nz - upZ)
        // Re-normalise: the linear blend of two unit vectors is short of unit
        // length, and `fromDeviceUp` measures tilt-from-flat against the
        // vector's own magnitude, so a shrinking vector would slowly report a
        // phone as flatter than it is.
        val m = sqrt(upX * upX + upY * upY + upZ * upZ)
        if (m > 1e-9) {
            upX /= m
            upY /= m
            upZ /= m
        }

        if (tMillis - lastEmitMs < MIN_EMIT_INTERVAL_MS) return null
        lastEmitMs = tMillis
        return hold
    }

    /**
     * The same filter fed from an ARCore camera attitude — `q_world_from_camera`,
     * exactly what `CaptureArController.publishPose` already has in its hand.
     *
     * The hop from camera frame to device frame is [StartOrientation
     * .worldUpInDevice], which is round 26's derivation through
     * `SENSOR_ORIENTATION`; doing it here rather than in the Android layer is
     * what keeps the whole chain on the JVM where it can be tested.
     */
    fun onCameraPose(
        worldFromCamera: Quat,
        sensorOrientationDeg: Int?,
        tMillis: Long,
    ): HoldOrientation? {
        val up = StartOrientation.worldUpInDevice(worldFromCamera, sensorOrientationDeg)
        return onDeviceUp(up.x, up.y, up.z, tMillis, TAU_FUSED_MS)
    }

    companion object {
        /**
         * 150 ms, for streams the platform has already fused (`TYPE_GRAVITY`,
         * an ARCore pose). Long enough that hand tremble does not reach the
         * needle, short enough that a deliberate tilt is on the dial before the
         * operator has finished making it.
         */
        const val TAU_FUSED_MS: Double = 150.0

        /**
         * 400 ms, for a raw accelerometer. A 2 Hz gait is attenuated by roughly
         * `1/(2π·2·0.4) ≈ 1/5` at this time constant, which puts a walking
         * operator's footfall under a degree on the dial while a real one-second
         * tilt still arrives essentially complete.
         */
        const val TAU_RAW_ACCEL_MS: Double = 400.0

        /** 50 ms between publications: 20 Hz into Compose state. */
        const val MIN_EMIT_INTERVAL_MS: Long = 50L

        /**
         * A tenth of a g. Below this the sample carries no usable direction —
         * free fall, or a sensor reporting zeros — and is dropped rather than
         * normalised into noise.
         */
        const val MIN_MAGNITUDE: Double = 0.98
    }
}
