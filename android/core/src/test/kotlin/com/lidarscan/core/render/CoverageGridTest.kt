package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 11 item 42 — the density binning behind coverage colouring.
 *
 * Three properties matter and none of them is "it looks nice": the lattice must
 * be anchored so the answer does not depend on arrival order, a fully-covered
 * point must come out looking exactly as it does in the shipped
 * grayscale-intensity view, and a thin point must be visible even though there
 * are, by definition, very few of them.
 */
class CoverageGridTest {

    @Test
    fun `the lattice is anchored at the world origin, not at the cloud`() {
        val a = CoverageGrid()
        val b = CoverageGrid()
        // Same three coordinates, opposite insertion order, plus a distant point
        // in one of them that would move a bounds-relative lattice.
        a.add(0.10f, 0.10f, 0.10f)
        a.add(0.20f, 0.20f, 0.20f)
        a.add(500f, 0f, 0f)
        b.add(500f, 0f, 0f)
        b.add(0.20f, 0.20f, 0.20f)
        b.add(0.10f, 0.10f, 0.10f)
        assertEquals(a.countAt(0.15f, 0.15f, 0.15f), b.countAt(0.15f, 0.15f, 0.15f))
        assertEquals(2, a.countAt(0.15f, 0.15f, 0.15f))
        assertEquals(a.cellCount, b.cellCount)
    }

    @Test
    fun `negative coordinates land in their own cells`() {
        val g = CoverageGrid()
        g.add(-0.10f, 0f, 0f)
        g.add(0.10f, 0f, 0f)
        // floor(-0.1 / 0.25) = -1 and floor(0.1 / 0.25) = 0 — two cells, not one.
        assertEquals(2, g.cellCount)
        assertEquals(1, g.countAt(-0.10f, 0f, 0f))
    }

    @Test
    fun `coverage ramps linearly between the thin and dense thresholds`() {
        val g = CoverageGrid()
        assertEquals(0f, g.coverageOf(0), 0f)
        assertEquals(0f, g.coverageOf(CoverageGrid.DEFAULT_THIN), 0f)
        assertEquals(1f, g.coverageOf(CoverageGrid.DEFAULT_DENSE), 0f)
        assertEquals(1f, g.coverageOf(10_000), 0f)
        val mid = (CoverageGrid.DEFAULT_THIN + CoverageGrid.DEFAULT_DENSE) / 2
        assertEquals(0.5f, g.coverageOf(mid), 0.02f)
        assertEquals(CoverageLevel.THIN, g.levelOf(1))
        assertEquals(CoverageLevel.PARTIAL, g.levelOf(mid))
        assertEquals(CoverageLevel.DENSE, g.levelOf(100))
    }

    @Test
    fun `a fully covered point is drawn exactly as the grayscale default draws it`() {
        // The promise in item 42 that coverage "works with the intensity /
        // grayscale default": where the map is dense, coverage mode must be
        // indistinguishable from the mode the owner asked to be the default in
        // ROUND 10 item 39. Anything else would make it a mode nobody leaves on.
        val g = CoverageGrid()
        for (shade in intArrayOf(0, 1, 64, 128, 200, 255)) {
            val argb = g.tint(shade, coverage = 1f)
            assertEquals(shade, (argb shr 16) and 0xFF)
            assertEquals(shade, (argb shr 8) and 0xFF)
            assertEquals(shade, argb and 0xFF)
            assertEquals(0xFF, (argb ushr 24) and 0xFF)
        }
    }

    @Test
    fun `a thin point is amber and legible even when its own shade is nearly black`() {
        val g = CoverageGrid()
        val thin = g.tint(shade = 10, coverage = 0f)
        val r = (thin shr 16) and 0xFF
        val gr = (thin shr 8) and 0xFF
        val b = thin and 0xFF
        // Amber, not red: red is already the app's failure colour (the no-data
        // banner, tracking lost) and "thin here" is not a failure.
        assertTrue("r=$r g=$gr b=$b", r > gr && gr > b)
        // ...and bright enough to see against a near-black ground, which a
        // straight multiply by a shade of 10 would not have been.
        assertTrue("r=$r", r > 80)
        assertNotEquals(thin, g.tint(shade = 10, coverage = 1f))
    }

    @Test
    fun `the thin fraction answers am I nearly done`() {
        val g = CoverageGrid()
        // One thin cell, one dense one.
        g.add(0f, 0f, 0f)
        repeat(CoverageGrid.DEFAULT_DENSE + 5) { g.add(1f, 0f, 0f) }
        assertEquals(2, g.cellCount)
        assertEquals(0.5f, g.thinFraction(), 0.001f)
        assertEquals(0f, CoverageGrid().thinFraction(), 0f)
    }

    @Test
    fun `non-finite coordinates are ignored rather than corrupting a cell`() {
        val g = CoverageGrid()
        g.add(Float.NaN, 0f, 0f)
        g.add(0f, Float.POSITIVE_INFINITY, 0f)
        assertEquals(0, g.cellCount)
        assertEquals(0L, g.pointsCounted)
        g.add(1f, 1f, 1f)
        assertEquals(1L, g.pointsCounted)
        g.clear()
        assertEquals(0, g.cellCount)
    }
}
