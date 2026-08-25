@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.app.ar.CaptureArController
import com.lidarscan.app.ar.StartPoseSource
import com.lidarscan.core.calib.MountTrim
import com.lidarscan.core.calib.StoredMountTrim
import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.PoseSample
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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 40 item C — **"you moved" was said to a phone that never looked.**
 *
 * ## The field report
 *
 * The first 1.0.0 user outside the owner, on an OPPO CPH2499 (Android 15),
 * reported that *"the re-zero button does not respond when clicked"*. His
 * 2026-08-25 12:13–12:16 capture log says what happened, and it is not what the
 * app told him:
 *
 * ```
 * 12:13:39.851 [ar] world frame reset: … tries=1 framesYielded=0
 * 12:13:39.852 [ar] start gate: waiting for tracking — blocker=NO_POSES
 * 12:13:43.913 [ar] start gate: timed out — blocker=NO_POSES
 * 12:14:06.514 [ar] mount hold abandoned after 30000 ms of movement      ← here
 * …
 * 12:14:45.923 [ar] mount hold abandoned after 30000 ms of movement      ← here
 * 12:14:59.728 [ar] start hold: TIMED OUT after 10000 ms — falling back …
 * ```
 *
 * Both `abandoned … of movement` lines describe a pose window that never held a
 * single sample, and the sentence the operator got — *"Could not get a still
 * enough hold. Brace the phone against your body and try again."* — is advice
 * about his hands for a fault in the tracker. He braced the phone. It could not
 * have worked. That is the defect this file pins.
 *
 * (The reason the window was empty is item A — round 28 took the live viewport
 * off the idle page and `ArPosePumpView` went with it, so nothing called
 * `Session.update()` before Start. That is a Compose-tree fact and it is fixed
 * where it lives, in `CaptureScreen`. This test is about what the app SAYS when
 * the window is empty, which stays true for the case item A cannot fix: a
 * camera that really has stopped.)
 *
 * ## Why a dead source rather than a still one
 *
 * `SteadyPoseSource` and `DriftingPoseSource` in `CaptureRound22Test` both
 * deliver frames; every existing start-hold test therefore exercises the arm
 * where the tracker works. The state that shipped to the OPPO user is the one
 * with no frames at all, and it had no test — which is how a message about
 * movement came to be printed for it.
 */
class CaptureRound40DeadTrackerTest {

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() { Dispatchers.resetMain() }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    /**
     * The OPPO's tracker: a session that was created and resumed and then
     * yielded nothing. `yieldedFrames = 0` is the same number its
     * `world frame reset` line carried.
     */
    private class DeadPoseSource : StartPoseSource {
        override fun resetPoseCounters() {}

        override fun resetWorldFrame(attempts: Int): CaptureArController.ResetResult =
            CaptureArController.ResetResult(ok = true, attempts = 1, yieldedFrames = 0L)

        override fun poseWindow(): List<PoseSample> = emptyList()
    }

    private fun tempRoot(): File = File.createTempFile("round40vm", "").let {
        it.delete(); it.mkdirs(); it
    }

    private fun newVm(
        logs: MutableList<String> = java.util.concurrent.CopyOnWriteArrayList(),
        storedTrim: StoredMountTrim? = null,
    ): Pair<CaptureViewModel, MutableList<String>> {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = FileProjectStore(tempRoot(), appVersion = "test"),
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            runAutoProcess = { _, _ -> null },
            loadStoredMountTrim = { storedTrim },
            startPoseSource = DeadPoseSource(),
            logEvent = { tag, line -> logs.add("[$tag] $line") },
        )
        return vm to logs
    }

    /** A trim from a previous run, so the fall-back has something to fall back TO. */
    private fun incumbent() = StoredMountTrim(
        trim = MountTrim(
            qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0,
            sensor = SensorType.COIN_D6,
            capturedAtEpochMillis = System.currentTimeMillis(),
            sampleCount = 240,
            spreadDeg = 0.30,
            spreadP90Deg = 0.20,
            stabilityDeg = 0.29,
        ),
        appRunId = "previous-run",
    )

    /**
     * The start gate offers a flat scan when it has no poses; the operator's
     * "start anyway" is what lets the sequence reach the hold this file is
     * about. Waiting for the block rather than sleeping keeps the test honest
     * about the order those two steps happen in.
     */
    private suspend fun acceptTheFlatScan(vm: CaptureViewModel) {
        withTimeout(60_000) { vm.startBlock.first { it != null } }
        vm.startAnyway()
    }

    @Test
    fun `the start hold blames the camera, not the operator, when no pose ever arrives`(): Unit = runBlocking {
        val (vm, logs) = newVm(storedTrim = incumbent())
        withTimeout(8_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture(skipChecklist = true)
        // A dead tracker HOLDS the start rather than blocking it: the gate asks
        // before recording a scan it knows will be flat, exactly as the OPPO
        // log's 12:14:41 `start anyway: the operator accepted a flat scan`.
        // Answering it is the operator's part, and the test plays him.
        acceptTheFlatScan(vm)
        withTimeout(60_000) { vm.captureState.first { it == CaptureState.RECORDING } }

        val timeout = logs.firstOrNull { it.contains("start hold: TIMED OUT") }
        assertNotNull(
            "the fall-back is a decision and is logged:\n" + logs.joinToString("\n"),
            timeout,
        )
        assertTrue(
            "and it names the cause it can actually see — no poses, not a bad hold:\n$timeout",
            timeout!!.contains("NO POSES"),
        )

        val note = vm.mountTrimNote.value
        assertNotNull("the operator is told something", note)
        assertTrue(
            "the sentence names the camera: $note",
            note!!.contains("No position tracking") && note.contains("no frames"),
        )
        assertFalse(
            "and it must NOT ask him to hold stiller — that is the OPPO bug: $note",
            note.contains("steady hold") || note.contains("Brace"),
        )

        vm.stopCapture()
        withTimeout(20_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    @Test
    fun `with no saved trim the same hold says so instead of inventing one`(): Unit = runBlocking {
        val (vm, _) = newVm(storedTrim = null)
        withTimeout(8_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture(skipChecklist = true)
        acceptTheFlatScan(vm)
        withTimeout(60_000) { vm.captureState.first { it == CaptureState.RECORDING } }

        val note = vm.mountTrimNote.value
        assertNotNull(note)
        assertTrue(
            "no poses AND no incumbent is a different sentence from no poses alone: $note",
            note!!.contains("No position tracking") && note.contains("bracket defaults"),
        )

        vm.stopCapture()
        withTimeout(20_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }
}
