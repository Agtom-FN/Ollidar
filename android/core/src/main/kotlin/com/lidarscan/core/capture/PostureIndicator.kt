package com.lidarscan.core.capture

import com.lidarscan.core.calib.HoldOrientation
import kotlin.math.abs
import kotlin.math.hypot

/**
 * ROUND 33 item 179 — **posture, which is two angles, not one.**
 *
 * Round 28 drew a dial and round 30 gave it a source that moves. The owner used
 * the moving one and found the next thing wrong with it: **one axis is not
 * posture.** A rig can be perfectly square in the screen plane — the round-28
 * needle dead level, orange, nothing to report — and be aimed at the floor, and
 * the D6's fan sweeps whatever it is aimed at. The dial cannot say so because
 * [AttitudeIndicator.reading] is handed a roll and only a roll.
 *
 * This is that reading with its second angle, and it is deliberately built as a
 * layer ON TOP of round 28 rather than as a replacement for it:
 *
 *  * the tolerance is [AttitudeIndicator.AMBER_DEG] — read, not copied, because
 *    item 179(d) is explicit that a ring and a colour drawn from two constants
 *    is how they come to disagree;
 *  * the roll is [AttitudeIndicator.deviationFromSquareDeg] — the same
 *    snap-to-the-nearest-quarter-turn that makes a landscape hold read level,
 *    unchanged and unrestated;
 *  * the pitch is [HoldOrientation.screenPitchDeg], which is the OTHER angle of
 *    the vector [LiveAttitude] is already filtering. No second sensor, no second
 *    listener, no second time constant, and no second throttle.
 *
 * ### Why the two axes combine radially
 *
 * [offPostureDeg] is `hypot(pitch, roll)` and not `max(|pitch|, |roll|)`,
 * because the thing being measured is the angle between where the phone is
 * pointing and where it should be pointing, and that angle does not care which
 * way round the housing it is. It is also what makes the recording strip's
 * tolerance RING true: a circle at the 10° radius is the boundary if and only
 * if the test is radial, and a ring the state does not agree with is worse than
 * no ring.
 *
 * ### Everything here is arithmetic
 *
 * Same rule as round 28's: an instrument that lies is worse than no instrument,
 * and the only way to know it does not lie is to test it on a bare JVM against
 * literal angles. The Compose side of item 179 draws what this object returns
 * and computes nothing.
 */
object PostureIndicator {

    /**
     * The one tolerance, item 179(d). An alias and not a value: it is
     * [AttitudeIndicator.AMBER_DEG] (10°), so the ring, the colour, the hint
     * and the haptic can only ever change together.
     */
    val toleranceDeg: Double get() = AttitudeIndicator.AMBER_DEG

    /**
     * The largest tilt either indicator will DRAW, degrees.
     *
     * Roll cannot exceed 45° by construction ([AttitudeIndicator
     * .MAX_DEVIATION_DEG]); pitch can reach 90°, and a ghost phone rotated 90°
     * is a line one pixel wide — the instrument would vanish exactly when it
     * has the most to say. So the drawing saturates and the WORDS do not: the
     * hint and the reading are still the true angle.
     */
    const val MAX_DRAWN_DEG: Double = 45.0

    // ── The wording, item 179(b) ───────────────────────────────────────────
    //
    // Four instructions, two words each, well under the wording law's six, and
    // exactly ONE of them is ever shown: the dominant axis. "Tilt forward and
    // level right" is a sentence an operator has to parse while walking, and
    // fixing the larger error usually fixes both anyway, because a hand that is
    // 20° back and 4° over is a hand doing one thing wrong.

    /** Leaning back — the top edge away, the sensor aimed low. Bring it forward. */
    const val HINT_TILT_FORWARD = "Tilt forward."

    /** Leaning forward — the top edge towards the operator, the sensor aimed high. */
    const val HINT_TILT_BACK = "Tilt back."

    /** The LEFT edge is high. Bring it down. */
    const val HINT_LEVEL_LEFT = "Level left."

    /** The RIGHT edge is high. Bring it down. */
    const val HINT_LEVEL_RIGHT = "Level right."

    /**
     * @param pitchDeg signed lean, positive BACK (screen towards the sky). See
     *   [HoldOrientation.screenPitchDeg].
     * @param rollDeg signed deviation from the nearest square hold, `[-45, 45]`,
     *   positive with the RIGHT edge high. See
     *   [AttitudeIndicator.deviationFromSquareDeg].
     * @param offPostureDeg the combined magnitude — what the tolerance is tested
     *   against, and what the instrument announces.
     * @param beyondTolerance true past [toleranceDeg]; amber.
     * @param known false when the phone is too flat for the angles to mean
     *   anything ([HoldOrientation.confident]), or when there is no reading at
     *   all. The indicators draw their housing and nothing in it.
     */
    data class Reading(
        val pitchDeg: Double,
        val rollDeg: Double,
        val offPostureDeg: Double,
        val beyondTolerance: Boolean,
        val known: Boolean,
    ) {
        /** The pitch as it is DRAWN — saturated at [MAX_DRAWN_DEG]. See there. */
        val drawnPitchDeg: Double get() = pitchDeg.coerceIn(-MAX_DRAWN_DEG, MAX_DRAWN_DEG)

        /** The roll as it is drawn. Already inside the range; clamped for symmetry. */
        val drawnRollDeg: Double get() = rollDeg.coerceIn(-MAX_DRAWN_DEG, MAX_DRAWN_DEG)

        /**
         * The correction to show, or null when there is nothing to correct.
         *
         * The dominant axis only. Ties go to pitch — arbitrary, but stated, and
         * a tie is one sample of a hand that is about to move anyway.
         */
        val hint: String?
            get() = when {
                !known || !beyondTolerance -> null
                abs(pitchDeg) >= abs(rollDeg) -> if (pitchDeg > 0) HINT_TILT_FORWARD else HINT_TILT_BACK
                rollDeg > 0 -> HINT_LEVEL_RIGHT
                else -> HINT_LEVEL_LEFT
            }
    }

    /** The reading for a phone whose posture cannot be established. */
    val UNKNOWN = Reading(
        pitchDeg = 0.0,
        rollDeg = 0.0,
        offPostureDeg = 0.0,
        beyondTolerance = false,
        known = false,
    )

    /** From the two angles directly — the form the tests state their cases in. */
    fun reading(pitchDeg: Double, rollDeg: Double, confident: Boolean = true): Reading {
        if (!confident || pitchDeg.isNaN() || rollDeg.isNaN()) return UNKNOWN
        val deviation = AttitudeIndicator.deviationFromSquareDeg(rollDeg)
        val magnitude = hypot(pitchDeg, deviation)
        return Reading(
            pitchDeg = pitchDeg,
            rollDeg = deviation,
            offPostureDeg = magnitude,
            beyondTolerance = magnitude > toleranceDeg,
            known = true,
        )
    }

    /**
     * From the live hold — the form both placements use.
     *
     * Null is a reading in its own right: it is what [LiveAttitudeFeed]
     * publishes before the first sample and again once the last placement lets
     * go, and it must draw as "no reading" rather than as the last one seen.
     */
    fun reading(hold: HoldOrientation?): Reading =
        if (hold == null) UNKNOWN
        else reading(hold.screenPitchDeg, hold.screenUpAngleDeg, hold.confident)

    /**
     * Where the recording strip's bubble sits, in the housing's own pixels,
     * `+x` right and `+y` DOWN (Compose's canvas sense, so the caller adds it
     * to the centre and draws).
     *
     * ### The two signs, which are the whole design
     *
     *  * **Down for a backward lean.** Positive pitch is the top edge away and
     *    therefore the rear sensor aimed at the floor; the bubble marks where
     *    the rig is pointing, so it drops. Tilt forward and it climbs home.
     *  * **Left for a left bank.** Positive roll is the RIGHT edge high, i.e.
     *    the left edge low, and the bubble rolls to the low side the way a ball
     *    rolls downhill in a bowl. Hence the negation, and it is the only one.
     *
     * ### The scale and the clamp
     *
     * [toleranceRadiusPx] is where the tolerance ring is drawn, and the mapping
     * puts exactly [toleranceDeg] there — so "inside the ring" and "inside
     * tolerance" are the same statement rather than two that nearly agree.
     * Beyond that the offset is clamped **radially** to [maxRadiusPx], which
     * keeps the bubble whole inside the housing at any angle and keeps its
     * DIRECTION honest while it is pegged: a phone 40° off still says which way.
     */
    fun bubbleOffset(
        reading: Reading,
        toleranceRadiusPx: Float,
        maxRadiusPx: Float,
    ): BubbleOffset {
        if (!reading.known) return BubbleOffset(0f, 0f)
        val perDegree = if (toleranceDeg <= 0.0) 0f else (toleranceRadiusPx / toleranceDeg).toFloat()
        val dx = (-reading.rollDeg).toFloat() * perDegree
        val dy = reading.pitchDeg.toFloat() * perDegree
        val length = hypot(dx, dy)
        if (length <= maxRadiusPx || length <= 0f) return BubbleOffset(dx, dy)
        val scale = maxRadiusPx / length
        return BubbleOffset(dx * scale, dy * scale)
    }

    /** [bubbleOffset]'s result. Pixels from the housing's centre, `+y` down. */
    data class BubbleOffset(val dx: Float, val dy: Float)
}
