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
 * ROUND 25 item 115 — **"when the user click to other tab just stop and exit
 * the scan and tracking."**
 *
 * The owner's sentence overturns half of round 24's item 111, which
 * deliberately left a live capture alone on a tab switch. Three things now
 * happen on a real exit from the Scan tab, and each has its own failure mode
 * if it is done carelessly:
 *
 *  * **the capture is sealed by the NORMAL path** — not a quick-stop. The seal
 *    prunes an empty scan, writes the manifest and hands the container to
 *    auto-process on `containerScope`; a second stop path would drift from all
 *    three within a round.
 *  * **the seal does not navigate.** Emitting `sealedProjectId` would drag an
 *    operator who asked for Settings into Projects, and — because the flow
 *    replays — would bounce them again the next time they entered the Scan tab,
 *    which is round 23's item 101 defect arriving by a new road.
 *  * **tracking is closed, not paused.** A paused ARCore session still holds
 *    the camera. `shutDownTracking` is injected here precisely so this suite
 *    can prove it fired, once, and never on a rotation.
 *
 * The harness is round 23's and round 24's: ONE ViewModel for the whole test,
 * as item 88 leaves it, with the screen's entry and exit calls made by hand
 * where the composition would make them.
 */
class CaptureRound25Test {

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

    private fun tempRoot(): File = File.createTempFile("round25vm", "").let {
        it.delete(); it.mkdirs(); it
    }

    /** Everything item 115 changes, observable from a bare JVM. */
    private class Probe {
        val logs = CopyOnWriteArrayList<String>()
        val trackingShutdowns = AtomicInteger(0)
        val savedInBackground = CopyOnWriteArrayList<String>()
        val autoProcessed = CopyOnWriteArrayList<String>()
    }

    private fun newVm(probe: Probe): CaptureViewModel {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            runAutoProcess = { dir, _ ->
                probe.autoProcessed += dir.absolutePath
                null
            },
            shutDownTracking = { probe.trackingShutdowns.incrementAndGet() },
            onScanSavedInBackground = { id -> probe.savedInBackground += id },
            logEvent = { tag, line -> probe.logs.add("[$tag] $line") },
        )
        built += vm
        return vm
    }

    /**
     * Wait for something the SEAL produces.
     *
     * `captureState == IDLE` is set while `sealAndStopLocked` still has work to
     * do — the manifest write, the auto-process hand-off and the navigation
     * decision all come after it — so a test that asserts the instant the state
     * flips is asserting against a seal that is halfway done. Polling rather
     * than adding a "seal finished" flow: the flow would exist only for the
     * tests, and a production signal invented for a test is a signal nobody
     * maintains.
     */
    private suspend fun awaitProbe(what: String, check: () -> Boolean) {
        withTimeout(15_000) {
            while (!check()) kotlinx.coroutines.delay(20)
        }
        check().let { if (!it) throw AssertionError(what) }
    }

    private suspend fun recordFor(vm: CaptureViewModel, millis: Long = 700) {
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(millis)
    }

    // ── the owner's sentence ────────────────────────────────────────────────

    /**
     * Leave mid-recording: the scan stops, seals **exactly once**, and the
     * auto-process that belongs to the container scope still runs.
     *
     * "Exactly once" is the assertion that matters. `sealPending` is a
     * compare-and-set and `stopCapture` is idempotent by design, but item 115
     * adds a SECOND caller of it — the screen's teardown — beside the operator's
     * STOP button, and a double seal would write the manifest twice and
     * auto-process the same container twice.
     */
    @Test
    fun `leaving mid-recording seals exactly once and still auto-processes`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        recordFor(vm)

        vm.onScanScreenLeaving(configurationChange = false)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        awaitProbe("the seal never completed") {
            probe.logs.any { it.contains("sealed after leaving the Scan tab") }
        }

        assertEquals(
            "the scan must be sealed exactly once",
            1,
            probe.logs.count { it.contains("sealed after leaving the Scan tab") },
        )
        assertEquals(
            "…and auto-process must still have run on its container",
            1,
            probe.autoProcessed.size,
        )
        assertTrue(
            "the stop must be logged as the owner's item 115, not as a mystery",
            probe.logs.any { it.contains("scan tab left while") && it.contains("stopping and sealing") },
        )
    }

    /**
     * A scan sealed by walking away must NOT navigate — and must say so
     * somewhere the operator will see it.
     *
     * The two halves are one property: suppressing the navigation without the
     * notice would be the app silently discarding a walk, and emitting the
     * navigation would re-arm the round-23 bounce (the flow replays, so the id
     * would still be buffered on the next entry into the Scan tab).
     */
    @Test
    fun `a seal caused by leaving does not navigate, and reports on Projects`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        recordFor(vm)

        vm.onScanScreenLeaving(configurationChange = false)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        awaitProbe("Projects was never told") { probe.savedInBackground.isNotEmpty() }

        assertEquals(
            "Projects must be told, once, which scan was saved",
            1,
            probe.savedInBackground.size,
        )
        assertTrue(
            "the navigation event must never have been emitted",
            probe.logs.none { it.contains("navigate -> Projects") },
        )
        // And the buffer is genuinely empty, not merely un-logged: a replayed
        // id here is the exact defect of round 23 item 101.
        assertNull(
            "no seal navigation may be buffered for the next entry",
            vm.sealedProjectId.replayCache.firstOrNull(),
        )
    }

    /**
     * The very next Stop the operator presses navigates normally.
     *
     * The suppression flag is per-seal. A sticky one would silently break
     * "Stop → the viewer opens" for the rest of the session — a bug needing two
     * scans to reproduce, which is the kind that ships.
     */
    @Test
    fun `the suppression is spent, so the next ordinary Stop navigates`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        recordFor(vm)
        vm.onScanScreenLeaving(configurationChange = false)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        awaitProbe("the first seal never completed") { probe.savedInBackground.size == 1 }

        // Back to the Scan tab (item 111: a fresh entry is a new scan) and a
        // normal walk, ended with the button.
        vm.onScanScreenEntered()
        recordFor(vm)
        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }

        val sealed = withTimeout(15_000) { vm.sealedProjectId.first() }
        assertNotNull("an ordinary Stop must still navigate", sealed)
        assertEquals(
            "…and only the FIRST scan may have been reported as a background save",
            1,
            probe.savedInBackground.size,
        )
    }

    /**
     * Leave mid-start: the latch is released, so the Scan button is alive on
     * the way back in.
     *
     * This is the round-21 "Start is dead until the app is killed" failure
     * arriving by a new road — the sequence's owner was disposed, not released
     * — and item 115 adds a brand new way to dispose it.
     */
    @Test
    fun `leaving mid-start cancels the sequence and releases the latch`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        // Leave while the sequence is still resolving. `starting` is the flag
        // the button renders, so it is the one the test waits for.
        withTimeout(15_000) { vm.starting.first { it } }

        vm.onScanScreenLeaving(configurationChange = false)

        assertTrue(
            "the cancellation must be logged with what it cancelled",
            probe.logs.any { it.contains("scan tab left mid-start") },
        )
        // Back in, and the button must work: `startCapture` refuses while the
        // latch is held, so a start that reaches RECORDING is proof it is not.
        vm.onScanScreenEntered()
        assertTrue("the latch must be released", !vm.starting.value)
        recordFor(vm)
        assertEquals(CaptureState.RECORDING, vm.captureState.value)
        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        Unit
    }

    // ── the battery half ────────────────────────────────────────────────────

    /**
     * Tracking is shut down on every real exit, including from an idle tab.
     *
     * The idle case is not an afterthought: a Scan tab merely previewing a
     * connected D6 has an ARCore session open too, and item 115's "no
     * background camera/battery use" is about that case just as much as about a
     * recording. `close()` shuts the gate as well as releasing the camera, so
     * after this a pose pump that somehow outlived its view cannot drive a
     * frame — which is the property, rather than "we called pause somewhere".
     */
    @Test
    fun `leaving an idle Scan tab still closes the tracking session`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        withTimeout(15_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.onScanScreenLeaving(configurationChange = false)

        assertEquals("tracking must be shut down exactly once", 1, probe.trackingShutdowns.get())
        assertTrue(
            probe.logs.any { it.contains("tracking session closed and the pose pump released") },
        )
    }

    /** And on the recording exit too, after the seal has been asked for. */
    @Test
    fun `leaving mid-recording also closes the tracking session`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        recordFor(vm)

        vm.onScanScreenLeaving(configurationChange = false)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }

        assertEquals(1, probe.trackingShutdowns.get())
    }

    // ── the rotation, which is not leaving ──────────────────────────────────

    /**
     * A **rotation** stops nothing and closes nothing.
     *
     * This is what round 24's `a tab switch mid-recording does not touch the
     * capture` was really protecting, and item 115 must not take it with it:
     * turning the phone sideways mid-walk producing the identical Compose
     * sequence is exactly why item 111 needed the Activity's
     * `isChangingConfigurations` in the first place.
     */
    @Test
    fun `a rotation mid-recording leaves the capture alone`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        recordFor(vm)

        vm.onScanScreenLeaving(configurationChange = true)
        vm.onScanScreenEntered()

        assertEquals(
            "a rotation must never stop a scan",
            CaptureState.RECORDING,
            vm.captureState.value,
        )
        assertEquals(
            "…and must never close the tracking session",
            0,
            probe.trackingShutdowns.get(),
        )
        assertTrue(probe.savedInBackground.isEmpty())

        vm.stopCapture()
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        Unit
    }

    /**
     * The configuration flag is still consumed by the entry that reads it, with
     * item 115 hanging off the same call. A sticky flag would mean one rotation
     * disabled the stop-on-leave for the life of the ViewModel.
     */
    @Test
    fun `a rotation does not disarm the next real exit`() = runBlocking {
        val probe = Probe()
        val vm = newVm(probe)
        recordFor(vm)

        vm.onScanScreenLeaving(configurationChange = true)
        vm.onScanScreenEntered()
        assertEquals(0, probe.trackingShutdowns.get())

        vm.onScanScreenLeaving(configurationChange = false)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.IDLE } }
        assertEquals("the next REAL exit must still stop everything", 1, probe.trackingShutdowns.get())
        awaitProbe("the seal never reported") { probe.savedInBackground.size == 1 }
    }
}
