package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 19 item 75 — the coverage compass: thin coverage as a direction.
 *
 * The fixtures paint sectors DIRECTLY (a wall is just many returns in one
 * azimuth band), because what is under test is the census and the verdicts,
 * not trigonometry the renderer feeds it with.
 */
class CoverageCompassTest {

    private fun compass() = CoverageCompass()

    /** Paint `n` returns into the middle of sector `s`, 2 m out. */
    private fun paint(c: CoverageCompass, sector: Int, n: Int) {
        val deg = (sector + 0.5) * 30.0
        val x = (2.0 * Math.cos(Math.toRadians(deg))).toFloat()
        val z = (2.0 * Math.sin(Math.toRadians(deg))).toFloat()
        repeat(n) { c.add(x, z) }
    }

    @Test
    fun `nothing is claimed before the evidence floor`() {
        val c = compass()
        c.setOperator(0f, 0f)
        paint(c, 0, 500)
        assertFalse(c.measurable())
        assertNull(c.largestThinArc())
        assertNull(c.adviceLine())
        assertTrue(c.sectorCoverage().all { it == 0f })
    }

    @Test
    fun `a wall of returns makes its sector covered and the empty side thin`() {
        val c = compass()
        c.setOperator(0f, 0f)
        // Sectors 0..5 well painted, 6..11 nearly empty.
        for (s in 0..5) paint(c, s, 2_000)
        for (s in 6..11) paint(c, s, 20)
        assertTrue(c.measurable())
        val arc = c.largestThinArc()
        assertNotNull(arc)
        assertEquals(6, arc!!.first)
        assertEquals(6, arc.second)
        val cov = c.sectorCoverage()
        assertTrue(cov[0] > 0.9f)
        assertTrue(cov[8] < CoverageCompass.DEFAULT_THIN_FRACTION)
    }

    @Test
    fun `the thin arc is circular — a run across the wrap is one run`() {
        val c = compass()
        c.setOperator(0f, 0f)
        for (s in 1..9) paint(c, s, 2_000)
        // 10, 11, 0 empty: a 3-sector run across the wrap.
        val arc = c.largestThinArc()
        assertNotNull(arc)
        assertEquals(10, arc!!.first)
        assertEquals(3, arc.second)
    }

    @Test
    fun `range gates keep the operator's own bracket out of the census`() {
        val c = compass()
        c.setOperator(0f, 0f)
        c.add(0.1f, 0.1f) // the rig itself
        c.add(50f, 0f) // another room entirely
        assertEquals(0L, c.pointsCounted)
    }

    @Test
    fun `advice names the side relative to the walk, and only with two thin sectors`() {
        val c = compass()
        // Walk toward +X, far enough to establish a heading, ENDING at the
        // origin — the paints below are all measured from there.
        c.setOperator(-1.0f, 0f)
        c.setOperator(0f, 0f)
        // Everything painted except the arc straight BEHIND the walk (-X):
        // sectors around 180 deg, i.e. 5 and 6.
        for (s in 0..11) {
            if (s == 5 || s == 6) paint(c, s, 10) else paint(c, s, 2_000)
        }
        val line = c.adviceLine()
        assertNotNull(line)
        assertTrue("got: $line", line!!.contains("behind you"))
        assertTrue(line.contains("walk past them before stopping"))
        // The owner correction on record: no environmental guessing.
        assertFalse(line.contains("light"))
    }

    @Test
    fun `a single thin sliver is not an instruction`() {
        val c = compass()
        c.setOperator(0f, 0f)
        for (s in 0..11) {
            if (s == 3) paint(c, s, 10) else paint(c, s, 2_000)
        }
        assertNull(c.adviceLine())
    }

    @Test
    fun `deterministic — the same points give the same verdicts`() {
        val a = compass()
        val b = compass()
        for (c in listOf(a, b)) {
            c.setOperator(0f, 0f)
            for (s in 0..7) paint(c, s, 1_500 + s)
        }
        assertEquals(a.pointsCounted, b.pointsCounted)
        assertEquals(a.largestThinArc(), b.largestThinArc())
        assertTrue(a.sectorCoverage().contentEquals(b.sectorCoverage()))
    }

    @Test
    fun `clear forgets everything including the heading`() {
        val c = compass()
        c.setOperator(0f, 0f)
        c.setOperator(1f, 0f)
        paint(c, 0, 20_000)
        c.clear()
        assertEquals(0L, c.pointsCounted)
        assertFalse(c.measurable())
        assertNull(c.adviceLine())
    }
}
