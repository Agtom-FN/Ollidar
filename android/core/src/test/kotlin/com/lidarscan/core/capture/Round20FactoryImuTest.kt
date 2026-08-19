package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 20 (item 81) — the factory LENS_POSE_ROTATION path of
 * [CameraFromImu.resolveWithFactory]: the per-unit calibration is used when it
 * agrees with the SENSOR_ORIENTATION convention, under EITHER documented
 * reading of the tag's direction; a convention mismatch falls back to the
 * coarse rotation loudly; an absent tag (every emulator) is the coarse path
 * unchanged. Tests must pass both ways — that is the item's own requirement.
 */
class Round20FactoryImuTest {

    private val rz90 = CameraFromImu.rearCamera(90)
    private val flip = CameraFromImu.CAMERA2_TO_ARCORE

    private fun deg(a: Quat, b: Quat) = Math.toDegrees(a.angleTo(b))

    /** The factory tag an IDEAL 90-degree device would carry, under reading A (camera->reference). */
    private fun idealLensA(): Quat = (flip.conjugate() * rz90).conjugate().normalized()

    /** ...and under reading B (reference->camera). */
    private fun idealLensB(): Quat = (flip.conjugate() * rz90).normalized()

    private fun xyzw(q: Quat) = doubleArrayOf(q.x, q.y, q.z, q.w)

    @Test
    fun `an absent tag is the coarse path, verbatim`() {
        val r = CameraFromImu.resolveWithFactory(null, 90, frontFacing = false)
        val coarse = CameraFromImu.resolve(90, frontFacing = false)
        assertEquals(0.0, deg(r.quat, coarse.quat), 1e-9)
        assertTrue(r.derived)
        assertFalse(r.why.contains("factory"))
    }

    @Test
    fun `an ideal factory tag reproduces the coarse rotation under reading A`() {
        val r = CameraFromImu.resolveWithFactory(xyzw(idealLensA()), 90, frontFacing = false)
        assertEquals(0.0, deg(r.quat, rz90), 1e-9)
        assertTrue(r.why.contains("factory"))
    }

    @Test
    fun `an ideal factory tag reproduces the coarse rotation under reading B too`() {
        val r = CameraFromImu.resolveWithFactory(xyzw(idealLensB()), 90, frontFacing = false)
        assertEquals(0.0, deg(r.quat, rz90), 1e-9)
        assertTrue(r.why.contains("factory"))
    }

    @Test
    fun `the per-unit deviation is what the factory path exists to carry`() {
        // A real unit is the ideal mount plus a couple of degrees of assembly
        // tolerance — the part the coarse Rz(90) guess cannot know.
        val tweak = Quat.fromAxisAngle(Vec3(1.0, 0.0, 0.0), Math.toRadians(2.0))
        val actual = (tweak * rz90).normalized()
        val lens = (flip.conjugate() * actual).conjugate().normalized()
        val r = CameraFromImu.resolveWithFactory(xyzw(lens), 90, frontFacing = false)
        assertEquals(0.0, deg(r.quat, actual), 1e-9)
        assertEquals(2.0, deg(r.quat, rz90), 1e-6)
        assertTrue(r.derived)
    }

    @Test
    fun `a convention mismatch falls back to the coarse rotation, loudly`() {
        // A tag 90 deg away from BOTH readings' expectations — not a
        // calibration this derivation covers.
        val nonsense = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), Math.toRadians(90.0))
        val lens = (flip.conjugate() * (nonsense * rz90).normalized()).conjugate().normalized()
        val r = CameraFromImu.resolveWithFactory(xyzw(lens), 90, frontFacing = false)
        assertEquals(0.0, deg(r.quat, rz90), 1e-9)
        assertTrue(r.why.contains("convention mismatch"))
    }

    @Test
    fun `a front camera never uses the factory path`() {
        val r = CameraFromImu.resolveWithFactory(xyzw(idealLensA()), 90, frontFacing = true)
        assertFalse(r.derived)
    }

    @Test
    fun `degenerate tags are refused - zero, NaN, wrong arity`() {
        val coarse = CameraFromImu.resolve(90, frontFacing = false)
        for (bad in listOf(
            doubleArrayOf(0.0, 0.0, 0.0, 0.0),
            doubleArrayOf(Double.NaN, 0.0, 0.0, 1.0),
            doubleArrayOf(1.0, 0.0, 0.0),
        )) {
            val r = CameraFromImu.resolveWithFactory(bad, 90, frontFacing = false)
            assertEquals(0.0, deg(r.quat, coarse.quat), 1e-9)
        }
    }
}
