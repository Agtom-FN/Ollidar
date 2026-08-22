package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 28 item 168 — the attitude indicator's angle mapping, on a bare JVM.
 *
 * The instrument is a `Canvas` and cannot be asserted on without an emulator;
 * the *number it draws* is pure arithmetic and is asserted here, because the
 * whole value of an instrument is that it does not lie.
 */
class AttitudeIndicatorTest {

    private val eps = 1e-9

    // ── the wrap ────────────────────────────────────────────────────────────

    @Test
    fun `wrap lands in the half-open range`() {
        assertEquals(0.0, AttitudeIndicator.wrapDegrees(0.0), eps)
        assertEquals(90.0, AttitudeIndicator.wrapDegrees(90.0), eps)
        assertEquals(-90.0, AttitudeIndicator.wrapDegrees(-90.0), eps)
        assertEquals(-90.0, AttitudeIndicator.wrapDegrees(270.0), eps)
        assertEquals(1.0, AttitudeIndicator.wrapDegrees(361.0), eps)
        assertEquals(-1.0, AttitudeIndicator.wrapDegrees(-361.0), eps)
    }

    /**
     * The one input the wrap can get wrong, and the reason it is tested on its
     * own: 180 must come back as +180, not −180, or an upside-down phone
     * flickers between two answers at frame rate.
     */
    @Test
    fun `the half-turn resolves one way only`() {
        assertEquals(180.0, AttitudeIndicator.wrapDegrees(180.0), eps)
        assertEquals(180.0, AttitudeIndicator.wrapDegrees(-180.0), eps)
        assertEquals(180.0, AttitudeIndicator.wrapDegrees(540.0), eps)
    }

    // ── deviation from square ───────────────────────────────────────────────

    /**
     * The item's whole design decision: all four square holds read level. An
     * indicator pegged at 90° through a perfectly good landscape scan would be
     * read as broken, correctly.
     */
    @Test
    fun `every square hold reads level`() {
        listOf(0.0, 90.0, -90.0, 180.0, -180.0, 270.0, 360.0).forEach { roll ->
            assertEquals(
                "roll $roll should read level",
                0.0,
                AttitudeIndicator.deviationFromSquareDeg(roll),
                eps,
            )
        }
    }

    @Test
    fun `deviation is signed and keeps the direction you tipped`() {
        assertEquals(7.0, AttitudeIndicator.deviationFromSquareDeg(7.0), eps)
        assertEquals(-7.0, AttitudeIndicator.deviationFromSquareDeg(-7.0), eps)
        // Same 7° tip, measured from the landscape hold rather than portrait.
        assertEquals(7.0, AttitudeIndicator.deviationFromSquareDeg(97.0), eps)
        assertEquals(-7.0, AttitudeIndicator.deviationFromSquareDeg(83.0), eps)
    }

    @Test
    fun `deviation never exceeds the quarter-turn half width`() {
        var roll = -360.0
        while (roll <= 360.0) {
            val d = AttitudeIndicator.deviationFromSquareDeg(roll)
            assertTrue(
                "roll $roll gave $d",
                d >= -AttitudeIndicator.MAX_DEVIATION_DEG - 1e-9 &&
                    d <= AttitudeIndicator.MAX_DEVIATION_DEG + 1e-9,
            )
            roll += 0.5
        }
    }

    /** Exactly halfway between two square holds is the largest reading there is. */
    @Test
    fun `the diagonal hold is the worst case`() {
        assertEquals(45.0, AttitudeIndicator.deviationFromSquareDeg(45.0), eps)
        assertEquals(-45.0, AttitudeIndicator.deviationFromSquareDeg(-45.0), eps)
    }

    // ── the reading ─────────────────────────────────────────────────────────

    @Test
    fun `a square hold is known, level and not amber`() {
        val r = AttitudeIndicator.reading(0.0)
        assertTrue(r.known)
        assertEquals(0.0, r.needleDeg, eps)
        assertEquals(0.0, r.offSquareDeg, eps)
        assertFalse(r.beyondThreshold)
    }

    @Test
    fun `the threshold is exclusive, and it is on the magnitude`() {
        assertFalse(AttitudeIndicator.reading(AttitudeIndicator.AMBER_DEG).beyondThreshold)
        assertFalse(AttitudeIndicator.reading(-AttitudeIndicator.AMBER_DEG).beyondThreshold)
        assertTrue(AttitudeIndicator.reading(AttitudeIndicator.AMBER_DEG + 0.1).beyondThreshold)
        assertTrue(AttitudeIndicator.reading(-(AttitudeIndicator.AMBER_DEG + 0.1)).beyondThreshold)
    }

    /**
     * A tipped landscape hold must go amber for the same tip that would take a
     * portrait hold amber — the threshold is on the deviation, not on the roll.
     */
    @Test
    fun `landscape crosses the threshold at the same tip as portrait`() {
        assertFalse(AttitudeIndicator.reading(95.0).beyondThreshold)
        assertTrue(AttitudeIndicator.reading(105.0).beyondThreshold)
        assertEquals(15.0, AttitudeIndicator.reading(105.0).offSquareDeg, eps)
    }

    // ── the honest unknown ──────────────────────────────────────────────────

    @Test
    fun `no attitude at all is unknown, not level`() {
        assertEquals(AttitudeIndicator.UNKNOWN, AttitudeIndicator.reading(null))
        assertFalse(AttitudeIndicator.reading(null).known)
    }

    /**
     * A phone too flat to have a screen-plane angle reads unknown rather than
     * plausible-and-wrong. `StartOrientation` already makes this call; the
     * instrument must not launder its `confident = false` into a needle.
     */
    @Test
    fun `a phone held flat draws no needle`() {
        val r = AttitudeIndicator.reading(37.0, confident = false)
        assertFalse(r.known)
        assertEquals(0.0, r.needleDeg, eps)
        assertFalse(r.beyondThreshold)
    }

    @Test
    fun `a NaN roll is unknown rather than propagated into the drawing`() {
        assertFalse(AttitudeIndicator.reading(Double.NaN).known)
    }

    /**
     * The instrument is fed straight from `StartOrientation`, so the two must
     * agree without a conversion at the call site. A landscape-left hold
     * classified from literal gravity is level on this instrument.
     */
    @Test
    fun `it reads a StartOrientation classification directly`() {
        // World-up towards the screen's right edge: landscape-left, +90° roll.
        val hold = com.lidarscan.core.calib.StartOrientation.fromDeviceUp(
            com.lidarscan.core.calib.Vec3(1.0, 0.0, 0.0),
        )
        val r = AttitudeIndicator.reading(hold.screenUpAngleDeg, hold.confident)
        assertTrue(r.known)
        assertEquals(0.0, r.offSquareDeg, 1e-6)
    }
}
