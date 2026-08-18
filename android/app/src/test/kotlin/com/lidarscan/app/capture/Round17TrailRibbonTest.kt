package com.lidarscan.app.capture

import com.lidarscan.core.capture.TrajectoryRibbon
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 17 item 65 — **"my path not show in the point cloud. its just a 2d map
 * of my path."**
 *
 * ROUND 16 built the whole in-cloud ribbon: the metric flow, its accessor, the
 * `:core` vertex builder, the Filament material, the `LINE_STRIP` draw in the
 * same 3D pass as the points, and the Review-side file reader. What it never
 * did was publish the metric flow as the walk happened — `_worldPoints` was
 * written by `setCapacity()` and `clear()` and by nothing else. The bird's-eye
 * tile, published two lines away in the same method, kept updating. So the
 * owner got one view of his path and it was the 2D one, which is exactly what
 * he reported.
 *
 * This test walks a recorder and asserts that both views move together. It runs
 * on a bare JVM because [TrajectoryTrailRecorder.onPose] was split out of
 * `onFrame` for exactly that purpose — the previous shape took a
 * `com.google.ar.core.Frame`, which is why the line that was missing had no
 * test to be missing from.
 */
class Round17TrailRibbonTest {

    /** A four-metre straight walk at chest height, sampled every 20 cm. */
    private fun walk(r: TrajectoryTrailRecorder, metres: Float = 4.0f) {
        var d = 0.0f
        while (d <= metres) {
            r.onPose(x = d, y = 1.45f, z = 0.0f, tracking = true)
            d += 0.2f
        }
    }

    @Test
    fun `the metric path is published as the walk happens, not only when a preset changes`() {
        val r = TrajectoryTrailRecorder()
        assertTrue("nothing walked yet", r.worldPoints.value.isEmpty())

        walk(r)

        // THE ASSERTION. Before this round this list was empty here, for the
        // whole of every capture.
        assertTrue(
            "the 3D ribbon needs at least two vertices; got ${r.worldPoints.value.size}",
            r.worldPoints.value.size >= 2,
        )
        // And the two views agree, because they are published from one instant.
        assertEquals(r.points.value.size, r.worldPoints.value.size)
    }

    @Test
    fun `the published path builds a drawable ribbon with the height carried`() {
        val r = TrajectoryTrailRecorder()
        walk(r)

        val ribbon = TrajectoryRibbon.fromTrail(r.worldPoints.value)
        assertTrue("a LINE_STRIP under two vertices draws nothing", ribbon.count >= 2)
        assertEquals(ribbon.count * 3, ribbon.xyz.size)
        assertEquals(ribbon.count, ribbon.rgba.size)

        // The walk was at 1.45 m, so the ribbon threads the cloud at chest
        // height rather than lying on the floor — the reason ROUND 16 added
        // `y` to TrajectoryTrail.Point in the first place.
        for (i in 0 until ribbon.count) {
            assertEquals(1.45f, ribbon.xyz[i * 3 + 1], 1e-4f)
        }
        // ...and it really is a walk along +x, not a point.
        val firstX = ribbon.xyz[0]
        val lastX = ribbon.xyz[(ribbon.count - 1) * 3]
        assertTrue("expected ~4 m of travel, got ${lastX - firstX}", lastX - firstX > 3.0f)
    }

    @Test
    fun `standing still publishes nothing, so a stationary operator costs no redraws`() {
        val r = TrajectoryTrailRecorder()
        r.onPose(0f, 1.45f, 0f, tracking = true)
        val afterFirst = r.worldPoints.value
        repeat(50) { r.onPose(0.001f * it, 1.45f, 0f, tracking = true) }
        assertEquals("sub-spacing jitter must not republish", afterFirst, r.worldPoints.value)
    }

    @Test
    fun `clear empties both views, so one walk is one trail`() {
        val r = TrajectoryTrailRecorder()
        walk(r)
        assertTrue(r.worldPoints.value.isNotEmpty())
        r.clear()
        assertTrue(r.worldPoints.value.isEmpty())
        assertTrue(r.points.value.isEmpty())
        assertEquals(0f, r.totalPathM.value)
    }

    @Test
    fun `a lost-tracking stretch survives into the ribbon as its own colour`() {
        val r = TrajectoryTrailRecorder()
        var d = 0.0f
        while (d <= 2.0f) {
            r.onPose(d, 1.45f, 0f, tracking = d < 1.0f)
            d += 0.2f
        }
        val ribbon = TrajectoryRibbon.fromTrail(r.worldPoints.value)
        val untracked = ribbon.rgba.count { it == TrajectoryRibbon.UNTRACKED }
        assertTrue("the red lost-tracking segment must reach the renderer", untracked > 0)
    }
}
