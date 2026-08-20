package com.lidarscan.core.feedback

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 24 item 109 — **the delivery decision, and the bytes that carry it.**
 *
 * The feature ships before its endpoint exists, which makes the routing rule
 * the part most likely to be wrong for the longest time: nobody will notice a
 * mis-built multipart body until the day a server is finally there to reject
 * it. So the multipart body is byte-asserted here, on a bare JVM, today.
 */
class FeedbackTest {

    // ── which path a bundle takes ──────────────────────────────────────────

    @Test
    fun `no server configured means the share sheet`() {
        assertEquals(FeedbackRoute.SHARE, FeedbackConfig().route)
        assertNull(FeedbackConfig().url())
    }

    /**
     * A URL with no token is a request that is going to 401. Offering "Send"
     * for it would be the app promising something it already knows will fail —
     * the share sheet is the honest answer.
     */
    @Test
    fun `a URL without a token is not a configured server`() {
        assertEquals(FeedbackRoute.SHARE, FeedbackConfig(serverUrl = "https://h", token = "").route)
        assertEquals(FeedbackRoute.SHARE, FeedbackConfig(serverUrl = "", token = "t").route)
        assertEquals(FeedbackRoute.SHARE, FeedbackConfig(serverUrl = "  ", token = "  ").route)
    }

    @Test
    fun `both configured means a server POST`() {
        val config = FeedbackConfig(serverUrl = "https://cloud.example.com", token = "t")
        assertEquals(FeedbackRoute.SERVER, config.route)
        assertEquals("https://cloud.example.com/feedback", config.url())
    }

    /**
     * A base URL typed by a human ends in a slash about half the time, and
     * `https://host//feedback` is normalised by some proxies and 404'd by
     * others.
     */
    @Test
    fun `a trailing slash on the base URL does not double up`() {
        assertEquals("https://h/feedback", FeedbackEndpoint.urlFor("https://h/"))
        assertEquals("https://h/feedback", FeedbackEndpoint.urlFor("https://h///"))
        assertEquals("https://h/feedback", FeedbackEndpoint.urlFor("https://h"))
    }

    /** The feedback path must never collide with the job API's own routes. */
    @Test
    fun `feedback has its own path`() {
        assertEquals("/feedback", FeedbackEndpoint.PATH)
        assertFalse(FeedbackEndpoint.PATH.contains("job"))
    }

    // ── the bytes ──────────────────────────────────────────────────────────

    @Test
    fun `the multipart body uses CRLF and terminates its final boundary`() {
        val boundary = Multipart.boundary(0x2a)
        val body = Multipart.body(
            boundary = boundary,
            fields = mapOf(FeedbackEndpoint.MESSAGE_FIELD to "it broke"),
            fileField = FeedbackEndpoint.FILE_FIELD,
            fileName = "logs.zip",
            fileMime = "application/zip",
            fileBytes = byteArrayOf(1, 2, 3),
        )
        val text = body.toString(Charsets.ISO_8859_1)

        assertTrue("line endings must be CRLF", text.contains("\r\n"))
        assertFalse("no bare LF may appear", text.replace("\r\n", "").contains("\n"))
        assertTrue(text.startsWith("--$boundary\r\n"))
        assertTrue("the closing boundary needs its trailing dashes", text.endsWith("--$boundary--\r\n"))
        assertTrue(text.contains("name=\"${FeedbackEndpoint.MESSAGE_FIELD}\""))
        assertTrue(text.contains("filename=\"logs.zip\""))
        assertTrue(text.contains("Content-Type: application/zip"))
    }

    /** The archive must arrive as it was written — nothing here re-encodes it. */
    @Test
    fun `the file bytes survive verbatim, including bytes that are not text`() {
        val payload = byteArrayOf(0x50, 0x4B, 0x03, 0x04, -1, 0, 127, -128)
        val body = Multipart.body(
            boundary = "B",
            fields = emptyMap(),
            fileField = "bundle",
            fileName = "b.zip",
            fileMime = "application/zip",
            fileBytes = payload,
        )
        val head = body.toString(Charsets.ISO_8859_1).substringBefore("\r\n\r\n").length + 4
        val extracted = body.copyOfRange(head, head + payload.size)
        assertTrue(payload.contentEquals(extracted))
    }

    @Test
    fun `text parts come before the archive so a server can read them first`() {
        val body = Multipart.body(
            boundary = "B",
            fields = linkedMapOf(
                FeedbackEndpoint.MESSAGE_FIELD to "m",
                FeedbackEndpoint.INFO_FIELD to "i",
            ),
            fileField = "bundle",
            fileName = "b.zip",
            fileMime = "application/zip",
            fileBytes = ByteArray(0),
        ).toString(Charsets.ISO_8859_1)
        assertTrue(body.indexOf("name=\"message\"") < body.indexOf("filename="))
        assertTrue(body.indexOf("name=\"info\"") < body.indexOf("filename="))
    }

    @Test
    fun `the content type carries the boundary`() {
        val boundary = Multipart.boundary(1)
        assertEquals("multipart/form-data; boundary=$boundary", Multipart.contentType(boundary))
    }

    // ── what the operator is told ──────────────────────────────────────────

    /**
     * ROUND 7's rule, applied to a hand-off that genuinely may not complete:
     * the failure names where the file IS, not a retry into a server that is
     * not there.
     */
    @Test
    fun `a failure says where the file went`() {
        val failed = FeedbackResult(
            sent = false,
            route = FeedbackRoute.SHARE,
            downloadsPath = "Downloads/LidarScan/logs.zip",
            failure = "no network",
        )
        assertEquals("Could not send. Saved to Downloads.", FeedbackWording.resultFor(failed))
        assertTrue(WordingLaw.isDetail(FeedbackWording.resultFor(failed)))
        // The second half is the whole point: an error must say what happened
        // AND leave the operator somewhere to go. Here the somewhere is a
        // folder, not a retry — retrying into a server that does not exist is
        // not an action, it is a loop.
        assertTrue(FeedbackWording.NOT_SENT.contains("Downloads"))
        assertTrue("the file is really there", failed.downloadsPath!!.contains("Downloads/LidarScan"))
    }

    @Test
    fun `a success says so and nothing else`() {
        val ok = FeedbackResult(sent = true, route = FeedbackRoute.SERVER, downloadsPath = "d")
        assertEquals("Sent.", FeedbackWording.resultFor(ok))
    }

    @Test
    fun `the note names the path before the tap`() {
        assertEquals("Sends to your server.", FeedbackWording.noteFor(FeedbackRoute.SERVER))
        assertEquals("Opens the share sheet.", FeedbackWording.noteFor(FeedbackRoute.SHARE))
    }

    @Test
    fun `the privacy note is one honest line`() {
        assertEquals("Sends your logs and device info.", FeedbackWording.PRIVACY_NOTE)
        assertTrue(WordingLaw.isInstruction(FeedbackWording.PRIVACY_NOTE))
    }

    @Test
    fun `every feedback string obeys the wording law`() {
        for (line in FeedbackWording.ALL) {
            assertTrue("\"$line\" is ${WordingLaw.wordCount(line)} words", WordingLaw.isInstruction(line))
        }
        for (line in FeedbackWording.DETAILS) {
            assertTrue("\"$line\" is ${WordingLaw.wordCount(line)} words", WordingLaw.isDetail(line))
        }
        for (line in FeedbackWording.ALL + FeedbackWording.DETAILS) {
            assertTrue("jargon in \"$line\"", WordingLaw.jargonIn(line).isEmpty())
        }
    }

    // ── the device summary ─────────────────────────────────────────────────

    /**
     * The whole privacy claim, as a test: the bundle's summary is six known
     * lines and there is no identifier among them.
     */
    @Test
    fun `the device summary carries facts and no identifiers`() {
        val text = DeviceFacts(
            appVersion = "0.9.9",
            versionCode = 909,
            deviceModel = "Pixel 8",
            androidVersion = "15",
            engineAbi = 12,
            scanCount = 7,
            storageBytes = 1_500_000_000L,
        ).asText()
        assertEquals(6, text.trim().lines().size)
        assertTrue(text.contains("app=0.9.9 (909)"))
        assertTrue(text.contains("device=Pixel 8"))
        assertTrue(text.contains("engineAbi=12"))
        for (forbidden in listOf("imei", "androidId", "serial", "lat", "lon", "email", "account")) {
            assertFalse("$forbidden must not be in the bundle", text.lowercase().contains(forbidden))
        }
    }

    @Test
    fun `storage reads in units a person uses`() {
        assertEquals("1.5 GB", DeviceFacts.formatBytes(1_500_000_000L))
        assertEquals("840 MB", DeviceFacts.formatBytes(840_000_000L))
        assertEquals("12 KB", DeviceFacts.formatBytes(12_000L))
        assertEquals("0 B", DeviceFacts.formatBytes(0L))
    }
}
