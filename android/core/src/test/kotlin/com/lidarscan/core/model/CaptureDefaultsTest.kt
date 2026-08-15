package com.lidarscan.core.model

import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.render.DisplayProfile
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B5 — the profile table is the thing this task adds that changes what a
 * capture *does*, so every claim it makes is asserted rather than trusted.
 */
class CaptureDefaultsTest {

    @Test
    fun `every profile maps to its own display profile`() {
        assertEquals(DisplayProfile.SURVEY, CaptureDefaults.forProfile(WorkflowProfile.SURVEY).displayProfile)
        assertEquals(DisplayProfile.FLOOR_PLAN, CaptureDefaults.forProfile(WorkflowProfile.FLOOR_PLAN).displayProfile)
        assertEquals(DisplayProfile.RESEARCH, CaptureDefaults.forProfile(WorkflowProfile.RESEARCH).displayProfile)
        assertEquals(DisplayProfile.QUICK_SCAN, CaptureDefaults.forProfile(WorkflowProfile.QUICK_SCAN).displayProfile)
    }

    @Test
    fun `survey is the only profile that blocks capture on fix quality`() {
        val survey = CaptureDefaults.forProfile(WorkflowProfile.SURVEY)
        assertTrue(survey.requireRtkFixForCapture)
        assertEquals(FixType.RTK_FLOAT, survey.minFixForCapture)
        listOf(WorkflowProfile.FLOOR_PLAN, WorkflowProfile.RESEARCH, WorkflowProfile.QUICK_SCAN).forEach {
            assertFalse("$it must not block on fix quality", CaptureDefaults.forProfile(it).requireRtkFixForCapture)
        }
    }

    @Test
    fun `survey exports LAS because it is the only format A9 gives a real CRS`() {
        assertEquals(ExportFormat.LAS14, CaptureDefaults.forProfile(WorkflowProfile.SURVEY).exportFormat)
    }

    @Test
    fun `research records only — the raw streams are the point of that profile`() {
        assertFalse(CaptureDefaults.forProfile(WorkflowProfile.RESEARCH).liveSlam)
        // and every other profile previews live
        listOf(WorkflowProfile.SURVEY, WorkflowProfile.FLOOR_PLAN, WorkflowProfile.QUICK_SCAN).forEach {
            assertTrue("$it should preview live", CaptureDefaults.forProfile(it).liveSlam)
        }
    }

    @Test
    fun `floor plan runs no camera pipeline — colour does not enter wall extraction`() {
        val fp = CaptureDefaults.forProfile(WorkflowProfile.FLOOR_PLAN)
        assertFalse(fp.captureCameraKeyframes)
        assertFalse(fp.colorizeAfterProcessing)
    }

    @Test
    fun `colorization is never pre-checked without keyframes to sample from`() {
        WorkflowProfile.entries.forEach {
            val d = CaptureDefaults.forProfile(it)
            if (d.colorizeAfterProcessing) {
                assertTrue("$it pre-checks colorize but records no keyframes", d.captureCameraKeyframes)
            }
        }
    }

    @Test
    fun `the floor-plan slice band matches A12's SliceOptions default`() {
        val fp = CaptureDefaults.forProfile(WorkflowProfile.FLOOR_PLAN)
        assertEquals(1.0f, fp.planSliceMinM, 0f)
        assertEquals(1.5f, fp.planSliceMaxM, 0f)
    }

    @Test
    fun `engine profile strings are exactly the four the C ABI header names`() {
        assertEquals("survey", CaptureDefaults.engineProfileString(WorkflowProfile.SURVEY))
        assertEquals("floorplan", CaptureDefaults.engineProfileString(WorkflowProfile.FLOOR_PLAN))
        assertEquals("research", CaptureDefaults.engineProfileString(WorkflowProfile.RESEARCH))
        assertEquals("quickscan", CaptureDefaults.engineProfileString(WorkflowProfile.QUICK_SCAN))
    }

    @Test
    fun `export format codes mirror the C++ ExportFormat enum`() {
        assertEquals(0, ExportFormat.PLY_BINARY.code)
        assertEquals(1, ExportFormat.LAS14.code)
        assertEquals(2, ExportFormat.PCD.code)
        assertEquals(3, ExportFormat.DXF.code)
        assertEquals(4, ExportFormat.PDF.code)
    }

    @Test
    fun `only the three point-cloud writers are offered as export formats`() {
        val offered = ExportFormat.pointCloudFormats
        assertEquals(listOf(ExportFormat.PLY_BINARY, ExportFormat.LAS14, ExportFormat.PCD), offered)
        // A9's export_points() returns kUnimplemented for DXF/PDF — those go
        // through A12's own writers, from the plan screen.
        assertFalse(ExportFormat.DXF.isPointCloud)
        assertFalse(ExportFormat.PDF.isPointCloud)
    }

    @Test
    fun `a manifest without capture defaults falls back to its profile's`() {
        val manifest = ProjectManifest(
            name = "Legacy",
            sensor = SensorType.COIN_D6,
            profile = WorkflowProfile.SURVEY,
            createdAtEpochMillis = 0L,
            appVersion = "0",
        )
        assertNull(manifest.captureDefaults)
        assertEquals(CaptureDefaults.forProfile(WorkflowProfile.SURVEY), manifest.effectiveCaptureDefaults())
        assertNotNull(manifest.effectiveDisplayParams())
    }

    @Test
    fun `a project's own capture defaults win over its profile's`() {
        val custom = CaptureDefaults.forProfile(WorkflowProfile.SURVEY).copy(liveSlam = false)
        val manifest = ProjectManifest(
            name = "Edited",
            sensor = SensorType.MID360,
            profile = WorkflowProfile.SURVEY,
            createdAtEpochMillis = 0L,
            appVersion = "0",
            captureDefaults = custom,
        )
        // This is the whole reason the defaults are stored rather than
        // re-derived: an operator's per-project edit must survive.
        assertFalse(manifest.effectiveCaptureDefaults().liveSlam)
    }
}
