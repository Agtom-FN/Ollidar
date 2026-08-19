package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin

/**
 * ROUND 9, owner item 35 — **the phone's own IMU, the platform-free half.**
 *
 * The owner's sentence: *"lidar data and the imu position data need sync the
 * frequency."* The numbers behind it, measured on the owner's real `scan-017`:
 * ARCore hands out poses at ~30 Hz (150 poses over 4.999 s, median interval
 * 33.33 ms) while the COIN-D6 samples at 4000 Hz. Every D6 return therefore
 * lands between two ARCore poses, and until this round the only thing bridging
 * that gap was a straight LERP/SLERP — which is exact for a dolly and wrong for
 * a walking human, whose 2 Hz gait puts real angular motion inside every 33 ms
 * interval.
 *
 * The engine's answer is an IMU-densified pose interpolator: integrate the
 * phone's gyro between the bracketing ARCore poses instead of slerping through
 * them. Proven engine-side on a synthetic wall with 1.5 deg of 12 Hz jitter —
 * plane-fit RMS **0.739 cm (plain slerp) -> 0.021 cm (IMU-densified)**. It
 * needs the phone's raw gyro + accel, which this app has never pushed.
 *
 * Everything in this file is deliberately plain-JVM: no `SensorManager`, no
 * `SensorEvent`, no `Context`. The Android wiring is
 * `com.lidarscan.app.ar.PhoneImuRecorder`; the logic that can be wrong — the
 * two-stream merge, the clock-domain sanity check, and the
 * `SENSOR_ORIENTATION -> camera_from_imu` derivation — lives here so it is
 * testable without a device. (The capture AVD has no real IMU, so an
 * instrumented test could not check any of it either.)
 */

/** One fused IMU sample, in the ANDROID DEVICE frame, at the engine's clock. */
data class ImuSample(
    /**
     * `SensorEvent.timestamp`, **unconverted**. On virtually every device this
     * is already `CLOCK_BOOTTIME`, which is exactly the domain the engine and
     * ARCore's `Frame.getTimestamp()` use — so converting it would be the bug,
     * not the fix. [ImuClockDomain] is what checks the "virtually" part.
     */
    val tMonoNs: Long,
    /** Angular rate, rad/s, device frame (`TYPE_GYROSCOPE`). */
    val gx: Float,
    val gy: Float,
    val gz: Float,
    /** Specific force, m/s^2, device frame (`TYPE_ACCELEROMETER`), gravity included. */
    val ax: Float,
    val ay: Float,
    val az: Float,
)

/**
 * Merges the two independent Android sensor streams into one [ImuSample]
 * stream.
 *
 * **The choice, and why.** `TYPE_GYROSCOPE` and `TYPE_ACCELEROMETER` are
 * separate `SensorEventListener` callbacks with separate (usually equal, never
 * guaranteed equal) rates, and although each stream is monotonic in itself,
 * the two are *not* ordered against each other — an accel event stamped after
 * a gyro event can be delivered before it. Three options were on the table:
 *
 *  1. **hold the latest accel, emit on every gyro** — what this does;
 *  2. buffer both and interpolate accel to each gyro timestamp;
 *  3. emit on every event of either type, with the other half stale.
 *
 * (1) wins because the consumer is a *gyro integrator*. The densifier's whole
 * job is `∫ω dt` between two ARCore poses; the accelerometer is there for
 * gravity/bias observability, not for the path shape, and a few milliseconds of
 * accel staleness moves the gravity direction by nothing measurable. Option (2)
 * buys sub-millisecond accel alignment that no consumer reads, at the cost of a
 * buffer and added latency. Option (3) would double the sample rate with half
 * the samples carrying a repeated gyro reading, which an integrator would
 * happily double-count.
 *
 * So: **the gyro sets the cadence; the accel is a held value.** Each emitted
 * sample carries the GYRO's own timestamp, never a blend of the two — an
 * invented timestamp is precisely what a densifier cannot survive.
 *
 * A gyro sample arriving before any accel has been seen is dropped
 * ([droppedNoAccel]); one arriving with an accel older than [maxAccelAgeNs] is
 * emitted anyway but counted ([staleAccel]) — dropping it would punch a hole in
 * the integration, which is strictly worse than a slightly old gravity vector.
 * Out-of-order gyro samples are dropped, matching what the engine does to
 * out-of-order poses ("rather than silently corrupting every interpolation
 * that follows").
 *
 * Not thread-safe by construction, and it does not need to be: the recorder
 * registers both sensors on ONE `HandlerThread`, so both callbacks and this
 * merger run on that single thread.
 */
class ImuStreamMerger(
    /** How stale a held accel may be before it is counted (not dropped). Default 3 gyro periods at 200 Hz. */
    private val maxAccelAgeNs: Long = 15_000_000L,
) {
    private var ax = 0f
    private var ay = 0f
    private var az = 0f
    private var accelTNs = Long.MIN_VALUE
    private var lastGyroNs = Long.MIN_VALUE

    var gyroEvents: Long = 0L
        private set
    var accelEvents: Long = 0L
        private set
    var emitted: Long = 0L
        private set
    var droppedNoAccel: Long = 0L
        private set
    var droppedOutOfOrder: Long = 0L
        private set
    var staleAccel: Long = 0L
        private set

    fun onAccel(tMonoNs: Long, x: Float, y: Float, z: Float) {
        accelEvents++
        // No monotonic guard here on purpose: a held value has no ordering
        // requirement, and refusing an out-of-order accel would keep an OLDER
        // reading rather than a newer one.
        ax = x
        ay = y
        az = z
        accelTNs = tMonoNs
    }

    /** Returns the sample to push, or null when this gyro event cannot make one. */
    fun onGyro(tMonoNs: Long, x: Float, y: Float, z: Float): ImuSample? {
        gyroEvents++
        if (accelTNs == Long.MIN_VALUE) {
            droppedNoAccel++
            return null
        }
        if (lastGyroNs != Long.MIN_VALUE && tMonoNs <= lastGyroNs) {
            droppedOutOfOrder++
            return null
        }
        lastGyroNs = tMonoNs
        if (abs(tMonoNs - accelTNs) > maxAccelAgeNs) staleAccel++
        emitted++
        return ImuSample(tMonoNs, x, y, z, ax, ay, az)
    }

    fun reset() {
        ax = 0f; ay = 0f; az = 0f
        accelTNs = Long.MIN_VALUE
        lastGyroNs = Long.MIN_VALUE
        gyroEvents = 0L; accelEvents = 0L; emitted = 0L
        droppedNoAccel = 0L; droppedOutOfOrder = 0L; staleAccel = 0L
    }
}

/**
 * The delivered rate, measured rather than assumed.
 *
 * Android treats a requested sampling period as a hint, and silently caps it:
 * without `HIGH_SAMPLING_RATE_SENSORS` the platform clamps delivery to 200 Hz
 * whatever the sensor's `minDelay` says. "Was the requested rate granted?" is
 * therefore a question only the arriving timestamps can answer, and it matters
 * — the densifier's benefit scales with how much of the 33 ms ARCore interval
 * it can actually see.
 *
 * A first-to-last average over the whole session, not a sliding window: this is
 * a session-health number for the log and the diagnostics row, not a control
 * signal.
 */
class ImuRateMeter {
    private var firstNs = Long.MIN_VALUE
    private var lastNs = Long.MIN_VALUE
    private var count = 0L

    fun record(tMonoNs: Long) {
        if (firstNs == Long.MIN_VALUE) firstNs = tMonoNs
        lastNs = tMonoNs
        count++
    }

    /** Samples per second, or 0.0 until there are at least two spanning a real interval. */
    fun hz(): Double {
        if (count < 2 || lastNs <= firstNs) return 0.0
        return (count - 1).toDouble() * 1e9 / (lastNs - firstNs).toDouble()
    }

    fun reset() {
        firstNs = Long.MIN_VALUE
        lastNs = Long.MIN_VALUE
        count = 0L
    }
}

/**
 * **The clock-domain guard**, following the precedent
 * `ArCameraCharacteristicsProbe.probe` set for `SENSOR_INFO_TIMESTAMP_SOURCE`:
 * an assumption the whole pipeline rests on gets checked once, loudly, rather
 * than being trusted forever because it is usually true.
 *
 * The assumption here: `SensorEvent.timestamp` is `CLOCK_BOOTTIME`, the same
 * domain as `Frame.getTimestamp()` and everything the engine calls
 * `t_mono_ns`. The app converts nowhere, so a device that stamps in
 * `CLOCK_MONOTONIC` (which excludes deep sleep) or in some vendor epoch would
 * feed the densifier IMU samples that sit hours away from the poses they are
 * supposed to densify. The platform does NOT guarantee BOOTTIME here — it is
 * a de-facto convention, which is exactly the kind of thing that is wrong on
 * one phone in the field.
 *
 * The check is a comparison against `SystemClock.elapsedRealtimeNanos()` read at
 * DELIVERY: a real BOOTTIME stamp is at most a few milliseconds in the past
 * (sample time to callback), so anything beyond [MAX_SKEW_NS] is a different
 * clock, not a slow phone. On a device that has been awake a while, a
 * `CLOCK_MONOTONIC` stamp differs by the accumulated sleep time and a vendor
 * epoch differs by far more; both trip this comfortably.
 */
object ImuClockDomain {
    /** One second. Delivery latency is milliseconds; a wrong clock is seconds-to-hours. */
    const val MAX_SKEW_NS = 1_000_000_000L

    /** The stream name used in the loud log line, mirroring `StreamId.kPoseAr` in the camera probe. */
    const val STREAM_NAME = "phone-imu"

    fun looksLikeBoottime(sensorTimestampNs: Long, elapsedRealtimeNs: Long): Boolean {
        if (sensorTimestampNs <= 0L) return false
        return abs(elapsedRealtimeNs - sensorTimestampNs) <= MAX_SKEW_NS
    }
}

/**
 * `camera_from_imu` — the rotation taking a vector in the **Android
 * device/sensor frame** into the **ARCore camera frame**, [quat] being that
 * rotation as `(x, y, z, w)`.
 *
 * [derived] is false when it could not be worked out and identity is being
 * pushed instead; [why] then says so, and the caller must log it loudly rather
 * than let a guess pass for a measurement.
 */
data class CameraFromImuExtrinsics(val quat: Quat, val derived: Boolean, val why: String) {
    /** `(x, y, z, w)` in the order `scan_engine_set_imu_extrinsics` takes. */
    fun toXyzw(): DoubleArray = doubleArrayOf(quat.x, quat.y, quat.z, quat.w)
}

/**
 * ROUND 9 item 35 (B3) — **deriving `camera_from_imu` from
 * `CameraCharacteristics.SENSOR_ORIENTATION`.**
 *
 * The two frames are NOT the same, and assuming they are would rotate every
 * integrated increment 90 degrees off — the densified path would bend sideways
 * instead of following the walk. The derivation, written out because a
 * quaternion constant with no derivation next to it is unreviewable:
 *
 * ### The two frames
 *
 *  * **Android device (sensor) frame.** Defined against the device's NATURAL
 *    orientation and never remapped by display rotation: `+X` right along the
 *    screen, `+Y` up along the screen, `+Z` out of the *front* of the screen,
 *    toward the user. `TYPE_GYROSCOPE`/`TYPE_ACCELEROMETER` report in this
 *    frame.
 *  * **ARCore camera frame** (`Frame.getCamera().getPose()`): `+X` right in the
 *    captured IMAGE, `+Y` up in the captured image, `-Z` along the direction
 *    the camera looks. It is image-aligned, not display-aligned — that is
 *    precisely why `getDisplayOrientedPose()` exists as a separate call, and
 *    why the capture path deliberately uses `getPose()` (see
 *    `CaptureArController.publishPose`).
 *
 * ### The shared axis
 *
 * For a REAR camera the optical axis is the device's `-Z`. ARCore's `-Z_cam` is
 * the look direction, so `-Z_cam = -Z_dev`, i.e. **`Z_cam = Z_dev`**. The two
 * frames therefore differ by a pure rotation about their common `Z`, and
 * `SENSOR_ORIENTATION` is the size of it.
 *
 * ### The angle
 *
 * `SENSOR_ORIENTATION` is documented as the CLOCKWISE angle the output image
 * must be rotated through to appear upright on the device in its natural
 * orientation. Take a scene direction that is physically device-`+Y` (up). For
 * the rotated image to look upright, that direction must end up as screen-up
 * AFTER a clockwise rotation by `θ`; so in the RAW image it points along
 * `ccw(θ)` of up — that is, at `(-sin θ, cos θ)` in the image's `(right, up)`
 * axes, which are `(+X_cam, +Y_cam)`.
 *
 * Sanity-check it physically at `θ = 90` (a typical phone rear camera): hold
 * the phone portrait and photograph a standing person. The raw sensor image is
 * landscape and needs a 90-degree clockwise rotation to be upright, so after
 * that rotation the head is at the top — meaning that in the RAW image the head
 * is at the LEFT. Head = device-up. So `device +Y -> -X_cam`, which is what
 * `(-sin 90, cos 90) = (-1, 0)` says.
 *
 * Doing the same for device `+X` gives `(cos θ, sin θ)`, and `Z` is shared, so
 *
 * ```
 * R_cam_from_dev = Rz(+θ) = | cos θ  -sin θ  0 |
 *                           | sin θ   cos θ  0 |
 *                           |   0       0    1 |
 * ```
 *
 * a right-handed rotation of `+SENSOR_ORIENTATION` about the shared `+Z`
 * (which points out of the screen toward the user). As a quaternion:
 * `(0, 0, sin(θ/2), cos(θ/2))`. For the usual `θ = 90` that is
 * `(0, 0, 0.7071, 0.7071)`.
 *
 * ### What is NOT derived
 *
 * **Front-facing cameras.** There the optical axis is `+Z_dev`, so `Z_cam =
 * -Z_dev` and the relationship is no longer a rotation about a shared axis;
 * the mirroring convention the preview applies makes the sign of the remaining
 * term genuinely ambiguous from `SENSOR_ORIENTATION` alone. A front-facing AR
 * session therefore gets IDENTITY **plus a loud warning**, per this item's own
 * instruction — not a guess.
 *
 * **Translation.** This is a rotation only, which is what the C ABI takes
 * (`scan_engine_set_imu_extrinsics(scan_engine*, const double quat_xyzw[4])`).
 * The IMU-to-camera lever arm is a few millimetres inside one phone chassis and
 * contributes a centripetal term the densifier does not model.
 */
object CameraFromImu {

    /** A rear camera at [sensorOrientationDeg]: `Rz(+θ)` about the shared `+Z`. */
    fun rearCamera(sensorOrientationDeg: Int): Quat {
        val half = sensorOrientationDeg * (PI / 180.0) / 2.0
        return Quat(0.0, 0.0, sin(half), cos(half)).normalized()
    }

    /**
     * The extrinsic to push, or identity-with-a-reason when it cannot be
     * derived. [sensorOrientationDeg] is null when the characteristics query
     * failed or the tag was absent.
     */
    fun resolve(sensorOrientationDeg: Int?, frontFacing: Boolean): CameraFromImuExtrinsics {
        if (sensorOrientationDeg == null) {
            return CameraFromImuExtrinsics(
                Quat.IDENTITY,
                derived = false,
                why = "CameraCharacteristics.SENSOR_ORIENTATION unavailable",
            )
        }
        if (frontFacing) {
            return CameraFromImuExtrinsics(
                Quat.IDENTITY,
                derived = false,
                why = "front-facing camera: the optical axis is +Z_device, so camera_from_imu is not a " +
                    "rotation about a shared axis and SENSOR_ORIENTATION alone does not determine it",
            )
        }
        // Anything not a multiple of 90 is not a thing Camera2 defines
        // (SENSOR_ORIENTATION is documented as one of 0/90/180/270); a device
        // reporting otherwise is reporting something this derivation does not
        // cover, so say so instead of rounding it.
        val normalized = ((sensorOrientationDeg % 360) + 360) % 360
        if (normalized % 90 != 0) {
            return CameraFromImuExtrinsics(
                Quat.IDENTITY,
                derived = false,
                why = "SENSOR_ORIENTATION=$sensorOrientationDeg is not one of 0/90/180/270",
            )
        }
        return CameraFromImuExtrinsics(
            rearCamera(normalized),
            derived = true,
            why = "rear camera, SENSOR_ORIENTATION=$normalized deg -> Rz(+$normalized) about the shared +Z",
        )
    }

    // ── ROUND 20 (item 81): the FACTORY calibration ──────────────────────────
    //
    // `CameraCharacteristics.LENS_POSE_ROTATION` is a per-unit, factory-solved
    // 6-DoF camera pose — the real answer to the question the coarse Rz(θ)
    // guess above approximates from SENSOR_ORIENTATION alone. Two conventions
    // stand between it and the densifier's `camera_from_imu`:
    //
    //  1. **The frame flip.** Camera2's camera-aligned frame has +Y down the
    //     image and +Z toward the scene; ARCore's physical camera frame has +Y
    //     up the image and looks along −Z. The two differ by Rx(180 deg),
    //     [CAMERA2_TO_ARCORE] below.
    //  2. **The direction of the quaternion.** The platform documentation's
    //     wording on whether LENS_POSE_ROTATION maps camera→reference or
    //     reference→camera has been read both ways by real vendors, so this
    //     does not gamble: both readings are computed, and the one that agrees
    //     with the SENSOR_ORIENTATION-derived coarse rotation (which IS
    //     trustworthy about the ~90 deg gross layout) within
    //     [MAX_FACTORY_DISAGREEMENT_DEG] wins. The factory value then
    //     contributes exactly what the coarse one cannot: the sub-degree
    //     per-unit deviation from the ideal right angle.
    //
    // If NEITHER reading agrees (a convention this derivation does not cover),
    // or the tag is absent (emulators), the coarse rotation is used and the
    // `why` says so loudly — a fallback, never a silent guess.

    /** Rx(180 deg): Camera2's camera-aligned frame -> ARCore's physical camera frame. */
    val CAMERA2_TO_ARCORE = Quat(1.0, 0.0, 0.0, 0.0)

    /** Past this the factory quaternion is a convention mismatch, not a calibration. */
    const val MAX_FACTORY_DISAGREEMENT_DEG = 30.0

    /**
     * The extrinsic to push, preferring the factory LENS_POSE_ROTATION when it
     * is present AND consistent with the coarse convention. [lensPoseRotationXyzw]
     * is the tag verbatim, `(x, y, z, w)`, or null when the device lacks it.
     */
    fun resolveWithFactory(
        lensPoseRotationXyzw: DoubleArray?,
        sensorOrientationDeg: Int?,
        frontFacing: Boolean,
    ): CameraFromImuExtrinsics {
        val coarse = resolve(sensorOrientationDeg, frontFacing)
        if (frontFacing) return coarse
        val raw = lensPoseRotationXyzw
        if (raw == null || raw.size != 4 || raw.any { !it.isFinite() }) return coarse
        val lens = Quat(raw[0], raw[1], raw[2], raw[3])
        if (lens.norm < 1e-6) return coarse
        val q = lens.normalized()
        // Reading A: the tag maps camera -> reference, so reference -> camera
        // is its conjugate. Reading B: the tag already maps reference -> camera.
        val candidateA = (CAMERA2_TO_ARCORE * q.conjugate()).normalized()
        val candidateB = (CAMERA2_TO_ARCORE * q).normalized()
        if (!coarse.derived) {
            // No SENSOR_ORIENTATION to adjudicate with — take the documented
            // reading (A) and say the assumption out loud.
            return CameraFromImuExtrinsics(
                candidateA,
                derived = true,
                why = "factory LENS_POSE_ROTATION (camera->reference reading, unverified: " +
                    "SENSOR_ORIENTATION unavailable to cross-check)",
            )
        }
        val devA = Math.toDegrees(coarse.quat.angleTo(candidateA))
        val devB = Math.toDegrees(coarse.quat.angleTo(candidateB))
        val pick = when {
            devA <= MAX_FACTORY_DISAGREEMENT_DEG && devA <= devB -> candidateA to devA
            devB <= MAX_FACTORY_DISAGREEMENT_DEG -> candidateB to devB
            else -> null
        }
        if (pick == null) {
            return CameraFromImuExtrinsics(
                coarse.quat,
                derived = coarse.derived,
                why = "factory LENS_POSE_ROTATION disagrees with the SENSOR_ORIENTATION " +
                    "convention by %.1f/%.1f deg under both readings — convention mismatch, ".format(devA, devB) +
                    "falling back to the coarse rotation (${coarse.why})",
            )
        }
        return CameraFromImuExtrinsics(
            pick.first,
            derived = true,
            why = "factory LENS_POSE_ROTATION, %.2f deg from the coarse Rz(%d) — per-unit calibration in force"
                .format(pick.second, ((sensorOrientationDeg ?: 0) % 360 + 360) % 360),
        )
    }
}

/**
 * What the recorder reports about itself — small on purpose, so it can be
 * logged once a second and surfaced in the capture diagnostics row without
 * anything having to interpret it.
 */
data class PhoneImuStatus(
    val running: Boolean = false,
    val gyroPresent: Boolean = false,
    val accelPresent: Boolean = false,
    /** Samples accepted by `scan_engine_push_imu`. */
    val samplesPushed: Long = 0L,
    /** Samples the engine refused (a non-OK `scan_error_t`). */
    val samplesRejected: Long = 0L,
    /** Gyro events the merge could not turn into a sample (no accel yet, or out of order). */
    val samplesDropped: Long = 0L,
    /** Measured delivery rate of the merged stream, Hz. */
    val measuredHz: Double = 0.0,
    /** What was asked of `registerListener`, Hz (0 == SENSOR_DELAY_FASTEST). */
    val requestedHz: Double = 0.0,
    /** The ceiling the sensor's own `minDelay` implies, Hz. */
    val sensorMaxHz: Double = 0.0,
    /** True once enough samples have arrived to say the measured rate is within 20% of the sensor ceiling. */
    val requestedRateGranted: Boolean = false,
    /** False once the clock-domain probe has failed; pushing is then DISABLED. */
    val clockDomainOk: Boolean = true,
    /** Non-null when the recorder is deliberately not pushing, and why. */
    val disabledReason: String? = null,
    /** The `camera_from_imu` actually pushed, and whether it was derived or is a warned-about identity. */
    val extrinsicsDerived: Boolean = false,
    val extrinsicsNote: String = "not set",
) {
    /** One line for the capture log. */
    fun summary(): String = buildString {
        append("phone-imu ")
        append(if (running) "running" else "stopped")
        append(" pushed=").append(samplesPushed)
        append(" rejected=").append(samplesRejected)
        append(" dropped=").append(samplesDropped)
        append(" rate=").append(String.format("%.1f", measuredHz)).append("Hz")
        append("/max=").append(String.format("%.0f", sensorMaxHz)).append("Hz")
        append(" granted=").append(requestedRateGranted)
        append(" clockOk=").append(clockDomainOk)
        if (disabledReason != null) append(" DISABLED=").append(disabledReason)
        append(" extrinsics=").append(if (extrinsicsDerived) "derived" else "IDENTITY")
        append(" (").append(extrinsicsNote).append(")")
    }
}
