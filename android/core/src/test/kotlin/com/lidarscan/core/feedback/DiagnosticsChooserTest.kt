package com.lidarscan.core.feedback

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 29 item 173(c) — **the chooser's state, without an emulator.**
 *
 * The interesting question about *Send diagnostics* is not what the sheet looks
 * like, it is which doors exist on a given phone and what each one promises.
 * Both are values here, so both are pinned here.
 */
class DiagnosticsChooserTest {

    @Test
    fun `a phone with no cloud fields gets three doors`() {
        assertEquals(
            listOf(FeedbackRoute.GITHUB, FeedbackRoute.SAVE, FeedbackRoute.SHARE),
            DiagnosticsChooser.optionsFor(serverConfigured = false),
        )
    }

    @Test
    fun `a configured server is offered last and only when it is configured`() {
        val options = DiagnosticsChooser.optionsFor(serverConfigured = true)
        assertEquals(4, options.size)
        assertEquals(FeedbackRoute.SERVER, options.last())
        assertFalse(FeedbackRoute.SERVER in DiagnosticsChooser.optionsFor(serverConfigured = false))
    }

    @Test
    fun `GitHub is first, because that is where the owner reads`() {
        assertEquals(FeedbackRoute.GITHUB, DiagnosticsChooser.optionsFor(false).first())
    }

    @Test
    fun `every door has a title and a detail, and both obey the wording law`() {
        for (route in DiagnosticsChooser.optionsFor(serverConfigured = true)) {
            val title = DiagnosticsChooser.titleFor(route)
            val detail = DiagnosticsChooser.detailFor(route)
            assertTrue("blank title for $route", title.isNotBlank())
            assertTrue("$route title: $title", WordingLaw.isInstruction(title))
            assertTrue("$route detail: $detail", WordingLaw.isDetail(detail))
        }
    }

    @Test
    fun `every door has its own test tag`() {
        val tags = DiagnosticsChooser.optionsFor(true).map { DiagnosticsChooser.testTagFor(it) }
        assertEquals(tags.size, tags.toSet().size)
        assertEquals("diagnosticsGithub", DiagnosticsChooser.testTagFor(FeedbackRoute.GITHUB))
    }

    // ── what each route is allowed to CLAIM when it finishes ───────────────

    @Test
    fun `a local route never says Sent - the app did not send anything`() {
        for (route in listOf(FeedbackRoute.GITHUB, FeedbackRoute.SAVE)) {
            assertTrue("$route", route.isLocal)
            val line = FeedbackWording.resultFor(
                FeedbackResult(sent = true, route = route, downloadsPath = "Downloads/LidarScan/a.zip"),
            )
            assertFalse("$route said \"${FeedbackWording.SENT}\"", line == FeedbackWording.SENT)
        }
    }

    @Test
    fun `GitHub says it opened a browser and Save says where the file is`() {
        assertEquals(
            FeedbackWording.OPENED_GITHUB,
            FeedbackWording.resultFor(
                FeedbackResult(true, FeedbackRoute.GITHUB, "Downloads/LidarScan/a.zip"),
            ),
        )
        assertEquals(
            "Saved to Downloads/LidarScan/a.zip",
            FeedbackWording.resultFor(
                FeedbackResult(true, FeedbackRoute.SAVE, "Downloads/LidarScan/a.zip"),
            ),
        )
    }

    @Test
    fun `a failed local route says the zip was not saved, not that a send failed`() {
        assertEquals(
            FeedbackWording.NOT_SAVED,
            FeedbackWording.resultFor(FeedbackResult(false, FeedbackRoute.SAVE, null)),
        )
        assertEquals(
            FeedbackWording.NOT_SENT,
            FeedbackWording.resultFor(FeedbackResult(false, FeedbackRoute.SHARE, "x")),
        )
    }

    @Test
    fun `the two delivery routes are unchanged`() {
        assertFalse(FeedbackRoute.SERVER.isLocal)
        assertFalse(FeedbackRoute.SHARE.isLocal)
        assertEquals(
            FeedbackWording.SENT,
            FeedbackWording.resultFor(FeedbackResult(true, FeedbackRoute.SHARE, "x")),
        )
        // Round 24's config still decides only between the two it knows about.
        assertEquals(FeedbackRoute.SHARE, FeedbackConfig().route)
        assertEquals(FeedbackRoute.SERVER, FeedbackConfig("https://h", "t").route)
    }
}
