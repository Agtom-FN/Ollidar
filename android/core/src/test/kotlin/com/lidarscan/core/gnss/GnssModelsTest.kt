package com.lidarscan.core.gnss

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B9 — the fix ladder, the §3.4 capture gate and the NTRIP form's validation.
 * All three are pure logic and all three are where a wrong answer is invisible
 * on a device (a gate that never fires; a caster that silently never connects).
 */
class GnssModelsTest {

    @Test
    fun `the fix ladder is ordered, and is not the GGA quality digit`() {
        // scanengine_c.h: "ORDERED, and the order is load-bearing... This is
        // NOT the GGA quality digit (which numbers RTK-fixed 4 and RTK-float 5)".
        assertEquals(listOf(0, 1, 2, 3, 4), FixType.entries.map { it.code })
        assertTrue(FixType.RTK_FIXED.code > FixType.RTK_FLOAT.code)
        assertTrue(FixType.RTK_FLOAT.atLeast(FixType.DGPS))
        assertFalse(FixType.DGPS.atLeast(FixType.RTK_FLOAT))
        assertTrue(FixType.RTK_FIXED.atLeast(FixType.RTK_FIXED))
    }

    @Test
    fun `unknown fix codes degrade to none rather than throwing`() {
        assertEquals(FixType.NONE, FixType.fromCode(99))
        assertEquals(FixType.NONE, FixType.fromCode(-1))
        assertEquals(NtripState.IDLE, NtripState.fromCode(42))
    }

    @Test
    fun `the accuracy readout says whether the sigma was measured or assumed`() {
        val measured = GnssFixSnapshot(fix = FixType.RTK_FIXED, hasFix = true, sigmaHorizontalM = 0.02f, sigmaFromGst = true)
        assertTrue(measured.accuracyText().contains("measured"))
        assertTrue(measured.accuracyText().contains("GST"))

        val assumed = measured.copy(sigmaFromGst = false)
        assertTrue(assumed.accuracyText().contains("estimated from fix type"))

        val none = GnssFixSnapshot()
        assertTrue(none.accuracyText().contains("No accuracy"))
    }

    @Test
    fun `the fix timeline reports the fraction at or above a gate`() {
        val s = GnssStatsSnapshot(byFix = longArrayOf(0, 10, 0, 30, 60))
        assertEquals(0.9, s.fractionAtLeast(FixType.RTK_FLOAT), 1e-9)
        assertEquals(0.6, s.fractionAtLeast(FixType.RTK_FIXED), 1e-9)
        assertEquals(1.0, s.fractionAtLeast(FixType.SINGLE), 1e-9)
        assertEquals(0.0, GnssStatsSnapshot().fractionAtLeast(FixType.SINGLE), 1e-9)
    }

    @Test
    fun `an enforcing profile blocks below its gate and a permissive one warns`() {
        val blocked = evaluateCaptureGate(FixType.SINGLE, FixType.RTK_FLOAT, enforce = true, rtkIsTrajectorySource = false)
        assertEquals(CaptureGateVerdict.BLOCK, blocked.verdict)
        assertTrue(blocked.blocksCapture)

        val warned = evaluateCaptureGate(FixType.SINGLE, FixType.RTK_FLOAT, enforce = false, rtkIsTrajectorySource = false)
        assertEquals(CaptureGateVerdict.WARN, warned.verdict)
        assertFalse(warned.blocksCapture)

        val ok = evaluateCaptureGate(FixType.RTK_FIXED, FixType.RTK_FLOAT, enforce = true, rtkIsTrajectorySource = false)
        assertEquals(CaptureGateVerdict.OK, ok.verdict)
    }

    @Test
    fun `no fix always blocks when RTK is the only trajectory source`() {
        // §3.4's D6-outdoor case: with no pose stream the assembler resolves
        // nothing and every point lands in dropped_no_pose — so this is not
        // "ungeoreferenced", it is "no cloud".
        val g = evaluateCaptureGate(FixType.NONE, FixType.NONE, enforce = false, rtkIsTrajectorySource = true)
        assertEquals(CaptureGateVerdict.BLOCK, g.verdict)
        assertTrue(g.detail.contains("no pose"))
    }

    @Test
    fun `a profile with no gate and no RTK dependency never interferes`() {
        val g = evaluateCaptureGate(FixType.NONE, FixType.NONE, enforce = false, rtkIsTrajectorySource = false)
        assertEquals(CaptureGateVerdict.OK, g.verdict)
    }

    @Test
    fun `NTRIP validation requires a host, a port and a mountpoint`() {
        val empty = NtripSettings()
        val fatal = empty.validate().filter { it.severity == IssueSeverity.FATAL }
        assertEquals(setOf("Caster host", "Mountpoint"), fatal.map { it.field }.toSet())
        assertFalse(empty.isConnectable)

        val ok = NtripSettings(host = "rtk2go.com", port = 2101, mountpoint = "TESTBASE")
        assertTrue(ok.isConnectable)
    }

    @Test
    fun `a URL pasted into the host field is refused with the reason`() {
        // The commonest paste: the whole caster URL including the mountpoint.
        val issues = NtripSettings(host = "http://rtk2go.com:2101/MOUNT", mountpoint = "M").validate()
        val host = issues.first { it.field == "Caster host" }
        assertEquals(IssueSeverity.FATAL, host.severity)
        assertTrue(host.message.contains("URL"))
    }

    @Test
    fun `an out-of-range port is fatal`() {
        assertTrue(
            NtripSettings(host = "h", mountpoint = "m", port = 0).validate()
                .any { it.field == "Port" && it.severity == IssueSeverity.FATAL },
        )
        assertTrue(
            NtripSettings(host = "h", mountpoint = "m", port = 70000).validate()
                .any { it.field == "Port" && it.severity == IssueSeverity.FATAL },
        )
    }

    @Test
    fun `a sub-second GGA interval is a warning, not a block`() {
        val s = NtripSettings(host = "h", mountpoint = "m", ggaIntervalMs = 200)
        assertTrue(s.isConnectable)
        assertTrue(s.validate().any { it.severity == IssueSeverity.WARNING && it.field == "GGA interval" })
    }

    @Test
    fun `corrections age reads unknown rather than fresh before the first frame`() {
        // A10 §3: age is -1, not 0, before the first CRC-valid frame —
        // "unknown" and "fresh" are different claims and only one is reassuring.
        assertTrue(NtripStatsSnapshot().ageText().contains("no corrections yet"))
        assertTrue(NtripStatsSnapshot(correctionAgeS = 1.5f).ageText().contains("1.5"))
    }

    @Test
    fun `a georef record names its CRS or says it has none`() {
        val local = GeorefRecord(
            converged = false, epsg = 0, yawDeg = 0.0, globalFromLocal = DoubleArray(16),
            enuOriginLatDeg = 0.0, enuOriginLonDeg = 0.0, enuOriginHeightM = 0.0,
            horizontalSigmaM = 0.0, verticalSigmaM = 0.0, cep95M = 0.0, samples = 0, inliers = 0,
            residualRmsM = 0.0, spanM = 0.0, dominantFix = FixType.NONE, blocker = "trajectory too short",
        )
        assertTrue(local.epsgText.contains("local frame"))
        assertEquals("EPSG:32650", local.copy(epsg = 32650).epsgText)
    }
}
