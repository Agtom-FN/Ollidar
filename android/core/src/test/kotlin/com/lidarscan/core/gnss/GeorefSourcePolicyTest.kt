package com.lidarscan.core.gnss

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 5.2: the three paths the owner asked to be covered — rover present,
 * phone fix, permission denied — plus the mid-session upgrade, which is the one
 * behaviour that could plausibly have been built as a state machine and
 * deliberately was not.
 */
class GeorefSourcePolicyTest {

    private val rtkFixed = GnssFixSnapshot(
        fix = FixType.RTK_FIXED,
        satellites = 24,
        sigmaHorizontalM = 0.019f,
        sigmaFromGst = true,
        hasFix = true,
    )
    private val noFix = GnssFixSnapshot(fix = FixType.NONE, hasFix = false)

    private val phoneFix = PhoneFix(
        latDeg = 22.3193,
        lonDeg = 114.1694,
        altitudeM = 12.0,
        accuracyM = 4.2f,
        utcMillis = 1_776_000_000_000L,
    )

    // ── rover present ───────────────────────────────────────────────────────

    @Test
    fun `a rover fix wins and quotes its own measured sigma`() {
        val s = GeorefSourcePolicy.resolve(rtkFixed, phoneFix, sessionActive = true, permissionDenied = false)
        assertEquals(GeorefSource.RTK_ROVER, s.source)
        assertTrue(s.isRtk)
        assertFalse(s.isPhoneFallback)
        assertEquals("RTK FIXED ±2 cm", s.chipLabel)
        assertEquals(0.019f, s.accuracyM!!, 1e-6f)
    }

    @Test
    fun `the phone fallback is not armed while a rover has a fix`() {
        assertFalse(
            GeorefSourcePolicy.shouldRunPhoneFallback(rtkFixed, sessionActive = true, permissionDenied = false),
        )
    }

    // ── phone fix ───────────────────────────────────────────────────────────

    @Test
    fun `no rover falls back to the phone and says so in metres`() {
        val s = GeorefSourcePolicy.resolve(noFix, phoneFix, sessionActive = true, permissionDenied = false)
        assertEquals(GeorefSource.PHONE_GPS, s.source)
        assertTrue(s.isPhoneFallback)
        assertEquals("PHONE GPS ±4.2 m", s.chipLabel)
        assertEquals(4.2f, s.accuracyM!!, 1e-6f)
    }

    @Test
    fun `a null rover snapshot is the same as no fix`() {
        val s = GeorefSourcePolicy.resolve(null, phoneFix, sessionActive = true, permissionDenied = false)
        assertEquals(GeorefSource.PHONE_GPS, s.source)
    }

    @Test
    fun `the fallback is armed only during a session`() {
        assertTrue(GeorefSourcePolicy.shouldRunPhoneFallback(noFix, sessionActive = true, permissionDenied = false))
        // Not while merely previewing: the permission prompt must land when a
        // capture actually starts, not when the Capture tab is opened.
        assertFalse(GeorefSourcePolicy.shouldRunPhoneFallback(noFix, sessionActive = false, permissionDenied = false))
    }

    @Test
    fun `an unknown phone accuracy is reported as unknown rather than as zero`() {
        val vague = phoneFix.copy(accuracyM = 0f)
        val s = GeorefSourcePolicy.resolve(noFix, vague, sessionActive = true, permissionDenied = false)
        assertEquals("PHONE GPS · accuracy unknown", s.chipLabel)
        assertNull(s.accuracyM)
    }

    @Test
    fun `waiting for the first phone fix says so`() {
        val s = GeorefSourcePolicy.resolve(noFix, null, sessionActive = true, permissionDenied = false)
        assertEquals(GeorefSource.NONE, s.source)
        assertEquals("WAITING FOR GPS", s.chipLabel)
    }

    // ── permission denied ───────────────────────────────────────────────────

    @Test
    fun `a denied permission stops the fallback but never the capture`() {
        assertFalse(GeorefSourcePolicy.shouldRunPhoneFallback(noFix, sessionActive = true, permissionDenied = true))
        val s = GeorefSourcePolicy.resolve(noFix, null, sessionActive = true, permissionDenied = true)
        assertEquals(GeorefSource.NONE, s.source)
        assertEquals("NO GEOREF · location off", s.chipLabel)
        assertTrue(s.permissionDenied)
        assertTrue(GeorefSourcePolicy.PERMISSION_DENIED_NOTE.contains("local frame"))
    }

    @Test
    fun `a denied permission does not hide a rover that is present`() {
        val s = GeorefSourcePolicy.resolve(rtkFixed, null, sessionActive = true, permissionDenied = true)
        assertEquals(GeorefSource.RTK_ROVER, s.source)
        assertTrue(s.permissionDenied)
    }

    // ── mid-session upgrade ─────────────────────────────────────────────────

    @Test
    fun `a rover connecting mid-session upgrades the chip with no intermediate state`() {
        val before = GeorefSourcePolicy.resolve(noFix, phoneFix, sessionActive = true, permissionDenied = false)
        assertEquals(GeorefSource.PHONE_GPS, before.source)

        val after = GeorefSourcePolicy.resolve(rtkFixed, phoneFix, sessionActive = true, permissionDenied = false)
        assertEquals(GeorefSource.RTK_ROVER, after.source)
        assertEquals("RTK FIXED ±2 cm", after.chipLabel)
        // And the fallback disarms itself on the same evaluation.
        assertFalse(
            GeorefSourcePolicy.shouldRunPhoneFallback(rtkFixed, sessionActive = true, permissionDenied = false),
        )
    }

    @Test
    fun `an RTK float rover still outranks the phone`() {
        val float = rtkFixed.copy(fix = FixType.RTK_FLOAT, sigmaHorizontalM = 0.28f)
        val s = GeorefSourcePolicy.resolve(float, phoneFix, sessionActive = true, permissionDenied = false)
        assertEquals(GeorefSource.RTK_ROVER, s.source)
        assertEquals("RTK FLOAT ±28 cm", s.chipLabel)
    }

    @Test
    fun `accuracy formatting switches from cm to m at one metre`() {
        assertEquals("2 cm", GeorefSourcePolicy.formatAccuracy(0.019f))
        assertEquals("99 cm", GeorefSourcePolicy.formatAccuracy(0.99f))
        assertEquals("1.0 m", GeorefSourcePolicy.formatAccuracy(1.0f))
        assertEquals("12.5 m", GeorefSourcePolicy.formatAccuracy(12.5f))
    }
}
