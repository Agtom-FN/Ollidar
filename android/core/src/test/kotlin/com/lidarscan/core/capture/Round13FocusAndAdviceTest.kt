package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 13.
 *
 * Three things, all of which shipped wrong in 0.7.1 or were missing:
 *  * the `.format()` precedence bug that printed placeholders at the operator;
 *  * [CaptureFocus] — the Do Not Disturb decisions (owner item 47);
 *  * [ScanSummary.nextWalkAdvice] — a count is not an instruction.
 */
class Round13FocusAndAdviceTest {

    // ── the format-precedence bug ────────────────────────────────────────────

    /**
     * `"a %f" + "b".format(x)` applies `.format` to `"b"` only, because a method
     * call binds tighter than `+`. The owner's 0.7.1 log carried the same bug in
     * the seal summary line; this one was worse, because it went to the screen.
     */
    @Test
    fun `an impossible-step break states its numbers instead of its placeholders`() {
        val s = PoseSectionBreak(
            tMonoNs = 99_208_728_400_384L,
            positionJumpM = 0.7775830950310137,
            rotationJumpDeg = 13.53233109340493,
            gapMillis = 33,
            reason = PoseSectionBreak.Reason.IMPOSSIBLE_STEP,
        ).summary
        assertFalse("the sentence still carries a literal placeholder: $s", s.contains("%"))
        assertTrue("expected the measured jump in the sentence: $s", s.contains("0.78 m"))
        assertTrue("expected the measured rotation in the sentence: $s", s.contains("13.5"))
        assertTrue(s.contains("33 ms"))
    }

    @Test
    fun `a tracking-regained break also states its numbers`() {
        val s = PoseSectionBreak(
            tMonoNs = 1L,
            positionJumpM = 1.05,
            rotationJumpDeg = 0.54,
            gapMillis = 33,
            reason = PoseSectionBreak.Reason.TRACKING_REGAINED,
        ).summary
        assertFalse(s.contains("%"))
        assertTrue(s.contains("1.05 m"))
    }

    // ── Do Not Disturb (owner item 47) ───────────────────────────────────────

    @Test
    fun `engage only when the phone is not already quiet`() {
        assertTrue(CaptureFocus.shouldEngage(CaptureFocus.FILTER_ALL))
        // Every one of these is the user's own choice, and taking it over would
        // mean RESTORING a weaker filter at stop — i.e. this feature turning
        // somebody's Do Not Disturb off.
        assertFalse(CaptureFocus.shouldEngage(CaptureFocus.DESIRED_FILTER))
        assertFalse(CaptureFocus.shouldEngage(CaptureFocus.FILTER_NONE))
        assertFalse(CaptureFocus.shouldEngage(CaptureFocus.FILTER_ALARMS))
    }

    @Test
    fun `restore puts back exactly what was there, and nothing else`() {
        // The ordinary case: we set PRIORITY over ALL, so ALL goes back.
        assertEquals(
            CaptureFocus.FILTER_ALL,
            CaptureFocus.filterToRestore(CaptureFocus.FILTER_ALL, CaptureFocus.DESIRED_FILTER),
        )
        // Never engaged: nothing to restore.
        assertNull(CaptureFocus.filterToRestore(null, CaptureFocus.DESIRED_FILTER))
        // Somebody moved the filter mid-capture — a bedtime rule, the user, a
        // work profile. That is a NEWER decision than ours and it wins.
        assertNull(CaptureFocus.filterToRestore(CaptureFocus.FILTER_ALL, CaptureFocus.FILTER_NONE))
        assertNull(CaptureFocus.filterToRestore(CaptureFocus.FILTER_ALL, CaptureFocus.FILTER_ALL))
        // Degenerate: previous was already what we set, so there is nothing to do.
        assertNull(
            CaptureFocus.filterToRestore(CaptureFocus.DESIRED_FILTER, CaptureFocus.DESIRED_FILTER),
        )
    }

    @Test
    fun `every DND state has a stable log token and only the bad ones speak up`() {
        val tokens = DndState.values().map { CaptureFocus.logToken(it) }
        assertEquals("log tokens must be unique", tokens.size, tokens.toSet().size)
        tokens.forEach { assertFalse("a log token must not contain spaces: $it", it.contains(" ")) }
        // A working feature says nothing; the operator only hears about it when
        // the walk is NOT protected.
        assertNull(CaptureFocus.note(DndState.PROTECTED))
        assertNull(CaptureFocus.note(DndState.ALREADY_QUIET))
        assertNull(CaptureFocus.note(DndState.DISABLED))
        assertNotNull(CaptureFocus.note(DndState.NO_PERMISSION))
        assertNotNull(CaptureFocus.note(DndState.FAILED))
    }

    // ── the summary says what to do next (owner item 49 / ROUND 13) ──────────

    private fun summary(sections: Int, drops: Int = 0, points: Long = 100_000L) = ScanSummary(
        pointsCaptured = points,
        elapsedMillis = 41_000L,
        pathLengthMeters = 18.95,
        sections = sections,
        trackingDrops = drops,
        recordingSizeBytes = 2_558_126L,
    )

    @Test
    fun `scan-030's five sections produce an instruction, not just a count`() {
        val s = summary(sections = 5)
        assertEquals(ScanGrade.POOR, s.grade)
        assertEquals(4, s.breaks)
        val advice = s.nextWalkAdvice
        assertNotNull("five sections must tell the operator what to change", advice)
        assertTrue(advice!!.contains("camera"))
        // The reason must name what actually happens — re-anchoring — rather
        // than "tracking restarted", which is not what the gyro says occurred.
        assertTrue(s.gradeReason.contains("re-anchored"))
        assertFalse(s.gradeReason.contains("%"))
    }

    @Test
    fun `scan-029's two sections say the pieces can be put back together`() {
        val s = summary(sections = 2)
        assertEquals(ScanGrade.FAIR, s.grade)
        assertEquals(1, s.breaks)
        assertTrue(s.gradeReason.contains("Process"))
        // Singular, because there was one break. A summary that says "1 times"
        // reads like a machine wrote it.
        assertTrue(s.gradeReason.contains("1 time and"))
        assertNotNull(s.nextWalkAdvice)
    }

    @Test
    fun `a clean scan is given nothing to fix`() {
        val s = summary(sections = 1)
        assertEquals(ScanGrade.GOOD, s.grade)
        assertEquals(0, s.breaks)
        assertNull("a good scan must not invent advice", s.nextWalkAdvice)
    }

    @Test
    fun `an empty scan gets the cable message and no next-walk lecture`() {
        val s = summary(sections = 1, points = 0L)
        assertEquals(ScanGrade.POOR, s.grade)
        assertNull(s.nextWalkAdvice)
        assertTrue(s.gradeReason.contains("cable"))
    }
}
