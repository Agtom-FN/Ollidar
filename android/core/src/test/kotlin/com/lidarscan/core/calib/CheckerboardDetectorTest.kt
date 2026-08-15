package com.lidarscan.core.calib

import kotlin.math.abs
import kotlin.math.hypot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The detector's verified envelope, stated as tests rather than as a claim.
 *
 * Every case renders a board at a KNOWN pose ([SyntheticBoard], a ray-caster
 * — so the ground truth is the pose, not a drawn quad), runs the detector,
 * and compares against the exact projected corner positions. What is *not*
 * covered is equally important and is named in the detector's own doc: no
 * real camera image has ever been through this code.
 */
class CheckerboardDetectorTest {

    private val spec = CheckerboardSpec(cols = 8, rows = 6, squareSizeM = 0.080)
    private val intrinsics = PinholeIntrinsics(
        fx = 620.0, fy = 620.0, cx = 320.0, cy = 240.0, width = 640, height = 480,
    )
    private val detector = SaddleCheckerboardDetector()

    private fun detectAt(
        azimuthDeg: Double,
        elevationDeg: Double,
        rollDeg: Double,
        distanceM: Double = 1.6,
        noiseSigma: Double = 2.0,
        blurSigma: Double = 0.8,
    ): Pair<CheckerboardDetection?, Mat4> {
        val pose = SyntheticBoard.poseFor(spec, azimuthDeg, elevationDeg, rollDeg, distanceM)
        val image = SyntheticBoard.render(spec, intrinsics, pose, blurSigma, noiseSigma)
        return detector.detect(image, spec) to pose
    }

    /** Corner-position error against the exact projection, in pixels. */
    private fun cornerError(detection: CheckerboardDetection, pose: Mat4): Double {
        val truth = SyntheticBoard.projectCorners(spec, intrinsics, pose)
        // The board is 180°-symmetric, so a detection may legitimately be
        // ordered from the opposite corner. Score both and take the better —
        // which is exactly what downstream cares about, since the plane is
        // invariant to the labelling.
        val forward = detection.corners.indices.maxOf {
            hypot(detection.corners[it].x - truth[it].x, detection.corners[it].y - truth[it].y)
        }
        val n = truth.size
        val reversed = detection.corners.indices.maxOf {
            hypot(detection.corners[it].x - truth[n - 1 - it].x, detection.corners[it].y - truth[n - 1 - it].y)
        }
        return minOf(forward, reversed)
    }

    @Test
    fun `detects a fronto-parallel board`() {
        val (detection, pose) = detectAt(0.0, 0.0, 0.0)
        assertNotNull("no detection on the easiest possible case", detection)
        assertEquals(spec.cornerCount, detection!!.corners.size)
        assertTrue("worst corner error ${cornerError(detection, pose)} px", cornerError(detection, pose) < 0.5)
    }

    /**
     * The poses the wizard actually prescribes: azimuth −38…+38, elevation
     * −24…+26, roll −60…+60 (WIZARD.md screen 2). Roll is the axis S6 found
     * doubles the error when it is missing, so a detector that quietly failed
     * on rolled poses would silently reintroduce that failure.
     */
    @Test
    fun `detects across the prescribed pose sweep including roll`() {
        var detected = 0
        var worst = 0.0
        val poses = PosePlan.poses(8)
        for (p in poses) {
            val (detection, pose) = detectAt(p.azimuthDeg, p.elevationDeg, p.rollDeg)
            if (detection != null) {
                detected++
                worst = maxOf(worst, cornerError(detection, pose))
            }
        }
        assertEquals("every prescribed pose should be detectable on synthetic imagery", poses.size, detected)
        // Measured worst case over this sweep at the time of writing: 0.26 px.
        assertTrue("worst corner error across the sweep: $worst px", worst < 0.6)
    }

    @Test
    fun `detects a hard oblique pose`() {
        val (detection, pose) = detectAt(azimuthDeg = 38.0, elevationDeg = 26.0, rollDeg = -55.0)
        assertNotNull(detection)
        assertTrue(cornerError(detection!!, pose) < 1.0)
    }

    @Test
    fun `survives heavier blur and noise`() {
        val (detection, pose) = detectAt(12.0, -8.0, 30.0, noiseSigma = 6.0, blurSigma = 1.6)
        assertNotNull("detection lost to blur+noise", detection)
        assertTrue(cornerError(detection!!, pose) < 1.5)
    }

    /** No board in frame must be no detection — not a grid fitted to noise. */
    @Test
    fun `returns null on an empty scene`() {
        val flat = LumaImage(640, 480, ByteArray(640 * 480) { 128.toByte() })
        assertNull(detector.detect(flat, spec))
    }

    @Test
    fun `returns null when the expected grid is larger than the board in frame`() {
        // A 12x9 board is asked for; a 8x6 one is shown. The lattice growth
        // finds a block, but it is the wrong size, so the detection must fail
        // rather than return 108 corners of which 60 are invented.
        val (detection, _) = detectAt(0.0, 0.0, 0.0)
        assertNotNull(detection)
        val pose = SyntheticBoard.poseFor(spec, 0.0, 0.0, 0.0, 1.6)
        val image = SyntheticBoard.render(spec, intrinsics, pose)
        assertNull(detector.detect(image, CheckerboardSpec(cols = 12, rows = 9, squareSizeM = 0.080)))
    }

    /** Subpixel refinement must actually do something: integer positions would show up as a quantised error floor. */
    @Test
    fun `subpixel refinement lands corners off the integer grid`() {
        val (detection, _) = detectAt(6.0, 4.0, 12.0)
        assertNotNull(detection)
        val fractional = detection!!.corners.count { c ->
            abs(c.x - Math.round(c.x)) > 1e-6 || abs(c.y - Math.round(c.y)) > 1e-6
        }
        assertTrue("only $fractional of ${detection.corners.size} corners were refined", fractional > detection.corners.size / 2)
    }
}
