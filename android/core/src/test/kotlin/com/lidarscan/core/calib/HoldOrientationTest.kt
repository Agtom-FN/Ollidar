package com.lidarscan.core.calib

import com.lidarscan.core.capture.PoseSample
import com.lidarscan.core.model.SensorType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

/**
 * ROUND 26 item 125(b)/(c) — **the four quadrants, from synthetic gravity.**
 *
 * Two separate claims are under test here and they are deliberately not mixed
 * up with each other:
 *
 *  1. **The classifier is right.** Given where gravity is, the app names the
 *     quadrant an operator would name. Written first as literal device-frame
 *     vectors ("the sky is off the screen's right edge"), so that the statement
 *     being asserted needs no quaternion algebra to read; then through the full
 *     ARCore chain, where `SENSOR_ORIENTATION` and the operator's yaw both get
 *     to be wrong and must not be.
 *
 *  2. **The round-20 trim was already orientation-complete, and a 90° roll is
 *     a LANDSCAPE HOLD rather than "hold tilt" to be thrown away.** Item 125
 *     asks for this to be *verified*, and it is the half that could have been
 *     assumed: `MountTrim.fromHoldOrientation` cancels the whole swing, so the
 *     lidar comes out level in every quadrant, and `MountTrimSampler` — the
 *     gate that a landscape hold has to clear to become a trim at all — has no
 *     opinion about the mean's value, only about its dispersion. If either of
 *     those ever stops being true, a landscape scan comes out on its side and
 *     these are the tests that say so.
 */
class HoldOrientationTest {

    private val up = Vec3(0.0, 1.0, 0.0)

    private fun assertVecClose(expected: Vec3, actual: Vec3, what: String, tol: Double = 1e-9) {
        assertTrue(
            "$what: expected $expected got $actual",
            abs(expected.x - actual.x) < tol &&
                abs(expected.y - actual.y) < tol &&
                abs(expected.z - actual.z) < tol,
        )
    }

    // --- 1. the classifier, from literal gravity ---------------------------

    @Test
    fun `each quadrant is named from where the sky is in the screen plane`() {
        // Device frame: +X right along the screen, +Y up along the screen, both
        // against the NATURAL orientation. "up" below is world-up expressed in
        // that frame, i.e. the direction of the sky as the screen sees it.
        assertEquals(
            DeviceOrientation.PORTRAIT,
            StartOrientation.fromDeviceUp(Vec3(0.0, 1.0, 0.0)).orientation,
        )
        // Sky off the screen's RIGHT edge: the right edge is uppermost, which is
        // the quarter-turn to the left — Surface.ROTATION_90.
        assertEquals(
            DeviceOrientation.LANDSCAPE_LEFT,
            StartOrientation.fromDeviceUp(Vec3(1.0, 0.0, 0.0)).orientation,
        )
        assertEquals(
            DeviceOrientation.PORTRAIT_REVERSE,
            StartOrientation.fromDeviceUp(Vec3(0.0, -1.0, 0.0)).orientation,
        )
        assertEquals(
            DeviceOrientation.LANDSCAPE_RIGHT,
            StartOrientation.fromDeviceUp(Vec3(-1.0, 0.0, 0.0)).orientation,
        )
    }

    @Test
    fun `the quadrant boundary is at forty five degrees, and it does not overlap`() {
        // 44 degrees off portrait is still portrait; 46 has crossed. Asserting
        // both sides of the same edge is what stops a future "just widen it a
        // bit" from silently making two quadrants claim the same hold.
        val nearlyPortrait = StartOrientation.fromDeviceUp(
            Vec3(Math.sin(Math.toRadians(44.0)), Math.cos(Math.toRadians(44.0)), 0.0),
        )
        val justLandscape = StartOrientation.fromDeviceUp(
            Vec3(Math.sin(Math.toRadians(46.0)), Math.cos(Math.toRadians(46.0)), 0.0),
        )
        assertEquals(DeviceOrientation.PORTRAIT, nearlyPortrait.orientation)
        assertEquals(DeviceOrientation.LANDSCAPE_LEFT, justLandscape.orientation)
        assertEquals(44.0, nearlyPortrait.screenUpAngleDeg, 1e-9)
        assertEquals(46.0, justLandscape.screenUpAngleDeg, 1e-9)
    }

    @Test
    fun `a phone lying flat says it does not know rather than guessing`() {
        // Screen up at the ceiling: gravity is along the device's own +Z and the
        // screen-plane angle is whatever the last microtremor said. A ceiling
        // scan has no "which way up", and reporting one would be a measurement
        // the app did not make.
        val flat = StartOrientation.fromDeviceUp(Vec3(0.002, -0.001, 1.0).normalized())
        assertFalse("a flat phone must not be confident", flat.confident)
        assertEquals(DeviceOrientation.PORTRAIT, flat.orientation)
        assertTrue(flat.logSuffix().contains("too flat"))

        // And 25 degrees off flat — above the 20-degree floor — IS believed.
        val tilted = StartOrientation.fromDeviceUp(
            Vec3(0.0, Math.sin(Math.toRadians(25.0)), Math.cos(Math.toRadians(25.0))),
        )
        assertTrue("25 degrees off flat is above the floor", tilted.confident)
        assertEquals(DeviceOrientation.PORTRAIT, tilted.orientation)
        assertEquals(25.0, tilted.tiltFromFlatDeg, 1e-9)
    }

    // --- 1b. ROUND 33 item 179(a): the OTHER angle of the same vector ------

    @Test
    fun `pitch is the elevation of the same vector, and positive means leaning back`() {
        // Upright: world-up lies in the screen plane, so there is no elevation.
        assertEquals(0.0, StartOrientation.fromDeviceUp(Vec3(0.0, 1.0, 0.0)).screenPitchDeg, 1e-9)

        // Screen facing the SKY — the phone lying on its back on a table. World
        // up is straight out of the screen (+Z), which is the extreme of
        // "leaning back": the top edge has gone away from the operator and the
        // rear camera is aimed at the floor.
        assertEquals(90.0, StartOrientation.fromDeviceUp(Vec3(0.0, 0.0, 1.0)).screenPitchDeg, 1e-9)

        // Face down: the other extreme.
        assertEquals(-90.0, StartOrientation.fromDeviceUp(Vec3(0.0, 0.0, -1.0)).screenPitchDeg, 1e-9)

        // And a real hold: 25 degrees back, held portrait.
        val back = StartOrientation.fromDeviceUp(
            Vec3(0.0, Math.cos(Math.toRadians(25.0)), Math.sin(Math.toRadians(25.0))),
        )
        assertEquals(25.0, back.screenPitchDeg, 1e-9)
        assertEquals(DeviceOrientation.PORTRAIT, back.orientation)
        assertEquals("the in-plane bearing is untouched by the lean", 0.0, back.screenUpAngleDeg, 1e-9)
    }

    @Test
    fun `pitch needs no unit vector, because it is an atan2 and not an asin`() {
        // The callers do not agree about magnitude: an accelerometer hands over
        // 9.81 m/s squared and the filter hands over a normalised direction. A
        // pitch derived by asin against the vector's own length would be right
        // for one of them and quietly wrong for the other.
        val raw = StartOrientation.fromDeviceUp(
            Vec3(0.0, 9.81 * Math.cos(Math.toRadians(31.0)), 9.81 * Math.sin(Math.toRadians(31.0))),
        )
        assertEquals(31.0, raw.screenPitchDeg, 1e-9)
    }

    @Test
    fun `pitch and tilt-from-flat are one fact, so they can never disagree`() {
        // `tiltFromFlatDeg` is the unsigned complement of the pitch: it is how
        // far off flat, and the pitch is which way. Round 26 already shipped the
        // first; item 179 needed the sign, and taking it from a second
        // derivation would be how the two come to differ by a degree.
        for (deg in listOf(-88.0, -47.0, -12.0, 0.0, 12.0, 47.0, 88.0)) {
            val r = Math.toRadians(deg)
            val got = StartOrientation.fromDeviceUp(Vec3(0.0, Math.cos(r), Math.sin(r)))
            assertEquals("pitch at $deg", deg, got.screenPitchDeg, 1e-9)
            assertEquals(
                "tilt-from-flat at $deg",
                90.0 - abs(deg),
                got.tiltFromFlatDeg,
                1e-9,
            )
        }
    }

    @Test
    fun `pitch is the same lean in every quadrant, at every yaw and every camera mounting`() {
        // The mirror of the roll's quadrant test above, and the property that
        // lets item 179's ghost have no branch in it: a roll turns the phone
        // about the screen's own normal, and the screen's normal is the one axis
        // that rotation cannot move. So a landscape hold leaning 20 degrees
        // forward reads exactly like a portrait hold leaning 20 degrees forward,
        // which is what makes "Tilt back." true in all four holds.
        for (orientation in DeviceOrientation.entries) {
            for (sensorDeg in listOf(0, 90, 180, 270)) {
                for (yaw in listOf(0.0, 37.1, 134.0, -51.6, 179.0)) {
                    for (tilt in listOf(90.0, 70.0, 55.0)) {
                        val hold = StartOrientation.syntheticHold(orientation, sensorDeg, yaw, tilt)
                        val got = StartOrientation.classify(hold, sensorDeg)
                        // `syntheticHold` pitches the device forward by
                        // `90 - tilt` about its own X, i.e. the top edge towards
                        // the operator, which is the negative sense.
                        assertEquals(
                            "pitch for $orientation at sensor $sensorDeg, yaw $yaw, tilt $tilt",
                            -(90.0 - tilt),
                            got.screenPitchDeg,
                            1e-6,
                        )
                        assertEquals(
                            "quadrant survives the lean",
                            orientation,
                            got.orientation,
                        )
                    }
                }
            }
        }
    }

    // --- 2. the classifier, through the whole ARCore chain -----------------

    @Test
    fun `every quadrant survives the camera frame, the sensor mounting and any yaw`() {
        // The three things that get to be wrong: which quadrant, where the
        // operator is facing, and what the rear camera's SENSOR_ORIENTATION is.
        // 0/90/270 are the values Android actually reports; 90 is the usual one.
        for (orientation in DeviceOrientation.entries) {
            for (sensorDeg in listOf(0, 90, 180, 270)) {
                for (yaw in listOf(0.0, 37.1, 134.0, -51.6, 179.0)) {
                    val hold = StartOrientation.syntheticHold(orientation, sensorDeg, yaw)
                    val got = StartOrientation.classify(hold, sensorDeg)
                    assertEquals(
                        "quadrant $orientation at sensor $sensorDeg, yaw $yaw",
                        orientation,
                        got.orientation,
                    )
                    assertTrue("upright holds are confident", got.confident)
                    assertEquals("upright is 90 degrees off flat", 90.0, got.tiltFromFlatDeg, 1e-6)
                }
            }
        }
    }

    @Test
    fun `an unknown SENSOR_ORIENTATION falls back to ninety, which is what phones report`() {
        val hold = StartOrientation.syntheticHold(DeviceOrientation.LANDSCAPE_LEFT, 90)
        assertEquals(
            DeviceOrientation.LANDSCAPE_LEFT,
            StartOrientation.classify(hold, sensorOrientationDeg = null).orientation,
        )
        // And a device that genuinely mounts its camera at 0 is misread by the
        // fallback — which is exactly why the probe is consulted first and the
        // fallback is documented rather than relied on.
        val zeroMount = StartOrientation.syntheticHold(DeviceOrientation.LANDSCAPE_LEFT, 0)
        assertEquals(
            DeviceOrientation.LANDSCAPE_LEFT,
            StartOrientation.classify(zeroMount, sensorOrientationDeg = 0).orientation,
        )
    }

    @Test
    fun `a tilted hold keeps its quadrant until it is genuinely flat`() {
        // Walking with the rig pitched 30 degrees down at the floor is a normal
        // scan, not an unknown orientation.
        val hold = StartOrientation.syntheticHold(
            DeviceOrientation.LANDSCAPE_RIGHT,
            sensorOrientationDeg = 90,
            yawDeg = 12.0,
            tiltFromFlatDeg = 60.0,
        )
        val got = StartOrientation.classify(hold, 90)
        assertEquals(DeviceOrientation.LANDSCAPE_RIGHT, got.orientation)
        assertTrue(got.confident)
        assertEquals(60.0, got.tiltFromFlatDeg, 1e-6)
    }

    @Test
    fun `the trim answers the same question as the hold it came from`() {
        // The trim drops the yaw. The yaw is a rotation ABOUT gravity, so it
        // cannot move gravity within the screen plane — classifying the trim's
        // recovered swing has to give the same quadrant as classifying the hold.
        for (orientation in DeviceOrientation.entries) {
            val hold = StartOrientation.syntheticHold(orientation, 90, yawDeg = 118.0)
            val trim = MountTrim.fromHoldOrientation(hold)
            assertEquals(
                "trim-derived quadrant for $orientation",
                StartOrientation.classify(hold, 90).orientation,
                StartOrientation.fromTrim(trim, 90).orientation,
            )
        }
    }

    // --- 3. the trim is orientation-complete ------------------------------

    @Test
    fun `the lidar comes out level in all four quadrants`() {
        // The claim item 125 actually cares about: after the trim is composed
        // onto the nominal, the sensor's own up axis points at the sky, whatever
        // way up the phone was held. `hold ∘ trim` is the world-from-lidar
        // rotation with an identity nominal, and it must be a pure yaw about
        // gravity — i.e. it must carry +Y to +Y exactly.
        for (orientation in DeviceOrientation.entries) {
            for (tilt in listOf(90.0, 70.0, 110.0)) {
                val hold = StartOrientation.syntheticHold(
                    orientation,
                    sensorOrientationDeg = 90,
                    yawDeg = -63.0,
                    tiltFromFlatDeg = tilt,
                )
                val trim = MountTrim.fromHoldOrientation(hold)
                val worldFromLidar = (hold * trim.rotation).normalized()
                assertVecClose(
                    up,
                    worldFromLidar.rotate(up),
                    "$orientation at tilt $tilt levels the sensor",
                    tol = 1e-9,
                )
                // Being a pure yaw is the stronger statement: the rotation axis
                // itself lies along gravity, so nothing but the operator's
                // facing survives.
                assertTrue(
                    "$orientation: the residual is a yaw about gravity",
                    abs(worldFromLidar.x) < 1e-9 && abs(worldFromLidar.z) < 1e-9,
                )
            }
        }
    }

    @Test
    fun `a landscape hold is a landscape hold, not a rejected tilt`() {
        // The specific worry item 125 names. Two things have to hold: the
        // sampler must ACCEPT a steady 90-degree-rolled hold (its gates are
        // about dispersion, never about the mean's value), and the trim it
        // produces must be the ~90-degree one that cancels the roll rather than
        // the ~0-degree one that would leave the map on its side.
        for (orientation in DeviceOrientation.entries) {
            val hold = StartOrientation.syntheticHold(orientation, 90)
            val samples = (0 until 24).map { i ->
                PoseSample(
                    tMonoNs = i * 40_000_000L,
                    position = Vec3.ZERO,
                    orientation = hold,
                    tracking = true,
                )
            }
            val result = MountTrimSampler.capture(samples, nowMillis = 1_700_000_000_000L, sensor = SensorType.COIN_D6)
            assertTrue(
                "$orientation hold must be accepted, got $result",
                result is MountTrimResult.Captured,
            )
            val trim = (result as MountTrimResult.Captured).trim
            assertTrue("the trim is gravity-referenced", trim.gravityReferenced)
            // Portrait is the 90-degree trim round 20 measured on the owner's
            // own holds; landscape is the one that has never been written down.
            val expected = when (orientation) {
                DeviceOrientation.PORTRAIT -> 90.0
                DeviceOrientation.PORTRAIT_REVERSE -> 90.0
                DeviceOrientation.LANDSCAPE_LEFT -> 0.0
                DeviceOrientation.LANDSCAPE_RIGHT -> 180.0
            }
            assertEquals(
                "$orientation trim magnitude",
                expected,
                trim.magnitudeDeg,
                1e-6,
            )
        }
    }

    @Test
    fun `the log line names the quadrant and carries its numbers`() {
        val hold = StartOrientation.syntheticHold(DeviceOrientation.LANDSCAPE_LEFT, 90, yawDeg = 20.0)
        val line = StartOrientation.classify(hold, 90).logSuffix()
        assertTrue("names the quadrant: $line", line.startsWith("landscape-left"))
        assertTrue("carries the roll: $line", line.contains("roll +90.0"))
        assertTrue("carries the tilt: $line", line.contains("tilt 90.0"))
        assertFalse("a confident reading says nothing about being flat", line.contains("too flat"))
    }
}
