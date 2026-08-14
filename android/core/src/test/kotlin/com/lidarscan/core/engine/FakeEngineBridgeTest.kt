package com.lidarscan.core.engine

import com.lidarscan.core.model.SensorType
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FakeEngineBridgeTest {

    @Test
    fun `starts disconnected and idle`() {
        val bridge = FakeEngineBridge()

        assertEquals(ConnectionState.DISCONNECTED, bridge.connectionState.value)
        assertEquals(CaptureState.IDLE, bridge.captureState.value)
    }

    @Test
    fun `connect transitions to connected`() = runTest {
        val bridge = FakeEngineBridge()

        val result = bridge.connect(EngineTarget(SensorType.MID360))

        assertTrue(result.isSuccess)
        assertEquals(ConnectionState.CONNECTED, bridge.connectionState.value)
    }

    @Test
    fun `startCapture without connecting first fails`() = runTest {
        val bridge = FakeEngineBridge()

        val result = bridge.startCapture(projectDirectory = "/tmp/project.lscan", liveSlam = true)

        assertTrue(result.isFailure)
        assertEquals(CaptureState.IDLE, bridge.captureState.value)
    }

    @Test
    fun `capture lifecycle moves through recording, paused and back to idle`() = runTest {
        val bridge = FakeEngineBridge()
        bridge.connect(EngineTarget(SensorType.COIN_D6))

        bridge.startCapture(projectDirectory = "/tmp/project.lscan", liveSlam = false)
        assertEquals(CaptureState.RECORDING, bridge.captureState.value)

        bridge.pauseCapture()
        assertEquals(CaptureState.PAUSED, bridge.captureState.value)

        bridge.resumeCapture()
        assertEquals(CaptureState.RECORDING, bridge.captureState.value)

        bridge.stopCapture()
        assertEquals(CaptureState.IDLE, bridge.captureState.value)
    }
}
