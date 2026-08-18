package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 12 — the tracking-stable start gate, the loop-return measurement, and
 * the grade that stopped over-claiming.
 *
 * All three come out of the same field session. The owner walked at normal pace
 * on 0.7.0 and reported "quality not so good, still shift"; measuring the two
 * captures he exported produced:
 *
 *  * `scan-025` took a **2.015 m step in 33 ms, 6.9 s after Start**, and
 *    `scan-028` a 0.30 m one 3.7 s in — with `tracking` reading TRACKING and
 *    pose quality GOOD across both. Hence [TrackingWarmup].
 *  * both captures are loops that end **0.52 m** and **0.80 m** from where they
 *    began after ~15 m of walking. Hence [LoopReturnTracker].
 *  * `scan-026` — the 0.52 m one, whose map disagrees with itself by 5.26 cm at
 *    8 s separation — was graded **GOOD SCAN** by 0.7.0's summary card, whose
 *    every input is a count. Hence the grade change.
 */
class Round12StartGateAndGradeTest {

    private fun pose(tMillis: Long, x: Double, tracking: Boolean = true) = PoseSample(
        tMonoNs = tMillis * 1_000_000L,
        position = Vec3(x, 0.0, 0.0),
        orientation = Quat.IDENTITY,
        tracking = tracking,
    )

    /** 30 Hz of poses walking at `mps`, `millis` long, starting at `startMs`. */
    private fun walk(millis: Long, mps: Double, startMs: Long = 0L, tracking: Boolean = true):
        MutableList<PoseSample> {
        val out = ArrayList<PoseSample>()
        var t = startMs
        while (t <= startMs + millis) {
            out.add(pose(t, mps * (t - startMs) / 1000.0, tracking))
            t += 33L
        }
        return out
    }

    // ── the start gate ──────────────────────────────────────────────────────

    @Test
    fun `a settled tracker clears the gate`() {
        val v = TrackingWarmup().evaluate(walk(3_000, 0.5))
        assertTrue("3 s of clean 0.5 m/s walking must be ready: ${v.logSuffix}", v.ready)
        assertNull(v.blocker)
        assertTrue(v.stableMillis >= TrackingWarmup.REQUIRED_STABLE_MILLIS)
    }

    @Test
    fun `a two-metre step in one frame blocks the gate — the scan-025 case`() {
        val samples = walk(3_000, 0.5)
        // Exactly the owner's jump: 2.015 m in 33 ms, injected two thirds of the
        // way through an otherwise clean window.
        val at = samples.size * 2 / 3
        for (i in at until samples.size) {
            samples[i] = samples[i].copy(position = samples[i].position + Vec3(2.015, 0.0, 0.0))
        }
        val v = TrackingWarmup().evaluate(samples)
        assertFalse("the jump must block Start: ${v.logSuffix}", v.ready)
        assertEquals(TrackingWarmup.Blocker.IMPOSSIBLE_STEP, v.blocker)

        // ... and it must UNBLOCK once enough clean time has passed after it,
        // or the gate would be a trap and the operator could never start. 2.5 s
        // of clean walking appended CONTINUOUSLY from where the jumped section
        // left off.
        val recovered = ArrayList(samples)
        val lastMs = samples.last().tMonoNs / 1_000_000L
        val lastX = samples.last().position.x
        var t = lastMs + 33L
        while (t <= lastMs + 2_500L) {
            recovered.add(pose(t, lastX + 0.5 * (t - lastMs) / 1000.0))
            t += 33L
        }
        val v2 = TrackingWarmup().evaluate(recovered)
        assertTrue("the gate must clear once the tracker settles: ${v2.logSuffix}", v2.ready)
    }

    @Test
    fun `a tracker that is not tracking blocks, and an empty stream blocks`() {
        val lost = walk(3_000, 0.5).toMutableList()
        lost[lost.size - 1] = lost.last().copy(tracking = false)
        assertEquals(TrackingWarmup.Blocker.NOT_TRACKING, TrackingWarmup().evaluate(lost).blocker)
        assertEquals(TrackingWarmup.Blocker.NO_POSES, TrackingWarmup().evaluate(emptyList()).blocker)
    }

    @Test
    fun `a clean but short window is TOO_SHORT rather than broken`() {
        val v = TrackingWarmup().evaluate(walk(600, 0.5))
        assertFalse(v.ready)
        assertEquals(TrackingWarmup.Blocker.TOO_SHORT, v.blocker)
        // The distinction matters for what the operator is told: "settling" is
        // not the same message as "point at a textured surface".
        assertTrue(v.label.contains("settle"))
    }

    // ── the loop return ─────────────────────────────────────────────────────

    @Test
    fun `a short walk is not a loop and claims nothing`() {
        val t = LoopReturnTracker()
        var path = 0f
        for (i in 0..40) {
            t.add(i * 0.1f, 0f, path)
            path += 0.1f
        }
        assertFalse(t.isLoop)
        assertNull(t.endGapMeters)
        assertNull(t.closestApproachMeters)
    }

    @Test
    fun `a walk that comes back reports the gap it comes back with`() {
        // Out 6 m and back, 12 m of path — past the 8 m loop floor — finishing
        // 0.52 m short, which is scan-026's measured number.
        val t = LoopReturnTracker()
        var path = 0f
        var x = 0f
        while (x < 6f) {
            t.add(x, 0f, path); x += 0.15f; path += 0.15f
        }
        while (x > 0.52f) {
            t.add(x, 0f, path); x -= 0.15f; path += 0.15f
        }
        t.add(0.52f, 0f, path)
        assertTrue(t.isLoop)
        val gap = t.endGapMeters
        assertNotNull(gap)
        assertEquals(0.52, gap!!.toDouble(), 0.02)
        // The closest approach is the same here because the walk is monotone
        // back toward the start.
        assertEquals(0.52, t.closestApproachMeters!!.toDouble(), 0.02)
    }

    // ── the grade ───────────────────────────────────────────────────────────

    /** scan-026's own numbers, from the owner's seal log. */
    private fun scan026(trimAccuracy: Double? = null, loopGap: Double? = null) = ScanSummary(
        pointsCaptured = 126_554,
        elapsedMillis = 61_195,
        pathLengthMeters = 14.5,
        sections = 1,
        trackingDrops = 0,
        recordingSizeBytes = 4_031_566,
        mountTrimAccuracyDeg = trimAccuracy,
        loopEndGapMeters = loopGap,
    )

    @Test
    fun `the GOOD sentence no longer claims the room lines up`() {
        val s = scan026()
        assertEquals(ScanGrade.GOOD, s.grade)
        // This is the assertion that would have stopped 0.7.0 telling the owner
        // scan-026 was a good scan without qualification.
        assertTrue(
            "the GOOD reason must say alignment was not measured: ${s.gradeReason}",
            s.gradeReason.contains("not measured"),
        )
    }

    @Test
    fun `a trim that never converged caps the grade at FAIR`() {
        assertEquals(ScanGrade.GOOD, scan026(trimAccuracy = 0.4).grade)
        val poor = scan026(trimAccuracy = 1.6)
        assertEquals(ScanGrade.FAIR, poor.grade)
        assertTrue(poor.gradeReason.contains("Mount reference"))
        // Unknown accuracy is not a downgrade — every trim taken before 0.7.1
        // has none, and punishing them would grade the app's own history.
        assertEquals(ScanGrade.GOOD, scan026(trimAccuracy = null).grade)
    }

    @Test
    fun `the loop-return line is conditional and never part of the grade`() {
        val drifted = scan026(loopGap = 0.52)
        // Reported...
        val note = drifted.loopReturnNote
        assertNotNull(note)
        assertTrue("must state the condition: $note", note!!.contains("If you finished"))
        assertTrue(note.contains("52 cm"))
        // ... and NOT graded: same grade as the identical scan without it.
        assertEquals(scan026().grade, drifted.grade)

        // A walk that really closed says so quietly and does not warn.
        val closed = scan026(loopGap = 0.08)
        assertTrue(closed.loopReturnNote!!.contains("tracker held"))

        // A walk that was never a loop says nothing at all.
        assertNull(scan026().loopReturnNote)
    }
}
