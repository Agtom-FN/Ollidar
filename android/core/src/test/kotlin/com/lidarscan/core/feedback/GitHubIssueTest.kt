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

    // ── ROUND 35 item 186: the log goes in the issue, as text ─────────────

    /** A synthetic session: chatty, with the three interesting lines buried. */
    private fun syntheticLog(chatter: Int = 4_000): String = buildString {
        appendLine("2026-08-23 09:00:00.001 [session] START sensor=COIN_D6")
        appendLine("2026-08-23 09:00:01.002 [crash] java.lang.IllegalStateException: boom")
        appendLine("2026-08-23 09:00:02.003 [net-debug] sweep 192.168.1.50 udp/2368 silent")
        appendLine(
            "2026-08-23 09:00:03.004 [session] NO DATA after 2003ms: bytesIn=0 " +
                "packetsOk=0 state=RECORDING sensor=COIN_D6",
        )
        repeat(chatter) {
            appendLine("2026-08-23 09:0$it [ar] pose t=$it x=0.123 y=0.456 z=0.789 conf=0.91")
        }
    }

    @Test
    fun `the diagnostics body carries the log in a code block and says how much`() {
        val log = "line one\nline two\nline three\n"
        val excerpt = GitHubIssue.logExcerpt(log, 10_000)
        val body = GitHubIssue.diagnosticsBody(facts, excerpt)
        assertTrue(body.contains("| app | 0.9.14 (914) |"))
        assertTrue(body.contains(GitHubIssue.LOG_FENCE_OPEN))
        assertTrue(body.contains("line one"))
        assertTrue(body.contains("line three"))
        assertTrue(body.contains("Last 3 lines of the log"))
        // The block CLOSES. A closing fence may not carry an info string —
        // "```text" at the bottom opens a second block instead of ending the
        // first, and the whole log renders inside a block that never ends.
        assertEquals(GitHubIssue.LOG_FENCE_CLOSE, body.trimEnd().lines().last())
        assertEquals(1, body.lines().count { it == GitHubIssue.LOG_FENCE_OPEN })
        assertEquals(1, body.lines().count { it == GitHubIssue.LOG_FENCE_CLOSE })
        assertTrue(body.contains("Save to phone"))
        // The zip is gone from this route and so is every sentence about it.
        assertFalse(body.contains("Downloads"))
        assertFalse(body.contains("A link cannot carry a file"))
    }

    @Test
    fun `a device with no log says so rather than printing an empty block`() {
        val body = GitHubIssue.diagnosticsBody(facts, GitHubIssue.logExcerpt("", 10_000))
        assertTrue(body.contains(GitHubIssue.NO_LOG_NOTE))
        assertFalse(body.contains(GitHubIssue.LOG_FENCE_OPEN))
    }

    /** **(b) the budget, and the line boundary.** Nothing is ever cut mid-line. */
    @Test
    fun `the excerpt spends its budget in whole lines and never splits one`() {
        val log = (1..200).joinToString("\n") { "2026-08-23 09:00:00.00$it [ar] pose number $it" }
        val whole = log.lines()
        for (budget in listOf(60, 200, 1_000, 4_000)) {
            val excerpt = GitHubIssue.logExcerpt(log, budget)
            val real = excerpt.lines.filterNot { it.startsWith(GitHubIssue.GAP_PREFIX) }
            for (line in real) {
                assertTrue("\"$line\" is not a whole line of the log", whole.contains(line))
            }
            val spent = excerpt.lines.sumOf { it.length + 1 }
            assertTrue("budget $budget overspent by ${spent - budget}", spent <= budget + 80)
            assertEquals(real.size, excerpt.kept)
            assertEquals(200, excerpt.total)
        }
    }

    /** …and it is a TAIL: what it keeps is the newest, not the oldest. */
    @Test
    fun `a plain tail keeps the most recent lines`() {
        val log = (1..200).joinToString("\n") { "plain line $it" }
        val excerpt = GitHubIssue.logExcerpt(log, 300)
        assertTrue(excerpt.kept in 15..25)
        assertTrue(excerpt.isContiguous)
        assertEquals("plain line 200", excerpt.lines.last())
    }

    /**
     * **(b2) the tag priority.** A naive tail of a chatty session loses the
     * crash, the sweep and the stall — which are the only three lines anybody
     * opens the issue to read.
     */
    @Test
    fun `the crash, the sweep and the stall survive a chatty session`() {
        val log = syntheticLog()
        val excerpt = GitHubIssue.logExcerpt(log, 3_000)
        val text = excerpt.lines.joinToString("\n")
        assertTrue("the crash was pushed out", text.contains("[crash] java.lang.IllegalStateException"))
        assertTrue("the sweep was pushed out", text.contains("[net-debug] sweep"))
        assertTrue("the stall was pushed out", text.contains("NO DATA after 2003ms"))
        // A naive tail of the same budget would have kept none of them.
        val naive = log.lines().takeLast(excerpt.kept).joinToString("\n")
        assertFalse(naive.contains("[crash]"))
        // The jump is admitted rather than hidden.
        assertFalse(excerpt.isContiguous)
        assertTrue(text.contains(GitHubIssue.GAP_PREFIX))
        assertTrue(GitHubIssue.logExcerptHeading(excerpt).contains("Gaps are marked"))
        // …and the priority pass never eats more than half the budget, so the
        // context around them survives too.
        assertTrue("the tail was starved", text.contains("[ar] pose"))
    }

    /** **(d) the ceiling holds** against a log far larger than the URL can be. */
    @Test
    fun `a huge log still produces a URL under the ceiling`() {
        val url = GitHubIssue.diagnosticsUrl(facts, syntheticLog(chatter = 40_000))
        assertTrue("url was ${url.length}", url.length <= GitHubIssue.MAX_URL_CHARS)
        assertTrue(url.startsWith("https://github.com/Agtom-FN/Ollidar/issues/new?title="))
        val body = java.net.URLDecoder.decode(url.substringAfter("&body="), "UTF-8")
        assertTrue(body.contains(GitHubIssue.LOG_FENCE_OPEN))
        assertTrue(body.contains("[crash] java.lang.IllegalStateException"))
        assertTrue(body.contains("| app | 0.9.14 (914) |"))
    }

    /** A short log is carried whole, and the URL is nowhere near the ceiling. */
    @Test
    fun `a short log goes in whole`() {
        val log = (1..12).joinToString("\n") { "2026-08-23 09:00:00.0$it [store] short $it" }
        val url = GitHubIssue.diagnosticsUrl(facts, log)
        assertTrue(url.length < GitHubIssue.MAX_URL_CHARS)
        val body = java.net.URLDecoder.decode(url.substringAfter("&body="), "UTF-8")
        for (line in log.lines()) assertTrue(body.contains(line))
        assertTrue(body.contains("Last 12 lines of the log (of 12)"))
    }

    /** An empty log still opens an issue — the device table is worth having. */
    @Test
    fun `an empty log still opens an issue with the device table`() {
        val url = GitHubIssue.diagnosticsUrl(facts, "")
        assertTrue(url.length < GitHubIssue.MAX_URL_CHARS)
        val body = java.net.URLDecoder.decode(url.substringAfter("&body="), "UTF-8")
        assertTrue(body.contains(GitHubIssue.NO_LOG_NOTE))
        assertTrue(body.contains("| app | 0.9.14 (914) |"))
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
