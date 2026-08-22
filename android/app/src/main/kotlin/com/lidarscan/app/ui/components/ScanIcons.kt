package com.lidarscan.app.ui.components

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Contrast
import androidx.compose.material.icons.filled.Straighten
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp

/**
 * ROUND 28 item 168 — **the icon language, as the owner decided it against the
 * mockup sheet.**
 *
 * Three of the app's icon buttons were wrong in a way that a style guide cannot
 * fix, because the problem was *collision* rather than aesthetics:
 *
 *  * **Advanced (scan-local settings) wore the Settings tab's glyph.** Both were
 *    the sliders idea, so pressing sliders in one place gave per-scan controls
 *    and pressing sliders in another gave app settings. The tab moved to `Menu`,
 *    and Advanced becomes three **vertical** faders — deliberately *not*
 *    `Icons.Filled.Tune`, which is horizontal and is the glyph the tab used to
 *    carry. The owner's note is exact about this: "the whole point is Advanced
 *    must not look like the Settings tab."
 *  * **Measure** is the horizontal ruler (mockup option M1), which is
 *    `Icons.Filled.Straighten` — a ruler bar with tick marks, matching the
 *    mockup's drawing closely enough that a custom vector would be a copy.
 *  * **Display** is the contrast half-circle (mockup option D1), which is
 *    `Icons.Filled.Contrast` — the same circle-with-a-filled-half the mockup
 *    draws.
 *
 * Where a stock glyph matches the mockup it is used; only [AdvancedFaders] is
 * hand-built, and only because nothing in the Material set is a vertical fader.
 *
 * The vector below is transcribed from the mockup's own `D3` SVG: three
 * full-height rails at x = 6, 12, 18 with a filled knob on each at a different
 * height, 2 dp strokes, on a 24 × 24 viewport — the same geometry the owner
 * approved, so what ships is what he looked at.
 */
object ScanIcons {

    /** Mockup M1 — the horizontal ruler. */
    val Measure: ImageVector get() = Icons.Filled.Straighten

    /** Mockup D1 — the contrast half-circle. */
    val Display: ImageVector get() = Icons.Filled.Contrast

    /** Mockup D3 — three vertical faders. See the object header for why it is custom. */
    val AdvancedFaders: ImageVector by lazy {
        ImageVector.Builder(
            name = "AdvancedFaders",
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f,
        ).apply {
            val stroke = SolidColor(Color.Black)
            // The three rails, full height.
            listOf(6f, 12f, 18f).forEach { x ->
                path(
                    stroke = stroke,
                    strokeLineWidth = 2f,
                    strokeLineCap = StrokeCap.Round,
                    strokeLineJoin = StrokeJoin.Round,
                ) {
                    moveTo(x, 4f)
                    lineTo(x, 20f)
                }
            }
            // The three knobs, each at its own height — which is what makes the
            // glyph read as *set to something* rather than as a bar chart.
            listOf(6f to 9f, 12f to 15f, 18f to 7f).forEach { (cx, cy) ->
                path(fill = SolidColor(Color.Black)) {
                    moveTo(cx - 2.2f, cy)
                    arcToRelative(2.2f, 2.2f, 0f, true, true, 4.4f, 0f)
                    arcToRelative(2.2f, 2.2f, 0f, true, true, -4.4f, 0f)
                    close()
                }
            }
        }.build()
    }
}
