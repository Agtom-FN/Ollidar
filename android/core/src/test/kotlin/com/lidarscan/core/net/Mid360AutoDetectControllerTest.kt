@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.core.net

import kotlinx.coroutines.delay
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * AUTO-DETECT wizard-state tests for [Mid360AutoDetectController], against a
 * fake [Mid360Detector] — no real socket, no emulator. Mirrors
 * `D6ConnectControllerTest`'s shape one package over.
 */
class Mid360AutoDetectControllerTest {

    private val sampleHeartbeat = Mid360Heartbeat(
        serialNumber = "MCP7K0034759",
        deviceType = "Mid-360",
        firmwareVersion = "35010108",
        lidarIp = "192.168.1.159",
        lidarNetmask = "255.255.255.0",
        lidarGateway = "192.168.1.1",
        persistedHostIp = "192.168.1.5",
        persistedHostPointPort = 56301,
        persistedHostImuPort = 56401,
    )

    private class FakeMid360Detector(
        private val result: Mid360DetectionResult,
        private val delayMs: Long = 0,
    ) : Mid360Detector {
        var lastTimeoutMs: Long? = null

        override suspend fun detect(timeoutMs: Long, onElapsedMs: (Long) -> Unit): Mid360DetectionResult {
            lastTimeoutMs = timeoutMs
            if (delayMs > 0) {
                onElapsedMs(delayMs)
                delay(delayMs)
            }
            return result
        }
    }

    @Test
    fun `starts idle`() = runTest {
        val controller = Mid360AutoDetectController(FakeMid360Detector(Mid360DetectionResult.TimedOut), this)
        assertEquals(Mid360AutoDetectState.Status.IDLE, controller.state.value.status)
    }

    @Test
    fun `start moves to LISTENING immediately, then FOUND with a matching host`() = runTest {
        val detector = FakeMid360Detector(Mid360DetectionResult.Found(sampleHeartbeat), delayMs = 100)
        val controller = Mid360AutoDetectController(detector, this)

        controller.start(timeoutMs = 5_000) { listOf(LocalAddress("192.168.1.5", 24)) }
        assertEquals(Mid360AutoDetectState.Status.LISTENING, controller.state.value.status)

        advanceUntilIdle()

        val state = controller.state.value
        assertEquals(Mid360AutoDetectState.Status.FOUND, state.status)
        assertEquals(sampleHeartbeat, state.found)
        assertEquals(true, state.hostMatches)
        assertEquals(5_000L, detector.lastTimeoutMs)
    }

    @Test
    fun `a found heartbeat whose persisted host is not one of this phone's addresses reports a mismatch`() =
        runTest {
            val detector = FakeMid360Detector(Mid360DetectionResult.Found(sampleHeartbeat))
            val controller = Mid360AutoDetectController(detector, this)

            controller.start { listOf(LocalAddress("10.0.0.9", 24)) }
            advanceUntilIdle()

            val state = controller.state.value
            assertEquals(Mid360AutoDetectState.Status.FOUND, state.status)
            assertEquals(false, state.hostMatches)
        }

    @Test
    fun `no local Ethernet address yet also reports a mismatch, not a crash`() = runTest {
        val detector = FakeMid360Detector(Mid360DetectionResult.Found(sampleHeartbeat))
        val controller = Mid360AutoDetectController(detector, this)

        controller.start { emptyList() }
        advanceUntilIdle()

        assertEquals(false, controller.state.value.hostMatches)
    }

    @Test
    fun `timeout reports TIMED_OUT with no heartbeat`() = runTest {
        val controller = Mid360AutoDetectController(FakeMid360Detector(Mid360DetectionResult.TimedOut), this)

        controller.start { emptyList() }
        advanceUntilIdle()

        val state = controller.state.value
        assertEquals(Mid360AutoDetectState.Status.TIMED_OUT, state.status)
        assertNull(state.found)
    }

    @Test
    fun `a detector error surfaces its message`() = runTest {
        val detector = FakeMid360Detector(Mid360DetectionResult.Error("socket bind failed"))
        val controller = Mid360AutoDetectController(detector, this)

        controller.start { emptyList() }
        advanceUntilIdle()

        val state = controller.state.value
        assertEquals(Mid360AutoDetectState.Status.ERROR, state.status)
        assertEquals("socket bind failed", state.message)
    }

    @Test
    fun `cancel while listening returns to idle without leaving a stale result`() = runTest {
        val detector = FakeMid360Detector(Mid360DetectionResult.Found(sampleHeartbeat), delayMs = 10_000)
        val controller = Mid360AutoDetectController(detector, this)

        controller.start { emptyList() }
        assertEquals(Mid360AutoDetectState.Status.LISTENING, controller.state.value.status)

        controller.cancel()

        assertEquals(Mid360AutoDetectState.Status.IDLE, controller.state.value.status)
        assertNull(controller.state.value.found)
    }

    @Test
    fun `reset clears a previous FOUND result`() = runTest {
        val detector = FakeMid360Detector(Mid360DetectionResult.Found(sampleHeartbeat))
        val controller = Mid360AutoDetectController(detector, this)
        controller.start { listOf(LocalAddress("192.168.1.5", 24)) }
        advanceUntilIdle()
        assertTrue(controller.state.value.found != null)

        controller.reset()

        assertEquals(Mid360AutoDetectState(), controller.state.value)
    }

    /** First call hangs "forever" (never completes within the test's virtual time budget); second returns fast. */
    private class SequencedDetector(private val results: List<Mid360DetectionResult>) : Mid360Detector {
        var calls = 0
        override suspend fun detect(timeoutMs: Long, onElapsedMs: (Long) -> Unit): Mid360DetectionResult {
            val index = calls++
            if (index == 0) {
                delay(1_000_000) // never resolves before the test ends
                error("first call's detect() must never complete")
            }
            return results[index - 1]
        }
    }

    @Test
    fun `starting again cancels a still-listening previous attempt`() = runTest {
        val detector = SequencedDetector(listOf(Mid360DetectionResult.TimedOut))
        val controller = Mid360AutoDetectController(detector, this)

        controller.start { emptyList() } // call #0: will hang once it actually runs
        // Let the first attempt's coroutine actually start and reach its
        // suspended detect() call before cancelling it — otherwise `cancel`
        // races a job that hasn't been dispatched yet and this test would
        // exercise nothing.
        runCurrent()
        assertEquals(Mid360AutoDetectState.Status.LISTENING, controller.state.value.status)
        assertEquals(1, detector.calls)

        controller.start { emptyList() } // call #1: cancels #0's job, then times out
        advanceUntilIdle()

        assertEquals(2, detector.calls)
        assertEquals(Mid360AutoDetectState.Status.TIMED_OUT, controller.state.value.status)
    }
}
