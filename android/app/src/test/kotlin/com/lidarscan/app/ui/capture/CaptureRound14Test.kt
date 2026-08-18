@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.CaptureFocus
import com.lidarscan.core.capture.DndState
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 14 — the app-side behaviours, driven through the real
 * `CaptureViewModel` against `:core`'s `FakeEngineBridge`.
 */
class CaptureRound14Test {

    @Before
    fun setUp() = Dispatchers.setMain(Dispatchers.Unconfined)

    @After
    fun tearDown() = Dispatchers.resetMain()

    private fun tempStore(): FileProjectStore {
        val dir = File.createTempFile("round14", "").let { it.delete(); it.mkdirs(); it }
        return FileProjectStore(dir, appVersion = "test")
    }

    private class Mid360Detector : SensorAutoDetector {
        override val sensor = SensorType.MID360
        override suspend fun detect(): AutoDetection = AutoDetection(
            sensor = sensor,
            // "lidarIp|hostIp", exactly as Mid360HeartbeatAutoDetector reports
            // it: the host half is the address the lidar has PERSISTED and will
            // unicast to. These are the owner's real values.
            transportHint = "192.168.1.159|192.168.1.5",
            label = "Livox Mid-360 · fake",
        )
    }

    private class D6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    private fun mid360Vm(
        adapterPresent: Boolean,
        addresses: List<String>,
    ): CaptureViewModel {
        val series = AtomicInteger(0)
        return CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = tempStore(),
            autoDetectors = listOf(Mid360Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            ethernetSnapshot = { adapterPresent to addresses },
        )
    }

    // ── item 53: the preflight ──────────────────────────────────────────────

    @Test
    fun `a Mid-360 on the wrong subnet is refused before a byte is recorded`(): Unit = runBlocking {
        // The owner's scan-031 and scan-032: the phone's Ethernet DHCPs to
        // .100, the lidar unicasts to .5, and 0.8.0 recorded two sealed scans
        // of nothing before saying so.
        val vm = mid360Vm(adapterPresent = true, addresses = listOf("192.168.1.100"))
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture()
        kotlinx.coroutines.delay(400)

        assertEquals(
            "the session must not have started",
            CaptureState.IDLE,
            vm.captureState.value,
        )
        assertTrue(
            "no project may be created for a scan that cannot record",
            vm.uiState.value is CaptureUiState.NewScan,
        )
        val err = vm.saveError.value
        assertNotNull(err)
        assertTrue(err!!, err.contains("192.168.1.5"))
        assertTrue(err, err.contains("192.168.1.100"))
        assertTrue(err, err.contains("Static"))
    }

    @Test
    fun `a correctly addressed Mid-360 with a heartbeat records`(): Unit = runBlocking {
        val vm = mid360Vm(adapterPresent = true, addresses = listOf("192.168.1.5"))
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        // The detection itself IS the parsed heartbeat.
        assertEquals("ok", vm.mid360Preflight()!!.logToken)

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    @Test
    fun `a D6 rig never consults the preflight`(): Unit = runBlocking {
        // The gate must be invisible to the sensor the owner actually walks
        // with — including on a phone with no Ethernet adapter at all.
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = tempStore(),
            autoDetectors = listOf(D6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            ethernetSnapshot = { false to emptyList() },
        )
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        assertNull("a D6 has no preflight", vm.mid360Preflight())

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        assertNull(vm.saveError.value)
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    @Test
    fun `no adapter at all is refused with a different sentence`(): Unit = runBlocking {
        val vm = mid360Vm(adapterPresent = false, addresses = emptyList())
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        assertEquals("no-ethernet", vm.mid360Preflight()!!.logToken)
    }

    // ── item 52: the DND note actually reaches a screen ─────────────────────

    @Test
    fun `the unprotected note survives the seal`(): Unit = runBlocking {
        // 0.8.0 wiped this at seal AND never collected it, so the owner's three
        // sessions each logged `dnd=unprotected-no-permission` with nothing on
        // screen at any point. The filter is restored at Stop — that is
        // `dndState` — but the FACT that the walk was unprotected is still true
        // afterwards and is still true of the next walk.
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = tempStore(),
            autoDetectors = listOf(D6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            engageDnd = { DndState.NO_PERMISSION },
        )
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        assertEquals(CaptureFocus.note(DndState.NO_PERMISSION), vm.dndNote.value)

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        assertNotNull("the note must outlive the seal", vm.dndNote.value)
        assertEquals(DndState.DISABLED, vm.dndState.value)
    }

    @Test
    fun `granting the access clears the note`() {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = tempStore(),
            autoDetectors = emptyList(),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
        )
        vm.refreshDndNote(enabled = true, granted = false)
        assertNotNull(vm.dndNote.value)
        vm.refreshDndNote(enabled = true, granted = true)
        assertNull("the note must go when the grant arrives", vm.dndNote.value)
        vm.refreshDndNote(enabled = false, granted = false)
        assertNull("a switched-off feature is not an unprotected walk", vm.dndNote.value)
    }
}
