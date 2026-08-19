@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.app.ar.CaptureArController
import com.lidarscan.app.ar.StartPoseSource
import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.PoseSample
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.Collections
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 21 (item 84) — the v0.9.5 start deadlock, reproduced from the owner's
 * own log (`lidarscan-capture-log-2026-08-20-0133.txt`, 01:29:07 / 01:29:53 /
 * 01:31:47: three times `start hold: trim captured…` immediately followed by
 * `start IGNORED: a start is already in flight`, and zero seal summaries
 * after — three dead captures, each needing an app restart).
 *
 * The mechanism: `startCapture()` clears `startPending` BEFORE launching the
 * round-20 hold stage, so the hold stage's own re-entry knocked on the same
 * door a second finger-press does — the ROUND 17 in-flight atomic — which was
 * (correctly) still claimed by the very sequence trying to resume. The
 * re-entry was "IGNORED", the atomic was never released, and Start was dead
 * until process death.
 *
 * Why round 20's tests missed it: the hold stage is skipped whenever there is
 * no AR controller, and no JVM test could build one (ARCore + Context). These
 * tests drive the REAL path — tracking gate → hold-steady stage → record
 * call — through [StartPoseSource], the round-21 seam, with a fake pose ring
 * that behaves like the owner's phone did (gate not ready at first evaluate,
 * steady afterwards).
 */
class CaptureRound21Test {

    @Before fun setUp() { Dispatchers.setMain(Dispatchers.Unconfined) }

    @After fun tearDown() { Dispatchers.resetMain() }

    private class ImmediateD6Detector : SensorAutoDetector {
        override val sensor = SensorType.COIN_D6
        override suspend fun detect(): AutoDetection =
            AutoDetection(sensor = sensor, transportHint = "/dev/fake-d6", label = "COIN-D6 · fake")
    }

    /**
     * A pose ring shaped like the owner's log: empty right after the
     * world-frame reset (the gate must actually WAIT, as it did at 01:29:03),
     * then a steady 30 Hz stream that clears the gate and converges the hold.
     * Every [poseWindow] call appends ~[appendMillisPerCall] ms of samples, so
     * wall-clock ticks in the gate/hold loops see the ring grow exactly the
     * way a settling tracker's does.
     */
    private class FakeStartPoseSource(
        private val appendMillisPerCall: Long = 400L,
    ) : StartPoseSource {
        val resets = AtomicInteger(0)
        private val window = mutableListOf<PoseSample>()
        private var tNs = 1_000_000_000L

        override fun resetPoseCounters() {}

        override fun resetWorldFrame(attempts: Int): CaptureArController.ResetResult {
            resets.incrementAndGet()
            synchronized(window) { window.clear() } // a new session has a new (empty) ring
            return CaptureArController.ResetResult(ok = true, attempts = 1, yieldedFrames = 3L)
        }

        override fun poseWindow(): List<PoseSample> = synchronized(window) {
            repeat((appendMillisPerCall / 33L).toInt()) {
                tNs += 33_000_000L
                window.add(
                    PoseSample(
                        tMonoNs = tNs,
                        position = Vec3(0.0, 0.0, 0.0),
                        orientation = Quat(0.0, 0.0, 0.0, 1.0),
                        tracking = true,
                    ),
                )
            }
            window.toList()
        }
    }

    private fun tempRoot(): File = File.createTempFile("round21vm", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    private fun newVm(
        engine: FakeEngineBridge = FakeEngineBridge(),
        source: StartPoseSource? = FakeStartPoseSource(),
        logs: MutableList<String> = Collections.synchronizedList(mutableListOf()),
        store: FileProjectStore = FileProjectStore(tempRoot(), appVersion = "test"),
        watchdogMillis: Long = CaptureViewModel.START_WATCHDOG_MS,
    ): Pair<CaptureViewModel, MutableList<String>> {
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = engine,
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
            startPoseSource = source,
            startWatchdogMillis = watchdogMillis,
            logEvent = { tag, line -> logs.add("[$tag] $line") },
        )
        return vm to logs
    }

    private suspend fun awaitLog(logs: List<String>, needle: String, timeoutMs: Long = 8_000) {
        withTimeout(timeoutMs) {
            while (logs.none { it.contains(needle) }) kotlinx.coroutines.delay(20)
        }
    }

    // ── the owner's failure, verbatim shape ─────────────────────────────────

    @Test
    fun `the hold stage re-entry reaches the record call with the atomic held`(): Unit = runBlocking {
        val (vm, logs) = newVm()
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture(skipChecklist = true)

        // The full owner sequence must land: gate waited, gate cleared, hold
        // converged, trim captured IN THE SCAN'S OWN FRAME — and then, unlike
        // 0.9.5, the recording must actually begin.
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }

        assertTrue("the gate must have actually waited", logs.any { it.contains("start gate: waiting for tracking") })
        assertTrue("the hold stage must have run", logs.any { it.contains("start hold: trim captured in the scan's own frame") })
        assertFalse(
            "the sequence's own re-entry must never be IGNORED — that is the v0.9.5 deadlock:\n" +
                logs.joinToString("\n"),
            logs.any { it.contains("start IGNORED") },
        )
        assertTrue("the engine must have been told to record", logs.any { it.contains("[session] start: project=") })

        // The latch must be RELEASED once recording runs — a held latch is a
        // dead Start button even when this capture succeeded.
        assertFalse("starting must have resolved", vm.starting.value)

        vm.stopCapture()
        withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    @Test
    fun `a second finger press during the hold is still ignored and pulses the panel`(): Unit = runBlocking {
        val (vm, logs) = newVm()
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture(skipChecklist = true)
        awaitLog(logs, "start hold: waiting for a steady hold")

        val pulsesBefore = vm.startProgress.value?.pulses ?: 0
        // The owner's finger, mid-hold. Round 17's one-press rule must hold…
        vm.startCapture(skipChecklist = true)
        assertTrue(
            "a finger press during the hold must be swallowed by the in-flight guard",
            logs.any { it.contains("start IGNORED: a start is already in flight") },
        )
        // …and round 21 item 85: a swallowed press must be VISIBLE.
        assertEquals(
            "a swallowed press must pulse the start-progress panel",
            pulsesBefore + 1,
            vm.startProgress.value?.pulses ?: -1,
        )

        // …while the sequence it interrupted still resolves into a recording.
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        assertEquals(
            "exactly ONE recording may begin for the two presses",
            1,
            logs.count { it.contains("[session] start: project=") },
        )

        vm.stopCapture()
        withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    // ── the owner tried three times; all three must now produce captures ────

    @Test
    fun `three consecutive start sequences each produce a sealed capture`(): Unit = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "test")
        val (vm, logs) = newVm(store = store)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        repeat(3) { cycle ->
            vm.startCapture(skipChecklist = true)
            withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
            // Let a FakeEngineBridge stats tick land so the seal has points.
            kotlinx.coroutines.delay(700)
            vm.stopCapture()
            withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
            withTimeout(10_000) { vm.uiState.first { it is CaptureUiState.NewScan } }
            assertEquals(
                "cycle $cycle: every start sequence must run the hold stage",
                cycle + 1,
                logs.count { it.contains("start hold: trim captured in the scan's own frame") },
            )
        }

        assertEquals("three Starts must leave three projects on disk", 3, store.list().size)
        assertFalse("no press may ever be IGNORED in this sequence", logs.any { it.contains("start IGNORED") })
    }

    // ── item 84: the watchdog — Start must never be permanently dead ────────

    /** An engine whose FIRST start hangs forever — the shape of any unforeseen stall. */
    private class HangingOnceEngineBridge : FakeEngineBridge() {
        private val hung = AtomicBoolean(false)
        override suspend fun startCapture(projectDirectory: String, liveSlam: Boolean, profile: String): Result<Unit> {
            if (hung.compareAndSet(false, true)) kotlinx.coroutines.awaitCancellation()
            return super.startCapture(projectDirectory, liveSlam, profile)
        }
    }

    @Test
    fun `the watchdog releases a start sequence that never resolves`(): Unit = runBlocking {
        val engine = HangingOnceEngineBridge()
        // No pose source: the sequence goes straight to the (hanging) record
        // call, which is the stall the stage timeouts cannot cover.
        val (vm, logs) = newVm(engine = engine, source = null, watchdogMillis = 500L)
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture(skipChecklist = true)
        assertTrue("the press must have claimed the latch", vm.starting.value)

        // The watchdog must fire, log, surface an actionable failure, and free the latch.
        withTimeout(8_000) { vm.starting.first { !it } }
        awaitLog(logs, "start WATCHDOG")
        val error = vm.saveError.value
        assertTrue("the failure must be on screen and actionable, got: $error", error != null && error.contains("press Start"))

        // Start must be alive again: the next press must produce a real recording.
        vm.startCapture(skipChecklist = true)
        withTimeout(15_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        vm.stopCapture()
        withTimeout(10_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }
}
