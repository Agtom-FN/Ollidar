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

    /**
     * ROUND 10 (owner item 37) — **this test used to assert the bug.**
     *
     * It was called "screen y is flipped so north is up" and it checked that
     * a point at larger world **+Z** sat HIGHER on screen. That is precisely
     * the mirror: with screen-right = +X and screen-up = +Z, the out-of-screen
     * normal is `X x Z = -Y`, i.e. the tile is a view from UNDER the floor.
     * The name was the give-away — the recorder's own comment calls ARCore's
     * +Z "south-ish", so "+Z at the top" is a south-up map with east still on
     * the right, which no map is.
     *
     * ARCore's forward is **-Z**. Walking forward must go UP the tile.
     */
    @Test
    fun `walking forward goes up the tile, because ARCore forward is minus Z`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f)
        trail.add(0f, 0f, true) // start
        trail.add(0f, -5f, true) // 5 m forward, i.e. -Z
        val norm = trail.normalized()
        assertTrue(
            "walking forward (-Z) must move UP the tile — canvas y decreases upward",
            norm[1].y < norm[0].y,
        )
    }

    /**
     * ROUND 10 (owner item 37) — **the chirality test the trail never had.**
     *
     * Every previous assertion on this class measured a sign-blind quantity
     * (spans, aspect, centring) or asserted the mirror outright, which is
     * exactly the gap ROUND 9 found on the POINTS side: a mirrored L has the
     * same extent, the same aspect and the same point count as the real one.
     *
     * The stimulus is the owner's acceptance test verbatim: *"walk an L-shaped
     * path, the on-screen trail turns the same way the operator turned."*
     *
     * ARCore's world frame is right-handed with +Y up and the camera looking
     * along -Z, so for an operator walking forward:
     *
     *     forward = -Z,  up = +Y,  LEFT = up x forward = Y x (-Z) = -X
     *
     * So an L that goes forward and then turns LEFT ends at negative X. On
     * screen that has to be: up the tile, then to the LEFT of the tile.
     */
    @Test
    fun `an L-shaped walk turning LEFT renders a trail turning LEFT`() {
        val trail = TrajectoryTrail(minSpacingM = 0.1f)
        // Leg 1: 4 m forward along -Z from the origin.
        var z = 0f
        while (z > -4f) {
            trail.add(0f, z, true)
            z -= 0.5f
        }
        // Leg 2: turn LEFT (which is -X) and walk 3 m.
        var x = 0f
        while (x > -3f) {
            trail.add(x, -4f, true)
            x -= 0.5f
        }

        val norm = trail.normalized()
        val start = norm.first()
        val corner = norm[8] // the corner itself: (0, -4), first point of leg 2
        val end = norm.last()

        // Leg 1 goes UP the tile (canvas y decreases upward) and does not
        // wander sideways.
        assertTrue("leg 1 (forward) must go up the tile", corner.y < start.y)
        assertEquals("leg 1 must not drift sideways", start.x, corner.x, 1e-4f)

        // Leg 2 goes LEFT across the tile, and stays at the same height.
        assertTrue("leg 2 (a LEFT turn) must go left on screen", end.x < corner.x)
        assertEquals("leg 2 must not change height", corner.y, end.y, 1e-4f)

        // ...and the falsifiable half, stated so a future regression cannot be
        // read as an improvement: under the pre-ROUND-10 projection
        // (`y = 1 - nz`) leg 1 would run DOWN the tile. Reintroduce that flip
        // and the first assertion above fails, which is the whole point.
        assertTrue(
            "sanity: the trail must actually have two legs",
            kotlin.math.abs(end.x - start.x) > 0.1f && kotlin.math.abs(end.y - start.y) > 0.1f,
        )
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
