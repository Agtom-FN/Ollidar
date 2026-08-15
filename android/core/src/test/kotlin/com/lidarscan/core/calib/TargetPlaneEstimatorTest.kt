package com.lidarscan.core.calib

import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

/**
 * The camera half of the mount-calibration residual. `scan_mount_calib_add_
 * observation` takes exactly `(normal, d)` in the camera frame, so these are
 * the only two numbers whose accuracy the extrinsic inherits from the image
 * side — which is why they are checked against an exactly known pose rather
 * than against a reprojection residual (a homography can fit its own
 * correspondences beautifully and still return the wrong plane if the
 * decomposition's scale or sign is wrong).
 */
class TargetPlaneEstimatorTest {

    private val spec = CheckerboardSpec(cols = 8, rows = 6, squareSizeM = 0.080)
    private val intrinsics = PinholeIntrinsics(
        fx = 1460.0, fy = 1460.0, cx = 640.0, cy = 360.0, width = 1280, height = 720,
    )

    private fun detectionFrom(pose: Mat4, noisePx: Double = 0.0, seed: Int = 3): CheckerboardDetection {
        val rng = Random(seed)
        val corners = SyntheticBoard.projectCorners(spec, intrinsics, pose).map { c ->
            if (noisePx <= 0.0) {
                c
            } else {
                Corner(c.x + (rng.nextDouble() - 0.5) * 2 * noisePx, c.y + (rng.nextDouble() - 0.5) * 2 * noisePx, 1.0)
            }
        }
        return CheckerboardDetection(spec, corners, 1.0)
    }

    private fun truthPlane(pose: Mat4): Pair<Vec3, Double> {
        val r = pose.rotation()
        var n = Vec3(r[2], r[5], r[8]).normalized()
        var d = n dot pose.translation
        if (d < 0) { n = n * -1.0; d = -d }
        return n to d
    }

    @Test
    fun `recovers the plane exactly from noise-free correspondences`() {
        for (p in PosePlan.poses(8)) {
            val pose = SyntheticBoard.poseFor(spec, p.azimuthDeg, p.elevationDeg, p.rollDeg, 1.6)
            val obs = TargetPlaneEstimator.estimate(detectionFrom(pose), intrinsics)
            assertNotNull("no plane for pose ${p.index}", obs)
            val (n, d) = truthPlane(pose)
            val angleDeg = Math.toDegrees(angleBetween(obs!!.normal, n))
            assertTrue("normal off by $angleDeg deg at pose ${p.index}", angleDeg < 0.05)
            assertEquals("d at pose ${p.index}", d, obs.d, 5e-4)
            assertTrue("reprojection rms ${obs.reprojectionRmsPx}", obs.reprojectionRmsPx < 0.05)
        }
    }

    /**
     * With 0.5 px of corner noise — roughly what the detector's own synthetic
     * error budget looks like — the plane must still be good to a fraction of
     * a degree. S6's target for the whole extrinsic is 0.16°, and the camera
     * plane is only one of its inputs.
     */
    @Test
    fun `degrades gracefully with corner noise`() {
        val pose = SyntheticBoard.poseFor(spec, 20.0, -10.0, 35.0, 1.6)
        val obs = TargetPlaneEstimator.estimate(detectionFrom(pose, noisePx = 0.5), intrinsics)
        assertNotNull(obs)
        val (n, d) = truthPlane(pose)
        assertTrue(Math.toDegrees(angleBetween(obs!!.normal, n)) < 0.6)
        assertEquals(d, obs.d, 0.01)
    }

    @Test
    fun `d is positive and the board is in front of the camera`() {
        val pose = SyntheticBoard.poseFor(spec, -30.0, 18.0, -45.0, 2.0)
        val obs = TargetPlaneEstimator.estimate(detectionFrom(pose), intrinsics)!!
        assertTrue("the C ABI requires a positive d", obs.d > 0)
        assertTrue("board centre must be in front of the lens", obs.boardCentreCamera.z > 0)
    }

    @Test
    fun `incidence and distance match the commanded pose`() {
        val obs = TargetPlaneEstimator.estimate(
            detectionFrom(SyntheticBoard.poseFor(spec, 0.0, 0.0, 0.0, 1.75)),
            intrinsics,
        )!!
        assertEquals(1.75, obs.distanceM, 0.01)
        assertTrue("head-on should be near 0 deg incidence, got ${obs.incidenceDeg}", obs.incidenceDeg < 1.0)

        val oblique = TargetPlaneEstimator.estimate(
            detectionFrom(SyntheticBoard.poseFor(spec, 38.0, 0.0, 0.0, 1.6)),
            intrinsics,
        )!!
        assertEquals(38.0, oblique.incidenceDeg, 1.5)
    }

    @Test
    fun `refuses a degenerate correspondence set`() {
        // Every corner on one image row: the homography's null space is
        // two-dimensional, and returning "a" solution would be inventing one.
        val collinear = CheckerboardDetection(
            spec,
            spec.objectPoints().mapIndexed { i, _ -> Corner(100.0 + i * 3.0, 240.0, 1.0) },
            1.0,
        )
        assertNull(TargetPlaneEstimator.estimate(collinear, intrinsics))
    }

    @Test
    fun `the recovered pose reprojects the corners it was given`() {
        val pose = SyntheticBoard.poseFor(spec, 25.0, 15.0, -20.0, 1.4)
        val detection = detectionFrom(pose)
        val obs = TargetPlaneEstimator.estimate(detection, intrinsics)!!
        var worst = 0.0
        spec.objectPoints().forEachIndexed { i, o ->
            val p = obs.cameraFromBoard.transform(o)
            val u = intrinsics.fx * p.x / p.z + intrinsics.cx
            val v = intrinsics.fy * p.y / p.z + intrinsics.cy
            worst = maxOf(worst, abs(u - detection.corners[i].x), abs(v - detection.corners[i].y))
        }
        assertTrue("worst reprojection $worst px", worst < 0.1)
    }
}
