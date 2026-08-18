package com.lidarscan.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.processing.TrajectoryFile
import com.lidarscan.core.capture.StitchResult
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 17 item 65 — **the file the path is drawn from, produced by the
 * PHONE's engine and read back by the PHONE's decoder.**
 *
 * The owner reported "my path not show in the point cloud" against a build
 * whose engine predated `write_trajectory` entirely: every container he had
 * processed on the device — scan-036, scan-038 — has a `processed/` directory
 * with no `trajectory.bin` in it, so Review had nothing to draw and said
 * nothing about why. ROUND 16 added the writer, the reader, the material and
 * the draw call and touched **zero files under `src/androidTest/`**, so
 * nothing on the device path was ever asserted.
 *
 * This is that assertion, and it is deliberately end-to-end across the seam
 * that actually broke: the real `libscanengine_jni.so` writes the file, and
 * `TrajectoryFile` — an independent decoder written from the format spec
 * rather than from the C++ struct — reads it back into the exact `Ribbon` the
 * renderer is handed. A format drift between the two shows up here as a length
 * mismatch rather than as an empty 3D view in somebody's flat.
 *
 * Runs on the same staged scan-030 bytes as [Round13ProcessScanTest].
 */
@RunWith(AndroidJUnit4::class)
class Round17TrajectoryOnDeviceTest {

    private fun stageScan030(): File {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val dest = File(ctx.filesDir, "round17/scan-030.lscan")
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
    fun processing_writes_a_trajectory_the_phone_can_decode_into_a_drawable_ribbon() {
        val dir = stageScan030()
        val traj = File(dir, "processed/trajectory.bin")
        // The pre-ROUND-16 state, which is the state every container the owner
        // owns is still in.
        assertTrue("staged container must not already carry a trajectory", !traj.isFile)
        assertEquals(
            "Review must say so rather than drawing nothing in silence",
            com.lidarscan.core.capture.TrajectoryRibbon.EMPTY,
            TrajectoryFile.read(traj),
        )

        val raw = ScanEngineNative.nativeProcReprocessD6(dir.absolutePath, true) { true }
        val r: StitchResult? = StitchResult.fromNative(raw)
        assertNotNull("the reprocess returned nothing", r)
        requireNotNull(r)
        assertTrue("the pipeline did not run", r.ran)

        // 1. The engine wrote it, on the device, through the same C entry point
        //    the Process button uses.
        assertTrue("processed/trajectory.bin was not written", traj.isFile)

        // 2. And it is exactly the length the format says: an 8-byte magic, a
        //    u32 count, a u32 reserved, then a record per pose. If the C++
        //    writer and this arithmetic ever disagree, the phone reads garbage
        //    or nothing — and it would do it silently. ROUND 18 item 70: the
        //    record grew from 12 to 16 bytes ("LSTRAJ02" — xyz plus a u32 of
        //    untracked/jump flags, so Review stops drawing the tracker's blind
        //    stretches as walked lines).
        assertEquals(
            "trajectory.bin length disagrees with the pose count the engine reported",
            16L + 16L * r.poses,
            traj.length(),
        )

        // 3. The phone's own decoder turns it into vertices the renderer can
        //    draw. `count >= 2` is the real bar: a LINE_STRIP under two
        //    vertices draws nothing at all, which looks exactly like the bug.
        val ribbon = TrajectoryFile.read(traj)
        assertTrue(
            "decoded ribbon has ${ribbon.count} vertices — a path needs at least 2",
            ribbon.count >= 2,
        )
        assertEquals(ribbon.count * 3, ribbon.xyz.size)
        assertEquals(ribbon.count, ribbon.rgba.size)
        assertTrue(
            "thinning must not collapse a 40 s walk to a handful of points",
            ribbon.count >= 20,
        )

        // 4. Every vertex is finite and the walk actually goes somewhere — a
        //    ribbon of NaNs is a ribbon that draws nothing, and Filament will
        //    not complain about it.
        var minX = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var minZ = Float.MAX_VALUE
        var maxZ = -Float.MAX_VALUE
        for (i in 0 until ribbon.count) {
            val x = ribbon.xyz[i * 3]
            val y = ribbon.xyz[i * 3 + 1]
            val z = ribbon.xyz[i * 3 + 2]
            assertTrue("vertex $i is not finite", x.isFinite() && y.isFinite() && z.isFinite())
            if (x < minX) minX = x
            if (x > maxX) maxX = x
            if (z < minZ) minZ = z
            if (z > maxZ) maxZ = z
        }
        assertTrue(
            "the walk has no extent: ${maxX - minX} x ${maxZ - minZ} m",
            (maxX - minX) > 1.0f || (maxZ - minZ) > 1.0f,
        )

        // 5. Reprocessing again is idempotent about this file too — the Process
        //    button is reachable twice and must not corrupt what it wrote.
        val firstLength = traj.length()
        ScanEngineNative.nativeProcReprocessD6(dir.absolutePath, true) { true }
        assertEquals(firstLength, traj.length())
    }
}
