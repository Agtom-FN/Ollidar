package com.lidarscan.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.capture.MountVerdict
import com.lidarscan.core.capture.StitchResult
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 13 — "Process this scan", on the owner's ACTUAL scan-030 bytes,
 * through the real Android path: the app's own `libscanengine_jni.so`, the
 * ABI-10 C entry point, and the same `StitchResult` decode the Review screen
 * uses.
 *
 * ## Why this has to run on a device and not on the JVM
 *
 * Everything else about stitching is covered by engine unit tests against
 * injected truth. What those cannot cover is the seam this round added: a flat
 * `DoubleArray(16)` crossing JNI, whose slots are a hand-maintained contract
 * between `processing_jni.cpp` and `StitchResult.fromNative`. A wrong index
 * there is a *plausible number in the wrong place* — it compiles, it runs, and
 * the operator is told the height spread improved when it did not. The only
 * way to catch it is to run the real bytes through the real binding and check
 * the answers against what the engine independently reports.
 *
 * scan-030 is the owner's 2026-08-18 walk: 40.9 s, five sections, the capture
 * that made them say "result not satisfy".
 */
@RunWith(AndroidJUnit4::class)
class Round13ProcessScanTest {

    private fun stageScan030(): File {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val dest = File(ctx.filesDir, "round13/scan-030.lscan")
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
    fun processing_scan030_puts_five_pieces_into_one_frame() {
        val dir = stageScan030()
        assertTrue("staged container is missing its raw returns",
            File(dir, "streams/lidar.bin").length() > 100_000)
        val sealedBefore = mapOf(
            "streams/lidar.bin" to File(dir, "streams/lidar.bin").length(),
            "streams/poses_ar.bin" to File(dir, "streams/poses_ar.bin").length(),
            "streams/imu_phone.bin" to File(dir, "streams/imu_phone.bin").length(),
            "streams/map.bin" to File(dir, "streams/map.bin").length(),
        )

        assertFalse(ScanEngineNative.nativeProcHasStitchedCloud(dir.absolutePath))

        // Drive it exactly the way ReviewViewModel does, progress callback and
        // all, so a callback that crashes the native thread fails the test.
        val seen = ArrayList<Float>()
        val raw = ScanEngineNative.nativeProcReprocessD6(dir.absolutePath, true) { f ->
            seen.add(f)
            true
        }
        val r: StitchResult? = StitchResult.fromNative(raw)
        assertNotNull("the reprocess returned nothing", r)
        requireNotNull(r)

        assertTrue("the pipeline did not run", r.ran)
        assertTrue("no corrected cloud was written", r.mapWritten)

        // The engine's own numbers for this container, independently produced
        // by engine_cli --d6-stitch and recorded in android/NOTES.md ROUND 13.
        // If a JNI slot moves, at least one of these breaks.
        assertEquals("scan-030 is a five-section capture", 5, r.sections)
        assertEquals(4, r.seams)
        assertEquals(78421L, r.points)
        assertEquals(1210L, r.poses)
        // The 14 poses ARCore recorded at the origin before it had a frame.
        assertEquals(14L, r.posesUntracked)

        // THE HEADLINE, and the one number that is not self-referential: the
        // operator walks on a flat floor, so the trajectory's spread along
        // gravity must SHRINK. 0.82 m -> 0.27 m.
        assertEquals(0.820, r.verticalExtentBeforeM, 0.01)
        assertEquals(0.271, r.verticalExtentAfterM, 0.01)
        assertTrue(
            "stitching must reduce the vertical wander, not increase it",
            r.verticalExtentAfterM < r.verticalExtentBeforeM * 0.5,
        )
        assertEquals(0.517, r.movedMeters, 0.01)

        // This rig's mount was fine on this walk; the watchdog must stay quiet.
        assertEquals(MountVerdict.OK, r.mountVerdict)
        assertEquals(0.0, r.mountImpossibleFraction, 1e-9)

        // Progress actually arrived and was monotone — the Review screen drives
        // a determinate bar from it.
        assertTrue("no progress was reported", seen.isNotEmpty())
        assertTrue("progress went backwards", seen.zipWithNext().all { (a, b) -> b >= a })

        // The words the operator reads.
        assertTrue(r.headline.contains("5 pieces"))
        assertFalse("a literal placeholder escaped to the UI", r.headline.contains("%"))
        assertFalse(requireNotNull(r.detail).contains("%"))

        // The derived product exists, with its provenance beside it...
        assertTrue(ScanEngineNative.nativeProcHasStitchedCloud(dir.absolutePath))
        val stitched = File(dir, "processed/map_stitched.bin")
        val sidecar = File(dir, "processed/stitch.json")
        assertTrue(stitched.length() > 1_000_000)
        assertTrue(sidecar.readText().contains("\"sections\": 5"))

        // ...and NOT ONE BYTE of what the phone sealed has moved. This is the
        // doctrine the whole design turns on: "replay == capture" still holds
        // over the raw streams, and deleting the two derived files restores
        // exactly what was recorded.
        for ((path, len) in sealedBefore) {
            assertEquals("$path was modified by processing", len, File(dir, path).length())
        }

        // Idempotent: the same raw bytes give the same answer, and the second
        // run replaces rather than appends.
        val again = StitchResult.fromNative(
            ScanEngineNative.nativeProcReprocessD6(dir.absolutePath, true, null),
        )
        requireNotNull(again)
        assertEquals(r.sections, again.sections)
        assertEquals(r.points, again.points)
        assertEquals(r.verticalExtentAfterM, again.verticalExtentAfterM, 1e-9)
        assertEquals(stitched.length(), File(dir, "processed/map_stitched.bin").length())

        dir.deleteRecursively()
    }

    @Test
    fun the_mount_watchdog_stays_quiet_on_a_correctly_mounted_walk() {
        val dir = stageScan030()
        val v = ScanEngineNative.nativeProcMountCheck(dir.absolutePath, 6.0)
        assertNotNull(v)
        requireNotNull(v)
        assertEquals(MountVerdict.OK, MountVerdict.of(v[0].toInt()))
        assertTrue("expected complete revolutions in the first 6 s", v[1] >= 10)
        assertEquals("no return may land at an impossible height", 0.0, v[4], 1e-9)
        dir.deleteRecursively()
    }
}
