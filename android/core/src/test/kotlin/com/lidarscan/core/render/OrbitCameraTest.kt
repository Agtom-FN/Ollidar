package com.lidarscan.core.render

import kotlin.math.abs
import kotlin.math.sqrt
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 25 item 117 — the viewer's camera arithmetic, on a bare JVM.
 *
 * The reason this file exists is the reason [OrbitCamera] exists: the camera it
 * replaces is a native `Manipulator` that cannot be constructed off-device, so
 * for four rounds the only way to check "does a pinch zoom" was to hold a phone.
 * Everything below is checkable in 40 ms, and the properties are the ones that
 * break in a viewer: a pole that flips, a dolly that walks the eye through the
 * target, a pan whose speed does not match the fingers.
 */
class OrbitCameraTest {

    private fun assertClose(expected: Float, actual: Float, tol: Float = 1e-3f, what: String = "") {
        assertTrue(
            "$what expected $expected, was $actual",
            abs(expected - actual) <= tol,
        )
    }

    private fun distanceFromTarget(c: OrbitCamera): Float {
        val dx = c.eyeX - c.targetX
        val dy = c.eyeY - c.targetY
        val dz = c.eyeZ - c.targetZ
        return sqrt(dx * dx + dy * dy + dz * dz)
    }

    // ── the framing this replaces ──────────────────────────────────────────

    /**
     * Adopting this class must not silently change the view the operator has
     * always opened on: filament-utils' `orbitHomePosition(4, 3, 8)` about the
     * origin, to the digit.
     */
    @Test
    fun `home reproduces the old orbit home position`() {
        val home = OrbitCamera.HOME
        assertClose(4f, home.eyeX, what = "eye x")
        assertClose(3f, home.eyeY, what = "eye y")
        assertClose(8f, home.eyeZ, what = "eye z")
        assertClose(0f, home.targetX, what = "target x")
        assertClose(sqrt(89f), home.distanceM, what = "distance")
    }

    /** `fromEye` and the eye accessors must be exact inverses, or nothing below means anything. */
    @Test
    fun `an eye round-trips through the angles`() {
        for (eye in listOf(
            Triple(4f, 3f, 8f),
            Triple(-12f, 0.5f, 3f),
            Triple(0f, 9f, 0.001f),
            Triple(30f, -14f, -22f),
        )) {
            val c = OrbitCamera.fromEye(eye.first, eye.second, eye.third, 1f, -2f, 3f)
            assertClose(eye.first, c.eyeX, 1e-2f, "eye x")
            assertClose(eye.second, c.eyeY, 1e-2f, "eye y")
            assertClose(eye.third, c.eyeZ, 1e-2f, "eye z")
        }
    }

    /** The basis has to be orthonormal, at every pitch, or the picture rolls. */
    @Test
    fun `the up vector is unit and perpendicular to the view direction`() {
        var c = OrbitCamera.HOME
        for (step in 0..40) {
            c = c.orbit(dxPx = 17f, dyPx = 23f, viewportWidthPx = 1080, viewportHeightPx = 1920)
            val len = sqrt(c.upX * c.upX + c.upY * c.upY + c.upZ * c.upZ)
            assertClose(1f, len, 1e-3f, "step $step up length")
            val vx = c.targetX - c.eyeX
            val vy = c.targetY - c.eyeY
            val vz = c.targetZ - c.eyeZ
            val vlen = sqrt(vx * vx + vy * vy + vz * vz)
            val dot = (vx * c.upX + vy * c.upY + vz * c.upZ) / vlen
            assertClose(0f, dot, 1e-3f, "step $step up·view")
        }
    }

    // ── orbit ──────────────────────────────────────────────────────────────

    /**
     * A swipe across the full width is half a turn — on any phone. The scale is
     * the viewport's, so the same gesture must not do different things on a
     * compact and a tablet.
     */
    @Test
    fun `a full-width drag is half a turn on any viewport`() {
        for (w in listOf(720, 1080, 1440, 2560)) {
            val turned = OrbitCamera.HOME.orbit(w.toFloat(), 0f, w, 1920)
            val expected = OrbitCamera.wrapAngle(OrbitCamera.HOME.yawRad - 3.1415927f)
            assertClose(expected, turned.yawRad, 1e-3f, "width $w")
        }
    }

    /** Dragging right spins the cloud right, so the camera walks left around it. */
    @Test
    fun `the orbit signs match every other 3D viewer`() {
        val right = OrbitCamera.HOME.orbit(120f, 0f, 1080, 1920)
        assertTrue("dragging right must decrease yaw", right.yawRad < OrbitCamera.HOME.yawRad)
        val down = OrbitCamera.HOME.orbit(0f, 120f, 1080, 1920)
        assertTrue("dragging down must raise the camera", down.eyeY > OrbitCamera.HOME.eyeY)
    }

    /** Orbiting never moves the target and never changes the distance. */
    @Test
    fun `orbit is a rotation, not a translation`() {
        var c = OrbitCamera.HOME.copy(targetX = 5f, targetY = -1f, targetZ = 12f)
        val d0 = c.distanceM
        repeat(30) { c = c.orbit(61f, -37f, 1080, 1920) }
        assertEquals(5f, c.targetX, 0f)
        assertEquals(-1f, c.targetY, 0f)
        assertEquals(12f, c.targetZ, 0f)
        assertClose(d0, c.distanceM, 1e-4f, "distance")
        assertClose(d0, distanceFromTarget(c), 1e-2f, "geometric distance")
    }

    /**
     * The pole clamp. At exactly 90° the eye, the target and the world up are
     * collinear and `lookAt` is degenerate — the picture flips or goes black,
     * depending on the driver. Driven far past the pole in both directions.
     */
    @Test
    fun `pitch is clamped short of the pole in both directions`() {
        var up = OrbitCamera.HOME
        repeat(50) { up = up.orbit(0f, 900f, 1080, 1920) }
        assertClose(OrbitCamera.MAX_PITCH_RAD, up.pitchRad, 1e-4f, "north pole")
        var down = OrbitCamera.HOME
        repeat(50) { down = down.orbit(0f, -900f, 1080, 1920) }
        assertClose(-OrbitCamera.MAX_PITCH_RAD, down.pitchRad, 1e-4f, "south pole")
        // And the clamp is short of vertical, not AT it.
        assertTrue(OrbitCamera.MAX_PITCH_RAD < 1.5707963f)
    }

    /** Yaw wraps rather than accumulating; a long spin must stay in (−π, π]. */
    @Test
    fun `yaw wraps instead of accumulating`() {
        var c = OrbitCamera.HOME
        repeat(200) { c = c.orbit(400f, 0f, 1080, 1920) }
        assertTrue("yaw ${c.yawRad} escaped the wrap", c.yawRad > -3.1415927f && c.yawRad <= 3.1415927f)
    }

    // ── pan ────────────────────────────────────────────────────────────────

    /**
     * The pan scale is exact, not tuned: at the target's depth one screen
     * height spans `2·d·tan(fov/2)` metres. Checked head-on (yaw 0, pitch 0)
     * where the arithmetic is readable by hand.
     */
    @Test
    fun `a pan moves the world exactly as far as the fingers`() {
        val c = OrbitCamera(distanceM = 10f, yawRad = 0f, pitchRad = 0f)
        val h = 1920
        val fov = OrbitCamera.DEFAULT_FOV_Y_RAD
        val expectedPerPixel = (2f * 10f * kotlin.math.tan(fov / 2f)) / h
        val panned = c.pan(dxPx = 100f, dyPx = 0f, viewportHeightPx = h)
        assertClose(-100f * expectedPerPixel, panned.targetX, 1e-4f, "x metres")
        assertClose(0f, panned.targetY, 1e-5f, "y untouched")
        assertClose(0f, panned.targetZ, 1e-4f, "z untouched")
    }

    /**
     * The cloud moves WITH the fingers. Stated as a sign test on the target,
     * because that is the half of it a refactor gets backwards.
     */
    @Test
    fun `the cloud follows the drag`() {
        val c = OrbitCamera(distanceM = 10f)
        assertTrue("drag right must move the target left", c.pan(60f, 0f, 1920).targetX < 0f)
        assertTrue("drag down must raise the target", c.pan(0f, 60f, 1920).targetY > 0f)
    }

    /** Pan is a translation: the angles and the distance are untouched. */
    @Test
    fun `pan changes only the target`() {
        val c = OrbitCamera.HOME
        val p = c.pan(133f, -44f, 1920)
        assertEquals(c.yawRad, p.yawRad, 0f)
        assertEquals(c.pitchRad, p.pitchRad, 0f)
        assertEquals(c.distanceM, p.distanceM, 0f)
        assertNotEquals(c.targetX, p.targetX)
    }

    /** Panning further away covers more ground per pixel — the perspective is real. */
    @Test
    fun `pan speed scales with distance`() {
        val near = OrbitCamera(distanceM = 2f).pan(100f, 0f, 1920)
        val far = OrbitCamera(distanceM = 40f).pan(100f, 0f, 1920)
        assertTrue(abs(far.targetX) > abs(near.targetX) * 10f)
    }

    /**
     * Panning near the pole must not accelerate. The camera right vector is the
     * horizon direction and is independent of pitch, which is easy to lose in a
     * "simplification" that derives it from the full basis.
     */
    @Test
    fun `pan does not accelerate near the pole`() {
        val flat = OrbitCamera(distanceM = 10f, pitchRad = 0f).pan(100f, 0f, 1920)
        val steep = OrbitCamera(distanceM = 10f, pitchRad = OrbitCamera.MAX_PITCH_RAD).pan(100f, 0f, 1920)
        assertClose(flat.targetX, steep.targetX, 1e-4f, "horizontal pan")
    }

    // ── dolly ──────────────────────────────────────────────────────────────

    /** Fingers apart (>1) brings it closer; fingers together pushes it away. */
    @Test
    fun `a pinch dollies in the direction the fingers moved`() {
        val c = OrbitCamera(distanceM = 10f)
        assertClose(5f, c.dolly(2f).distanceM, 1e-4f, "pinch out")
        assertClose(20f, c.dolly(0.5f).distanceM, 1e-4f, "pinch in")
    }

    /**
     * The near clamp is the one that matters. Unclamped, a determined pinch
     * walks the eye through the target and out the far side — the cloud ends up
     * behind the camera and the viewer looks broken rather than zoomed.
     */
    @Test
    fun `the dolly is clamped at both ends`() {
        var inward = OrbitCamera(distanceM = 10f)
        repeat(100) { inward = inward.dolly(2f) }
        assertEquals(OrbitCamera.MIN_DISTANCE_M, inward.distanceM, 1e-6f)
        assertTrue("the eye must never reach the target", distanceFromTarget(inward) > 0f)

        var outward = OrbitCamera(distanceM = 10f)
        repeat(100) { outward = outward.dolly(0.5f) }
        assertEquals(OrbitCamera.MAX_DISTANCE_M, outward.distanceM, 1e-6f)
    }

    /**
     * A gesture detector that reports nonsense must not be able to send the
     * distance to NaN — from which no later gesture could recover it, and the
     * viewer would be dead until the screen was left and re-entered.
     */
    @Test
    fun `a nonsense scale factor is refused, not propagated`() {
        val c = OrbitCamera(distanceM = 10f)
        assertEquals(10f, c.dolly(0f).distanceM, 0f)
        assertEquals(10f, c.dolly(-3f).distanceM, 0f)
        assertEquals(10f, c.dolly(Float.NaN).distanceM, 0f)
        assertEquals(10f, c.dolly(Float.POSITIVE_INFINITY).distanceM, 0f)
    }

    /** `dollyBy(+0.1)` is "10 % closer", which is the form a scroll wheel wants. */
    @Test
    fun `dollyBy is a signed fraction`() {
        assertClose(10f / 1.1f, OrbitCamera(distanceM = 10f).dollyBy(0.1f).distanceM, 1e-4f)
        assertClose(10f / 0.9f, OrbitCamera(distanceM = 10f).dollyBy(-0.1f).distanceM, 1e-4f)
    }

    // ── reset framing ──────────────────────────────────────────────────────

    /**
     * Reset frames what is THERE. A scan whose geometry sits forty metres from
     * the session origin must not be "reset" to a view of empty space — the
     * operator's escape hatch from a lost camera would itself lose the camera.
     */
    @Test
    fun `reset centres on the cloud, not on the origin`() {
        val framed = OrbitCamera.framing(38f, -1f, 40f, 44f, 3f, 52f)
        assertClose(41f, framed.targetX, 1e-3f, "target x")
        assertClose(1f, framed.targetY, 1e-3f, "target y")
        assertClose(46f, framed.targetZ, 1e-3f, "target z")
        // The direction you look FROM is a preference; the home angles stand.
        assertClose(OrbitCamera.HOME.yawRad, framed.yawRad, 1e-5f, "yaw")
        assertClose(OrbitCamera.HOME.pitchRad, framed.pitchRad, 1e-5f, "pitch")
    }

    /** A bigger cloud is framed from further back, and the whole sphere fits. */
    @Test
    fun `reset pulls back far enough to hold the bounding sphere`() {
        val small = OrbitCamera.framing(-1f, -1f, -1f, 1f, 1f, 1f)
        val big = OrbitCamera.framing(-50f, -50f, -50f, 50f, 50f, 50f)
        assertTrue(big.distanceM > small.distanceM * 40f)
        // The half-angle the sphere subtends must be inside the field of view.
        val radius = 0.5f * sqrt(3f * 100f * 100f)
        val subtended = kotlin.math.asin((radius / big.distanceM).coerceIn(-1f, 1f))
        assertTrue(
            "the cloud must fit the 45° vertical FoV, subtended $subtended",
            subtended < OrbitCamera.DEFAULT_FOV_Y_RAD / 2f,
        )
    }

    /**
     * A cloud that has not loaded, or one single point, must fall back rather
     * than divide by a zero radius. `framing` is called on a double tap, which
     * is exactly the thing an operator does when the viewer looks empty.
     */
    @Test
    fun `a degenerate bound falls back to home`() {
        assertEquals(OrbitCamera.HOME.distanceM, OrbitCamera.framing(0f, 0f, 0f, 0f, 0f, 0f).distanceM, 1e-4f)
        assertEquals(OrbitCamera.HOME, OrbitCamera.framing(1f, 1f, 1f, -1f, -1f, -1f))
        assertEquals(
            OrbitCamera.HOME,
            OrbitCamera.framing(0f, 0f, 0f, Float.NaN, 1f, 1f),
        )
    }

    /** The framing distance obeys the same clamps every other path does. */
    @Test
    fun `reset respects the distance clamps`() {
        val vast = OrbitCamera.framing(-1e6f, -1e6f, -1e6f, 1e6f, 1e6f, 1e6f)
        assertEquals(OrbitCamera.MAX_DISTANCE_M, vast.distanceM, 1e-3f)
    }
}
