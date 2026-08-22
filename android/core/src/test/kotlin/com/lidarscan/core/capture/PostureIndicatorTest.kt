package com.lidarscan.core.capture

import com.lidarscan.core.WordingLaw
import com.lidarscan.core.calib.DeviceOrientation
import com.lidarscan.core.calib.HoldOrientation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.hypot

/**
 * ROUND 33 item 179 — **the posture reading, on a bare JVM.**
 *
 * Round 28's rule, applied to the second axis: an instrument that lies is worse
 * than no instrument, and the only way to know it does not lie is to test it
 * against literal angles with no Compose, no sensor and no emulator between the
 * statement and the assertion. Every number the two indicators draw with is
 * decided here — the tolerance, the combination of the axes, which correction
 * is shown, and where the bubble sits.
 */
class PostureIndicatorTest {

    private fun hold(pitch: Double, roll: Double, confident: Boolean = true) = HoldOrientation(
        orientation = DeviceOrientation.PORTRAIT,
        screenUpAngleDeg = roll,
        screenPitchDeg = pitch,
        tiltFromFlatDeg = 90.0 - kotlin.math.abs(pitch),
        confident = confident,
    )

    // ── the tolerance, which is round 28's and not a copy of it ────────────

    @Test
    fun `the tolerance is round twenty eight's threshold and not a second constant`() {
        // Item 179(d), as an identity rather than as a comment: two copies of a
        // threshold is how the ring at the 10-degree radius and the colour that
        // changes at 10 degrees come to disagree by a degree and a half.
        assertEquals(AttitudeIndicator.AMBER_DEG, PostureIndicator.toleranceDeg, 0.0)
    }

    @Test
    fun `either axis alone crosses the tolerance at ten degrees`() {
        assertFalse(PostureIndicator.reading(pitchDeg = 9.5, rollDeg = 0.0).beyondTolerance)
        assertTrue(PostureIndicator.reading(pitchDeg = 10.5, rollDeg = 0.0).beyondTolerance)
        assertFalse(PostureIndicator.reading(pitchDeg = 0.0, rollDeg = -9.5).beyondTolerance)
        assertTrue(PostureIndicator.reading(pitchDeg = 0.0, rollDeg = -10.5).beyondTolerance)
    }

    @Test
    fun `the two axes combine radially, so two legal angles can add up to an illegal one`() {
        // 8 and 8 are each inside the tolerance and the posture is not: the rig
        // is 11.3 degrees away from where it should be pointing, and the
        // operator cannot see either component from behind the phone. This is
        // the case a per-axis test would pass and the owner would photograph.
        val both = PostureIndicator.reading(pitchDeg = 8.0, rollDeg = 8.0)
        assertEquals(hypot(8.0, 8.0), both.offPostureDeg, 1e-9)
        assertTrue(both.beyondTolerance)

        // And the ring is drawn at that radius, so "outside the ring" and
        // "amber" are one statement.
        val onTheRing = PostureIndicator.reading(pitchDeg = 6.0, rollDeg = 8.0)
        assertEquals(10.0, onTheRing.offPostureDeg, 1e-9)
        assertFalse("exactly on the ring is still inside", onTheRing.beyondTolerance)
    }

    @Test
    fun `a roll-only posture reads exactly what round twenty eight's dial read`() {
        // The compatibility claim the emulator suite leans on: for a pitch of
        // zero the combined magnitude IS the old off-square number, so the
        // instrument's spoken description did not have to change and three
        // rounds of assertions on it stay honest.
        for (roll in listOf(0.0, 7.0, -15.0, 40.0, 130.0, -95.0)) {
            val old = AttitudeIndicator.reading(roll)
            val new = PostureIndicator.reading(pitchDeg = 0.0, rollDeg = roll)
            assertEquals("roll $roll", old.offSquareDeg, new.offPostureDeg, 1e-9)
            assertEquals("roll $roll", old.beyondThreshold, new.beyondTolerance)
        }
    }

    @Test
    fun `the roll axis still snaps to the nearest square hold, so a landscape hold is level`() {
        // Round 28's deviation-from-square, reused rather than restated: a
        // landscape hold reads level in the posture instrument for the same
        // reason it read level in the dial.
        val landscape = PostureIndicator.reading(pitchDeg = 0.0, rollDeg = 90.0)
        assertEquals(0.0, landscape.rollDeg, 1e-9)
        assertFalse(landscape.beyondTolerance)
        val landscapeOff = PostureIndicator.reading(pitchDeg = 0.0, rollDeg = 102.0)
        assertEquals(12.0, landscapeOff.rollDeg, 1e-9)
        assertTrue(landscapeOff.beyondTolerance)
    }

    @Test
    fun `a phone too flat to read draws nothing at all`() {
        val flat = PostureIndicator.reading(hold(pitch = 85.0, roll = 3.0, confident = false))
        assertFalse(flat.known)
        assertNull("an unknown posture has no correction to offer", flat.hint)
        assertEquals(PostureIndicator.UNKNOWN, flat)
        // And no reading at all — the feed's null, before the first sample and
        // after the last placement lets go — is the same drawing.
        assertEquals(PostureIndicator.UNKNOWN, PostureIndicator.reading(null))
    }

    @Test
    fun `the live hold carries both axes into the reading`() {
        val r = PostureIndicator.reading(hold(pitch = -22.0, roll = 95.0))
        assertTrue(r.known)
        assertEquals(-22.0, r.pitchDeg, 1e-9)
        assertEquals(5.0, r.rollDeg, 1e-9)
        assertEquals(hypot(22.0, 5.0), r.offPostureDeg, 1e-9)
    }

    // ── the correction, item 179(b) ────────────────────────────────────────

    @Test
    fun `there is no correction while the posture is good`() {
        assertNull(PostureIndicator.reading(pitchDeg = 0.0, rollDeg = 0.0).hint)
        assertNull(PostureIndicator.reading(pitchDeg = 6.0, rollDeg = 3.0).hint)
    }

    @Test
    fun `the correction names the dominant axis and never two at once`() {
        // Leaning BACK is the top edge away and the sensor aimed at the floor;
        // the correction is to bring it forward. The prototype's arrow, in a
        // string.
        assertEquals(
            PostureIndicator.HINT_TILT_FORWARD,
            PostureIndicator.reading(pitchDeg = 24.0, rollDeg = 6.0).hint,
        )
        assertEquals(
            PostureIndicator.HINT_TILT_BACK,
            PostureIndicator.reading(pitchDeg = -24.0, rollDeg = 6.0).hint,
        )
        // Positive roll is the RIGHT edge high — the phone banked left — and the
        // correction names the side that has to come down.
        assertEquals(
            PostureIndicator.HINT_LEVEL_RIGHT,
            PostureIndicator.reading(pitchDeg = 4.0, rollDeg = 26.0).hint,
        )
        assertEquals(
            PostureIndicator.HINT_LEVEL_LEFT,
            PostureIndicator.reading(pitchDeg = 4.0, rollDeg = -26.0).hint,
        )
    }

    @Test
    fun `a tie goes to pitch, deliberately and once`() {
        // Not an important choice, but an undocumented one flickers: a rig held
        // 12 and 12 would alternate between two instructions at 20 Hz.
        assertEquals(
            PostureIndicator.HINT_TILT_FORWARD,
            PostureIndicator.reading(pitchDeg = 12.0, rollDeg = 12.0).hint,
        )
    }

    @Test
    fun `the correction survives the landscape snap rather than reading the raw roll`() {
        // Held landscape and banked 26 degrees past square. The dominant axis is
        // the roll's DEVIATION (26), not its raw value (116) — if the snap were
        // skipped here, every landscape hold would be told to level itself.
        val r = PostureIndicator.reading(pitchDeg = 4.0, rollDeg = 116.0)
        assertEquals(26.0, r.rollDeg, 1e-9)
        assertEquals(PostureIndicator.HINT_LEVEL_RIGHT, r.hint)
    }

    @Test
    fun `every correction is legal under the wording law`() {
        listOf(
            PostureIndicator.HINT_TILT_FORWARD,
            PostureIndicator.HINT_TILT_BACK,
            PostureIndicator.HINT_LEVEL_LEFT,
            PostureIndicator.HINT_LEVEL_RIGHT,
        ).forEach {
            assertTrue("'$it' is two words, not six", WordingLaw.isInstruction(it))
            assertTrue(
                "'$it' on the Scan tab: ${WordingLaw.violations(it, WordingLaw.TabBarScreen.SCAN)}",
                WordingLaw.passes(it, WordingLaw.TabBarScreen.SCAN),
            )
            assertTrue("'$it' is a sentence", it.endsWith("."))
        }
    }

    // ── the drawing's saturation ───────────────────────────────────────────

    @Test
    fun `the ghost saturates at forty five degrees while the words keep telling the truth`() {
        val steep = PostureIndicator.reading(pitchDeg = 78.0, rollDeg = 0.0)
        assertEquals(78.0, steep.pitchDeg, 1e-9)
        assertEquals(PostureIndicator.MAX_DRAWN_DEG, steep.drawnPitchDeg, 1e-9)
        assertEquals(PostureIndicator.HINT_TILT_FORWARD, steep.hint)
        val backwards = PostureIndicator.reading(pitchDeg = -78.0, rollDeg = 0.0)
        assertEquals(-PostureIndicator.MAX_DRAWN_DEG, backwards.drawnPitchDeg, 1e-9)
        // Roll cannot exceed 45 by construction, so its clamp is a no-op and
        // asserting that is what stops the clamp being "tightened" one day.
        assertEquals(44.0, PostureIndicator.reading(pitchDeg = 0.0, rollDeg = 44.0).drawnRollDeg, 1e-9)
    }

    // ── the bubble, item 179(c) ────────────────────────────────────────────

    private val ring = 30f
    private val max = 54f

    private fun offset(pitch: Double, roll: Double) =
        PostureIndicator.bubbleOffset(PostureIndicator.reading(pitch, roll), ring, max)

    @Test
    fun `a level rig puts the bubble dead centre`() {
        val o = offset(0.0, 0.0)
        assertEquals(0f, o.dx, 1e-6f)
        assertEquals(0f, o.dy, 1e-6f)
    }

    @Test
    fun `ten degrees lands exactly on the tolerance ring, which is what makes the ring true`() {
        assertEquals(ring, offset(10.0, 0.0).dy, 1e-4f)
        assertEquals(ring, -offset(0.0, 10.0).dx, 1e-4f)
        // Any direction, not just the two axes: the ring is a circle and the
        // mapping has to be one too.
        val diagonal = offset(6.0, 8.0)
        assertEquals(ring, hypot(diagonal.dx, diagonal.dy), 1e-3f)
    }

    @Test
    fun `the bubble drops when the rig leans back and moves to the low side when it banks`() {
        // Positive pitch is the top edge away and therefore the rear sensor
        // aimed at the floor: the bubble marks where the rig is pointing, so it
        // goes DOWN (+y is down on a canvas).
        assertTrue("leaning back must drop the bubble", offset(5.0, 0.0).dy > 0f)
        assertTrue("leaning forward must lift it", offset(-5.0, 0.0).dy < 0f)
        // Positive roll is the right edge high, so the LEFT edge is low and the
        // bubble rolls downhill to the left.
        assertTrue("banking left must move it left", offset(0.0, 5.0).dx < 0f)
        assertTrue("banking right must move it right", offset(0.0, -5.0).dx > 0f)
    }

    @Test
    fun `a big tilt is clamped to the housing and keeps its direction while it is pegged`() {
        val far = offset(20.0, 20.0)
        assertEquals("clamped to the housing", max, hypot(far.dx, far.dy), 1e-3f)
        // Direction preserved: the unclamped vector is (-60, +60), so the pegged
        // one must still be down-and-left in equal measure. A bubble that stuck
        // to the rim at the wrong bearing would point the operator the wrong way
        // at exactly the moment he most needs it.
        assertEquals(-far.dx, far.dy, 1e-3f)
        assertTrue(far.dx < 0f && far.dy > 0f)

        // And a single axis pegs on that axis rather than on the diagonal.
        val pitchOnly = offset(40.0, 0.0)
        assertEquals(0f, pitchOnly.dx, 1e-6f)
        assertEquals(max, pitchOnly.dy, 1e-3f)
    }

    @Test
    fun `an unknown posture parks the bubble rather than drawing it somewhere`() {
        val o = PostureIndicator.bubbleOffset(PostureIndicator.UNKNOWN, ring, max)
        assertEquals(0f, o.dx, 1e-6f)
        assertEquals(0f, o.dy, 1e-6f)
    }
}
