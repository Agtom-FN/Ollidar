package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * ROUND 8: the third-person follow camera, driven by synthetic walks that carry
 * **ROUND 7's measured gait** — ±2 cm lateral sway, ±3 cm bob, ±3° yaw per step
 * at 2 Hz, the same model `engine/tests/test_pushbroom.cpp` and
 * `D6WalkingGaitPlanarityTest` use. Tuning the camera against one rig motion and
 * testing it against another would prove nothing about the phone in the
 * operator's hands.
 *
 * The pose rate throughout is **30 Hz**, ARCore's, because that is what actually
 * feeds `FollowCamera` on a phone-tracked D6 (`CaptureArController.publishPose`
 * fires once per ARCore frame).
 */
class FollowCameraTest {

    private companion object {
        const val POSE_HZ = 30.0
        const val GAIT_HZ = 2.0
        const val SWAY_M = 0.02
        const val BOB_M = 0.03
        const val WALK_MPS = 1.0

        fun nanos(tSeconds: Double): Long = (tSeconds * 1e9).toLong()
    }

    /**
     * A Y-up walk sample with ROUND 7's gait riding on it: [alongX]/[alongZ] are
     * the ideal ground-plane path, sway is applied perpendicular to it and bob
     * vertically. Height 1.5 m — a phone held at chest level.
     */
    private fun gaitSample(
        t: Double,
        alongX: Double,
        alongZ: Double,
        headingRad: Double,
    ): Triple<Double, Double, Double> {
        val phase = 2.0 * PI * GAIT_HZ * t
        // Perpendicular to the walk direction in the ground plane.
        val perpX = -sin(headingRad)
        val perpZ = cos(headingRad)
        val sway = SWAY_M * sin(phase)
        return Triple(
            alongX + perpX * sway,
            1.5 + BOB_M * sin(phase),
            alongZ + perpZ * sway,
        )
    }

    /**
     * RMS of a series about its own best-fit straight line — "how much does this
     * wobble, once the walk itself is taken out". The same shape of metric
     * ROUND 7's plane fit used, and for the same reason: a constant offset or a
     * constant drift is not roughness, and must not be allowed to masquerade as
     * it.
     */
    private fun detrendedRms(values: List<Double>): Double {
        val n = values.size
        if (n < 3) return 0.0
        var sx = 0.0; var sy = 0.0; var sxx = 0.0; var sxy = 0.0
        for (i in 0 until n) {
            val x = i.toDouble()
            sx += x; sy += values[i]; sxx += x * x; sxy += x * values[i]
        }
        val denom = n * sxx - sx * sx
        val slope = if (abs(denom) < 1e-12) 0.0 else (n * sxy - sx * sy) / denom
        val intercept = (sy - slope * sx) / n
        var acc = 0.0
        for (i in 0 until n) {
            val r = values[i] - (intercept + slope * i)
            acc += r * r
        }
        return sqrt(acc / n)
    }

    // --- the shape of the thing ---------------------------------------------

    @Test
    fun `walking along plus x puts the camera behind in x, above in y, looking at the rig`() {
        val cam = FollowCamera()
        var solution = cam.solution()
        var rigX = 0.0; var rigY = 0.0; var rigZ = 0.0

        val steps = (4.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
            rigX = x; rigY = y; rigZ = z
            solution = cam.update(nanos(t), x, y, z)
        }

        assertTrue("solution must be finite", solution.isFinite())
        assertTrue("heading must have been measured by 4 s of walking", solution.headingMeasured)

        // BEHIND: negative x of the rig, by roughly d·cos(35°).
        assertTrue(
            "eye x ${solution.eyeX} must be behind rig x $rigX",
            solution.eyeX < rigX - 1.0,
        )
        // ABOVE: greater height than the rig, by roughly d·sin(35°).
        assertTrue(
            "eye y ${solution.eyeY} must be above rig y $rigY",
            solution.eyeY > rigY + 1.0,
        )
        // …and it is behind along the WALK, not merely somewhere with a smaller
        // x: the lateral offset from the walk line is essentially nil.
        assertEquals("eye must sit on the walk line", 0.0, solution.eyeZ, 0.15)

        // The look-at target is the RIG — this is the assertion that fails for
        // the branch this replaces, whose target was the whole cloud's centroid
        // (at 4 s of walking from the origin, x = 2.0, i.e. two metres out).
        //
        // The tolerance is 15 cm and the measured error is 16 mm, which is the
        // velocity lead term in `FollowCamera.solve` doing its job: without it a
        // tau = 0.5 s low-pass would sit a full tau * v = 0.5 m behind at this
        // walking speed, and would fail this assertion outright at 1.5 m/s.
        val targetErr = hypot(solution.targetX - rigX, solution.targetZ - rigZ)
        assertTrue(
            "target (${solution.targetX}, ${solution.targetZ}) must track the rig ($rigX, $rigZ); err=$targetErr",
            targetErr < 0.15,
        )
        assertEquals("target height tracks the rig too", rigY, solution.targetY, 0.15)

        // Geometry check against the configured pitch: 35° below horizontal.
        val back = hypot(solution.targetX - solution.eyeX, solution.targetZ - solution.eyeZ)
        val rise = solution.eyeY - solution.targetY
        val pitchDeg = Math.toDegrees(kotlin.math.atan2(rise, back))
        assertEquals("pitched down 35°", 35.0, pitchDeg, 1.0)
    }

    // --- smoothing -----------------------------------------------------------

    @Test
    fun `gait sway and bob are low-passed out of the camera path`() {
        val cam = FollowCamera()
        val rawLateral = ArrayList<Double>()
        val rawHeight = ArrayList<Double>()
        val eyeLateral = ArrayList<Double>()
        val eyeHeight = ArrayList<Double>()

        val steps = (6.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
            val s = cam.update(nanos(t), x, y, z)
            // Measure only the last 3 s — the first three are the filters
            // priming, and including them would flatter the camera by comparing
            // its start transient against the rig's steady sway.
            if (t >= 3.0) {
                rawLateral += z
                rawHeight += y
                eyeLateral += s.eyeZ
                eyeHeight += s.eyeY
            }
        }

        val rawLatRms = detrendedRms(rawLateral)
        val eyeLatRms = detrendedRms(eyeLateral)
        val rawHgtRms = detrendedRms(rawHeight)
        val eyeHgtRms = detrendedRms(eyeHeight)

        // Sanity: the stimulus really is there. A ±2 cm sinusoid has an RMS of
        // 0.02/sqrt(2) = 14.1 mm, a ±3 cm one 21.2 mm.
        assertEquals("the rig really does sway ±2 cm", 0.0141, rawLatRms, 0.002)
        assertEquals("the rig really does bob ±3 cm", 0.0212, rawHgtRms, 0.003)

        // The claim, quantified. tau = 0.5 s against a 2 Hz fundamental predicts
        // a first-order gain of 1/sqrt(1 + (2*pi*2*0.5)^2) = 0.157, i.e. 6.4x.
        // MEASURED here:
        //
        //   lateral   rig 13.95 mm RMS -> camera 1.44 mm RMS   =  9.70x
        //   vertical  rig 20.93 mm RMS -> camera 3.58 mm RMS   =  5.85x
        //
        // Lateral beats the prediction because the half-window chord estimator
        // has already removed most of the sway before the low-pass sees it (the
        // heading and the lead term both ride on it); vertical is close to the
        // predicted 6.4x because bob enters the position filter directly.
        //
        // 4x is asserted rather than the measured 5.9x so that a re-tune inside
        // the documented tau band does not fail the suite for no behavioural
        // reason — but the ratios go into the failure message, so a regression
        // that merely halves the smoothing is visible in the report rather than
        // silently passing.
        val latRatio = rawLatRms / max(eyeLatRms, 1e-9)
        val hgtRatio = rawHgtRms / max(eyeHgtRms, 1e-9)
        assertTrue(
            "camera lateral jitter ${eyeLatRms}m vs rig ${rawLatRms}m — only ${latRatio}x smoother",
            latRatio >= 4.0,
        )
        assertTrue(
            "camera height jitter ${eyeHgtRms}m vs rig ${rawHgtRms}m — only ${hgtRatio}x smoother",
            hgtRatio >= 4.0,
        )
        // And in absolute terms: under 5 mm of residual shake at a ~5 m leash is
        // under 0.06° of angular motion, i.e. below one pixel on a 1080-tall
        // viewport with a 45° vertical FoV.
        assertTrue("residual lateral shake ${eyeLatRms}m must be under 5 mm", eyeLatRms < 0.005)
        assertTrue("residual vertical shake ${eyeHgtRms}m must be under 5 mm", eyeHgtRms < 0.005)
    }

    @Test
    fun `the smoothed heading does not breathe with the gait`() {
        val cam = FollowCamera()
        val headings = ArrayList<Double>()
        val steps = (6.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
            val s = cam.update(nanos(t), x, y, z)
            if (t >= 3.0) headings += Math.toDegrees(s.headingRad)
        }
        // A straight walk: the heading is 0° and the peak-to-peak wobble must be
        // well inside ROUND 7's own ±3°-per-step yaw, which this estimator never
        // reads in the first place (it works on translation, not phone attitude
        // — the point of deriving heading from the trajectory).
        //
        // Measured peak-to-peak: 0.062°. The 1° assertion is two decades of
        // slack, deliberately: what it is pinning is "the camera does not rotate
        // with the gait at all", and any regression that reintroduced phone yaw
        // or shortened the chord window below one gait cycle would blow through
        // it by more than an order of magnitude.
        val peakToPeak = (headings.max() - headings.min())
        assertTrue("heading wobble ${peakToPeak}° must stay under 1°", peakToPeak < 1.0)
        assertEquals("straight walk along +x is heading 0°", 0.0, headings.last(), 1.0)
    }

    // --- turning -------------------------------------------------------------

    @Test
    fun `a ninety degree turn rotates the camera heading, lagging then converging`() {
        val cam = FollowCamera()
        // 3 s along +x, then a 1 s constant-rate turn, then 4 s along +z.
        // Heading in the Y-up ground plane is atan2(dz, dx): +x is 0, +z is +90°.
        val turnStart = 3.0
        val turnEnd = 4.0
        var x = 0.0
        var z = 0.0
        var headingAtTurnPlusThird = Double.NaN
        var headingFinal = Double.NaN

        val steps = (8.0 * POSE_HZ).toInt()
        var previousT = 0.0
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val dt = t - previousT
            previousT = t
            val course = when {
                t <= turnStart -> 0.0
                t >= turnEnd -> PI / 2.0
                else -> (PI / 2.0) * (t - turnStart) / (turnEnd - turnStart)
            }
            x += WALK_MPS * dt * cos(course)
            z += WALK_MPS * dt * sin(course)
            val (sx, sy, sz) = gaitSample(t, alongX = x, alongZ = z, headingRad = course)
            val s = cam.update(nanos(t), sx, sy, sz)
            if (abs(t - (turnEnd + 0.3)) < 0.5 / POSE_HZ) headingAtTurnPlusThird = s.headingRad
            headingFinal = s.headingRad
        }

        // LAGGING: 0.3 s after the turn is complete the camera is demonstrably
        // still coming round — it must not have snapped. (0.5 s of chord window
        // plus 0.6 s of filter; anything close to 90° here would mean the
        // smoothing is not doing its job.)
        val lagErr = headingErrorDeg(headingAtTurnPlusThird, PI / 2.0)
        assertTrue(
            "0.3 s after the corner the camera should still be turning, was ${90.0 - lagErr}° in",
            lagErr > 15.0,
        )
        assertTrue("…but it must have started, was ${90.0 - lagErr}° in", lagErr < 85.0)

        // CONVERGING: 4 s down the new leg it is on the new heading.
        val finalErr = headingErrorDeg(headingFinal, PI / 2.0)
        assertTrue("heading must converge on +z; err=${finalErr}°", finalErr < 5.0)

        // And the camera is physically behind along the NEW direction: −z of the
        // rig, not −x.
        val s = cam.solution()
        assertTrue("eye must be behind in z", s.eyeZ < s.targetZ - 1.0)
        assertEquals("eye must no longer be offset in x", s.targetX, s.eyeX, 0.4)
    }

    // --- framing against RECENT geometry, not the whole cloud ------------------

    @Test
    fun `framing distance follows the recent geometry and never grows with the walk`() {
        val cam = FollowCamera()
        val distances = ArrayList<Double>()
        // Sixty seconds, sixty metres — the length at which the old FOLLOW branch
        // (distance = whole-cloud span x 1.2) had zoomed out to 70 m and stopped
        // following anything at all. The recent geometry is a constant 2 m room
        // the whole way.
        val steps = (60.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
            val s = cam.update(nanos(t), x, y, z, recentGeometryRadiusM = 2.0)
            if (t >= 5.0) distances += s.distanceM
        }
        // 2.0 / sin(22.5°) = 5.23 m, and it stays there for the whole minute.
        assertEquals("a 2 m room frames at 5.2 m", 5.23, distances.first(), 0.1)
        assertEquals("…still, 55 s and 55 m later", 5.23, distances.last(), 0.1)
        assertTrue("distance must not drift with walk length", (distances.max() - distances.min()) < 0.1)
    }

    @Test
    fun `a bigger space frames from further back, a smaller one from closer`() {
        fun settle(radius: Double?): Double {
            val cam = FollowCamera()
            var d = 0.0
            val steps = (10.0 * POSE_HZ).toInt()
            for (i in 0..steps) {
                val t = i / POSE_HZ
                val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
                d = cam.update(nanos(t), x, y, z, recentGeometryRadiusM = radius).distanceM
            }
            return d
        }
        val corridor = settle(1.2)
        val nominal = settle(null)
        val hall = settle(6.0)
        assertTrue("a 1.2 m corridor frames closer than the 2 m nominal", corridor < nominal)
        assertTrue("a 6 m hall frames further back than the 2 m nominal", hall > nominal)
        // The clamps hold: an absurd measurement cannot throw the camera across
        // the building, which is the whole difference from a whole-cloud fit.
        assertEquals("an absurd radius is clamped at maxGeometryRadiusM", settle(400.0), settle(8.0), 0.01)
        assertTrue("…and lands inside the leash limits", settle(400.0) <= 20.0)
    }

    // --- degenerate inputs ----------------------------------------------------

    @Test
    fun `an empty trail yields a finite default third-person camera`() {
        val cam = FollowCamera()
        val s = cam.solution()
        assertTrue("nothing may be NaN before the first pose", s.isFinite())
        assertFalse("and it must admit the heading is assumed", s.headingMeasured)
        assertTrue("it must still be a usable leash", s.distanceM >= 2.5)
        // Y-up default heading is −z (the direction an ARCore session starts
        // facing), so the camera parks on +z — the same side `Manipulator`'s
        // orbitHomePosition(4, 3, 8) and the old FOLLOW branch both used, so
        // switching modes before the first step does not jump the view.
        assertTrue("default eye sits on +z", s.eyeZ > s.targetZ)
        assertEquals("…and dead centre in x", s.targetX, s.eyeX, 1e-9)
    }

    @Test
    fun `a single pose is a camera, not a division by zero`() {
        val cam = FollowCamera()
        val s = cam.update(nanos(0.0), 3.0, 1.5, -2.0)
        assertTrue(s.isFinite())
        assertFalse("one pose cannot establish a heading", s.headingMeasured)
        assertEquals("but the target is that pose", 3.0, s.targetX, 1e-9)
        assertEquals(1.5, s.targetY, 1e-9)
        assertEquals(-2.0, s.targetZ, 1e-9)
    }

    @Test
    fun `a rig that stops holds its last heading instead of spinning`() {
        val cam = FollowCamera()
        // 3 s of walking along +x, then 4 s standing still — with the gait still
        // running, because a standing operator still sways: this is exactly the
        // input that makes a naive atan2-of-the-last-displacement estimator
        // produce a uniformly random angle and spin the camera on the spot.
        var s = cam.solution()
        val steps = (7.0 * POSE_HZ).toInt()
        var headingAtStop = Double.NaN
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val along = if (t <= 3.0) WALK_MPS * t else WALK_MPS * 3.0
            val (x, y, z) = gaitSample(t, alongX = along, alongZ = 0.0, headingRad = 0.0)
            s = cam.update(nanos(t), x, y, z)
            if (abs(t - 3.0) < 0.5 / POSE_HZ) headingAtStop = s.headingRad
        }
        assertTrue(s.isFinite())
        assertTrue("the heading measured while walking survives the stop", s.headingMeasured)
        assertTrue(
            "heading drifted ${headingErrorDeg(s.headingRad, headingAtStop)}° while standing still",
            headingErrorDeg(s.headingRad, headingAtStop) < 2.0,
        )
        assertTrue("and the camera stays behind the stationary rig", s.eyeX < s.targetX - 1.0)
    }

    @Test
    fun `non-finite and out-of-order samples are refused rather than poisoning the filters`() {
        val cam = FollowCamera()
        val steps = (3.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
            cam.update(nanos(t), x, y, z)
        }
        val good = cam.solution()

        // One NaN through the filters would be permanent — NaN * alpha + x is
        // NaN forever — and a NaN camera in Filament renders black with no error.
        val afterNan = cam.update(nanos(4.0), Double.NaN, 1.5, 0.0)
        assertTrue("a NaN pose must not reach the state", afterNan.isFinite())
        assertEquals(good.eyeX, afterNan.eyeX, 1e-9)
        assertTrue(cam.update(nanos(4.1), 1.0, Double.POSITIVE_INFINITY, 0.0).isFinite())
        assertTrue(cam.update(nanos(4.2), 1.0, 1.5, Double.NaN).isFinite())

        // A repeated or rewound timestamp is dropped, not clamped — same rule as
        // CaptureArController.publishPose. Feeding it would divide a chord by a
        // zero or negative interval.
        val beforeStale = cam.solution()
        val stale = cam.update(nanos(1.0), 99.0, 99.0, 99.0)
        assertEquals(beforeStale.eyeX, stale.eyeX, 1e-9)
        assertEquals(beforeStale.targetY, stale.targetY, 1e-9)

        // Still healthy afterwards.
        val resumed = cam.update(nanos(5.0), 5.0, 1.5, 0.0)
        assertTrue(resumed.isFinite())
    }

    @Test
    fun `a long stall eases rather than jump-cutting`() {
        val cam = FollowCamera()
        val steps = (3.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
            cam.update(nanos(t), x, y, z)
        }
        val before = cam.solution()
        // Sixty seconds later — the operator was on another screen, or the mode
        // was switched away and back. dt clamps to maxDtSeconds, so alpha is
        // 1 - e^(-0.5/0.5) = 0.63: a fast ease, not a teleport.
        val after = cam.update(nanos(63.0), 60.0, 1.5, 0.0)
        assertTrue(after.isFinite())
        val moved = hypot(after.targetX - before.targetX, after.targetZ - before.targetZ)
        assertTrue("the target must move toward the new pose", moved > 1.0)
        assertTrue("…but must not arrive in one frame", after.targetX < 60.0 - 5.0)
    }

    @Test
    fun `reset forgets the previous session entirely`() {
        val cam = FollowCamera()
        val steps = (4.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val (x, y, z) = gaitSample(t, alongX = WALK_MPS * t, alongZ = 0.0, headingRad = 0.0)
            cam.update(nanos(t), x, y, z)
        }
        assertTrue(cam.hasMeasuredHeading())
        cam.reset()
        assertFalse("a new session has no measured heading", cam.hasMeasuredHeading())
        val s = cam.solution()
        assertTrue(s.isFinite())
        assertEquals("and is anchored at the new frame's origin", 0.0, s.targetX, 1e-9)
        // A pose stream from the new session may legitimately start at a smaller
        // timestamp; reset must have cleared the monotonicity latch too, or the
        // first several seconds of the new walk would be silently dropped.
        val fresh = cam.update(nanos(0.0), 7.0, 1.5, 7.0)
        assertEquals(7.0, fresh.targetX, 1e-9)
    }

    // --- the frame convention -------------------------------------------------

    @Test
    fun `the up axis is a choice, not an assumption`() {
        // The same physical walk, expressed in the engine's pushbroom-test
        // convention (+z up) instead of ARCore's (+y up): along +x at 1 m/s, at a
        // constant height of 1.5 m in the UP axis.
        val cam = FollowCamera(FollowCameraConfig(upAxis = UpAxis.Z_UP))
        var s = cam.solution()
        val steps = (4.0 * POSE_HZ).toInt()
        for (i in 0..steps) {
            val t = i / POSE_HZ
            val phase = 2.0 * PI * GAIT_HZ * t
            s = cam.update(
                nanos(t),
                WALK_MPS * t,
                SWAY_M * sin(phase),   // sway is in y here — the ground plane is (x, y)
                1.5 + BOB_M * sin(phase),
            )
        }
        assertTrue(s.isFinite())
        assertEquals("up is +z", 1.0, s.upZ, 1e-9)
        assertEquals(0.0, s.upY, 1e-9)
        assertTrue("behind in x", s.eyeX < s.targetX - 1.0)
        assertTrue("above in z", s.eyeZ > s.targetZ + 1.0)
        assertEquals("and on the walk line in y", s.targetY, s.eyeY, 0.15)

        // The Y-up default must NOT have been used for the initial heading: a
        // Z-up frame has no ARCore start-facing convention to borrow.
        val fresh = FollowCamera(FollowCameraConfig(upAxis = UpAxis.Z_UP)).solution()
        assertEquals("the Z-up placeholder heading is +x, so the camera parks on −x", 0.0, fresh.headingRad, 1e-9)
        assertTrue(fresh.eyeX < fresh.targetX)
    }

    @Test
    fun `heading arithmetic is done on the circle`() {
        // The helper exists because a straight subtraction is right at 179° and
        // wrong at 181°, and every convergence assertion above depends on it.
        assertEquals(0.0, headingErrorDeg(PI, -PI), 1e-9)
        assertEquals(2.0, headingErrorDeg(Math.toRadians(179.0), Math.toRadians(-179.0)), 1e-9)
        assertEquals(90.0, headingErrorDeg(Math.toRadians(-135.0), Math.toRadians(135.0)), 1e-9)
    }

    private fun max(a: Double, b: Double): Double = if (a > b) a else b
}
