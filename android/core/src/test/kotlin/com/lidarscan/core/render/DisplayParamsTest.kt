package com.lidarscan.core.render

import com.lidarscan.core.gnss.FixType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B10 — the Kotlin port of A14's model. These cases assert the two things a
 * port can get wrong: a transcription slip in the four profile presets, and a
 * clamp that disagrees with `clamp_display_params()`'s documented ranges.
 */
class DisplayParamsTest {

    @Test
    fun `profile presets match A14 section 5's table`() {
        val survey = profileDefaults(DisplayProfile.SURVEY)
        assertEquals(PointSizeMode.ADAPTIVE, survey.pointSize.mode)
        assertEquals(1.5f, survey.pointSize.adaptiveMinPx, 0f)
        assertEquals(4.0f, survey.pointSize.adaptiveMaxPx, 0f)
        assertEquals(15_000_000, survey.lodPointBudget)
        assertEquals(ColorMode.HEIGHT, survey.colorMode)
        assertEquals(Colormap.SPECTRUM, survey.height.colormap)
        assertTrue(survey.edlEnabled)
        assertEquals(0.6f, survey.edlStrength, 1e-6f)
        assertTrue(survey.showTrajectory)
        assertTrue(survey.showPoseGraph)

        val fp = profileDefaults(DisplayProfile.FLOOR_PLAN)
        assertEquals(PointSizeMode.FIXED_PIXELS, fp.pointSize.mode)
        assertEquals(1.5f, fp.pointSize.fixedPx, 0f)
        assertEquals(8_000_000, fp.lodPointBudget)
        assertEquals(Colormap.THERMAL, fp.height.colormap)
        // The height RANGE is pinned to the clip band, not left on auto —
        // the non-obvious half of A14's floor-plan preset.
        assertFalse(fp.height.autoRange)
        assertEquals(1.0f, fp.height.manualMin, 0f)
        assertEquals(1.5f, fp.height.manualMax, 0f)
        assertTrue(fp.clipHeightEnabled)
        assertEquals(1.0f, fp.clipHeightMin, 0f)
        assertEquals(1.5f, fp.clipHeightMax, 0f)
        assertFalse(fp.showTrajectory)

        val research = profileDefaults(DisplayProfile.RESEARCH)
        assertEquals(ColorMode.RGB, research.colorMode)
        assertEquals(50_000_000, research.lodPointBudget)

        val quick = profileDefaults(DisplayProfile.QUICK_SCAN)
        assertEquals(ColorMode.INTENSITY, quick.colorMode)
        assertEquals(Colormap.GRAYSCALE, quick.intensity.colormap)
        assertEquals(2_000_000, quick.lodPointBudget)
        // EDL is OFF here because S3 never measured its cost — the one place a
        // profile deliberately spends nothing rather than a little.
        assertFalse(quick.edlEnabled)
        assertTrue(quick.showTrajectory)
        assertFalse(quick.showPoseGraph)
    }

    @Test
    fun `every profile preset is already clamped`() {
        // The same property A14's own `profiles/all_are_already_clamped` case
        // asserts: clamping a preset must be a no-op.
        DisplayProfile.entries.forEach { p ->
            val d = profileDefaults(p)
            assertEquals("$p", d, d.clamped())
        }
    }

    @Test
    fun `clamp pins every field to A14's documented range`() {
        val wild = DisplayParams(
            pointSize = PointSizeParams(fixedPx = 999f, adaptiveMinPx = 40f, adaptiveMaxPx = 2f, worldSizeM = 50f),
            lodPointBudget = 1,
            height = ScalarColorParams(manualMin = 9f, manualMax = 1f, gamma = 99f, brightness = -3f),
            edlStrength = 7f,
            clipHeightMin = 5f,
            clipHeightMax = -5f,
        ).clamped()

        assertEquals(64f, wild.pointSize.fixedPx, 0f)
        assertEquals(1f, wild.pointSize.worldSizeM, 0f)
        // An inverted min/max pair is SWAPPED, not rejected.
        assertTrue(wild.pointSize.adaptiveMinPx <= wild.pointSize.adaptiveMaxPx)
        assertEquals(1_000, wild.lodPointBudget)
        assertTrue(wild.height.manualMin <= wild.height.manualMax)
        assertEquals(4f, wild.height.gamma, 0f)
        assertEquals(0.1f, wild.height.brightness, 1e-6f)
        assertEquals(1f, wild.edlStrength, 0f)
        assertTrue(wild.clipHeightMin <= wild.clipHeightMax)
    }

    @Test
    fun `clamp replaces non-finite values with the field's default`() {
        val nan = DisplayParams(
            pointSize = PointSizeParams(fixedPx = Float.NaN),
            edlStrength = Float.POSITIVE_INFINITY,
            height = ScalarColorParams(gamma = Float.NaN),
        ).clamped()
        // The DEFAULT, not the nearest bound: A14's clamp "replaces NaN/Inf
        // with the field's default", which for edlStrength is 0.5 and not the
        // 1.0 an infinity would clamp to.
        assertEquals(2f, nan.pointSize.fixedPx, 0f)
        assertEquals(0.5f, nan.edlStrength, 0f)
        assertEquals(1f, nan.height.gamma, 0f)
    }

    @Test
    fun `alpha is always the vertex's own, in every colour mode`() {
        // point_page.h reserves alpha for LOD-fade/selection, so no colour mode
        // may repurpose it — A14's one absolute rule.
        ColorMode.entries.forEach { mode ->
            val c = evaluatePointColor(
                r = 200, g = 100, b = 50, a = 77, z = 1.2f,
                attrs = PointAttributes(intensity = 0.5f, tSeconds = 3.0, fix = FixType.RTK_FIXED),
                params = DisplayParams(colorMode = mode),
            )
            assertEquals("mode $mode", 77, c.a)
        }
    }

    @Test
    fun `rgb mode is an exact passthrough`() {
        val c = evaluatePointColor(11, 22, 33, 255, 5f, PointAttributes(), DisplayParams(colorMode = ColorMode.RGB))
        assertEquals(11, c.r); assertEquals(22, c.g); assertEquals(33, c.b)
    }

    @Test
    fun `time and fix-quality fall back to RGB when their data is absent`() {
        val src = Triple(11, 22, 33)
        listOf(ColorMode.TIME, ColorMode.FIX_QUALITY).forEach { mode ->
            val c = evaluatePointColor(
                src.first, src.second, src.third, 255, 5f,
                PointAttributes(), // nothing supplied
                DisplayParams(colorMode = mode),
            )
            assertEquals("mode $mode", src.first, c.r)
            assertEquals("mode $mode", src.second, c.g)
            assertEquals("mode $mode", src.third, c.b)
        }
    }

    @Test
    fun `fix-quality uses the palette when a fix is supplied`() {
        val c = evaluatePointColor(
            0, 0, 0, 255, 0f,
            PointAttributes(fix = FixType.RTK_FIXED),
            DisplayParams(colorMode = ColorMode.FIX_QUALITY),
        )
        val expected = DisplayParams.DEFAULT_FIX_COLORS[FixType.RTK_FIXED.code]
        assertEquals(expected.r, c.r); assertEquals(expected.g, c.g); assertEquals(expected.b, c.b)
    }

    @Test
    fun `intensity falls back to RGB luminance, matching A9's bridge`() {
        // 0.299*255 = 76.245 -> grayscale colormap at t=0.299 -> 76
        val c = evaluatePointColor(
            255, 0, 0, 255, 0f,
            PointAttributes(),
            DisplayParams(
                colorMode = ColorMode.INTENSITY,
                intensity = ScalarColorParams(manualMin = 0f, manualMax = 1f, colormap = Colormap.GRAYSCALE),
            ),
        )
        assertEquals(76, c.r)
        assertEquals(c.r, c.g)
        assertEquals(c.r, c.b)
    }

    @Test
    fun `height maps the vertex z through the active scalar block`() {
        val params = DisplayParams(
            colorMode = ColorMode.HEIGHT,
            height = ScalarColorParams(manualMin = 0f, manualMax = 4f, colormap = Colormap.GRAYSCALE),
            // The OTHER scalar blocks must never be read in height mode.
            intensity = ScalarColorParams(manualMin = 100f, manualMax = 200f, colormap = Colormap.THERMAL),
        )
        assertEquals(255, evaluatePointColor(0, 0, 0, 255, 4f, PointAttributes(), params).r)
        assertEquals(0, evaluatePointColor(0, 0, 0, 255, 0f, PointAttributes(), params).r)
        assertEquals(128, evaluatePointColor(0, 0, 0, 255, 2f, PointAttributes(), params).r)
    }

    @Test
    fun `invert flips the mapped value`() {
        val p = DisplayParams(
            colorMode = ColorMode.HEIGHT,
            height = ScalarColorParams(manualMin = 0f, manualMax = 1f, colormap = Colormap.GRAYSCALE, invert = true),
        )
        assertEquals(0, evaluatePointColor(0, 0, 0, 255, 1f, PointAttributes(), p).r)
        assertEquals(255, evaluatePointColor(0, 0, 0, 255, 0f, PointAttributes(), p).r)
    }

    @Test
    fun `the active scalar block is chosen by colour mode, neutral for rgb`() {
        val d = DisplayParams(
            colorMode = ColorMode.RGB,
            height = ScalarColorParams(gamma = 3f, manualMin = 7f, manualMax = 9f),
        )
        // kRgb/kFixQuality get a neutral identity mapping, exactly as
        // to_uniforms() does — a shader branches on the mode first.
        assertEquals(1f, d.activeScalar.gamma, 0f)
        assertEquals(0f, d.activeScalar.manualMin, 0f)
        assertEquals(1f, d.activeScalar.manualMax, 0f)
        assertEquals(Colormap.GRAYSCALE, d.activeScalar.colormap)
    }

    @Test
    fun `colour-mode availability explains the two modes that have no data`() {
        val offline = colorModeAvailability(gnssActive = false)
        assertNull(offline[ColorMode.RGB])
        assertNull(offline[ColorMode.HEIGHT])
        assertNull(offline[ColorMode.INTENSITY])
        // Not merely disabled — the UI has a sentence for why.
        assertNotNull(offline[ColorMode.TIME])
        assertNotNull(offline[ColorMode.FIX_QUALITY])

        val live = colorModeAvailability(gnssActive = true)
        assertNull(live[ColorMode.FIX_QUALITY])
        // Time stays unavailable regardless: PointVertex carries no timestamp.
        assertNotNull(live[ColorMode.TIME])
    }

    @Test
    fun `Rgba packs to ARGB the way Compose and Filament expect`() {
        val c = Rgba(0x12, 0x34, 0x56, 0x78)
        assertEquals(0x78123456.toInt(), c.toArgbInt())
        val f = c.toFloat4()
        assertEquals(0x12 / 255f, f[0], 1e-6f)
        assertEquals(0x78 / 255f, f[3], 1e-6f)
    }

    // ── ROUND 8, owner item 29: the capture screen's own defaults ──────────

    /**
     * The exact block the owner's real Pixel 8 Pro capture should have been
     * drawn with, and was not. Its `project.json` recorded:
     *
     * ```json
     * "pointSize": { "mode": "ADAPTIVE", "fixedPx": 2.5, ... },
     * "colorMode": "RGB"
     * ```
     *
     * 2.5 px points smear an indoor cloud into a solid surface — which is
     * exactly the structure ("are the walls straight?") the operator opens the
     * live view to check — and RGB on an uncolorized D6 return is a
     * pass-through of nothing.
     */
    @Test
    fun `the capture defaults are intensity, one pixel, and identity tone`() {
        assertEquals(ColorMode.INTENSITY, DisplayParams.CAPTURE_COLOR_MODE)
        assertEquals(1.0f, DisplayParams.CAPTURE_POINT_SIZE_PX, 0f)
        assertEquals(1.0f, DisplayParams.CAPTURE_GAMMA, 0f)
        assertEquals(1.0f, DisplayParams.CAPTURE_BRIGHTNESS, 0f)

        val d = DisplayParams.captureDefaults()
        assertEquals(ColorMode.INTENSITY, d.colorMode)
        assertEquals(1.0f, d.pointSize.fixedPx, 0f)
        // The MODE matters as much as the number: the capture sheet's only
        // point-size control writes `fixedPx`, and a renderer honouring
        // ADAPTIVE never reads that field at all — so the slider the owner was
        // moving had no defined effect. `2.5` was not merely too big; it was
        // being written into a field the declared mode said to ignore.
        assertEquals(PointSizeMode.FIXED_PIXELS, d.pointSize.mode)
        assertEquals(1f, d.intensity.gamma, 0f)
        assertEquals(1f, d.intensity.brightness, 0f)
        assertEquals(1f, d.height.gamma, 0f)
        assertEquals(1f, d.height.brightness, 0f)

        // And it is NOT the 2.5 px the field capture used, stated as its own
        // assertion so the regression has a name.
        assertNotEquals(2.5f, d.pointSize.fixedPx)
    }

    /** Whatever else changes, the capture block must survive its own clamp unaltered. */
    @Test
    fun `the capture defaults are already clamped`() {
        val once = DisplayParams.captureDefaults()
        assertEquals(once, once.clamped())
        assertTrue(
            "1.0 px must be inside the Android-side point-size range (ROUND 5 lowered the floor to 0.1)",
            once.pointSize.fixedPx >= DisplayLimits.POINT_SIZE_MIN_PX,
        )
    }
}
