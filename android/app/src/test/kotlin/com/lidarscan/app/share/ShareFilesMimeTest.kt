package com.lidarscan.app.share

import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

/**
 * ROUND 23 item 104c/d — **the half of the group share sheet that can be
 * tested without an emulator.**
 *
 * `ShareTargets.shareFiles` builds an `ACTION_SEND_MULTIPLE`, and neither
 * `FileProvider` nor `Intent` can be exercised on a bare JVM. What decides
 * whether the sheet is useful, though, is not the Intent plumbing — it is the
 * `type`, because that is what the system filters the target list with. ROUND
 * 15 item 56 learned this the expensive way on ONE file (a PNG offered as
 * `application/octet-stream` loses every image target); a mixed set has the
 * same trap with more ways to fall into it.
 *
 * So the MIME decision is a pure function, and it is tested here. The emulator
 * suite owns the rest.
 */
class ShareFilesMimeTest {

    private fun f(name: String) = File("/tmp/lidarscan-test/$name")

    @Test
    fun `one kind of file keeps its own precise type`() {
        assertEquals(
            "application/octet-stream",
            ShareTargets.commonMime(listOf(f("a.ply"), f("b.ply"), f("c.ply"))),
        )
        assertEquals("image/png", ShareTargets.commonMime(listOf(f("a.png"), f("b.png"))))
        assertEquals("application/zip", ShareTargets.commonMime(listOf(f("a.lscan.zip"))))
    }

    @Test
    fun `two image types collapse to the image family, not to octet-stream`() {
        // The ROUND 15 lesson: keep the image-capable targets in the sheet.
        assertEquals("image/*", ShareTargets.commonMime(listOf(f("a.png"), f("b.jpg"))))
    }

    @Test
    fun `genuinely mixed families fall back to the full wildcard`() {
        assertEquals("*/*", ShareTargets.commonMime(listOf(f("a.ply"), f("b.png"))))
        assertEquals("*/*", ShareTargets.commonMime(listOf(f("a.lscan.zip"), f("b.png"), f("c.las"))))
    }

    @Test
    fun `an empty set is the wildcard rather than a crash`() {
        assertEquals("*/*", ShareTargets.commonMime(emptyList()))
    }

    @Test
    fun `the point-cloud formats this app exports all share one type`() {
        // PLY, LAS and PCD are the three format chips on Review, plus the
        // bundle. The first three must agree, or a three-scan export would
        // arrive at the sheet as the bare wildcard for no reason.
        assertEquals(
            "application/octet-stream",
            ShareTargets.commonMime(listOf(f("a.ply"), f("b.las"), f("c.pcd"))),
        )
    }

    @Test
    fun `mimeFor still answers per file — the multi path did not change it`() {
        assertEquals("application/zip", ShareTargets.mimeFor(f("scan-068.lscan.zip")))
        assertEquals("image/png", ShareTargets.mimeFor(f("plan.png")))
        assertEquals("application/octet-stream", ShareTargets.mimeFor(f("scan-068.ply")))
        assertEquals("application/pdf", ShareTargets.mimeFor(f("plan.pdf")))
    }
}
