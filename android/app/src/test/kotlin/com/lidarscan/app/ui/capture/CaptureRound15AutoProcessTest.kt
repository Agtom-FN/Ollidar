@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.MountVerdict
import com.lidarscan.core.capture.SelfCheck
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.capture.StitchResult
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
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
 * ROUND 15 item 55 — **Stop → seal → process → card → Projects.**
 *
 * The thing this file exists to protect is the ORDER, because every failure
 * mode of "do more work at Stop" is an ordering failure:
 *
 *  * processing must start only after the container has been sealed AND
 *    re-opened successfully, so that nothing it does can lose a scan;
 *  * the navigation to Projects must be emitted whatever processing does —
 *    a failed or slow reprocess must never strand the operator on the Capture
 *    tab with a scan they cannot reach;
 *  * the card must hold while it runs (that is ROUND 10's choreography: the
 *    screen navigates when `sessionSummary` goes null, and nothing here may
 *    change that) and must then show the POST-process numbers;
 *  * a capture recorded in one piece must take the fast path and still get the
 *    ROUND 12 ruler, because that is the number the card is for.
 *
 * The reprocess itself is injected, which is the point: it is handle-less in
 * production (it takes a DIRECTORY), so a bare-JVM test can drive the whole
 * flow with no native library and no engine — and the fact that it CAN is the
 * same property that makes it safe to run while the next capture is arming.
 */
class CaptureRound15AutoProcessTest {

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() { Dispatchers.resetMain() }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    private fun tempRoot(): File = File.createTempFile("round15Auto", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    private fun result(
        sections: Int,
        measurable: Boolean = true,
        ran: Boolean = true,
    ) = StitchResult(
        ran = ran,
        mapWritten = sections > 1,
        sections = sections,
        seams = sections - 1,
        seamsRefined = sections - 1,
        points = 220438,
        poses = 1210,
        posesUntracked = 0,
        movedMeters = 0.517,
        movedDegrees = 3.2,
        verticalExtentBeforeM = 0.82,
        verticalExtentAfterM = 0.27,
        endGapBeforeM = 0.0,
        endGapAfterM = 0.10,
        mountVerdict = MountVerdict.OK,
        mountImpossibleFraction = 0.0,
        selfCheck = SelfCheck(
            measurable = measurable,
            offsetMeters = 0.0197,
            floorMeters = 0.0099,
            windows = 12,
            separationSeconds = 8.0,
            p90Meters = 0.05,
        ),
    )

    private fun newVm(
        store: FileProjectStore,
        auto: suspend (File, (Float) -> Boolean) -> StitchResult?,
    ) = CaptureViewModel(
        engineBridge = FakeEngineBridge(),
        projectStore = store,
        autoDetectors = listOf(ImmediateD6Detector()),
        claimSeriesNumber = AtomicInteger(0).let { c -> { c.incrementAndGet() } },
        peekSeriesNumber = { 1 },
        runAutoProcess = auto,
    )

    private suspend fun recordAndStop(vm: CaptureViewModel) {
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    @Test
    fun `the sealed scan is processed and the card shows the post-process numbers`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val seenDir = AtomicReference<File?>(null)
        val vm = newVm(store) { dir, onProgress ->
            seenDir.set(dir)
            // The container must already be on disk when this is called — that
            // is the whole safety argument for doing this at Stop at all.
            assertTrue("processing must run on a sealed container", dir.isDirectory)
            onProgress(0.25f)
            onProgress(0.80f)
            result(sections = 5)
        }

        recordAndStop(vm)
        withTimeout(5_000) { vm.autoProcess.first { !it.running && it.result != null } }

        val state = vm.autoProcess.value
        assertNotNull("auto-process must have run", state.result)
        assertNotNull(seenDir.get())
        assertFalse(state.failed)
        assertEquals(1f, state.progress, 1e-6f)

        // POST-process numbers, on the card.
        val line = state.line!!
        assertTrue(line, line.contains("5 pieces aligned"))
        assertTrue(line, line.contains("0.82"))
        assertTrue(line, line.contains("0.27"))
        assertFalse("no unsubstituted format specifier may reach the card", line.contains("%"))

        // ROUND 15 item 57, on the same card.
        val sc = state.result!!.selfCheckLine!!
        assertTrue(sc, sc.contains("2.0 cm"))
    }

    @Test
    fun `navigation to Projects happens whether or not processing succeeds`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val vm = newVm(store) { _, _ -> throw IllegalStateException("native library missing") }

        recordAndStop(vm)

        // The scan is still there and the operator can still reach it. This is
        // the assertion that matters: a reprocess is an optional extra, and an
        // optional extra must never be able to swallow a capture.
        val id = withTimeout(5_000) { vm.sealedProjectId.first() }
        assertTrue(id.isNotEmpty())
        assertNotNull(store.open(id))

        withTimeout(5_000) { vm.autoProcess.first { it.failed } }
        val line = vm.autoProcess.value.line!!
        assertTrue(line, line.contains("the scan is saved"))
        assertTrue(line, line.contains("tap Process"))
    }

    @Test
    fun `a run that reports it did not run is a failure, not a silent success`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        // `ran = false` is what the engine returns for a container it could not
        // resolve. ROUND 13 shipped a bug where exactly this looked like
        // success (the JNI progress callback silently cancelled the whole run
        // and returned `ran = 0` with no error anywhere), so it gets its own
        // case.
        val vm = newVm(store) { _, _ -> result(sections = 1, ran = false) }
        recordAndStop(vm)
        withTimeout(5_000) { vm.autoProcess.first { it.active && !it.running } }
        assertTrue(vm.autoProcess.value.failed)
        assertNull(vm.autoProcess.value.result)
    }

    @Test
    fun `a one-piece capture takes the fast path and still gets the ruler`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val vm = newVm(store) { _, _ -> result(sections = 1) }
        recordAndStop(vm)
        withTimeout(5_000) { vm.autoProcess.first { !it.running && it.result != null } }

        val state = vm.autoProcess.value
        assertFalse("nothing to stitch on one section", state.willStitch)
        assertTrue("the fast path must be marked as such", state.skipped)
        assertTrue(state.line!!, state.line!!.contains("recorded in one piece"))
        // ...and the number is still there, which is the whole point of running
        // the cheap half.
        val sc = state.result!!.selfCheckLine!!
        assertTrue(sc, sc.contains("2.0 cm"))
    }

    @Test
    fun `dismissing the card does not cancel a run that is still going`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val gate = kotlinx.coroutines.CompletableDeferred<Unit>()
        val vm = newVm(store) { _, _ ->
            gate.await()
            result(sections = 3)
        }
        recordAndStop(vm)
        withTimeout(5_000) { vm.autoProcess.first { it.running } }

        // The operator taps Done while it is still working — which is exactly
        // what ROUND 10's flow invites, since Done is what navigates.
        vm.dismissSessionSummary()
        assertTrue("the run must survive the card", vm.autoProcess.value.running)

        gate.complete(Unit)
        withTimeout(5_000) { vm.autoProcess.first { it.active && !it.running } }
        assertNotNull(vm.autoProcess.value.result)
    }
}
