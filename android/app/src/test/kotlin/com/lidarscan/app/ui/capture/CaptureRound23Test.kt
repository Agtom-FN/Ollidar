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
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 23 item 101 — **the owner's exact sequence, three times, in ONE
 * process.**
 *
 * The defect: `_sealedProjectId` is a `replay = 1` `MutableSharedFlow`, and its
 * own doc comment justified that with an invariant ROUND 22 item 88 then
 * deleted — *"the buffer belongs to the ViewModel, which nav destroys on the
 * way to Projects"*. With `saveState`/`restoreState` the ViewModel survives the
 * hop, so every return to the Scan tab re-attached `CaptureRoute`'s collector,
 * the buffered id replayed, and the operator was bounced back to Projects
 * before he could press anything. His log shows the cost precisely: between
 * `seal navigate -> Projects` at 12:02:43 and a process restart at 12:05:20
 * there is not one line, from a man who was pressing the scan button.
 *
 * These tests reproduce the SHAPE of that trip on a bare JVM: the ViewModel
 * lives for the whole test (one process, one ViewModel — that is item 88's
 * behaviour) and a collector is attached and detached around each seal exactly
 * as the composition does. Before the fix, [sealNavigationHandled] does not
 * exist and the second collector receives the FIRST scan's id; after it, the
 * event is spent by the collector that acts on it.
 */
class CaptureRound23Test {

    /**
     * Every ViewModel this file builds, stopped in [tearDown].
     *
     * `CaptureViewModel` runs a 500 ms motion ticker for its whole life. Left
     * running after a test class finishes, it pokes the test `Main` dispatcher
     * while the NEXT class is calling `Dispatchers.setMain`, which fails with
     * "Main is used concurrently with setting it" — a cross-class flake with
     * nothing to do with either test. See `CaptureViewModel.shutDownForTest`.
     */
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

    private fun tempRoot(): File = File.createTempFile("round23vm", "").let {
        it.delete(); it.mkdirs(); it
    }

    private fun newVm(
        logs: MutableList<String> = CopyOnWriteArrayList(),
    ): Pair<CaptureViewModel, MutableList<String>> {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            // The seal's auto-process is item 90's business, not this file's.
            runAutoProcess = { _, _ -> null },
            logEvent = { tag, line -> logs.add("[$tag] $line") },
        )
        built += vm
        return vm to logs
    }

    /**
     * One trip through the Scan tab: press, record, stop, and then do exactly
     * what `CaptureRoute` does — attach a collector, take the id it is handed,
     * spend it, and navigate away (which detaches the collector).
     *
     * Returns the id the navigation was performed with.
     */
    private suspend fun recordSealAndNavigate(vm: CaptureViewModel): String {
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        // Coming back onto the tab is what the shell does before every press.
        vm.onScanScreenEntered()
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }

        // The composition's collector, for the lifetime of this visit only.
        val navigated = withTimeout(15_000) { vm.sealedProjectId.first() }
        vm.sealNavigationHandled(navigated)
        return navigated
    }

    // ══ item 101: the root cause ════════════════════════════════════════════

    /**
     * **The regression test for the owner's #1 complaint.**
     *
     * A collector that attaches AFTER the navigation has been performed — i.e.
     * the operator tapping back into the Scan tab — must be handed nothing. If
     * it is handed the last scan's id, `CaptureRoute` calls `onScanSealed`
     * again and the shell walks straight back to Projects, silently, which is
     * what "the scan button is dead" was.
     */
    @Test
    fun `a spent seal navigation is not replayed when the Scan tab is re-entered`(): Unit = runBlocking {
        val (vm, logs) = newVm()
        val first = recordSealAndNavigate(vm)
        assertTrue("the seal must have produced a project id", first.isNotBlank())

        // Back onto the Scan tab: a NEW composition, a NEW collector, the SAME
        // ViewModel (item 88).
        vm.onScanScreenEntered()
        val replayed = java.util.concurrent.atomic.AtomicReference<String?>(null)
        val job: Job = launch { vm.sealedProjectId.collect { replayed.set(it) } }
        kotlinx.coroutines.delay(400)
        job.cancel()

        assertNull(
            "re-entering the Scan tab must NOT re-deliver a navigation that has already " +
                "been performed — that is the bounce the owner reported as a dead scan button",
            replayed.get(),
        )
        assertTrue(
            "the consumption must be in the log, next to the navigate line it pairs with",
            logs.any { it.contains("navigation consumed id=$first") },
        )
    }

    /**
     * The ROUND 10 property that `replay = 1` exists for must survive the fix:
     * a collector that attaches LATE (an Activity recreation mid-seal) still
     * receives the id. Only a collector that has ACTED on it spends it.
     */
    @Test
    fun `a late collector still receives an unspent seal navigation - ROUND 10 stands`(): Unit = runBlocking {
        val (vm, _) = newVm()
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }

        // Nobody was listening at the moment of the emit; attach only now.
        kotlinx.coroutines.delay(300)
        val id = withTimeout(10_000) { vm.sealedProjectId.first() }
        assertTrue("the late collector must still be handed the sealed id", id.isNotBlank())
    }

    /**
     * **The owner's sequence, three times over, in one process.**
     *
     * start → record → seal → navigate to Projects → back to the Scan tab →
     * START AGAIN. Three distinct projects, three recordings, one ViewModel —
     * and after each one the ViewModel is re-armed rather than left in the
     * state the seal put it in.
     */
    @Test
    fun `three consecutive scans, one ViewModel, one process`(): Unit = runBlocking {
        val (vm, _) = newVm()
        val ids = mutableListOf<String>()
        repeat(3) {
            ids += recordSealAndNavigate(vm)
            // Back on the Scan tab, nothing is left over from the last scan.
            vm.onScanScreenEntered()
            assertTrue(
                "the tab must be re-armed for a NEW scan after seal #${ids.size}, was ${vm.uiState.value}",
                vm.uiState.value is CaptureUiState.NewScan,
            )
            assertFalse("no start may be left in flight after a seal", vm.starting.value)
        }
        assertEquals("three presses must produce three projects", 3, ids.distinct().size)
    }

    // ══ item 101(b): a refused tap is never silent ══════════════════════════

    @Test
    fun `a refused tap is logged and shown in the same words`(): Unit = runBlocking {
        val (vm, logs) = newVm()
        vm.reportStartTapRefused(com.lidarscan.core.Wording.START_NEEDS_SENSOR)

        assertEquals(
            "the screen must carry the reason",
            com.lidarscan.core.Wording.START_NEEDS_SENSOR,
            vm.startTapRefusal.value,
        )
        val line = logs.firstOrNull { it.contains("start tap refused") }
        assertNotNull("a refused tap must reach the capture log", line)
        assertTrue(
            "the log and the screen must never tell different stories: $line",
            line!!.contains(com.lidarscan.core.Wording.START_NEEDS_SENSOR),
        )

        vm.dismissStartTapRefusal()
        assertNull(vm.startTapRefusal.value)
    }

    @Test
    fun `pressing during a live capture refuses out loud instead of doing nothing`(): Unit = runBlocking {
        val (vm, logs) = newVm()
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }

        vm.startCapture(skipChecklist = true)
        withTimeout(5_000) { vm.startTapRefusal.first { it != null } }
        assertTrue(
            "the second press must be answered, not swallowed",
            logs.any { it.contains("start tap refused") },
        )

        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    // ══ item 101(a): re-entry never disturbs a live capture ═════════════════

    @Test
    fun `re-entering the tab mid-recording changes nothing`(): Unit = runBlocking {
        val (vm, logs) = newVm()
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        val projectId = (vm.uiState.value as CaptureUiState.Loaded).project.id

        // The operator checks the Projects list mid-walk and comes back.
        vm.onScanScreenEntered()

        assertEquals(
            "a tab switch during a walk must not touch the recording",
            CaptureState.RECORDING,
            vm.captureState.value,
        )
        assertEquals(
            "…nor the project it is recording into",
            projectId,
            (vm.uiState.value as CaptureUiState.Loaded).project.id,
        )
        assertFalse(
            "…and it must not claim to have re-armed anything",
            logs.any { it.contains("scan tab re-armed") },
        )

        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    // ══ item 106(a): the Detail rungs map through ═══════════════════════════

    @Test
    fun `picking a Detail rung sets the clamped budget and its preset`(): Unit = runBlocking {
        val (vm, logs) = newVm()
        val offered = vm.detailLevels
        assertTrue("Auto is always offered", com.lidarscan.core.capture.DetailLevel.AUTO in offered)

        for (level in offered) {
            vm.setDetailLevel(level)
            val expected = com.lidarscan.core.capture.DetailLevels.budgetPointsFor(
                level = level,
                tier = com.lidarscan.core.capture.DeviceTier.STANDARD,
            ) / 1_000_000
            withTimeout(5_000) { vm.lodBudgetMPoints.first { it == expected } }
            assertEquals(
                "the rung must be readable back off the budget it set",
                level,
                vm.detailLevel.value,
            )
        }
        assertTrue(
            "the choice must be in the log with the tier that clamped it",
            logs.any { it.contains("detail=") && it.contains("tier=") },
        )
    }
}
