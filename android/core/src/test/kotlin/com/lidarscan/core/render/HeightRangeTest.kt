package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 28 item 154. The design review's §E-1 point 3 asked for exactly one
 * thing to become impossible to ship again: *"a build-time assertion that a
 * synthetic cloud with a known 2 m span renders >= 3 distinct ramp colours."*
 *
 * The reason it is a `:core` JVM test and not an instrumented one is the reason
 * the bug survived to a screenshot in the first place — a monochrome cloud is
 * not a crash, not an exception and not a compile error, so the only thing that
 * catches it is either an emulator someone remembers to look at or an assertion
 * that runs on every build. [HeightRange] exists to make the second possible:
 * the axis choice, the normalisation and its guards are pure arithmetic, and
 * `points.mat` is a four-operation transcription of [HeightRange.normalise].
 */
class HeightRangeTest {

    /** The Y-up world frame the runtime actually uses. Regressing this to 2 fails half this file. */
    @Test
    fun `height axis is y`() {
        assertEquals(1, HeightRange.AXIS)
    }

    /**
     * A synthetic cloud walked at one height: 2 m of Y, a 6 m room in x and z.
     * `n` points evenly spread top to bottom.
     */
    private fun syntheticCloud(n: Int, ySpanM: Float): List<FloatArray> =
        List(n) { i ->
            val f = i.toFloat() / (n - 1)
            floatArrayOf(
                -3f + 6f * f,        // x: across the room
                f * ySpanM,          // y: the height the ramp is supposed to read
                -3f + 6f * (1f - f), // z: along the room
            )
        }

    private fun rampColours(cloud: List<FloatArray>, axis: Int): Set<ColormapLut.Rgba8> {
        var lo = Float.MAX_VALUE
        var hi = -Float.MAX_VALUE
        for (p in cloud) {
            lo = minOf(lo, p[axis]); hi = maxOf(hi, p[axis])
        }
        val range = HeightRange.resolve(lo, hi, HeightRange.Range(0f, 3f))
        return cloud
            .map { HeightRange.normalise(it[axis], range.min, range.max) }
            .map { ColormapLut.raw(Colormap.TURBO, it) }
            .toSet()
    }

    /** §E-1 point 3, stated as the review states it. */
    @Test
    fun `two metre cloud produces at least three distinct ramp colours`() {
        val colours = rampColours(syntheticCloud(64, ySpanM = 2f), HeightRange.AXIS)
        assertTrue("expected >= 3 distinct Turbo colours, got ${colours.size}", colours.size >= 3)
    }

    /**
     * The regression itself, pinned from the other side: the same cloud carries
     * a flat height in the axis the shader USED to read once the walk is level.
     * If `AXIS` ever goes back to 2 the test above would still pass on this
     * fixture (the room has z span), so the failure is nailed down with a cloud
     * that is level in y and level in nothing else.
     */
    @Test
    fun `level walk collapses on the old z axis and not on y`() {
        // A metre of height, and a walk that covers a room at that height.
        val cloud = List(64) { i ->
            val f = i.toFloat() / 63f
            floatArrayOf(-3f + 6f * f, f * 1f, 4.2f) // z is one wall: no span at all
        }
        assertTrue(rampColours(cloud, HeightRange.AXIS).size >= 3)
        // z: degenerate -> the guard fires -> one mid-ramp colour for everything.
        assertEquals(1, rampColours(cloud, 2).size)
    }

    // --- the degenerate-span guard -------------------------------------------

    @Test
    fun `degenerate span resolves to a mid-ramp window centred on the data`() {
        val r = HeightRange.resolve(1.5f, 1.5f, HeightRange.Range(0f, 3f))
        assertEquals(1.5f - HeightRange.DEGENERATE_HALF_SPAN_M, r.min, 1e-6f)
        assertEquals(1.5f + HeightRange.DEGENERATE_HALF_SPAN_M, r.max, 1e-6f)
        // The whole point of centring rather than clamping the span: mid ramp.
        assertEquals(0.5f, HeightRange.normalise(1.5f, r.min, r.max), 1e-6f)
    }

    @Test
    fun `sub epsilon span is still degenerate`() {
        val r = HeightRange.resolve(2f, 2f + HeightRange.EPSILON_M / 2f, HeightRange.Range(0f, 3f))
        assertTrue(r.span > HeightRange.EPSILON_M)
        assertEquals(1.0f, r.span, 1e-5f)
    }

    @Test
    fun `a real span is passed through untouched`() {
        val r = HeightRange.resolve(-0.4f, 1.6f, HeightRange.Range(0f, 3f))
        assertEquals(-0.4f, r.min, 1e-6f)
        assertEquals(1.6f, r.max, 1e-6f)
    }

    @Test
    fun `inverted bounds are ordered rather than producing a negative span`() {
        val r = HeightRange.resolve(1.6f, -0.4f, HeightRange.Range(0f, 3f))
        assertEquals(-0.4f, r.min, 1e-6f)
        assertEquals(1.6f, r.max, 1e-6f)
    }

    @Test
    fun `degenerate span never divides by zero in normalise`() {
        val t = HeightRange.normalise(1f, 1f, 1f)
        assertTrue(t.isFinite())
        assertEquals(0.5f, t, 1e-6f)
    }

    // --- the NaN guard --------------------------------------------------------

    @Test
    fun `nan bounds fall back to the caller's range`() {
        val fallback = HeightRange.Range(0f, 3f)
        assertEquals(fallback, HeightRange.resolve(Float.NaN, 2f, fallback))
        assertEquals(fallback, HeightRange.resolve(0f, Float.NaN, fallback))
        assertEquals(fallback, HeightRange.resolve(Float.NaN, Float.NaN, fallback))
    }

    @Test
    fun `infinite bounds fall back to the caller's range`() {
        val fallback = HeightRange.Range(0f, 3f)
        assertEquals(fallback, HeightRange.resolve(Float.NEGATIVE_INFINITY, 2f, fallback))
        assertEquals(fallback, HeightRange.resolve(0f, Float.POSITIVE_INFINITY, fallback))
    }

    @Test
    fun `nan never leaves normalise`() {
        assertEquals(0.5f, HeightRange.normalise(Float.NaN, 0f, 2f), 1e-6f)
        assertEquals(0.5f, HeightRange.normalise(1f, Float.NaN, 2f), 1e-6f)
        assertEquals(0.5f, HeightRange.normalise(1f, 0f, Float.NaN), 1e-6f)
        // And the ramp still answers with a colour, not a black hole.
        assertNotEquals(null, ColormapLut.raw(Colormap.TURBO, HeightRange.normalise(Float.NaN, 0f, 2f)))
    }

    @Test
    fun `normalise clamps outside the range instead of extrapolating`() {
        assertEquals(0f, HeightRange.normalise(-10f, 0f, 2f), 1e-6f)
        assertEquals(1f, HeightRange.normalise(10f, 0f, 2f), 1e-6f)
    }

    // --- the growth re-apply threshold ---------------------------------------

    @Test
    fun `an unchanged cloud does not re-apply`() {
        val applied = HeightRange.Range(0f, 2f)
        assertFalse(HeightRange.needsReapply(applied, applied))
    }

    @Test
    fun `a sub-texel drift does not re-apply`() {
        val applied = HeightRange.Range(0f, 2f)
        // 2 % of a 2 m span is 40 mm; 10 mm is under one LUT texel of colour.
        assertFalse(HeightRange.needsReapply(applied, HeightRange.Range(0f, 2.01f)))
    }

    @Test
    fun `a growing cloud re-applies`() {
        val applied = HeightRange.Range(0f, 2f)
        // The operator walks up a flight of stairs.
        assertTrue(HeightRange.needsReapply(applied, HeightRange.Range(0f, 5f)))
        // Or down into a basement.
        assertTrue(HeightRange.needsReapply(applied, HeightRange.Range(-3f, 2f)))
    }

    /**
     * The first page of a capture is degenerate, so the applied span is the
     * [HeightRange.DEGENERATE_HALF_SPAN_M] window — and the real cloud arriving
     * around it MUST still trigger, or a capture would spend its whole life in
     * the fallback colour it was given on frame one.
     */
    @Test
    fun `growth out of the degenerate fallback re-applies`() {
        val applied = HeightRange.resolve(1.2f, 1.2f, HeightRange.Range(0f, 3f))
        assertTrue(HeightRange.needsReapply(applied, HeightRange.Range(0f, 2.4f)))
    }

    @Test
    fun `nan bounds never trigger a re-apply`() {
        val applied = HeightRange.Range(0f, 2f)
        assertFalse(HeightRange.needsReapply(applied, HeightRange.Range(Float.NaN, 2f)))
        assertFalse(HeightRange.needsReapply(applied, HeightRange.Range(0f, Float.NaN)))
    }

    /** A zero-span applied range must not make the tolerance zero and re-apply every frame. */
    @Test
    fun `a zero applied span does not make every frame re-apply`() {
        val applied = HeightRange.Range(1f, 1f)
        assertFalse(HeightRange.needsReapply(applied, HeightRange.Range(1f, 1f)))
    }
}
