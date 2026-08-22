@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.core.capture

import com.lidarscan.core.model.SensorType
import kotlinx.coroutines.delay
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Round 5's reduced-step capture flow, as seen by the state machine: detected →
 * connected → live preview with no taps in between. Against fake detectors and a
 * fake connect lambda — no USB, no socket, no emulator.
 */
class CaptureAutoConnectControllerTest {

    private val d6 = AutoDetection(
        sensor = SensorType.COIN_D6,
        transportHint = "/dev/bus/usb/001/003",
        label = "COIN-D6 · /dev/bus/usb/001/003",
    )
    private val mid360 = AutoDetection(
        sensor = SensorType.MID360,
        transportHint = "192.168.1.159|192.168.1.5",
        label = "Mid-360 · 192.168.1.159",
        detail = "SN MCP7K0034759",
    )

    private class FakeDetector(
        override val sensor: SensorType,
        private val result: AutoDetection?,
        private val delayMs: Long = 0,
        private val throws: Boolean = false,
    ) : SensorAutoDetector {
        var detectCalls = 0
        var completed = false

        override suspend fun detect(): AutoDetection? {
            detectCalls++
            if (delayMs > 0) delay(delayMs)
            if (throws) throw IllegalStateException("probe blew up")
            completed = true
            return result
        }
    }

    @Test
    fun `starts idle and says so`() = runTest {
        val c = CaptureAutoConnectController(emptyList(), { Result.success(Unit) }, this)
        assertEquals(CaptureAutoConnectState.Phase.IDLE, c.state.value.phase)
        assertFalse(c.state.value.isPreviewing)
        assertNull(c.state.value.detection)
    }

    @Test
    fun `detected then connected lands in PREVIEW with no extra call`() = runTest {
        var connects = 0
        val c = CaptureAutoConnectController(
            detectors = listOf(FakeDetector(SensorType.COIN_D6, d6, delayMs = 50)),
            connect = { connects++; Result.success(Unit) },
            scope = this,
        )

        c.start()
        runCurrent()
        assertEquals(CaptureAutoConnectState.Phase.SEARCHING, c.state.value.phase)
        assertTrue(c.state.value.isBusy)

        advanceUntilIdle()
        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        assertEquals(d6, c.state.value.detection)
        assertEquals(SensorType.COIN_D6, c.state.value.sensor)
        assertTrue(c.state.value.isPreviewing)
        assertEquals(1, connects)
        assertTrue(c.state.value.statusLine().contains("live preview"))
    }

    @Test
    fun `the first detector to answer wins and the other is cancelled`() = runTest {
        val fast = FakeDetector(SensorType.MID360, mid360, delayMs = 20)
        val slow = FakeDetector(SensorType.COIN_D6, d6, delayMs = 5_000)
        val c = CaptureAutoConnectController(listOf(slow, fast), { Result.success(Unit) }, this)

        c.start()
        advanceUntilIdle()

        assertEquals(mid360, c.state.value.detection)
        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        // The slow probe was started (both run concurrently — a Mid-360 operator
        // must not wait out the D6 window) but never got to finish.
        assertEquals(1, slow.detectCalls)
        assertFalse("the losing probe must be cancelled, not awaited", slow.completed)
    }

    @Test
    fun `nothing found is FAILED with the retry copy`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(
                FakeDetector(SensorType.COIN_D6, null, delayMs = 10),
                FakeDetector(SensorType.MID360, null, delayMs = 30),
            ),
            { Result.success(Unit) },
            this,
        )

        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
        assertEquals(CaptureAutoConnectController.NOTHING_FOUND, c.state.value.message)
        assertFalse(c.state.value.isPreviewing)
    }

    @Test
    fun `a detector that throws counts as not-found rather than taking the flow down`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(
                FakeDetector(SensorType.COIN_D6, d6, delayMs = 10, throws = true),
                FakeDetector(SensorType.MID360, mid360, delayMs = 40),
            ),
            { Result.success(Unit) },
            this,
        )

        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        assertEquals(mid360, c.state.value.detection)
    }

    @Test
    fun `a failed connect keeps the detection and reports the engine's own message`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6)),
            { Result.failure(IllegalStateException("scan_engine_add_device failed")) },
            this,
        )

        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
        assertEquals(d6, c.state.value.detection)
        assertEquals("scan_engine_add_device failed", c.state.value.message)
    }

    @Test
    fun `a connect lambda that throws is treated as a failed connect`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6)),
            { throw IllegalStateException("native library not loaded") },
            this,
        )

        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
        assertEquals("native library not loaded", c.state.value.message)
    }

    @Test
    fun `retry re-runs the whole sequence`() = runTest {
        var attempt = 0
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6)),
            {
                attempt++
                if (attempt == 1) Result.failure(IllegalStateException("busy")) else Result.success(Unit)
            },
            this,
        )

        c.start()
        advanceUntilIdle()
        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)

        c.retry()
        advanceUntilIdle()
        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        assertEquals(2, attempt)
    }

    @Test
    fun `restarting cancels an in-flight attempt instead of racing it`() = runTest {
        val slow = FakeDetector(SensorType.COIN_D6, d6, delayMs = 5_000)
        var connects = 0
        val c = CaptureAutoConnectController(listOf(slow), { connects++; Result.success(Unit) }, this)

        c.start()
        runCurrent()
        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        // Two probe runs (one cancelled, one completed), exactly one connect.
        assertEquals(2, slow.detectCalls)
        assertEquals(1, connects)
    }

    @Test
    fun `cancel while searching returns to IDLE`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6, delayMs = 5_000)),
            { Result.success(Unit) },
            this,
        )

        c.start()
        runCurrent()
        c.cancel()

        assertEquals(CaptureAutoConnectState.Phase.IDLE, c.state.value.phase)
        assertNull(c.state.value.detection)
    }

    @Test
    fun `cancel does not disturb a live preview`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6)),
            { Result.success(Unit) },
            this,
        )
        c.start()
        advanceUntilIdle()

        c.cancel()

        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        assertEquals(d6, c.state.value.detection)
    }

    @Test
    fun `a transport lost during preview becomes FAILED, and only during preview`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6)),
            { Result.success(Unit) },
            this,
        )

        // Ignored before a preview exists: a disconnect event while still
        // searching is just the searching state.
        c.onConnectionLost()
        assertEquals(CaptureAutoConnectState.Phase.IDLE, c.state.value.phase)

        c.start()
        advanceUntilIdle()
        c.onConnectionLost()

        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
        assertTrue(c.state.value.message!!.contains("disconnected"))
        // The detection is kept so the status line can still name what was lost.
        assertEquals(d6, c.state.value.detection)
    }

    // ── ROUND 5 owner addition 1: the manual-entry fallback ──────────────────

    @Test
    fun `nothing found opens the manual panel automatically`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, null)),
            { Result.success(Unit) },
            this,
        )
        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
        assertTrue("manual entry must open itself when nothing is found", c.state.value.manualEntryOpen)
    }

    @Test
    fun `a failed connect opens the manual panel too`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.MID360, mid360)),
            { Result.failure(IllegalStateException("no route to 192.168.1.159")) },
            this,
        )
        c.start()
        advanceUntilIdle()

        assertTrue(c.state.value.manualEntryOpen)
        assertEquals(mid360, c.state.value.detection)
    }

    @Test
    fun `a successful detect leaves the panel closed but reachable`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6)),
            { Result.success(Unit) },
            this,
        )
        c.start()
        advanceUntilIdle()
        assertFalse(c.state.value.manualEntryOpen)

        // "Enter manually" stays reachable after a successful detect — the
        // two-devices-on-one-rig case.
        c.showManualEntry()
        assertTrue(c.state.value.manualEntryOpen)
        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)

        c.hideManualEntry()
        assertFalse(c.state.value.manualEntryOpen)
        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
    }

    @Test
    fun `a manual connect reaches PREVIEW without running any detector`() = runTest {
        val detector = FakeDetector(SensorType.COIN_D6, null)
        var connected: AutoDetection? = null
        val c = CaptureAutoConnectController(
            listOf(detector),
            { connected = it; Result.success(Unit) },
            this,
        )

        val typed = AutoDetection(SensorType.MID360, "192.168.1.100|192.168.1.5", "Mid-360 · 192.168.1.100")
        c.connectManually(typed)
        // CONNECTING is published synchronously, before the coroutine runs — the
        // panel must never look idle while a connect is in flight.
        assertEquals(CaptureAutoConnectState.Phase.CONNECTING, c.state.value.phase)
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        assertEquals(typed, connected)
        assertEquals(0, detector.detectCalls)
        // The panel it was driven from stays up rather than vanishing under the finger.
        assertTrue(c.state.value.manualEntryOpen)
    }

    @Test
    fun `a failed manual connect keeps the panel open with the reason`() = runTest {
        val c = CaptureAutoConnectController(
            emptyList(),
            { Result.failure(IllegalStateException("USB permission denied")) },
            this,
        )
        c.connectManually(d6)
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
        assertEquals("USB permission denied", c.state.value.message)
        assertTrue(c.state.value.manualEntryOpen)
    }

    @Test
    fun `a manual connect cancels an in-flight auto-detect`() = runTest {
        val slow = FakeDetector(SensorType.COIN_D6, d6, delayMs = 5_000)
        var connects = 0
        val c = CaptureAutoConnectController(listOf(slow), { connects++; Result.success(Unit) }, this)

        c.start()
        runCurrent()
        c.connectManually(mid360)
        advanceUntilIdle()

        assertEquals(mid360, c.state.value.detection)
        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        assertEquals("the racing auto-probe must not also connect", 1, connects)
        assertFalse(slow.completed)
    }

    @Test
    fun `a transport lost during preview also opens the manual panel`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(FakeDetector(SensorType.COIN_D6, d6)),
            { Result.success(Unit) },
            this,
        )
        c.start()
        advanceUntilIdle()
        c.onConnectionLost()

        assertTrue(c.state.value.manualEntryOpen)
    }

    @Test
    fun `no detectors at all fails cleanly rather than hanging`() = runTest {
        val c = CaptureAutoConnectController(emptyList(), { Result.success(Unit) }, this)
        c.start()
        advanceUntilIdle()
        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
    }

    // ── ROUND 31 item 176(b): a detector that knows more than "nothing" ──────

    /** A detector that finds nothing but can explain why — the ambiguous serial port. */
    private class ExplainingDetector(
        override val sensor: SensorType,
        override val lastFailureMessage: String?,
    ) : SensorAutoDetector {
        override suspend fun detect(): AutoDetection? = null
    }

    @Test
    fun `a detector's own failure message replaces the generic one`() = runTest {
        // "No scanner found" would be a lie here: a scanner IS found, and the
        // ladder cannot tell which of two it is. The operator's next move is a
        // tap on the picker, not a hunt for a cable.
        val c = CaptureAutoConnectController(
            listOf(ExplainingDetector(SensorType.COIN_D6, "Can't tell which scanner this is.")),
            { Result.success(Unit) },
            this,
        )
        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.FAILED, c.state.value.phase)
        assertEquals("Can't tell which scanner this is.", c.state.value.message)
        // The panel with the picker in it still opens with the line, exactly as
        // round 5's "nothing found flows straight into manual entry" does.
        assertTrue(c.state.value.manualEntryOpen)
    }

    @Test
    fun `detectors with nothing to add leave the generic message alone`() = runTest {
        val c = CaptureAutoConnectController(
            listOf(
                ExplainingDetector(SensorType.COIN_D6, null),
                FakeDetector(SensorType.MID360, null),
            ),
            { Result.success(Unit) },
            this,
        )
        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectController.NOTHING_FOUND, c.state.value.message)
    }

    @Test
    fun `a detector that FOUND something never has its failure message read`() = runTest {
        // Guards the ordering: `lastFailureMessage` is consulted only on the
        // all-empty path, so a stale message from a previous run cannot appear
        // over a successful detection.
        val c = CaptureAutoConnectController(
            listOf(
                ExplainingDetector(SensorType.STL27L, "stale ambiguity from a previous retry"),
                FakeDetector(SensorType.COIN_D6, d6),
            ),
            { Result.success(Unit) },
            this,
        )
        c.start()
        advanceUntilIdle()

        assertEquals(CaptureAutoConnectState.Phase.PREVIEW, c.state.value.phase)
        assertNull(c.state.value.message)
    }
}
