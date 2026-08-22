package com.lidarscan.app.ui.settings

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 28 item 164 — **the values in Settings' right-hand column.**
 *
 * The review found two of these wrong by looking at a screenshot: T1 (a raw
 * filesystem path standing where a storage figure belonged) and T4 (a version
 * line nobody could read). A screenshot is the wrong instrument for a string,
 * so every value on the rebuilt page is a pure function and every one of them
 * is pinned here.
 *
 * The last test is the one that would have caught T1 before it shipped: no row
 * value on a **tab-bar screen** may carry jargon, and `WordingLaw` now refuses
 * to exempt Settings ([WordingLaw.TabBarScreen.SETTINGS], round 28 item 169).
 */
class SettingsFormatTest {

    // ── the Storage row (T1) ────────────────────────────────────────────────

    @Test
    fun `storage states the size and the count together`() {
        assertEquals("8.1 GB · 66 scans", SettingsFormat.storageLine(8_100_000_000L, 66))
    }

    @Test
    fun `one scan is not one scans`() {
        assertEquals("12 MB · 1 scan", SettingsFormat.storageLine(12_000_000L, 1))
    }

    /**
     * A fresh install has an empty projects directory, and "0 B · 0 scans" is
     * three values none of which is the answer to "is anything here yet".
     */
    @Test
    fun `an empty device says so in words`() {
        assertEquals("No scans yet", SettingsFormat.storageLine(0L, 0))
    }

    // ── the Version row (T4) ────────────────────────────────────────────────

    @Test
    fun `version is the name and the build code`() {
        assertEquals("0.9.13 (913)", SettingsFormat.versionLine("0.9.13", 913, developerMode = false))
    }

    /**
     * The developer state is part of the VALUE, not a second row: it is the
     * only indicator of an unlocked device visible without scrolling to the
     * section it unlocked.
     */
    @Test
    fun `an unlocked device says dev on the version row`() {
        assertEquals("0.9.13 (913) · dev", SettingsFormat.versionLine("0.9.13", 913, developerMode = true))
    }

    // ── the Mount row ───────────────────────────────────────────────────────

    @Test
    fun `an unmeasured mount is not set, not an error`() {
        assertEquals("Not set", SettingsFormat.mountLine(null))
    }

    @Test
    fun `a measured mount states its rotation to a tenth`() {
        assertEquals("Set · 91.0°", SettingsFormat.mountLine(91.04))
    }

    // ── the developer rows ──────────────────────────────────────────────────

    @Test
    fun `sensor timing names the sensor and the delay`() {
        assertEquals("D6 · 12 ms", SettingsFormat.sensorTimingLine(12))
    }

    @Test
    fun `engine states both which library loaded and which one is running`() {
        assertEquals("native · ABI 12", SettingsFormat.engineLine(true, useFakeEngine = false, abi = 12))
        assertEquals("simulated · ABI 12 available", SettingsFormat.engineLine(true, useFakeEngine = true, abi = 12))
        assertEquals("simulated · no native library", SettingsFormat.engineLine(false, useFakeEngine = false, abi = 0))
    }

    @Test
    fun `a log with nothing in it says empty rather than zero bytes`() {
        assertEquals("empty", SettingsFormat.captureLogLine(0L))
        assertEquals("2 MB", SettingsFormat.captureLogLine(2_100_000L))
    }

    // ── the Empty-scans row ─────────────────────────────────────────────────

    @Test
    fun `nothing to clean up is a word, not a zero`() {
        assertEquals("None", SettingsFormat.emptyScanLine(0))
        assertEquals("1 scan", SettingsFormat.emptyScanLine(1))
        assertEquals("3 scans", SettingsFormat.emptyScanLine(3))
    }

    // ── the Cloud row ───────────────────────────────────────────────────────

    /**
     * The row shows the host, not the URL: `https://` and a trailing slash are
     * eleven characters of nothing on a 56 dp row, and the full value is one
     * tap away in the sheet that edits it.
     */
    @Test
    fun `cloud shows the host or that there is none`() {
        assertEquals("Not set", SettingsFormat.cloudLine(""))
        assertEquals("cloud.example.com", SettingsFormat.cloudLine("https://cloud.example.com/"))
        assertEquals("10.0.2.2:8080", SettingsFormat.cloudLine("http://10.0.2.2:8080"))
    }

    // ── T1, as a law rather than as a screenshot ────────────────────────────

    /**
     * **This is the T1 regression test.** Settings is a primary tab, so round
     * 28 item 169 gives it no jargon exemption and a twelve-word ceiling. Every
     * value the page can put in a row is checked against that — which the old
     * `/storage/emulated/0/Android/data/com.lidarscan.app.debug/files/Projects`
     * card would not have survived, and which is why the path now lives behind
     * seven taps instead.
     */
    @Test
    fun `every Settings row value passes the tab-bar wording law`() {
        val values = listOf(
            SettingsFormat.storageLine(8_100_000_000L, 66),
            SettingsFormat.storageLine(0L, 0),
            SettingsFormat.versionLine("0.9.13", 913, developerMode = false),
            SettingsFormat.versionLine("0.9.13", 913, developerMode = true),
            SettingsFormat.mountLine(null),
            SettingsFormat.mountLine(91.04),
            SettingsFormat.emptyScanLine(0),
            SettingsFormat.emptyScanLine(3),
            SettingsFormat.cloudLine(""),
            SettingsFormat.cloudLine("https://cloud.example.com/"),
        )
        values.forEach { value ->
            assertTrue(
                "\"$value\": ${WordingLaw.violations(value, WordingLaw.TabBarScreen.SETTINGS)}",
                WordingLaw.passes(value, WordingLaw.TabBarScreen.SETTINGS),
            )
        }
    }
}
