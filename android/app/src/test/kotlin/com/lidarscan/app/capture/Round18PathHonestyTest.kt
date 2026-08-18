package com.lidarscan.app.capture

import com.lidarscan.core.capture.TrajectoryRibbon
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 18 item 70 — **"the path record seems not so accurate."**
 *
 * The owner is right, and the inaccuracy is structural, not noise. During his
 * 6-7 s tracking losses ARCore freezes (round 17 measured 181 consecutive
 * poses of 0.000 m / 0.00 deg in scan-040): the drawn path holds still while
 * he walks, then TELEPORTS to wherever tracking re-acquired — and that
 * teleport was drawn as an ordinary walked line and added to pathM as metres
 * walked. scan-046's seal shows the shape: a 6.9 s freeze, then a 0.595 m
 * jump, all inked in confident teal.
 *
 * The fix is verdicts, not smoothing: a segment the tracker was blind across
 * is carried as `jump`, drawn as a bridge (dashed on the tile and the plan
 * sheet, darkened red in the 3D ribbon), and kept out of pathM — which now
 * means what the card says it means. The jumped metres are reported beside
 * it, so nothing is hidden either.
 */
class Round18PathHonestyTest {

    /** Walks [metres] at 5 poses/metre, 150 ms apart, from [t0]. */
    private fun walk(
        r: TrajectoryTrailRecorder,
        from: Float,
        metres: Float,
        t0: Long,
        tracking: Boolean = true,
    ): Long {
        var d = 0.0f
        var t = t0
        while (d <= metres) {
            r.onPose(x = from + d, y = 1.45f, z = 0.0f, tracking = tracking, tMonoNs = t)
            d += 0.2f
            t += 150_000_000L
        }
        return t
    }

    @Test
    fun `a re-acquisition teleport is a jump, not metres walked`() {
        val r = TrajectoryTrailRecorder()
        // Two metres of honest walking...
        var t = walk(r, from = 0f, metres = 2f, t0 = 1_000_000_000L)
        val walkedBefore = r.totalPathM.value
        assertTrue(walkedBefore > 1.5f)
        assertEquals(0f, r.totalJumpM.value)

        // ...then a scan-046-shaped freeze: the tracker disowns its poses and
        // holds the last position for six seconds while the operator walks on.
        repeat(40) {
            r.onPose(x = 2f, y = 1.45f, z = 0f, tracking = false, tMonoNs = t)
            t += 150_000_000L
        }
        // Re-acquisition lands 1.5 m away, tracking again.
        r.onPose(x = 3.5f, y = 1.45f, z = 0f, tracking = true, tMonoNs = t)

        // The teleport went to jumpM, and pathM did not move.
        assertEquals(walkedBefore, r.totalPathM.value, 1e-4f)
        assertTrue("the 1.5 m teleport must be counted as jumped", r.totalJumpM.value > 1.0f)

        // And the trail carries the verdict for every renderer to draw.
        assertTrue(r.worldPoints.value.any { it.jump })
    }

    @Test
    fun `a refused re-anchor's silent teleport is caught by the speed rule`() {
        val r = TrajectoryTrailRecorder()
        var t = walk(r, from = 0f, metres = 1f, t0 = 1_000_000_000L)
        val walked = r.totalPathM.value
        // Tracking green the whole way, but the frame steps 2 m in 33 ms —
        // 60 m/s, ten times PoseSectionTracker's ceiling. This is what an
        // unhealed IMPOSSIBLE_STEP looks like to the trail.
        r.onPose(x = 3.2f, y = 1.45f, z = 0f, tracking = true, tMonoNs = t + 33_000_000L)
        assertEquals(walked, r.totalPathM.value, 1e-4f)
        assertTrue(r.totalJumpM.value > 1.5f)
        assertTrue(r.worldPoints.value.last().jump)
    }

    @Test
    fun `a clean walk has no jumps and its whole length is pathM`() {
        val r = TrajectoryTrailRecorder()
        walk(r, from = 0f, metres = 4f, t0 = 1_000_000_000L)
        assertEquals(0f, r.totalJumpM.value)
        assertTrue(r.totalPathM.value > 3.5f)
        assertFalse(r.worldPoints.value.any { it.jump })
        // The tile's own length agrees — one walk, one number.
        assertEquals(r.totalPathM.value, r.pathLengthM.value, 1e-3f)
    }

    @Test
    fun `the ribbon draws a bridge where the walk was blind`() {
        val r = TrajectoryTrailRecorder()
        var t = walk(r, from = 0f, metres = 1f, t0 = 1_000_000_000L)
        repeat(10) {
            r.onPose(x = 1f, y = 1.45f, z = 0f, tracking = false, tMonoNs = t)
            t += 150_000_000L
        }
        walk(r, from = 2.5f, metres = 1f, t0 = t)

        val ribbon = TrajectoryRibbon.fromTrail(r.worldPoints.value)
        assertTrue(
            "the blind bridge must reach the renderer as its own colour",
            ribbon.rgba.any { it == TrajectoryRibbon.BRIDGE },
        )
        // And the rest of the walk is still the gradient, not a sea of red.
        assertTrue(ribbon.rgba.count { it != TrajectoryRibbon.BRIDGE && it != TrajectoryRibbon.UNTRACKED } >= 2)
    }

    @Test
    fun `clear resets the jumped metres with everything else`() {
        val r = TrajectoryTrailRecorder()
        var t = walk(r, from = 0f, metres = 1f, t0 = 1_000_000_000L)
        r.onPose(x = 5f, y = 1.45f, z = 0f, tracking = true, tMonoNs = t + 33_000_000L)
        assertTrue(r.totalJumpM.value > 0f)
        r.clear()
        assertEquals(0f, r.totalJumpM.value)
        assertEquals(0f, r.totalPathM.value)
    }
}
