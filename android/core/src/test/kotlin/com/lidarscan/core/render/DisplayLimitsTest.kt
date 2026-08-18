package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 5 (owner mockup review, addition 2): point size in **0.1 px steps**
 * wherever it is set, with the round-5 0.1 px floor intact. ROUND 19 item 76
 * widened the ceiling to Review's 12 px — the two panels now read ONE range
 * (round 16 named the divergence; this round closed it) — so the pins below
 * are on the SHARED constants, not on the old capture-only ceiling.
 */
class DisplayLimitsTest {

    @Test
    fun `one range, both panels — the round-5 floor and Review's ceiling`() {
        assertEquals(0.1f, DisplayLimits.POINT_SIZE_MIN_PX, 0f)
        assertEquals(12.0f, DisplayLimits.POINT_SIZE_MAX_PX, 0f)
        assertEquals(0.1f, DisplayLimits.POINT_SIZE_STEP_PX, 0f)
        // The LOD budget too: 0.5-50 M for both, round 16's other divergence.
        assertEquals(0.5f, DisplayLimits.LOD_MIN_M, 0f)
        assertEquals(50f, DisplayLimits.LOD_MAX_M, 0f)
    }

    @Test
    fun `slider steps land on the tenth-pixel grid`() {
        // 0.1 → 12.0 in 0.1 steps is 119 intervals, so 118 interior stops.
        assertEquals(118, DisplayLimits.POINT_SIZE_STEPS)
    }

    @Test
    fun `snapping rounds onto the grid and stays in range`() {
        assertEquals(0.1f, DisplayLimits.snapPointSize(0.12f), 1e-6f)
        assertEquals(0.5f, DisplayLimits.snapPointSize(0.47f), 1e-6f)
        assertEquals(2.5f, DisplayLimits.snapPointSize(2.53f), 1e-6f)
        assertEquals(12.0f, DisplayLimits.snapPointSize(90f), 1e-6f)
        assertEquals(0.1f, DisplayLimits.snapPointSize(-4f), 1e-6f)
    }

    @Test
    fun `snapping never produces a value that prints with float noise`() {
        var px = DisplayLimits.POINT_SIZE_MIN_PX
        while (px <= DisplayLimits.POINT_SIZE_MAX_PX + 1e-4f) {
            val snapped = DisplayLimits.snapPointSize(px)
            val oneDecimal = Math.round(snapped * 10f) / 10f
            assertEquals("$snapped must already be on the 0.1 grid", oneDecimal, snapped, 1e-6f)
            px += DisplayLimits.POINT_SIZE_STEP_PX
        }
    }

    @Test
    fun `clamped preserves a sub-half-pixel point size`() {
        // A14's own clamp_display_params() floor is 0.5 px; the round-5 control
        // goes to 0.1, so this is the assertion that the Android-side relaxation
        // is actually in place (and would catch a re-sync with the C++ ranges
        // that silently re-imposed 0.5).
        val tiny = DisplayParams(pointSize = PointSizeParams(mode = PointSizeMode.FIXED_PIXELS, fixedPx = 0.1f))
        assertEquals(0.1f, tiny.clamped().pointSize.fixedPx, 1e-6f)

        val below = DisplayParams(pointSize = PointSizeParams(fixedPx = 0.01f)).clamped()
        assertEquals(DisplayLimits.POINT_SIZE_MIN_PX, below.pointSize.fixedPx, 1e-6f)
    }

    @Test
    fun `clamped still enforces A14's ceiling`() {
        val huge = DisplayParams(pointSize = PointSizeParams(fixedPx = 999f)).clamped()
        assertEquals(64f, huge.pointSize.fixedPx, 0f)
    }

    @Test
    fun `refresh options start at uncapped and read out in words`() {
        assertEquals(0, DisplayLimits.REFRESH_HZ_OPTIONS.first())
        assertEquals("Max", DisplayLimits.refreshLabel(0))
        assertEquals("15 fps", DisplayLimits.refreshLabel(15))
        assertTrue(DisplayLimits.REFRESH_HZ_OPTIONS.contains(30))
    }
}
