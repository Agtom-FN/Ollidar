@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.CaptureAutoConnectState
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.EngineTarget
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.engine.SerialLidarBaud
import com.lidarscan.core.engine.SerialModemLines
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
import org.junit.Assert.assertNull
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
 *
 * ## ROUND 31 item 176(a) — what round 25 did not test, and the owner found
 *
 * Every test below started from a ViewModel that had never connected to
 * anything. That is not the state the picker is used in. The picker exists to
 * OVERRIDE an auto-detect that guessed wrong, so by the time a finger reaches
 * it the engine is already connected — to the wrong sensor, on the very port
 * the manual connect is about to reopen at a different baud. Round 25 never
 * put a ViewModel in that state, so nothing noticed that the previous
 * connection was never released; on hardware, `claimInterface` then refused
 * the second open and the manual choice died as a `Result.failure`.
 *
 * The `alreadyConnected…` tests below are that state, and they are the
 * regression fence.
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
        calls.clear()
        openFailure = null
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

    /**
     * ROUND 31 item 176(a) — auto-detect getting it WRONG, which is the only
     * situation the manual picker is ever used in. Answers COIN-D6 on the port
     * the operator is about to say is an STL-27L.
     */
    private class WrongD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection = AutoDetection(
            sensor = SensorType.COIN_D6,
            transportHint = DEVICE.path,
            label = "COIN-D6 · ${DEVICE.label}",
        )
    }

    /**
     * ROUND 31 item 176(a): records the bridge calls in order, because the
     * ORDER is the fix. Releasing the previous connection has to happen
     * BEFORE the port is reopened at the new baud — the other way round is
     * what the phone was doing, and it is what threw.
     */
    private class RecordingBridge(scope: CoroutineScope, val calls: MutableList<String>) :
        FakeEngineBridge(scope) {
        override suspend fun connect(target: EngineTarget): Result<Unit> {
            calls += "connect:${target.sensor.name}:${target.transportHint}"
            // The one behaviour of `RealEngineBridge.connect` this suite
            // depends on and `FakeEngineBridge` does not have: a serial
            // sensor with no device path is REFUSED, with the sensor named.
            // `connectManualSerialLidar` relies on exactly that to carry a
            // failed port-open into the UI, so a fake that connects happily to
            // null would let this suite pass a build the phone cannot run.
            if (target.transportHint == null && SerialLidarBaud.isSerial(target.sensor)) {
                return Result.failure(
                    IllegalArgumentException("${target.sensor.displayName} connect needs a device path"),
                )
            }
            return super.connect(target)
        }

        override suspend fun disconnect() {
            calls += "disconnect"
            super.disconnect()
        }
    }

    private fun tempRoot(): File = File.createTempFile("item119vm", "").let {
        it.delete(); it.mkdirs(); it
    }

    /** Every (path, baud) pair the manual path asked the USB layer to open. */
    private val opened = CopyOnWriteArrayList<Pair<String, Int>>()

    /** Bridge calls in the order they were made, for the round-31 ordering assertions. */
    private val calls = CopyOnWriteArrayList<String>()

    /** Set by a test to make the port refuse to open. */
    @Volatile private var openFailure: String? = null

    private fun newVm(detector: SensorAutoDetector = SilentDetector()): CaptureViewModel {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = RecordingBridge(engineScope, calls),
            projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
            autoDetectors = listOf(detector),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            attachedSerialDevices = { listOf(DEVICE) },
            openSerialPort = { path, baud, lines ->
                // ROUND 32 item 178(a): the modem-line state is part of what a
                // manual connect DOES, so it is part of what this records.
                calls += "open:$path@$baud ${lines.log}"
                opened += path to baud
                openFailure?.let { Result.failure(IllegalStateException(it)) } ?: Result.success(Unit)
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

    // ── ROUND 31 item 176(a): the manual pick, from the state it is used in ──

    @Test
    fun `an STL-27L pick over a wrong D6 auto-detect releases the port first, then binds`() = runBlocking {
        val vm = newVm(WrongD6Detector())
        // Auto-detect gets it wrong, exactly as it did on the owner's Pixel.
        awaitPreview(vm)
        assertEquals(SensorType.COIN_D6, vm.sensor.value)
        calls.clear()
        opened.clear()

        vm.connectManualSerialLidar(DEVICE, SensorType.STL27L)
        val state = awaitPreview(vm)

        // THE FIX, as an ordering assertion: the previous connection is
        // released before the port is reopened at 921600. Without the
        // disconnect the port is still claimed and the real `registry.open`
        // throws "Could not claim interface 0" here.
        assertEquals(
            listOf(
                "disconnect",
                "open:${DEVICE.path}@${SerialLidarBaud.STL27L} ${SerialModemLines.STL27L.log}",
                "connect:STL27L:${DEVICE.path}",
            ),
            calls.toList(),
        )
        // And the choice is what the session IS, not merely what was tapped.
        assertEquals(SensorType.STL27L, state.detection?.sensor)
        assertEquals(SensorType.STL27L, vm.sensor.value)
        assertTrue(vm.poseTrackingRequired)
    }

    @Test
    fun `picking the SAME sensor as a wrong auto-detect still rebinds cleanly`() = runBlocking {
        // The operator confirming the guess must not be a special case: the
        // port is released and reopened at the same baud, and nothing stacks.
        val vm = newVm(WrongD6Detector())
        awaitPreview(vm)
        calls.clear()

        vm.connectManualSerialLidar(DEVICE, SensorType.COIN_D6)
        awaitPreview(vm)

        assertEquals(
            listOf(
                "disconnect",
                "open:${DEVICE.path}@${SerialLidarBaud.COIN_D6} ${SerialModemLines.COIN_D6.log}",
                "connect:COIN_D6:${DEVICE.path}",
            ),
            calls.toList(),
        )
    }

    @Test
    fun `a manual pick on a disconnected engine does not invent a disconnect`() = runBlocking {
        val vm = newVm()
        calls.clear()

        vm.connectManualSerialLidar(DEVICE, SensorType.STL27L)
        awaitPreview(vm)

        assertEquals(
            listOf("open:${DEVICE.path}@${SerialLidarBaud.STL27L} ${SerialModemLines.STL27L.log}", "connect:STL27L:${DEVICE.path}"),
            calls.toList(),
        )
    }

    @Test
    fun `a port that will not open fails as the picked sensor, never as a D6`() = runBlocking {
        // The failure the owner actually saw. It must still name the STL-27L:
        // a failure attributed to the sensor the operator did NOT pick is how
        // "manual selection is not adopted" reads from the outside.
        val vm = newVm()
        openFailure = "Could not claim interface 0"

        vm.connectManualSerialLidar(DEVICE, SensorType.STL27L)
        val state = withTimeout(5_000) {
            var st = vm.autoConnectState!!.value
            while (st.phase != CaptureAutoConnectState.Phase.FAILED) {
                delay(10)
                st = vm.autoConnectState!!.value
            }
            st
        }

        assertEquals(SensorType.STL27L, state.detection?.sensor)
        assertTrue(state.detection?.label?.contains("STL-27L") == true)
        // The engine was never handed a device path, so it refused rather than
        // silently connecting to whatever was there.
        assertNull(state.detection?.transportHint)
        openFailure = null
    }

    private companion object {
        /** Long enough for a cancelled Unconfined child to finish unwinding. */
        const val SETTLE_MS = 50L

        val DEVICE = ManualSerialDevice(path = "/dev/bus/usb/001/003", label = "003 · VID 6790/PID 29987")
    }
}
