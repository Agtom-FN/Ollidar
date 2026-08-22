package com.lidarscan.app.ui.projects

import com.lidarscan.app.ui.theme.scanColorScheme
import com.lidarscan.core.capture.PoseSectionBreak
import com.lidarscan.core.model.ProjectManifest
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * ROUND 28 item 162 — **the chip law and the grade mark, over the owner's own
 * fleet.**
 *
 * The review's headline number for this screen is 198: 66 project cards times
 * three chips, every one of them saying something all 66 scans had in common.
 * The fix is a predicate, so it is a predicate this test can hold still. The
 * fleet below is the shape of the owner's real library (65 COIN-D6 quick scans,
 * one Mid-360) rather than a two-element toy, because "one chip" is the whole
 * claim and it is only interesting at scale.
 */
class Round28ProjectsRowTest {

    private fun manifest(
        name: String,
        sensor: SensorType = SensorType.COIN_D6,
        profile: WorkflowProfile = WorkflowProfile.QUICK_SCAN,
        points: Long? = 46_500L,
        epsg: Int? = 32650,
        recovered: Boolean = false,
        breaks: Int = 0,
    ) = ProjectManifest(
        name = name,
        sensor = sensor,
        profile = profile,
        createdAtEpochMillis = 1_755_000_000_000L,
        appVersion = "0.9.13",
        pointCountEstimate = points,
        crsEpsg = epsg,
        recovered = recovered,
        sectionBreaks = List(breaks) {
            PoseSectionBreak(
                tMonoNs = it.toLong(),
                positionJumpM = 1.0,
                rotationJumpDeg = 9.0,
                gapMillis = 120L,
                reason = PoseSectionBreak.Reason.TRACKING_REGAINED,
            )
        },
    )

    /** 65 identical D6 quick scans and one Mid-360, as the owner's phone holds them. */
    private fun ownersFleet(): List<ProjectManifest> =
        List(65) { manifest("Scan-%03d".format(it)) } +
            manifest("Scan-079", sensor = SensorType.MID360)

    private fun chips(fleet: List<ProjectManifest>): List<RowDeviation> {
        val norms = RowNorms.ofManifests(fleet)
        return fleet.mapNotNull { norms.deviation(it) }
    }

    // ── the chip law (§C.4, finding P1a) ───────────────────────────────────

    @Test
    fun `sixty-five D6 scans and one Mid-360 draw exactly one chip`() {
        val fleet = ownersFleet()
        val drawn = chips(fleet)

        assertEquals("198 chips became one", 1, drawn.size)
        assertEquals(RowDeviation.SENSOR, drawn.single())
        // …and it is the Mid-360's row, not an arbitrary one.
        val norms = RowNorms.ofManifests(fleet)
        assertNull("a D6 in a fleet of D6s says nothing", norms.deviation(fleet.first()))
        assertEquals(RowDeviation.SENSOR, norms.deviation(fleet.last()))
    }

    @Test
    fun `a fleet that agrees about everything draws no chips at all`() {
        assertEquals(0, chips(List(66) { manifest("Scan-%03d".format(it)) }).size)
    }

    @Test
    fun `the norm moves with the data rather than being hard-coded`() {
        // Flip the fleet: 65 Mid-360s and one D6. The chip must move to the D6.
        val fleet = List(65) { manifest("Scan-%03d".format(it), sensor = SensorType.MID360) } +
            manifest("Scan-lonely-d6")
        val norms = RowNorms.ofManifests(fleet)
        assertNull(norms.deviation(fleet.first()))
        assertEquals(RowDeviation.SENSOR, norms.deviation(fleet.last()))
    }

    @Test
    fun `a tie is not a norm, so a split fleet draws nothing`() {
        // 33/33 genuinely has no mode, and drawing 66 chips because the modal
        // count tied would be the loudest possible answer to the quietest
        // possible question. See `modalValue`.
        val fleet = List(33) { manifest("d6-$it") } +
            List(33) { manifest("mid-$it", sensor = SensorType.MID360) }
        assertEquals(0, chips(fleet).size)
    }

    @Test
    fun `the one scan without a CRS is the one that says so`() {
        val fleet = List(65) { manifest("Scan-%03d".format(it)) } + manifest("Scan-nocrs", epsg = null)
        val drawn = chips(fleet)
        assertEquals(1, drawn.size)
        assertEquals(RowDeviation.GEOREF_MISSING, drawn.single())
    }

    @Test
    fun `a recovered manifest outranks every other deviation`() {
        // Its name, sensor and profile are a reconstruction (ROUND 6 item 20),
        // so "do not trust the rest of this row" is the only chip worth drawing.
        val fleet = List(65) { manifest("Scan-%03d".format(it)) } +
            manifest("Scan-recovered", sensor = SensorType.MID360, recovered = true)
        assertEquals(RowDeviation.RECOVERED, chips(fleet).single())
    }

    @Test
    fun `at most one chip is ever drawn for one row`() {
        // Deviating on sensor AND profile AND georeferencing still buys one
        // chip. Three chips per row is the defect this item removes.
        val fleet = List(65) { manifest("Scan-%03d".format(it)) } +
            manifest(
                "Scan-odd",
                sensor = SensorType.MID360,
                profile = WorkflowProfile.SURVEY,
                epsg = null,
            )
        assertEquals(1, chips(fleet).size)
    }

    // ── the grade mark (finding P1b) ───────────────────────────────────────

    @Test
    fun `an empty scan is graded EMPTY whether it is a null or a zero`() {
        assertEquals(ProjectRowGrade.EMPTY, ProjectRowGrade.of(manifest("stray", points = null)))
        assertEquals(ProjectRowGrade.EMPTY, ProjectRowGrade.of(manifest("stray", points = 0L)))
    }

    @Test
    fun `section breaks are the grade evidence the manifest actually carries`() {
        // 1 section: nothing to say. The manifest cannot tell GOOD from FAIR
        // (density and tracking drops are not persisted), so it says nothing
        // rather than inventing a verdict — see `ProjectRowGrade`.
        assertNull(ProjectRowGrade.of(manifest("clean", breaks = 0)))
        // 2 and 4 sections: the grader's own thresholds, same arithmetic.
        assertEquals(ProjectRowGrade.FAIR, ProjectRowGrade.of(manifest("rebuilt", breaks = 1)))
        assertEquals(ProjectRowGrade.FAIR, ProjectRowGrade.of(manifest("rebuilt", breaks = 2)))
        assertEquals(ProjectRowGrade.POOR, ProjectRowGrade.of(manifest("shattered", breaks = 3)))
        assertEquals(ProjectRowGrade.POOR, ProjectRowGrade.of(manifest("shattered", breaks = 9)))
    }

    @Test
    fun `an empty scan is EMPTY even when it also fell apart`() {
        // Order matters: "there is nothing in this file" is the thing to say,
        // and "the camera re-anchored nine times" is a diagnosis of a scan
        // that has content.
        assertEquals(
            ProjectRowGrade.EMPTY,
            ProjectRowGrade.of(manifest("nothing", points = 0L, breaks = 9)),
        )
    }

    // ── grade → colour (§C.3, `ScanColorScheme.grade`) ─────────────────────

    @Test
    fun `every grade maps to the semantic the review names, in both themes`() {
        for (dark in listOf(true, false)) {
            val c = scanColorScheme(dark)
            assertEquals("GOOD is good", c.good, c.grade("GOOD"))
            // FAIR is the NORM in this fleet, so it is ink-mute and not amber:
            // a wall of amber is the same defect as a wall of chips (item 149).
            assertEquals("FAIR is the norm, not a warning", c.inkMute, c.grade("FAIR"))
            assertNotEquals(c.warn, c.grade("FAIR"))
            assertEquals(c.warn, c.grade("POOR"))
            assertEquals(c.warn, c.grade("2D ONLY"))
            assertEquals(c.warn, c.grade("2D_ONLY"))
            // `bad` means the operator lost something, and an empty scan is the
            // one case on this screen where they did.
            assertEquals(c.bad, c.grade(ProjectRowGrade.EMPTY))
            // Unknown and absent are ink-mute, never red (§C.6, item 163).
            assertEquals(c.inkMute, c.grade(null))
            assertEquals(c.inkMute, c.grade("something else"))
        }
    }

    @Test
    fun `the codes this screen emits are all mapped, none fall through`() {
        val c = scanColorScheme(dark = true)
        assertEquals(c.bad, c.grade(ProjectRowGrade.EMPTY))
        assertEquals(c.warn, c.grade(ProjectRowGrade.POOR))
        assertEquals(c.inkMute, c.grade(ProjectRowGrade.FAIR))
    }
}
