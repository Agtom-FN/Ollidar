package com.lidarscan.core.render

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 26 item 126 — the tap arbitration, as a truth table rather than as a
 * remembered intention.
 */
class ViewerChromeTest {

    @Test
    fun `a tap on empty space hides the controls, and another brings them back`() {
        val hidden = ViewerChrome.onViewportTap(controlsShown = true, measureMode = false)
        assertFalse(hidden)
        assertTrue(ViewerChrome.onViewportTap(controlsShown = hidden, measureMode = false))
    }

    @Test
    fun `a measure tap is not a tap on empty space`() {
        // Both directions: measure mode never changes the chrome, whichever
        // state the chrome is in. Asserting only the shown case would let a
        // future "hide while measuring" through.
        assertTrue(ViewerChrome.onViewportTap(controlsShown = true, measureMode = true))
        assertFalse(ViewerChrome.onViewportTap(controlsShown = false, measureMode = true))
        assertTrue(ViewerChrome.tapIsMeasurement(measureMode = true))
        assertFalse(ViewerChrome.tapIsMeasurement(measureMode = false))
    }

    @Test
    fun `measure mode can never be entered into an invisible toolbar`() {
        // The 📏 that leaves measure mode is one of the controls. If turning
        // measure on could leave them hidden, the mode would have no exit.
        assertTrue(ViewerChrome.controlsVisible(controlsShown = false, measureMode = true))
        assertTrue(ViewerChrome.controlsVisible(controlsShown = true, measureMode = true))
    }

    @Test
    fun `hidden means hidden when nothing else is claiming the screen`() {
        assertFalse(ViewerChrome.controlsVisible(controlsShown = false, measureMode = false))
        assertTrue(ViewerChrome.controlsVisible(controlsShown = true, measureMode = false))
    }
}
