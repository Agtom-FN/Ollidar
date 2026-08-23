package com.lidarscan.core.measure

import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * B11 — the measure tool's geometry, in `:core` so it is JVM-testable without a
 * GPU (Tech Spec §3.13's "Review (viewer, display params, **measure**, plan
 * view, export)").
 *
 * The picking problem, stated precisely: the user taps a screen pixel; the
 * renderer holds a camera and a few million points. There is no depth buffer to
 * read back (Filament exposes no readback path this app uses), so the pick is
 * done the way every point-cloud viewer without GPU picking does it — project
 * candidate points to screen and take the nearest one inside a radius, with
 * **depth as the tie-break** so a tap that lands on a near wall does not select
 * a point on the far wall behind it.
 */

data class Vec3(val x: Float, val y: Float, val z: Float) {
    fun distanceTo(o: Vec3): Double {
        val dx = (x - o.x).toDouble()
        val dy = (y - o.y).toDouble()
        val dz = (z - o.z).toDouble()
        return sqrt(dx * dx + dy * dy + dz * dz)
    }
}

/** A picked point plus the evidence needed to decide whether to trust the pick. */
data class PickResult(
    val point: Vec3,
    /** Screen-space distance from the tap, in pixels. */
    val screenDistancePx: Float,
    /** View-space depth (metres in front of the camera). */
    val depthM: Float,
)

/**
 * Projects `points` with `viewProjection` (ROW-major 4×4, the convention
 * everything crossing this app's JNI uses) and returns the nearest point to
 * (tapX, tapY) within `radiusPx`.
 *
 * Returns null when nothing is in range — which the UI must *say*, because a
 * silent no-op on a tap is indistinguishable from a frozen screen.
 *
 * @param viewProjection row-major 4×4 mapping world → clip
 * @param viewportW/@param viewportH pixels
 * @param stride examine every Nth point. A tap does not need a million-point
 *   scan to land within a pixel or two of the right return, and the scan runs
 *   on the UI's coroutine; 1 is exact.
 */
fun pickNearestPoint(
    points: List<Vec3>,
    viewProjection: DoubleArray,
    viewportW: Int,
    viewportH: Int,
    tapX: Float,
    tapY: Float,
    radiusPx: Float = 48f,
    stride: Int = 1,
): PickResult? {
    require(viewProjection.size == 16) { "viewProjection must be a 4x4 (16 doubles), was ${viewProjection.size}" }
    if (points.isEmpty() || viewportW <= 0 || viewportH <= 0) return null
    val step = stride.coerceAtLeast(1)

    var best: PickResult? = null
    var i = 0
    while (i < points.size) {
        val p = points[i]
        i += step

        // Row-major: clip = M * [x y z 1]^T
        val cx = viewProjection[0] * p.x + viewProjection[1] * p.y + viewProjection[2] * p.z + viewProjection[3]
        val cy = viewProjection[4] * p.x + viewProjection[5] * p.y + viewProjection[6] * p.z + viewProjection[7]
        val cz = viewProjection[8] * p.x + viewProjection[9] * p.y + viewProjection[10] * p.z + viewProjection[11]
        val cw = viewProjection[12] * p.x + viewProjection[13] * p.y + viewProjection[14] * p.z + viewProjection[15]

        // Behind the camera (or on the plane) — not visible, so not pickable.
        if (cw <= 1e-6) continue
        val ndcX = cx / cw
        val ndcY = cy / cw
        if (ndcX < -1.2 || ndcX > 1.2 || ndcY < -1.2 || ndcY > 1.2) continue

        val sx = ((ndcX + 1.0) * 0.5 * viewportW).toFloat()
        // NDC +y is up, screen +y is down.
        val sy = ((1.0 - (ndcY + 1.0) * 0.5) * viewportH).toFloat()
        val dx = sx - tapX
        val dy = sy - tapY
        val d = sqrt((dx * dx + dy * dy).toDouble()).toFloat()
        if (d > radiusPx) continue

        val depth = cw.toFloat()
        val cur = best
        // Nearest to the tap wins; within a pixel of each other, the CLOSER
        // point to the camera wins. Without that tie-break a tap on a near
        // surface routinely selects the far wall seen through the gaps between
        // its points, which is the classic point-picking surprise.
        val better = when {
            cur == null -> true
            d < cur.screenDistancePx - 1f -> true
            abs(d - cur.screenDistancePx) <= 1f && depth < cur.depthM -> true
            else -> false
        }
        if (better) best = PickResult(p, d, depth)
    }
    return best
}

/**
 * Which component of a [Vec3] points at the sky.
 *
 * **`y`, and this file is the FOURTH site that had to be told so.** Round 28
 * item 154 found `points.mat`, `PointCloudRenderer` and the auto-range each
 * independently believing the up axis was `z`, fixed all three, and wrote the
 * derivation down at `com.lidarscan.core.render.HeightRange.AXIS`: the runtime
 * frame is ARCore's, which is Y-up, because `CaptureArController.publishPose`
 * pushes `camera.pose` into the engine verbatim. It did not find this one,
 * because B11's measure geometry is in a different package and names its
 * components rather than indexing them.
 *
 * The consequence was visible on every measurement the Review screen has ever
 * printed: [Measurement.horizontalM] summed `x` and **`y`** — the plan distance
 * with the *vertical* mixed into it — and the card's `Δz` line reported a
 * horizontal component under a vertical name. Found in round 38 while wiring
 * item 190's height read-out, which is the first thing in the app that had to
 * ask a picked point how high it was.
 *
 * Stated here as a name rather than imported as an index, because `Vec3` has
 * fields and not a triple; `MeasureTest` pins it against `HeightRange.AXIS` so
 * the two cannot drift.
 */
val Vec3.heightM: Double get() = y.toDouble()

/** A completed two-tap measurement. */
data class Measurement(val from: Vec3, val to: Vec3) {
    val distanceM: Double get() = from.distanceTo(to)
    val deltaXM: Double get() = (to.x - from.x).toDouble()
    val deltaYM: Double get() = (to.y - from.y).toDouble()
    val deltaZM: Double get() = (to.z - from.z).toDouble()

    /**
     * The rise from [from] to [to] — the up-axis component, per [heightM].
     *
     * Was published as `deltaZM` by the read-out, which is a horizontal axis in
     * this frame. See [heightM].
     */
    val verticalM: Double get() = to.heightM - from.heightM

    /**
     * Horizontal (plan) distance — the number a floor plan actually wants.
     *
     * The two components that are **not** [heightM]: `x` and `z`. It read
     * `x` and `y` until round 38, which put the whole rise of a staircase into
     * the "horizontal" number.
     */
    val horizontalM: Double get() = sqrt(deltaXM * deltaXM + deltaZM * deltaZM)
}

/** Distance unit for the measure readout. Mirrors `:app`'s `Units`, kept here so `:core` needs no `:app` dependency. */
enum class MeasureUnit(val abbreviation: String) { METERS("m"), FEET("ft") }

/**
 * The international foot, exactly (the US survey foot differs by 2 ppm — see
 * [formatDistance]).
 *
 * Public since round 38: `HeightLegend` writes the same unit toggle onto the
 * viewer's height axis and there is no version of "two constants that are both
 * 0.3048" worth having.
 */
const val METRES_PER_FOOT = 0.3048

/**
 * Formats a distance for the measure HUD.
 *
 * Feet are rendered as **feet + inches** below 100 ft, because "12.47 ft" is not
 * how anyone using imperial units reads a tape, and the whole point of the unit
 * toggle is to match the user's instrument. The conversion uses the
 * international foot (0.3048 m exactly) — the US survey foot differs by 2 ppm
 * and choosing it silently would be a 2 mm error per kilometre nobody asked for.
 */
fun formatDistance(metres: Double, unit: MeasureUnit, precisionMm: Boolean = true): String = when (unit) {
    MeasureUnit.METERS -> when {
        abs(metres) < 1.0 && precisionMm -> "${(metres * 1000.0).roundToInt()} mm"
        abs(metres) < 10.0 -> "%.3f m".format(metres)
        else -> "%.2f m".format(metres)
    }
    MeasureUnit.FEET -> {
        val totalFeet = metres / METRES_PER_FOOT
        if (abs(totalFeet) < 100.0) {
            val sign = if (totalFeet < 0) "-" else ""
            val abs = abs(totalFeet)
            var feet = abs.toInt()
            var inches = (abs - feet) * 12.0
            // 11.98" must read 12' 0", not 11' 12".
            if (inches >= 11.995) {
                feet += 1
                inches = 0.0
            }
            "$sign$feet' %.2f\"".format(inches)
        } else {
            "%.2f ft".format(totalFeet)
        }
    }
}
