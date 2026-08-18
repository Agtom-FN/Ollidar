package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 16 items 58 and 59, on the `:core` side — the two things a bare JVM can
 * pin about the owner's headline regression and his headline request.
 *
 * Item 58's mechanism (the ARCore session race) lives in `:app` and cannot be
 * unit-tested without an ARCore device; what CAN be tested here, and what
 * shipped wrong in 0.9.0, is the VERDICT: a capture with no poses was graded
 * **FAIR** and offered to the operator as a usable scan.
 */
class Round16PoseLossAndPathTest {

    // ── item 58(c): the seal has to be honest ────────────────────────────────

    /** scan-039, to the number, as the app actually sealed it. */
    private fun scan039(poses: Long?) = ScanSummary(
        pointsCaptured = 184_454L,
        elapsedMillis = 51_551L,
        pathLengthMeters = 0.0,
        sections = 1,
        trackingDrops = 0,
        recordingSizeBytes = 1_753_048L,
        mountTrimAccuracyDeg = 1.68,
        loopEndGapMeters = null,
        posesRecorded = poses,
    )

    @Test
    fun `scan-039 graded FAIR before this round and is POOR now`() {
        // What 0.9.0 did: no pose count, so every other measure applied — 51 s
        // and a 0 m path made it "from the spot", 3,578 points per second beat
        // every density floor, one section, no drops, and the only thing that
        // pulled it off GOOD was a 1.68 deg mount trim.
        val asShipped = scan039(poses = null)
        assertEquals(ScanGrade.FAIR, asShipped.grade)
        assertFalse(asShipped.isTwoDimensionalOnly)

        // What it is.
        val honest = scan039(poses = 0L)
        assertTrue(honest.isTwoDimensionalOnly)
        assertEquals(ScanGrade.POOR, honest.grade)
        assertEquals("2D ONLY", honest.headline)
        assertTrue(honest.gradeReason.startsWith("2D ONLY"))
        // The sentence has to say the file has no room in it — "rescan" alone
        // would send the operator to walk it again the same way.
        assertTrue(honest.gradeReason.contains("no room in this file"))
    }

    @Test
    fun `the 2D verdict outranks every other reason`() {
        // Even a capture that ALSO broke into five sections and dropped
        // tracking four times reports the 2D fault, because that is the one
        // that makes the other two unmeasurable.
        val s = scan039(poses = 0L).copy(sections = 5, trackingDrops = 4)
        assertEquals(ScanGrade.POOR, s.grade)
        assertTrue(s.gradeReason.startsWith("2D ONLY"))
    }

    @Test
    fun `a scan with no points is still the no-data case, not the 2D case`() {
        // Zero points AND zero poses is a sensor that never spoke, and its
        // instruction is about the cable. Stacking a second diagnosis on it
        // would send the operator to fix the wrong thing.
        val s = scan039(poses = 0L).copy(pointsCaptured = 0L)
        assertFalse(s.isTwoDimensionalOnly)
        assertTrue(s.gradeReason.contains("D6 cable"))
    }

    @Test
    fun `a normal capture is untouched by the new field`() {
        // scan-036, as sealed: the field is additive and must not move a grade
        // that was already right.
        val before = ScanSummary(
            pointsCaptured = 84_282L,
            elapsedMillis = 45_437L,
            pathLengthMeters = 10.5,
            sections = 2,
            trackingDrops = 1,
            recordingSizeBytes = 1_000L,
            mountTrimAccuracyDeg = 1.68,
            loopEndGapMeters = 0.65,
        )
        val after = before.copy(posesRecorded = 1_360L)
        assertEquals(before.grade, after.grade)
        assertEquals(before.gradeReason, after.gradeReason)
        assertFalse(after.isTwoDimensionalOnly)
        assertEquals("FAIR", after.headline)
    }

    @Test
    fun `next-walk advice for a 2D scan is about tracking, not about speed`() {
        val advice = scan039(poses = 0L).nextWalkAdvice
        assertNotNull(advice)
        assertTrue(advice!!.contains("Tracking steady"))
    }

    // ── item 59: the walked path ─────────────────────────────────────────────

    @Test
    fun `the trail carries height now and the bird's-eye tile still ignores it`() {
        val trail = TrajectoryTrail(minSpacingM = 0.15f, capacity = 100)
        assertTrue(trail.add(0f, 0f, tracking = true, y = 1.50f))
        assertTrue(trail.add(1f, 0f, tracking = true, y = 1.80f))
        assertTrue(trail.add(2f, 0f, tracking = true, y = 1.20f))
        val pts = trail.snapshot()
        assertEquals(listOf(1.50f, 1.80f, 1.20f), pts.map { it.y })
        // ...and the ground path is 2 m, not the 2.06 m a 3D length would give:
        // a bird's-eye tile measures ground distance, and a hand rising and
        // falling must not stretch its scale bar.
        assertEquals(2.0f, trail.pathLengthM(), 1e-4f)
    }

    @Test
    fun `height never gates the spacing rule`() {
        // A stationary operator raising and lowering the phone by half a metre
        // must not fill the ring: the 15 cm rule is a GROUND rule.
        val trail = TrajectoryTrail(minSpacingM = 0.15f, capacity = 100)
        assertTrue(trail.add(0f, 0f, tracking = true, y = 1.0f))
        assertFalse(trail.add(0f, 0f, tracking = true, y = 1.5f))
        assertFalse(trail.add(0f, 0f, tracking = true, y = 0.5f))
        assertEquals(1, trail.size)
    }

    @Test
    fun `the ribbon marks both ends and both are distinguishable`() {
        val trail = TrajectoryTrail(minSpacingM = 0.15f, capacity = 200)
        for (i in 0 until 40) trail.add(i * 0.2f, 0f, tracking = true, y = 1.4f)
        val ribbon = TrajectoryRibbon.fromTrail(trail.snapshot())
        assertEquals(40, ribbon.count)
        assertEquals(TrajectoryRibbon.START_MARKER, ribbon.rgba.first())
        assertEquals(TrajectoryRibbon.END_MARKER, ribbon.rgba.last())
        // ...and the two markers are not the same colour, which is the entire
        // point of having two.
        assertTrue(TrajectoryRibbon.START_MARKER != TrajectoryRibbon.END_MARKER)
        // The middle is a gradient: sampled at a third and two thirds it has
        // moved, monotonically, from teal toward ember.
        val a = ribbon.rgba[ribbon.count / 3]
        val b = ribbon.rgba[2 * ribbon.count / 3]
        assertTrue(a != b)
        fun red(c: Int) = c and 0xFF
        assertTrue(red(b) > red(a))
    }

    @Test
    fun `a lost-tracking stretch outranks the markers`() {
        val trail = TrajectoryTrail(minSpacingM = 0.15f, capacity = 200)
        // The very first point is untracked — the case where a start marker
        // would otherwise hide the one colour that carries a warning.
        trail.add(0f, 0f, tracking = false, y = 1.4f)
        for (i in 1 until 20) trail.add(i * 0.2f, 0f, tracking = true, y = 1.4f)
        val ribbon = TrajectoryRibbon.fromTrail(trail.snapshot())
        assertEquals(TrajectoryRibbon.UNTRACKED, ribbon.rgba.first())
    }

    @Test
    fun `a walk shorter than a line draws nothing`() {
        assertEquals(TrajectoryRibbon.EMPTY, TrajectoryRibbon.fromTrail(emptyList()))
        assertEquals(
            TrajectoryRibbon.EMPTY,
            TrajectoryRibbon.fromTrail(listOf(TrajectoryTrail.Point(0f, 0f, true, 1f))),
        )
    }

    @Test
    fun `a sealed pose stream is thinned by distance and keeps its last pose`() {
        // 3,000 poses along 30 m — ARCore's 30 Hz over a 100 s walk. At the
        // 12 cm stride that is ~250 vertices, not 3,000.
        val n = 3_000
        val xyz = FloatArray(n * 3)
        for (i in 0 until n) {
            xyz[i * 3] = i * 0.01f
            xyz[i * 3 + 1] = 1.4f
            xyz[i * 3 + 2] = 0f
        }
        val ribbon = TrajectoryRibbon.fromPoses(xyz, n)
        assertTrue("thinned to ${ribbon.count}", ribbon.count in 200..300)
        // The END of the walk is the end of the ribbon, exactly — the end
        // marker has to sit where the operator actually stopped, because the
        // gap between it and the start marker IS the loop-end gap the card
        // reports.
        assertEquals(xyz[(n - 1) * 3], ribbon.xyz[(ribbon.count - 1) * 3])
        assertEquals(xyz[0], ribbon.xyz[0])
    }

    @Test
    fun `thinning keeps corners`() {
        // An L: 5 m east, then 5 m north. A stride that dropped the corner
        // would draw a diagonal through the wall.
        val pts = ArrayList<Float>()
        var x = 0f
        while (x <= 5f) { pts.addAll(listOf(x, 1.4f, 0f)); x += 0.01f }
        var z = 0f
        while (z <= 5f) { pts.addAll(listOf(5f, 1.4f, z)); z += 0.01f }
        val arr = pts.toFloatArray()
        val ribbon = TrajectoryRibbon.fromPoses(arr, arr.size / 3)
        // Some vertex must sit within a stride of the corner (5, 1.4, 0).
        val nearCorner = (0 until ribbon.count).any { i ->
            val dx = ribbon.xyz[i * 3] - 5f
            val dz = ribbon.xyz[i * 3 + 2] - 0f
            dx * dx + dz * dz < 0.02f
        }
        assertTrue(nearCorner)
    }

    @Test
    fun `the packing is GL order, red in the low byte`() {
        val c = TrajectoryRibbon.pack(0x11, 0x22, 0x33, 0x44)
        assertEquals(0x11, c and 0xFF)
        assertEquals(0x22, (c ushr 8) and 0xFF)
        assertEquals(0x33, (c ushr 16) and 0xFF)
        assertEquals(0x44, (c ushr 24) and 0xFF)
    }

    @Test
    fun `the same walk always builds the same ribbon`() {
        val trail = TrajectoryTrail(minSpacingM = 0.15f, capacity = 200)
        for (i in 0 until 30) trail.add(i * 0.3f, i * 0.1f, tracking = i % 7 != 0, y = 1.4f)
        val pts = trail.snapshot()
        assertEquals(TrajectoryRibbon.fromTrail(pts), TrajectoryRibbon.fromTrail(pts))
    }
}
