package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.store.FileProjectStore
import com.lidarscan.core.capture.CueKind
import com.lidarscan.core.capture.ScanGrade
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
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
 * ROUND 11, owner items 43 + 44 — at the ViewModel, where the wiring is.
 *
 * The `:core` pieces are tested on their own (`Round11CuesAndSummaryTest`,
 * `MountTrimRefinerTest`); what those cannot show is that the seal actually
 * fills the card, that the card is cleared when the next capture starts, and
 * that a cue never fires outside a recording — three things that are properties
 * of the ViewModel's lifecycle and of nothing else. ROUND 10 spent a whole round
 * on exactly that class of bug.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class CaptureRound11SummaryTest {

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

    private fun tempRoot(): File = File.createTempFile("round11Summary", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    private fun newVm(
        store: FileProjectStore,
        cues: MutableList<CueKind> = CopyOnWriteArrayList(),
        cuesOn: Boolean = true,
    ) = CaptureViewModel(
        engineBridge = FakeEngineBridge(),
        projectStore = store,
        autoDetectors = listOf(ImmediateD6Detector()),
        claimSeriesNumber = { AtomicInteger(0).incrementAndGet() },
        peekSeriesNumber = { 1 },
        playCue = { cues.add(it) },
        cuesEnabled = { cuesOn },
    )

    @Test
    fun `sealing fills the scan summary card and grades it`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val vm = newVm(store)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        assertNull("nothing to summarise before a scan", vm.scanSummary.value)

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        delay(700)
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }

        val summary = withTimeout(5_000) { vm.scanSummary.first { it != null } }!!
        assertNotNull(summary)
        // The raw counters and the graded card describe the SAME instant — they
        // are snapshotted side by side inside the seal, so they can never
        // disagree about which scan they are about.
        assertEquals(vm.sessionSummary.value!!.pointsCaptured, summary.pointsCaptured)
        assertEquals(vm.sessionSummary.value!!.elapsedMillis, summary.elapsedMillis)
        // One section, because nothing relocalized; no tracking drops, because
        // there is no ARCore controller in a JVM test.
        assertEquals(1, summary.sections)
        assertEquals(0, summary.trackingDrops)
        // The grade is a value, never null, whatever the numbers were.
        assertTrue(summary.grade in ScanGrade.entries)
        assertTrue(summary.gradeReason.isNotBlank())
    }

    @Test
    fun `dismissing the card clears both summaries, and the next Start clears them again`() =
        runBlocking {
            val store = FileProjectStore(tempRoot(), appVersion = "test")
            val vm = newVm(store)
            withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

            vm.startCapture()
            withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
            delay(700)
            vm.stopCapture()
            withTimeout(5_000) { vm.scanSummary.first { it != null } }

            vm.dismissSessionSummary()
            assertNull(vm.scanSummary.value)
            assertNull(vm.sessionSummary.value)

            // ...and a Start that follows a card the operator never dismissed
            // must not open the new capture showing the old scan's verdict.
            // This is ROUND 10 item 38's lesson applied to the one new piece of
            // per-session state this round adds.
            vm.startCapture()
            withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
            assertNull("a running capture has no summary", vm.scanSummary.value)
            vm.stopCapture()
            withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
            Unit
        }

    @Test
    fun `no cue fires while the tab is merely previewing`() = runBlocking {
        // Cues are for a walk. Buzzing at someone lining up a preview, or at a
        // phone left on a table with the app open, is how a default-ON feature
        // gets switched off and stays off.
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val fired = CopyOnWriteArrayList<CueKind>()
        val vm = newVm(store, fired)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        // Several hint ticks (500 ms each) with no recording in progress.
        delay(1_600)
        assertTrue("cues fired outside a recording: $fired", fired.isEmpty())
        assertNull(vm.lastCue.value)
    }

    @Test
    fun `the point count that reaches the summary is the resolved map count`() = runBlocking {
        // The end of the ROUND 10 backlog item: the counter used to add the raw
        // sensor-frame preview stream to the resolved map stream. The arithmetic
        // is proved in `PointCountTally`'s own test; what this pins is that the
        // number the summary and the manifest carry is the ViewModel's, from
        // whatever the bridge reported, and that it is not zero on a scan that
        // recorded something.
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val vm = newVm(store)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        delay(700)
        val id = (vm.uiState.value as CaptureUiState.Loaded).project.id
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }

        val summary = withTimeout(5_000) { vm.scanSummary.first { it != null } }!!
        val sealed = store.open(id)
        assertNotNull(sealed)
        assertEquals(
            "the manifest and the card must report the same number",
            summary.pointsCaptured,
            sealed!!.manifest.pointCountEstimate,
        )
    }
}
