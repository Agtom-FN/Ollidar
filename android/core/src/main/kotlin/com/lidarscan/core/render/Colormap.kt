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
enum class ColorMode { RGB, HEIGHT, INTENSITY, TIME, FIX_QUALITY }

enum class Colormap { GRAYSCALE, SPECTRUM, THERMAL }

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

    fun raw(cm: Colormap, t: Float): Rgba8 = when (cm) {
        Colormap.GRAYSCALE -> grayscaleRaw(t)
        Colormap.SPECTRUM -> spectrumRaw(t)
        Colormap.THERMAL -> thermalRaw(t)
    }

    /** One colormap's 256-entry LUT, sampled the same way `build_lut()` does: i/(SIZE-1). */
    fun buildLut(cm: Colormap): Array<Rgba8> = Array(SIZE) { i -> raw(cm, i.toFloat() / (SIZE - 1)) }

    /**
     * The whole 256x3 RGBA8 texture `PointCloudRenderer` uploads: row 0 =
     * grayscale, row 1 = spectrum, row 2 = thermal (matches
     * `points.mat`'s `(colormap + 0.5) / 3.0` row-centre sample and
     * [Colormap]'s ordinal order). `width*height*4` bytes, row-major,
     * top row first — ready for `Texture.setImage`.
     */
    fun buildTextureRgba8(): ByteArray {
        val rows = arrayOf(
            buildLut(Colormap.GRAYSCALE),
            buildLut(Colormap.SPECTRUM),
            buildLut(Colormap.THERMAL),
        )
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
