package com.lidarscan.core.render

import com.lidarscan.core.measure.MeasureUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 38 item 190 — the height legend's arithmetic, its units and its gates.
 *
 * The item's test clause is *"legend values match the render range (same source
 * of truth), unit formatting, mode/visibility gating"*. The first of those is
 * the interesting one and it is asserted the only way it can be honestly
 * asserted on a JVM: by running the shader's own normalisation
 * ([HeightRange.normalise], which is `points.mat`'s line) forward from a
 * legend label's own value and checking it lands on the tick the label was
 * drawn at. A legend tick that round-trips through the renderer's arithmetic
 * back to its own position is a legend that cannot be lying about the pixels.
 */
class HeightLegendTest {

    private val turboish = HeightRange.Range(-0.42f, 2.58f)

    // ── the values are the render range's ──────────────────────────────────

    /**
     * The tick a label is drawn at, pushed through the SHADER's normalisation,
     * comes back to that same tick. This is the "same source of truth" claim,
     * stated as a round trip rather than as an equality between two copies of
     * the same number.
     */
    @Test
    fun `every tick round-trips through the shader's normalisation`() {
        for (t in HeightLegend.TICKS) {
            val metres = HeightLegend.valueAt(turboish, t)
            val back = HeightRange.normalise(metres, turboish.min, turboish.max)
            assertEquals("tick $t", t, back, 1e-5f)
        }
    }

    @Test
    fun `the ends of the ramp are the ends of the range`() {
        assertEquals(turboish.min, HeightLegend.valueAt(turboish, 0f), 1e-6f)
        assertEquals(turboish.max, HeightLegend.valueAt(turboish, 1f), 1e-6f)
        assertEquals(
            (turboish.min + turboish.max) / 2f,
            HeightLegend.valueAt(turboish, 0.5f),
            1e-6f,
        )
    }

    @Test
    fun `a tick off the end of the bar is clamped to the bar`() {
        assertEquals(turboish.min, HeightLegend.valueAt(turboish, -3f), 1e-6f)
        assertEquals(turboish.max, HeightLegend.valueAt(turboish, 9f), 1e-6f)
    }

    /** The model is bottom-first, parallel to [HeightLegend.TICKS], and rises. */
    @Test
    fun `the model reads bottom to top`() {
        val m = HeightLegend.model(turboish, MeasureUnit.METERS)
        assertEquals(HeightLegend.TICKS.size, m.labels.size)
        assertEquals("-0.42 m", m.bottomLabel)
        assertEquals("1.08 m", m.middleLabel)
        assertEquals("2.58 m", m.topLabel)
        assertEquals(m.labels.first(), m.bottomLabel)
        assertEquals(m.labels.last(), m.topLabel)
    }

    /**
     * An auto-range that GROWS moves the labels — item 190(b). Asserted against
     * `HeightRange.resolve`, which is the function the renderer calls, so the
     * legend is being fed exactly the shape the shader is.
     */
    @Test
    fun `growing the cloud's bounds moves the labels`() {
        val fallback = HeightRange.Range(0f, 3f)
        val first = HeightRange.resolve(0f, 1.2f, fallback)
        val later = HeightRange.resolve(-0.3f, 2.9f, fallback)
        val a = HeightLegend.model(first, MeasureUnit.METERS)
        val b = HeightLegend.model(later, MeasureUnit.METERS)
        assertEquals("1.20 m", a.topLabel)
        assertEquals("2.90 m", b.topLabel)
        assertEquals("0.00 m", a.bottomLabel)
        assertEquals("-0.30 m", b.bottomLabel)
    }

    /** Auto off means the manual pair, verbatim — the renderer's own fallback path. */
    @Test
    fun `a manual range is printed as it stands`() {
        val manual = HeightRange.Range(1f, 1.5f)
        val m = HeightLegend.model(manual, MeasureUnit.METERS)
        assertEquals("1.00 m", m.bottomLabel)
        assertEquals("1.25 m", m.middleLabel)
        assertEquals("1.50 m", m.topLabel)
    }

    // ── unit formatting ────────────────────────────────────────────────────

    @Test
    fun `precision follows magnitude and the unit is always printed`() {
        assertEquals("0.00 m", HeightLegend.format(0f, MeasureUnit.METERS))
        assertEquals("0.25 m", HeightLegend.format(0.25f, MeasureUnit.METERS))
        assertEquals("9.99 m", HeightLegend.format(9.99f, MeasureUnit.METERS))
        assertEquals("42.5 m", HeightLegend.format(42.5f, MeasureUnit.METERS))
        assertEquals("420 m", HeightLegend.format(420.4f, MeasureUnit.METERS))
    }

    /**
     * Feet are DECIMAL here, unlike `formatDistance`'s feet-and-inches — see
     * [HeightLegend.format]'s header for why an axis and a tape want different
     * notation. 3 m is 9.84 ft.
     */
    @Test
    fun `feet are decimal feet, not feet and inches`() {
        assertEquals("9.84 ft", HeightLegend.format(3f, MeasureUnit.FEET))
        assertEquals("0.00 ft", HeightLegend.format(0f, MeasureUnit.FEET))
        assertEquals("-1.38 ft", HeightLegend.format(-0.42f, MeasureUnit.FEET))
        assertTrue(HeightLegend.format(3f, MeasureUnit.FEET).endsWith(" ft"))
    }

    @Test
    fun `switching units re-prints the same range`() {
        val metres = HeightLegend.model(HeightRange.Range(0f, 3f), MeasureUnit.METERS)
        val feet = HeightLegend.model(HeightRange.Range(0f, 3f), MeasureUnit.FEET)
        assertEquals("3.00 m", metres.topLabel)
        assertEquals("9.84 ft", feet.topLabel)
        // Same axis, same number of ticks, only the notation moves.
        assertEquals(metres.labels.size, feet.labels.size)
    }

    /** A minus sign on a rounded zero reads as a measurement below zero. It is not one. */
    @Test
    fun `negative zero prints as zero`() {
        assertEquals("0.00 m", HeightLegend.format(-0.001f, MeasureUnit.METERS))
        assertEquals("0.00 ft", HeightLegend.format(-0.0001f, MeasureUnit.FEET))
        // …and a real negative keeps its sign.
        assertEquals("-0.02 m", HeightLegend.format(-0.02f, MeasureUnit.METERS))
    }

    // ── gating ─────────────────────────────────────────────────────────────

    @Test
    fun `the legend is drawn in height mode with chrome up and a range`() {
        assertTrue(HeightLegend.visible(ColorMode.HEIGHT, chromeVisible = true, range = turboish))
    }

    /** Item 190(b): hidden entirely in INTENSITY and every other mode. */
    @Test
    fun `no legend outside height mode`() {
        for (mode in ColorMode.entries) {
            val shown = HeightLegend.visible(mode, chromeVisible = true, range = turboish)
            assertEquals("mode $mode", mode == ColorMode.HEIGHT, shown)
        }
    }

    /** Item 190(b): it counts as chrome, so the tap-to-hide takes it with the rest. */
    @Test
    fun `no legend when the controls are hidden`() {
        assertFalse(HeightLegend.visible(ColorMode.HEIGHT, chromeVisible = false, range = turboish))
    }

    @Test
    fun `no legend before a range has been applied`() {
        assertFalse(HeightLegend.visible(ColorMode.HEIGHT, chromeVisible = true, range = null))
    }

    /**
     * A zero-span range is not a scale — it prints one number three times. The
     * auto path cannot produce one ([HeightRange.resolve] opens a
     * [HeightRange.DEGENERATE_HALF_SPAN_M] window instead), but a saved manual
     * pair can.
     */
    @Test
    fun `no legend for a degenerate or non-finite range`() {
        assertFalse(HeightLegend.visible(ColorMode.HEIGHT, true, HeightRange.Range(1.5f, 1.5f)))
        assertFalse(
            HeightLegend.visible(
                ColorMode.HEIGHT,
                true,
                HeightRange.Range(1.5f, 1.5f + HeightRange.EPSILON_M / 2f),
            ),
        )
        assertFalse(HeightLegend.visible(ColorMode.HEIGHT, true, HeightRange.Range(Float.NaN, 3f)))
        assertFalse(
            HeightLegend.visible(
                ColorMode.HEIGHT,
                true,
                HeightRange.Range(0f, Float.POSITIVE_INFINITY),
            ),
        )
        // …and the resolver's own degenerate answer IS wide enough to draw.
        val resolved = HeightRange.resolve(1.5f, 1.5f, HeightRange.Range(0f, 3f))
        assertTrue(HeightLegend.visible(ColorMode.HEIGHT, true, resolved))
    }

    /** Three ticks, ends and middle, in ascending order — the shape the bar is drawn to. */
    @Test
    fun `the ticks are the ends and the middle, ascending`() {
        assertEquals(listOf(0f, 0.5f, 1f), HeightLegend.TICKS)
    }
}
