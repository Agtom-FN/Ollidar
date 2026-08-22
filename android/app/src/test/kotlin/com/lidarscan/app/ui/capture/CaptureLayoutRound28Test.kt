package com.lidarscan.app.ui.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 28 item 158 — the condition that reserved half of the owner's first
 * screen for a picture that did not exist.
 */
class CaptureLayoutRound28Test {

    /**
     * The regression, stated as the owner's own situation: a COIN-D6 on the
     * cable, the Scan tab open, nothing recording. Round 27 gave this state a
     * guaranteed 60 % viewport because `connected` was true.
     */
    @Test
    fun `connected and idle does not earn the screen`() {
        assertFalse(CaptureLayout.viewportEarnsTheScreen(recording = false, isReplay = false))
        assertEquals(0f, CaptureLayout.minViewportFraction(recording = false, isReplay = false), 0f)
    }

    /** Round 8's rule, applied to the state it was written about. */
    @Test
    fun `recording keeps the sixty per cent`() {
        assertTrue(CaptureLayout.viewportEarnsTheScreen(recording = true, isReplay = false))
        assertEquals(
            CaptureLayout.MIN_VIEWPORT_FRACTION,
            CaptureLayout.minViewportFraction(recording = true, isReplay = false),
            0f,
        )
    }

    /** A replay session's entire content IS the preview; there is nothing else on it. */
    @Test
    fun `replay earns the screen without recording`() {
        assertTrue(CaptureLayout.viewportEarnsTheScreen(recording = false, isReplay = true))
        assertEquals(
            CaptureLayout.MIN_VIEWPORT_FRACTION,
            CaptureLayout.minViewportFraction(recording = false, isReplay = true),
            0f,
        )
    }

    /**
     * `useCompactChrome` is a DIFFERENT question and keeps its own answer:
     * it decides whether the connect flow needs the room, which genuinely is
     * about whether a sensor is attached. Item 158 does not merge the two —
     * conflating them is what produced the defect.
     */
    @Test
    fun `the chrome question is still about the cable`() {
        assertTrue(CaptureLayout.useCompactChrome(connected = true, manualEntryOpen = false))
        assertFalse(CaptureLayout.useCompactChrome(connected = false, manualEntryOpen = false))
        assertFalse(CaptureLayout.useCompactChrome(connected = true, manualEntryOpen = true))
    }
}
