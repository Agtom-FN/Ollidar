package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 5 (owner mockup review, addition 2): point size is **0.1 – 3.0 px in 0.1
 * steps** wherever it is set. These tests pin the range, the snapping and — the
 * one that actually caught something — that [clamped] no longer clamps the
 * bottom of that range back up to A14's 0.5 px floor.
 */
class DisplayLimitsTest {

    @Test
    fun `the owner's range is what the control offers`() {
        assertEquals(0.1f, DisplayLimits.POINT_SIZE_MIN_PX, 0f)
        assertEquals(3.0f, DisplayLimits.POINT_SIZE_MAX_PX, 0f)
        assertEquals(0.1f, DisplayLimits.POINT_SIZE_STEP_PX, 0f)
    }

    @Test
    fun `slider steps land on the tenth-pixel grid`() {
        // 0.1 → 3.0 in 0.1 steps is 29 intervals, so 28 interior stops.
        assertEquals(28, DisplayLimits.POINT_SIZE_STEPS)
    }

    @Test
    fun `snapping rounds onto the grid and stays in range`() {
        assertEquals(0.1f, DisplayLimits.snapPointSize(0.12f), 1e-6f)
        assertEquals(0.5f, DisplayLimits.snapPointSize(0.47f), 1e-6f)
        assertEquals(2.5f, DisplayLimits.snapPointSize(2.53f), 1e-6f)
        assertEquals(3.0f, DisplayLimits.snapPointSize(9f), 1e-6f)
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
