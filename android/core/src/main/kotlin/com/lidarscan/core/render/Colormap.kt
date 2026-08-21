package com.lidarscan.core.render

import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * B4: a Kotlin port of `scanengine::cloud::display_params.cpp`'s three
 * procedural colormaps (`engine/docs/A14-display.md` §3 —
 * `grayscale_raw`/`spectrum_raw`/`thermal_raw`, and `evaluate_colormap`'s
 * LUT-interpolation). Byte-for-byte the same construction, not an
 * approximation: `engine/include/scanengine/cloud/display_params.h` has no
 * C ABI mirror at all (`engine/capi/scanengine_c.h` exposes none of
 * `display_params.h`'s API — confirmed by reading it end to end), so there
 * is no JNI call this renderer could make to fetch `colormap_lut()` bytes
 * from the engine directly. Reimplementing the closed-form math from A14's
 * own published formulas (rather than approximating with, say, a named
 * third-party palette) is what keeps `PointCloudRenderer`'s height/intensity
 * colouring in agreement with `evaluate_point_color()`'s ground truth — the
 * same "shader agrees by construction" goal A14's own header states, just
 * carried one layer further (C++ formula -> Kotlin formula -> GLSL LUT
 * sample) because the JNI boundary has no route for the LUT bytes
 * themselves.
 *
 * [ColorMode]/[Colormap]/[PointSizeMode] below mirror
 * `scanengine::cloud::ColorMode`/`Colormap`/`PointSizeMode` int-for-int
 * (`ordinal` == the C++ enum's underlying value == `points.mat`'s
 * `colorMode`/`colormap`/`pointSizeMode` material parameters) — keep the
 * declaration order in lock-step with `display_params.h` if it changes.
 */
/**
 * ROUND 11 (owner item 42) added [COVERAGE], and it is the ONE value in this
 * enum whose ordinal must never cross the C ABI.
 *
 * Every other mode is a shader branch: the engine's `scanengine::cloud::ColorMode`
 * has the same five values in the same order and `points.mat` switches on the
 * integer. Coverage is not a shader branch at all — it is a per-point colour the
 * RENDERER computes from a density grid it owns and writes into its own GPU copy
 * of the vertices, so the shader is asked for plain RGB pass-through (mode 0)
 * and never learns that coverage exists.
 *
 * That is what keeps the promise in item 42 that coverage is "never written into
 * the container": the tint lives in a Filament VertexBuffer for as long as the
 * live view does, and the engine's PageStore — which is also the map cache that
 * gets sealed — is never touched. `PointCloudRenderer.shaderColorMode()` is the
 * single place the translation happens.
 */
enum class ColorMode { RGB, HEIGHT, INTENSITY, TIME, FIX_QUALITY, COVERAGE }

/**
 * ROUND 26 (owner item 127) added [TURBO], and it is the ONE value in this enum
 * the engine has never heard of.
 *
 * `scanengine::cloud::Colormap` has exactly three values — grayscale, spectrum,
 * thermal — and the paragraph above says the ordinals mirror it int-for-int, so
 * a fourth needs its cost stated rather than assumed. The cost is nothing, for
 * three reasons that were each checked rather than remembered:
 *
 *  1. **Nothing serialises this ordinal across JNI.** `engine/capi/scanengine_c.h`
 *     exposes none of `display_params.h` — no struct, no function, no enum — which
 *     is the same finding [ColormapLut]'s header records for `colormap_lut()` and
 *     `DisplayParams`' for the whole parameter model. There is no C ABI call that
 *     takes a `Colormap`, so there is no call a fourth value could break. **The
 *     engine is not modified and its ABI stays 12.**
 *  2. **Persistence is by NAME, not by ordinal.** `ScalarColorParams.colormap` is
 *     a `kotlinx.serialization` enum field, which writes `"TURBO"` / `"THERMAL"`
 *     into `project.json`. Appending a value therefore cannot renumber what an
 *     existing project already saved — and putting TURBO **fourth**, after
 *     THERMAL, additionally leaves ordinals 0..2 undisturbed for the one consumer
 *     that *is* numeric: `points.mat`'s `colormap` material parameter.
 *  3. **The shader indexes a texture the app builds.** The LUT row a fragment
 *     samples comes from [ColormapLut.buildTextureRgba8], which is Kotlin — so
 *     the fourth row exists the moment the enum does, provided the divisor in
 *     `points.mat` matches [ColormapLut.ROWS] (see the comment there).
 *
 * What TURBO is *not* is a claim about `evaluate_point_color()`. The other three
 * are ports of the engine's own closed-form maths and agree with it by
 * construction; Turbo is an APP-SIDE-ONLY ramp with no engine counterpart to
 * agree or disagree with, which is exactly why it is safe to add here and would
 * not be safe to add to [ColorMode] (whose ordinals the shader branches on
 * against the engine's own list).
 */
enum class Colormap { GRAYSCALE, SPECTRUM, THERMAL, TURBO }

enum class PointSizeMode { FIXED_PIXELS, ADAPTIVE, WORLD_SIZE }

object ColormapLut {
    const val SIZE = 256

    /** RGBA8, in [0, 255]. */
    data class Rgba8(val r: Int, val g: Int, val b: Int, val a: Int)

    private fun toU8Round(v: Float): Int = v.roundToInt().coerceIn(0, 255)

    /** `grayscale_raw` (display_params.cpp) — r=g=b=round(t*255). */
    fun grayscaleRaw(t: Float): Rgba8 {
        val v = toU8Round(t.coerceIn(0f, 1f) * 255f)
        return Rgba8(v, v, v, 255)
    }

    /**
     * `spectrum_raw` — full-saturation HSV hue sweep, h = 240°(1-t) -> 0°
     * (blue -> red), standard 60°-segment HSV->RGB with no trig, exactly as
     * display_params.cpp derives it.
     */
    fun spectrumRaw(t: Float): Rgba8 {
        val h = 240.0f * (1.0f - t.coerceIn(0f, 1f))
        val hp = h / 60.0f
        val x = 1.0f - abs(hp.mod(2.0f) - 1.0f)
        val (r1, g1, b1) = when {
            hp < 1.0f -> Triple(1f, x, 0f)
            hp < 2.0f -> Triple(x, 1f, 0f)
            hp < 3.0f -> Triple(0f, 1f, x)
            hp < 4.0f -> Triple(0f, x, 1f)
            hp < 5.0f -> Triple(x, 0f, 1f)
            else -> Triple(1f, 0f, x)
        }
        return Rgba8(toU8Round(r1 * 255f), toU8Round(g1 * 255f), toU8Round(b1 * 255f), 255)
    }

    private fun lerpRgba(a: Rgba8, b: Rgba8, t: Float): Rgba8 {
        val ct = t.coerceIn(0f, 1f)
        fun lerp(x: Int, y: Int) = (x + (y - x) * ct).roundToInt().coerceIn(0, 255)
        return Rgba8(lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a))
    }

    /** `thermal_raw` — black -> red -> yellow -> white, piecewise-linear over three equal thirds. */
    fun thermalRaw(t: Float): Rgba8 {
        val tt = t.coerceIn(0f, 1f)
        val black = Rgba8(0, 0, 0, 255)
        val red = Rgba8(255, 0, 0, 255)
        val yellow = Rgba8(255, 255, 0, 255)
        val white = Rgba8(255, 255, 255, 255)
        return when {
            tt < 1.0f / 3.0f -> lerpRgba(black, red, tt * 3.0f)
            tt < 2.0f / 3.0f -> lerpRgba(red, yellow, (tt - 1.0f / 3.0f) * 3.0f)
            else -> lerpRgba(yellow, white, (tt - 2.0f / 3.0f) * 3.0f)
        }
    }

    /**
     * ROUND 26 (owner item 127)'s Turbo ramp: dark blue -> blue -> cyan ->
     * green -> yellow -> orange -> dark red, piecewise-linear through the ten
     * anchor stops below.
     *
     * **This is Turbo's SHAPE, not Google's table.** The published Turbo
     * colormap is a 256-entry lookup table; reproducing it bit-exactly means
     * vendoring a 768-byte data blob with no derivation anyone can check, which
     * is the opposite of what every other ramp in this file is (a formula
     * transcribed from a document, next to the document's own reasoning). The
     * anchors here are Turbo's published stops at 1/8 intervals — plus one extra
     * at 0.9375 where the ramp's curvature into the dark red terminus is
     * sharpest and a straight 0.875 -> 1.0 segment would visibly cut the corner
     * — interpolated linearly. The result is the perceptually-ordered rainbow
     * item 127 asked for, with Turbo's actual advantage over [spectrumRaw]
     * preserved: it starts and ends DARK, so the top and bottom of a height
     * range read as distinct instead of both saturating.
     *
     * **It is deliberately not one of the engine's three.** [grayscaleRaw],
     * [spectrumRaw] and [thermalRaw] are ports whose job is to agree with
     * `evaluate_point_color()` by construction; this one has no C++ counterpart
     * to agree with, so approximating a named external palette costs nothing —
     * there is no ground truth being contradicted. See [Colormap]'s header for
     * why a fourth value is free across the whole JNI/persistence boundary.
     */
    private val TURBO_STOPS: Array<Pair<Float, Rgba8>> = arrayOf(
        0.0000f to Rgba8(48, 18, 59, 255),
        0.1250f to Rgba8(70, 107, 227, 255),
        0.2500f to Rgba8(36, 168, 246, 255),
        0.3750f to Rgba8(30, 215, 201, 255),
        0.5000f to Rgba8(95, 241, 135, 255),
        0.6250f to Rgba8(170, 247, 73, 255),
        0.7500f to Rgba8(231, 215, 48, 255),
        0.8750f to Rgba8(253, 150, 36, 255),
        0.9375f to Rgba8(238, 86, 18, 255),
        1.0000f to Rgba8(122, 4, 3, 255),
    )

    /**
     * Turbo, evaluated at [t]. See [TURBO_STOPS] for what this ramp is and is
     * not. `t` is clamped to [0, 1] like every other ramp here, so a caller that
     * has not normalised its scalar gets the endpoint colour rather than an
     * extrapolated one.
     *
     * Every stop is a value the anchor table states EXACTLY: the segment search
     * lands on `tt == t1` for the anchors themselves, where the interpolation
     * fraction is `(t1 - t0) / (t1 - t0)` — literally 1.0, not 1.0 within float
     * error — so [lerpRgba] returns the upper stop unrounded. That is what lets
     * `ColormapLutTest` pin 0.25 / 0.5 / 0.75 as equalities rather than deltas.
     */
    fun turboRaw(t: Float): Rgba8 {
        val tt = t.coerceIn(0f, 1f)
        for (i in 0 until TURBO_STOPS.size - 1) {
            val (t0, c0) = TURBO_STOPS[i]
            val (t1, c1) = TURBO_STOPS[i + 1]
            if (tt <= t1) return lerpRgba(c0, c1, (tt - t0) / (t1 - t0))
        }
        return TURBO_STOPS.last().second
    }

    fun raw(cm: Colormap, t: Float): Rgba8 = when (cm) {
        Colormap.GRAYSCALE -> grayscaleRaw(t)
        Colormap.SPECTRUM -> spectrumRaw(t)
        Colormap.THERMAL -> thermalRaw(t)
        // ROUND 26 (item 127): app-side only — see [Colormap]'s header.
        Colormap.TURBO -> turboRaw(t)
    }

    /** One colormap's 256-entry LUT, sampled the same way `build_lut()` does: i/(SIZE-1). */
    fun buildLut(cm: Colormap): Array<Rgba8> = Array(SIZE) { i -> raw(cm, i.toFloat() / (SIZE - 1)) }

    /**
     * How many rows [buildTextureRgba8] produces — **one per [Colormap], and
     * that is the definition, not a coincidence.**
     *
     * ROUND 26 (owner item 127) added a fourth colormap and found three places
     * that each independently believed there were three: this builder's
     * hard-coded row array, `points.mat`'s `/ 3.0` row-centre divisor, and the
     * `Texture.Builder().height(3)` that allocates the GPU copy. Two of those
     * fail SILENTLY when they disagree — the shader samples a row centre that is
     * off by a fraction of a texel and points come out the wrong colour, with no
     * error anywhere. So the count is stated once, here, derived from the enum
     * itself, and the other sites are required to spell its name.
     */
    val ROWS: Int = Colormap.entries.size

    /**
     * The whole 256 x [ROWS] RGBA8 texture `PointCloudRenderer` uploads: one row
     * per [Colormap] **in ordinal order**, so row N is `Colormap.entries[N]` —
     * row 0 grayscale, row 1 spectrum, row 2 thermal, row 3 turbo — which is
     * what makes `points.mat`'s `(colormap + 0.5) / ROWS` row-centre sample land
     * on the ramp the material parameter names. `width*height*4` bytes,
     * row-major, top row first — ready for `Texture.setImage`.
     *
     * ROUND 26 (item 127): built by iterating [Colormap.entries] rather than
     * from a literal array of three, so a fifth colormap cannot desync the
     * texture from the enum — the row appears the moment the value does.
     */
    fun buildTextureRgba8(): ByteArray {
        val rows = Colormap.entries.map { buildLut(it) }
        val out = ByteArray(SIZE * rows.size * 4)
        var i = 0
        for (row in rows) {
            for (px in row) {
                out[i++] = px.r.toByte()
                out[i++] = px.g.toByte()
                out[i++] = px.b.toByte()
                out[i++] = px.a.toByte()
            }
        }
        return out
    }
}
