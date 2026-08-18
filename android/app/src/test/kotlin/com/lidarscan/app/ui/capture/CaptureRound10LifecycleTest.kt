@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.app.debug.CaptureLog
import com.lidarscan.core.capture.AutoDetection
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
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 10, owner item 38 — **the capture lifecycle, which the owner called
 * unacceptable.**
 *
 * > "When i finish the capture and click stop, it will stay with the capture
 * > page but not heading to project. when click capture after capture, it
 * > still show with the previous capture. i can't start a new capture unless i
 * > close and reopen the app."
 *
 * Three symptoms, and reading the code found two distinct causes, neither of
 * which any existing test could see:
 *
 *  1. **"still show with the previous capture"** — the live cloud does not
 *     live in the ViewModel. `RealEngineBridge` creates ONE `scan_engine*` for
 *     the process and the engine's `PageStore` belongs to the engine, not to
 *     the session, so capture #2 opened on top of capture #1's pages. Every
 *     ViewModel-level test passed because every ViewModel-level state WAS
 *     being reset. Covered here by counting `resetLiveView()` calls through
 *     the bridge; the native half is covered by the instrumented test.
 *
 *  2. **"not heading to project"** — `sealedProjectId` was a `replay = 0`
 *     `MutableSharedFlow`, whose own comment claimed the buffer covered "a
 *     collector that is momentarily absent". It does not: with zero
 *     subscribers a `replay = 0` flow discards the value. The seal runs under
 *     `NonCancellable` in `viewModelScope` and therefore outlives the
 *     composition, so an Activity recreation mid-seal loses the navigation
 *     while keeping the scan. The second case here subscribes AFTER the seal
 *     has completed — which is what an absent collector looks like — and
 *     requires the id to arrive anyway.
 *
 * `CaptureViewModelMultiCycleTest` already covers "two cycles produce two
 * sealed projects"; this file covers what that one could not see.
 */
class CaptureRound10LifecycleTest {

    @Before
    fun setUp() {
        Dispatchers.setMain(Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    /** A bridge that remembers whether anyone asked it to empty the live window. */
    private class CountingBridge : FakeEngineBridge() {
        val resets = AtomicInteger(0)
        override suspend fun resetLiveView(): Result<Unit> {
            resets.incrementAndGet()
            return super.resetLiveView()
        }
    }

    private fun tempRoot(): File = File.createTempFile("round10Lifecycle", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    private fun newVm(bridge: FakeEngineBridge, store: FileProjectStore, series: AtomicInteger) =
        CaptureViewModel(
            engineBridge = bridge,
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
        )

    @Test
    fun `the live window is emptied on entering Capture and again after every seal`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val bridge = CountingBridge()
        val vm = newVm(bridge, store, AtomicInteger(0))

        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        // Entering the tab is a new-scan context (ROUND 9 item 33) and the live
        // cloud is part of that context, however long-lived the store behind it
        // happens to be.
        withTimeout(5_000) {
            while (bridge.resets.get() < 1) kotlinx.coroutines.delay(10)
        }
        val afterEntry = bridge.resets.get()
        assertTrue("entering Capture must empty the live window", afterEntry >= 1)

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

        assertTrue(
            "sealing must empty the live window too, or the NEXT capture opens on this one's cloud",
            bridge.resets.get() > afterEntry,
        )
    }

    @Test
    fun `a collector that subscribes after the seal still gets the navigation`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val vm = newVm(FakeEngineBridge(), store, AtomicInteger(0))

        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        val recordedId = (vm.uiState.value as CaptureUiState.Loaded).project.id

        // Deliberately NOT collecting yet. This is an Activity recreation
        // mid-seal: the seal is `NonCancellable` in `viewModelScope` and runs
        // to completion, the composition that was collecting is gone.
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

        // ...and only now does the new composition attach its collector. Under
        // `replay = 0` this waits forever and the operator sits on the Capture
        // tab looking at a scan that saved perfectly.
        val navigated = withTimeout(5_000) { vm.sealedProjectId.first() }
        assertEquals("the seal must navigate to the scan it just wrote", recordedId, navigated)
    }

    @Test
    fun `capture stop capture in one ViewModel seals two scans and re-arms clean`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val bridge = CountingBridge()
        val vm = newVm(bridge, store, AtomicInteger(0))
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        val ids = mutableListOf<String>()
        repeat(2) {
            vm.startCapture()
            withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
            kotlinx.coroutines.delay(700)
            ids += (vm.uiState.value as CaptureUiState.Loaded).project.id
            vm.stopCapture()
            withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
            val armed = withTimeout(5_000) {
                vm.uiState.first { it is CaptureUiState.NewScan } as CaptureUiState.NewScan
            }
            // "Start immediately works": a fresh auto-name, no typed name left
            // over from the scan that just ended, and zeroed stats.
            assertTrue("the re-armed tab must offer a fresh auto-name", armed.autoName.isNotBlank())
            assertEquals("the typed name is spent with the scan it named", "", vm.scanName.value)
            assertEquals("the stats line must not carry the finished scan", 0L, vm.stats.value.pointsCaptured)
        }

        assertNotEquals("the second Start must create a different project", ids[0], ids[1])
        assertEquals("two cycles, two scans on disk", 2, store.list().size)
        for (p in store.list()) {
            assertTrue(
                "${p.id} must have sealed with points",
                (p.manifest.pointCountEstimate ?: 0L) > 0L,
            )
        }
    }

    /**
     * ROUND 10, owner item 40 — *"the capture log please save with date and
     * time in the file name."*
     */
    @Test
    fun `the capture log export name carries local date and time and never repeats`() {
        // 2026-08-18 11:11 local, the minute the owner exported scan-020's log.
        val cal = java.util.Calendar.getInstance()
        cal.set(2026, java.util.Calendar.AUGUST, 18, 11, 11, 30)
        cal.set(java.util.Calendar.MILLISECOND, 0)
        val name = CaptureLog.exportFileName(cal.timeInMillis)
        assertEquals("lidarscan-capture-log-2026-08-18-1111.txt", name)

        // The whole point: a later export is a DIFFERENT file, so MediaStore
        // stops making `lidarscan-capture-log (1).txt` and reports pair with
        // scans by name instead of by opening them.
        cal.set(java.util.Calendar.MINUTE, 42)
        val later = CaptureLog.exportFileName(cal.timeInMillis)
        assertNotEquals(name, later)
        assertEquals("lidarscan-capture-log-2026-08-18-1142.txt", later)

        // Local time to the minute, matching `ScanAutoName`'s own format, so
        // `Scan-020-2026-08-18-1106` and this name are comparable by eye.
        assertTrue(name.matches(Regex("""lidarscan-capture-log-\d{4}-\d{2}-\d{2}-\d{4}\.txt""")))
    }
}
