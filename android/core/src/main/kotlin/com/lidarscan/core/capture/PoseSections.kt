package com.lidarscan.core.capture

import kotlinx.serialization.Serializable

/**
 * ROUND 7, item 3 — **the pose jumps that turn one scan into "sections".**
 *
 * The origin being at capture start is by design: each scan is its own frame
 * until georeferencing, and the owner's "the scan of sections map with the imu
 * and the origin reset start when capture start" is half a description of that
 * intended behaviour. The other half is not intended.
 *
 * ARCore's world frame is not a fixed thing. When VIO loses tracking and then
 * **relocalizes**, the reported pose can move by tens of centimetres between two
 * consecutive frames — the session is correcting its own accumulated drift, and
 * it does so as a step, not a drift. That is correct behaviour from ARCore and
 * catastrophic for a pushbroom, which composes `world_from_phone(t)` with the
 * mount extrinsic and has no way to know that "world" just moved. Every point
 * resolved before the jump sits in the old frame and every point after it in the
 * new one, and the two slabs are offset by exactly the correction: a room in
 * sections, with the seam wherever tracking hiccupped.
 *
 * A relocalization is not a mistake to be suppressed. The right response is to
 * **record where it happened**, so that:
 *
 *  * the operator is told, while walking, that the scan just acquired a seam;
 *  * the seal writes the seam times into the project, so post-processing has
 *    the one piece of information that lets it align the two halves rather than
 *    averaging across a discontinuity (A13's merge is exactly this machinery,
 *    one level up: two clouds, a rigid transform between them).
 *
 * ## What counts as a jump
 *
 * Two independent detectors, because they catch different failures:
 *
 *  1. **A tracking gap.** `tracking` going false and later true again is
 *     ARCore telling us it lost and re-acquired. Whether or not the pose moved,
 *     the frame may have. This is the reliable signal and it needs no threshold.
 *  2. **A kinematically impossible step while tracking.** A person cannot move
 *     [MAX_SPEED_MPS] or turn [MAX_TURN_RATE_DEG_PER_S] — so a step that implies
 *     more than that between two consecutive poses is the frame moving, not the
 *     operator. This catches the silent case: ARCore correcting drift **without
 *     ever reporting a loss**, which is the one that produces a seam nobody can
 *     explain afterwards.
 *
 * Both are deliberately generous. A false section break costs a line in the
 * manifest and a hint on screen; a missed one costs a bent room.
 */
@Serializable
data class PoseSectionBreak(
    /** When the discontinuity was observed, in the engine's clock (CLOCK_BOOTTIME ns). */
    val tMonoNs: Long,
    /** How far the reported position moved across the break, metres. */
    val positionJumpM: Double,
    /** How far the reported orientation moved across the break, degrees. */
    val rotationJumpDeg: Double,
    /** Gap between the two poses that straddle the break, milliseconds. */
    val gapMillis: Long,
    val reason: Reason,
) {
    enum class Reason {
        /** ARCore lost tracking and re-acquired it — the frame may have been corrected. */
        TRACKING_REGAINED,

        /** The reported pose stepped further than a walking human could have moved. */
        IMPOSSIBLE_STEP,
    }

    /** One sentence, for the capture panel and the log. */
    val summary: String
        get() = when (reason) {
            Reason.TRACKING_REGAINED ->
                "tracking re-acquired after ${gapMillis} ms — the frame may have shifted " +
                    "%.2f m / %.1f°".format(positionJumpM, rotationJumpDeg)
            Reason.IMPOSSIBLE_STEP ->
                "pose stepped %.2f m / %.1f° in ${gapMillis} ms — faster than a walk, so the " +
                    "tracker corrected itself".format(positionJumpM, rotationJumpDeg)
        }
}

/**
 * Consumes the same [PoseSample] stream the keyframe recorder and the mount
 * re-zero already read, and reports section breaks. Pure `:core`, no ARCore
 * types, therefore unit-testable on a bare JVM — which matters, because no
 * ARCore device exists in this environment and this is arithmetic that decides
 * whether a scan is one cloud or two.
 *
 * Thread-safe by the same rule as [RigMotionTracker]: the ARCore frame callback
 * is the only writer, and readers take a snapshot.
 */
class PoseSectionTracker(
    private val maxSpeedMps: Double = MAX_SPEED_MPS,
    private val maxTurnRateDegPerS: Double = MAX_TURN_RATE_DEG_PER_S,
    private val maxBreaks: Int = MAX_BREAKS,
) {
    private val breaksList = ArrayList<PoseSectionBreak>()
    private var previous: PoseSample? = null

    /** The last sample seen while tracking, for measuring across a tracking gap. */
    private var lastTracked: PoseSample? = null
    private var wasTracking = true

    @Synchronized
    fun add(sample: PoseSample): PoseSectionBreak? {
        val prev = previous
        previous = sample
        val break_ = detect(prev, sample)
        if (sample.tracking) lastTracked = sample
        wasTracking = sample.tracking
        if (break_ != null && breaksList.size < maxBreaks) breaksList.add(break_)
        return break_
    }

    private fun detect(prev: PoseSample?, sample: PoseSample): PoseSectionBreak? {
        if (prev == null) return null

        // (1) Tracking regained. Measured against the last sample that was
        //     actually tracking, not against the last sample seen — the poses
        //     reported *during* a loss are the tracker's own guesses and
        //     comparing to them measures nothing.
        if (sample.tracking && !wasTracking) {
            val anchor = lastTracked ?: return null
            val gapNs = sample.tMonoNs - anchor.tMonoNs
            return PoseSectionBreak(
                tMonoNs = sample.tMonoNs,
                positionJumpM = (sample.position - anchor.position).norm,
                rotationJumpDeg = Math.toDegrees(anchor.orientation.angleTo(sample.orientation)),
                gapMillis = gapNs / 1_000_000L,
                reason = PoseSectionBreak.Reason.TRACKING_REGAINED,
            )
        }

        // (2) A step no walk could produce, while tracking throughout.
        if (!sample.tracking || !prev.tracking) return null
        val dtNs = sample.tMonoNs - prev.tMonoNs
        if (dtNs <= 0L) return null
        val dtSeconds = dtNs / 1e9
        // Below this the quantisation of dt itself makes the implied rate
        // meaningless — two poses 1 ms apart imply 30 m/s from 3 cm of ordinary
        // VIO jitter.
        if (dtSeconds < MIN_DT_SECONDS) return null

        val dPos = (sample.position - prev.position).norm
        val dRotDeg = Math.toDegrees(prev.orientation.angleTo(sample.orientation))
        val impossible = dPos / dtSeconds > maxSpeedMps || dRotDeg / dtSeconds > maxTurnRateDegPerS
        if (!impossible) return null
        return PoseSectionBreak(
            tMonoNs = sample.tMonoNs,
            positionJumpM = dPos,
            rotationJumpDeg = dRotDeg,
            gapMillis = dtNs / 1_000_000L,
            reason = PoseSectionBreak.Reason.IMPOSSIBLE_STEP,
        )
    }

    /** Every break so far, oldest first. */
    @Synchronized
    fun breaks(): List<PoseSectionBreak> = breaksList.toList()

    /**
     * How many contiguous sections the capture is in. One with no breaks; a
     * break starts a new one. This is the number the capture panel shows,
     * because "3 sections" is a thing an operator can act on ("walk that stretch
     * again") and "2 relocalizations" is not.
     */
    @Synchronized
    fun sectionCount(): Int = breaksList.size + 1

    @Synchronized
    fun reset() {
        breaksList.clear()
        previous = null
        lastTracked = null
        wasTracking = true
    }

    companion object {
        /**
         * Faster than any walk-through, with room for a genuine lunge. A jog is
         * ~4 m/s; anything past 6 m/s between two ARCore frames is the frame
         * moving, not the phone.
         */
        const val MAX_SPEED_MPS = 6.0

        /**
         * A brisk hand-held turn is ~90°/s and a deliberate spin ~180°/s. 400°/s
         * sustained across a 33 ms frame interval is a relocalization.
         */
        const val MAX_TURN_RATE_DEG_PER_S = 400.0

        /** Below this, dt quantisation makes the implied rate meaningless. */
        const val MIN_DT_SECONDS = 0.008

        /** A bound on what one session records — a pathological session must not grow without limit. */
        const val MAX_BREAKS = 512
    }
}

/** Convenience for the capture panel: what to say about [sectionCount] sections. */
fun sectionHint(sectionCount: Int): String? = when {
    sectionCount <= 1 -> null
    else ->
        "Tracking jumped — this scan is now in $sectionCount sections. They are recorded with the scan and " +
            "aligned when you process it; walking the seam again with good texture in view helps."
}
