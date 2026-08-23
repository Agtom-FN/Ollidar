package com.lidarscan.core.render

import com.lidarscan.core.measure.METRES_PER_FOOT
import com.lidarscan.core.measure.MeasureUnit
import kotlin.math.abs

/**
 * ROUND 38 item 190 — **what a colour means, in metres.**
 *
 * > *"for the display colour in height, show the pixel with color indicated by
 * > height."* — owner, 2026-08-23.
 *
 * HEIGHT mode paints every point through [ColormapLut] against a range the
 * renderer resolves ([HeightRange]), and until now the viewer showed the
 * result and never the scale. A cyan point was higher than a blue one and that
 * was the whole of what could be read off the screen: the operator could see
 * *that* the cloud was coloured by height and could not see *what height*. The
 * legend is the missing half — a ramp with the range printed on it, so a colour
 * is a number.
 *
 * ## What this file is, and what it deliberately is not
 *
 * It is the **arithmetic and the gating**: which tick sits where, what number
 * that tick carries, how that number is written in the operator's own units,
 * and whether the legend is drawn at all. It is in `:core` for the reason every
 * other decision in this package is — so it can be asserted on a JVM, without a
 * GPU and without a device.
 *
 * It is **not** a second opinion about the range. The values printed here come
 * from a [HeightRange.Range] the caller hands in, and the only range the Review
 * screen ever hands in is the one the renderer has actually uploaded to the
 * shader's `valueMin`/`valueMax`
 * (`PointCloudRenderer.appliedValueRange()` → `ReviewUiState.heightRange`).
 * That is the item's "same source of truth" requirement discharged **by having
 * no second source at all**, rather than by two computations that agree today:
 * there is no path by which this file could compute an auto-range, so there is
 * no path by which the legend could disagree with the pixels.
 *
 * The ramp COLOURS are the same story one layer up — the swatch is drawn from
 * `HeightRamp`, which samples the same [ColormapLut] the GLSL texture is built
 * from (round 28 item 154's "one ramp, sampled the same way everywhere").
 */
object HeightLegend {

    /**
     * Where the labelled ticks sit, as fractions of the ramp measured from its
     * **bottom** — which is [HeightRange.Range.min], because the shader
     * normalises `min → t = 0` and the ramp is drawn with `t = 0` at the
     * bottom, the way a height axis reads.
     *
     * Three, not five: the bar is about 40 % of a phone viewport tall, its
     * labels are 12 sp mono, and a fourth label between the middle and an end
     * collides with both at fontScale 1.3. Ends and middle are also the three
     * a reader actually uses — the extremes tell them the span, the middle
     * tells them whether the ramp is linear (it is) without having to trust it.
     */
    val TICKS: List<Float> = listOf(0f, 0.5f, 1f)

    /** A resolved legend: the ticks, bottom-first, each with its printed label. */
    data class Model(
        val range: HeightRange.Range,
        val unit: MeasureUnit,
        /** Bottom-first, parallel to [TICKS]. */
        val labels: List<String>,
    ) {
        val bottomLabel: String get() = labels.first()
        val middleLabel: String get() = labels[labels.size / 2]
        val topLabel: String get() = labels.last()
    }

    /**
     * Whether the legend is drawn at all. Four gates, and each of them is one
     * of the item's own clauses:
     *
     *  1. **HEIGHT only.** In INTENSITY the ramp means reflectance, not metres,
     *     and a legend printed in metres beside it would be a lie. Item 190(b):
     *     *"hidden entirely in INTENSITY/other modes"*. The honest INTENSITY
     *     legend is a different feature with a different axis and it is not
     *     this round's.
     *  2. **Chrome.** *"and when the controls are hidden by the tap-to-hide (it
     *     counts as chrome)"* — the tap-to-hide exists so the operator can LOOK
     *     at the cloud (see [ViewerChrome]), and a bar with three labels on it
     *     is exactly the sort of thing they are hiding.
     *  3. **A range must exist.** Before the first page lands the renderer has
     *     uploaded nothing, so there is no range and the legend has nothing
     *     true to say. It says nothing rather than 0.00–0.00.
     *  4. **The span must be real.** [HeightRange.EPSILON_M], the same
     *     millimetre floor the range resolver and the thumbnail use. A
     *     zero-span range prints one number three times, which is not a scale.
     *     (The *auto* path can never produce one — [HeightRange.resolve] opens
     *     a 1 m window on a degenerate cloud — but a manual range whose min and
     *     max are equal can, and that is a shape a saved `project.json` is
     *     allowed to hold.)
     */
    fun visible(
        colorMode: ColorMode,
        chromeVisible: Boolean,
        range: HeightRange.Range?,
    ): Boolean {
        if (colorMode != ColorMode.HEIGHT) return false
        if (!chromeVisible) return false
        val r = range ?: return false
        if (!r.min.isFinite() || !r.max.isFinite()) return false
        return r.span >= HeightRange.EPSILON_M
    }

    /**
     * The height, in metres, at fraction [t] up the ramp — the exact inverse of
     * `points.mat`'s normalisation (and of [HeightRange.normalise], which is
     * that line's JVM twin). `t` is clamped, so a caller that asks for a tick
     * off the end of the bar gets the end of the bar.
     */
    fun valueAt(range: HeightRange.Range, t: Float): Float =
        range.min + (range.max - range.min) * t.coerceIn(0f, 1f)

    /**
     * One tick's label, in the operator's units.
     *
     * **Decimal feet, not feet-and-inches**, and this is the one place this
     * file deliberately does not do what `formatDistance` does. That formatter
     * renders imperial as `12' 5.28"` because a *distance* is read against a
     * tape; a legend tick is read against the two ticks above and below it, and
     * three labels of different widths in a 12 dp gutter stop lining up. The
     * same reasoning rules out metres-below-a-metre becoming millimetres: an
     * axis whose bottom label says `270 mm` and whose top says `0.82 m` is an
     * axis the reader has to convert in their head before they can compare its
     * own two ends.
     *
     * So: **one unit for the whole axis, always suffixed**, with the precision
     * chosen by magnitude — 2 dp under ten, 1 dp under a hundred, whole
     * numbers above. A range can be negative (the cloud's origin is where the
     * session started, which is rarely the floor), and `-0.00` is printed as
     * `0.00`, because a minus sign on a zero reads as a measurement rather than
     * as rounding.
     */
    fun format(metres: Float, unit: MeasureUnit): String {
        val v = when (unit) {
            MeasureUnit.METERS -> metres.toDouble()
            MeasureUnit.FEET -> metres.toDouble() / METRES_PER_FOOT
        }
        val decimals = when {
            !v.isFinite() -> 0
            abs(v) < 10.0 -> 2
            abs(v) < 100.0 -> 1
            else -> 0
        }
        val text = "%.${decimals}f".format(v)
        // "-0.00" is rounding, not a measurement below zero.
        val cleaned = if (text.startsWith("-") && text.drop(1).none { it in '1'..'9' }) text.drop(1) else text
        return "$cleaned ${unit.abbreviation}"
    }

    /** The whole legend for [range] in [unit] — labels bottom-first, parallel to [TICKS]. */
    fun model(range: HeightRange.Range, unit: MeasureUnit): Model =
        Model(
            range = range,
            unit = unit,
            labels = TICKS.map { format(valueAt(range, it), unit) },
        )
}
