package com.lidarscan.core.capture

/**
 * ROUND 16 item 59 — **the walked path, coloured so it can be read.**
 *
 * > *"i want to see the path of mine showing in the pointcloud too for me to
 * >  check if the scan is right"* — owner, on 0.9.0.
 *
 * The renderer's job is to put a line strip on the GPU; deciding what the line
 * MEANS is this class's, and it lives in `:core` for the usual reason — it is
 * the part that can be wrong, and it is the part a JVM test can pin. There is
 * no Filament, no Android and no GL below.
 *
 * ## What the colours say, and why each one earns its place
 *
 * A path drawn in one flat colour answers "where did I go". That is not the
 * owner's question. His question is whether the SCAN is right, and to answer it
 * he has to be able to see, in the cloud, three things a single colour cannot
 * carry:
 *
 *  * **Which end is which.** A loop drawn in one colour is a loop with no
 *    beginning, and the whole diagnostic value of a loop is whether it comes
 *    back to where it started. So the strip carries a time gradient — cool at
 *    the start, warm at the end — and both ends carry a brighter marker vertex.
 *    A gap between the two marker colours IS the loop-end gap the summary card
 *    reports in centimetres, standing there in the room, at true scale.
 *  * **Where the tracker was unsure.** `PoseSample.tracking` is already carried
 *    on every trail point and has never been shown anywhere except as a
 *    two-pixel colour change on a 108 dp tile. A stretch walked with tracking
 *    lost is a stretch whose returns are in the wrong place, and seeing it lie
 *    across a smeared wall is the fastest possible explanation of that smear.
 *  * **Nothing else.** No speed, no density, no per-point anything. Five
 *    colours is a legend the operator can hold in their head while walking;
 *    twelve is a chart.
 *
 * ## The gradient is not a colormap
 *
 * Deliberately NOT drawn from `ColormapLut`. Those ramps are for POINT values —
 * height, intensity, coverage — and the path has to be unmistakably not a
 * point: if it shares the cloud's palette then in height mode it becomes one
 * more thing at that height. So it uses the app's own accent pair, which
 * appears nowhere in any point colormap.
 *
 * Packed RGBA8 in the same little-endian order the point pages use, so the
 * ribbon's vertex buffer and the cloud's are the same 16-byte layout.
 */
object TrajectoryRibbon {

    /**
     * ARGB-less, GL-order packed RGBA8 (r in the low byte), matching
     * `PointVertex`'s `r, g, b, a` field order read as a little-endian int.
     */
    fun pack(r: Int, g: Int, b: Int, a: Int = 255): Int =
        (r and 0xFF) or ((g and 0xFF) shl 8) or ((b and 0xFF) shl 16) or ((a and 0xFF) shl 24)

    /** ScanTeal — the walk's beginning. */
    val START = pack(0x2E, 0xC4, 0xB6)

    /** Ember — the walk's end, and the operator's current position while live. */
    val END = pack(0xE8, 0x6A, 0x2B)

    /** A brighter marker so the two ends read as points, not as gradient stops. */
    val START_MARKER = pack(0x9B, 0xFF, 0xF2)
    val END_MARKER = pack(0xFF, 0xC7, 0x8A)

    /** Tracking was lost here. Muted red; the returns from this stretch moved. */
    val UNTRACKED = pack(0xC0, 0x3A, 0x3A, 0xFF)

    /**
     * How many vertices at each end are painted with the marker colour rather
     * than the gradient. Two, because one vertex of a line strip is invisible
     * (a strip draws segments, not points) and three starts to read as a
     * coloured stub rather than a marker.
     */
    const val MARKER_VERTICES = 2

    /**
     * The result: interleaved positions and one packed colour per vertex.
     * `count` may be less than `xyz.size / 3` when the caller reuses a buffer.
     */
    data class Ribbon(val xyz: FloatArray, val rgba: IntArray, val count: Int) {
        override fun equals(other: Any?): Boolean =
            other is Ribbon && count == other.count &&
                xyz.contentEquals(other.xyz) && rgba.contentEquals(other.rgba)

        override fun hashCode(): Int =
            (count * 31 + xyz.contentHashCode()) * 31 + rgba.contentHashCode()
    }

    val EMPTY = Ribbon(FloatArray(0), IntArray(0), 0)

    /**
     * Builds the ribbon from the live trail.
     *
     * The trail's `y` is the phone's height in ARCore's world frame, which is
     * where it belongs: the path is drawn at the height it was walked at, so it
     * threads through the cloud at chest height rather than lying on a floor
     * the room may not have.
     */
    fun fromTrail(points: List<TrajectoryTrail.Point>): Ribbon {
        if (points.size < 2) return EMPTY
        val n = points.size
        val xyz = FloatArray(n * 3)
        val rgba = IntArray(n)
        for (i in 0 until n) {
            val p = points[i]
            xyz[i * 3] = p.x
            xyz[i * 3 + 1] = p.y
            xyz[i * 3 + 2] = p.z
            rgba[i] = colorAt(i, n, p.tracking)
        }
        return Ribbon(xyz, rgba, n)
    }

    /**
     * Builds the ribbon from a decoded pose stream — `x, y, z` triples, in
     * order, as the engine hands them back for a sealed container.
     *
     * `strideM` thins the polyline: a 110 s walk is 3,300 poses at ARCore's
     * 30 Hz and a 3,300-vertex strip is 3,299 segments of about eight
     * millimetres each, which is a thick smudge rather than a path and costs
     * fifty times the vertices it needs. Thinning by DISTANCE and not by index
     * keeps the corners — where the interesting information is — and drops only
     * the straights.
     */
    fun fromPoses(xyzIn: FloatArray, count: Int, strideM: Float = DEFAULT_STRIDE_M): Ribbon {
        val n = count.coerceAtMost(xyzIn.size / 3)
        if (n < 2) return EMPTY
        val keptX = ArrayList<Float>(n)
        val keptY = ArrayList<Float>(n)
        val keptZ = ArrayList<Float>(n)
        var lastX = xyzIn[0]
        var lastY = xyzIn[1]
        var lastZ = xyzIn[2]
        keptX.add(lastX); keptY.add(lastY); keptZ.add(lastZ)
        for (i in 1 until n) {
            val x = xyzIn[i * 3]
            val y = xyzIn[i * 3 + 1]
            val z = xyzIn[i * 3 + 2]
            if (!x.isFinite() || !y.isFinite() || !z.isFinite()) continue
            val dx = x - lastX
            val dy = y - lastY
            val dz = z - lastZ
            // The LAST pose is always kept, below, so the end marker really is
            // the end of the walk and not the last point that happened to clear
            // the stride.
            if (dx * dx + dy * dy + dz * dz < strideM * strideM) continue
            keptX.add(x); keptY.add(y); keptZ.add(z)
            lastX = x; lastY = y; lastZ = z
        }
        val lastIdx = (n - 1) * 3
        if (keptX.size < 2 ||
            keptX.last() != xyzIn[lastIdx] ||
            keptY.last() != xyzIn[lastIdx + 1] ||
            keptZ.last() != xyzIn[lastIdx + 2]
        ) {
            keptX.add(xyzIn[lastIdx])
            keptY.add(xyzIn[lastIdx + 1])
            keptZ.add(xyzIn[lastIdx + 2])
        }
        val m = keptX.size
        if (m < 2) return EMPTY
        val xyz = FloatArray(m * 3)
        val rgba = IntArray(m)
        for (i in 0 until m) {
            xyz[i * 3] = keptX[i]
            xyz[i * 3 + 1] = keptY[i]
            xyz[i * 3 + 2] = keptZ[i]
            // A sealed container's pose stream carries `tracking_lost` too, but
            // the engine's trajectory export does not surface it yet, so every
            // vertex here is treated as tracked. Stated rather than guessed:
            // colouring a stretch red on no evidence would be worse than not
            // colouring it.
            rgba[i] = colorAt(i, m, tracking = true)
        }
        return Ribbon(xyz, rgba, m)
    }

    /**
     * The colour of vertex [i] of [n]. Extracted so both builders cannot drift
     * apart, and so the ordering rule — untracked BEATS the markers, which beat
     * the gradient — is written once.
     *
     * Untracked wins because it is the only colour that carries a warning; a
     * walk that lost tracking in its first half-metre must not have that hidden
     * under a start marker.
     */
    fun colorAt(i: Int, n: Int, tracking: Boolean): Int = when {
        !tracking -> UNTRACKED
        i < MARKER_VERTICES -> START_MARKER
        i >= n - MARKER_VERTICES -> END_MARKER
        else -> lerp(START, END, if (n <= 1) 0f else i.toFloat() / (n - 1).toFloat())
    }

    /** Per-channel linear blend of two packed colours. */
    fun lerp(a: Int, b: Int, t: Float): Int {
        val u = t.coerceIn(0f, 1f)
        fun ch(shift: Int): Int {
            val ca = (a ushr shift) and 0xFF
            val cb = (b ushr shift) and 0xFF
            return (ca + ((cb - ca) * u)).toInt().coerceIn(0, 255)
        }
        return pack(ch(0), ch(8), ch(16), ch(24))
    }

    /**
     * 12 cm. Just under the live trail's own 15 cm spacing, so a sealed scan's
     * path is drawn at essentially the same fidelity the operator watched it at
     * — the two must not look like different walks.
     */
    const val DEFAULT_STRIDE_M = 0.12f
}
