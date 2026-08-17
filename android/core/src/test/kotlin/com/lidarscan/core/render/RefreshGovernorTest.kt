package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 5.3 item 17: the live-view refresh ceiling is the *hardware's*, and a
 * phone that cannot sustain it eases down instead of hitching (or crashing).
 */
class RefreshGovernorTest {

    private val ms = 1_000_000L
    private val sustainNs = (RefreshGovernor.SUSTAINED_WINDOW_MS + 100) * ms

    @Test
    fun `options never offer more than the device can do`() {
        assertEquals(listOf(0, 60, 45, 30, 20, 15, 10), RefreshGovernor.optionsFor(60))
        assertEquals(listOf(0, 120, 90, 60, 45, 30, 20, 15, 10), RefreshGovernor.optionsFor(120))
        assertEquals(listOf(0, 90, 60, 45, 30, 20, 15, 10), RefreshGovernor.optionsFor(90))
        // A nonsense/unreported ceiling still yields a usable list rather than an
        // empty one that would render a control with no options.
        assertTrue(RefreshGovernor.optionsFor(0).size >= 2)
        assertEquals(0, RefreshGovernor.optionsFor(0).first())
    }

    @Test
    fun `a transient long frame never downshifts`() {
        val g = RefreshGovernor(deviceCeilingHz = 120)
        g.request(0)
        // One 40 ms frame against a 120 Hz target — a page upload, not a trend.
        assertNull(g.onFrameInterval(nowNs = 1_000 * ms, intervalNs = 40 * ms))
        // …then back to normal, which resets the overrun window.
        assertNull(g.onFrameInterval(nowNs = 1_040 * ms, intervalNs = 8 * ms))
        assertNull(g.onFrameInterval(nowNs = 5_000 * ms, intervalNs = 40 * ms))
        assertFalse(g.isDownshifted)
        assertNull(g.note())
    }

    @Test
    fun `a sustained overrun downshifts one notch and says so`() {
        val g = RefreshGovernor(deviceCeilingHz = 120)
        g.request(0)
        assertNull(g.onFrameInterval(nowNs = 0, intervalNs = 40 * ms))
        val next = g.onFrameInterval(nowNs = sustainNs, intervalNs = 40 * ms)
        assertEquals(90, next)
        assertEquals(90, g.effectiveHz)
        assertTrue(g.isDownshifted)
        val note = g.note()!!
        assertTrue(note.contains("90 fps"))
        // The note must say the recording is safe — that is the whole point of
        // item 17's "recording is NEVER throttled".
        assertTrue(note.contains("Recording is unaffected"))
    }

    @Test
    fun `it walks down a notch at a time rather than collapsing to the floor`() {
        val g = RefreshGovernor(deviceCeilingHz = 120)
        g.request(0)
        var now = 0L
        val seen = mutableListOf<Int>()
        repeat(40) {
            now += 500 * ms
            g.onFrameInterval(now, intervalNs = 200 * ms)?.let { seen.add(it) }
        }
        assertEquals(listOf(90, 60, 45, 30, 20, 15, 10), seen)
        // …and it stops at the floor rather than heading for zero.
        assertEquals(RefreshGovernor.FLOOR_HZ, g.effectiveHz)
    }

    @Test
    fun `it never raises the cap on its own`() {
        val g = RefreshGovernor(deviceCeilingHz = 120)
        g.request(0)
        var now = 0L
        now += sustainNs
        g.onFrameInterval(0, 40 * ms)
        assertEquals(90, g.onFrameInterval(now, 40 * ms))

        // Now the phone is comfortable for a long while: still 90, not back to 120.
        repeat(100) {
            now += 100 * ms
            assertNull(g.onFrameInterval(now, 4 * ms))
        }
        assertEquals(90, g.effectiveHz)
    }

    @Test
    fun `an explicit request wins and clears the downshift`() {
        val g = RefreshGovernor(deviceCeilingHz = 120)
        g.request(0)
        g.onFrameInterval(0, 40 * ms)
        g.onFrameInterval(sustainNs, 40 * ms)
        assertTrue(g.isDownshifted)

        g.request(60)
        assertEquals(60, g.effectiveHz)
        assertFalse(g.isDownshifted)
        assertNull(g.note())
    }

    /**
     * ROUND 5 AUDIT: the app-layer recovery path (re-selecting the SAME option
     * the operator already had chosen, after an auto-downshift eased it below
     * that) depends on `request()` clearing the downshift even when the
     * incoming value is IDENTICAL to what was already requested — this class
     * has no "no-op if unchanged" guard, unlike the app-layer callers that
     * wrap it (`PointCloudRenderer.setMaxRefreshHz`, `CaptureViewModel
     * .setRefreshHz`'s `MutableStateFlow`), which is exactly why those needed
     * their own audit fix (see `PointCloudRenderer.setMaxRefreshHz`'s doc).
     * Pinning the class-level contract here so a future guard added to THIS
     * class would fail loudly instead of silently reintroducing that bug.
     */
    @Test
    fun `re-requesting the SAME already-selected rate still clears an active downshift`() {
        val g = RefreshGovernor(deviceCeilingHz = 120)
        g.request(0)
        g.onFrameInterval(0, 40 * ms)
        g.onFrameInterval(sustainNs, 40 * ms)
        assertTrue(g.isDownshifted)
        assertEquals(90, g.effectiveHz)

        // Same request as the very first one — not a new value. `effectiveHz`
        // itself goes back to `0` (request()'s own "Max" sentinel, same as a
        // fresh instance that was never downshifted), and `isDownshifted`
        // resolves it against the device ceiling to confirm the cap is
        // genuinely back at 120, not just that the sentinel changed.
        g.request(0)
        assertEquals(0, g.effectiveHz)
        assertFalse(g.isDownshifted)
        assertNull(g.note())
    }

    @Test
    fun `a target already at the floor is left alone`() {
        val g = RefreshGovernor(deviceCeilingHz = 60)
        g.request(RefreshGovernor.FLOOR_HZ)
        assertNull(g.onFrameInterval(0, 500 * ms))
        assertNull(g.onFrameInterval(sustainNs, 500 * ms))
        assertEquals(RefreshGovernor.FLOOR_HZ, g.effectiveHz)
    }

    @Test
    fun `a 60 Hz phone asking for max starts its ladder at 45`() {
        val g = RefreshGovernor(deviceCeilingHz = 60)
        g.request(0)
        g.onFrameInterval(0, 40 * ms)
        assertEquals(45, g.onFrameInterval(sustainNs, 40 * ms))
    }
}
