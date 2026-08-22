package com.lidarscan.app.ui.profile

import com.lidarscan.core.render.PointCountFormat
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 28 item 165 — **the "This phone" table's values.**
 *
 * §A.8's finding F6 is the review's only ✅ and the pattern §C.4 generalised
 * into `ScanRow`, so the table is kept verbatim and its strings are pinned
 * rather than eyeballed. Two of the five rows are new work: `SCANS` absorbs the
 * point total and `GEOREF` the georeferenced count, both from the Projects
 * header item 151 deleted.
 */
class ProfileFactsTest {

    @Test
    fun `app names the product and both version numbers`() {
        assertEquals("Ollidar 0.9.13 (913)", ProfileFacts.appLine("Ollidar", "0.9.13", 913))
    }

    // ── DEVICE: two old rows, one new row ───────────────────────────────────

    @Test
    fun `device is the phone and the android release on one line`() {
        assertEquals("Pixel 8 Pro · Android 16", ProfileFacts.deviceLine("Pixel 8 Pro", "16"))
    }

    /**
     * `Build.VERSION.RELEASE` is declared nullable and this app reads it with
     * `orEmpty()`, so the blank case is reachable on a real device rather than
     * theoretical. It must not render "Pixel 8 Pro · Android ".
     */
    @Test
    fun `a missing android release drops the clause rather than trailing it`() {
        assertEquals("Pixel 8 Pro", ProfileFacts.deviceLine("Pixel 8 Pro", ""))
    }

    @Test
    fun `an unknown model still says something`() {
        assertEquals("Unknown phone · Android 16", ProfileFacts.deviceLine("   ", "16"))
    }

    // ── SCANS: the Projects header's two figures ────────────────────────────

    /**
     * The owner's own library: 66 scans, 8.1 M points. The point half goes
     * through [PointCountFormat] because item 150 established there is exactly
     * one point formatter in this app — the Review header read `0.0 M pts` for
     * a 46,500-point scan precisely because a second one existed.
     */
    @Test
    fun `scans carries the count and the point total`() {
        assertEquals("66 · 8.1 M points", ProfileFacts.scansLine(66, 8_100_000L))
    }

    @Test
    fun `the point total is the one formatter, not a local variant`() {
        val line = ProfileFacts.scansLine(3, 46_500L)
        assertTrue(line, line.endsWith(PointCountFormat.longForm(46_500L)))
    }

    @Test
    fun `an empty library says so instead of showing zeroes`() {
        assertEquals("None yet", ProfileFacts.scansLine(0, 0L))
    }

    // ── GEOREF: the header's third figure ───────────────────────────────────

    /**
     * A fraction rather than a bare count: 65 georeferenced scans is excellent
     * out of 66 and alarming out of 400, and the row has no room to say which.
     */
    @Test
    fun `georef is stated against the total`() {
        assertEquals("65 of 66", ProfileFacts.georeferencedLine(65, 66))
    }

    @Test
    fun `no georeferenced scans reads as none of the total`() {
        assertEquals("None of 66", ProfileFacts.georeferencedLine(0, 66))
        assertEquals("None yet", ProfileFacts.georeferencedLine(0, 0))
    }
}
