package com.lidarscan.core.render

import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * ROUND 25 item 117 — **the point-cloud viewer's camera, as arithmetic.**
 *
 * Owner: *"Add pan and zoom in out function for lidar scan review."* What
 * existed was one gesture. `PointCloudRenderer` drove filament-utils'
 * `Manipulator` in `ORBIT` mode: a one-finger drag orbited, a pinch dollied,
 * and **pan did not exist** — `Manipulator.grabBegin`'s `strafe` flag was never
 * passed `true`, so a two-finger drag fed the first pointer's coordinates
 * straight into the orbit path and spun the cloud instead of sliding it. On a
 * scan of a corridor, where the interesting geometry is thirty metres from the
 * origin the camera is nailed to, that is the difference between a viewer and a
 * turntable.
 *
 * ## Why this is `:core` and not a Filament call
 *
 * The `Manipulator` is native — a JNI object out of `libfilament-jni.so`,
 * unconstructible on a bare JVM, and (its own comment in the renderer says so)
 * **unable to retarget**: its target position is fixed at `Builder.build()`
 * time. Pan is precisely "move the target", so the existing camera physically
 * could not do the thing the owner asked for. Both problems have the same
 * answer: fourteen lines of spherical trigonometry that the app owns, that a
 * unit test can hold still, and that hands Filament nothing but the `lookAt`
 * triple it already consumes.
 *
 * ## Conventions, stated once
 *
 *  * **Y-up**, matching the renderer's `upVector(0f, 1f, 0f)` and the follow
 *    camera's stated convention. Not restated at the call site: two places
 *    asserting the same up-vector is how they come to disagree.
 *  * [yawRad] is measured about +Y from the +Z axis toward +X, so the home
 *    view reproduces the old `orbitHomePosition(4f, 3f, 8f)` exactly (see
 *    [HOME]) — the viewer opens on the same framing it always has, which is
 *    what keeps this a new capability rather than a new look.
 *  * [pitchRad] is positive **above** the target. Clamped to ±[MAX_PITCH_RAD]
 *    rather than ±90°, because at exactly 90° the eye, the target and the world
 *    up are collinear and the `lookAt` basis is degenerate — the picture
 *    flips, or goes black, depending on the driver.
 *  * Screen coordinates are Android's: +x right, **+y down**.
 */
data class OrbitCamera(
    val targetX: Float = 0f,
    val targetY: Float = 0f,
    val targetZ: Float = 0f,
    /** Azimuth about +Y, radians, wrapped to (−π, π]. */
    val yawRad: Float = 0f,
    /** Elevation above the target, radians, clamped to ±[MAX_PITCH_RAD]. */
    val pitchRad: Float = 0f,
    /** Distance from the target, metres, clamped to [MIN_DISTANCE_M]…[MAX_DISTANCE_M]. */
    val distanceM: Float = 1f,
) {

    /** The camera position implied by the orbit, in world metres. */
    val eyeX: Float get() = targetX + distanceM * cos(pitchRad) * sin(yawRad)
    val eyeY: Float get() = targetY + distanceM * sin(pitchRad)
    val eyeZ: Float get() = targetZ + distanceM * cos(pitchRad) * cos(yawRad)

    /**
     * The camera's own up vector.
     *
     * Not the world up. Handing Filament `(0,1,0)` works only while the pitch
     * is shallow; past about 60° the roll it implies becomes visible, and at
     * the pole it is undefined. Derived from the same two angles as the eye, so
     * the basis is orthonormal by construction rather than by luck.
     */
    val upX: Float get() = -sin(pitchRad) * sin(yawRad)
    val upY: Float get() = cos(pitchRad)
    val upZ: Float get() = -sin(pitchRad) * cos(yawRad)

    // ── the three gestures ─────────────────────────────────────────────────

    /**
     * One finger: **orbit**.
     *
     * The drag is scaled by the viewport rather than by a constant, so a swipe
     * across the screen is half a turn on any phone — a fixed radians-per-pixel
     * makes the same gesture do different things on a tablet and a compact.
     *
     * Signs: dragging right spins the cloud right, which means the camera walks
     * left around it; dragging down tips the top of the cloud toward the viewer,
     * which means the camera rises. Both are what every 3D viewer does, and
     * both are asserted rather than described.
     */
    fun orbit(dxPx: Float, dyPx: Float, viewportWidthPx: Int, viewportHeightPx: Int): OrbitCamera {
        val w = max(viewportWidthPx, 1)
        val h = max(viewportHeightPx, 1)
        // A full width is half a turn; a full height is a full pole-to-pole
        // sweep of the (clamped) pitch range.
        val yaw = yawRad - dxPx / w * PI_F
        val pitch = pitchRad + dyPx / h * PI_F
        return copy(yawRad = wrapAngle(yaw), pitchRad = clampPitch(pitch))
    }

    /**
     * Two fingers: **pan** — translate the target in the view plane.
     *
     * The scale is exact rather than tuned: at the target's depth, one screen
     * height spans `2·d·tan(fov/2)` metres, so a pixel is that over the viewport
     * height. That is what makes the geometry stay under the fingers instead of
     * sliding at some rate that feels right on one scan and wrong on the next —
     * and it is why [fovYRad] is a parameter rather than a constant here even
     * though the renderer only ever uses 45°: the value belongs to the camera
     * that is projecting, not to this arithmetic.
     *
     * The target moves **against** the drag, which is what makes the cloud move
     * **with** it.
     */
    fun pan(dxPx: Float, dyPx: Float, viewportHeightPx: Int, fovYRad: Float = DEFAULT_FOV_Y_RAD): OrbitCamera {
        val h = max(viewportHeightPx, 1)
        val metresPerPixel = (2f * distanceM * tan(fovYRad / 2f)) / h
        // Camera right = normalize(cross(forward, worldUp)); with forward
        // pointing from the eye back to the target this reduces to (cos y, 0,
        // −sin y) — the horizon direction, independent of pitch, which is why
        // panning near the pole does not accelerate.
        val rx = cos(yawRad)
        val rz = -sin(yawRad)
        val dx = -dxPx * metresPerPixel
        val dy = dyPx * metresPerPixel
        return copy(
            targetX = targetX + rx * dx + upX * dy,
            targetY = targetY + upY * dy,
            targetZ = targetZ + rz * dx + upZ * dy,
        )
    }

    /**
     * Pinch: **dolly** toward or away from the target, clamped.
     *
     * [scaleFactor] is `ScaleGestureDetector`'s: >1 for fingers moving apart,
     * which is "bring it closer", so it divides. Clamped at both ends, and the
     * near clamp is the one that matters — an unclamped dolly walks the eye
     * through the target and out the far side, at which point the cloud is
     * behind the camera and the viewer looks broken rather than zoomed.
     *
     * A non-positive or non-finite factor returns the camera unchanged: a
     * gesture detector that reports nonsense must not be able to send the
     * distance to NaN, from which no later gesture can recover it.
     */
    fun dolly(scaleFactor: Float): OrbitCamera {
        if (!scaleFactor.isFinite() || scaleFactor <= 0f) return this
        return copy(distanceM = clampDistance(distanceM / scaleFactor))
    }

    /** [dolly] expressed as a signed fraction: +0.1 is "10 % closer". */
    fun dollyBy(fraction: Float): OrbitCamera = dolly(1f + fraction)

    companion object {
        private const val PI_F = 3.1415927f

        /** 89°: one degree short of the degenerate `lookAt` at the pole. */
        const val MAX_PITCH_RAD = 1.5533431f

        /**
         * 12 cm. Below this the near plane (0.05 m) is within a pinch of the
         * target and the cloud starts clipping away as the operator zooms in,
         * which reads as the app deleting their scan.
         */
        const val MIN_DISTANCE_M = 0.12f

        /** 2 km — beyond the far plane's 2 km there is nothing left to see. */
        const val MAX_DISTANCE_M = 2000f

        /** The renderer's `Camera.Fov.VERTICAL` 45°, as radians. */
        const val DEFAULT_FOV_Y_RAD = 0.7853982f

        /**
         * The framing the viewer has always opened on: filament-utils'
         * `orbitHomePosition(4, 3, 8)` about the origin, expressed in this
         * camera's angles. Kept to the digit so that adopting this class is not
         * also a silent change of default view.
         */
        val HOME: OrbitCamera = fromEye(eyeX = 4f, eyeY = 3f, eyeZ = 8f)

        fun clampPitch(radians: Float): Float = radians.coerceIn(-MAX_PITCH_RAD, MAX_PITCH_RAD)

        fun clampDistance(metres: Float): Float =
            if (!metres.isFinite()) MIN_DISTANCE_M else metres.coerceIn(MIN_DISTANCE_M, MAX_DISTANCE_M)

        /** Wraps to (−π, π]. Purely cosmetic for the render, and load-bearing for the tests. */
        fun wrapAngle(radians: Float): Float {
            var a = radians
            while (a <= -PI_F) a += 2f * PI_F
            while (a > PI_F) a -= 2f * PI_F
            return a
        }

        /** The camera that looks at [targetX]/[targetY]/[targetZ] from the given eye. */
        fun fromEye(
            eyeX: Float,
            eyeY: Float,
            eyeZ: Float,
            targetX: Float = 0f,
            targetY: Float = 0f,
            targetZ: Float = 0f,
        ): OrbitCamera {
            val dx = eyeX - targetX
            val dy = eyeY - targetY
            val dz = eyeZ - targetZ
            val d = clampDistance(sqrt(dx * dx + dy * dy + dz * dz))
            val pitch = clampPitch(kotlin.math.asin((dy / d).coerceIn(-1f, 1f)))
            // atan2(x, z), not (z, x): yaw is measured from +Z toward +X.
            val yaw = if (abs(dx) < 1e-6f && abs(dz) < 1e-6f) 0f else atan2(dx, dz)
            return OrbitCamera(
                targetX = targetX,
                targetY = targetY,
                targetZ = targetZ,
                yawRad = wrapAngle(yaw),
                pitchRad = pitch,
                distanceM = d,
            )
        }

        /**
         * **Double tap: reset the framing** — the whole cloud, at the home
         * angles.
         *
         * Reset is deliberately "frame what is there" and not "go back to
         * (4, 3, 8)". A scan whose geometry sits forty metres from the session
         * origin would be reset to a view of empty space, and the operator's
         * escape hatch from a lost camera would itself lose the camera. The
         * angles ARE the home angles, because the direction you look from is a
         * preference and the place you look at is a fact.
         *
         * An empty or degenerate bound (a single point, a cloud that has not
         * loaded) falls back to [HOME] rather than dividing by a zero radius.
         */
        fun framing(
            minX: Float,
            minY: Float,
            minZ: Float,
            maxX: Float,
            maxY: Float,
            maxZ: Float,
            fovYRad: Float = DEFAULT_FOV_Y_RAD,
        ): OrbitCamera {
            val spanX = maxX - minX
            val spanY = maxY - minY
            val spanZ = maxZ - minZ
            if (!spanX.isFinite() || !spanY.isFinite() || !spanZ.isFinite() ||
                spanX < 0f || spanY < 0f || spanZ < 0f
            ) {
                return HOME
            }
            val radius = 0.5f * sqrt(spanX * spanX + spanY * spanY + spanZ * spanZ)
            // The bounding sphere fits the vertical field of view with a 15 %
            // margin, so the cloud does not touch the edges of the viewport.
            val distance = clampDistance(
                if (radius <= 1e-4f) HOME.distanceM else 1.15f * radius / sin(fovYRad / 2f),
            )
            return HOME.copy(
                targetX = (minX + maxX) / 2f,
                targetY = (minY + maxY) / 2f,
                targetZ = (minZ + maxZ) / 2f,
                distanceM = distance,
            )
        }
    }
}
