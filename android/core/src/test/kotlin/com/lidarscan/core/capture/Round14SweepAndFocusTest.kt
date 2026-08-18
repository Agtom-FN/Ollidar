package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import kotlin.math.cos
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 14 — items 50 and 52.
 *
 * The numbers in the assertions are the owner's own captures wherever one
 * exists, so a threshold cannot be moved without a test saying which real scan
 * changed verdict.
 */
class Round14SweepAndFocusTest {

    // ── ParallaxWatch (item 50) ─────────────────────────────────────────────

    /** Yaw about the world Y axis, so a turn of [deg] is exactly [deg] of rotation. */
    private fun yaw(deg: Double): Quat {
        val h = Math.toRadians(deg) / 2.0
        return Quat(0.0, sin(h), 0.0, cos(h))
    }

    /**
     * `n` poses at 30 Hz, turning [degPerSecond] and travelling
     * [metresPerSecond] in a straight line.
     */
    private fun sweep(
        n: Int,
        degPerSecond: Double,
        metresPerSecond: Double,
        tracking: Boolean = true,
        startNs: Long = 1_000_000_000L,
    ): List<PoseSample> = (0 until n).map { i ->
        val t = i / 30.0
        PoseSample(
            tMonoNs = startNs + (t * 1e9).toLong(),
            position = Vec3(metresPerSecond * t, 0.0, 0.0),
            orientation = yaw(degPerSecond * t),
            tracking = tracking,
        )
    }

    private fun endOf(s: List<PoseSample>) = s.last().tMonoNs

    @Test
    fun `standing and sweeping is starved`() {
        // scan-034's regime: ~40 deg/s of turn on ~4 cm/s of travel = 0.1 cm/deg.
        val s = sweep(n = 60, degPerSecond = 40.0, metresPerSecond = 0.04)
        val r = ParallaxWatch().measure(s, endOf(s))!!
        assertTrue("40 deg/s at 4 cm/s must trip: ${r.cmPerDegree} cm/deg", r.starved)
        assertTrue("expected well under the floor, got ${r.cmPerDegree}", r.cmPerDegree < 0.5)
    }

    @Test
    fun `walking while sweeping is not starved`() {
        // scan-033's regime: 0.26 m/s of walk (its measured median) while
        // turning at 20 deg/s, which is well above the capture's own p90 of
        // 26 deg/s being sustained for a full two seconds. 1.3 cm/deg.
        val s = sweep(n = 60, degPerSecond = 20.0, metresPerSecond = 0.26)
        val r = ParallaxWatch().measure(s, endOf(s))!!
        assertTrue("the rotation floor must be cleared, else this proves nothing", r.rotationDeg >= ParallaxWatch.MIN_ROTATION_DEG)
        assertFalse("0.26 m/s must clear the floor: ${r.cmPerDegree} cm/deg", r.starved)
        assertTrue(r.cmPerDegree > ParallaxWatch.MIN_CM_PER_DEGREE)
    }

    @Test
    fun `a hard turn taken at walking pace is the accepted false alarm`() {
        // Honesty about the threshold: spinning at 40 deg/s while walking at
        // 0.26 m/s is 0.65 cm/deg and DOES trip. That is the 5.5% of scan-033's
        // windows the sweep in ParallaxWatch's header accepts — a fast turn
        // genuinely is the moment a walking tracker is most likely to
        // re-anchor (ROUND 13 found 2 of 4 of scan-030's breaks followed a
        // 133 deg/2 s spin), so being told to keep moving through it is not
        // wrong advice. It is recorded here so nobody "fixes" it by accident.
        val s = sweep(n = 60, degPerSecond = 40.0, metresPerSecond = 0.26)
        assertTrue(ParallaxWatch().isStarved(s, endOf(s)))
    }

    @Test
    fun `a phone that is not turning is never starved however still it is`() {
        // A rig on a bench: no translation at all, and no rotation either.
        // Without rotation there is no parallax to be short of, and warning
        // here would fire through every pause in every capture.
        val s = sweep(n = 60, degPerSecond = 0.0, metresPerSecond = 0.0)
        val r = ParallaxWatch().measure(s, endOf(s))!!
        assertFalse(r.starved)
        assertTrue(r.rotationDeg < ParallaxWatch.MIN_ROTATION_DEG)
    }

    @Test
    fun `a re-anchor teleport cannot be counted as parallax`() {
        // scan-035's actual failure: 1.631 m of pose change in one 33 ms frame.
        // Folding that into the travel total would report 5 cm/deg of beautiful
        // parallax at the precise moment the tracker had none — so the step is
        // dropped, and the window still reads starved.
        val base = sweep(n = 60, degPerSecond = 40.0, metresPerSecond = 0.04).toMutableList()
        val jumped = base[30]
        base[30] = jumped.copy(position = Vec3(jumped.position.x + 1.631, 0.0, 0.0))
        val r = ParallaxWatch().measure(base, endOf(base))!!
        assertTrue("the teleport must not rescue the window: ${r.cmPerDegree} cm/deg", r.starved)
    }

    @Test
    fun `an untracked window is not a measurement`() {
        val s = sweep(n = 60, degPerSecond = 40.0, metresPerSecond = 0.04, tracking = false)
        assertNull(ParallaxWatch().measure(s, endOf(s)))
    }

    @Test
    fun `only the last two seconds count`() {
        // Ten seconds of walking followed by two of standing still and turning:
        // the watch must judge the two, not the twelve.
        val walk = sweep(n = 300, degPerSecond = 40.0, metresPerSecond = 0.26)
        val stand = sweep(
            n = 60,
            degPerSecond = 40.0,
            metresPerSecond = 0.0,
            startNs = walk.last().tMonoNs + 33_000_000L,
        )
        val all = walk + stand
        assertTrue(ParallaxWatch().isStarved(all, endOf(all)))
    }

    @Test
    fun `the cue fires last and repeats slowly`() {
        val scheduler = CueScheduler()
        val starved = CueConditions(turningWithoutMoving = true)
        scheduler.tick(starved, 0L)
        assertEquals(CueKind.PARALLAX_STARVED, scheduler.tick(starved, 1_000L))
        // Nagging is itself a shake of the tracker: nothing until the window.
        assertNull(scheduler.tick(starved, 6_000L))
        assertEquals(CueKind.PARALLAX_STARVED, scheduler.tick(starved, 14_000L))
    }

    @Test
    fun `a tracking loss outranks the parallax warning`() {
        val scheduler = CueScheduler()
        val both = CueConditions(trackingDegraded = true, turningWithoutMoving = true)
        scheduler.tick(both, 0L)
        assertEquals(CueKind.TRACKING_DEGRADED, scheduler.tick(both, 1_000L))
    }

    // ── The from-the-spot grade (item 50) ───────────────────────────────────

    private fun scan034() = ScanSummary(
        pointsCaptured = 135_702L,
        elapsedMillis = 67_825L,
        pathLengthMeters = 2.7,
        sections = 1,
        trackingDrops = 0,
        recordingSizeBytes = 4_374_852L,
        mountTrimAccuracyDeg = 1.32,
    )

    private fun scan033() = ScanSummary(
        pointsCaptured = 220_113L,
        elapsedMillis = 110_854L,
        pathLengthMeters = 26.6,
        sections = 2,
        trackingDrops = 1,
        recordingSizeBytes = 7_083_069L,
        mountTrimAccuracyDeg = 1.32,
        loopEndGapMeters = 0.45,
    )

    @Test
    fun `scan-034 is recognised as a from-the-spot scan`() {
        val s = scan034()
        assertTrue(s.isFromTheSpot)
        // The number the 0.8.0 card actually printed, and why it was nonsense.
        assertEquals(50_260.0, s.pointsPerMeter, 200.0)
        // What it is judged on instead.
        assertEquals("points per second", s.densityUnit)
        assertEquals(2_001.0, s.densityValue, 5.0)
    }

    @Test
    fun `a walking scan is untouched`() {
        val s = scan033()
        assertFalse(s.isFromTheSpot)
        assertEquals("points per metre", s.densityUnit)
        assertEquals(s.pointsPerMeter, s.densityValue, 1e-9)
    }

    @Test
    fun `a scan stopped after four seconds is not from the spot`() {
        // Short path, short duration: a scan that was abandoned, which is a
        // different fault and must keep the old density treatment.
        val s = ScanSummary(
            pointsCaptured = 8_000L,
            elapsedMillis = 4_000L,
            pathLengthMeters = 3.0,
            sections = 1,
            trackingDrops = 0,
            recordingSizeBytes = 1_000L,
        )
        assertFalse(s.isFromTheSpot)
        assertEquals("points per metre", s.densityUnit)
    }

    @Test
    fun `the from-the-spot advice is to keep walking, not to slow down`() {
        val advice = scan034().nextWalkAdvice!!
        assertTrue(advice, advice.contains("keep walking", ignoreCase = true))
        assertFalse(advice, advice.contains("slow down", ignoreCase = true))
    }

    @Test
    fun `a healthy sweep is not failed for standing still`() {
        // scan-034 has one section and no real tracking drops. Under 0.8.0 its
        // grade came from a 50,124 points-per-metre figure; it must now come
        // from a 2,001 points-per-second one, and both land above the floor.
        assertTrue(scan034().densityValue > ScanSummary.MIN_RATE_GOOD)
    }

    @Test
    fun `a dead sensor is still POOR when standing still`() {
        val s = scan034().copy(pointsCaptured = 6_000L)
        assertTrue(s.isFromTheSpot)
        assertEquals(ScanGrade.POOR, s.grade)
        assertTrue(s.gradeReason, s.gradeReason.contains("points per second"))
    }

    // ── DND ask-once (item 52) ──────────────────────────────────────────────

    @Test
    fun `the explainer is offered once, when it is needed`() {
        assertTrue(CaptureFocus.shouldAsk(enabled = true, granted = false, alreadyAsked = false))
    }

    @Test
    fun `the explainer is not offered again after it has been shown`() {
        // Declining is an answer. This is the whole reason the flag exists.
        assertFalse(CaptureFocus.shouldAsk(enabled = true, granted = false, alreadyAsked = true))
    }

    @Test
    fun `the explainer is not offered when there is nothing to grant`() {
        assertFalse(CaptureFocus.shouldAsk(enabled = true, granted = true, alreadyAsked = false))
        assertFalse(CaptureFocus.shouldAsk(enabled = false, granted = false, alreadyAsked = false))
    }

    @Test
    fun `the explainer leads with the physics and not with the permission`() {
        // The 0.8.0 Settings copy said "Needs Do Not Disturb access" and gave
        // no way to get it. This copy has to say what it costs the SCAN.
        assertTrue(CaptureFocus.ASK_BODY, CaptureFocus.ASK_BODY.contains("shakes the phone"))
        assertTrue(CaptureFocus.ASK_BODY.contains("Scans still run without this"))
        assertEquals("Open settings", CaptureFocus.ASK_CONFIRM)
    }

    @Test
    fun `the settings row states which side of the grant the operator is on`() {
        assertTrue(CaptureFocus.accessStatus(granted = true).contains("granted"))
        assertTrue(CaptureFocus.accessStatus(granted = false).contains("not granted"))
    }

    @Test
    fun `an unprotected walk still has a note to show`() {
        // 0.8.0 computed this sentence on every Start and no composable ever
        // collected it, which is why the owner saw nothing at all.
        assertNotNull(CaptureFocus.note(DndState.NO_PERMISSION))
        assertEquals("unprotected-no-permission", CaptureFocus.logToken(DndState.NO_PERMISSION))
    }
}
