package com.lidarscan.core.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 25 item 118, **owner amendment** — the rate limiter, against a fake
 * clock.
 *
 * The properties asserted here are exactly the ones `ArSessionGateTest`
 * asserts about the round-22 refusal limiter, because this IS that limiter
 * with a category key: the first line passes, the rest of the window is
 * suppressed, the suppressed count is reported on the next line that gets
 * through, categories do not starve one another, and the window reopens.
 *
 * A fake clock rather than sleeping: a test that has to take four seconds to
 * prove a one-second window is a test people delete.
 */
class ConnectionDebugRateLimiterTest {

    private var now = 10_000L
    private val limiter = ConnectionDebugRateLimiter(intervalMillis = 1_000L, clockMillis = { now })

    @Test
    fun `the first line in a category always passes`() {
        val admission = limiter.admit("wizard-poll")
        assertNotNull(admission)
        assertEquals(0, admission!!.suppressed)
        assertEquals("", admission.suffix)
    }

    @Test
    fun `subsequent lines inside the window are suppressed`() {
        assertNotNull(limiter.admit("wizard-poll"))
        now += 100
        assertNull(limiter.admit("wizard-poll"))
        now += 800
        assertNull(limiter.admit("wizard-poll"))
        assertEquals(2, limiter.suppressedSoFar("wizard-poll"))
    }

    @Test
    fun `the window reopens and the next line carries the suppressed count`() {
        assertNotNull(limiter.admit("wizard-poll"))
        now += 100
        limiter.admit("wizard-poll")
        limiter.admit("wizard-poll")
        limiter.admit("wizard-poll")

        now += 1_000
        val admission = limiter.admit("wizard-poll")
        assertNotNull("the window must reopen once the interval has passed", admission)
        assertEquals(3, admission!!.suppressed)
        assertEquals(" (+3 more since the last line)", admission.suffix)
        // Handing the count to the caller clears it: the NEXT line must not
        // report the same three again.
        assertEquals(0, limiter.suppressedSoFar("wizard-poll"))
    }

    @Test
    fun `the suffix text is the round-22 wording verbatim`() {
        // Matching `ArSessionGate.noteRefusal` is the point of this class. A
        // person who has read one of those lines can read this one.
        assertNotNull(limiter.admit("discovery"))
        now += 10
        limiter.admit("discovery")
        now += 2_000
        assertEquals(
            " (+1 more since the last line)",
            limiter.admit("discovery")!!.suffix,
        )
    }

    @Test
    fun `categories are independent`() {
        // A wizard polling at 1 Hz must not silently delete every auto-detect
        // sweep — which is what one shared window would do.
        assertNotNull(limiter.admit("wizard-poll"))
        assertNotNull(limiter.admit("auto-detect"))
        assertNotNull(limiter.admit("discovery"))

        now += 100
        assertNull(limiter.admit("wizard-poll"))
        assertNull(limiter.admit("auto-detect"))

        now += 5_000
        assertEquals(1, limiter.admit("wizard-poll")!!.suppressed)
        assertEquals(1, limiter.admit("auto-detect")!!.suppressed)
        // discovery was never suppressed, so its count is still zero.
        assertEquals(0, limiter.admit("discovery")!!.suppressed)
    }

    @Test
    fun `line appends the suffix for a single-line caller`() {
        assertEquals("listening on 56201", limiter.line("discovery", "listening on 56201"))
        now += 10
        assertNull(limiter.line("discovery", "heard 430B"))
        now += 1_500
        assertEquals(
            "heard 430B (+1 more since the last line)",
            limiter.line("discovery", "heard 430B"),
        )
    }

    @Test
    fun `reopen lets a fresh story speak at once`() {
        // `ArSessionGate.claim` does the same thing for the same reason: the
        // wizard being re-entered, or something being plugged in, is a new
        // story and is entitled to a line rather than the tail of the old
        // window.
        assertNotNull(limiter.admit("wizard-poll"))
        now += 10
        assertNull(limiter.admit("wizard-poll"))
        limiter.reopen("wizard-poll")
        val admission = limiter.admit("wizard-poll")
        assertNotNull(admission)
        // Reopening discards nothing: the one suppressed line is still counted.
        assertEquals(1, admission!!.suppressed)
    }

    @Test
    fun `reopenAll reopens every category`() {
        limiter.admit("wizard-poll")
        limiter.admit("auto-detect")
        now += 10
        assertNull(limiter.admit("wizard-poll"))
        assertNull(limiter.admit("auto-detect"))
        limiter.reopenAll()
        assertNotNull(limiter.admit("wizard-poll"))
        assertNotNull(limiter.admit("auto-detect"))
    }

    @Test
    fun `a long session emits at most one line per interval per category`() {
        // The property the 512 KB capture.log actually depends on: 600 polls
        // at 100 ms is a minute of wizard, and it must not be 600 blocks.
        var admitted = 0
        repeat(600) {
            if (limiter.admit(ConnectionDebugRateLimiter.CATEGORY_WIZARD_POLL) != null) admitted++
            now += 100
        }
        assertTrue("expected roughly one per second over 60s, got $admitted", admitted in 55..61)
    }

    @Test
    fun `the default interval matches the round-22 gate`() {
        assertEquals(1_000L, ConnectionDebugRateLimiter.DEFAULT_INTERVAL_MILLIS)
    }
}
