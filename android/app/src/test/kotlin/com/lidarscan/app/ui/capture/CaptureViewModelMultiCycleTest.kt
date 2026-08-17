@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

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
 * ROUND 5 AUDIT (task 2, multi-cycle recording): "Start -> Stop (seal) ->
 * Start again within one connect session MUST produce a second valid
 * recording."
 *
 * This drives the real `CaptureViewModel` — no Android framework, no
 * emulator — against `:core`'s `FakeEngineBridge` and a real
 * `FileProjectStore` over a temp directory, exactly mirroring the Capture
 * tab's own "no project id, auto-detect on entry" construction
 * (`CaptureScreen.kt`'s `CaptureRoute`). `viewModelScope` needs a `Main`
 * dispatcher outside Android; `Dispatchers.Unconfined` is used (not a
 * `TestDispatcher`) specifically so `FakeEngineBridge`'s own real
 * `delay(400ms)` connect simulation and `500ms` stats ticks resolve in real
 * wall-clock time rather than needing a virtual-time scheduler nobody here
 * is driving.
 *
 * This is the regression test for the bug this audit found and fixed in
 * `CaptureViewModel.stopCapture()`: `_uiState` used to stay `Loaded(project1)`
 * after a Stop, so `startCapture()`'s own `(_uiState.value as? Loaded)
 * ?.project ?: createProjectForThisScan()` fallback silently re-recorded into
 * project #1 on the second Start instead of creating project #2.
 */
class CaptureViewModelMultiCycleTest {

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

    @Test
    fun `start stop start within one connect session creates a second sealed project`() = runBlocking {
        val tempDir = File.createTempFile("captureVmTest", "").let {
            it.delete()
            it.mkdirs()
            it
        }
        val store = FileProjectStore(tempDir, appVersion = "test")
        val engine = FakeEngineBridge()
        val series = AtomicInteger(0)

        val vm = CaptureViewModel(
            engineBridge = engine,
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
        )

        // Auto-detect races on init (round 5 item 7) — wait for the fake D6 to
        // be "found" and connected, exactly like a real device would be.
        withTimeout(5_000) {
            vm.autoConnectState!!.first { it.isPreviewing }
        }
        assertEquals(
            "the tab must open with nothing to record into yet",
            true,
            vm.uiState.value is CaptureUiState.NewScan,
        )

        // --- cycle 1 ---------------------------------------------------
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        // Let at least one FakeEngineBridge stats tick land (500ms) so the
        // sealed manifest's pointCountEstimate is genuinely non-zero, not just
        // structurally present.
        kotlinx.coroutines.delay(700)
        val loaded1 = vm.uiState.value as? CaptureUiState.Loaded
            ?: error("expected a Loaded project while recording cycle 1, got ${vm.uiState.value}")
        val project1Id = loaded1.project.id

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        // The bug this audit fixed: `_uiState` used to stay `Loaded(project1)`
        // here. It must instead return to a re-armable NewScan state so the
        // NEXT Start creates a genuinely new project.
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

        // The connect session itself must survive the stop untouched — same
        // sensor, still previewing, no re-detect needed for cycle 2.
        assertTrue("the connect session must survive Stop", vm.autoConnectState!!.value.isPreviewing)

        // --- cycle 2, same connect session ------------------------------
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        val loaded2 = vm.uiState.value as? CaptureUiState.Loaded
            ?: error("expected a Loaded project while recording cycle 2, got ${vm.uiState.value}")
        val project2Id = loaded2.project.id

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

        // --- the actual assertions ---------------------------------------
        assertNotEquals("Start #2 must create a DIFFERENT project than Start #1", project1Id, project2Id)

        val onDisk = store.list()
        assertEquals("two Start/Stop cycles must leave two sealed projects on disk", 2, onDisk.size)
        for (project in onDisk) {
            assertTrue(
                "project ${project.id} must be sealed with a non-empty point count, got " +
                    "${project.manifest.pointCountEstimate}",
                (project.manifest.pointCountEstimate ?: 0L) > 0L,
            )
        }
    }
}
