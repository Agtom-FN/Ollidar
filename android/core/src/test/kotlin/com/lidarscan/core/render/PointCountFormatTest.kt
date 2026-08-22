package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Test

/** ROUND 28 item 150 — the formatter that used to print `0.0 M pts`. */
class PointCountFormatTest {

    /**
     * The regression, stated as the owner's own number. 46,500 points on the
     * Review header rendered `0.0 M pts` while the Projects card for the same
     * scan rendered `46.5 K pts`.
     */
    @Test
    fun `the owner's scan no longer reads zero`() {
        assertEquals("46.5 K pts", PointCountFormat.compactPts(46_500L))
        assertEquals("52.0 K pts", PointCountFormat.compactPts(52_041L))
    }

    @Test
    fun `it switches unit at exact powers of ten`() {
        assertEquals("999", PointCountFormat.compact(999L))
        assertEquals("1.0 K", PointCountFormat.compact(1_000L))
        assertEquals("999.9 K", PointCountFormat.compact(999_949L))
        assertEquals("1.0 M", PointCountFormat.compact(1_000_000L))
    }

    @Test
    fun `zero and one are not special cases`() {
        assertEquals("0", PointCountFormat.compact(0L))
        assertEquals("1", PointCountFormat.compact(1L))
        assertEquals("1 pts", PointCountFormat.compactPts(1L))
    }

    @Test
    fun `the long form is for a library, the compact form for a row`() {
        assertEquals("8.1 M points", PointCountFormat.longForm(8_100_000L))
        assertEquals("842 points", PointCountFormat.longForm(842L))
        assertEquals("no points yet", PointCountFormat.longForm(0L))
    }

    /**
     * ROUND 28 item 162: an empty scan is a *result*, with its own words, shown
     * in the list rather than hidden behind a truncated header clause.
     */
    @Test
    fun `an empty scan says so instead of printing a zero`() {
        assertEquals("Empty — no points", PointCountFormat.rowClause(0L))
        assertEquals("Empty — no points", PointCountFormat.rowClause(-1L))
        assertEquals("No capture", PointCountFormat.rowClause(null))
        assertEquals("46.5 K pts", PointCountFormat.rowClause(46_500L))
    }

    /** Locale-independent: a comma decimal separator would break every assertion above. */
    @Test
    fun `it does not follow the device locale`() {
        val previous = java.util.Locale.getDefault()
        try {
            java.util.Locale.setDefault(java.util.Locale.GERMANY)
            assertEquals("46.5 K pts", PointCountFormat.compactPts(46_500L))
            assertEquals("8.1 M points", PointCountFormat.longForm(8_100_000L))
        } finally {
            java.util.Locale.setDefault(previous)
        }
    }
}
