package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 17 item 64 — **scan-045, and why it was graded GOOD.**
 *
 * The owner's 0.9.1 field session sealed a capture with `pathM=0.0`,
 * `points=55228`, `poses=225`, an `engine startCapture FAILED … invalid state`
 * line, no `map.bin` in the exported bundle and no `processed/` directory. The
 * card said **GOOD SCAN**.
 *
 * Every number in [scan045] is from that log. The first test is the assertion
 * that the card can never say that again; the rest pin the boundaries so the
 * new rules cannot quietly swallow a capture that was fine.
 */
class Round17HonestSealTest {

    private fun scan045(
        engineStarted: Boolean? = null,
        worldPointsResolved: Long? = null,
    ) = ScanSummary(
        pointsCaptured = 55_228L,
        elapsedMillis = 28_000L,
        pathLengthMeters = 0.0,
        sections = 1,
        trackingDrops = 0,
        recordingSizeBytes = 1_200_000L,
        posesRecorded = 225L,
        engineStarted = engineStarted,
        worldPointsResolved = worldPointsResolved,
    )

    @Test
    fun `round 16's grader really did call scan-045 a GOOD scan`() {
        // Not a regression guard — a record of the starting point, and of WHY
        // the two new fields had to exist rather than the old ones being
        // tightened. With neither of them supplied, every check ROUND 16 had
        // passes: one section, no drops, 225 poses (so not 2D-only), and a
        // zero path that makes the grader judge it on points per SECOND.
        val old = scan045()
        assertEquals(ScanGrade.GOOD, old.grade)
        assertFalse(old.isTwoDimensionalOnly)
        assertTrue("a wiped trail looks exactly like a deliberate sweep", old.isFromTheSpot)
    }

    @Test
    fun `a failed engine start can never be GOOD`() {
        val s = scan045(engineStarted = false)
        assertTrue(s.engineStartFailed)
        assertEquals(ScanGrade.POOR, s.grade)
        assertEquals("NOT RECORDED", s.headline)
        assertTrue(s.gradeReason.contains("never started"))
        assertTrue(s.nextWalkAdvice!!.contains("Press Start once"))
    }

    @Test
    fun `returns with no resolved world points is NO ROOM, whatever the pose count says`() {
        // This is the check ROUND 16 could not make. 225 poses is not zero, so
        // `isTwoDimensionalOnly` is false and stays false; what is zero is the
        // thing the file is actually made of.
        val s = scan045(engineStarted = true, worldPointsResolved = 0L)
        assertFalse(s.isTwoDimensionalOnly)
        assertTrue(s.isNoRoom)
        assertEquals(ScanGrade.POOR, s.grade)
        assertEquals("NO ROOM", s.headline)
        assertTrue(s.gradeReason.contains("none of them were placed in space"))
    }

    @Test
    fun `not measured is not the same as measured zero`() {
        // The same rule ROUND 16 wrote for `posesRecorded`, and for the same
        // reason: a Mid-360 session, a replay and every unit test reach this
        // code with nothing to report, and defaulting to 0 would fail all of
        // them for a fault none of them has.
        val s = scan045(engineStarted = null, worldPointsResolved = null)
        assertFalse(s.engineStartFailed)
        assertFalse(s.isNoRoom)
        assertEquals(ScanGrade.GOOD, s.grade)
    }

    @Test
    fun `a healthy capture is untouched by both new rules`() {
        val good = ScanSummary(
            pointsCaptured = 161_465L,
            elapsedMillis = 81_000L,
            pathLengthMeters = 12.8,
            sections = 1,
            trackingDrops = 0,
            recordingSizeBytes = 2_600_000L,
            posesRecorded = 2_430L,
            engineStarted = true,
            worldPointsResolved = 161_465L,
        )
        assertFalse(good.engineStartFailed)
        assertFalse(good.isNoRoom)
        assertEquals(ScanGrade.GOOD, good.grade)
        assertEquals("GOOD", good.headline)
    }

    @Test
    fun `a zero-point capture keeps the ROUND 7 no-data diagnosis, not the new ones`() {
        // Stacking diagnoses tells the operator to fix the wrong thing. A scan
        // with no returns at all is a cable, and it says so first.
        val empty = ScanSummary(
            pointsCaptured = 0L,
            elapsedMillis = 9_000L,
            pathLengthMeters = 0.0,
            sections = 1,
            trackingDrops = 0,
            recordingSizeBytes = 0L,
            posesRecorded = 0L,
            engineStarted = false,
            worldPointsResolved = 0L,
        )
        assertFalse("no returns is not 'the returns went nowhere'", empty.isNoRoom)
        assertEquals(ScanGrade.POOR, empty.grade)
        assertTrue(empty.gradeReason.contains("No points were recorded"))
    }
}
