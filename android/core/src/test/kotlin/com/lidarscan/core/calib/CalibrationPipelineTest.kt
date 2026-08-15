package com.lidarscan.core.calib

import java.io.File
import kotlin.random.Random
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Everything in the wizard's path that is not the detector or the plane
 * estimator: the geometry vocabulary, the lidar-side segmentation, the pose
 * plan and its diversity wheel, the live checks and their automatic shutter,
 * the verdict banding, and the device-level store.
 */
class CalibrationPipelineTest {

    // --- geometry -----------------------------------------------------------

    @Test
    fun `a rigid transform round-trips through its inverse`() {
        val t = Mat4.fromRotationTranslation(
            Quat.fromAxisAngle(Vec3(0.3, 1.0, -0.2), 0.7),
            Vec3(0.1, -0.06, 0.02),
        )
        assertTrue(t.isRigid())
        val p = Vec3(1.2, -0.4, 3.0)
        val back = t.inverseRigid().transform(t.transform(p))
        assertEquals(p.x, back.x, 1e-12)
        assertEquals(p.y, back.y, 1e-12)
        assertEquals(p.z, back.z, 1e-12)
    }

    /**
     * The one failure `scan_engine_set_mount_extrinsics` explicitly refuses:
     * "A column-major matrix handed across JNI is REJECTED instead of
     * producing a plausible-looking mirrored cloud nobody notices until
     * export." The app-side check has to catch the same thing, or the only
     * signal is a bare SCAN_ERR_INVALID_ARGUMENT.
     */
    @Test
    fun `isRigid rejects a transposed (column-major) rotation with translation in the wrong row`() {
        val rowMajor = Mat4.fromRotationTranslation(
            Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), 0.6),
            Vec3(0.05, -0.09, 0.01),
        )
        // What a column-major producer would hand over: the same numbers
        // transposed, which puts the translation in the bottom ROW.
        val cm = DoubleArray(16)
        for (r in 0 until 4) for (c in 0 until 4) cm[r * 4 + c] = rowMajor.m[c * 4 + r]
        assertFalse("a transposed matrix must not pass the rigid check", Mat4(cm).isRigid())
    }

    @Test
    fun `fromColumnMajor undoes an ARCore-style matrix`() {
        val rowMajor = Mat4.fromRotationTranslation(
            Quat.fromAxisAngle(Vec3(1.0, 0.0, 0.0), -0.4),
            Vec3(0.0, -0.06, -0.035),
        )
        val columnMajor = FloatArray(16)
        for (r in 0 until 4) for (c in 0 until 4) columnMajor[c * 4 + r] = rowMajor.m[r * 4 + c].toFloat()
        val back = Mat4.fromColumnMajor(columnMajor)
        for (i in 0 until 16) assertEquals(rowMajor.m[i], back.m[i], 1e-6)
        assertTrue(back.isRigid(1e-5))
    }

    @Test
    fun `quaternion angleTo is sign-insensitive`() {
        val q = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), 1.1).normalized()
        val negated = Quat(-q.x, -q.y, -q.z, -q.w)
        // A quaternion and its negation are the same rotation; ARCore hands
        // out either sign freely (A8 §3.4).
        assertEquals(0.0, q.angleTo(negated), 1e-9)
        val rotated = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), 1.1 + 0.25).normalized()
        assertEquals(0.25, q.angleTo(rotated), 1e-9)
    }

    @Test
    fun `rotation matrix and quaternion agree`() {
        val q = Quat.fromAxisAngle(Vec3(0.4, -0.8, 0.3), 2.2).normalized()
        val m = Mat4.fromRotationTranslation(q, Vec3.ZERO)
        assertEquals(0.0, q.angleTo(m.rotationQuat()), 1e-9)
    }

    // --- lidar-side segmentation -------------------------------------------

    private val boardPlaneCamera = TargetPlaneObservation(
        normal = Vec3(0.0, 0.0, -1.0),
        d = 1.6,
        cameraFromBoard = Mat4.identity(),
        reprojectionRmsPx = 0.1,
        incidenceDeg = 0.0,
        distanceM = 1.6,
    )

    @Test
    fun `predicting the plane in the lidar frame accounts for the mount offset`() {
        // Lidar 6 cm above the camera along -y; a plane 1.6 m away along -z
        // must come back at the same distance (the offset is perpendicular to
        // the normal, so it changes nothing) ...
        val alongY = BoardSegmenter.predictInLidarFrame(
            boardPlaneCamera,
            Mat4.fromRotationTranslation(Quat.IDENTITY, Vec3(0.0, -0.06, 0.0)),
        )
        assertEquals(1.6, alongY.d, 1e-12)

        // ... but a 3.5 cm offset ALONG the normal does, by exactly that much.
        val alongZ = BoardSegmenter.predictInLidarFrame(
            boardPlaneCamera,
            Mat4.fromRotationTranslation(Quat.IDENTITY, Vec3(0.0, 0.0, -0.035)),
        )
        assertEquals(1.6 - 0.035, alongZ.d, 1e-12)
    }

    /** Mid-360: a 3-D scanner sees a patch, so a plane is fitted. */
    private fun planePatch(count: Int, plane: LidarPlane, noise: Double, rng: Random): List<Vec3> {
        // Build an orthonormal basis in the plane.
        val n = plane.normal.normalized()
        val a = (if (kotlin.math.abs(n.x) < 0.9) Vec3(1.0, 0.0, 0.0) else Vec3(0.0, 1.0, 0.0))
        val u = (a cross n).normalized()
        val v = (n cross u).normalized()
        val centre = n * plane.d
        return (0 until count).map {
            val s = (rng.nextDouble() - 0.5) * 0.7
            val t = (rng.nextDouble() - 0.5) * 0.55
            centre + u * s + v * t + n * ((rng.nextDouble() - 0.5) * 2 * noise)
        }
    }

    @Test
    fun `segments a Mid-360 patch and rejects the wall behind it`() {
        val rng = Random(11)
        val plane = LidarPlane(Vec3(0.0, 0.0, -1.0).normalized(), 1.6)
        val board = planePatch(400, plane, 0.005, rng)
        val wall = planePatch(400, LidarPlane(plane.normal, 2.3), 0.01, rng)
        val seg = BoardSegmenter.segment(board + wall, plane, LidarProfile.MID360, random = rng)

        assertTrue("kept ${seg.count} returns", seg.count >= 350)
        assertTrue("the wall must not be in the segmentation", seg.count <= board.size + 5)
        assertTrue("residual rms ${seg.residualRmsM}", seg.residualRmsM < 0.01)
    }

    /**
     * D6: a 2-D scanner returns a LINE across the board. Fitting a plane to it
     * is rank-deficient by construction — which is the geometry behind S6's
     * "2 constraints per pose instead of 3" — so the segmenter must fit a
     * line in the scan plane instead.
     */
    @Test
    fun `segments a D6 scan line`() {
        val rng = Random(5)
        val plane = LidarPlane(Vec3(0.0, 1.0, 0.0), 1.5)
        val onBoard = (0 until 60).map {
            val x = (it - 30) * 0.012
            Vec3(x, 1.5 + (rng.nextDouble() - 0.5) * 0.01, 0.0)
        }
        val elsewhere = (0 until 40).map {
            Vec3((it - 20) * 0.03, 2.6 + (rng.nextDouble() - 0.5) * 0.02, 0.0)
        }
        val seg = BoardSegmenter.segment(onBoard + elsewhere, plane, LidarProfile.D6, random = rng)
        assertTrue("kept ${seg.count}", seg.count >= 55)
        assertTrue(seg.points.all { it.z == 0.0 })
        assertTrue(seg.residualRmsM < 0.01)
    }

    @Test
    fun `an empty segmentation is reported, not faked`() {
        val plane = LidarPlane(Vec3(0.0, 0.0, -1.0), 1.6)
        val nowhereNear = (0 until 200).map { Vec3(0.0, 0.0, -0.2 - it * 0.001) }
        val seg = BoardSegmenter.segment(nowhereNear, plane, LidarProfile.MID360)
        assertEquals(0, seg.count)
        assertTrue(seg.toFloatArray().isEmpty())
    }

    @Test
    fun `toFloatArray lays out x y z per point for the JNI call`() {
        val seg = BoardSegmentation(listOf(Vec3(1.0, 2.0, 3.0), Vec3(4.0, 5.0, 6.0)), 0.0, 2)
        assertArrayEqualsF(floatArrayOf(1f, 2f, 3f, 4f, 5f, 6f), seg.toFloatArray())
    }

    private fun assertArrayEqualsF(expected: FloatArray, actual: FloatArray) {
        assertEquals(expected.size, actual.size)
        for (i in expected.indices) assertEquals(expected[i], actual[i], 1e-6f)
    }

    // --- pose plan + diversity ---------------------------------------------

    @Test
    fun `the prescribed sweep stays inside its ranges and varies roll`() {
        val poses = PosePlan.poses(12)
        assertEquals(12, poses.size)
        assertTrue(poses.all { it.azimuthDeg in -38.0..38.0 })
        assertTrue(poses.all { it.elevationDeg in -24.0..26.0 })
        assertTrue(poses.all { it.rollDeg in -60.0..60.0 })
        // S6 finding 3: roll variation is what the whole prescription is for.
        assertTrue("roll must span both signs", poses.any { it.rollDeg > 20 } && poses.any { it.rollDeg < -20 })
    }

    @Test
    fun `any prefix of the sweep is well spread`() {
        // The point of a low-discrepancy sequence: stopping at 5 or at 8 both
        // give an even spread, which is what makes "5 acceptable, 8
        // recommended, 12 no better" a real choice rather than a gamble.
        for (n in listOf(5, 8, 12)) {
            val wheel = DiversityWheel()
            PosePlan.poses(n).forEach { wheel.add(it.azimuthDeg, it.elevationDeg, it.rollDeg) }
            assertTrue("coverage at n=$n was ${wheel.coverage}", wheel.coverage >= 0.5)
        }
    }

    @Test
    fun `the diversity wheel names the axis a user is neglecting`() {
        val wheel = DiversityWheel()
        // A user who steps sideways but never tilts — exactly the failure S6
        // measured as doubling the error.
        for (i in 0 until 8) wheel.add(-38.0 + i * 9.0, -24.0 + i * 6.0, 0.0)
        assertEquals(DiversityWheel.Axis.ROLL, wheel.weakestAxis())
        val (_, _, roll) = wheel.axisCoverage()
        assertTrue("roll coverage should be minimal, was $roll", roll <= 1.0 / DiversityWheel.BUCKETS + 1e-9)
    }

    @Test
    fun `recommended pose counts follow S6`() {
        assertEquals(8, PosePlan.recommendedCount(LidarProfile.MID360))
        assertEquals(12, PosePlan.recommendedCount(LidarProfile.D6))
    }

    // --- live checks + shutter ---------------------------------------------

    private fun goodObservation(profile: LidarProfile) = LiveObservation(
        detection = CheckerboardDetection(
            CheckerboardSpec(),
            List(48) { Corner(100.0 + (it % 8) * 40.0, 100.0 + (it / 8) * 40.0, 1.0) },
            50.0,
        ),
        plane = TargetPlaneObservation(Vec3(0.0, 0.0, -1.0), 1.6, Mat4.identity(), 0.2, 20.0, 1.6),
        imageWidth = 640,
        imageHeight = 480,
        cameraRollDeg = 30.0,
        prescribedRollDeg = 25.0,
        lidarReturnsOnBoard = profile.minReturnsPerPose + 5,
        angularRateRadPerS = 0.05,
        linearSpeedMPerS = 0.01,
        trackingOk = true,
    )

    @Test
    fun `all five checks pass on a good frame`() {
        val thresholds = PoseCheckThresholds.forProfile(LidarProfile.MID360)
        val state = PoseChecker.evaluate(goodObservation(LidarProfile.MID360), thresholds)
        assertTrue("failing: ${PoseCheck.entries - state.passing}", state.allGreen)
        assertNull(state.firstFailure())
    }

    @Test
    fun `each check fails for its own documented reason`() {
        val thresholds = PoseCheckThresholds.forProfile(LidarProfile.MID360)
        val base = goodObservation(LidarProfile.MID360)

        assertFalse(PoseChecker.evaluate(base.copy(detection = null), thresholds).passing.contains(PoseCheck.BOARD_VISIBLE))
        assertFalse(
            PoseChecker.evaluate(
                base.copy(plane = base.plane!!.copy(incidenceDeg = 70.0)),
                thresholds,
            ).passing.contains(PoseCheck.VIEWING_ANGLE),
        )
        assertFalse(PoseChecker.evaluate(base.copy(cameraRollDeg = -20.0), thresholds).passing.contains(PoseCheck.ROLL_MATCHED))
        assertFalse(PoseChecker.evaluate(base.copy(lidarReturnsOnBoard = 3), thresholds).passing.contains(PoseCheck.LIDAR_SEES_IT))
        assertFalse(PoseChecker.evaluate(base.copy(angularRateRadPerS = 1.2), thresholds).passing.contains(PoseCheck.HOLD_STILL))
        assertFalse(PoseChecker.evaluate(base.copy(trackingOk = false), thresholds).passing.contains(PoseCheck.HOLD_STILL))
    }

    @Test
    fun `roll matching wraps around 180 degrees`() {
        assertEquals(10.0, PoseChecker.angleDifferenceDeg(-175.0, 175.0), 1e-9)
    }

    @Test
    fun `the shutter fires only after a continuous dwell`() {
        val timer = ShutterTimer(dwellMillis = 1_000)
        assertFalse(timer.update(true, 0).fire)
        assertFalse(timer.update(true, 500).fire)
        // A single red frame resets the ring — a pose that was briefly still
        // is exactly the pose S6's budget cannot absorb.
        assertEquals(0f, timer.update(false, 600).ring)
        // The dwell restarts from the first green frame after the reset (700),
        // so 1_600 is only 900 ms in and must NOT fire; 1_700 is 1_000 ms and
        // must.
        assertFalse(timer.update(true, 700).fire)
        assertFalse(timer.update(true, 1_600).fire)
        assertTrue(timer.update(true, 1_700).fire)
    }

    @Test
    fun `the shutter ring reports progress`() {
        val timer = ShutterTimer(dwellMillis = 1_000)
        timer.update(true, 0)
        assertEquals(0.4f, timer.update(true, 400).ring, 1e-6f)
    }

    // --- verdict ------------------------------------------------------------

    @Test
    fun `gate bands map to the engine's enum`() {
        assertEquals(CalibrationGate.GOOD, CalibrationGate.fromEngine(1))
        assertEquals(CalibrationGate.USABLE, CalibrationGate.fromEngine(2))
        assertEquals(CalibrationGate.REJECT, CalibrationGate.fromEngine(3))
        assertEquals(CalibrationGate.UNKNOWN, CalibrationGate.fromEngine(99))
    }

    /**
     * WIZARD.md screen 4's worked example: a Good verdict is shown as
     * "±5 mm at 3 m", not as a pixel count.
     */
    @Test
    fun `the gate is shown in millimetres, not pixels`() {
        val readout = GateReadout.of(CalibrationGate.GOOD, splitHalfPx = 8.0, focalPx = 1460.0)
        assertEquals(16.4, readout.millimetresAt3m, 0.2)
        assertTrue(readout.headline.contains("mm at 3 m"))
        assertFalse("the user must never be shown pixels", readout.headline.contains("px"))
    }

    // --- persistence --------------------------------------------------------

    private fun calibration(id: String, bracket: String = "reference-v1", serial: String? = "SN1") =
        MountCalibration(
            id = id,
            sensor = com.lidarscan.core.model.SensorType.MID360,
            bracketId = bracket,
            sensorSerial = serial,
            cameraFromLidar = Mat4.identity().m,
            splitHalfPx = 7.5,
            gate = CalibrationGate.GOOD,
            poseCount = 8,
            squareSizeM = 0.08,
            boardCols = 8,
            boardRows = 6,
            clockOffsetNs = -1_200_000,
            createdAtEpochMillis = 1_000L + id.hashCode().toLong().coerceAtLeast(0),
            appVersion = "0.1.0",
            phoneModel = "Pixel 8",
        )

    @Test
    fun `the device store round-trips and is keyed by phone, bracket and serial`() {
        val dir = createTempDir("calib-store")
        try {
            val store = FileMountCalibrationStore(File(dir, "mount_calibrations.json"))
            assertTrue(store.all().isEmpty())
            assertNull(store.find("Pixel 8", "reference-v1", "SN1"))

            val c = calibration("a")
            store.save(c)
            val found = store.find("Pixel 8", "reference-v1", "SN1")
            assertNotNull(found)
            assertEquals(c.id, found!!.id)
            assertEquals(CalibrationGate.GOOD, found.gate)
            assertEquals(-1_200_000L, found.clockOffsetNs)
            assertEquals(16, found.cameraFromLidar.size)

            // A different bracket is a different calibration, not this one.
            assertNull(store.find("Pixel 8", "other-bracket", "SN1"))
        } finally {
            dir.deleteRecursively()
        }
    }

    @Test
    fun `saving the same key replaces rather than accumulates`() {
        val dir = createTempDir("calib-store2")
        try {
            val store = FileMountCalibrationStore(File(dir, "mount_calibrations.json"))
            store.save(calibration("first"))
            store.save(calibration("second"))
            assertEquals(1, store.all().size)
            assertEquals("second", store.all().first().id)
        } finally {
            dir.deleteRecursively()
        }
    }

    @Test
    fun `a lidar with no serial still matches the same phone and bracket`() {
        val dir = createTempDir("calib-store3")
        try {
            val store = FileMountCalibrationStore(File(dir, "mount_calibrations.json"))
            store.save(calibration("d6", serial = null))
            assertNotNull(store.find("Pixel 8", "reference-v1", null))
            assertNotNull("a swapped-but-identical unit should still default to the bracket's calibration",
                store.find("Pixel 8", "reference-v1", "SOME-OTHER-SERIAL"))
        } finally {
            dir.deleteRecursively()
        }
    }

    @Test
    fun `a corrupt store degrades to empty instead of blocking capture`() {
        val dir = createTempDir("calib-store4")
        try {
            val file = File(dir, "mount_calibrations.json")
            file.writeText("{ this is not json")
            val store = FileMountCalibrationStore(file)
            assertTrue(store.all().isEmpty())
            store.save(calibration("recovered"))
            assertEquals(1, store.all().size)
        } finally {
            dir.deleteRecursively()
        }
    }

    @Suppress("DEPRECATION")
    private fun createTempDir(prefix: String): File =
        File.createTempFile(prefix, "").let { it.delete(); it.mkdirs(); it }
}
