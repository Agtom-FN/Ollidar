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

    @Test
    fun `texture is 256 wide by 3 rows of RGBA8, grayscale row first`() {
        val bytes = ColormapLut.buildTextureRgba8()
        assertEquals(ColormapLut.SIZE * 3 * 4, bytes.size)

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
    }
}
