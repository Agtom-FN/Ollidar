package com.lidarscan.core.capture

/**
 * ROUND 5.3 (item 18, walkthrough-first): the operator's own path, decimated for
 * drawing.
 *
 * The capture is a **walk**: the phone (with the D6 on its back, or the Mid-360 on
 * its adapter) is carried through the space, and the single most useful thing the
 * screen can show mid-walk is *where you have already been* — which is how you see
 * that you have not covered the far wall yet. The poses are already streaming (
 * ARCore VIO for a phone-tracked D6, LIO for a Mid-360), so this is purely a
 * matter of keeping and projecting them.
 *
 * Ground-plane only, and that is a decision: ARCore's world frame is Y-up, so the
 * trail is (x, z) and the height is dropped. A 3D ribbon would be prettier and
 * would tell a walking operator less. The screen order of those two axes is NOT
 * arbitrary — see [normalized], which had it backwards until ROUND 10.
 *
 * Pure `:core` so the decimation and the fit are unit-testable; `:app` owns the
 * pose subscription and the Canvas.
 */
class TrajectoryTrail(
    /**
     * Minimum spacing between kept points, metres. At 0.15 m a 90 m walk is 600
     * points — enough to read the shape of a route through a building, cheap
     * enough to redraw at 5 Hz.
     */
    private val minSpacingM: Float = 0.15f,
    /** Ring capacity. The oldest points fall off rather than the trail growing forever. */
    capacity: Int = 600,
) {
    /**
     * ROUND 6 (owner items 21 + 22): settable, because the Light / Optimal /
     * Full presets size it. Shrinking drops the OLDEST points immediately —
     * keeping the newest is what a walkthrough operator glancing at "where have
     * I just been" actually wants.
     */
    var capacity: Int = capacity.coerceAtLeast(2)
        set(value) {
            field = value.coerceAtLeast(2)
            while (points.size > field) points.removeFirst()
        }

    /**
     * x/y/z metres in the pose frame, plus whether tracking was good when it
     * was taken.
     *
     * ROUND 16 item 59 — `y` is new, and its absence was a real omission
     * rather than a saving. The trail existed only to be drawn on a 108 x 84 dp
     * bird's-eye tile, so the recorder threw the height away at the door
     * (`pose.tx()`, `pose.tz()`, and nothing else). The owner now wants the
     * walk drawn INSIDE the 3D cloud — *"i want to see the path of mine showing
     * in the pointcloud too for me to check if the scan is right"* — and a path
     * pinned to y = 0 in a room whose floor is at y = 0 and whose returns run
     * to the ceiling would be a line under the floor, which is precisely the
     * ROUND 10 mirror bug wearing a different hat.
     *
     * Defaulted, so the 2D tile's own maths and its round-10 chirality test are
     * untouched: [normalized] and [pathLengthM] still read x and z only, which
     * is correct — a bird's-eye tile measures ground distance, and a walk up a
     * staircase must not make the tile's scale bar longer.
     */
    data class Point(val x: Float, val z: Float, val tracking: Boolean, val y: Float = 0f)

    /** A point mapped into 0..1 canvas space, aspect preserved. */
    data class NormalizedPoint(val x: Float, val y: Float, val tracking: Boolean)

    private val points = ArrayDeque<Point>()

    val size: Int get() = points.size

    /**
     * Offers a pose. Kept only when it is at least [minSpacingM] from the last
     * kept one, so standing still does not fill the ring with the same spot and
     * push the walk out of it.
     *
     * Returns true when the point was kept (the caller can use it to decide
     * whether a redraw is worth it).
     */
    fun add(x: Float, z: Float, tracking: Boolean, y: Float = 0f): Boolean {
        if (!x.isFinite() || !z.isFinite()) return false
        // ROUND 16 item 59: height is carried, never gated on. The spacing rule
        // is a GROUND-distance rule (it exists so a standing operator does not
        // fill the ring), and letting a hand rising and falling by 15 cm push
        // points into the trail would make a stationary sweep look like a walk.
        val last = points.lastOrNull()
        if (last != null) {
            val dx = x - last.x
            val dz = z - last.z
            if (dx * dx + dz * dz < minSpacingM * minSpacingM) return false
        }
        points.addLast(Point(x, z, tracking, if (y.isFinite()) y else 0f))
        while (points.size > capacity) points.removeFirst()
        return true
    }

    fun snapshot(): List<Point> = points.toList()

    fun clear() = points.clear()

    /**
     * Fits the trail into a [width] × [height] box with [paddingFraction] margin,
     * **preserving aspect** so a corridor looks like a corridor, and returns
     * points in 0..1 canvas space.
     *
     * ## ROUND 10 item 37 — this projection was MIRRORED, and here is the whole
     * derivation, because "it looked backwards" is how the last one got shipped
     *
     * ARCore's world frame is right-handed and gravity-aligned: **+X right,
     * +Y up, −Z forward** (the direction the camera looks). A trail tile is a
     * bird's-eye view — the viewer is ABOVE the floor at +Y looking down −Y —
     * so the screen basis has to satisfy
     *
     *     right × up = +Y     (the axis pointing OUT of the screen, at the viewer)
     *
     * Take `right = +X`. Then `up` must be **−Z**, because
     * `X × (−Z) = −(X × Z) = −(−Y) = +Y`. World +Z is therefore *down* the
     * tile. Canvas y also grows downward, so the mapping is `canvas_y = nz`
     * with **no flip at all**.
     *
     * What shipped through 0.6.0 was `y = 1 − nz`, i.e. `right = +X, up = +Z`,
     * whose out-of-screen normal is `X × Z = −Y` — a view from **underneath the
     * floor looking up**. Every trail was therefore its own mirror image, and
     * an operator turning left watched the trail turn right. That is exactly
     * what the owner reported after ROUND 9 fixed the *points*: the two had
     * never agreed, and fixing the points made the disagreement visible.
     *
     * It is the same rule the floor-plan canvas already follows and the same
     * one the engine's own `plan/occupancy.cpp` uses for a Y-up cloud
     * (`plan_x = world z, plan_y = world x` — the transpose of this, which is
     * the same chirality with the tile rotated 90°). Those two were right; this
     * file was the outlier.
     *
     * A trail with fewer than two distinct points has no extent to fit, so it maps
     * to the centre rather than dividing by zero.
     */
    fun normalized(paddingFraction: Float = 0.12f): List<NormalizedPoint> {
        val pts = points
        if (pts.isEmpty()) return emptyList()
        var minX = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var minZ = Float.MAX_VALUE
        var maxZ = -Float.MAX_VALUE
        for (p in pts) {
            if (p.x < minX) minX = p.x
            if (p.x > maxX) maxX = p.x
            if (p.z < minZ) minZ = p.z
            if (p.z > maxZ) maxZ = p.z
        }
        val spanX = maxX - minX
        val spanZ = maxZ - minZ
        val span = maxOf(spanX, spanZ)
        val pad = paddingFraction.coerceIn(0f, 0.4f)
        val usable = 1f - 2f * pad

        if (span <= 1e-4f) {
            return pts.map { NormalizedPoint(0.5f, 0.5f, it.tracking) }
        }

        // Centre the smaller axis inside the square fit, so a straight walk sits
        // on the middle line instead of hugging an edge.
        val offsetX = (span - spanX) / 2f
        val offsetZ = (span - spanZ) / 2f
        return pts.map { p ->
            val nx = pad + usable * ((p.x - minX + offsetX) / span)
            val nz = pad + usable * ((p.z - minZ + offsetZ) / span)
            // NO flip: see the KDoc. World +Z goes DOWN the tile, which is what
            // makes the tile a view from above rather than from below.
            NormalizedPoint(x = nx, y = nz, tracking = p.tracking)
        }
    }

    /** Straight-line path length in metres — the "you have walked this far" number. */
    fun pathLengthM(): Float {
        var total = 0f
        var previous: Point? = null
        for (p in points) {
            previous?.let { total += kotlin.math.hypot((p.x - it.x).toDouble(), (p.z - it.z).toDouble()).toFloat() }
            previous = p
        }
        return total
    }
}
