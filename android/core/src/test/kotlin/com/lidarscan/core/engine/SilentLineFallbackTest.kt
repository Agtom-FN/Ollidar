package com.lidarscan.core.engine

import com.lidarscan.core.model.SensorType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 32 item 178(b)/(c)/(a) — **the silent line, decided on a bare JVM.**
 *
 * Every number here is calibrated against the owner's 2026-08-22 retest, and
 * the retest's own counters are used as the fixtures rather than round figures:
 * a threshold that is only tested against 0 and 1 000 000 has not been tested
 * against the thing it exists to classify.
 */
class SilentLineFallbackTest {

    // ── the owner's actual line, as numbers ────────────────────────────────

    /** The STL-27L probe window from the 19:17 sweep: `bytes=16 542c=0 packets=0`. */
    private val ownersProbeWindow = 16L

    /**
     * The two failed captures: 552 bytes in 25 s, 666 bytes in 5 s — plus the
     * 42 bytes the first two-second watchdog tick saw.
     *
     * These are CAPTURE-length figures and are judged against
     * [SilentLineFallback.QUIET_LINE_BYTES], not against the 750 ms probe
     * window's bar. The two thresholds are different numbers for different
     * durations and mixing them up is exactly the mistake this comment exists
     * to stop.
     */
    private val ownersCaptures = listOf(42L, 552L, 666L)

    @Test
    fun `the owner's line is silent, and a real sensor's never is`() {
        assertTrue("16 bytes in 750 ms must count as silent", SilentLineFallback.isSilent(ownersProbeWindow))
        // The 42-byte tick is a probe-length figure too, and is also silent.
        assertTrue(SilentLineFallback.isSilent(42L))
        // A healthy STL-27L is ~92 kB/s; even a tenth of one probe window of it
        // is three orders of magnitude clear of the bar.
        assertFalse(SilentLineFallback.isSilent(9_200L))
        // And a LOUD line carrying the wrong protocol — round 31's mis-clocked
        // stream — is explicitly NOT silent, because re-clocking a port that is
        // plainly streaming something is the churn this gate exists to avoid.
        assertFalse(SilentLineFallback.isSilent(34_500L))
    }

    /** The fallback rates are the LD family's, in the order the argument gives. */
    @Test
    fun `the fallback ladder is the LD family's rates, slowest first`() {
        assertEquals(listOf(230_400, 460_800), SilentLineFallback.FALLBACK_BAUDS)
        assertFalse(
            "the datasheet rate must not be in its own fallback list",
            SerialLidarBaud.STL27L in SilentLineFallback.FALLBACK_BAUDS,
        )
    }

    // ── the state machine ──────────────────────────────────────────────────

    /** Silent at 921 600, alive at 230 400 — the case the item is for. */
    @Test
    fun `a fallback rate that answers is the rate that is chosen`() {
        val attempts = listOf(
            SilentLineFallback.Attempt(921_600, bytes = 16, headers = 0, packets = 0),
            SilentLineFallback.Attempt(230_400, bytes = 34_104, headers = 712, packets = 709),
        )
        assertEquals(230_400, SilentLineFallback.chooseBaud(attempts, packetsToIdentify = 4))
    }

    /** Nothing answered anywhere: no rate is invented. */
    @Test
    fun `a line that is silent at every rate chooses nothing`() {
        val attempts = SilentLineFallback.FALLBACK_BAUDS.plus(921_600).map {
            SilentLineFallback.Attempt(it, bytes = 20, headers = 0, packets = 0)
        }
        assertNull(SilentLineFallback.chooseBaud(attempts, packetsToIdentify = 4))
    }

    /**
     * Headers without CRC-valid packets never win. This is round 31's gate,
     * re-asserted one axis over: a fallback rate that produces `54 2C`
     * coincidences and no valid packet has not found the sensor.
     */
    @Test
    fun `bare headers at a fallback rate do not adopt it`() {
        val attempts = listOf(
            SilentLineFallback.Attempt(921_600, bytes = 16, headers = 0, packets = 0),
            SilentLineFallback.Attempt(230_400, bytes = 30_000, headers = 9, packets = 3),
        )
        assertNull(SilentLineFallback.chooseBaud(attempts, packetsToIdentify = 4))
        assertEquals(230_400, SilentLineFallback.chooseBaud(attempts, packetsToIdentify = 3))
    }

    /** First past the bar, in the order tried — not the loudest. */
    @Test
    fun `the earliest answering rate wins when two somehow both answer`() {
        val attempts = listOf(
            SilentLineFallback.Attempt(230_400, bytes = 30_000, headers = 600, packets = 600),
            SilentLineFallback.Attempt(460_800, bytes = 60_000, headers = 1_200, packets = 1_200),
        )
        assertEquals(230_400, SilentLineFallback.chooseBaud(attempts, packetsToIdentify = 4))
    }

    // ── the evidence, which is the point of the round ─────────────────────

    @Test
    fun `the evidence line carries every rate that was tried`() {
        val line = SilentLineFallback.evidenceLine(
            listOf(
                SilentLineFallback.Attempt(921_600, 16, 0, 0),
                SilentLineFallback.Attempt(230_400, 34_104, 712, 709),
            ),
        )
        assertEquals("921600:bytes=16,542c=0,packets=0 230400:bytes=34104,542c=712,packets=709", line)
    }

    /** A session at the documented rate says nothing; one that is not, says so. */
    @Test
    fun `only a non-standard rate gets a session line`() {
        assertNull(SilentLineFallback.nonStandardBaudLine(SerialLidarBaud.STL27L))
        assertEquals(
            "STL-27L at 230400 (non-standard)",
            SilentLineFallback.nonStandardBaudLine(230_400),
        )
    }

    // ── item 178(c): which message the operator gets ──────────────────────

    /**
     * The message-selection boundary, as the ViewModel applies it: below
     * [SilentLineFallback.QUIET_LINE_BYTES] the banner asks whether the sensor
     * is spinning; above it, the old "the data is not the sensor's" answer is
     * still the right one.
     *
     * Pinned here because the owner's 552 bytes fell on the wrong side of a
     * boundary that did not exist, and was told the baud was wrong while
     * holding a spinning sensor.
     */
    @Test
    fun `the owner's failed captures select the silent-line message`() {
        for (bytes in ownersCaptures) {
            assertTrue("$bytes must ask about spinning", bytes < SilentLineFallback.QUIET_LINE_BYTES)
        }
        // One second of a real COIN-D6 (~24 kB/s) is far past the boundary, so
        // a genuine wrong-protocol stream still gets the wrong-protocol answer.
        assertFalse(24_000L < SilentLineFallback.QUIET_LINE_BYTES)
        // The capture bar is looser than the probe bar, because a capture
        // listens for seconds and a probe for 750 ms.
        assertTrue(SilentLineFallback.QUIET_LINE_BYTES > SilentLineFallback.SILENT_LINE_BYTES)
    }

    // ── item 178(a): the modem lines ──────────────────────────────────────

    /**
     * **The COIN-D6 must not move.** This is the regression guard on the one
     * sensor with a hundred field captures behind it: its lines are the ones it
     * has always had, and a future round that decides to unify the two states
     * has to come here and say so.
     */
    @Test
    fun `the D6 keeps both lines low and the STL-27L asserts both`() {
        val d6 = SerialModemLines.forSensorOrNull(SensorType.COIN_D6)!!
        assertFalse("the D6's DTR is the state every capture in captures/ was taken with", d6.dtr)
        assertFalse(d6.rts)

        val stl = SerialModemLines.forSensorOrNull(SensorType.STL27L)!!
        assertTrue("the CH340 dev-kit board gates on DTR", stl.dtr)
        assertTrue("…and on RTS", stl.rts)
    }

    /** The Mid-360 is not a serial device and must not be given a serial answer. */
    @Test
    fun `a non-serial sensor has no modem lines`() {
        assertNull(SerialModemLines.forSensorOrNull(SensorType.MID360))
        assertNull(SerialLidarBaud.forSensorOrNull(SensorType.MID360))
    }

    /**
     * ROUND 32 item 178(c) — the two operator-facing sentences, under the law.
     *
     * The Scan tab is a **tab-bar** screen and gets no exemption (round 28 item
     * 169). Asserted here rather than eyeballed for the reason that item
     * exists — and because the sentence being replaced was a 30-word paragraph
     * that sent a man to check a setting while the answer was in his hand.
     */
    @Test
    fun `the silent-line message obeys the wording law`() {
        val screen = com.lidarscan.core.WordingLaw.TabBarScreen.SCAN
        assertTrue(
            com.lidarscan.core.Wording.SILENT_LINE_TITLE,
            com.lidarscan.core.WordingLaw.passes(com.lidarscan.core.Wording.SILENT_LINE_TITLE, screen),
        )
        assertTrue(
            com.lidarscan.core.Wording.SILENT_LINE_DETAIL,
            com.lidarscan.core.WordingLaw.passes(com.lidarscan.core.Wording.SILENT_LINE_DETAIL, screen),
        )
        assertEquals(6, com.lidarscan.core.WordingLaw.wordCount(com.lidarscan.core.Wording.SILENT_LINE_TITLE))
        assertEquals(8, com.lidarscan.core.WordingLaw.wordCount(com.lidarscan.core.Wording.SILENT_LINE_DETAIL))
        // The law's third clause: an error says what happened AND what to do.
        // Both branches of the question have to lead somewhere.
        assertTrue(com.lidarscan.core.Wording.SILENT_LINE_DETAIL.contains("cable"))
        assertTrue(com.lidarscan.core.Wording.SILENT_LINE_DETAIL.contains("power"))
    }

    /** The log fragment, because it is what a still-failing retest will be read from. */
    @Test
    fun `the modem-line log fragment is greppable`() {
        assertEquals("dtr=1 rts=1", SerialModemLines.STL27L.log)
        assertEquals("dtr=0 rts=0", SerialModemLines.COIN_D6.log)
    }
}
