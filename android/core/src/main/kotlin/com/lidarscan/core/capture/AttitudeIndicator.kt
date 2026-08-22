package com.lidarscan.core.capture

import kotlin.math.abs
import kotlin.math.round

/**
 * ROUND 28 item 168 — **the mini attitude indicator's angle mapping.**
 *
 * The owner asked for a small circular instrument beside the STOP button and
 * inside the hold-still card: a neutral ring with side ticks, an orange horizon
 * needle and centre dot that rotate with the device's roll, and the needle
 * turning amber past a tilt threshold. The drawing is a Compose `Canvas`; **the
 * angle is here**, in `:core`, because an instrument that lies is worse than no
 * instrument and the only way to know it does not lie is to test it on a bare
 * JVM against literal angles.
 *
 * ### What "level" means, and why it is not "roll = 0"
 *
 * The obvious mapping — needle angle = the device's roll in the screen plane —
 * is wrong for this rig, and wrong in a way that would make the instrument
 * useless exactly when it matters. `StartOrientation` already establishes that
 * the operator may hold the phone portrait *or* landscape and that both are
 * legitimate holds which round 20's trim absorbs completely. A landscape hold
 * has a roll of ±90°. An indicator that pegged its needle at 90° for the whole
 * of a perfectly good landscape scan would be read, correctly, as broken.
 *
 * So the instrument reads **deviation from the nearest square hold**: the
 * signed residual after snapping to the nearest quarter turn, which lands in
 * [-45°, +45°]. Held squarely in any of the four orientations the needle is
 * level; tip the rig and it tips with you, in the direction you tipped. That is
 * the question the operator is actually asking while walking — *am I holding
 * this square?* — rather than *what is my absolute roll?*, which he cannot act
 * on.
 *
 * ### The threshold
 *
 * [AMBER_DEG] is 10°. Below that a walking gait alone moves the needle a few
 * degrees per step and an amber instrument would be amber for the whole scan,
 * which is the same failure as a chip that never varies (item 149). Above it
 * the rig is genuinely off-square: at 10° of roll a D6's pushbroom sweep is
 * displaced across the walk direction by `sin 10° ≈ 17 %` of its stand-off,
 * which is the scale at which the seal grade starts to notice.
 *
 * ### When the answer is "I don't know"
 *
 * A phone held flat — face up on a table, or a ceiling scan — has no meaningful
 * screen-plane angle at all, and `StartOrientation.MIN_TILT_FROM_FLAT_DEG`
 * already encodes that with its `confident` flag. [Reading.known] carries it
 * through, and the instrument draws its ring with no needle rather than a
 * needle pointing at noise. An instrument that admits it cannot read is trusted;
 * one that shows a plausible wrong number once is never trusted again.
 */
object AttitudeIndicator {

    /** Past this much off-square, the needle goes amber. See the header. */
    const val AMBER_DEG: Double = 10.0

    /**
     * The largest deviation the instrument can express, by construction: the
     * residual after snapping to the nearest quarter turn cannot exceed 45°.
     *
     * Stated as a constant so the drawing code can size its arc against the
     * same number rather than assuming one.
     */
    const val MAX_DEVIATION_DEG: Double = 45.0

    /**
     * @param needleDeg the horizon needle's rotation, degrees, in
     *   `[-45, 45]`. Positive tips the needle's right-hand end **down**, which
     *   is the sense a real attitude indicator turns and the sense the mockup
     *   draws (`rotate(-12 …)` for a phone rolled 12° one way).
     * @param offSquareDeg the unsigned magnitude — what the threshold is tested
     *   against, and what a caller would announce.
     * @param beyondThreshold true past [AMBER_DEG]; the needle draws amber.
     * @param known false when the phone is too flat for the screen-plane angle
     *   to mean anything. The needle is not drawn.
     */
    data class Reading(
        val needleDeg: Double,
        val offSquareDeg: Double,
        val beyondThreshold: Boolean,
        val known: Boolean,
    )

    /** The reading for a phone whose attitude cannot be established. */
    val UNKNOWN = Reading(needleDeg = 0.0, offSquareDeg = 0.0, beyondThreshold = false, known = false)

    /**
     * Wrap an angle into `(-180, 180]`.
     *
     * Its own function because the wrap is where an angle mapping goes wrong,
     * and it goes wrong at exactly one input: 180 itself, which must come back
     * as +180 and not −180 so that a phone held upside down does not flicker
     * between two answers a hundred times a second.
     */
    fun wrapDegrees(deg: Double): Double {
        var d = deg % 360.0
        if (d <= -180.0) d += 360.0
        if (d > 180.0) d -= 360.0
        return d
    }

    /**
     * The signed deviation from the nearest square hold, in `[-45, 45]`.
     *
     * `roll − 90 · round(roll / 90)`. Portrait (0°), landscape either way
     * (±90°) and upside-down (180°) all read as level; anything between reads
     * as how far from the nearest of them.
     */
    fun deviationFromSquareDeg(rollDeg: Double): Double {
        val wrapped = wrapDegrees(rollDeg)
        val snapped = 90.0 * round(wrapped / 90.0)
        return wrapDegrees(wrapped - snapped)
    }

    /**
     * @param rollDeg `HoldOrientation.screenUpAngleDeg` — where world-up sits in
     *   the screen plane. Null when there is no attitude at all (no ARCore
     *   session, replay, a Mid-360 capture that does not use phone tracking).
     * @param confident `HoldOrientation.confident` — false when the phone is
     *   too flat for the angle to be meaningful.
     */
    fun reading(rollDeg: Double?, confident: Boolean = true): Reading {
        if (rollDeg == null || !confident || rollDeg.isNaN()) return UNKNOWN
        val deviation = deviationFromSquareDeg(rollDeg)
        val magnitude = abs(deviation)
        return Reading(
            needleDeg = deviation,
            offSquareDeg = magnitude,
            beyondThreshold = magnitude > AMBER_DEG,
            known = true,
        )
    }
}
