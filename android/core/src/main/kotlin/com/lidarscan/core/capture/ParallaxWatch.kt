package com.lidarscan.core.capture

import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.min
import kotlin.math.sqrt

/**
 * ROUND 14 — **the owner's headline complaint, and it is not about speed.**
 *
 * > *"The new scan is much better when i go around … but its not good with
 * > tilting and moving around the phone."*
 *
 * ## What sweeping does NOT cost
 *
 * This has to come first, because it is what the round's measurements actually
 * found and it rules out every fix that would have been easier to write.
 * Comparing the owner's walking capture (scan-033, 26.6 m, the best he has
 * taken) against his two from-the-spot sweeps (scan-034, scan-035):
 *
 * | | scan-033 walk | scan-034 sweep | scan-035 sweep |
 * | --- | ---: | ---: | ---: |
 * | map self-consistency @ 8 s | 1.97 cm | 2.45 cm | 1.74 cm |
 * | (its own measurement floor) | 0.99 cm | 1.03 cm | 1.40 cm |
 * | impossible-elevation fraction | 0.00 % | 0.00 % | 0.00 % |
 * | IMU densifier fallback rate | 31.3 % | 31.9 % | 31.8 % |
 * | resolved points per second | 1,985 | 2,001 | 2,006 |
 *
 * The local geometry of a sweep is as good as the local geometry of a walk.
 * And ARCore's ORIENTATION is not the problem either: cross-correlated against
 * the recorded 400 Hz gyro the best-fit lag is −5 ms (i.e. none), and the
 * per-frame disagreement stays tiny right up the rate range —
 *
 * | gyro rate | ARCore-vs-gyro p90, per 33 ms frame | at 3 m range |
 * | --- | ---: | ---: |
 * | 0-10 °/s | 0.031° | 0.16 cm |
 * | 20-40 °/s | 0.075° | 0.39 cm |
 * | 60-90 °/s | 0.190° | 1.00 cm |
 * | 90-150 °/s | 0.221° | 1.16 cm |
 *
 * — so there is no orientation-fidelity ceiling to warn the operator about
 * below 150 °/s, and "sweep slower" is not the advice. Nor is coverage: at the
 * D6's 10 Hz revolution the owner's median sweep advances the fan 0.92-1.03°
 * between revolutions against a 0.90° within-fan pitch, which is as close to
 * isotropic sampling as this sensor gets.
 *
 * ## What it does cost
 *
 * **Parallax.** Over 1 s windows:
 *
 * | | translation per degree turned | seconds spent turning >20°/s with <0.5 cm/° |
 * | --- | ---: | ---: |
 * | scan-033 (walk) | **2.43 cm/°** | 1.8 % |
 * | scan-034 (sweep) | **0.53 cm/°** | 10.5 % |
 * | scan-035 (sweep) | **0.56 cm/°** | 5.9 % |
 *
 * A monocular visual-inertial tracker recovers depth from translation. Rotate
 * a camera on the spot and every bearing changes while no baseline is created:
 * no new feature can be triangulated, and the tracker has nothing to do but
 * lean on the map it already built. Leaning on the map is how a relocalisation
 * happens — and scan-035's, 22.5 s in, moved the pose **1.631 m / 162.57° in
 * 33 ms** while the phone's own gyro integrated **1.56°** over the same 33 ms.
 * A 104× disagreement. The phone did not move; the world did.
 *
 * (ROUND 14 also removed the reason a 162° hypothesis was available at all —
 * the ARCore session used to survive from capture to capture carrying its
 * feature map with it. See `CaptureArController.resetWorldFrame`. This watch is
 * the other half: not letting the operator walk into the degenerate regime in
 * the first place.)
 *
 * ## Why this is not the existing TOO_FAST cue
 *
 * [CueKind.TOO_FAST] keys on LINEAR speed. In scan-034 the owner's median
 * linear speed was 4 cm/s — the too-fast cue is structurally incapable of
 * firing during exactly the captures that went wrong. Adding an angular
 * threshold alone would be no better: rotation is not the fault, rotation
 * *without* translation is, so the quantity has to be the ratio.
 *
 * ## The threshold, and how it was chosen
 *
 * Swept over the owner's three captures with a 2 s rolling window, counting the
 * fraction of windows that would fire:
 *
 * | rotation floor | cm per degree | scan-033 (walk, GOOD) | scan-034 | scan-035 |
 * | ---: | ---: | ---: | ---: | ---: |
 * | 20° | 0.5 | 1.8 % | 32.2 % | 9.6 % |
 * | **20°** | **0.8** | **5.5 %** | **46.7 %** | **39.7 %** |
 * | 30° | 0.8 | 4.6 % | 30.3 % | 34.1 % |
 * | 40° | 1.0 | 6.2 % | 16.1 % | 35.4 % |
 *
 * 20°/0.8 is taken: it separates the good capture from the two bad ones by
 * roughly 8×, and it is the only row that fires on BOTH sweeps at a comparable
 * rate — a rule that catches scan-034 and misses scan-035 would be fitted to
 * the snap rather than to the technique. The floor of 20° over 2 s (10 °/s
 * sustained) is what stops a stationary phone on a bench from qualifying:
 * with no rotation there is no parallax to be starved of.
 *
 * Pure `:core`, no Android, no allocation per tick beyond the window scan, so
 * the rule can be asserted rather than eyeballed.
 */
class ParallaxWatch(
    /** Rolling window. Two seconds: long enough to be a technique, short enough to act on. */
    private val windowMillis: Long = 2_000L,
    /** Below this much rotation across the window, there is nothing to be starved of. */
    private val minRotationDeg: Double = MIN_ROTATION_DEG,
    /** Centimetres of travel per degree turned, below which the tracker is running blind. */
    private val minCmPerDegree: Double = MIN_CM_PER_DEGREE,
) {
    /**
     * True when [samples] — oldest first, as
     * `CaptureArController.poseWindow()` delivers them — show rotation without
     * the translation that would let a camera make sense of it.
     *
     * Samples with `tracking = false` are dropped rather than bridged: a pose
     * the tracker did not stand behind cannot be evidence about how well the
     * tracker is doing. A step or turn larger than any person can make in one
     * frame is dropped too, for the same reason [TrackingWarmup] drops it — it
     * is a re-anchor, and folding a 1.6 m teleport into the translation total
     * would report excellent parallax at the exact moment there is none.
     */
    fun isStarved(samples: List<PoseSample>, nowMonoNs: Long): Boolean =
        measure(samples, nowMonoNs)?.starved == true

    /**
     * The window ending at the newest sample.
     *
     * Taking "now" from the samples rather than from a system clock is
     * deliberate: `PoseSample.tMonoNs` is stamped in the ENGINE's clock domain,
     * and the ViewModel's wall/uptime clocks are not that domain. Comparing the
     * two would work on the phone and silently mis-window a replay.
     */
    fun measure(samples: List<PoseSample>): Reading? =
        measure(samples, samples.lastOrNull()?.tMonoNs ?: return null)

    /** As [measure], with the window's end supplied. */
    fun measure(samples: List<PoseSample>, nowMonoNs: Long): Reading? {
        if (samples.size < MIN_SAMPLES) return null
        val cutoff = nowMonoNs - windowMillis * 1_000_000L
        var rotationDeg = 0.0
        var travelM = 0.0
        var used = 0
        var previous: PoseSample? = null
        for (s in samples) {
            if (s.tMonoNs < cutoff) continue
            if (!s.tracking) {
                // A gap in tracking breaks the chain: the next good sample is
                // not one frame away from this one.
                previous = null
                continue
            }
            val p = previous
            previous = s
            if (p == null) continue
            val dt = (s.tMonoNs - p.tMonoNs) * 1e-9
            if (dt <= 0.0 || dt > MAX_FRAME_GAP_S) continue
            val step = distance(p, s)
            val turn = angleDeg(p, s)
            if (step > MAX_STEP_M || turn > MAX_TURN_DEG) continue
            rotationDeg += turn
            travelM += step
            used++
        }
        if (used < MIN_SAMPLES) return null
        val cmPerDeg = if (rotationDeg > 0.0) travelM * 100.0 / rotationDeg else Double.MAX_VALUE
        return Reading(
            rotationDeg = rotationDeg,
            travelCm = travelM * 100.0,
            cmPerDegree = cmPerDeg,
            starved = rotationDeg >= minRotationDeg && cmPerDeg < minCmPerDegree,
        )
    }

    data class Reading(
        val rotationDeg: Double,
        val travelCm: Double,
        val cmPerDegree: Double,
        val starved: Boolean,
    )

    private fun distance(a: PoseSample, b: PoseSample): Double {
        val dx = b.position.x - a.position.x
        val dy = b.position.y - a.position.y
        val dz = b.position.z - a.position.z
        return sqrt(dx * dx + dy * dy + dz * dz)
    }

    private fun angleDeg(a: PoseSample, b: PoseSample): Double {
        // |dot| of two unit quaternions is cos(theta/2); the absolute value
        // folds q and -q together, which are the same rotation.
        val q1 = a.orientation
        val q2 = b.orientation
        val dot = abs(q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w)
        return 2.0 * Math.toDegrees(acos(min(1.0, dot)))
    }

    companion object {
        const val MIN_ROTATION_DEG = 20.0
        const val MIN_CM_PER_DEGREE = 0.8

        /** Fewer than this many usable frames in the window and there is no measurement. */
        const val MIN_SAMPLES = 20

        /** Above this, two samples are not consecutive frames of one motion. */
        const val MAX_FRAME_GAP_S = 0.2

        /**
         * A re-anchor, not a movement — the same reasoning
         * [PoseSectionTracker] uses, and the same reason its thresholds exist.
         */
        const val MAX_STEP_M = 0.5
        const val MAX_TURN_DEG = 20.0

        /** The one sentence the capture screen shows while this is true. */
        const val HINT: String =
            "Turning on the spot — take a few steps as you sweep. The camera judges distance " +
                "from movement, and with none it can lose the room."
    }
}
