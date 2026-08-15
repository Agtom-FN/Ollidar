package com.lidarscan.core.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class Mid360SelfTestTest {

    private fun sample(
        elapsedMs: Long,
        pointsOut: Long = 0,
        baseline: Long = 0,
        datagrams: Long = 0,
        deviceState: Int = 2,
        stateLabel: String = "starting",
        imuUnavailable: Boolean = false,
    ) = Mid360SelfTest.Sample(
        elapsedMs = elapsedMs,
        pointsOut = pointsOut,
        baselinePointsOut = baseline,
        pointDatagrams = datagrams,
        pointsPerSec = 39_949.0,
        imuHz = 200.0,
        lossPct = 0.0,
        deviceState = deviceState,
        deviceStateLabel = stateLabel,
        imuUnavailable = imuUnavailable,
    )

    @Test
    fun `the first point passes, at any rate`() {
        // Deliberately ONE point after 1.45 s — A3's measured handshake time.
        // The Mid-360 gate is first-packet, not a rate, because its failure
        // mode is total silence and there is no partial-credit regime.
        val verdict = Mid360SelfTest.evaluate(sample(elapsedMs = 1_450, pointsOut = 1))
        assertTrue(verdict is Mid360SelfTest.Verdict.Passed)
        assertTrue((verdict as Mid360SelfTest.Verdict.Passed).detail.contains("1.45 s"))
    }

    @Test
    fun `the baseline is subtracted so a re-test on a streaming device is honest`() {
        // Re-running the test without a baseline would pass instantly off the
        // previous run's point count.
        val stillTesting = Mid360SelfTest.evaluate(
            sample(elapsedMs = 500, pointsOut = 5_000, baseline = 5_000),
        )
        assertTrue(stillTesting is Mid360SelfTest.Verdict.Testing)

        val passed = Mid360SelfTest.evaluate(
            sample(elapsedMs = 500, pointsOut = 5_001, baseline = 5_000),
        )
        assertTrue(passed is Mid360SelfTest.Verdict.Passed)
    }

    @Test
    fun `stays in Testing for the whole 8 s window and reports progress`() {
        val early = Mid360SelfTest.evaluate(sample(elapsedMs = 0))
        assertTrue(early is Mid360SelfTest.Verdict.Testing)
        assertEquals(0f, (early as Mid360SelfTest.Verdict.Testing).progress, 1e-6f)

        val mid = Mid360SelfTest.evaluate(sample(elapsedMs = 4_000))
        assertTrue(mid is Mid360SelfTest.Verdict.Testing)
        assertEquals(0.5f, (mid as Mid360SelfTest.Verdict.Testing).progress, 1e-6f)

        val late = Mid360SelfTest.evaluate(sample(elapsedMs = 7_999))
        assertTrue(late is Mid360SelfTest.Verdict.Testing)
    }

    @Test
    fun `fails exactly at the window boundary, not before`() {
        assertTrue(Mid360SelfTest.evaluate(sample(elapsedMs = 7_999)) is Mid360SelfTest.Verdict.Testing)
        val failed = Mid360SelfTest.evaluate(sample(elapsedMs = 8_000))
        assertTrue(failed is Mid360SelfTest.Verdict.Failed)
        val detail = (failed as Mid360SelfTest.Verdict.Failed).detail
        assertTrue(detail.contains("no packet within 8 s"))
        assertTrue(detail.contains("starting"))
    }

    @Test
    fun `the window is shorter than the engine's own connect grace, deliberately`() {
        // A3's Mid360ReconnectConfig::connect_timeout_ms is 10 s; the UI stops
        // asking the user to wait at 8. Locking this in so a later edit has to
        // be deliberate.
        assertEquals(8_000L, Mid360SelfTest.WINDOW_MS)
        assertTrue(Mid360SelfTest.WINDOW_MS < 10_000L)
    }

    @Test
    fun `datagrams-but-no-points is diagnosed as a different fault from silence`() {
        val bytesNoPoints = Mid360SelfTest.diagnose(sample(elapsedMs = 8_000, datagrams = 1_200))
        assertTrue(bytesNoPoints.contains("Datagrams ARE arriving"))
        assertTrue(bytesNoPoints.contains("point port"))

        val silence = Mid360SelfTest.diagnose(sample(elapsedMs = 8_000, datagrams = 0))
        assertTrue(silence.contains("No UDP datagram arrived at all"))
        assertTrue(silence.contains("host IP"))
        // Power is the one an operator forgets and cannot deduce from the app.
        assertTrue(silence.contains("9–27 V"))
    }

    @Test
    fun `a driver fault is named as such rather than blamed on the cable`() {
        val d = Mid360SelfTest.diagnose(
            sample(elapsedMs = 8_000, deviceState = Mid360SelfTest.DEVICE_STATE_FAULT),
        )
        assertTrue(d.contains("reported a fault"))
        assertTrue(d.contains("host IP is not an address this phone holds"))
    }

    @Test
    fun `the pre-bound path reports IMU as off, never as zero`() {
        // A "0.00 Hz IMU" readout on a path that structurally cannot carry
        // IMU would read as a broken device.
        val verdict = Mid360SelfTest.evaluate(
            sample(elapsedMs = 1_000, pointsOut = 1, imuUnavailable = true),
        )
        assertTrue(verdict is Mid360SelfTest.Verdict.Passed)
        val passedDetail = (verdict as Mid360SelfTest.Verdict.Passed).detail
        assertTrue(passedDetail.contains("IMU off (pre-bound socket)"))
        assertTrue(!passedDetail.contains("0.00 Hz"))

        val line = Mid360SelfTest.healthLine("Streaming", 39_949.0, 0.0, 0.0, 100, 0, imuUnavailable = true)
        assertTrue(line.contains("IMU n/a"))
    }

    @Test
    fun `the health line quotes loss and its complement, both`() {
        val line = Mid360SelfTest.healthLine("Streaming", 39_949.0, 200.0, 1.8596, 2_397_115, 0)
        assertTrue(line.contains("Streaming"))
        assertTrue(line.contains("200.00 Hz IMU"))
        // A3's tables are written in loss %; desktop C2 shows the inverted
        // "ok rate". Both, so neither reader has to convert in their head.
        assertTrue(line.contains("1.860% loss"))
        assertTrue(line.contains("98.1% ok"))
    }

    @Test
    fun `the reference constants match A3 section 7's soak figures`() {
        assertEquals(200_000.0, Mid360SelfTest.NOMINAL_SENSOR_POINTS_PER_SEC, 0.0)
        assertEquals(40_000.0, Mid360SelfTest.NOMINAL_STORE_POINTS_PER_SEC, 0.0)
        assertEquals(200.0, Mid360SelfTest.NOMINAL_IMU_HZ, 0.0)
        // Mid360Config::max_loss_pct — the level the driver itself calls degraded.
        assertEquals(1.0, Mid360SelfTest.LOSS_DEGRADED_PCT, 0.0)
    }
}
