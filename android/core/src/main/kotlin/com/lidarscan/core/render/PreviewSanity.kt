package com.lidarscan.core.render

/**
 * ROUND 8 — **the Projects-tab thumbnail was drawing the raw 2D fan.**
 *
 * ### The evidence
 *
 * The owner exported a real 0.4.0 capture from his Pixel 8 Pro
 * (`captures/scan-015-pixel-0.4.0.lscan/`, 191,381 points over 26.3 s with a
 * COIN-D6). Its `processed/preview.f32` — the file the Projects tab draws a
 * scan's tile from — contains 4,040 points, and:
 *
 * ```
 *   z == 0.0f exactly:  2,027 of 4,040   (50.2 %)
 *   z != 0:             2,013            (z from -4.90 to +0.10 m)
 * ```
 *
 * Exactly half. That is not corruption and it is not a rounding artefact — it
 * is **two streams superimposed**. A D6 capture with the pushbroom running
 * holds two point streams in one `PageStore` (INT24-wiring.md §2):
 *
 *  * `SCAN_STREAM_LIDAR_D6` — the driver's raw sensor-frame preview. The D6 is
 *    a 2D lidar and its returns lie in its own scan plane **by construction**,
 *    so every one of them has `z == 0` exactly; and
 *  * `SCAN_STREAM_SLAM_MAP` — A8's resolved world-frame cloud, the 3D room.
 *
 * The live renderer has known this since B3 and filters
 * ([com.lidarscan.core.render.StreamFilter]). `writeProjectPreview` did not: it
 * walked every page of the source and sampled them all. So a D6 project's
 * thumbnail was the 3D map with a flat disc of raw fan returns drawn through
 * the middle of it, at 1:1 — which, at 108 dp and viewed end-on, reads as a
 * flat 2D scan. Combined with the Review screen showing nothing at all for a D6
 * project (owner item 27c), this is the second half of *"when i check the
 * recording, it still show a 2D scan"*.
 *
 * ### What this file is
 *
 * The stream filtering is fixed at the source (see `writeProjectPreview`). This
 * is the second line of defence: a **verdict on the numbers themselves**,
 * applied before a preview is written and again after it is read back, so that
 * a preview which is somehow neither finite nor plausibly a room is refused
 * instead of drawn. It is pure `:core` with no Android types precisely so it
 * can be run against the real exported fixture in a JVM test.
 *
 * ### Why the thresholds are what they are
 *
 * Everything here is deliberately loose. The job is to catch **classes** of
 * wrongness — NaN, uninitialised memory, a fan masquerading as a room — not to
 * second-guess geometry. A false rejection costs a thumbnail; a false
 * acceptance costs the owner's trust in what he is looking at, which is what
 * this round is about.
 */
object PreviewSanity {

    /**
     * The largest extent a hand-carried indoor scan can plausibly have, in
     * metres. The COIN-D6 sees 12 m (spec §2.1) and a walk is minutes long, so
     * a kilometre is already absurd; this is set two orders of magnitude past
     * absurd so it only ever fires on garbage, never on an unusually large
     * survey.
     */
    const val MAX_PLAUSIBLE_EXTENT_M = 100_000.0f

    /**
     * Above this fraction of points sitting at exactly z == 0, the sample is
     * dominated by sensor-frame fan returns rather than by resolved geometry.
     *
     * "Exactly" is doing the work: a resolved point's z is the sum of the rig
     * height, the pose interpolation and the ray, and the probability of that
     * landing on the float 0.0 bit pattern is negligible. A raw D6 return's z
     * is 0 by construction, every time. The real fixture measured **50.2 %**;
     * a correct D6 preview measures 0. Anything past a third is the bug.
     *
     * Not zero-tolerance, because a floor scan genuinely can put a handful of
     * points at the origin plane, and a thumbnail is not worth a false alarm.
     */
    const val MAX_ZERO_Z_FRACTION = 0.33f

    /** The fewest points worth drawing a tile from. Below this the tile says "no data" instead of lying. */
    const val MIN_POINTS = 16

    sealed interface Verdict {
        data object Ok : Verdict

        /** Rejected, with a sentence naming what was wrong — logged, never silently swallowed. */
        data class Rejected(val reason: String) : Verdict
    }

    /**
     * @param xyz interleaved x, y, z triples.
     * @param count how many triples in [xyz] are meaningful (the array may be larger).
     */
    fun check(xyz: FloatArray, count: Int): Verdict {
        if (count < MIN_POINTS) {
            return Verdict.Rejected("only $count points — too few to be a scan preview")
        }
        if (xyz.size < count * 3) {
            return Verdict.Rejected(
                "buffer holds ${xyz.size} floats but claims $count points (needs ${count * 3})",
            )
        }

        var minX = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var minY = Float.MAX_VALUE
        var maxY = -Float.MAX_VALUE
        var minZ = Float.MAX_VALUE
        var maxZ = -Float.MAX_VALUE
        var zeroZ = 0
        for (i in 0 until count) {
            val x = xyz[i * 3]
            val y = xyz[i * 3 + 1]
            val z = xyz[i * 3 + 2]
            // NaN and +/-Inf both fail this, and so does the uninitialised-memory
            // case: reading a freed or never-written buffer as float typically
            // yields values around 1e38, which `isFinite` passes and the extent
            // check below catches.
            if (!x.isFinite() || !y.isFinite() || !z.isFinite()) {
                return Verdict.Rejected("point $i is not finite ($x, $y, $z)")
            }
            if (x < minX) minX = x
            if (x > maxX) maxX = x
            if (y < minY) minY = y
            if (y > maxY) maxY = y
            if (z < minZ) minZ = z
            if (z > maxZ) maxZ = z
            if (z == 0.0f) zeroZ++
        }

        val extent = maxOf(maxX - minX, maxY - minY, maxZ - minZ)
        if (extent > MAX_PLAUSIBLE_EXTENT_M) {
            return Verdict.Rejected(
                "extent %.3g m is not a scan — this is uninitialised or foreign memory"
                    .format(extent),
            )
        }
        if (extent <= 0f) {
            return Verdict.Rejected("every point is at the same place — nothing to draw")
        }

        val zeroFraction = zeroZ.toFloat() / count.toFloat()
        if (zeroFraction > MAX_ZERO_Z_FRACTION) {
            return Verdict.Rejected(
                "%.1f%% of points sit at exactly z = 0 — that is the raw sensor-frame fan, not the resolved map"
                    .format(zeroFraction * 100f),
            )
        }
        return Verdict.Ok
    }
}
