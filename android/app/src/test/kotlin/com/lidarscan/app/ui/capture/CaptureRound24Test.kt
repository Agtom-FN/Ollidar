@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList
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
 * ROUND 24 item 111 — **"When ever the user click the scan tab its a new
 * scan."**
 *
 * The owner's sentence is short and the implementation is not, because the
 * three cases it must NOT fire in are each worse than the bug it fixes:
 *
 *  1. **under a live capture.** Checking the Projects list mid-walk is a normal
 *     thing to do. Resetting the stats, the trail and the section counters
 *     under a running recording would be a data-integrity bug reported as "the
 *     app forgot my scan".
 *  2. **on a rotation.** ROUND 22 item 88 made the ViewModel survive both a tab
 *     switch and a configuration change, and the two produce the identical
 *     Compose sequence. Only the Activity can tell them apart, which is what
 *     [CaptureViewModel.onScanScreenLeaving] carries.
 *  3. **over a running auto-process.** That job lives on `containerScope`
 *     (item 90) precisely so it outlives the screen; a reset must not touch it.
 *
 * The harness is `CaptureRound23Test`'s, deliberately: one ViewModel for the
 * whole test, exactly as item 88 leaves it, with the screen's entry and exit
 * calls made by hand where the composition would make them.
 */
class CaptureRound24Test {

    /** See `CaptureViewModel.shutDownForTest` — a ticker left running flakes the NEXT class. */
    private val built = CopyOnWriteArrayList<CaptureViewModel>()

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() {
        built.forEach { it.shutDownForTest() }
        built.clear()
        Dispatchers.resetMain()
    }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    private fun tempRoot(): File = File.createTempFile("round24vm", "").let {
        it.delete(); it.mkdirs(); it
    }

    private fun newVm(logs: MutableList<String> = CopyOnWriteArrayList()): Pair<CaptureViewModel, MutableList<String>> {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            runAutoProcess = { _, _ -> null },
            logEvent = { tag, line -> logs.add("[$tag] $line") },
        )
        built += vm
        return vm to logs
    }

    /** Record a short scan, seal it, and spend the navigation exactly as the screen does. */
    private suspend fun recordAndSeal(vm: CaptureViewModel): String {
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        val sealed = withTimeout(15_000) { vm.sealedProjectId.first() }
        vm.sealNavigationHandled(sealed)
        return sealed
    }

    /**
     * The owner's sentence, end to end: seal → (Projects) → back to the Scan
     * tab, and the screen is a clean ready screen rather than the corpse of the
     * last one.
     */
    @Test
    fun `a fresh entry into the Scan tab is a new scan`() = runBlocking {
        val (vm, logs) = newVm()
        val sealed = recordAndSeal(vm)
        assertNotNull(sealed)

        // After a seal the screen is still carrying the last scan's summary and
        // its numbers — that is what "the corpse of the last one" means.
        assertTrue("the seal must leave a summary to clear", vm.scanSummary.value != null)

        // The trip: Projects, then back. `onScanScreenLeaving(false)` is what
        // the composition reports when the operator LEFT rather than rotated.
        vm.onScanScreenLeaving(configurationChange = false)
        vm.onScanScreenEntered()

        assertNull("the last scan's graded card must be gone", vm.scanSummary.value)
        assertNull("…and its session summary", vm.sessionSummary.value)
        assertEquals("…and the point counter is back to zero", 0L, vm.stats.value.pointsCaptured)
        assertTrue(
            "the reset must be logged, like every other automatic action",
            logs.any { it.contains("scan tab entered: fresh entry") },
        )
        // The ready screen names the NEXT scan, not the one just saved.
        val ui = vm.uiState.value
        assertTrue("the tab must show a fresh new-scan state, was $ui", ui is CaptureUiState.NewScan)
    }

    /**
     * The case that must never regress: a tab switch DURING a recording leaves
     * the capture completely alone. The operator checks the list and comes back
     * to a scan that is still running and still counting.
     */
    @Test
    fun `a tab switch mid-recording does not touch the capture`() = runBlocking {
        val (vm, _) = newVm()
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        val pointsBefore = vm.stats.value.pointsCaptured

        // Off to Projects and back, mid-walk.
        vm.onScanScreenLeaving(configurationChange = false)
        vm.onScanScreenEntered()

        assertEquals(
            "the capture must still be recording",
            CaptureState.RECORDING,
            vm.captureState.value,
        )
        assertTrue(
            "the points counter must not be reset under a live capture",
            vm.stats.value.pointsCaptured >= pointsBefore,
        )
        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        Unit
    }

    /**
     * A **rotation** is not an entry.
     *
     * Both produce dispose-then-compose against the same ViewModel, so without
     * this discriminator turning the phone sideways mid-setup would wipe the
     * scan name and the stats the operator was looking at.
     */
    @Test
    fun `a configuration change is not a new scan`() = runBlocking {
        val (vm, logs) = newVm()
        recordAndSeal(vm)
        val summaryBefore = vm.scanSummary.value
        assertNotNull(summaryBefore)

        // The device rotated: the screen is disposed and recomposed, and the
        // Activity says so.
        vm.onScanScreenLeaving(configurationChange = true)
        vm.onScanScreenEntered()

        assertEquals("a rotation must change nothing", summaryBefore, vm.scanSummary.value)
        assertTrue(
            "and it must not log a fresh entry",
            logs.none { it.contains("scan tab entered: fresh entry") },
        )
    }

    /**
     * The flag is spent by the entry that reads it.
     *
     * Otherwise one rotation would suppress every subsequent tab entry for the
     * life of the ViewModel — a bug that reproduces once and then hides.
     */
    @Test
    fun `the configuration-change flag is consumed, not sticky`() = runBlocking {
        val (vm, logs) = newVm()
        recordAndSeal(vm)

        vm.onScanScreenLeaving(configurationChange = true)
        vm.onScanScreenEntered()
        assertTrue(logs.none { it.contains("scan tab entered: fresh entry") })

        // A real trip away and back, right afterwards.
        vm.onScanScreenLeaving(configurationChange = false)
        vm.onScanScreenEntered()
        assertTrue(
            "the next real entry must still be a new scan",
            logs.any { it.contains("scan tab entered: fresh entry") },
        )
        assertNull(vm.scanSummary.value)
    }

    /**
     * The very first entry — no leaving call has ever been made — is a new
     * scan. `pendingConfigurationChange` defaults to false, and this pins that
     * default as behaviour rather than as an initialiser nobody reads.
     */
    @Test
    fun `the first entry of all is a new scan`() = runBlocking {
        val (vm, logs) = newVm()
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.onScanScreenEntered()
        assertTrue(logs.any { it.contains("scan tab entered: fresh entry") })
    }

    /**
     * ROUND 23 item 101(a) still holds: entering the tab with a start latch
     * held and nothing running releases it, and says so. Item 111 adds a reset
     * next to that; it must not have replaced it.
     */
    @Test
    fun `entering still re-arms a stuck start latch`() = runBlocking {
        val (vm, logs) = newVm()
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        // A refusal that leaves the standing reason on screen, then a trip.
        vm.reportStartTapRefused(com.lidarscan.core.Wording.START_NEEDS_SENSOR)
        assertNotNull(vm.startTapRefusal.value)

        vm.onScanScreenLeaving(configurationChange = false)
        vm.onScanScreenEntered()
        assertNull("a stale refusal must not greet the next scan", vm.startTapRefusal.value)
        assertTrue(logs.any { it.contains("start tap refused") })
    }
}
