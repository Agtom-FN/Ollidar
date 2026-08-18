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
 * ROUND 15 — items 54, 56 and 57, in the half that runs on a bare JVM.
 *
 * The engine proves the geometry (`test_round15_live_heal.cpp`,
 * `test_round15_plan.cpp`). What is proved here is everything the operator
 * actually reads and everything the two sides have to agree about:
 *
 *  * the pose BRACKET a break is measured across, because the live healer and
 *    `post::stitch_sections` must pick the same pair or the live and offline
 *    corrections silently differ;
 *  * the self-check sentence, especially its NOT-MEASURABLE branch, which is
 *    the one a card would otherwise print as "0.0 cm";
 *  * the floor plan's honest modes.
 */
class Round15Test {

    private fun sample(
        tNs: Long,
        x: Double = 0.0,
        y: Double = 0.0,
        z: Double = 0.0,
        tracking: Boolean = true,
    ) = PoseSample(tNs, Vec3(x, y, z), Quat(0.0, 0.0, 0.0, 1.0), tracking)

    // --- item 54: the bracket ----------------------------------------------

    @Test
    fun `an impossible step brackets the two poses either side of it`() {
        val t = PoseSectionTracker()
        assertNull(t.addBracketed(sample(0L, x = 0.0)))
        assertNull(t.addBracketed(sample(33_000_000L, x = 0.03)))
        // 0.9 m in 33 ms is 27 m/s. Nobody walks that.
        val b = t.addBracketed(sample(66_000_000L, x = 0.93))!!
        assertEquals(PoseSectionBreak.Reason.IMPOSSIBLE_STEP, b.breakInfo.reason)
        // BEFORE is the previous pose, AFTER is the one that triggered it —
        // exactly `poses[i-1]`/`poses[i]` in section_stitch.cpp.
        assertEquals(33_000_000L, b.before.tMonoNs)
        assertEquals(66_000_000L, b.after.tMonoNs)
        assertEquals(0.03, b.before.position.x, 1e-9)
        assertEquals(0.93, b.after.position.x, 1e-9)
    }

    @Test
    fun `a tracking gap brackets the last TRACKED pose, not the last pose seen`() {
        val t = PoseSectionTracker()
        t.addBracketed(sample(0L, x = 0.0, tracking = true))
        t.addBracketed(sample(33_000_000L, x = 0.10, tracking = true))
        // The poses reported DURING a loss are the tracker's own guesses;
        // measuring against them measures nothing, which is why the anchor has
        // to be the last one that was really tracking.
        t.addBracketed(sample(66_000_000L, x = 5.0, tracking = false))
        t.addBracketed(sample(99_000_000L, x = 9.0, tracking = false))
        val b = t.addBracketed(sample(132_000_000L, x = 0.40, tracking = true))!!
        assertEquals(PoseSectionBreak.Reason.TRACKING_REGAINED, b.breakInfo.reason)
        assertEquals(33_000_000L, b.before.tMonoNs)
        assertEquals(0.10, b.before.position.x, 1e-9)
        assertEquals(132_000_000L, b.after.tMonoNs)
    }

    @Test
    fun `add and addBracketed are the same detector`() {
        val a = PoseSectionTracker()
        val b = PoseSectionTracker()
        val stream = listOf(
            sample(0L),
            sample(33_000_000L, x = 0.03),
            sample(66_000_000L, x = 0.93),
            sample(99_000_000L, x = 0.96),
        )
        for (s in stream) {
            assertEquals(a.add(s)?.reason, b.addBracketed(s)?.breakInfo?.reason)
        }
        assertEquals(a.sectionCount(), b.sectionCount())
        assertEquals(2, a.sectionCount())
    }

    // --- item 57: the sentence ---------------------------------------------

    private fun stitch(selfCheck: SelfCheck?) = StitchResult(
        ran = true,
        mapWritten = true,
        sections = 1,
        seams = 0,
        seamsRefined = 0,
        points = 1000,
        poses = 100,
        posesUntracked = 0,
        movedMeters = 0.0,
        movedDegrees = 0.0,
        verticalExtentBeforeM = 0.2,
        verticalExtentAfterM = 0.2,
        endGapBeforeM = 0.0,
        endGapAfterM = 0.0,
        mountVerdict = MountVerdict.OK,
        mountImpossibleFraction = 0.0,
        selfCheck = selfCheck,
    )

    @Test
    fun `a measurable self-check reads in centimetres and carries its own floor`() {
        val line: String = stitch(
            SelfCheck(
                measurable = true,
                offsetMeters = 0.0197,
                floorMeters = 0.0099,
                windows = 12,
                separationSeconds = 8.0,
                p90Meters = 0.05,
            ),
        ).selfCheckLine!!
        assertTrue(line, line.contains("2.0 cm"))
        assertTrue(line, line.contains("1.0 cm"))
        assertTrue(line, line.contains("8 s"))
        // Plain words: no jargon from the implementation may leak out.
        for (word in listOf("consistency", "voxel", "planarity", "separation", "median")) {
            assertFalse("jargon '$word' in: $line", line.lowercase().contains(word))
        }
        // And the ROUND 13 format-string bug's regression bar: every
        // placeholder must have been substituted.
        assertFalse(line, line.contains("%"))
    }

    @Test
    fun `an unmeasurable self-check says so instead of claiming zero`() {
        val line: String = stitch(
            SelfCheck(
                measurable = false,
                offsetMeters = 0.0,
                floorMeters = 0.0,
                windows = 1,
                separationSeconds = 0.0,
                p90Meters = 0.0,
            ),
        ).selfCheckLine!!
        assertTrue(line, line.contains("not measurable"))
        // The number that would be a lie must not appear at all.
        assertFalse(line, line.contains("0.0 cm"))
        assertFalse(line, line.contains("%"))
    }

    @Test
    fun `no self-check at all is no line at all`() {
        assertNull(stitch(null).selfCheckLine)
    }

    @Test
    fun `fromNative accepts the old 16-slot array and the new 22-slot one`() {
        val old = DoubleArray(16)
        old[0] = 1.0
        old[2] = 3.0
        val a = StitchResult.fromNative(old)!!
        assertEquals(3, a.sections)
        assertNull(a.selfCheck)

        val new = DoubleArray(22)
        new[0] = 1.0
        new[2] = 3.0
        new[16] = 1.0
        new[17] = 0.0197
        new[18] = 0.0099
        new[19] = 12.0
        new[20] = 8.0
        new[21] = 0.05
        val b = StitchResult.fromNative(new)!!
        assertEquals(3, b.sections)
        val sc = b.selfCheck!!
        assertTrue(sc.measurable)
        assertEquals(0.0197, sc.offsetMeters, 1e-9)
        assertEquals(8.0, sc.separationSeconds, 1e-9)
    }

    // --- item 56: the floor plan's honest modes -----------------------------

    private fun plan(
        mode: FloorPlanResult.Mode,
        walls: Int,
        rooms: Int,
        fromFloorMap: Boolean = false,
        paired: Int = 0,
    ) = FloorPlanResult(
        ran = true,
        mode = mode,
        wallsFromFloorMap = fromFloorMap,
        noRoomClosed = walls > 0 && rooms == 0,
        cloudPoints = 220438,
        bandPoints = 21143,
        mapPoints = 99373,
        occupiedCells = 1327,
        mapCells = 679,
        walls = walls,
        wallsPaired = paired,
        openings = 9,
        doors = 1,
        windows = 0,
        rooms = rooms,
        wallLengthMeters = 24.88,
        roomAreaM2 = if (rooms > 0) 31.5 else 0.0,
        largestRoomAreaM2 = if (rooms > 0) 31.5 else 0.0,
        extentXMeters = 14.65,
        extentYMeters = 9.80,
        pixelsPerMeter = 122.0,
        scaleBarMeters = 2.0,
        pngWidth = 1600,
        pngHeight = 1195,
        pngPath = "/x/floorplan.png",
    )

    @Test
    fun `density mode never claims a surveyed line`() {
        val r = plan(FloorPlanResult.Mode.DENSITY, walls = 0, rooms = 0)
        assertTrue(r.headline, r.headline.contains("No wall could be fitted"))
        assertTrue(r.headline, r.headline.contains("to scale"))
        val d = r.detail!!
        assertTrue(d, d.contains("nothing here is a surveyed line"))
    }

    @Test
    fun `walls traced from the floor map say their thickness is assumed`() {
        val r = plan(FloorPlanResult.Mode.WALLS, walls = 10, rooms = 0, fromFloorMap = true)
        assertTrue(r.headline, r.headline.contains("10 walls"))
        assertTrue(r.headline, r.headline.contains("no outline closed"))
        val d = r.detail!!
        assertTrue(d, d.contains("assumed, not measured"))
        // The action, because "no outline closed" is the one verdict the
        // operator can still do something about while they are in the room.
        assertTrue(d, d.contains("Walking the gaps again"))
        assertFalse(r.headline, r.headline.contains("%"))
        assertFalse(d, d.contains("%"))
    }

    @Test
    fun `paired faces are reported as measured`() {
        val r = plan(FloorPlanResult.Mode.WALLS, walls = 9, rooms = 1, paired = 2)
        assertTrue(r.headline, r.headline.contains("1 room"))
        assertFalse(r.headline, r.headline.contains("1 rooms"))
        val d = r.detail!!
        assertTrue(d, d.contains("2 of 9 walls were scanned on both sides"))
        assertTrue(d, d.contains("measured rather than assumed"))
    }

    @Test
    fun `fromNative refuses a short array rather than reading past it`() {
        assertNull(FloorPlanResult.fromNative(DoubleArray(23)))
        assertNull(FloorPlanResult.fromNative(null))
        val v = DoubleArray(24)
        v[0] = 1.0
        v[1] = 1.0
        val r = FloorPlanResult.fromNative(v, arrayOf("/a.png", "", "", "streams/map.bin"))!!
        assertEquals(FloorPlanResult.Mode.DENSITY, r.mode)
        assertEquals("/a.png", r.pngPath)
        assertEquals("", r.pdfPath)
        assertFalse(r.hasDrawings)
        assertTrue(r.hasImage)
    }

    // --- item 54: what the capture panel says --------------------------------

    @Test
    fun `a fully healed capture is not told its scan is in pieces`() {
        val healed = sectionHint(sectionCount = 3, unhealedBreaks = 0)!!
        assertTrue(healed, healed.contains("corrected as it happened"))
        assertFalse(
            "a healed break must not be described as the scan being in pieces",
            healed.contains("sections"),
        )
        // ...and one that could not be healed keeps the ROUND 7 sentence,
        // because that one the operator can actually act on.
        val unhealed = sectionHint(sectionCount = 3, unhealedBreaks = 1)!!
        assertTrue(unhealed, unhealed.contains("now in 3 sections"))
        assertTrue(unhealed, unhealed.contains("walking the seam again"))
        assertNull(sectionHint(sectionCount = 1, unhealedBreaks = 0))
    }

    @Test
    fun `the default argument keeps every old caller on the old sentence`() {
        // PoseSectionsTest and every existing call site pass one argument.
        assertEquals(sectionHint(3, 2), sectionHint(3))
    }
}
