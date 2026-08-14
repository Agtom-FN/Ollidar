package com.lidarscan.core.engine

import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B2's instrumentation-free JVM test for the connect-wizard state machine,
 * against [FakeEngineBridge] — no Android, no emulator, no USB hardware.
 * Mirrors the shape of B1's `FakeEngineBridgeTest`.
 */
class D6ConnectControllerTest {

    private fun controller(scope: TestScope, bridge: EngineBridge = FakeEngineBridge()) =
        D6ConnectController(bridge, scope) to bridge

    @Test
    fun `starts with no device`() = runTest {
        val (controller, _) = controller(this)
        assertEquals(ConnectWizardState.NoDevice, controller.state.value)
    }

    @Test
    fun `device found moves to awaiting permission`() = runTest {
        val (controller, _) = controller(this)

        controller.onDeviceFound("/dev/bus/usb/001/002")

        assertEquals(
            ConnectWizardState.AwaitingPermission("/dev/bus/usb/001/002"),
            controller.state.value,
        )
    }

    @Test
    fun `permission denied fails the wizard`() = runTest {
        val (controller, _) = controller(this)
        controller.onDeviceFound("/dev/bus/usb/001/002")

        controller.onPermissionDenied()

        val state = controller.state.value
        assertTrue(state is ConnectWizardState.Failed)
        assertEquals("/dev/bus/usb/001/002", (state as ConnectWizardState.Failed).devicePath)
    }

    @Test
    fun `permission granted connects through the bridge to Connected`() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val bridge = FakeEngineBridge()
        val controller = D6ConnectController(bridge, TestScope(dispatcher))
        controller.onDeviceFound("/dev/bus/usb/001/002")

        controller.onPermissionGranted("/dev/bus/usb/001/002")
        assertEquals(ConnectWizardState.Connecting("/dev/bus/usb/001/002"), controller.state.value)

        dispatcher.scheduler.advanceUntilIdle()

        assertEquals(ConnectWizardState.Connected("/dev/bus/usb/001/002"), controller.state.value)
        assertEquals(ConnectionState.CONNECTED, bridge.connectionState.value)
    }

    @Test
    fun `device lost while connected disconnects the bridge`() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val bridge = FakeEngineBridge()
        val controller = D6ConnectController(bridge, TestScope(dispatcher))
        controller.onDeviceFound("/dev/bus/usb/001/002")
        controller.onPermissionGranted("/dev/bus/usb/001/002")
        dispatcher.scheduler.advanceUntilIdle()
        assertEquals(ConnectWizardState.Connected("/dev/bus/usb/001/002"), controller.state.value)

        controller.onDeviceLost()
        dispatcher.scheduler.advanceUntilIdle()

        assertEquals(ConnectWizardState.NoDevice, controller.state.value)
        assertEquals(ConnectionState.DISCONNECTED, bridge.connectionState.value)
    }

    @Test
    fun `retry after failure reconnects`() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val bridge = FakeEngineBridge()
        val controller = D6ConnectController(bridge, TestScope(dispatcher))
        controller.onDeviceFound("/dev/bus/usb/001/002")
        controller.onPermissionDenied()
        assertTrue(controller.state.value is ConnectWizardState.Failed)

        controller.retry("/dev/bus/usb/001/002")
        dispatcher.scheduler.advanceUntilIdle()

        assertEquals(ConnectWizardState.Connected("/dev/bus/usb/001/002"), controller.state.value)
    }
}
