package com.lidarscan.core.gnss

import java.time.ZoneOffset
import java.time.ZonedDateTime
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 5.2: the bytes the phone-location fallback pushes into the engine.
 *
 * These matter more than they look: `scan_engine_push_nmea` is the only GNSS
 * *ingest* door in the C ABI, so a sentence with a bad checksum or a wrong field
 * order does not fail loudly — it is counted as a parse error somewhere inside
 * the engine and the capture is simply not georeferenced. So the checksum, the
 * field layout and (most of all) the sigma fields are asserted here rather than
 * discovered in a field session.
 */
class PhoneFixNmeaTest {

    private val utcMillis =
        ZonedDateTime.of(2026, 8, 17, 19, 32, 7, 250_000_000, ZoneOffset.UTC).toInstant().toEpochMilli()

    private val fix = PhoneFix(
        latDeg = 22.319300,
        lonDeg = 114.169400,
        altitudeM = 12.34,
        accuracyM = 4.0f,
        verticalAccuracyM = 6.0f,
        utcMillis = utcMillis,
        speedMps = 1.5f,
        bearingDeg = 271.4f,
        satellites = 9,
    )

    private fun checksumOk(sentence: String): Boolean {
        val body = sentence.substringAfter('$').substringBefore('*')
        val stated = sentence.substringAfter('*').trim()
        var cs = 0
        for (c in body) cs = cs xor c.code
        return "%02X".format(cs) == stated
    }

    private fun fields(sentence: String): List<String> =
        sentence.substringAfter('$').substringBefore('*').split(',')

    @Test
    fun `every sentence carries a valid NMEA checksum and CRLF`() {
        for (s in listOf(PhoneFixNmea.gga(fix), PhoneFixNmea.gst(fix), PhoneFixNmea.rmc(fix))) {
            assertTrue("bad checksum: $s", checksumOk(s))
            assertTrue("must be CRLF-terminated: $s", s.endsWith("\r\n"))
            assertTrue(s.startsWith("$"))
        }
    }

    @Test
    fun `the burst is GGA then GST then RMC, all sharing one UTC`() {
        val lines = PhoneFixNmea.burst(fix).trim().lines().map { it.trim() }
        assertEquals(3, lines.size)
        assertTrue(lines[0].startsWith("\$GPGGA,"))
        assertTrue(lines[1].startsWith("\$GPGST,"))
        assertTrue(lines[2].startsWith("\$GPRMC,"))
        // Epoch assembly closes on a CHANGED utc, so all three must agree or the
        // GST sigma lands in a different epoch than the GGA it describes.
        val utc = lines.map { fields(it + "*00")[1] }
        assertEquals(1, utc.distinct().size)
        assertEquals("193207.25", utc.first())
    }

    @Test
    fun `GGA reports a single-point fix, never an RTK quality`() {
        val f = fields(PhoneFixNmea.gga(fix))
        assertEquals("1", f[6])
        assertEquals(PhoneFixNmea.GGA_QUALITY_SINGLE, f[6].toInt())
    }

    @Test
    fun `GGA position is degrees-minutes with the right hemispheres`() {
        val f = fields(PhoneFixNmea.gga(fix))
        assertEquals("2219.1580", f[2])
        assertEquals("N", f[3])
        assertEquals("11410.1640", f[4])
        assertEquals("E", f[5])
        assertEquals("09", f[7])
        assertEquals("12.34", f[9])
        assertEquals("M", f[10])
        // Geoid separation stays EMPTY: Android already reports ellipsoidal
        // height, and 0.0 would assert a geoid that is nowhere near true.
        assertEquals("", f[11])
    }

    @Test
    fun `southern and western coordinates flip the hemisphere fields`() {
        val south = fix.copy(latDeg = -33.8688, lonDeg = -70.6693)
        val f = fields(PhoneFixNmea.gga(south))
        assertEquals("S", f[3])
        assertEquals("W", f[5])
        // …and the numbers are the ABSOLUTE value: a negative ddmm.mmmm is not NMEA.
        assertFalse(f[2].contains('-'))
        assertFalse(f[4].contains('-'))
    }

    @Test
    fun `GST carries Android's accuracy as a circular error at one sigma`() {
        val f = fields(PhoneFixNmea.gst(fix))
        assertEquals("4.00", f[2]) // RMS
        assertEquals("4.00", f[3]) // semi-major
        assertEquals("4.00", f[4]) // semi-minor
        assertEquals("0.0", f[5]) // orientation — a circle has none
        // sigma_lat and sigma_lon are accuracy/sqrt(2), so that
        // sqrt(lat^2 + lon^2) == the accuracy Android reported, not 1.41x it.
        assertEquals(2.83, f[6].toDouble(), 0.01)
        assertEquals(2.83, f[7].toDouble(), 0.01)
        val horizontal = Math.hypot(f[6].toDouble(), f[7].toDouble())
        assertEquals(4.0, horizontal, 0.02)
        assertEquals("6.00", f[8]) // vertical, as reported by the platform
    }

    @Test
    fun `an absent vertical accuracy is left empty rather than guessed`() {
        val f = fields(PhoneFixNmea.gst(fix.copy(verticalAccuracyM = null)))
        assertEquals("", f[8])
    }

    @Test
    fun `RMC carries the date and marks the fix valid`() {
        val f = fields(PhoneFixNmea.rmc(fix))
        assertEquals("A", f[2])
        assertEquals("170826", f[9]) // ddmmyy
        assertEquals(2.92, f[7].toDouble(), 0.01) // 1.5 m/s in knots
        assertEquals("271.4", f[8])
    }

    @Test
    fun `an unreported speed or bearing is empty, not zero`() {
        val f = fields(PhoneFixNmea.rmc(fix.copy(speedMps = null, bearingDeg = null)))
        assertEquals("", f[7])
        assertEquals("", f[8])
    }

    @Test
    fun `no satellite count is reported as empty rather than invented`() {
        val f = fields(PhoneFixNmea.gga(fix.copy(satellites = null)))
        assertEquals("", f[7])
    }

    @Test
    fun `a nonsensical accuracy degrades to the worst HDOP rather than the best`() {
        assertEquals(99.9, PhoneFixNmea.hdopFromAccuracy(0f), 1e-6)
        assertEquals(99.9, PhoneFixNmea.hdopFromAccuracy(Float.NaN), 1e-6)
        assertEquals(0.8, PhoneFixNmea.hdopFromAccuracy(4f), 1e-6)
    }

    @Test
    fun `sentences stay inside NMEA's 82-byte line limit`() {
        for (s in PhoneFixNmea.burst(fix).trim().lines()) {
            assertTrue("too long (${s.length}): $s", s.trim().length <= 82)
        }
    }
}
