package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** ROUND 5.3 item 18: the walk-so-far trail — decimation, ring behaviour and the fit. */
class TrajectoryTrailTest {

    @Test
    fun `standing still does not fill the trail`() {
        val trail = TrajectoryTrail(minSpacingM = 0.15f)
        assertTrue(trail.add(0f, 0f, true))
        repeat(100) { assertFalse(trail.add(0.01f, 0.01f, true)) }
        assertEquals(1, trail.size)
    }

    @Test
    fun `walking keeps one point per spacing step`() {
        val trail = TrajectoryTrail(minSpacingM = 0.5f)
        // 5 m of walk in 10 cm steps: 50 offers, ~10 kept.
        var x = 0f
        repeat(50) {
            x += 0.1f
            trail.add(x, 0f, true)
        }
        // 10 spacing steps over 5 m — asserted as a range because `x += 0.1f`
        // accumulates float error and whether the very last offer clears the
        // threshold is a rounding coin-flip, not behaviour worth pinning.
        assertTrue("expected ~10 kept points, got ${trail.size}", trail.size in 9..11)
    }

    @Test
    fun `the ring drops the oldest points rather than growing forever`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f, capacity = 10)
        var x = 0f
        repeat(100) {
            x += 0.2f
            trail.add(x, 0f, true)
        }
        assertEquals(10, trail.size)
        // The kept window is the most RECENT stretch — where you are now matters
        // more than where you started.
        assertTrue(trail.snapshot().first().x > 15f)
    }

    @Test
    fun `non-finite poses are refused`() {
        val trail = TrajectoryTrail()
        assertFalse(trail.add(Float.NaN, 0f, true))
        assertFalse(trail.add(0f, Float.POSITIVE_INFINITY, true))
        assertEquals(0, trail.size)
    }

    @Test
    fun `tracking quality travels with each point`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f)
        trail.add(0f, 0f, true)
        trail.add(1f, 0f, false)
        assertEquals(listOf(true, false), trail.snapshot().map { it.tracking })
        assertEquals(listOf(true, false), trail.normalized().map { it.tracking })
    }

    @Test
    fun `the fit preserves aspect and stays inside the padded box`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f)
        // An L: 10 m east, then 2 m north.
        var x = 0f
        while (x < 10f) { trail.add(x, 0f, true); x += 0.5f }
        var z = 0f
        while (z < 2f) { trail.add(10f, z, true); z += 0.5f }

        val norm = trail.normalized(paddingFraction = 0.1f)
        assertTrue(norm.all { it.x in 0.09f..0.91f && it.y in 0.09f..0.91f })

        // The long axis spans the full usable width; the short axis spans the same
        // FRACTION of it that the walk did in the world (1.5 m of 9.5 m), NOT the
        // full height — that is what aspect-preserving means, and it is why a
        // corridor does not render as a square.
        val pts = trail.snapshot()
        val worldSpanX = pts.maxOf { it.x } - pts.minOf { it.x }
        val worldSpanZ = pts.maxOf { it.z } - pts.minOf { it.z }
        val spanX = norm.maxOf { it.x } - norm.minOf { it.x }
        val spanY = norm.maxOf { it.y } - norm.minOf { it.y }
        assertEquals(0.8f, spanX, 0.02f)
        assertEquals(0.8f * (worldSpanZ / worldSpanX), spanY, 0.02f)
        assertTrue("the short axis must not be stretched to fill the box", spanY < spanX / 3f)
    }

    @Test
    fun `screen y is flipped so north is up`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f)
        trail.add(0f, 0f, true) // south end
        trail.add(0f, 5f, true) // north end
        val norm = trail.normalized()
        assertTrue("the later, more northerly point must sit HIGHER on screen", norm[1].y < norm[0].y)
    }

    @Test
    fun `a single point maps to the centre instead of dividing by zero`() {
        val trail = TrajectoryTrail()
        trail.add(3f, 4f, true)
        val norm = trail.normalized()
        assertEquals(1, norm.size)
        assertEquals(0.5f, norm[0].x, 1e-6f)
        assertEquals(0.5f, norm[0].y, 1e-6f)
    }

    @Test
    fun `an empty trail normalizes to nothing`() {
        assertTrue(TrajectoryTrail().normalized().isEmpty())
        assertEquals(0f, TrajectoryTrail().pathLengthM(), 0f)
    }

    @Test
    fun `path length measures the walk, not the straight line home`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f)
        // Out 5 m and back: 10 m walked, 0 m displaced.
        var x = 0f
        while (x < 5f) { trail.add(x, 0f, true); x += 0.5f }
        while (x > 0f) { trail.add(x, 0f, true); x -= 0.5f }
        assertEquals(9.5f, trail.pathLengthM(), 0.2f)
    }

    @Test
    fun `clear resets everything`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f)
        trail.add(0f, 0f, true)
        trail.add(1f, 1f, true)
        trail.clear()
        assertEquals(0, trail.size)
        assertTrue(trail.normalized().isEmpty())
    }
}
