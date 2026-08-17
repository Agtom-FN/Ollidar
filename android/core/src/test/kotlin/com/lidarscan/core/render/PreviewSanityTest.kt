package com.lidarscan.core.render

import java.io.DataInputStream
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 8 — the Projects-tab preview, tested against the OWNER'S OWN EXPORT.
 *
 * `captures/scan-015-pixel-0.4.0.lscan/` is a real COIN-D6 capture off a
 * Pixel 8 Pro running LidarScan 0.4.0: 191,381 points over 26.3 seconds, walked
 * through a room, exported through the app's own share sheet. It is checked
 * into the repository precisely so this file can point at it, because the bug
 * it contains is invisible in any synthetic fixture — a fake source produces
 * one stream, and the bug is that there were two.
 *
 * The headline case below is [the_real_0_4_0_export_preview_is_half_raw_2d_fan],
 * and it is a **characterisation test**: it asserts that the bug is present in
 * the shipped file, with the exact number. If that ever stops holding, either
 * the fixture was replaced or someone has quietly regenerated it, and this test
 * failing is the right way to find that out.
 */
class PreviewSanityTest {

    // --- the real capture ----------------------------------------------------

    private fun repoRoot(): File {
        // Gradle runs a test with `user.dir` at the module directory, but that
        // is a convention rather than a contract (IDE runners differ), so walk
        // up looking for the marker instead of counting `..`s.
        var dir: File? = File(System.getProperty("user.dir") ?: ".").absoluteFile
        while (dir != null) {
            if (File(dir, "captures").isDirectory && File(dir, "engine").isDirectory) return dir
            dir = dir.parentFile
        }
        error("could not locate the repository root from ${System.getProperty("user.dir")}")
    }

    private fun realPreviewFile() =
        File(repoRoot(), "captures/scan-015-pixel-0.4.0.lscan/processed/preview.f32")

    /** Reads the on-disk preview format: magic, count, then BIG-endian xyz triples (DataOutputStream's order). */
    private fun readPreview(f: File): Pair<FloatArray, Int> {
        DataInputStream(f.inputStream().buffered()).use { s ->
            assertEquals("preview magic (\"LSPV\")", 0x4C53_5056, s.readInt())
            val n = s.readInt()
            val raw = FloatArray(n * 3)
            for (i in raw.indices) raw[i] = s.readFloat()
            return raw to n
        }
    }

    @Test
    fun the_real_0_4_0_export_preview_is_half_raw_2d_fan() {
        val f = realPreviewFile()
        assertTrue("the checked-in field capture is missing: $f", f.isFile)
        val (raw, n) = readPreview(f)

        assertEquals("scan-015's preview point count", 4040, n)

        var zeroZ = 0
        var nonFinite = 0
        for (i in 0 until n) {
            val z = raw[i * 3 + 2]
            if (!raw[i * 3].isFinite() || !raw[i * 3 + 1].isFinite() || !z.isFinite()) nonFinite++
            if (z == 0.0f) zeroZ++
        }

        // Not corruption. Every value is a perfectly good float — which is why
        // nothing caught this for four rounds.
        assertEquals("every value in the real preview is finite", 0, nonFinite)

        // EXACTLY half, which is the tell: one raw sensor-frame return sampled
        // for every resolved world-frame one. A COIN-D6's returns lie in its own
        // scan plane by construction, so their z is 0 to the bit.
        assertEquals("points at exactly z == 0 in the real preview", 2027, zeroZ)
        assertTrue(
            "the real 0.4.0 preview should be ~50 % raw fan; measured ${zeroZ * 100f / n}%",
            zeroZ.toFloat() / n > 0.49f,
        )

        // And therefore: the shipped file is refused by the check that now
        // guards both the write and the read.
        val verdict = PreviewSanity.check(raw, n)
        assertTrue(
            "the real 0.4.0 preview must be refused, got $verdict",
            verdict is PreviewSanity.Verdict.Rejected,
        )
        assertTrue(
            "the refusal must name the cause, got: ${(verdict as PreviewSanity.Verdict.Rejected).reason}",
            verdict.reason.contains("raw sensor-frame fan"),
        )
    }

    // --- the verdict itself --------------------------------------------------

    /** A resolved 3D room: points spread over a walk, no plane of exact zeros. */
    private fun resolvedRoom(n: Int = 2000): FloatArray {
        val out = FloatArray(n * 3)
        for (i in 0 until n) {
            val t = i.toFloat() / n
            out[i * 3] = t * 4.0f                                   // 4 m walk
            out[i * 3 + 1] = 2.4f + 0.01f * kotlin.math.sin(t * 37f) // the wall
            out[i * 3 + 2] = 0.1f + t * 2.7f                        // floor to ceiling
        }
        return out
    }

    @Test
    fun a_resolved_room_passes() {
        assertEquals(PreviewSanity.Verdict.Ok, PreviewSanity.check(resolvedRoom(), 2000))
    }

    @Test
    fun a_pure_2d_fan_is_refused() {
        // What a Record-only D6 preview looks like: a disc at z = 0.
        val n = 500
        val fan = FloatArray(n * 3)
        for (i in 0 until n) {
            val a = i.toFloat() / n * 2f * Math.PI.toFloat()
            fan[i * 3] = 2f * kotlin.math.cos(a)
            fan[i * 3 + 1] = 2f * kotlin.math.sin(a)
            fan[i * 3 + 2] = 0f
        }
        val v = PreviewSanity.check(fan, n)
        assertTrue("a pure 2D fan must be refused, got $v", v is PreviewSanity.Verdict.Rejected)
    }

    @Test
    fun nan_and_infinity_are_refused() {
        for (bad in listOf(Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY)) {
            val pts = resolvedRoom()
            pts[1234 * 3 + 1] = bad
            val v = PreviewSanity.check(pts, 2000)
            assertTrue("$bad must be refused, got $v", v is PreviewSanity.Verdict.Rejected)
            assertTrue((v as PreviewSanity.Verdict.Rejected).reason.contains("not finite"))
        }
    }

    @Test
    fun uninitialised_memory_sized_values_are_refused() {
        // The classic shape of a buffer that was never written, or was freed and
        // re-read: values around 1e38, which are FINITE and pass every naive
        // check. The extent gate is what catches them.
        //
        // 3.0e38, not the 6.4e38 that a first (little-endian) read of the real
        // preview.f32 appeared to show: `Float.MAX_VALUE` is 3.4e38, so a
        // 6.4e38 literal is already Infinity and would be caught one gate
        // earlier by the finiteness check — which would make this case a
        // duplicate of the NaN one rather than a test of the extent gate.
        val pts = resolvedRoom()
        pts[7 * 3] = 3.0e38f
        val v = PreviewSanity.check(pts, 2000)
        assertTrue("1e38-scale garbage must be refused, got $v", v is PreviewSanity.Verdict.Rejected)
        assertTrue((v as PreviewSanity.Verdict.Rejected).reason.contains("uninitialised"))
    }

    @Test
    fun a_few_points_on_the_floor_plane_do_not_trip_the_gate() {
        // The required false-positive control. A real scan can legitimately put
        // a handful of returns at exactly z = 0 (a floor sample, a point at the
        // session origin), and a thumbnail is not worth a false alarm.
        val pts = resolvedRoom()
        for (i in 0 until 300) pts[i * 3 + 2] = 0f  // 15 %, under the 33 % gate
        assertEquals(PreviewSanity.Verdict.Ok, PreviewSanity.check(pts, 2000))
    }

    @Test
    fun too_few_points_and_short_buffers_are_refused() {
        assertTrue(PreviewSanity.check(FloatArray(30), 10) is PreviewSanity.Verdict.Rejected)
        assertTrue(PreviewSanity.check(FloatArray(30), 100) is PreviewSanity.Verdict.Rejected)
    }
}
