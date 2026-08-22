package com.lidarscan.app.ui.theme

import androidx.compose.runtime.Immutable
import androidx.compose.ui.graphics.Color
import com.lidarscan.core.render.Colormap
import com.lidarscan.core.render.ColormapLut

/**
 * ROUND 28 item 154 — **one ramp, sampled the same way everywhere.**
 *
 * The point cloud is coloured by a GLSL texture lookup built from
 * [ColormapLut]; the Projects thumbnail was colouring itself by lerping between
 * the D6's teal and the STL-27L's sand. Two ramps, and the thumbnail's was
 * built from *sensor identity colours*, which is why a tile and the viewport
 * showing the same scan never quite looked like the same scan.
 *
 * This samples the LUT the shader samples, so a thumbnail, the Review display
 * sheet's gradient swatch and the live viewport are all reading one table. The
 * default is [Colormap.TURBO] — round 26 item 127's choice, and round 27 item
 * 141's migration target — whose floor and ceiling are both dark, which is what
 * makes a height ramp legible on a dark viewport in the first place.
 */
@Immutable
class HeightRamp(private val colormap: Colormap = Colormap.TURBO) {
    /** The ramp at `t`, clamped to [0, 1]. */
    fun at(t: Float): Color {
        val c = ColormapLut.raw(colormap, t)
        return Color(red = c.r / 255f, green = c.g / 255f, blue = c.b / 255f, alpha = 1f)
    }

    /**
     * Evenly spaced stops for a `Brush.horizontalGradient` — the swatch the
     * display sheet draws beside "Height ramp", per the mockup.
     */
    fun stops(count: Int = 8): List<Color> =
        List(count) { at(it.toFloat() / (count - 1).coerceAtLeast(1)) }

    companion object {
        val Turbo = HeightRamp(Colormap.TURBO)
    }
}
