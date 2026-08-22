package com.lidarscan.core.capture

import com.lidarscan.core.calib.DeviceOrientation
import com.lidarscan.core.calib.StartOrientation
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 30 item 175 — **the roll-from-gravity math, and the filter around it.**
 *
 * Round 28 tested [AttitudeIndicator]'s angle mapping against literal angles
 * and it was correct; what shipped was an instrument wired to a constant, so
 * the correct mapping was applied to a number that never changed. These tests
 * are about the number.
 *
 * Every case is stated as a **gravity vector in the device frame** — "the sky
 * is towards the screen's right edge" — because that is what the accelerometer
 * actually hands over, and because a test written in quaternions would prove
 * the algebra rather than the reading.
 */
class LiveAttitudeTest {

    private val g = 9.81

    /**
     * The accelerometer's reading for a phone held **upright** (perfectly
     * off-flat) and rolled so that world-up sits [angleDeg] from the screen's
     * own up, positive towards the screen's right — which is exactly
     * `HoldOrientation.screenUpAngleDeg`'s definition.
     */
    private fun upright(angleDeg: Double): Triple<Double, Double, Double> {
        val r = Math.toRadians(angleDeg)
        return Triple(g * sin(r), g * cos(r), 0.0)
    }

    private fun LiveAttitude.settleAt(angleDeg: Double, tStartMs: Long = 0L): Long {
        // Ten time constants of the slowest filter: the step response is inside
        // a thousandth of a degree of its target, so a test about a STEADY
        // reading is never accidentally a test about the filter's lag.
        val (x, y, z) = upright(angleDeg)
        var t = tStartMs
        val end = tStartMs + (LiveAttitude.TAU_RAW_ACCEL_MS * 10).toLong()
        while (t <= end) {
            onDeviceUp(x, y, z, t, LiveAttitude.TAU_RAW_ACCEL_MS)
            t += 10
        }
        return t
    }

    // ── The four quadrants ────────────────────────────────────────────────

    @Test
    fun everySquareHoldReadsLevel() {
        val quadrants = mapOf(
            0.0 to DeviceOrientation.PORTRAIT,
            90.0 to DeviceOrientation.LANDSCAPE_LEFT,
            180.0 to DeviceOrientation.PORTRAIT_REVERSE,
            -90.0 to DeviceOrientation.LANDSCAPE_RIGHT,
        )
        for ((angle, orientation) in quadrants) {
            val filter = LiveAttitude()
            filter.settleAt(angle)
            val hold = filter.hold
            assertNotNull("no reading at $angle deg", hold)
            assertEquals("quadrant at $angle deg", orientation, hold!!.orientation)
            val reading = filter.reading
            assertTrue("reading not known at $angle deg", reading.known)
            assertEquals("a square hold is level at $angle deg", 0.0, reading.offSquareDeg, 0.01)
            assertFalse("a square hold is not amber at $angle deg", reading.beyondThreshold)
        }
    }

    @Test
    fun aTipReadsTheSameInEveryQuadrant() {
        // The whole point of round 28's "deviation from the nearest square
        // hold": a landscape operator holding 15 deg off is 15 deg off, not
        // 105 deg off. Both signs, because a needle that only tips one way is
        // a needle nobody trusts the second time.
        for (square in listOf(0.0, 90.0, 180.0, -90.0)) {
            for (tip in listOf(15.0, -15.0)) {
                val filter = LiveAttitude()
                filter.settleAt(square + tip)
                assertEquals(
                    "square=$square tip=$tip",
                    tip,
                    filter.reading.needleDeg,
                    0.05,
                )
                assertTrue("15 deg is past the amber threshold", filter.reading.beyondThreshold)
            }
        }
    }

    @Test
    fun theAmberThresholdIsTheOneRoundTwentyEightSet() {
        val below = LiveAttitude().apply { settleAt(AttitudeIndicator.AMBER_DEG - 1.0) }
        val above = LiveAttitude().apply { settleAt(AttitudeIndicator.AMBER_DEG + 1.0) }
        assertFalse(below.reading.beyondThreshold)
        assertTrue(above.reading.beyondThreshold)
    }

    @Test
    fun aPhoneLyingFlatHasNoReadingAtAll() {
        // Face up on a table: gravity is entirely out of the screen, and the
        // screen-plane angle is noise. The instrument must draw a ring and no
        // needle rather than a plausible wrong number.
        val filter = LiveAttitude()
        var t = 0L
        repeat(400) {
            filter.onDeviceUp(0.0, 0.0, g, t)
            t += 10
        }
        assertNotNull("a flat phone still produces a hold", filter.hold)
        assertFalse("a flat phone is not confident", filter.hold!!.confident)
        assertEquals(AttitudeIndicator.UNKNOWN, filter.reading)
    }

    // ── The filter ────────────────────────────────────────────────────────

    @Test
    fun theFirstSampleIsPublishedImmediately() {
        val filter = LiveAttitude()
        val first = filter.onDeviceUp(0.0, g, 0.0, 1_000L)
        assertNotNull("the first reading must not wait for the throttle", first)
        assertEquals(DeviceOrientation.PORTRAIT, first!!.orientation)
    }

    @Test
    fun publicationIsThrottledToTwentyHertz() {
        val filter = LiveAttitude()
        val (x, y, z) = upright(0.0)
        var emits = 0
        var t = 0L
        // One second of the round-9 IMU's 400 Hz stream.
        repeat(400) {
            if (filter.onDeviceUp(x, y, z, t) != null) emits++
            t += 2 // 2 ms — a shade faster than the round-9 stream's measured 400 Hz
        }
        // 400 samples at 2 ms is 800 ms of stream: one seeding emit plus one
        // per 50 ms. Never the 400 that an un-throttled feed would write into
        // Compose state.
        assertTrue("emitted $emits times, expected roughly 17", emits in 14..20)
    }

    @Test
    fun everySampleIsStillFilteredEvenWhenItIsNotPublished() {
        // The throttle must not become a decimator: a needle that only sees
        // every twentieth sample is a needle with twenty times the noise.
        val filter = LiveAttitude()
        filter.onDeviceUp(0.0, g, 0.0, 0L)
        val (x, y, z) = upright(40.0)
        // 40 ms of samples, all inside one throttle window.
        for (t in 1L..40L) filter.onDeviceUp(x, y, z, t, LiveAttitude.TAU_FUSED_MS)
        assertTrue(
            "the unpublished samples moved the state (${filter.reading.needleDeg})",
            filter.reading.needleDeg > 5.0,
        )
    }

    @Test
    fun aStepIsSmoothedAndNeverOvershoots() {
        val filter = LiveAttitude()
        filter.onDeviceUp(0.0, g, 0.0, 0L)
        val (x, y, z) = upright(40.0)
        val tau = LiveAttitude.TAU_FUSED_MS
        var previous = 0.0
        var atOneTau = 0.0
        var t = 0L
        while (t <= (tau * 4).toLong()) {
            t += 5
            filter.onDeviceUp(x, y, z, t, tau)
            val now = filter.reading.needleDeg
            assertTrue("the needle went backwards at $t ms", now >= previous - 1e-9)
            assertTrue("the needle overshot the step at $t ms ($now)", now <= 40.0 + 1e-9)
            previous = now
            if (t == tau.toLong()) atOneTau = now
        }
        // One time constant is ~63 % of a first-order step. The bounds are wide
        // because the filter runs on the DIRECTION and the angle response of a
        // normalised blend is only approximately exponential — what is being
        // pinned is that it is a lag, not a snap and not a crawl.
        assertTrue("at one tau the needle was $atOneTau deg, expected ~25", atOneTau in 20.0..30.0)
        assertTrue("after four tau the needle was $previous deg, expected ~40", previous > 38.0)
    }

    @Test
    fun theSameFilterBehavesTheSameAtFourHundredHertzAndAtThirty() {
        // The time constant is a TIME, not a per-sample weight: the IMU's
        // 400 Hz and ARCore's 30 Hz must land on the same needle after the same
        // wall-clock interval, or the instrument would visibly change character
        // the moment a recording started.
        val fast = LiveAttitude()
        val slow = LiveAttitude()
        fast.onDeviceUp(0.0, g, 0.0, 0L)
        slow.onDeviceUp(0.0, g, 0.0, 0L)
        val (x, y, z) = upright(40.0)
        var t = 0L
        while (t < 300L) {
            t += 2
            fast.onDeviceUp(x, y, z, t)
            if (t % 33L == 0L) slow.onDeviceUp(x, y, z, t)
        }
        assertEquals(fast.reading.needleDeg, slow.reading.needleDeg, 1.5)
    }

    @Test
    fun theFilterDoesNotSweepTheLongWayRoundTheSeam() {
        // A phone held upside down sits on the +-180 deg seam. An angle filter
        // crossing it would drag the needle a whole turn through portrait, once
        // per wobble. A direction filter cannot: there is no seam in a vector.
        val filter = LiveAttitude()
        filter.settleAt(179.0)
        val (x, y, z) = upright(-179.0)
        var t = 10_000L
        repeat(200) {
            filter.onDeviceUp(x, y, z, t, LiveAttitude.TAU_FUSED_MS)
            assertTrue(
                "the needle left the upside-down hold on its way across the seam " +
                    "(${filter.reading.needleDeg} deg)",
                abs(filter.reading.needleDeg) <= 2.0,
            )
            t += 5
        }
    }

    @Test
    fun freeFallAndDeadSensorsAreDropped() {
        val filter = LiveAttitude()
        filter.settleAt(30.0)
        val before = filter.reading.needleDeg
        assertNull(filter.onDeviceUp(0.0, 0.0, 0.0, 99_000L))
        assertNull(filter.onDeviceUp(Double.NaN, 1.0, 0.0, 99_100L))
        assertEquals("a dropped sample must not move the needle", before, filter.reading.needleDeg, 1e-9)
    }

    @Test
    fun theFirstUsableSampleAfterADeadOneStillSeeds() {
        val filter = LiveAttitude()
        assertNull(filter.onDeviceUp(0.0, 0.0, 0.0, 0L))
        assertEquals(AttitudeIndicator.UNKNOWN, filter.reading)
        assertNotNull(filter.onDeviceUp(0.0, g, 0.0, 10L))
    }

    @Test
    fun resetForgetsTheHold() {
        val filter = LiveAttitude()
        filter.settleAt(30.0)
        assertTrue(filter.reading.known)
        filter.reset()
        assertNull(filter.hold)
        assertEquals(AttitudeIndicator.UNKNOWN, filter.reading)
        // And seeds again from scratch rather than blending across the gap.
        val resumed = filter.onDeviceUp(0.0, g, 0.0, 500_000L)
        assertNotNull(resumed)
        assertEquals(0.0, filter.reading.needleDeg, 0.01)
    }

    // ── The ARCore path ───────────────────────────────────────────────────

    @Test
    fun aCameraPoseDecodesToTheSameHoldTheStartOrientationDoes() {
        // The pose path must not be a second opinion. It goes through round
        // 26's own decoder, and this pins that it lands where `classify` lands
        // for the same attitude in every quadrant.
        for (orientation in DeviceOrientation.values()) {
            val pose = StartOrientation.syntheticHold(orientation, sensorOrientationDeg = 90)
            val expected = StartOrientation.classify(pose, 90)
            val filter = LiveAttitude()
            var t = 0L
            repeat(200) {
                filter.onCameraPose(pose, 90, t)
                t += 10
            }
            val hold = filter.hold!!
            assertEquals(orientation, hold.orientation)
            assertEquals(expected.screenUpAngleDeg, hold.screenUpAngleDeg, 0.1)
            assertEquals(0.0, filter.reading.offSquareDeg, 0.1)
        }
    }

    @Test
    fun cosineAndSineAgreeWithTheDecodersOwnConvention() {
        // A guard on this file's own helper: if `upright()` ever disagreed with
        // `screenUpAngleDeg`'s definition, every assertion above would be
        // measuring the helper instead of the filter.
        val (x, y, z) = upright(37.0)
        val direct = StartOrientation.fromDeviceUp(com.lidarscan.core.calib.Vec3(x, y, z))
        assertEquals(37.0, direct.screenUpAngleDeg, 1e-6)
        assertEquals(g * sin(Math.toRadians(37.0)), x, 1e-9)
        assertEquals(g * cos(Math.toRadians(37.0)), y, 1e-9)
        assertEquals(0.0, z, 1e-9)
    }
}
