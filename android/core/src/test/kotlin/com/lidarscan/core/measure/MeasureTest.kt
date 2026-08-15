package com.lidarscan.core.measure

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.tan

/**
 * B11 — the measure tool's picking and formatting.
 *
 * The projection is exercised against a hand-built view-projection matrix
 * rather than one taken from Filament, so the test asserts the *convention*
 * (row-major, +y up in NDC, +y down on screen) rather than agreeing with
 * whatever the renderer happened to hand over.
 */
class MeasureTest {

    /**
     * A row-major perspective * look-down-negative-z view, camera at the
     * origin looking along -z with +y up. Written out by hand so the row-major
     * expectation is explicit.
     */
    private fun perspectiveRowMajor(fovYDeg: Double, aspect: Double, near: Double, far: Double): DoubleArray {
        val f = 1.0 / tan(Math.toRadians(fovYDeg) / 2.0)
        return doubleArrayOf(
            f / aspect, 0.0, 0.0, 0.0,
            0.0, f, 0.0, 0.0,
            0.0, 0.0, (far + near) / (near - far), 2 * far * near / (near - far),
            0.0, 0.0, -1.0, 0.0,
        )
    }

    @Test
    fun `a point on the view axis projects to the screen centre`() {
        val vp = perspectiveRowMajor(60.0, 1.0, 0.1, 100.0)
        val hit = pickNearestPoint(
            points = listOf(Vec3(0f, 0f, -5f)),
            viewProjection = vp,
            viewportW = 1000,
            viewportH = 1000,
            tapX = 500f,
            tapY = 500f,
        )
        assertNotNull(hit)
        assertEquals(0f, hit!!.screenDistancePx, 0.5f)
        assertEquals(5f, hit.depthM, 1e-4f)
    }

    @Test
    fun `NDC plus-y maps to the upper half of the screen`() {
        val vp = perspectiveRowMajor(60.0, 1.0, 0.1, 100.0)
        // A point ABOVE the axis must land above the centre (smaller screen y).
        val hit = pickNearestPoint(
            points = listOf(Vec3(0f, 1f, -5f)),
            viewProjection = vp,
            viewportW = 1000,
            viewportH = 1000,
            tapX = 500f,
            tapY = 300f,
            radiusPx = 400f,
        )
        assertNotNull(hit)
        assertTrue("the +y point must project into the top half", hit!!.screenDistancePx < 400f)
        // And a tap symmetric about the centre in the OTHER direction must be
        // further away, which is what proves the flip happened.
        val below = pickNearestPoint(
            listOf(Vec3(0f, 1f, -5f)), vp, 1000, 1000, 500f, 700f, radiusPx = 400f,
        )
        assertTrue(below == null || below.screenDistancePx > hit.screenDistancePx)
    }

    @Test
    fun `points behind the camera are never picked`() {
        val vp = perspectiveRowMajor(60.0, 1.0, 0.1, 100.0)
        assertNull(pickNearestPoint(listOf(Vec3(0f, 0f, 5f)), vp, 1000, 1000, 500f, 500f))
    }

    @Test
    fun `an empty cloud or a zero viewport picks nothing rather than crashing`() {
        val vp = perspectiveRowMajor(60.0, 1.0, 0.1, 100.0)
        assertNull(pickNearestPoint(emptyList(), vp, 1000, 1000, 500f, 500f))
        assertNull(pickNearestPoint(listOf(Vec3(0f, 0f, -5f)), vp, 0, 0, 0f, 0f))
    }

    @Test
    fun `nothing within the radius returns null so the UI can say so`() {
        val vp = perspectiveRowMajor(60.0, 1.0, 0.1, 100.0)
        assertNull(pickNearestPoint(listOf(Vec3(0f, 0f, -5f)), vp, 1000, 1000, 50f, 50f, radiusPx = 10f))
    }

    @Test
    fun `the nearer of two points at the same pixel wins`() {
        val vp = perspectiveRowMajor(60.0, 1.0, 0.1, 100.0)
        val near = Vec3(0f, 0f, -3f)
        val far = Vec3(0f, 0f, -30f)
        // Far first in the list, so a naive "first match wins" would fail.
        val hit = pickNearestPoint(listOf(far, near), vp, 1000, 1000, 500f, 500f)
        assertEquals(near, hit!!.point)
    }

    @Test
    fun `stride samples the cloud instead of scanning all of it`() {
        val vp = perspectiveRowMajor(60.0, 1.0, 0.1, 100.0)
        val points = List(100) { Vec3(0f, 0f, -(it + 1).toFloat()) }
        val hit = pickNearestPoint(points, vp, 1000, 1000, 500f, 500f, stride = 10)
        // With stride 10 the first examined point is index 0.
        assertEquals(points[0], hit!!.point)
    }

    @Test
    fun `measurement decomposes into horizontal and vertical`() {
        val m = Measurement(Vec3(0f, 0f, 0f), Vec3(3f, 4f, 12f))
        assertEquals(13.0, m.distanceM, 1e-6)
        assertEquals(5.0, m.horizontalM, 1e-6)
        assertEquals(12.0, m.deltaZM, 1e-6)
    }

    @Test
    fun `metres are formatted at a precision that suits the magnitude`() {
        assertEquals("250 mm", formatDistance(0.25, MeasureUnit.METERS))
        assertEquals("2.500 m", formatDistance(2.5, MeasureUnit.METERS))
        assertEquals("42.50 m", formatDistance(42.5, MeasureUnit.METERS))
    }

    @Test
    fun `feet read as feet and inches, using the international foot`() {
        // 1 m = 3.280839895 ft = 3 ft 3.37 in
        assertEquals("3' 3.37\"", formatDistance(1.0, MeasureUnit.FEET))
        assertEquals("0' 0.00\"", formatDistance(0.0, MeasureUnit.FEET))
    }

    @Test
    fun `11 point 99 inches rounds up to the next foot, not to twelve inches`() {
        // 3.99958 ft — the classic off-by-one in a feet+inches formatter.
        val metres = 3.9999 * 0.3048
        assertEquals("4' 0.00\"", formatDistance(metres, MeasureUnit.FEET))
    }

    @Test
    fun `long distances drop to decimal feet`() {
        assertTrue(formatDistance(200.0, MeasureUnit.FEET).endsWith(" ft"))
    }
}
