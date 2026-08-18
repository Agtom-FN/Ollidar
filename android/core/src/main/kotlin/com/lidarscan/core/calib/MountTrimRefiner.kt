package com.lidarscan.core.calib

import com.lidarscan.core.capture.PoseSample
import com.lidarscan.core.model.SensorType

/**
 * ROUND 11 item 45 — the hold-still ring, and the reason it keeps going after
 * the gate has already said yes.
 *
 * > (a) "Replace trial-and-error (log shows long MOVING-refusal streaks) with a
 * >      hold-still progress ring ('hold steady… set'), same gate underneath."
 * > (c) "the gate currently ACCEPTS spreadP90 up to 2.5° — that is 12 cm at 3 m
 * >      and reverses sides on turn-around. Tighten: after the gate passes,
 * >      keep averaging while the phone stays still to drive spread down."
 *
 * ## What 2.4° costs, measured
 *
 * `engine/tests/test_round11_mount_trim.cpp` walks an out-and-back past a
 * ceiling beam 1.66 m overhead and measures where the two passes paint it:
 *
 * | trim | turn-around split at 1.66 m | scaled to 3 m |
 * | --- | ---: | ---: |
 * | 2.4° (what scan-020's gate accepted) | **13.1 cm** | 23.6 cm |
 * | 1.4° | 9.0 cm | 16.3 cm |
 * | 0.8° | 3.7 cm | 6.6 cm |
 * | 0.5° | 1.0 cm (the fixture's own floor) | 1.8 cm |
 *
 * — so the refinement target is worth about 9 cm of doubled feature at head
 * height and more than that further away. It is the single largest known error
 * left on this rig.
 *
 * ## Why "keep averaging" makes the TRIM better while the SPREAD stays the same
 *
 * This is the part worth being careful about, because the obvious reading of
 * item 45c is wrong. `spreadP90Deg` measures the JITTER of individual ARCore
 * frames about their mean. Holding still for four seconds instead of one does
 * not reduce that jitter — it is a property of the tracker and the operator's
 * hands — so a UI that promised "spread falling to 0.8°" would be promising
 * something that does not happen.
 *
 * What DOES improve is the accuracy of the MEAN, which is the thing actually
 * stored as the trim. And rather than model that (an s.e.m. argument needs the
 * samples to be independent, and ARCore's are visibly not — they drift together
 * over a second), this measures it: split the hold in half, average each half
 * separately, and report the angle between the two answers.
 *
 * That number — [Progress.stabilityDeg] — is an empirical answer to "if I took
 * this trim twice, how much would the two disagree", it needs no noise model,
 * it accounts for correlated drift automatically, and it is conservative (two
 * half-length means disagree by about √2 times the error of the full-length
 * one). It is what the ring counts down and what the operator is shown.
 *
 * ## Why the ring, and not another refusal
 *
 * The owner's 0.4.0 log has seven MOVING refusals in forty-four seconds, and
 * ROUND 8's answer was to make each refusal explain itself. That helped and it
 * is still not the right shape: a refusal is a verdict on a moment the operator
 * has already finished, so all they can do is guess and tap again. A ring is
 * the same gate sampled continuously — it fills while the hold is good, empties
 * the instant it is not, and the operator learns what "still" means with their
 * hands rather than by reading.
 *
 * Pure `:core`. The gate underneath is [MountTrimSampler] unchanged.
 */
class MountTrimRefiner(
    /** The refinement target from item 45c. */
    val targetStabilityDeg: Double = DEFAULT_TARGET_STABILITY_DEG,
    /** The longest hold the ring will ask for before settling for what it has. */
    val maxHoldMillis: Long = DEFAULT_MAX_HOLD_MS,
    /** The shortest hold the ROUND 8 gate itself needs. */
    val minHoldMillis: Long = MountTrim.WINDOW_MS,
) {
    /**
     * What the ring shows on one tick.
     *
     * [gatePasses] is the ROUND 8 gate evaluated live over the last
     * [MountTrim.WINDOW_MS], i.e. exactly the answer a tap would have got. It is
     * what makes the hold-still UI honest: the ring is not a decorative timer,
     * it is the gate being asked thirty times a second.
     */
    data class Progress(
        val holdMillis: Long,
        val samples: Int,
        val spreadP90Deg: Double,
        val spreadMaxDeg: Double,
        /** Split-half agreement of the mean over the whole hold; < 0 until measurable. */
        val stabilityDeg: Double,
        val gatePasses: Boolean,
        val refined: Boolean,
        val done: Boolean,
        /** 0..1 for the ring sweep. */
        val fraction: Float,
        val label: String,
    ) {
        /** For the capture log — the same `key=value` shape as everything else here. */
        val logSuffix: String
            get() = "holdMs=%d samples=%d p90=%.2fdeg stability=%.2fdeg gate=%b refined=%b".format(
                holdMillis,
                samples,
                spreadP90Deg,
                if (stabilityDeg < 0.0) Double.NaN else stabilityDeg,
                gatePasses,
                refined,
            )
    }

    /**
     * Evaluate the current hold. `samples` is the controller's pose ring,
     * newest last; `holdStartedAtMonoNs` is when the operator's finger went
     * down (or when the auto-refresh began), so that a hold interrupted by
     * movement can be restarted by the caller resetting it.
     */
    fun evaluate(
        samples: List<PoseSample>,
        holdStartedAtMonoNs: Long,
    ): Progress {
        if (samples.isEmpty()) {
            return Progress(
                holdMillis = 0L,
                samples = 0,
                spreadP90Deg = 0.0,
                spreadMaxDeg = 0.0,
                stabilityDeg = -1.0,
                gatePasses = false,
                refined = false,
                done = false,
                fraction = 0f,
                label = "Waiting for tracking…",
            )
        }

        val newest = samples.maxOf { it.tMonoNs }
        val hold = samples.filter { it.tMonoNs >= holdStartedAtMonoNs }
        val holdMillis = if (hold.isEmpty()) 0L else (newest - holdStartedAtMonoNs) / 1_000_000L

        // The live gate, over the SAME window a tap would have used, so the ring
        // and the tap can never disagree.
        val gate = MountTrimSampler.capture(samples, nowMillis = 0L)
        val gateMeasurement = (gate as? MountTrimResult.Rejected)?.measurement
        val gatePasses = gate is MountTrimResult.Captured

        val liveWindow = samples.filter {
            it.tMonoNs >= newest - MountTrim.WINDOW_MS * 1_000_000L
        }
        val p90: Double
        val worst: Double
        if (gatePasses && liveWindow.size >= MountTrim.MIN_SAMPLES) {
            val mean = MountTrimSampler.meanOrientation(liveWindow.map { it.orientation })
            val devs = liveWindow.map { Math.toDegrees(mean.angleTo(it.orientation)) }
            p90 = MountTrimSampler.percentile(devs, MountTrim.SPREAD_PERCENTILE)
            worst = devs.max()
        } else {
            p90 = gateMeasurement?.spreadP90Deg ?: 0.0
            worst = gateMeasurement?.spreadMaxDeg ?: 0.0
        }

        val stability = splitHalfStabilityDeg(hold)
        val refined = stability in 0.0..targetStabilityDeg
        val longEnough = holdMillis >= minHoldMillis
        val timedOut = holdMillis >= maxHoldMillis
        val done = gatePasses && longEnough && (refined || timedOut)

        // The ring fills over the whole refinement budget, not over the gate's
        // one second: filling to 100 % in a second and then continuing to ask
        // for a hold would be a lie about what the app wants.
        val fraction = (holdMillis.toFloat() / maxHoldMillis.toFloat()).coerceIn(0f, 1f)

        val label = when {
            !gatePasses && holdMillis < 300L -> "Hold steady…"
            !gatePasses -> "Too much movement — brace the phone"
            done && refined -> "Set — %.1f°".format(stability)
            done -> "Set — %.1f° (as good as it got)".format(if (stability < 0) p90 else stability)
            stability >= 0.0 -> "Improving… %.1f°".format(stability)
            else -> "Hold steady…"
        }

        return Progress(
            holdMillis = holdMillis,
            samples = hold.size,
            spreadP90Deg = p90,
            spreadMaxDeg = worst,
            stabilityDeg = stability,
            gatePasses = gatePasses,
            refined = refined,
            done = done,
            fraction = fraction,
            label = label,
        )
    }

    /**
     * Take the trim over the WHOLE hold rather than the gate's last second.
     *
     * The gate still decides (it is [MountTrimSampler.capture] over
     * [MountTrim.WINDOW_MS], unchanged, and a moving rig is refused exactly as
     * before) — but once it has said yes, the stored orientation is the mean of
     * every frame of the hold, which is the whole point of holding longer.
     * `spreadP90Deg` is recomputed over that longer set so the persisted trim
     * and the log report what was actually averaged.
     */
    fun capture(
        samples: List<PoseSample>,
        holdStartedAtMonoNs: Long,
        nowMillis: Long,
        sensor: SensorType = SensorType.COIN_D6,
    ): MountTrimResult {
        val gate = MountTrimSampler.capture(samples, nowMillis, sensor)
        if (gate !is MountTrimResult.Captured) return gate

        val hold = samples.filter { it.tMonoNs >= holdStartedAtMonoNs && it.tracking }
        if (hold.size < MountTrim.MIN_SAMPLES) return gate

        val mean = MountTrimSampler.meanOrientation(hold.map { it.orientation })
        val devs = hold.map { Math.toDegrees(mean.angleTo(it.orientation)) }
        return MountTrimResult.Captured(
            MountTrim.fromHoldOrientation(
                hold = mean,
                sensor = sensor,
                capturedAtEpochMillis = nowMillis,
                sampleCount = hold.size,
                spreadDeg = devs.max(),
                spreadP90Deg = MountTrimSampler.percentile(devs, MountTrim.SPREAD_PERCENTILE),
            ),
        )
    }

    /**
     * The angle between the mean of the first half of the hold and the mean of
     * the second half, in degrees. Negative when there are not yet enough
     * samples on both sides to have two means worth comparing.
     */
    fun splitHalfStabilityDeg(hold: List<PoseSample>): Double {
        val tracked = hold.filter { it.tracking }
        if (tracked.size < 2 * MountTrim.MIN_SAMPLES) return -1.0
        val ordered = tracked.sortedBy { it.tMonoNs }
        val half = ordered.size / 2
        val a = MountTrimSampler.meanOrientation(ordered.take(half).map { it.orientation })
        val b = MountTrimSampler.meanOrientation(ordered.drop(half).map { it.orientation })
        return Math.toDegrees(a.angleTo(b))
    }

    companion object {
        const val DEFAULT_TARGET_STABILITY_DEG = 0.8
        const val DEFAULT_MAX_HOLD_MS = 8_000L

        /**
         * How stale a trim may be before capture start re-takes it by itself
         * (item 45b). Twelve hours was ROUND 8's "this came from a previous
         * run" threshold; this is far shorter, because a bracket that has been
         * picked up and put down between two scans in the same session has
         * moved and the app has no way to know it.
         */
        const val AUTO_REFRESH_AFTER_MS = 10 * 60_000L
    }
}
