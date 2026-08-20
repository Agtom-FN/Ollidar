@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.CaptureAutoConnectState
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.engine.SerialLidarBaud
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 25 item 119 — **the Advanced sheet's sensor row, from the ViewModel
 * down.**
 *
 * The row itself is a two-option `SegmentedPill` in `ManualEntryPanel`; what
 * this suite pins is the thing underneath it that can go silently wrong. A
 * manual connect has to open the CH340 at the chosen sensor's baud *and* tell
 * the engine the same number, and the failure mode when it does not is not an
 * error — the port opens, the reader thread delivers bytes, and every one of
 * them is framing garbage. So the baud is asserted, per sensor, at the seam
 * where the app actually sets the divisor.
 *
 * Harness is rounds 23–25's: one real `CaptureViewModel` on a JVM, with the
 * USB layer replaced by a lambda that records what it was asked for.
 */
class CaptureStl27lManualConnectTest {

    private val built = CopyOnWriteArrayList<CaptureViewModel>()

    /**
     * The fake engine's own scope, held so tearDown can cancel it.
     * `FakeEngineBridge`'s default is a `SupervisorJob` nobody ever cancels;
     * a class that leaves one running can make the NEXT class's
     * `Dispatchers.setMain` throw "used concurrently with setting it", which is
     * a failure attributed to an innocent test.
     */
    private val engineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() {
        built.forEach { it.shutDownForTest() }
        built.clear()
        engineScope.cancel()
        // Cancellation is cooperative: `shutDownForTest` ASKS the children to
        // stop, it does not wait for them. `resetMain()` throws
        // "Dispatchers.Main is used concurrently with setting it" if anything is
        // still dispatching on Main while it runs — and because Gradle runs the
        // whole module's test classes in one JVM, that exception surfaces in
        // whichever class happens to run NEXT, blaming an innocent suite. One
        // short settle costs 50 ms per class and closes the category.
        runBlocking { delay(SETTLE_MS) }
        Dispatchers.resetMain()
    }

    /** A detector that never answers, so nothing races the manual path under test. */
    private class SilentDetector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection? = null
    }

    private fun tempRoot(): File = File.createTempFile("item119vm", "").let {
        it.delete(); it.mkdirs(); it
    }

    /** Every (path, baud) pair the manual path asked the USB layer to open. */
    private val opened = CopyOnWriteArrayList<Pair<String, Int>>()

    private fun newVm(): CaptureViewModel {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(engineScope),
            projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
            autoDetectors = listOf(SilentDetector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            attachedSerialDevices = { listOf(DEVICE) },
            openSerialPort = { path, baud ->
                opened += path to baud
                Result.success(Unit)
            },
        )
        built += vm
        return vm
    }

    private suspend fun awaitPreview(vm: CaptureViewModel): CaptureAutoConnectState =
        withTimeout(5_000) {
            var state = vm.autoConnectState!!.value
            while (state.phase != CaptureAutoConnectState.Phase.PREVIEW) {
                delay(10)
                state = vm.autoConnectState!!.value
            }
            state
        }

    @Test
    fun `picking STL-27L opens the port at 921600 and connects as an STL-27L`() = runBlocking {
        val vm = newVm()
        vm.connectManualSerialLidar(DEVICE, SensorType.STL27L)

        val state = awaitPreview(vm)

        assertEquals(listOf(DEVICE.path to SerialLidarBaud.STL27L), opened.toList())
        assertEquals(SensorType.STL27L, state.detection?.sensor)
        assertEquals(DEVICE.path, state.detection?.transportHint)
        assertTrue(
            "the status line should name the sensor the operator picked: ${state.detection?.label}",
            state.detection?.label?.contains("STL-27L") == true,
        )
        assertEquals(SensorType.STL27L, vm.sensor.value)
    }

    @Test
    fun `picking COIN-D6 still opens the port at 230400 — the default path did not move`() = runBlocking {
        val vm = newVm()
        vm.connectManualSerialLidar(DEVICE, SensorType.COIN_D6)

        val state = awaitPreview(vm)

        assertEquals(listOf(DEVICE.path to SerialLidarBaud.COIN_D6), opened.toList())
        assertEquals(SensorType.COIN_D6, state.detection?.sensor)
    }

    @Test
    fun `the pre-item-119 connectManualD6 spelling is an alias, not a second path`() = runBlocking {
        val vm = newVm()
        vm.connectManualD6(DEVICE)

        val state = awaitPreview(vm)

        assertEquals(listOf(DEVICE.path to SerialLidarBaud.COIN_D6), opened.toList())
        assertEquals(SensorType.COIN_D6, state.detection?.sensor)
    }

    /**
     * The reason [SensorType.isPhoneTrackedPushbroom] exists. An STL-27L has no
     * IMU either, so the pose pump must run for it — a session that answered
     * `false` here would record a fan of points with no trajectory under it and
     * look perfectly healthy while doing so.
     */
    @Test
    fun `an STL-27L session still requires phone pose tracking`() = runBlocking {
        val vm = newVm()
        vm.connectManualSerialLidar(DEVICE, SensorType.STL27L)
        awaitPreview(vm)

        assertTrue(vm.poseTrackingRequired)
    }

    private companion object {
        /** Long enough for a cancelled Unconfined child to finish unwinding. */
        const val SETTLE_MS = 50L

        val DEVICE = ManualSerialDevice(path = "/dev/bus/usb/001/003", label = "003 · VID 6790/PID 29987")
    }
}
