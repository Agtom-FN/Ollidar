package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Hand-computed endpoint values from `engine/docs/A14-display.md` §3's
 * formulas ("grayscale: r=g=b=round(t*255)"; "spectrum: h=240°(1-t)->0°,
 * blue -> red"; "thermal: black -> red -> yellow -> white") — the same
 * derivation style `engine/tests/test_display_params.cpp`'s
 * `colormap`-prefixed test cases use on the C++ side, since exact values are only
 * possible at these boundary points (see A14 doc's own grayscale note).
 */
class ColormapLutTest {

    @Test
    fun `grayscale endpoints are black and white`() {
        assertEquals(ColormapLut.Rgba8(0, 0, 0, 255), ColormapLut.grayscaleRaw(0f))
        assertEquals(ColormapLut.Rgba8(255, 255, 255, 255), ColormapLut.grayscaleRaw(1f))
    }

    @Test
    fun `grayscale midpoint is mid-grey`() {
        val mid = ColormapLut.grayscaleRaw(0.5f)
        assertEquals(mid.r, mid.g)
        assertEquals(mid.g, mid.b)
        assertEquals(128, mid.r) // round(0.5 * 255) == 128 (rounds .5 up)
    }

    @Test
    fun `spectrum sweeps blue at t=0 to red at t=1`() {
        assertEquals(ColormapLut.Rgba8(0, 0, 255, 255), ColormapLut.spectrumRaw(0f))
        assertEquals(ColormapLut.Rgba8(255, 0, 0, 255), ColormapLut.spectrumRaw(1f))
    }

    @Test
    fun `thermal sweeps black to red to yellow to white`() {
        assertEquals(ColormapLut.Rgba8(0, 0, 0, 255), ColormapLut.thermalRaw(0f))
        assertEquals(ColormapLut.Rgba8(255, 0, 0, 255), ColormapLut.thermalRaw(1f / 3f))
        assertEquals(ColormapLut.Rgba8(255, 255, 0, 255), ColormapLut.thermalRaw(2f / 3f))
        assertEquals(ColormapLut.Rgba8(255, 255, 255, 255), ColormapLut.thermalRaw(1f))
    }

    @Test
    fun `thermal channels are individually non-decreasing (near-monotonic luminance)`() {
        var prevR = -1
        var prevG = -1
        var prevB = -1
        for (i in 0..255) {
            val c = ColormapLut.thermalRaw(i / 255f)
            assertTrue("red channel decreased at $i", c.r >= prevR)
            assertTrue("green channel decreased at $i", c.g >= prevG)
            assertTrue("blue channel decreased at $i", c.b >= prevB)
            prevR = c.r; prevG = c.g; prevB = c.b
        }
    }

    /**
     * ROUND 26 (owner item 127)'s Turbo ramp. Unlike the three above, this one
     * is NOT a port of an engine formula — it is a piecewise-linear ramp through
     * published Turbo anchor stops (see `ColormapLut.TURBO_STOPS`) — so there is
     * no `test_display_params.cpp` case to agree with and these assertions ARE
     * the specification. Every anchor is pinned exactly, which is possible
     * because the segment search hits `tt == t1` with an interpolation fraction
     * of literally 1.0.
     */
    @Test
    fun `turbo hits every anchor stop exactly`() {
        assertEquals(ColormapLut.Rgba8(48, 18, 59, 255), ColormapLut.turboRaw(0f))
        assertEquals(ColormapLut.Rgba8(70, 107, 227, 255), ColormapLut.turboRaw(0.125f))
        assertEquals(ColormapLut.Rgba8(36, 168, 246, 255), ColormapLut.turboRaw(0.25f))
        assertEquals(ColormapLut.Rgba8(30, 215, 201, 255), ColormapLut.turboRaw(0.375f))
        assertEquals(ColormapLut.Rgba8(95, 241, 135, 255), ColormapLut.turboRaw(0.5f))
        assertEquals(ColormapLut.Rgba8(170, 247, 73, 255), ColormapLut.turboRaw(0.625f))
        assertEquals(ColormapLut.Rgba8(231, 215, 48, 255), ColormapLut.turboRaw(0.75f))
        assertEquals(ColormapLut.Rgba8(253, 150, 36, 255), ColormapLut.turboRaw(0.875f))
        assertEquals(ColormapLut.Rgba8(238, 86, 18, 255), ColormapLut.turboRaw(0.9375f))
        assertEquals(ColormapLut.Rgba8(122, 4, 3, 255), ColormapLut.turboRaw(1f))
    }

    /**
     * The shape item 127 actually asked for: "dark blue -> blue -> green ->
     * yellow -> orange -> red". Pinning the anchors above proves the table was
     * typed correctly; this proves the table is the ramp the owner described —
     * blue dominates the bottom, green the middle, red the top, and both ends
     * are DARK, which is Turbo's whole advantage over SPECTRUM for height.
     */
    @Test
    fun `turbo runs dark blue through green to dark red`() {
        val lo = ColormapLut.turboRaw(0f)
        assertTrue("bottom should be blue-dominant", lo.b > lo.r && lo.b > lo.g)

        val mid = ColormapLut.turboRaw(0.5f)
        assertTrue("middle should be green-dominant", mid.g > mid.r && mid.g > mid.b)

        val hi = ColormapLut.turboRaw(1f)
        assertTrue("top should be red-dominant", hi.r > hi.g && hi.r > hi.b)

        // Both ends dark, the middle bright. Luminance is eyeballed with a plain
        // channel sum; the point is the ordering, not the photometry.
        fun lum(c: ColormapLut.Rgba8) = c.r + c.g + c.b
        assertTrue("t=0 should be darker than the middle", lum(lo) < lum(mid))
        assertTrue("t=1 should be darker than the middle", lum(hi) < lum(mid))
    }

    /**
     * No NaN, no jumps. A piecewise ramp's failure mode is a mis-typed segment
     * boundary — a division by a zero-width span (NaN) or a discontinuity where
     * two segments disagree about the shared stop — and neither shows up in the
     * anchor test above, which only samples the stops themselves.
     */
    @Test
    fun `turbo is continuous and finite across the whole range`() {
        // Every anchor gap is 0.125 or 0.0625 wide, so no single 1/1024 step can
        // cross a whole segment; the largest legitimate per-step channel change
        // is bounded by the steepest segment (0.875 -> 0.9375, blue 36 -> 18 and
        // green 150 -> 86 over 0.0625) at well under 8 per step.
        val steps = 1024
        var prev = ColormapLut.turboRaw(0f)
        for (i in 1..steps) {
            val c = ColormapLut.turboRaw(i.toFloat() / steps)
            for (v in listOf(c.r, c.g, c.b, c.a)) {
                assertTrue("channel out of range at $i: $c", v in 0..255)
            }
            assertEquals("alpha must stay opaque at $i", 255, c.a)
            assertTrue("red jumped at $i", kotlin.math.abs(c.r - prev.r) <= 8)
            assertTrue("green jumped at $i", kotlin.math.abs(c.g - prev.g) <= 8)
            assertTrue("blue jumped at $i", kotlin.math.abs(c.b - prev.b) <= 8)
            prev = c
        }
        // Rgba8 is Int-valued, so "never NaN" is a statement about the float
        // arithmetic upstream of the rounding: a NaN fraction would have made
        // roundToInt() throw or coerce to 0, and the continuity bound above is
        // what would have caught the coercion.
        assertEquals(ColormapLut.turboRaw(1f), prev)
    }

    @Test
    fun `turbo clamps outside the unit range`() {
        assertEquals(ColormapLut.turboRaw(0f), ColormapLut.turboRaw(-1f))
        assertEquals(ColormapLut.turboRaw(0f), ColormapLut.turboRaw(-0.001f))
        assertEquals(ColormapLut.turboRaw(1f), ColormapLut.turboRaw(1.001f))
        assertEquals(ColormapLut.turboRaw(1f), ColormapLut.turboRaw(42f))
    }

    /** `raw()` must route TURBO to `turboRaw` and leave the other three alone. */
    @Test
    fun `raw dispatches every colormap to its own ramp`() {
        for (i in 0..8) {
            val t = i / 8f
            assertEquals(ColormapLut.grayscaleRaw(t), ColormapLut.raw(Colormap.GRAYSCALE, t))
            assertEquals(ColormapLut.spectrumRaw(t), ColormapLut.raw(Colormap.SPECTRUM, t))
            assertEquals(ColormapLut.thermalRaw(t), ColormapLut.raw(Colormap.THERMAL, t))
            assertEquals(ColormapLut.turboRaw(t), ColormapLut.raw(Colormap.TURBO, t))
        }
    }

    /**
     * ROUND 26 (item 127): the row count is the ENUM's size, and `points.mat`'s
     * `(colormap + 0.5) / 4.0` divisor is the same number typed a second time in
     * a language matc compiles and Kotlin cannot see. If this ever fails,
     * `points.mat` and `PointCloudRenderer.buildColormapTexture()` are the two
     * places that have to move with it.
     */
    @Test
    fun `ROWS equals the Colormap enum size`() {
        assertEquals(Colormap.entries.size, ColormapLut.ROWS)
        assertEquals(4, ColormapLut.ROWS)
    }

    @Test
    fun `texture is 256 wide by one row per Colormap, grayscale row first`() {
        val bytes = ColormapLut.buildTextureRgba8()
        assertEquals(ColormapLut.SIZE * Colormap.entries.size * 4, bytes.size)

        // Row 0, column 0 == grayscale(0) == black-opaque.
        assertEquals(0, bytes[0].toInt() and 0xFF)
        assertEquals(0, bytes[1].toInt() and 0xFF)
        assertEquals(0, bytes[2].toInt() and 0xFF)
        assertEquals(255, bytes[3].toInt() and 0xFF)

        // Row 1 (spectrum), column 0 == blue-opaque.
        val row1Start = ColormapLut.SIZE * 4
        assertEquals(0, bytes[row1Start].toInt() and 0xFF)
        assertEquals(0, bytes[row1Start + 1].toInt() and 0xFF)
        assertEquals(255, bytes[row1Start + 2].toInt() and 0xFF)

        // ROUND 26 (item 127): TURBO is LAST, which is the half of the ordinal
        // decision that matters — appending rather than inserting is what leaves
        // rows 0..2 (the three ordinals the engine also knows, and the ones an
        // existing project.json may have persisted under those names) exactly
        // where the shader already expects them.
        assertEquals(Colormap.TURBO, Colormap.entries.last())
        val turboStart = ColormapLut.SIZE * (Colormap.entries.size - 1) * 4
        val turbo0 = ColormapLut.turboRaw(0f)
        assertEquals(turbo0.r, bytes[turboStart].toInt() and 0xFF)
        assertEquals(turbo0.g, bytes[turboStart + 1].toInt() and 0xFF)
        assertEquals(turbo0.b, bytes[turboStart + 2].toInt() and 0xFF)
        assertEquals(turbo0.a, bytes[turboStart + 3].toInt() and 0xFF)

        // …and every row is its own colormap's LUT, in ordinal order, so the
        // builder cannot quietly emit the right NUMBER of rows in the wrong
        // order.
        Colormap.entries.forEachIndexed { row, cm ->
            val expected = ColormapLut.raw(cm, 0f)
            val at = ColormapLut.SIZE * row * 4
            assertEquals("row $row ($cm) red", expected.r, bytes[at].toInt() and 0xFF)
            assertEquals("row $row ($cm) green", expected.g, bytes[at + 1].toInt() and 0xFF)
            assertEquals("row $row ($cm) blue", expected.b, bytes[at + 2].toInt() and 0xFF)
        }
    }
}
