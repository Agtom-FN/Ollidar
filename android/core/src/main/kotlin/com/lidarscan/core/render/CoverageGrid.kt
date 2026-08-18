package com.lidarscan.core.render

import kotlin.math.floor
import kotlin.math.roundToInt

/**
 * ROUND 11 item 42 — coverage colouring: which parts of the room are still thin.
 *
 * > "Live-view mode tinting the map by local return density (thin = warning
 * >  tint, dense = normal) so the operator sees gaps while still in the room
 * >  and revisits instead of walking slow everywhere. Cheap deterministic
 * >  binning, honest at the point budget; live-only display concern, never
 * >  written into the container."
 *
 * The last sentence is the constraint that shapes the whole file. Density is a
 * **view**, not data: the container stores what the sensor measured, and a
 * per-point "how many neighbours did you have at 11:07" is a fact about the
 * walk, not about the room. So this produces a colour and nothing else, and the
 * renderer applies it to its own GPU copy of the vertices.
 *
 * ## Why a voxel count and not a nearest-neighbour radius
 *
 * A k-NN density estimate is the textbook answer and it is the wrong one here:
 * it costs a spatial index rebuild on a stream that is growing at ~1,450
 * points/s, and it is not deterministic under a changing point order unless the
 * index is too. A fixed-lattice voxel count is O(1) per point, needs no index,
 * and — because the lattice is anchored at the world origin and not at the
 * cloud's bounds — gives the SAME cell for the same coordinate no matter what
 * order the points arrived in or how many there are. That is what makes the
 * tint reproducible.
 *
 * 25 cm is chosen from the sensor rather than from taste: a COIN-D6 at 10 Hz
 * with 400 returns per revolution puts ~40 returns into a 25 cm patch of wall
 * in one pass at walking speed, so "one pass" and "several passes" land on
 * opposite sides of the ramp and a half-covered wall reads as half-covered.
 *
 * ## Honest at the point budget
 *
 * The renderer draws a decimated subset of the cloud when the LOD budget bites.
 * The counts here must come from **every point the engine published**, not from
 * the drawn subset — otherwise the tint would report the decimation rather than
 * the coverage, and a well-scanned room would go amber the moment it got big
 * enough to decimate. `:app` feeds this from the page upload path, before LOD.
 *
 * Not thread-safe; the renderer owns one and touches it on its own thread.
 */
class CoverageGrid(
    /** Voxel pitch, metres. */
    val cellSizeM: Float = DEFAULT_CELL_M,
    /** At or below this many returns in a cell, the tint is full warning. */
    val thinCount: Int = DEFAULT_THIN,
    /** At or above this many, the point is drawn normally. */
    val denseCount: Int = DEFAULT_DENSE,
) {
    private val cells = HashMap<Long, Int>()

    val cellCount: Int get() = cells.size

    var pointsCounted: Long = 0L
        private set

    fun clear() {
        cells.clear()
        pointsCounted = 0L
    }

    fun add(x: Float, y: Float, z: Float) {
        if (!x.isFinite() || !y.isFinite() || !z.isFinite()) return
        val k = key(x, y, z)
        cells[k] = (cells[k] ?: 0) + 1
        pointsCounted++
    }

    fun countAt(x: Float, y: Float, z: Float): Int = cells[key(x, y, z)] ?: 0

    /**
     * 0.0 = as thin as it gets, 1.0 = fully covered. Linear between
     * [thinCount] and [denseCount], which is deliberate: a log ramp would make
     * the difference between one pass and two passes almost invisible, and that
     * difference is the entire message.
     */
    fun coverageAt(x: Float, y: Float, z: Float): Float = coverageOf(countAt(x, y, z))

    fun coverageOf(count: Int): Float {
        if (denseCount <= thinCount) return 1f
        val t = (count - thinCount).toFloat() / (denseCount - thinCount).toFloat()
        return t.coerceIn(0f, 1f)
    }

    fun levelOf(count: Int): CoverageLevel = when {
        count <= thinCount -> CoverageLevel.THIN
        count >= denseCount -> CoverageLevel.DENSE
        else -> CoverageLevel.PARTIAL
    }

    /**
     * The tint for a point, given its own scalar shade (the grayscale/intensity
     * byte the rest of the app already draws) and its cell's coverage.
     *
     * Returns packed 0xAARRGGBB.
     *
     * The ramp: a fully covered point keeps its own shade untouched, so a
     * well-scanned room in coverage mode looks exactly like the grayscale
     * intensity view the owner asked for as the default (ROUND 10 item 39) —
     * which is what "works with the intensity/grayscale default" has to mean.
     * As coverage falls, the point is pulled toward a warm amber and, at the
     * thin end, brightened, because a thin region is by definition made of few
     * points and a dim tint on few points is invisible.
     */
    fun tint(shade: Int, coverage: Float): Int {
        val c = coverage.coerceIn(0f, 1f)
        val s = shade.coerceIn(0, 255)
        val warn = 1f - c
        // Amber, not red: red is already the app's failure colour (no-data
        // banner, tracking lost) and "thin here" is not a failure.
        val wr = 255f
        val wg = 176f
        val wb = 48f
        // Lift the floor so a nearly-empty cell is legible against the
        // background even though it holds a handful of points.
        val lift = 0.35f + 0.65f * (s / 255f)
        val r = (s * c + wr * lift * warn).roundToInt().coerceIn(0, 255)
        val g = (s * c + wg * lift * warn).roundToInt().coerceIn(0, 255)
        val b = (s * c + wb * lift * warn).roundToInt().coerceIn(0, 255)
        return (0xFF shl 24) or (r shl 16) or (g shl 8) or b
    }

    fun tintAt(shade: Int, x: Float, y: Float, z: Float): Int = tint(shade, coverageAt(x, y, z))

    /**
     * The share of occupied cells that are still THIN — the one number worth
     * putting on the screen beside the tint, because it answers "am I nearly
     * done" without the operator having to read the colours.
     */
    fun thinFraction(): Float {
        if (cells.isEmpty()) return 0f
        var thin = 0
        for (v in cells.values) if (v <= thinCount) thin++
        return thin.toFloat() / cells.size.toFloat()
    }

    private fun key(x: Float, y: Float, z: Float): Long {
        val i = floor(x / cellSizeM).toLong()
        val j = floor(y / cellSizeM).toLong()
        val k = floor(z / cellSizeM).toLong()
        // 21 bits each, biased — the same packing scheme post/point_grid.h uses,
        // and exact for +/-1,048,575 cells (262 km at 25 cm).
        return ((i + BIAS) and MASK shl 42) or ((j + BIAS) and MASK shl 21) or ((k + BIAS) and MASK)
    }

    companion object {
        const val DEFAULT_CELL_M = 0.25f
        const val DEFAULT_THIN = 8
        const val DEFAULT_DENSE = 40
        private const val BIAS = 1_048_576L
        private const val MASK = 0x1FFFFFL
    }
}

enum class CoverageLevel { THIN, PARTIAL, DENSE }
