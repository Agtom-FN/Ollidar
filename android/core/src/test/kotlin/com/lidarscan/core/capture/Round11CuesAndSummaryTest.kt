package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 11 items 43 + 44 and the point-counter fix — the three pieces of `:core`
 * logic this round added, on a bare JVM.
 *
 * Everything here is timing and arithmetic, which is exactly what a device test
 * cannot hold still and a unit test can.
 */
class Round11CuesAndSummaryTest {

    // ── item 43: the cue scheduler ──────────────────────────────────────────

    @Test
    fun `the first tick is silent, so a capture does not buzz before it starts`() {
        val s = CueScheduler()
        // Tracking is already bad at t = 0 (ARCore is still initializing, which
        // is the normal state at Start). Buzzing at the operator before they
        // have taken a step is how a default-ON feature gets turned off.
        assertNull(s.tick(CueConditions(trackingDegraded = true), nowMillis = 0L))
        assertEquals(CueKind.TRACKING_DEGRADED, s.tick(CueConditions(trackingDegraded = true), 100L))
    }

    @Test
    fun `a section break fires on the delta and never on the level`() {
        val s = CueScheduler()
        s.tick(CueConditions(sectionBreaks = 0), 0L)
        assertEquals(CueKind.SECTION_BREAK, s.tick(CueConditions(sectionBreaks = 1), 1_000L))
        // Same count, much later: an event that already fired must not fire
        // again just because its debounce expired.
        assertNull(s.tick(CueConditions(sectionBreaks = 1), 60_000L))
        assertEquals(CueKind.SECTION_BREAK, s.tick(CueConditions(sectionBreaks = 2), 61_000L))
    }

    @Test
    fun `tracking degraded repeats on its own interval and not faster`() {
        val s = CueScheduler()
        val bad = CueConditions(trackingDegraded = true)
        s.tick(bad, 0L)
        assertEquals(CueKind.TRACKING_DEGRADED, s.tick(bad, 100L))
        // Every 500 ms tick for the next four seconds must stay quiet.
        var t = 600L
        while (t < 4_100L) {
            assertNull("fired again at $t ms", s.tick(bad, t))
            t += 500L
        }
        assertEquals(CueKind.TRACKING_DEGRADED, s.tick(bad, 4_200L))
    }

    @Test
    fun `a losing cue keeps its debounce and fires on the next tick`() {
        val s = CueScheduler()
        s.tick(CueConditions(), 0L)
        // Both due in the same tick: the section break wins because two haptic
        // patterns at once are one unrecognizable pattern.
        val both = CueConditions(trackingDegraded = true, sectionBreaks = 1)
        assertEquals(CueKind.SECTION_BREAK, s.tick(both, 1_000L))
        // ...and the one that lost is still due immediately, not four seconds
        // later — it never consumed its debounce.
        assertEquals(
            CueKind.TRACKING_DEGRADED,
            s.tick(CueConditions(trackingDegraded = true, sectionBreaks = 1), 1_100L),
        )
    }

    @Test
    fun `disabled cues advance the state so switching them on does not fire a backlog`() {
        val s = CueScheduler()
        s.tick(CueConditions(), 0L)
        // Three section breaks happen while cues are off.
        assertNull(s.tick(CueConditions(sectionBreaks = 1), 1_000L, enabled = false))
        assertNull(s.tick(CueConditions(sectionBreaks = 2), 2_000L, enabled = false))
        assertNull(s.tick(CueConditions(sectionBreaks = 3), 3_000L, enabled = false))
        // Switching cues on must not replay them.
        assertNull(s.tick(CueConditions(sectionBreaks = 3), 4_000L, enabled = true))
        assertEquals(CueKind.SECTION_BREAK, s.tick(CueConditions(sectionBreaks = 4), 5_000L))
    }

    @Test
    fun `reset makes a scheduler behave like a new one`() {
        val s = CueScheduler()
        s.tick(CueConditions(sectionBreaks = 5), 0L)
        s.tick(CueConditions(sectionBreaks = 6), 1_000L)
        s.reset()
        // Capture #2 opens with a section count of 1 (i.e. 0 breaks); a
        // scheduler carrying capture #1's baseline of 6 would see that as a
        // decrease and, worse, would fire on the first increase from it.
        assertNull(s.tick(CueConditions(sectionBreaks = 0), 2_000L))
        assertEquals(CueKind.SECTION_BREAK, s.tick(CueConditions(sectionBreaks = 1), 3_000L))
    }

    @Test
    fun `the three patterns differ in buzz count, which is what survives a pocket`() {
        val counts = listOf(CueKind.TOO_FAST, CueKind.TRACKING_DEGRADED, CueKind.SECTION_BREAK)
            .map { CuePatterns.of(it).pattern.size / 2 }
        assertEquals(listOf(1, 2, 3), counts)
        for (kind in CueKind.entries) {
            val p = CuePatterns.of(kind)
            assertEquals(kind, p.kind)
            // VibrationEffect.createWaveform needs amplitudes the same length
            // as the timings, or it throws on the phone and nowhere else.
            assertEquals(p.pattern.size, p.amplitudes.size)
            assertTrue(p.toneRepeats in 1..4)
        }
    }

    // ── item 44: the scan summary and its grade ─────────────────────────────

    private fun summary(
        points: Long = 30_000L,
        millis: Long = 30_000L,
        path: Double = 20.0,
        sections: Int = 1,
        drops: Int = 0,
    ) = ScanSummary(points, millis, path, sections, drops, recordingSizeBytes = 1_000_000L)

    @Test
    fun `a clean walk at a sane pace grades GOOD`() {
        // 30,000 points over 20 m is 1,500 per metre — the owner's rig at
        // 1,453 points/s walked at about 1 m/s.
        val s = summary()
        assertEquals(ScanGrade.GOOD, s.grade)
        assertEquals(1500.0, s.pointsPerMeter, 1.0)
        assertTrue(s.gradeReason.contains("no tracking drops"))
    }

    @Test
    fun `zero points is POOR whatever else is true`() {
        val s = summary(points = 0L, sections = 1, drops = 0)
        assertEquals(ScanGrade.POOR, s.grade)
        assertTrue(s.gradeReason.contains("No points"))
    }

    @Test
    fun `walking too fast is the density gate, and it names the number`() {
        // 20,000 points over 40 m = 500 per metre: under GOOD's 800 floor and
        // over FAIR's 400 one.
        val fair = summary(points = 20_000L, path = 40.0)
        assertEquals(ScanGrade.FAIR, fair.grade)
        assertTrue(fair.gradeReason.contains("500 points per metre"))

        // 10,000 over 40 m = 250 per metre.
        val poor = summary(points = 10_000L, path = 40.0)
        assertEquals(ScanGrade.POOR, poor.grade)
        assertTrue(poor.gradeReason.contains("Rescan"))
    }

    @Test
    fun `sections and tracking drops degrade the grade before density is consulted`() {
        assertEquals(ScanGrade.FAIR, summary(sections = 2).grade)
        assertEquals(ScanGrade.POOR, summary(sections = 4).grade)
        assertEquals(ScanGrade.FAIR, summary(drops = 1).grade)
        assertEquals(ScanGrade.POOR, summary(drops = 4).grade)
        // The reason always names the worst thing, in the same order the grade
        // decided — so the word and the sentence can never disagree.
        assertTrue(summary(sections = 4, drops = 9).gradeReason.contains("sections"))
    }

    @Test
    fun `a stationary sweep is not punished for having no path`() {
        // A tripod scan walks nowhere. Dividing by zero would make the density
        // infinite (harmless) or NaN (a grade nobody can predict); the floor
        // makes it large and finite, so the grade falls to sections and drops.
        val s = summary(points = 50_000L, path = 0.0)
        assertTrue(s.pointsPerMeter.isFinite())
        assertEquals(ScanGrade.GOOD, s.grade)
    }

    @Test
    fun `average speed and rate are derived, not stored`() {
        val s = summary(points = 30_000L, millis = 30_000L, path = 30.0)
        assertEquals(1000.0, s.pointsPerSecond, 0.001)
        assertEquals(1.0, s.averageSpeedMps, 0.001)
        // Nothing divides by zero on a session that never ticked.
        val empty = summary(points = 0L, millis = 0L, path = 0.0)
        assertEquals(0.0, empty.pointsPerSecond, 0.0)
        assertEquals(0.0, empty.averageSpeedMps, 0.0)
    }

    // ── the doubled point count ─────────────────────────────────────────────

    @Test
    fun `the tally reports the resolved map and not the sum of both streams`() {
        // The owner's scan-020, in miniature: the same returns arrive twice,
        // once as the raw sensor-frame preview and once as the resolved map.
        // The old counter added them and reported 584,315 for a capture that
        // holds 293,166 points.
        val t = PointCountTally()
        t.add(PointStreamRole.RAW_SENSOR, 293_524)
        t.add(PointStreamRole.RESOLVED_MAP, 293_166)
        assertEquals(293_166L, t.points)
        assertEquals(293_524L, t.rawPoints)
        // ...and both halves are in the log line, so the next field report can
        // be read without anyone re-deriving which number was which.
        assertTrue(t.logSuffix().contains("map=293166"))
        assertTrue(t.logSuffix().contains("raw=293524"))
    }

    @Test
    fun `before any mapped point arrives the raw count is what there is`() {
        val t = PointCountTally()
        t.add(PointStreamRole.RAW_SENSOR, 1_000)
        assertEquals(1_000L, t.points)
        // The pushbroom's first batch closes at 100 ms of point time (ROUND 10),
        // so this switch-over happens once, immediately, and never again.
        t.add(PointStreamRole.RESOLVED_MAP, 40)
        assertEquals(40L, t.points)
        t.add(PointStreamRole.RESOLVED_MAP, 60)
        assertEquals(100L, t.points)
    }

    @Test
    fun `other streams are counted separately and never reported as points`() {
        val t = PointCountTally()
        t.add(PointStreamRole.RESOLVED_MAP, 500)
        t.add(PointStreamRole.OTHER, 9_999)
        assertEquals(500L, t.points)
        assertEquals(9_999L, t.otherPoints)
        t.reset()
        assertEquals(0L, t.points)
        assertNotNull(t.logSuffix())
    }
}
