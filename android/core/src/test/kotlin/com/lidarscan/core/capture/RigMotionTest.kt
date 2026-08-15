package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B8's motion estimate and keyframe cadence. These two numbers are the ones
 * S6 identified as the biggest lever in the whole colorization budget
 * (WIZARD.md §4: time sync x turn rate is 16.7 px of a 20.2 px budget, and
 * the stated mitigation is to prefer keyframes taken while turning slowly),
 * so their behaviour is worth pinning down away from a device.
 */
class RigMotionTest {

    private fun walkAndTurn(
        count: Int,
        periodNs: Long = 33_000_000L,
        speed: Double = 1.0,
        yawRateDegPerS: Double = 12.0,
        tracking: Boolean = true,
    ): List<PoseSample> = (0 until count).map { i ->
        val t = i * periodNs / 1e9
        PoseSample(
            tMonoNs = 500_000_000L + i * periodNs,
            position = Vec3(speed * t, 1.4, 0.0),
            orientation = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), Math.toRadians(yawRateDegPerS * t)),
            tracking = tracking,
        )
    }

    @Test
    fun `recovers the commanded angular rate and linear speed`() {
        val tracker = RigMotionTracker()
        val samples = walkAndTurn(20)
        samples.forEach(tracker::add)
        val estimate = tracker.estimateAt(samples[10].tMonoNs)
        assertTrue(estimate.valid)
        assertEquals(Math.toRadians(12.0), estimate.angularRateRadPerS.toDouble(), 1e-3)
        assertEquals(1.0, estimate.linearSpeedMPerS.toDouble(), 1e-3)
    }

    @Test
    fun `a stationary rig reads as stationary`() {
        val tracker = RigMotionTracker()
        walkAndTurn(20, speed = 0.0, yawRateDegPerS = 0.0).forEach(tracker::add)
        val estimate = tracker.estimateAt(600_000_000L)
        assertEquals(0.0, estimate.angularRateRadPerS.toDouble(), 1e-9)
        assertEquals(0.0, estimate.linearSpeedMPerS.toDouble(), 1e-9)
    }

    @Test
    fun `no window means no estimate, not a zero`() {
        val tracker = RigMotionTracker()
        // A zero rate would tell the colorizer this was an ideally still
        // frame — the opposite of "unknown".
        assertFalse(tracker.estimateAt(1_000L).valid)
        tracker.add(PoseSample(1_000L, Vec3.ZERO, Quat.IDENTITY, true))
        assertFalse(tracker.estimateAt(1_000L).valid)
    }

    @Test
    fun `tracking loss inside the window invalidates the estimate`() {
        val tracker = RigMotionTracker()
        val samples = walkAndTurn(10).toMutableList()
        samples[5] = samples[5].copy(tracking = false)
        samples.forEach(tracker::add)
        // The bracketing pair around sample 5 carries the loss.
        assertFalse(tracker.estimateAt(samples[5].tMonoNs).valid)
    }

    @Test
    fun `out-of-order poses are dropped, matching the engine's own refusal`() {
        val tracker = RigMotionTracker()
        tracker.add(PoseSample(2_000L, Vec3.ZERO, Quat.IDENTITY, true))
        tracker.add(PoseSample(1_000L, Vec3(9.0, 9.0, 9.0), Quat.IDENTITY, true))
        assertEquals(1, tracker.size())
        assertEquals(2_000L, tracker.latest()!!.tMonoNs)
    }

    @Test
    fun `the ring buffer is bounded`() {
        val tracker = RigMotionTracker(capacity = 8)
        walkAndTurn(50).forEach(tracker::add)
        assertEquals(8, tracker.size())
    }

    // --- selector -----------------------------------------------------------

    @Test
    fun `the selector holds the 2 to 5 fps cadence`() {
        val selector = KeyframeSelector(targetFps = 3.0)
        var emitted = 0
        // 10 seconds at 30 Hz, always slow enough to qualify.
        for (i in 0 until 300) {
            val t = i * 33_333_333L
            if (selector.offer(t, tracking = true, angularRateRadPerS = 0.05, linearSpeedMPerS = 0.5, motionValid = true) != null) {
                emitted++
            }
        }
        assertTrue("emitted $emitted keyframes in 10 s at a 3 fps target", emitted in 25..35)
    }

    /** S6's motion gate: a rig whipping around at 40 deg/s must produce nothing. */
    @Test
    fun `frames over the turn-rate gate are refused outright`() {
        val selector = KeyframeSelector(targetFps = 3.0)
        var emitted = 0
        for (i in 0 until 300) {
            val t = i * 33_333_333L
            if (selector.offer(t, true, Math.toRadians(40.0), 0.5, motionValid = true) != null) emitted++
        }
        assertEquals(0, emitted)
    }

    @Test
    fun `within a slot the slowest frame wins`() {
        val selector = KeyframeSelector(targetFps = 2.0)
        var chosen: KeyframeSelector.Candidate? = null
        val rates = listOf(0.20, 0.02, 0.15, 0.10)
        for ((i, rate) in rates.withIndex()) {
            val out = selector.offer(i * 100_000_000L, true, rate, 0.3, motionValid = true)
            if (out != null) chosen = out
        }
        assertNotNull(chosen)
        assertEquals("the slowest frame in the slot, not the first", 0.02, chosen!!.angularRateRadPerS, 1e-9)
    }

    @Test
    fun `frames without tracking never become keyframes`() {
        val selector = KeyframeSelector(targetFps = 3.0)
        for (i in 0 until 60) {
            assertNull(selector.offer(i * 33_333_333L, tracking = false, 0.01, 0.1, motionValid = true))
        }
    }

    /**
     * When the motion estimate is not available yet (the tracker has no
     * window), the gate cannot be applied — and the frame is taken rather than
     * dropped, because "unknown" at the start of a capture would otherwise
     * mean no keyframes at all for the first second.
     */
    @Test
    fun `an unknown motion estimate does not block the cadence`() {
        val selector = KeyframeSelector(targetFps = 3.0)
        var emitted = 0
        for (i in 0 until 60) {
            if (selector.offer(i * 33_333_333L, true, 0.0, 0.0, motionValid = false) != null) emitted++
        }
        assertTrue(emitted > 0)
    }

    @Test
    fun `the gate predicate matches S6's 15 deg per second`() {
        val selector = KeyframeSelector()
        assertTrue(selector.withinMotionGate(Math.toRadians(14.0)))
        assertFalse(selector.withinMotionGate(Math.toRadians(16.0)))
    }
}
