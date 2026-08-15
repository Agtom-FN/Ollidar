package com.lidarscan.core.render

import com.lidarscan.core.gnss.FixType
import kotlinx.serialization.Serializable
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * B10 — a Kotlin mirror of `engine/include/scanengine/cloud/display_params.h`
 * (A14), the model layer behind Tech Spec §3.9's render-settings panel.
 *
 * **Why a port and not a JNI binding.** `engine/capi/scanengine_c.h` mirrors
 * none of `display_params.h` — no struct, no function, at ABI 4 (checked). B4
 * already hit this for `colormap_lut()` and answered it the same way
 * ([ColormapLut]): port the closed-form math from A14's published formulas so
 * the Android result agrees with `evaluate_point_color()`'s ground truth *by
 * construction*, rather than approximating it. This file carries that decision
 * to the whole parameter model, which is what B10 actually needs — a value type
 * it can persist per project (§3.9: "settings persist per project"), clamp, and
 * project into the material parameters `points.mat` already declares.
 *
 * Field names, ranges, defaults and the four profile presets are transcribed
 * from `display_params.h` / `display_params.cpp` / A14 §5. Two things are
 * deliberately **not** ported:
 *
 * * `DisplayParamsUniforms` and `to_uniforms()`. A14 §4 says B10 "can treat it
 *   as a real UBO" — but B4's renderer is **Filament**, not raw Vulkan, and
 *   Filament's `MaterialInstance` takes named parameters, not a byte blob. So
 *   the Android app is in C1's position, not the one A14 predicted for it, and
 *   `PointCloudRenderer` binds the same field names individually. The
 *   208-byte std140 layout is therefore irrelevant here and porting it would be
 *   dead weight that could silently drift.
 * * `DisplayParamsController`. Compose `StateFlow` is this app's change-
 *   notification mechanism and already gives both of A14 §7's modes (poll via
 *   `.value`, push via `collect`); a second subscription mechanism next to it
 *   would be two sources of truth.
 */
@Serializable
data class DisplayParams(
    val pointSize: PointSizeParams = PointSizeParams(),
    /**
     * §3.12's coarse-to-fine LOD budget: a **soft render-time throttle** (skip
     * or decimate pages once resident points exceed it), not the `PageStore`
     * ingest cap. Clamped to [1 000, 200 000 000].
     */
    val lodPointBudget: Int = 5_000_000,
    val colorMode: ColorMode = ColorMode.RGB,
    val height: ScalarColorParams = ScalarColorParams(manualMin = 0f, manualMax = 3f),
    val intensity: ScalarColorParams = ScalarColorParams(),
    val time: ScalarColorParams = ScalarColorParams(),
    /**
     * Discrete palette for [ColorMode.FIX_QUALITY], indexed by [FixType.code].
     * Defaults follow A14's conventional red=bad / green=good GNSS read.
     */
    val fixQualityColors: List<Rgba> = DEFAULT_FIX_COLORS,
    val edlEnabled: Boolean = true,
    val edlStrength: Float = 0.5f,
    val background: Rgba = Rgba(18, 18, 22, 255),
    val clipHeightEnabled: Boolean = false,
    val clipHeightMin: Float = 0f,
    val clipHeightMax: Float = 3f,
    val clipBoxEnabled: Boolean = false,
    val clipBoxMin: List<Float> = listOf(-10f, -10f, -10f),
    val clipBoxMax: List<Float> = listOf(10f, 10f, 10f),
    val showTrajectory: Boolean = true,
    val showPoseGraph: Boolean = false,
) {
    /** The scalar block [colorMode] actually reads — A14 §4's "active block" selection. */
    val activeScalar: ScalarColorParams
        get() = when (colorMode) {
            ColorMode.HEIGHT -> height
            ColorMode.INTENSITY -> intensity
            ColorMode.TIME -> time
            // A correct shader branches on colorMode before reading these, so a
            // neutral identity mapping is what the other two modes get.
            ColorMode.RGB, ColorMode.FIX_QUALITY ->
                ScalarColorParams(autoRange = false, manualMin = 0f, manualMax = 1f, gamma = 1f, brightness = 1f, colormap = Colormap.GRAYSCALE, invert = false)
        }

    companion object {
        val DEFAULT_FIX_COLORS: List<Rgba> = listOf(
            Rgba(128, 128, 128, 255), // none
            Rgba(214, 64, 64, 255), // single
            Rgba(230, 150, 40, 255), // dgps
            Rgba(230, 210, 40, 255), // rtk float
            Rgba(60, 190, 90, 255), // rtk fixed
        )
    }
}

@Serializable
data class Rgba(val r: Int = 0, val g: Int = 0, val b: Int = 0, val a: Int = 255) {
    /** 0xAARRGGBB, which is what Compose's `Color(Long)` and Filament's float4 setters both want fed. */
    fun toArgbInt(): Int = ((a and 0xFF) shl 24) or ((r and 0xFF) shl 16) or ((g and 0xFF) shl 8) or (b and 0xFF)
    fun toFloat4(): FloatArray = floatArrayOf(r / 255f, g / 255f, b / 255f, a / 255f)
}

@Serializable
data class PointSizeParams(
    val mode: PointSizeMode = PointSizeMode.ADAPTIVE,
    val fixedPx: Float = 2f,
    val adaptiveMinPx: Float = 1f,
    val adaptiveMaxPx: Float = 6f,
    /** Distance (m) at which the adaptive size equals [adaptiveMinPx]. */
    val adaptiveReferenceM: Float = 5f,
    /** World-space diameter in metres — S3 §8.3's portable fallback when `gl_PointSize` is unreliable. */
    val worldSizeM: Float = 0.01f,
)

@Serializable
data class ScalarColorParams(
    /**
     * A14 §2: a **UI/persistence bit only**. `evaluate_point_color()` always
     * reads [manualMin]/[manualMax]; when this is true the *renderer* is
     * expected to refresh them from the real data range each frame (for height
     * that source is the pages' own z bounds).
     */
    val autoRange: Boolean = true,
    val manualMin: Float = 0f,
    val manualMax: Float = 1f,
    val gamma: Float = 1f,
    val brightness: Float = 1f,
    val colormap: Colormap = Colormap.SPECTRUM,
    val invert: Boolean = false,
)

/** Mirror of `scanengine::DisplayProfile`. Ordinals match the C++ enum. */
@Serializable
enum class DisplayProfile(val displayName: String) {
    SURVEY("Survey"),
    FLOOR_PLAN("Floor plan"),
    RESEARCH("Research"),
    QUICK_SCAN("Quick scan"),
}

/**
 * Clamps every field to A14's documented range, replaces non-finite values
 * with the field's default and swaps inverted min/max pairs. Always succeeds —
 * there is no input this cannot turn into a valid [DisplayParams], which is why
 * `clamp_display_params()` returns void in C++ and this returns a value rather
 * than a `Result`.
 */
fun DisplayParams.clamped(): DisplayParams {
    fun f(v: Float, default: Float, lo: Float, hi: Float): Float =
        if (!v.isFinite()) default else v.coerceIn(lo, hi)

    val ps = pointSize.let { p ->
        val minPx = f(p.adaptiveMinPx, 1f, 0.5f, 64f)
        val maxPx = f(p.adaptiveMaxPx, 6f, 0.5f, 64f)
        PointSizeParams(
            mode = p.mode,
            fixedPx = f(p.fixedPx, 2f, 0.5f, 64f),
            adaptiveMinPx = minOf(minPx, maxPx),
            adaptiveMaxPx = maxOf(minPx, maxPx),
            adaptiveReferenceM = f(p.adaptiveReferenceM, 5f, 0.01f, 1000f),
            worldSizeM = f(p.worldSizeM, 0.01f, 0.0005f, 1f),
        )
    }

    fun scalar(s: ScalarColorParams, defMin: Float, defMax: Float): ScalarColorParams {
        val lo = if (s.manualMin.isFinite()) s.manualMin else defMin
        val hi = if (s.manualMax.isFinite()) s.manualMax else defMax
        return ScalarColorParams(
            autoRange = s.autoRange,
            manualMin = minOf(lo, hi),
            manualMax = maxOf(lo, hi),
            // gamma's range is (0.1, 4.0] — exclusive at the bottom in the C++
            // header, which for a float clamp means the same thing as 0.1f here
            // (pow(t, 1/0.1) is finite); documented so the difference is not
            // mistaken for a transcription slip.
            gamma = f(s.gamma, 1f, 0.1f, 4f),
            brightness = f(s.brightness, 1f, 0.1f, 3f),
            colormap = s.colormap,
            invert = s.invert,
        )
    }

    val chLo = if (clipHeightMin.isFinite()) clipHeightMin else 0f
    val chHi = if (clipHeightMax.isFinite()) clipHeightMax else 3f
    val boxMin = (0..2).map { i -> clipBoxMin.getOrElse(i) { -10f }.let { if (it.isFinite()) it else -10f } }
    val boxMax = (0..2).map { i -> clipBoxMax.getOrElse(i) { 10f }.let { if (it.isFinite()) it else 10f } }

    return copy(
        pointSize = ps,
        lodPointBudget = lodPointBudget.coerceIn(1_000, 200_000_000),
        height = scalar(height, 0f, 3f),
        intensity = scalar(intensity, 0f, 1f),
        time = scalar(time, 0f, 1f),
        fixQualityColors = if (fixQualityColors.size == 5) fixQualityColors else DisplayParams.DEFAULT_FIX_COLORS,
        edlStrength = f(edlStrength, 0.5f, 0f, 1f),
        clipHeightMin = minOf(chLo, chHi),
        clipHeightMax = maxOf(chLo, chHi),
        clipBoxMin = (0..2).map { minOf(boxMin[it], boxMax[it]) },
        clipBoxMax = (0..2).map { maxOf(boxMin[it], boxMax[it]) },
    )
}

/**
 * A14 §5's four presets, transcribed from `profile_defaults()` value for value
 * (including the ones that are not obvious: Floor plan's height *range* is
 * pinned to the clip band rather than left on auto, and Quick scan's EDL is off
 * because S3 never measured its cost).
 */
fun profileDefaults(profile: DisplayProfile): DisplayParams = when (profile) {
    DisplayProfile.SURVEY -> DisplayParams(
        pointSize = PointSizeParams(mode = PointSizeMode.ADAPTIVE, adaptiveMinPx = 1.5f, adaptiveMaxPx = 4f),
        lodPointBudget = 15_000_000,
        colorMode = ColorMode.HEIGHT,
        height = ScalarColorParams(autoRange = true, manualMin = 0f, manualMax = 3f, colormap = Colormap.SPECTRUM),
        edlEnabled = true,
        edlStrength = 0.6f,
        background = Rgba(16, 16, 20, 255),
        showTrajectory = true,
        showPoseGraph = true,
    )
    DisplayProfile.FLOOR_PLAN -> DisplayParams(
        pointSize = PointSizeParams(mode = PointSizeMode.FIXED_PIXELS, fixedPx = 1.5f),
        lodPointBudget = 8_000_000,
        colorMode = ColorMode.HEIGHT,
        height = ScalarColorParams(autoRange = false, manualMin = 1f, manualMax = 1.5f, colormap = Colormap.THERMAL),
        edlEnabled = true,
        edlStrength = 0.7f,
        clipHeightEnabled = true,
        clipHeightMin = 1f,
        clipHeightMax = 1.5f,
        showTrajectory = false,
        showPoseGraph = false,
    )
    DisplayProfile.RESEARCH -> DisplayParams(
        pointSize = PointSizeParams(mode = PointSizeMode.FIXED_PIXELS, fixedPx = 2f),
        lodPointBudget = 50_000_000,
        colorMode = ColorMode.RGB,
        edlEnabled = true,
        edlStrength = 0.4f,
        showTrajectory = true,
        showPoseGraph = true,
    )
    DisplayProfile.QUICK_SCAN -> DisplayParams(
        pointSize = PointSizeParams(mode = PointSizeMode.FIXED_PIXELS, fixedPx = 3f),
        lodPointBudget = 2_000_000,
        colorMode = ColorMode.INTENSITY,
        intensity = ScalarColorParams(colormap = Colormap.GRAYSCALE),
        edlEnabled = false,
        showTrajectory = true,
        showPoseGraph = false,
    )
}.clamped()

/**
 * A14's `PointAttributes`: per-point data `PointVertex` does not carry. Every
 * field is optional and a mode whose data is absent degrades to an RGB
 * pass-through rather than fabricating a value.
 */
data class PointAttributes(
    val intensity: Float? = null,
    val tSeconds: Double? = null,
    val fix: FixType? = null,
)

/**
 * The ground-truth per-point colour — the answer `points.mat` must reproduce.
 * Ported from `evaluate_point_color()` including its fallback table (A14 §2)
 * and its one absolute rule: **alpha is always the vertex's own alpha**, in
 * every mode, because `point_page.h` reserves alpha for LOD-fade/selection.
 */
fun evaluatePointColor(
    r: Int,
    g: Int,
    b: Int,
    a: Int,
    z: Float,
    attrs: PointAttributes,
    params: DisplayParams,
): Rgba {
    fun passthrough() = Rgba(r, g, b, a)

    fun scalar(value: Float, sp: ScalarColorParams): Rgba {
        val span = sp.manualMax - sp.manualMin
        var t = if (span <= 0f) 0f else ((value - sp.manualMin) / span).coerceIn(0f, 1f)
        if (sp.invert) t = 1f - t
        if (sp.gamma != 1f) t = t.toDouble().pow((1.0 / sp.gamma).toDouble()).toFloat()
        val c = ColormapLut.raw(sp.colormap, t)
        fun bright(v: Int) = (v * sp.brightness).roundToInt().coerceIn(0, 255)
        return Rgba(bright(c.r), bright(c.g), bright(c.b), a)
    }

    return when (params.colorMode) {
        ColorMode.RGB -> passthrough()
        ColorMode.HEIGHT -> scalar(z, params.height)
        ColorMode.INTENSITY -> {
            // The RGB-derived-intensity bridge `export/exporter.h` documents for
            // A9, so the review viewer and an export agree on what "intensity"
            // means for an uncolorized capture.
            val i = attrs.intensity ?: ((0.299f * r + 0.587f * g + 0.114f * b) / 255f)
            scalar(i, params.intensity)
        }
        ColorMode.TIME -> attrs.tSeconds?.let { scalar(it.toFloat(), params.time) } ?: passthrough()
        ColorMode.FIX_QUALITY -> attrs.fix?.let {
            val c = params.fixQualityColors.getOrElse(it.code) { DisplayParams.DEFAULT_FIX_COLORS[0] }
            Rgba(c.r, c.g, c.b, a)
        } ?: passthrough()
    }
}

/**
 * Which colour modes are *meaningful* right now, and why the others are not.
 *
 * A14 degrades [ColorMode.TIME] and [ColorMode.FIX_QUALITY] to an RGB
 * pass-through when the data is absent, which is correct but invisible: a user
 * picks "Fix quality", nothing changes, and there is no way to tell whether the
 * mode is broken or the data is missing. B10 shows the reason instead of the
 * silence.
 *
 * @param gnssActive true when a rover is attached and publishing fixes (B9)
 */
fun colorModeAvailability(gnssActive: Boolean): Map<ColorMode, String?> = mapOf(
    ColorMode.RGB to null,
    ColorMode.HEIGHT to null,
    ColorMode.INTENSITY to null,
    // PointVertex is 16 bytes of position + RGBA8 and carries no per-point
    // timestamp; A14 §2 leaves "a parallel time buffer" as an unmade A1/A14
    // decision, so there is nothing to colour by.
    ColorMode.TIME to "No per-point timestamps in this build — points carry position and RGBA only.",
    ColorMode.FIX_QUALITY to if (gnssActive) {
        null
    } else {
        "Needs a connected RTK rover — points carry no fix tag, so the colour comes from the live fix."
    },
)
