package com.lidarscan.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.capture.FloorPlanResult
import com.lidarscan.core.capture.StitchResult
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 15 items 56 + 57, through the REAL native library on a device, over
 * the owner's own scan-030 container.
 *
 * The JVM tests prove the sentences and the engine tests prove the geometry.
 * What only this can prove is that the two halves are actually connected on an
 * arm64 phone: that the new ABI-11 entry points are exported and callable, that
 * the double[] slot tables the JNI documents match the ones `:core` decodes,
 * and that a PNG the engine wrote is a file Android can decode.
 *
 * scan-030 is the five-section capture ROUND 13 used, so it also exercises the
 * one case the fast path skips.
 */
@RunWith(AndroidJUnit4::class)
class Round15PlanAndRulerTest {

    /**
     * The assets live in the TEST apk, not the app's — `targetContext.assets`
     * is a different APK and lists nothing. Same staging Round13ProcessScanTest
     * uses, kept identical on purpose so the two tests are looking at the same
     * bytes of the owner's scan-030.
     */
    private fun stageScan030(): File {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val dest = File(ctx.filesDir, "round15/scan-030.lscan")
        dest.deleteRecursively()
        dest.mkdirs()
        val am = InstrumentationRegistry.getInstrumentation().context.assets

        fun copyDir(assetPath: String, into: File) {
            val entries = am.list(assetPath) ?: return
            if (entries.isEmpty()) {
                into.parentFile?.mkdirs()
                am.open(assetPath).use { input -> into.outputStream().use { input.copyTo(it) } }
                return
            }
            into.mkdirs()
            for (e in entries) copyDir("$assetPath/$e", File(into, e))
        }
        copyDir("scan-030.lscan", dest)
        return dest
    }

    @Test
    fun the_reprocess_now_carries_the_self_consistency_ruler() {
        val dir = stageScan030()
        val v = ScanEngineNative.nativeProcReprocessD6(dir.absolutePath, true, null)
        assertNotNull("nativeProcReprocessD6 returned null", v)
        // ROUND 15 grew the slot table 16 -> 22. A native library that was not
        // rebuilt returns 16 and this is the assertion that says so out loud,
        // rather than the self-check quietly being absent forever.
        assertEquals("the slot table must be the ROUND 15 one", 22, v!!.size)

        val r = StitchResult.fromNative(v)
        assertNotNull(r)
        // ROUND 13's pinned numbers must not have moved: item 57 adds a
        // measurement, it does not change the correction.
        assertEquals(5, r!!.sections)
        assertEquals(4, r.seams)
        assertEquals(78421L, r.points)
        assertTrue(r.verticalExtentAfterM < r.verticalExtentBeforeM * 0.5)

        val sc = r.selfCheck
        assertNotNull("the ruler must have come back", sc)
        val line = r.selfCheckLine
        assertNotNull(line)
        assertFalse("no unsubstituted format specifier may reach the card", line!!.contains("%"))
        if (sc!!.measurable) {
            // A real indoor walk lands single-digit centimetres (ROUND 12
            // measured 0.70 cm crawling and 4.5-5.3 cm walking). A number
            // outside this range means the ruler is measuring something else.
            assertTrue("offset $line", sc.offsetMeters > 0.0 && sc.offsetMeters < 0.50)
            assertTrue("floor must be below the reading", sc.floorMeters <= sc.offsetMeters * 3.0)
            assertTrue(line, line.contains("cm"))
        } else {
            assertTrue(line, line.contains("not measurable"))
        }
    }

    @Test
    fun a_floor_plan_comes_out_of_a_sealed_container_and_android_can_decode_it() {
        val dir = stageScan030()
        val v = ScanEngineNative.nativeProcFloorPlan(
            dir.absolutePath,
            1.0,
            1.5,
            0.02,
            1200,
            null,
            "floorplan",
            "SCAN-030",
        )
        assertNotNull("nativeProcFloorPlan returned null", v)
        assertEquals(24, v!!.size)
        val paths = ScanEngineNative.nativeProcPlanFilePaths()
        assertNotNull(paths)
        val r = FloorPlanResult.fromNative(v, paths)
        assertNotNull(r)
        assertTrue("the plan must have run", r!!.ran)

        // The cloud must have come from the container, not from whatever the
        // process-wide ProcessingEngine happened to be holding.
        assertTrue("cloud source: ${r.cloudSource}", r.cloudSource.isNotEmpty())
        assertTrue(r.cloudPoints > 10_000L)

        // The PNG is the artifact the operator sees, so it is the one asserted
        // hardest: it exists, it is in `processed/`, and Android's own decoder
        // accepts it at the dimensions the engine reported.
        assertTrue("no PNG path", r.pngPath.isNotEmpty())
        val png = File(r.pngPath)
        assertTrue("PNG missing at ${r.pngPath}", png.isFile)
        assertTrue("PNG is empty", png.length() > 1024)
        assertTrue("PNG must live in processed/", png.parentFile!!.name == "processed")
        val bmp = android.graphics.BitmapFactory.decodeFile(png.absolutePath)
        assertNotNull("Android could not decode the engine's PNG", bmp)
        assertEquals(r.pngWidth, bmp!!.width)
        assertEquals(r.pngHeight, bmp.height)
        // Scale is a measurement and must be on the result, not only on the
        // picture.
        assertTrue(r.pixelsPerMeter > 1.0)
        assertTrue(r.scaleBarMeters > 0.0)

        // DXF and PDF exist exactly when something was fitted. A density-mode
        // plan deliberately writes neither — an empty DXF that opens to nothing
        // is worse than an absent one.
        if (r.mode == FloorPlanResult.Mode.WALLS) {
            assertTrue(r.walls > 0)
            assertTrue("no DXF beside ${r.walls} walls", r.dxfPath.isNotEmpty())
            assertTrue(File(r.dxfPath).length() > 100)
            assertTrue(File(r.pdfPath).length() > 100)
            // Independent check that the DXF is a DXF: R12 ASCII opens with a
            // group-code 0 SECTION and names the layers A12 declares.
            val dxf = File(r.dxfPath).readText()
            assertTrue(dxf.contains("SECTION"))
            assertTrue(dxf.contains("WALLS"))
            assertTrue(dxf.trimEnd().endsWith("EOF"))
        } else {
            assertEquals(0, r.walls)
            assertTrue(r.dxfPath.isEmpty())
            assertTrue(r.pdfPath.isEmpty())
        }

        // And the sentences never leak a format specifier.
        assertFalse(r.headline, r.headline.contains("%"))
        r.detail?.let { assertFalse(it, it.contains("%")) }

        // Determinism: a second run over the same container writes the same
        // bytes. No clock, no RNG seed drawn from anywhere, no compression
        // level — the same property the DXF and PDF writers have.
        val first = png.readBytes()
        ScanEngineNative.nativeProcFloorPlan(
            dir.absolutePath, 1.0, 1.5, 0.02, 1200, null, "floorplan", "SCAN-030",
        )
        assertTrue("the plan PNG must be byte-identical run to run", first.contentEquals(png.readBytes()))
    }
}
