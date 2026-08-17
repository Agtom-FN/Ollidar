package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import kotlinx.serialization.Serializable
import kotlin.math.abs

/**
 * ROUND 6, owner item 23 — **the one-tap mount re-zero.**
 *
 * > "allow to set a starting imu position as calibrated position for d6 since
 * > the d6 use the phone as the imu and the mounting of d6 on the phone maybe
 * > adjusted each time"
 *
 * The COIN-D6 has no IMU (ROUND 5 item 11): the phone's ARCore VIO *is* the
 * trajectory, and A8's pushbroom resolves every return as
 * `world_from_lidar = world_from_phone(t) · phone_from_lidar`. `phone_from_lidar`
 * is [BracketNominals.cadNominal] until the calibration wizard has been run —
 * and that nominal assumes a *fixed* bracket. The owner's rig is clamped on by
 * hand and comes off between scans, so the real `phone_from_lidar` differs from
 * the nominal by an unknown rotation every session. That rotation goes straight
 * into every resolved point.
 *
 * ## The measurement, and exactly what it can and cannot recover
 *
 * A phone IMU cannot see how a lidar is bolted to its back. What it CAN see is
 * the **phone's own gravity-aligned attitude**, and that is enough if the
 * operator does the one thing the affordance asks: hold the rig in its intended
 * scanning pose and tap. Then:
 *
 * ```
 *   W  world (ARCore, gravity-aligned, defined at session start)
 *   P  phone
 *   L  lidar
 *
 *   nominal, by construction:  W_R_P = q_ref  and  P_R_L = N   (the CAD nominal)
 *   actual, same physical pose: W_R_P = q_hold (what ARCore reports)
 *
 *   the LIDAR's world attitude is the same in both (that is what "same physical
 *   pose" means), so:
 *
 *       q_ref · N  =  q_hold · (P_R_L)_actual
 *   =>  (P_R_L)_actual  =  q_hold⁻¹ · q_ref · N
 *   =>  trim  =  q_hold⁻¹ · q_ref
 * ```
 *
 * With [REFERENCE_HOLD] the identity — ARCore's world frame is defined by the
 * phone's attitude at session start, so "the pose the nominal assumes" *is*
 * identity — the trim collapses to `q_hold⁻¹`: literally "zero out the attitude
 * you are holding right now", which is the owner's own words for it.
 *
 * **Rotation only.** A re-clamp changes orientation; it also changes the lever
 * arm by a few millimetres, and there is nothing in this measurement that can
 * observe that. [composedWith] therefore keeps the nominal's translation
 * verbatim rather than inventing a number — the mount-calibration wizard remains
 * the way to recover translation.
 *
 * ## Rejection
 *
 * A trim captured while the rig was moving is worse than no trim, so
 * [MountTrimSampler] refuses one: the samples must span at least
 * [MIN_SAMPLE_SPAN_MS], the spread of orientations across the window must stay
 * under [MAX_SPREAD_DEG], and ARCore must have been tracking throughout.
 */
@Serializable
data class MountTrim(
    /** The trim rotation, `(x, y, z, w)` — the engine's and ARCore's shared order. */
    val qx: Double,
    val qy: Double,
    val qz: Double,
    val qw: Double,
    /** Which sensor's nominal this trim is composed onto. A Mid-360 trim is not a D6 trim. */
    val sensor: SensorType = SensorType.COIN_D6,
    val capturedAtEpochMillis: Long = 0L,
    /** How many pose samples the average was taken over. */
    val sampleCount: Int = 0,
    /** Worst orientation deviation from the mean across the hold window, degrees — the "was it still?" evidence. */
    val spreadDeg: Double = 0.0,
) {
    val rotation: Quat get() = Quat(qx, qy, qz, qw).normalized()

    /** How far this trim tilts the mount away from the nominal, in degrees. 0 = the rig was held exactly nominal. */
    val magnitudeDeg: Double get() = Math.toDegrees(Quat.IDENTITY.angleTo(rotation))

    /**
     * `phone_from_lidar` for this session: the trim rotation applied to
     * [nominal]'s rotation block, [nominal]'s translation kept.
     *
     * Row-major throughout, because that is the only layout
     * `scan_engine_set_mount_extrinsics` accepts (see [Mat4]).
     */
    fun composedWith(nominal: Mat4): Mat4 {
        val trimmedRotation = Mat4.fromRotationTranslation(rotation, Vec3.ZERO) * nominal
        val m = trimmedRotation.m.copyOf()
        // Translation is the nominal's — a re-zero measures attitude, not lever arm.
        val t = nominal.translation
        m[3] = t.x
        m[7] = t.y
        m[11] = t.z
        return Mat4(m)
    }

    /** Age in milliseconds at [nowMillis]; a trim from a previous session is worth re-taking. */
    fun ageMillis(nowMillis: Long): Long = (nowMillis - capturedAtEpochMillis).coerceAtLeast(0L)

    /** "2 min ago" / "just now" — the capture panel shows this beside the re-zero affordance. */
    fun ageLabel(nowMillis: Long): String {
        val seconds = ageMillis(nowMillis) / 1000
        return when {
            seconds < 20 -> "just now"
            seconds < 90 -> "${seconds}s ago"
            seconds < 3600 -> "${seconds / 60} min ago"
            seconds < 86_400 -> "${seconds / 3600} h ago"
            else -> "${seconds / 86_400} d ago"
        }
    }

    companion object {
        /**
         * The attitude the CAD nominal assumes the phone is in. Identity: ARCore
         * defines its world frame from the device's attitude at session start,
         * so an operator who sets the reference at the start of the walk with
         * the rig held the way it will be carried IS holding it at identity by
         * construction. Named rather than inlined so the assumption is a thing
         * that can be pointed at (and, if a future bracket wants a different
         * hold, changed in one place).
         */
        val REFERENCE_HOLD: Quat = Quat.IDENTITY

        /** Shortest hold that can be averaged; ~1 s at any ARCore frame rate. */
        const val MIN_SAMPLE_SPAN_MS = 800L

        /** Longest window averaged. Older samples are irrelevant to "how am I holding it now". */
        const val WINDOW_MS = 1_200L

        /** Minimum samples in the window — a two-frame "average" is not one. */
        const val MIN_SAMPLES = 8

        /** Above this spread across the hold window the rig was not still. */
        const val MAX_SPREAD_DEG = 1.5

        /**
         * The trim for a rig whose phone attitude reads [hold] while it is held
         * in its scanning pose. See the class header for the derivation.
         */
        fun fromHoldOrientation(
            hold: Quat,
            sensor: SensorType = SensorType.COIN_D6,
            capturedAtEpochMillis: Long = 0L,
            sampleCount: Int = 0,
            spreadDeg: Double = 0.0,
        ): MountTrim {
            val q = (hold.normalized().conjugate() * REFERENCE_HOLD).normalized()
            return MountTrim(
                qx = q.x, qy = q.y, qz = q.z, qw = q.w,
                sensor = sensor,
                capturedAtEpochMillis = capturedAtEpochMillis,
                sampleCount = sampleCount,
                spreadDeg = spreadDeg,
            )
        }
    }
}

/** Why a re-zero attempt did not produce a trim. Each case is a sentence the panel can show verbatim. */
enum class MountTrimRejection(val message: String) {
    NO_POSES("No phone tracking yet — point the camera at something with detail and try again."),
    NOT_ENOUGH_SAMPLES("Hold the rig still for a full second, then tap again."),
    MOVING("The rig moved while measuring — hold it steady in the scanning pose and tap again."),
    NOT_TRACKING("Tracking was lost mid-measurement — wait for TRACKING, then tap again."),
}

sealed interface MountTrimResult {
    data class Captured(val trim: MountTrim) : MountTrimResult
    data class Rejected(val reason: MountTrimRejection) : MountTrimResult
}

/**
 * Averages a short window of pose orientations into a [MountTrim], or refuses.
 *
 * Pure `:core`, no ARCore types: `:app`'s controller feeds it
 * [com.lidarscan.core.capture.PoseSample]s straight off the pose stream, and
 * this is therefore unit-testable on a bare JVM — which matters, because the
 * arithmetic below is the whole of item 23 and no ARCore device exists here to
 * check it on.
 *
 * The average is a **chordal mean** (component-wise sum with sign alignment,
 * then normalise). For a set of orientations within a degree or two of each
 * other — which the rejection gates guarantee — it is indistinguishable from
 * the Riemannian mean and does not need an iteration that could fail to
 * converge on a phone.
 */
object MountTrimSampler {

    fun capture(
        samples: List<com.lidarscan.core.capture.PoseSample>,
        nowMillis: Long,
        sensor: SensorType = SensorType.COIN_D6,
        windowMs: Long = MountTrim.WINDOW_MS,
    ): MountTrimResult {
        if (samples.isEmpty()) return MountTrimResult.Rejected(MountTrimRejection.NO_POSES)
        val newest = samples.maxOf { it.tMonoNs }
        val window = samples.filter { it.tMonoNs >= newest - windowMs * 1_000_000L }
        if (window.size < MountTrim.MIN_SAMPLES) {
            return MountTrimResult.Rejected(MountTrimRejection.NOT_ENOUGH_SAMPLES)
        }
        if (window.any { !it.tracking }) return MountTrimResult.Rejected(MountTrimRejection.NOT_TRACKING)
        val spanMs = (window.maxOf { it.tMonoNs } - window.minOf { it.tMonoNs }) / 1_000_000L
        if (spanMs < MountTrim.MIN_SAMPLE_SPAN_MS) {
            return MountTrimResult.Rejected(MountTrimRejection.NOT_ENOUGH_SAMPLES)
        }

        val mean = meanOrientation(window.map { it.orientation })
        val spreadDeg = window.maxOf { Math.toDegrees(mean.angleTo(it.orientation)) }
        if (spreadDeg > MountTrim.MAX_SPREAD_DEG) {
            return MountTrimResult.Rejected(MountTrimRejection.MOVING)
        }

        return MountTrimResult.Captured(
            MountTrim.fromHoldOrientation(
                hold = mean,
                sensor = sensor,
                capturedAtEpochMillis = nowMillis,
                sampleCount = window.size,
                spreadDeg = spreadDeg,
            ),
        )
    }

    /**
     * Chordal mean of unit quaternions. Signs are aligned against the first
     * sample before summing — `q` and `-q` are the same rotation and ARCore
     * hands out either, so an unaligned sum of a still rig can cancel to zero
     * (the same double-cover hazard A8 §3.4 documents for SLERP).
     */
    fun meanOrientation(orientations: List<Quat>): Quat {
        require(orientations.isNotEmpty()) { "no orientations to average" }
        val first = orientations.first().normalized()
        var x = 0.0
        var y = 0.0
        var z = 0.0
        var w = 0.0
        for (raw in orientations) {
            val q = raw.normalized()
            val dot = q.x * first.x + q.y * first.y + q.z * first.z + q.w * first.w
            val s = if (dot < 0.0) -1.0 else 1.0
            x += s * q.x; y += s * q.y; z += s * q.z; w += s * q.w
        }
        val n = Quat(x, y, z, w)
        return if (abs(n.norm) < 1e-12) first else n.normalized()
    }
}
