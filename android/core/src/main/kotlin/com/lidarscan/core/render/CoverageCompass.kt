package com.lidarscan.core.render

import kotlin.math.atan2
import kotlin.math.floor
import kotlin.math.sqrt

/**
 * ROUND 19 item 75 — thin coverage as a DIRECTION, not just a fraction.
 *
 * Rooms rarely close because of coverage, not math: only scan-029 ever closed
 * a loop, and every other capture left whole walls painted thinly or not at
 * all. [CoverageGrid] already knows WHERE the map is thin, cell by cell — what
 * the operator needs mid-walk is which way to TURN, which is a much coarser
 * question: twelve 30-degree azimuth sectors around the walked path, each
 * counting the returns that resolved in that direction *relative to where the
 * operator stood when they were painted*.
 *
 * That last clause is the honesty of the thing. This is coverage of what the
 * D6 could see from the walked path — a sector reads thin either because the
 * wall there was never painted or because the operator never walked anywhere
 * that faces it, and both readings mean the same instruction: walk that way
 * before stopping. It makes no claim of global completeness and none about
 * rooms the path never entered.
 *
 * Thinness is RELATIVE (a sector under [thinFraction] of the mean sector
 * count), so the verdict is scale-free: a dense crawl and a quick pass are
 * judged against themselves, not against a magic constant. Nothing is said at
 * all below [minTotalPoints] — guidance from a hundred returns would be noise
 * wearing a compass.
 *
 * Same determinism rules as [CoverageGrid]: pure arithmetic, no clock, no
 * allocation on the add path. NOT thread-safe; the renderer owns one and
 * feeds it from its frame callback, same as the grid beside it.
 */
class CoverageCompass(
    private val sectorCount: Int = DEFAULT_SECTORS,
    private val minRangeM: Float = DEFAULT_MIN_RANGE_M,
    private val maxRangeM: Float = DEFAULT_MAX_RANGE_M,
    private val thinFraction: Float = DEFAULT_THIN_FRACTION,
    private val minTotalPoints: Long = DEFAULT_MIN_TOTAL,
) {
    private val counts = LongArray(sectorCount)

    private var operatorX = 0f
    private var operatorZ = 0f
    private var hasOperator = false

    // The walk's most recent direction, for the plain-words verdict. Updated
    // only on real displacement so standing still does not spin it.
    private var headingX = 0f
    private var headingZ = 0f
    private var hasHeading = false

    var pointsCounted: Long = 0L
        private set

    fun clear() {
        counts.fill(0L)
        pointsCounted = 0L
        hasOperator = false
        hasHeading = false
    }

    /** Where the operator stands NOW — the origin the next points are judged from. */
    fun setOperator(x: Float, z: Float) {
        if (!x.isFinite() || !z.isFinite()) return
        if (hasOperator) {
            val dx = x - operatorX
            val dz = z - operatorZ
            val d2 = dx * dx + dz * dz
            if (d2 > HEADING_STEP_M * HEADING_STEP_M) {
                val d = sqrt(d2)
                headingX = dx / d
                headingZ = dz / d
                hasHeading = true
                operatorX = x
                operatorZ = z
            }
        } else {
            operatorX = x
            operatorZ = z
            hasOperator = true
        }
    }

    /** One resolved world point, judged from the operator's current position. */
    fun add(x: Float, z: Float) {
        if (!hasOperator || !x.isFinite() || !z.isFinite()) return
        val dx = x - operatorX
        val dz = z - operatorZ
        val d2 = dx * dx + dz * dz
        if (d2 < minRangeM * minRangeM || d2 > maxRangeM * maxRangeM) return
        counts[sectorOf(dx, dz)]++
        pointsCounted++
    }

    /** Azimuth sector of a world XZ direction; 0 faces +X, ascending toward +Z. */
    fun sectorOf(dx: Float, dz: Float): Int {
        var deg = Math.toDegrees(atan2(dz.toDouble(), dx.toDouble()))
        if (deg < 0.0) deg += 360.0
        val s = floor(deg / (360.0 / sectorCount)).toInt()
        return if (s >= sectorCount) sectorCount - 1 else s
    }

    /** True when there is enough evidence for any verdict at all. */
    fun measurable(): Boolean = pointsCounted >= minTotalPoints

    /**
     * Per-sector coverage, 0..1 against the mean sector count (1 = at or
     * above the mean). All zeros before [measurable].
     */
    fun sectorCoverage(): FloatArray {
        val out = FloatArray(sectorCount)
        if (!measurable()) return out
        val mean = pointsCounted.toDouble() / sectorCount
        if (mean <= 0.0) return out
        for (i in 0 until sectorCount) {
            out[i] = (counts[i] / mean).toFloat().coerceIn(0f, 1f)
        }
        return out
    }

    fun isThin(sector: Int): Boolean {
        if (!measurable()) return false
        val mean = pointsCounted.toDouble() / sectorCount
        return counts[sector] < thinFraction * mean
    }

    /**
     * The largest circular run of thin sectors as (startSector, length), or
     * null when nothing is thin or nothing is measurable. Ties keep the
     * earliest start, so the answer is deterministic.
     */
    fun largestThinArc(): Pair<Int, Int>? {
        if (!measurable()) return null
        var bestStart = -1
        var bestLen = 0
        var i = 0
        while (i < sectorCount) {
            if (!isThin(i)) {
                i++
                continue
            }
            var len = 1
            while (len < sectorCount && isThin((i + len) % sectorCount)) len++
            if (len > bestLen) {
                bestLen = len
                bestStart = i
            }
            i += len
        }
        return if (bestLen > 0) bestStart to bestLen else null
    }

    /**
     * The summary-card sentence, or null when there is nothing worth saying
     * (unmeasured, or no thin arc of at least two sectors — a single thin
     * 30-degree sliver is not an instruction). Directions are relative to the
     * way the operator was WALKING at the end, because the room has no compass
     * — "the walls behind you" is checkable on the spot, "the west wall" is a
     * pretense.
     */
    fun adviceLine(): String? {
        val arc = largestThinArc() ?: return null
        val (start, len) = arc
        if (len < 2) return null
        val degrees = len * (360 / sectorCount)
        val where = if (hasHeading) {
            // Centre azimuth of the arc, relative to the walk heading. In a
            // right-handed +Y-up world, "right of heading" is heading azimuth
            // + 90 deg in this atan2(z, x) convention (right = forward x up =
            // (-fz, fx)).
            val centerDeg = (start + len / 2.0) * (360.0 / sectorCount)
            val headDeg = Math.toDegrees(atan2(headingZ.toDouble(), headingX.toDouble()))
            var rel = centerDeg - headDeg
            while (rel < 0) rel += 360.0
            while (rel >= 360.0) rel -= 360.0
            when {
                rel <= 45.0 || rel > 315.0 -> "ahead of you"
                rel <= 135.0 -> "to your right"
                rel <= 225.0 -> "behind you"
                else -> "to your left"
            }
        } else {
            "on one side of the room"
        }
        return "The walls $where are thin in the map (about $degrees° of the room) — " +
            "walk past them before stopping."
    }

    companion object {
        const val DEFAULT_SECTORS: Int = 12
        /** Below this the "wall" is the operator's own body/bracket. */
        const val DEFAULT_MIN_RANGE_M: Float = 0.4f
        /** Past this a return says little about THIS room's walls. */
        const val DEFAULT_MAX_RANGE_M: Float = 8.0f
        /** A sector under this fraction of the mean is thin. */
        const val DEFAULT_THIN_FRACTION: Float = 0.15f
        /** No verdicts before this many counted returns (~7 s of D6). */
        const val DEFAULT_MIN_TOTAL: Long = 10_000L
        /** Metres of travel before the heading updates. */
        const val HEADING_STEP_M: Float = 0.15f
    }
}
