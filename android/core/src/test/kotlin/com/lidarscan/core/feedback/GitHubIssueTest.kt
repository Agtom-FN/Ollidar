package com.lidarscan.core.feedback

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 29 item 173(c) — **the URL, held still.**
 *
 * The whole of this feature is a string. If the string is wrong the operator
 * gets a GitHub page with an empty body, or a body cut mid-table, or a `+` in
 * the middle of his sentence — and none of those look like an app bug to him,
 * they look like his report vanished. So the encoding, the clamp and the two
 * bodies are pinned here, on a bare JVM, exactly as `FeedbackEndpoint`'s
 * multipart was.
 */
class GitHubIssueTest {

    private val facts = DeviceFacts(
        appVersion = "0.9.14",
        versionCode = 914,
        deviceModel = "Google Pixel 8 Pro",
        androidVersion = "16",
        engineAbi = 12,
        scanCount = 66,
        storageBytes = 8_100_000_000L,
    )

    // ── the repository, and the shape of the URL ───────────────────────────

    @Test
    fun `the url points at the owner's public repository`() {
        assertEquals("https://github.com/Agtom-FN/Ollidar", GitHubIssue.REPO_URL)
        assertTrue(
            GitHubIssue.urlFor("t", "b")
                .startsWith("https://github.com/Agtom-FN/Ollidar/issues/new?title="),
        )
    }

    @Test
    fun `title and body are both query parameters, in that order`() {
        val url = GitHubIssue.urlFor("Title", "Body")
        assertEquals("https://github.com/Agtom-FN/Ollidar/issues/new?title=Title&body=Body", url)
    }

    // ── encoding ───────────────────────────────────────────────────────────

    @Test
    fun `a space is percent-twenty and never a plus`() {
        // `URLEncoder` writes `+`, which GitHub renders literally inside a code
        // fence. This is the whole reason the encoder is hand-rolled.
        assertEquals("two%20words", GitHubIssue.encode("two words"))
        assertFalse(GitHubIssue.encode("two words").contains('+'))
    }

    @Test
    fun `the unreserved set survives and everything else does not`() {
        assertEquals("aZ0-_.~", GitHubIssue.encode("aZ0-_.~"))
        assertEquals("%26", GitHubIssue.encode("&"))
        assertEquals("%3D", GitHubIssue.encode("="))
        assertEquals("%23", GitHubIssue.encode("#"))
        assertEquals("%2A", GitHubIssue.encode("*"))
        assertEquals("%0A", GitHubIssue.encode("\n"))
    }

    @Test
    fun `non-ascii goes out as utf-8 bytes`() {
        // The mount trim line carries a degree sign and the app's own name is
        // full of middots; a body that mangles them is a body nobody can read.
        assertEquals("91.0%C2%B0", GitHubIssue.encode("91.0°"))
        assertEquals("%E2%80%A6", GitHubIssue.encode("…"))
    }

    @Test
    fun `an ampersand in the operator's text cannot inject a second parameter`() {
        val url = GitHubIssue.urlFor("a&labels=bug", "b")
        assertEquals(1, url.count { it == '&' })
    }

    // ── the title ──────────────────────────────────────────────────────────

    @Test
    fun `the title is the first non-blank line of what he typed`() {
        assertEquals(
            "The scan button does nothing",
            GitHubIssue.titleFor("\n  The scan button does nothing\nthen it shook.", "0.9.14"),
        )
    }

    @Test
    fun `an empty message falls back to a title that still says which build`() {
        assertEquals("Feedback from Ollidar 0.9.14", GitHubIssue.titleFor("   \n ", "0.9.14"))
        assertEquals("Feedback from Ollidar 0.9.14", GitHubIssue.defaultTitle("0.9.14"))
    }

    @Test
    fun `a long first line is clamped with an ellipsis rather than by GitHub`() {
        val title = GitHubIssue.titleFor("x".repeat(400), "0.9.14")
        assertEquals(GitHubIssue.MAX_TITLE_CHARS, title.length)
        assertTrue(title.endsWith("…"))
    }

    // ── the bodies ─────────────────────────────────────────────────────────

    @Test
    fun `the feedback body carries his words, the device table and the attach hint`() {
        val body = GitHubIssue.feedbackBody("It stopped after ten seconds.", facts)
        assertTrue(body.startsWith("It stopped after ten seconds."))
        assertTrue(body.contains("| app | 0.9.14 (914) |"))
        assertTrue(body.contains("| device | Google Pixel 8 Pro |"))
        assertTrue(body.contains("| scan storage | 8.1 GB |"))
        assertTrue(body.contains(GitHubIssue.ATTACH_HINT))
    }

    @Test
    fun `the device table is the same six facts the bundle carries`() {
        val table = facts.asMarkdownTable()
        for (line in facts.asText().trim().lines()) {
            val key = line.substringBefore('=')
            val value = line.substringAfter('=')
            // `engineAbi` is spelled `engine ABI` for a human and the byte count
            // is spelled `8.1 GB`; every other value appears verbatim.
            if (key == "scanStorageBytes") continue
            assertTrue("$key missing from the table", table.contains(value))
        }
        assertTrue(table.contains("| engine ABI | 12 |"))
    }

    @Test
    fun `the diagnostics body names the zip and admits the app cannot attach it`() {
        val body = GitHubIssue.diagnosticsBody(facts, "lidarscan-logs-2026-08-22-1300.zip")
        assertTrue(body.contains("Downloads/LidarScan/lidarscan-logs-2026-08-22-1300.zip"))
        assertTrue(body.contains("A link cannot carry a file"))
        assertTrue(body.contains("| app | 0.9.14 (914) |"))
    }

    @Test
    fun `a diagnostics body with no zip still says where to look`() {
        val body = GitHubIssue.diagnosticsBody(facts, null)
        assertTrue(body.contains("Downloads/LidarScan"))
    }

    // ── the clamp ──────────────────────────────────────────────────────────

    @Test
    fun `a short report is not clamped and says nothing about being clamped`() {
        val url = GitHubIssue.feedbackUrl("One line.", facts)
        assertTrue(url.length < GitHubIssue.MAX_URL_CHARS)
        assertFalse(url.contains(GitHubIssue.encode(GitHubIssue.TRUNCATION_NOTE)))
    }

    @Test
    fun `a report longer than GitHub accepts is clamped by us, not by GitHub`() {
        val url = GitHubIssue.feedbackUrl("word ".repeat(4_000), facts)
        assertTrue("url was ${url.length}", url.length <= GitHubIssue.MAX_URL_CHARS)
    }

    @Test
    fun `a clamped body says it was cut`() {
        val url = GitHubIssue.feedbackUrl("word ".repeat(4_000), facts)
        assertTrue(url.contains(GitHubIssue.encode(GitHubIssue.TRUNCATION_NOTE)))
    }

    @Test
    fun `the clamp never cuts inside a percent escape`() {
        // Every `%` in a well-formed URL is followed by two hex digits. A cut
        // that lands inside one produces a URL the browser rejects outright.
        val url = GitHubIssue.feedbackUrl("…".repeat(6_000), facts)
        assertTrue(url.length <= GitHubIssue.MAX_URL_CHARS)
        var i = url.indexOf('?')
        while (i < url.length) {
            if (url[i] == '%') {
                assertTrue("truncated escape at $i in $url", i + 2 < url.length)
                assertTrue(url[i + 1].isHex() && url[i + 2].isHex())
            }
            i++
        }
    }

    @Test
    fun `a body of nothing but note still produces a usable url`() {
        val url = GitHubIssue.urlFor("t", "")
        assertTrue(url.endsWith("&body="))
    }

    private fun Char.isHex(): Boolean = this in '0'..'9' || this in 'A'..'F'
}
