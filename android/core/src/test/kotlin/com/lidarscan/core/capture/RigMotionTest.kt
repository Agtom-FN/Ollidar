package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

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

    // --- ROUND 5 AUDIT: false-motion-while-stationary regression coverage ---
    //
    // The owner reported the round-5 walkthrough motion hint firing while the
    // phone was stationary. `KeyframeRecorder.onFrame` and
    // `MountCalibrationViewModel.processDetection` are the two real callers,
    // and BOTH use the exact same pattern: `motion.add(sampleForThisFrame)`
    // then, in the same call, `motion.estimateAt(sampleForThisFrame.tMonoNs)`
    // — i.e. estimate AT the timestamp of the sample that was JUST added, with
    // no future sample ever existing yet. These tests simulate that pattern
    // directly rather than the "preload the whole stream, query the middle"
    // shape the tests above use, because the bug only reproduced under it.

    private fun liveStream(
        tracker: RigMotionTracker,
        count: Int,
        periodNs: Long = 33_000_000L,
        position: (Int) -> Vec3,
        orientation: (Int) -> Quat = { Quat.IDENTITY },
        tracking: Boolean = true,
    ): List<RigMotionEstimate> = (0 until count).map { i ->
        val t = 500_000_000L + i * periodNs
        tracker.add(PoseSample(t, position(i), orientation(i), tracking))
        // The live call pattern: estimate AT the sample just added, not at
        // some later, fully-buffered query time.
        tracker.estimateAt(t)
    }

    @Test
    fun `a live stationary stream reads ~0 speed, not permanently invalid`() {
        val tracker = RigMotionTracker()
        val estimates = liveStream(tracker, count = 40, position = { Vec3.ZERO })

        // The estimator needs at least two distinct-time samples in the
        // window before it can say anything — the first sample or two are
        // legitimately "not enough window yet", not a bug.
        val settled = estimates.drop(5)
        assertTrue("expected at least one valid estimate once the window has samples in it", settled.any { it.valid })
        for (e in settled) {
            if (!e.valid) continue
            assertEquals(0.0, e.linearSpeedMPerS.toDouble(), 1e-6)
            assertEquals(0.0, e.angularRateRadPerS.toDouble(), 1e-6)
        }
    }

    /**
     * The exact bug this audit found: before the fix, `before`/`after` could
     * both resolve to the sample that was just added (nothing newer exists
     * live), giving `dtNs == 0` and `valid == false` on every single call —
     * silently disabling the whole motion gate rather than reading as
     * stationary. Pinned directly so a regression shows up as a broken test,
     * not as a field report.
     */
    @Test
    fun `a live stream eventually produces a valid estimate, not permanent invalidity`() {
        val tracker = RigMotionTracker()
        val estimates = liveStream(tracker, count = 40, position = { i -> Vec3(i * 0.033, 0.0, 0.0) })
        assertTrue(
            "expected at least one valid estimate across 40 live frames — got all-invalid, " +
                "which is the dtNs==0 regression this test guards against",
            estimates.any { it.valid },
        )
    }

    @Test
    fun `a live stationary stream WITH pose jitter still reads ~0 speed`() {
        // Real ARCore poses jitter a few mm / a fraction of a degree per frame
        // even when the phone is physically still. A deterministic pseudo-random
        // jitter (not a fixed seed chosen to flatter the test) around a fixed
        // point, well under ARCore's real relative-pose-error scale (S6:
        // ~11 mm at 3 m).
        val tracker = RigMotionTracker()
        val rnd = java.util.Random(42)
        fun jitterM() = (rnd.nextDouble() - 0.5) * 0.006 // +/- 3 mm
        fun jitterRad() = (rnd.nextDouble() - 0.5) * Math.toRadians(0.4) // +/- 0.2 deg

        val estimates = liveStream(
            tracker,
            count = 60,
            position = { Vec3(jitterM(), jitterM(), jitterM()) },
            orientation = { Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), jitterRad()) },
        )

        val settled = estimates.drop(5).filter { it.valid }
        assertTrue("expected valid estimates once the window has samples in it", settled.isNotEmpty())
        // A stationary-but-jittery rig must read as SLOW, not as a fast walk or
        // turn — well under both KeyframeSelector's 15 deg/s gate and any
        // walking speed, or the round-5 "moving too fast" hint would fire on a
        // phone sitting still on a table.
        for (e in settled) {
            assertTrue("jitter-inflated angular rate ${e.angularRateRadPerS} rad/s looks like real motion", abs(e.angularRateRadPerS) < Math.toRadians(15.0).toFloat())
            assertTrue("jitter-inflated linear speed ${e.linearSpeedMPerS} m/s looks like real motion", abs(e.linearSpeedMPerS) < 0.3f)
        }
    }

    @Test
    fun `a live moving stream reads a real, non-zero speed (positive case)`() {
        val tracker = RigMotionTracker()
        // 1.2 m/s walk, matching a typical walkthrough pace.
        val estimates = liveStream(tracker, count = 60, position = { i -> Vec3(1.2 * i * 33_000_000L / 1e9, 0.0, 0.0) })
        val settled = estimates.drop(10).filter { it.valid }
        assertTrue(settled.isNotEmpty())
        for (e in settled) {
            assertEquals(1.2, e.linearSpeedMPerS.toDouble(), 0.05)
        }
    }

    @Test
    fun `the gate predicate matches S6's 15 deg per second`() {
        val selector = KeyframeSelector()
        assertTrue(selector.withinMotionGate(Math.toRadians(14.0)))
        assertFalse(selector.withinMotionGate(Math.toRadians(16.0)))
    }
}
