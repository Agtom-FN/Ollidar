package com.lidarscan.core.capture

import kotlin.math.sqrt

/**
 * ROUND 12 — **did the walk come back, and how far off was it when it did?**
 *
 * ## Why this is the number the owner's complaint is about
 *
 * > "current scan right when i go forward but when i turn around the scan
 * >  position shifted"
 *
 * ROUND 12 measured what actually happens on the owner's two normal-pace
 * captures, with `engine_cli --d6-selfcheck`:
 *
 * | capture | walked | surfaces re-painted 8 s apart disagree by |
 * | --- | ---: | ---: |
 * | scan-020 (5.3 cm/s crawl) | 10.8 m | **0.70 cm** (floor 0.29) |
 * | scan-026 (25 cm/s walk) | 15.3 m | **5.26 cm** (floor 0.99) |
 * | scan-028 (32 cm/s walk) | 15.8 m | **4.45 cm** (floor 0.70) |
 *
 * Neither the mount trim nor a lidar/pose clock offset accounts for it (both
 * adjudicated and both refuted — see `android/NOTES.md` ROUND 12). What is left
 * is the trajectory: ARCore's VIO slides, and the D6 has no way to notice
 * because it paints a plane and can measure nothing along the walk.
 *
 * The app cannot see the point cloud from Kotlin. But it CAN see the
 * trajectory, and a trajectory that comes back to where it started carries one
 * honest, geometric, free measurement: **the gap it comes back with.** On the
 * owner's own captures the engine reports 0.52 m (scan-026) and 0.80 m
 * (scan-028) between the first pose and the last, after ~15 m of walking.
 *
 * ## The claim is CONDITIONAL, and it says so
 *
 * A gap of 0.52 m means one of two things and this class cannot tell which:
 * the operator finished half a metre from where they began, or the tracker
 * drifted half a metre. So the number is reported with its condition attached
 * — *"if you finished where you started, this is the tracker's drift"* — and it
 * is deliberately NOT fed into the grade. A metric that quietly assumed the
 * operator's intent would be the same unearned confidence that let 0.7.0 grade
 * scan-026 **GOOD SCAN** while its map disagreed with itself by 5 cm.
 *
 * ## Why the trail's own ring cannot answer it
 *
 * [TrajectoryTrail] is a fixed-capacity ring sized for drawing — the oldest
 * points fall off, so on a long walk the start is simply gone. This keeps the
 * one point it needs (the first) and a running minimum, in constant space.
 *
 * Pure `:core`, no ARCore types.
 */
class LoopReturnTracker(
    /**
     * How far the operator must have walked before a return to the start is
     * evidence of anything. Below this, "near the start" just means "has not
     * left yet".
     *
     * 8 m is `TrajectoryLoopConfig::min_loop_path_m` in the engine, so the
     * app's notion of a loop and the offline loop closer's agree.
     */
    private val minLoopPathM: Float = MIN_LOOP_PATH_M,
) {
    private var startX = Float.NaN
    private var startZ = Float.NaN
    private var closest = Float.MAX_VALUE
    private var latest = Float.NaN
    private var qualified = false

    /**
     * Offers a ground-plane position and the path length walked so far.
     * Cheap: two subtractions and a square root.
     */
    fun add(x: Float, z: Float, pathMeters: Float) {
        if (!x.isFinite() || !z.isFinite()) return
        if (startX.isNaN()) {
            startX = x
            startZ = z
            return
        }
        val dx = x - startX
        val dz = z - startZ
        val d = sqrt(dx * dx + dz * dz)
        latest = d
        if (pathMeters >= minLoopPathM) {
            qualified = true
            if (d < closest) closest = d
        }
    }

    /** True once the walk was long enough for a return to mean something. */
    val isLoop: Boolean get() = qualified

    /**
     * The distance from the walk's END to its START, metres, or null when the
     * walk was too short to qualify.
     */
    val endGapMeters: Float? get() = if (qualified && latest.isFinite()) latest else null

    /**
     * The CLOSEST the walk ever came back to its start after qualifying, metres.
     *
     * This is the stronger of the two numbers when it is small: if the operator
     * physically stood back on their starting spot at any point, the map still
     * says they were `closest` metres away, and that gap is drift with no
     * conditional attached — an operator cannot be somewhere they were not.
     * When it is large it says only that they never went back.
     */
    val closestApproachMeters: Float? get() = if (qualified && closest < Float.MAX_VALUE) closest else null

    fun reset() {
        startX = Float.NaN
        startZ = Float.NaN
        closest = Float.MAX_VALUE
        latest = Float.NaN
        qualified = false
    }

    companion object {
        const val MIN_LOOP_PATH_M = 8.0f

        /**
         * Below this, a return gap is not worth mentioning: it is the same
         * order as where a person stops relative to where they started, and the
         * card would be crying wolf.
         *
         * 0.25 m. The owner's two captures read 0.52 m and 0.80 m, so both are
         * comfortably above it, and scan-020's straight out-and-back — which
         * genuinely ends where it began — would have to be worse than a quarter
         * of a metre before this says anything.
         */
        const val WORTH_MENTIONING_M = 0.25f
    }
}
