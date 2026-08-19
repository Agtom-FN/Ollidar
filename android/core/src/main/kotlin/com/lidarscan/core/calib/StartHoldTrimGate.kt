package com.lidarscan.core.calib

/**
 * ROUND 22 item 92 — **the start-hold trim gate accepted anything.**
 *
 * ROUND 20 put a mount measurement at the head of every Start: hold still for a
 * second or two and the app measures the bracket's rotation *in this scan's own
 * frame*, which is the right place to measure it and the reason no calibration
 * box is needed (item 86). What ROUND 20 did not do is ask whether the
 * measurement it got was any good before applying it.
 *
 * `CaptureViewModel.runStartHoldStage` took the first `MountTrimResult.Captured`
 * the refiner produced and applied it — unconditionally. Four hundred lines
 * away, the **auto-refresh** path has had exactly the comparison this needed
 * since ROUND 12 (`if (result.trim.qualityRank > trim.qualityRank) … keep the
 * incumbent`), written after the owner's scan-026/028 A/B proved that two trims
 * measured over different hold lengths are not comparable on `spreadP90` alone.
 * The start path never got it.
 *
 * The cost is in the owner's 2026-08-20 log: a start-hold trim with a
 * **3.18°** split-half accuracy silently replaced a persisted one measured at
 * **0.29°**. Eleven times worse, applied without a word, at the head of a scan.
 *
 * ## Two verdicts, and the second one is the interesting one
 *
 * **[StartHoldVerdict.REFUSE_WORSE]** is the ROUND 12 comparison, on
 * [MountTrim.qualityRank] (split-half accuracy where it exists, an
 * `UNMEASURED_RANK_BASE` penalty where it does not — so "measured beats
 * unverifiable" sorts correctly and two incomparable p90s are never compared).
 * A candidate must be worse by a real margin, not by float noise, to be
 * refused: [MATERIAL_MARGIN_DEG].
 *
 * **[StartHoldVerdict.REFUSE_DRIFT]** comes out of the same log line and is not
 * about the mount at all. That 3.18° candidate had a `spreadP90` of **0.20°**.
 * Those two numbers measure different things over the same samples:
 *
 *  * `spreadP90Deg` is **dispersion** — how far each sample sits from the mean
 *    of the window. 0.20° over a two-second hold is a genuinely still phone.
 *  * `stabilityDeg` is **split-half repeatability** — the first half of the
 *    window against the second half. 3.18° means the two halves disagree by
 *    over three degrees.
 *
 * A hold that is tight about its own mean and yet whose halves disagree by
 * sixteen times that tightness has not been noisy: it has **moved
 * monotonically**. The phone was still — the owner was holding it still — so
 * what drifted was the pose estimate. That is a tracking fault, and applying
 * its output as a mount extrinsic bakes a tracking fault into the geometry of
 * the whole scan.
 *
 * So it is refused as a trim and reported as what it is: "tracking is drifting
 * — hold on". The operator's correct response is to keep holding (the drift
 * settles as ARCore's map matures), which is exactly what the start panel now
 * asks for, and it is a completely different instruction from "hold stiller".
 *
 * Pure `:core` with no Android and no ViewModel, so the arithmetic that decides
 * this is testable against the owner's four real numbers on a bare JVM.
 */
enum class StartHoldVerdict {
    /** Better than (or not materially worse than) the incumbent, and not drifting. */
    ACCEPT,

    /** Materially worse than the persisted trim on [MountTrim.qualityRank]. */
    REFUSE_WORSE,

    /** Dispersion far below split-half disagreement: the POSE moved, not the phone. */
    REFUSE_DRIFT,
}

object StartHoldTrimGate {

    /**
     * How much worse than the incumbent a candidate must be before it is
     * refused, in degrees of [MountTrim.qualityRank].
     *
     * 0.10° is chosen against the numbers this gate exists for, not as a round
     * number: the owner's refusal is 3.18° against 0.29°, i.e. 2.89° clear of
     * the margin, and his ROUND 18 case (0.78° incumbent against an unverifiable
     * one-second sample, rank 100.55) is clear by two orders of magnitude. What
     * the margin buys is the other direction — a 0.29° incumbent must not be
     * kept over a 0.31° candidate on the strength of 0.02°, because at that
     * distance the two measurements are the same measurement and the FRESHER
     * one is taken in this scan's own frame, which is the whole point of the
     * start hold.
     */
    const val MATERIAL_MARGIN_DEG = 0.10

    /**
     * How many times larger than the window's own dispersion the split-half
     * disagreement must be before the hold is read as drift rather than noise.
     *
     * For pure zero-mean noise the two statistics are the same order of
     * magnitude — the split-half difference of a stationary window scales with
     * its dispersion. A factor of four is already well outside that; the owner's
     * case is **fifteen point nine** (3.18 / 0.20), so this fires on his data
     * with a wide margin and does not fire on an ordinary noisy hold.
     */
    const val DRIFT_RATIO = 4.0

    /**
     * Below this split-half disagreement the drift test does not run at all.
     *
     * A hold with `spreadP90 = 0.01°` and `stabilityDeg = 0.05°` satisfies the
     * ratio and is an *excellent* trim — five hundredths of a degree is far
     * inside the 0.8° goal (item 86) and nothing about it is worth refusing.
     * The drift verdict is about holds that are bad AND bad in a specific,
     * diagnosable way; 1.0° is the ROUND 18 line past which a captured trim
     * already warns at acceptance.
     */
    const val DRIFT_MIN_STABILITY_DEG = 1.0

    /**
     * True when this window's shape says the POSE moved monotonically while the
     * phone was held still. See the class doc for the owner's 0.20 / 3.18.
     *
     * Requires a measured [stabilityDeg] (a negative value means "not measured"
     * — see [MountTrim.stabilityDeg]) and a positive [spreadP90Deg], because a
     * zero dispersion makes the ratio meaningless rather than infinite.
     */
    fun trackingIsDrifting(spreadP90Deg: Double, stabilityDeg: Double): Boolean =
        stabilityDeg >= DRIFT_MIN_STABILITY_DEG &&
            spreadP90Deg > 0.0 &&
            stabilityDeg >= DRIFT_RATIO * spreadP90Deg

    /** [trackingIsDrifting] read off a whole trim. */
    fun trackingIsDrifting(candidate: MountTrim): Boolean =
        trackingIsDrifting(candidate.spreadP90Deg, candidate.stabilityDeg)

    /**
     * Should this start-hold [candidate] replace the persisted [incumbent]?
     *
     * [incumbent] is null when the phone has never had a trim, in which case
     * anything that is not drifting is better than the bracket defaults and is
     * accepted — refusing the only measurement available would leave the scan
     * on CAD nominals for no gain.
     */
    fun judge(candidate: MountTrim, incumbent: MountTrim?): StartHoldVerdict = when {
        trackingIsDrifting(candidate) -> StartHoldVerdict.REFUSE_DRIFT
        incumbent == null -> StartHoldVerdict.ACCEPT
        candidate.qualityRank > incumbent.qualityRank + MATERIAL_MARGIN_DEG ->
            StartHoldVerdict.REFUSE_WORSE
        else -> StartHoldVerdict.ACCEPT
    }

    /**
     * The sentence the start progress panel shows for a refusal — six words of
     * instruction, one short detail line, per item 98's wording law.
     */
    fun refusalStatus(verdict: StartHoldVerdict): String? = when (verdict) {
        StartHoldVerdict.ACCEPT -> null
        StartHoldVerdict.REFUSE_DRIFT -> "Tracking is drifting — hold on."
        StartHoldVerdict.REFUSE_WORSE -> "Worse reading — holding on."
    }

    /**
     * The log line for a refusal, with every number that produced it. Log lines
     * are not user-facing and are deliberately exempt from the wording law:
     * this is the line that has to answer "why" a year from now.
     */
    fun refusalLogLine(
        verdict: StartHoldVerdict,
        candidate: MountTrim,
        incumbent: MountTrim?,
    ): String {
        val cand = ("candidate magnitude=%.2fdeg spreadP90=%.2fdeg stability=%s samples=%d rank=%.2f")
            .format(
                candidate.magnitudeDeg,
                candidate.spreadP90Deg,
                candidate.accuracyDeg?.let { "%.2fdeg".format(it) } ?: "unmeasured",
                candidate.sampleCount,
                candidate.qualityRank,
            )
        val inc = incumbent?.let {
            ("incumbent magnitude=%.2fdeg stability=%s rank=%.2f")
                .format(
                    it.magnitudeDeg,
                    it.accuracyDeg?.let { a -> "%.2fdeg".format(a) } ?: "unmeasured",
                    it.qualityRank,
                )
        } ?: "no incumbent"
        return when (verdict) {
            StartHoldVerdict.REFUSE_DRIFT -> {
                val ratio =
                    if (candidate.spreadP90Deg > 0.0) candidate.stabilityDeg / candidate.spreadP90Deg else 0.0
                val why = ("start hold: REFUSED — the pose drifted during the hold " +
                    "(spreadP90 %.2fdeg is %.1fx below split-half %.2fdeg: the phone was still and " +
                    "the tracker was not). ")
                    .format(candidate.spreadP90Deg, ratio, candidate.stabilityDeg)
                why + cand + "; " + inc
            }
            StartHoldVerdict.REFUSE_WORSE ->
                "start hold: REFUSED — materially worse than the persisted trim " +
                    "(margin ${"%.2f".format(MATERIAL_MARGIN_DEG)}deg). $cand; $inc"
            StartHoldVerdict.ACCEPT -> "start hold: accepted. $cand; $inc"
        }
    }
}
