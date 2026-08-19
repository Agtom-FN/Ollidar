@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.app.ar.CaptureArController
import com.lidarscan.app.ar.StartPoseSource
import com.lidarscan.core.calib.MountTrim
import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.StoredMountTrim
import com.lidarscan.core.calib.Vec3
import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.MountVerdict
import com.lidarscan.core.capture.PoseSample
import com.lidarscan.core.capture.SelfCheck
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.capture.StitchResult
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.Collections
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CoroutineName
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.currentCoroutineContext
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
 * ROUND 22 — the app-side half of the six-defect stability round.
 *
 * Item 91 (the renderer) is pinned in `:core`'s `GpuPageBudgetTest`, item 92's
 * arithmetic in `:core`'s `StartHoldTrimGateTest`, item 88's navigation
 * decision in `TabNavSpecTest` and item 89's ownership in `ArSessionGateTest`.
 * What is left — and what this file is — are the two properties that only exist
 * once a real `CaptureViewModel` is driving a real start sequence and a real
 * seal:
 *
 *  * **item 90**: the post-seal auto-process runs on a scope that OUTLIVES this
 *    ViewModel, and it tells the truth about why it did not produce a result.
 *  * **item 92**: the start hold refuses a trim that is materially worse than
 *    the one already persisted, keeps sampling, and then falls back to the
 *    incumbent saying so — instead of ROUND 20's silent replacement of the
 *    owner's 0.29° trim with a 3.18° one.
 */
class CaptureRound22Test {

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() { Dispatchers.resetMain() }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    private fun tempRoot(): File = File.createTempFile("round22vm", "").let {
        it.delete(); it.mkdirs(); it
    }

    private fun okResult(sections: Int = 1, ran: Boolean = true) = StitchResult(
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
            measurable = true,
            offsetMeters = 0.0197,
            floorMeters = 0.0099,
            windows = 12,
            separationSeconds = 8.0,
            p90Meters = 0.05,
        ),
    )

    private fun newVm(
        store: FileProjectStore,
        // ROUND 22: a COPY-ON-WRITE list, not `Collections.synchronizedList`.
        //
        // `logEvent` is called from the seal, the auto-process and the start
        // sequence — three coroutines — while the assertions below iterate the
        // same list with `any`/`count`/`first`. Iterating a synchronized list
        // WITHOUT holding its monitor is unsafe by contract, and it duly threw
        // `ConcurrentModificationException` in a round-22 run. Copy-on-write
        // gives every iteration its own snapshot, which is exactly what a test
        // reading a growing log wants.
        logs: MutableList<String> = java.util.concurrent.CopyOnWriteArrayList(),
        autoScope: CoroutineScope? = null,
        storedTrim: StoredMountTrim? = null,
        poseSource: StartPoseSource? = null,
        auto: suspend (File, (Float) -> Boolean) -> StitchResult? = { _, _ -> okResult() },
    ): Pair<CaptureViewModel, MutableList<String>> {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            runAutoProcess = auto,
            autoProcessScope = autoScope,
            loadStoredMountTrim = { storedTrim },
            startPoseSource = poseSource,
            logEvent = { tag, line -> logs.add("[$tag] $line") },
        )
        return vm to logs
    }

    private suspend fun awaitLog(logs: List<String>, needle: String, timeoutMs: Long = 8_000) {
        withTimeout(timeoutMs) {
            while (logs.none { it.contains(needle) }) kotlinx.coroutines.delay(20)
        }
    }

    private suspend fun recordAndStop(vm: CaptureViewModel) {
        withTimeout(8_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        vm.stopCapture()
        withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    // ══ item 90: the auto-process runs where it can survive ═════════════════

    /**
     * **The structural proof that the job left `viewModelScope`.**
     *
     * A `CoroutineName` on the injected scope is inherited by every child and
     * by `withContext(Dispatchers.IO + NonCancellable)` (neither overrides it),
     * so reading it from inside the reprocess lambda says exactly which scope
     * launched the work. `viewModelScope` carries no name at all, which is the
     * control case below.
     *
     * This matters because the failure it fixes is invisible from the outside:
     * the engine finished, wrote byte-identical output, and the app said
     * `ran=false` — because item 88's navigation cancelled the scope the
     * awaiting coroutine belonged to, and the `runCatching` swallowed the
     * `CancellationException` that resulted.
     */
    @Test
    fun `the post-seal auto-process runs on the injected container scope, not viewModelScope`(): Unit = runBlocking {
        val marker = CoroutineName("round22-container-scope")
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default + marker)
        val observed = AtomicReference<CoroutineName?>(null)
        val (vm, _) = newVm(
            store = FileProjectStore(tempRoot(), appVersion = "test"),
            autoScope = scope,
        ) { _, _ ->
            observed.set(currentCoroutineContext()[CoroutineName])
            okResult()
        }

        recordAndStop(vm)
        withTimeout(8_000) { vm.autoProcess.first { !it.running && it.result != null } }

        assertEquals(
            "the auto-process must belong to the scope that outlives this screen",
            marker,
            observed.get(),
        )
        scope.coroutineContext[kotlinx.coroutines.Job]?.cancel()
    }

    @Test
    fun `with no injected scope the old viewModelScope behaviour is kept - the control case`(): Unit = runBlocking {
        val observed = AtomicReference<CoroutineName?>(CoroutineName("unset"))
        val (vm, _) = newVm(store = FileProjectStore(tempRoot(), appVersion = "test")) { _, _ ->
            observed.set(currentCoroutineContext()[CoroutineName])
            okResult()
        }
        recordAndStop(vm)
        withTimeout(8_000) { vm.autoProcess.first { !it.running && it.result != null } }
        assertNull("viewModelScope carries no name — the marker above is not ambient", observed.get())
    }

    // ══ item 90: three outcomes, three different sentences ══════════════════

    @Test
    fun `a reprocess that THREW names the exception class and message`(): Unit = runBlocking {
        val (vm, logs) = newVm(store = FileProjectStore(tempRoot(), appVersion = "test")) { _, _ ->
            throw IllegalStateException("libscanengine.so not found")
        }
        recordAndStop(vm)
        awaitLog(logs, "auto-process FAILED")

        val line = logs.first { it.contains("auto-process FAILED") }
        assertTrue(line, line.contains("java.lang.IllegalStateException"))
        assertTrue(line, line.contains("libscanengine.so not found"))
        assertFalse(
            "a throw is not the engine declining, and must never be printed as one:\n$line",
            line.contains("ran=false"),
        )
    }

    @Test
    fun `a reprocess that returned NULL is reported distinctly from ran equals false`(): Unit = runBlocking {
        val (vm, logs) = newVm(store = FileProjectStore(tempRoot(), appVersion = "test")) { _, _ -> null }
        recordAndStop(vm)
        awaitLog(logs, "auto-process FAILED")

        val line = logs.first { it.contains("auto-process FAILED") }
        assertTrue(line, line.contains("returned no result"))
        assertFalse(line, line.contains("ran=false"))
    }

    @Test
    fun `an engine that DECLINED is the only case that still prints ran equals false`(): Unit = runBlocking {
        val (vm, logs) = newVm(store = FileProjectStore(tempRoot(), appVersion = "test")) { _, _ ->
            okResult(ran = false)
        }
        recordAndStop(vm)
        awaitLog(logs, "auto-process FAILED")

        val line = logs.first { it.contains("auto-process FAILED") }
        assertTrue(line, line.contains("the engine declined to run (ran=false)"))
    }

    /**
     * The exact fabrication this item removes: a cancelled coroutine is not a
     * failed job, and must not be reported as one. With the auto-process on a
     * container scope a cancellation can now only come from the process going
     * away — so nothing is written to the card, and above all nothing claims
     * the engine failed.
     */
    @Test
    fun `a CANCELLED reprocess never becomes a fabricated failure`(): Unit = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val (vm, logs) = newVm(
            store = FileProjectStore(tempRoot(), appVersion = "test"),
            autoScope = scope,
        ) { _, _ -> throw kotlinx.coroutines.CancellationException("the scope went away") }

        recordAndStop(vm)
        kotlinx.coroutines.delay(600)

        assertTrue(
            "cancellation must not be laundered into an engine failure:\n" +
                logs.joinToString("\n"),
            logs.none { it.contains("auto-process FAILED") },
        )
        assertFalse("and it must not mark the card failed either", vm.autoProcess.value.failed)
        scope.coroutineContext[kotlinx.coroutines.Job]?.cancel()
    }

    @Test
    fun `the three failure reasons are three different sentences`() {
        val (vm, _) = newVm(store = FileProjectStore(tempRoot(), appVersion = "test"))
        val threw = vm.autoProcessFailureReason(IllegalStateException("boom"), null)
        val nulled = vm.autoProcessFailureReason(null, null)
        val declined = vm.autoProcessFailureReason(null, okResult(ran = false))

        assertEquals(3, setOf(threw, nulled, declined).size)
        assertTrue(threw, threw.contains("java.lang.IllegalStateException: boom"))
        assertTrue(nulled, nulled.contains("returned no result"))
        assertTrue(declined, declined.contains("ran=false"))
        // A throwable with no message must still produce a usable sentence.
        val bare = vm.autoProcessFailureReason(RuntimeException(), null)
        assertTrue(bare, bare.contains("(no message)"))
    }

    // ══ item 92: the start hold stops accepting anything ════════════════════

    /**
     * A pose ring that clears the tracking gate and converges the hold. Every
     * sample is a slightly different orientation, so the refiner produces a
     * REAL captured trim with real statistics — which is what the gate then
     * has to judge.
     */
    private class SteadyPoseSource : StartPoseSource {
        private val window = mutableListOf<PoseSample>()
        private var tNs = 1_000_000_000L
        private var n = 0

        override fun resetPoseCounters() {}

        override fun resetWorldFrame(attempts: Int): CaptureArController.ResetResult {
            synchronized(window) { window.clear() }
            return CaptureArController.ResetResult(ok = true, attempts = 1, yieldedFrames = 0L)
        }

        override fun poseWindow(): List<PoseSample> = synchronized(window) {
            repeat(12) {
                tNs += 33_000_000L
                // A hand-held "still": a few hundredths of a degree of wobble,
                // alternating so the window has a stable mean.
                val wobble = if (n++ % 2 == 0) 0.0004 else -0.0004
                window.add(
                    PoseSample(
                        tMonoNs = tNs,
                        position = Vec3(0.0, 0.0, 0.0),
                        orientation = Quat(wobble, 0.0, 0.0, 1.0).normalized(),
                        tracking = true,
                    ),
                )
            }
            window.toList()
        }
    }

    /**
     * ROUND 22 item 92 — **the owner's hold, reproduced through the REAL
     * refiner.**
     *
     * The two statistics that disagreed on 2026-08-20 are produced by two
     * different windows, and that is exactly how a 0.20° dispersion can sit
     * beside a 3.18° split-half:
     *
     *  * `MountTrimSampler.capture` — the gate — judges only the **last
     *    second** of the ring (`MountTrim.WINDOW_MS`), so a slow drift looks
     *    perfectly steady to it.
     *  * `MountTrimRefiner.splitHalfStabilityDeg` compares the mean of the
     *    **first half of the whole hold** with the mean of the second half, so
     *    the same slow drift shows up at its full size.
     *
     * This source drifts monotonically about X at 0.12° per 33 ms frame. Over
     * the eight-second hold cap that is ~28.8° of accumulated drift: a ~14.4°
     * split-half against a ~1.6° one-second dispersion, a ratio near **9**
     * against `StartHoldTrimGate.DRIFT_RATIO` of 4. The one-second window stays
     * inside `MountTrim.MAX_SPREAD_P90_DEG` (2.5°) throughout, so the gate
     * keeps saying "steady" — which is the failure being reproduced.
     *
     * **The rate is deliberately exaggerated, and the ratio does not depend on
     * it.** For a linear drift the split-half is `N/2 · rate` and the
     * one-second dispersion is about `13.5 · rate`, so the ratio is ~`N/27` —
     * a function of how many FRAMES the hold ran for, not of how fast the pose
     * moved. The rate is set high here only so the hold clears
     * `DEFAULT_TARGET_STABILITY_DEG` immediately (a hold that looks "refined"
     * ends early and never reaches the eight-second cap), which keeps the test
     * to a couple of seconds of wall clock. The owner's real drift was slower
     * and produced the same verdict over a longer hold.
     *
     * The phone is not moving in this fiction. The POSE is. That is the whole
     * point of the verdict.
     */
    private class DriftingPoseSource : StartPoseSource {
        private val window = mutableListOf<PoseSample>()
        private var tNs = 1_000_000_000L
        private var driftDeg = 0.0

        override fun resetPoseCounters() {}

        override fun resetWorldFrame(attempts: Int): CaptureArController.ResetResult {
            synchronized(window) { window.clear() }
            return CaptureArController.ResetResult(ok = true, attempts = 1, yieldedFrames = 0L)
        }

        override fun poseWindow(): List<PoseSample> = synchronized(window) {
            repeat(12) {
                tNs += 33_000_000L
                driftDeg += DEG_PER_FRAME
                val half = Math.toRadians(driftDeg) / 2.0
                window.add(
                    PoseSample(
                        tMonoNs = tNs,
                        position = Vec3(0.0, 0.0, 0.0),
                        orientation = Quat(Math.sin(half), 0.0, 0.0, Math.cos(half)).normalized(),
                        tracking = true,
                    ),
                )
            }
            window.toList()
        }

        private companion object {
            const val DEG_PER_FRAME = 0.12
        }
    }

    /**
     * An incumbent so good that no hold in this test can beat it: 0.01° of
     * split-half accuracy. Every captured candidate is therefore materially
     * worse, which is precisely the situation ROUND 20 handled by applying the
     * worse one anyway.
     */
    private fun excellentIncumbent() = StoredMountTrim(
        trim = MountTrim(
            qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0,
            sensor = SensorType.COIN_D6,
            capturedAtEpochMillis = System.currentTimeMillis(),
            sampleCount = 400,
            spreadDeg = 0.02,
            spreadP90Deg = 0.01,
            stabilityDeg = 0.01,
        ),
        appRunId = "an-earlier-run",
    )

    /**
     * **The owner's 2026-08-20 defect, end to end.** A hold that drifts under a
     * still phone used to be captured, judged "Steady… and improving", and
     * applied over a measured trim. It is now refused by name, the fall-back is
     * logged, and the capture still starts — on the better trim.
     */
    @Test
    fun `a drifting hold is refused, logged, and never replaces the persisted trim`(): Unit = runBlocking {
        val (vm, logs) = newVm(
            store = FileProjectStore(tempRoot(), appVersion = "test"),
            storedTrim = excellentIncumbent(),
            poseSource = DriftingPoseSource(),
        )
        withTimeout(8_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture(skipChecklist = true)
        // The refusal must not block the capture: recording still begins, on
        // the incumbent trim. A gate that could stop a scan would be a worse
        // bug than the one it fixes.
        withTimeout(30_000) { vm.captureState.first { it == CaptureState.RECORDING } }

        val refusal = logs.firstOrNull { it.contains("start hold: REFUSED") }
        assertNotNull(
            "a drifting hold must be refused, with its numbers:\n" + logs.joinToString("\n"),
            refusal,
        )
        assertTrue(
            "and refused as DRIFT — the specific, actionable reason:\n$refusal",
            refusal!!.contains("the pose drifted during the hold"),
        )
        assertTrue(refusal, refusal.contains("rank="))
        assertTrue(
            "the fall-back to the incumbent is a decision and must be logged",
            logs.any { it.contains("TIMED OUT") && it.contains("REFUSED") },
        )
        assertTrue(
            "the operator is told which reference the scan is running on: " + vm.mountTrimNote.value,
            vm.mountTrimNote.value?.contains("Kept the saved mount reference") == true,
        )
        assertTrue(
            "the trim in force must still be the 0.01 deg one",
            (vm.mountTrimProvenance.value.trim?.accuracyDeg ?: 9.0) < 0.02,
        )

        vm.stopCapture()
        withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    @Test
    fun `with no incumbent the same hold is accepted - the gate refuses, it does not obstruct`(): Unit = runBlocking {
        val (vm, logs) = newVm(
            store = FileProjectStore(tempRoot(), appVersion = "test"),
            storedTrim = null,
            poseSource = SteadyPoseSource(),
        )
        withTimeout(8_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture(skipChecklist = true)
        withTimeout(20_000) { vm.captureState.first { it == CaptureState.RECORDING } }

        assertTrue(
            "with nothing to compare against, a measurement beats the bracket defaults:\n" +
                logs.joinToString("\n"),
            logs.any { it.contains("start hold: trim captured in the scan's own frame") },
        )
        assertTrue(logs.none { it.contains("start hold: REFUSED") })

        vm.stopCapture()
        withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    /**
     * ROUND 22 item 92 — the `[pushbroom]` line that says which extrinsic the
     * scan is actually running on now carries the accuracy and the warning
     * flag. On 2026-08-20 that line existed and said nothing about the 3.18°
     * trim it was applying.
     */
    @Test
    fun `the extrinsic applied line carries accuracyDeg and warn`(): Unit = runBlocking {
        val (vm, logs) = newVm(
            store = FileProjectStore(tempRoot(), appVersion = "test"),
            storedTrim = excellentIncumbent(),
            poseSource = SteadyPoseSource(),
        )
        withTimeout(8_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture(skipChecklist = true)
        withTimeout(20_000) { vm.captureState.first { it == CaptureState.RECORDING } }

        awaitLog(logs, "[pushbroom] extrinsic")
        val line = logs.firstOrNull { it.contains("[pushbroom]") && it.contains("extrinsic") }
        assertNotNull("the scan must record which extrinsic it ran on:\n" + logs.joinToString("\n"), line)
        assertTrue(line!!, line.contains("accuracyDeg="))
        assertTrue(line, line.contains("warn="))

        vm.stopCapture()
        withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }
}
