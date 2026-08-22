package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** ROUND 28 item 158 — the pre-flight that replaced six controls with three rows. */
class ScanReadinessTest {

    private fun good(title: String) = ScanReadiness.Row(title, ScanReadiness.State.GOOD, "Ready")
    private fun warn(title: String) =
        ScanReadiness.Row(title, ScanReadiness.State.WARN, "Not set", "Re-zero before scanning.", "Re-zero")
    private fun bad(title: String) =
        ScanReadiness.Row(title, ScanReadiness.State.BAD, "Not found", "Plug it in, then retry.", "Retry")

    private val allGood = listOf(good("Sensor"), good("Mount"), good("Tracking"))

    @Test
    fun `three good rows start`() {
        assertTrue(ScanReadiness.canStart(allGood))
        assertNull(ScanReadiness.blocker(allGood))
    }

    /**
     * Round 12's rule, applied to the button: an unset mount is a scan that
     * will work and be worse, not a refusal. The pushbroom runs on the CAD
     * nominal, which is a real scan with a known error.
     */
    @Test
    fun `a warning does not disable the FAB`() {
        val rows = listOf(good("Sensor"), warn("Mount"), good("Tracking"))
        assertTrue(ScanReadiness.canStart(rows))
        assertNull(ScanReadiness.blocker(rows))
    }

    @Test
    fun `nothing on the cable means nothing to record`() {
        val rows = listOf(bad("Sensor"), good("Mount"), good("Tracking"))
        assertFalse(ScanReadiness.canStart(rows))
        assertEquals("Sensor", ScanReadiness.blocker(rows)?.title)
    }

    /**
     * §D.1: the offending row is the ONLY thing in bad colour. With two
     * failures the first wins — a screen with two red rows has stopped ranking
     * its own problems.
     */
    @Test
    fun `only the first failure is the blocker`() {
        val rows = listOf(good("Sensor"), bad("Mount"), bad("Tracking"))
        assertEquals("Mount", ScanReadiness.blocker(rows)?.title)
    }

    // ── the status line ─────────────────────────────────────────────────────

    @Test
    fun `the status line names the sensor when all is well`() {
        assertEquals("COIN-D6 · Ready", ScanReadiness.statusLine("COIN-D6", allGood))
    }

    /** A blocked screen says what is wrong, not what is plugged in. */
    @Test
    fun `the status line yields to the blocker`() {
        val rows = listOf(bad("Sensor"), good("Mount"), good("Tracking"))
        assertEquals("Not found", ScanReadiness.statusLine("COIN-D6", rows))
    }

    @Test
    fun `an unnamed sensor still reads as ready`() {
        assertEquals("Ready", ScanReadiness.statusLine(null, allGood))
        assertEquals("Ready", ScanReadiness.statusLine("  ", allGood))
    }

    // ── the wording law ─────────────────────────────────────────────────────

    /**
     * A row's detail IS its fix, and §C.6 caps a fix at six words. This is the
     * property that made three rows able to replace the loose control row.
     */
    @Test
    fun `every fix is six words or fewer`() {
        listOf(warn("Mount"), bad("Sensor")).forEach { row ->
            val detail = row.detail!!
            val words = detail.trim().split(Regex("\\s+")).size
            assertTrue("\"$detail\" is $words words", words <= 6)
        }
    }

    @Test
    fun `a row must name what it is about`() {
        var threw = false
        try {
            ScanReadiness.Row("", ScanReadiness.State.GOOD, "Ready")
        } catch (e: IllegalArgumentException) {
            threw = true
        }
        assertTrue("a blank title must not be constructible", threw)
    }

    @Test
    fun `an empty pre-flight cannot block`() {
        // Degenerate but reachable — a replay session has no sensor, no mount
        // and no tracking to check. It must not be refused a start.
        assertTrue(ScanReadiness.canStart(emptyList()))
        assertNull(ScanReadiness.blocker(emptyList()))
    }
}
