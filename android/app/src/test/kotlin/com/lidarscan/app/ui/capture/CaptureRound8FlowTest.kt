@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.calib.MountTrim
import com.lidarscan.core.calib.StoredMountTrim
import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 8, owner items 30d and 31, driven through the real [CaptureViewModel].
 *
 * Both are things that were *believed* to work and had no test that could tell:
 *
 *  * **30d** — a mount trim restored across a navigation has to reach the
 *    pushbroom, not merely reappear in the panel. ROUND 7 proved the trim
 *    survives a ViewModel rebuild (`CaptureRound7FieldBugsTest`); nothing
 *    proved the restored trim is what the extrinsic is then built from, and the
 *    log line that would say so was unreachable off-device because
 *    `applyMountExtrinsic` sat behind `arController ?: return`. ROUND 8 moved
 *    it into `startCapture` where it belongs, which is what makes this
 *    assertable on a bare JVM.
 *  * **31** — "stop => seal => Projects, and the Capture tab comes back RESET".
 *    `CaptureViewModelMultiCycleTest` already pins the half that was a data bug
 *    (a second Start must create project #2). These are the rest of the reset,
 *    including one real bug: the typed scan name was never cleared.
 */
class CaptureRound8FlowTest {

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

    private fun tempRoot(): File = File.createTempFile("round8", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    private fun trimOf(magnitudeDeg: Double, capturedAtMillis: Long): MountTrim {
        // ROUND 20 (item 79): about the HORIZONTAL Z axis, not Y — a pure-yaw
        // trim is exactly what the gravity-referenced decomposition now
        // discards on load (yaw about gravity is unobservable from a hold), so
        // a Y-axis fixture would normalise to 0 deg and stop testing what this
        // file is about: persistence. A Z-axis trim survives verbatim.
        val q = com.lidarscan.core.calib.Quat.fromAxisAngle(
            com.lidarscan.core.calib.Vec3(0.0, 0.0, 1.0),
            Math.toRadians(magnitudeDeg),
        )
        return MountTrim(
            qx = q.x, qy = q.y, qz = q.z, qw = q.w,
            sensor = SensorType.COIN_D6,
            capturedAtEpochMillis = capturedAtMillis,
            sampleCount = 31,
            spreadDeg = 0.82,
            spreadP90Deg = 0.41,
        )
    }

    // =====================================================================
    // ITEM 30d — a restored trim reaches the pushbroom
    // =====================================================================

    /**
     * The owner walks to Projects and back (which destroys the Capture tab's
     * `NavBackStackEntry` and this ViewModel), then presses Start. The extrinsic
     * that session runs on must be the restored trim, and the capture log must
     * say so with its **age** and its **origin** — because those two facts are
     * the whole reason ROUND 7 introduced `MountTrimProvenance.logSuffix`, and
     * the next field report is where they get read.
     *
     * The exact line this asserts on, from a real device, is:
     *
     * ```
     * [pushbroom] extrinsic applied: source=nominal trim=132.81deg
     *             trimAgeMs=57000 trimSource=restored-previous-run pushbroomEnabled=true
     * ```
     *
     * A JVM test has no engine handle, so it sees the sibling line
     * (`extrinsic resolved (no engine handle): …`) carrying the same suffix — a
     * deliberately different verb, so a field log can never let "we worked out
     * which extrinsic to use" be read as "the engine is using it".
     */
    @Test
    fun `a trim restored from a previous run is the extrinsic the next Start resolves`(): Unit = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "0.5.0")
        val logs = mutableListOf<String>()
        val stored = StoredMountTrim(
            trimOf(132.81, System.currentTimeMillis() - 57_000L),
            appRunId = "the-previous-run",
        )
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
            logEvent = { tag, message -> synchronized(logs) { logs += "[$tag] $message" } },
            loadStoredMountTrim = { stored },
            appRunId = "this-run",
        )
        withTimeout(5_000) { vm.mountTrim.first { it != null } }
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        withTimeout(5_000) {
            while (synchronized(logs) { logs.none { it.contains("[pushbroom] extrinsic") } }) {
                kotlinx.coroutines.delay(20)
            }
        }

        val line = synchronized(logs) { logs.last { it.contains("[pushbroom] extrinsic") } }
        // THE assertions. Before ROUND 8 no JVM test could reach this line at
        // all, and the owner's scan-009 ran on `trim=none` with nobody the wiser.
        assertTrue("the extrinsic must carry the trim: $line", line.contains("trim=132.81deg"))
        assertTrue("…its age: $line", line.contains("trimAgeMs="))
        assertTrue("…and where it came from: $line", line.contains("trimSource=restored-previous-run"))

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    /**
     * The other arm, and the one the owner's whole 18 August session ran on:
     * **with no trim at all the pushbroom is still enabled**, on the bare CAD
     * nominal.
     *
     * This matters because "set mount reference does not work" and "the live 3D
     * map does not appear" are two different complaints and it would be easy to
     * assume the first causes the second. It does not:
     * `applyMountExtrinsic` resolves `measured ?: trim?.composedWith(nominal)
     * ?: nominal`, so a null trim falls through to the nominal, the extrinsic is
     * still pushed, `pushbroom_enable` is still called, and the D6's live map
     * still draws. The field log says exactly that:
     * `extrinsic applied: source=nominal trim=none pushbroomEnabled=true`.
     */
    @Test
    fun `with no trim the extrinsic still resolves to the CAD nominal`(): Unit = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "0.5.0")
        val logs = mutableListOf<String>()
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
            logEvent = { tag, message -> synchronized(logs) { logs += "[$tag] $message" } },
            loadStoredMountTrim = { null },
            appRunId = "this-run",
        )
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        withTimeout(5_000) {
            while (synchronized(logs) { logs.none { it.contains("[pushbroom] extrinsic") } }) {
                kotlinx.coroutines.delay(20)
            }
        }
        val line = synchronized(logs) { logs.last { it.contains("[pushbroom] extrinsic") } }
        assertTrue(line, line.contains("source=nominal"))
        assertTrue(line, line.contains("trim=none"))

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
    }

    // =====================================================================
    // ITEM 31 — stop, seal, hand the shell a project id, come back re-armed
    // =====================================================================

    /**
     * The whole of item 31, minus the navigator: Stop seals, the sealed id is
     * published exactly once, and the tab is left genuinely ready to Start
     * again.
     */
    @Test
    fun `stop seals, publishes the project id, and re-arms the tab`(): Unit = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "0.5.0")
        val series = AtomicInteger(0)
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { series.incrementAndGet() },
            peekSeriesNumber = { series.get() + 1 },
        )
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }
        val firstAutoName = (vm.uiState.value as CaptureUiState.NewScan).autoName

        // The operator names this one by hand. That name is SPENT once the scan
        // it names is sealed — see below.
        vm.setScanName("Kitchen")

        val sealed = java.util.Collections.synchronizedList(mutableListOf<String>())
        val collector = kotlinx.coroutines.CoroutineScope(Dispatchers.Unconfined).launch {
            vm.sealedProjectId.collect { sealed += it }
        }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700) // one FakeEngineBridge stats tick
        val recordedId = (vm.uiState.value as CaptureUiState.Loaded).project.id
        assertTrue("the typed name must reach the project", recordedId.startsWith("kitchen"))
        assertTrue("stats must be live during the capture", vm.stats.value.pointsCaptured > 0L)

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }
        withTimeout(5_000) {
            while (sealed.isEmpty()) kotlinx.coroutines.delay(20)
        }

        // ── the navigation half ────────────────────────────────────────────
        assertEquals("exactly one seal, exactly one navigation", 1, sealed.size)
        assertEquals("…and it must name the scan that was just saved", recordedId, sealed.first())

        // ── the re-armed half ──────────────────────────────────────────────
        val next = vm.uiState.value as CaptureUiState.NewScan
        assertNotEquals(
            "the tab must offer a FRESH auto-name — the series number of scan #1 is spent",
            firstAutoName,
            next.autoName,
        )
        assertEquals(
            "the typed name is spent with the scan it named: otherwise every subsequent Start " +
                "of this session produces another project called \"Kitchen\"",
            "",
            vm.scanName.value,
        )
        assertEquals(
            "the stats line must not still be showing the finished scan's numbers",
            0L,
            vm.stats.value.pointsCaptured,
        )
        assertEquals(0L, vm.stats.value.elapsedMillis)
        // The session-summary sheet is unaffected — it was snapshotted from the
        // final stats before the reset, which is the whole reason it exists.
        assertNotNull("the summary sheet must still have the finished scan's numbers", vm.sessionSummary.value)
        assertTrue(vm.sessionSummary.value!!.pointsCaptured > 0L)

        // "still connected/armed, live preview running": the absence of code,
        // asserted.
        assertEquals(ConnectionState.CONNECTED, vm.connectionState.value)
        assertTrue("the connect session must survive Stop", vm.autoConnectState!!.value.isPreviewing)

        // And Start really is pressable again, with no reconnect in between.
        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        val secondId = (vm.uiState.value as CaptureUiState.Loaded).project.id
        assertNotEquals("Start #2 must create a different project", recordedId, secondId)
        assertTrue(
            "and it must be auto-named, not \"Kitchen\" again — got $secondId",
            !secondId.startsWith("kitchen"),
        )

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        collector.cancel()
    }

    /**
     * ROUND 7's field bug 1 must stay fixed THROUGH the new navigation.
     *
     * Item 31 makes the app walk to Projects automatically after every capture —
     * which is precisely the trip that used to silently discard 132° of mount
     * rotation. The trim is persisted now, so the automatic trip is safe; this
     * asserts that the reset added for item 31 did not quietly join the list of
     * things Stop clears.
     */
    @Test
    fun `the re-arm does not clear the mount trim`(): Unit = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "0.5.0")
        var persisted: StoredMountTrim? = StoredMountTrim(trimOf(132.81, System.currentTimeMillis()), "run-1")
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
            loadStoredMountTrim = { persisted },
            persistMountTrim = { persisted = it },
            appRunId = "run-1",
        )
        withTimeout(5_000) { vm.mountTrim.first { it != null } }
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(700)
        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

        assertNotNull("a re-armed tab must still be holding the mount reference", vm.mountTrim.value)
        assertEquals(132.81, vm.mountTrim.value!!.magnitudeDeg, 1e-6)
        assertNotNull("and it must still be on disk", persisted)
        assertEquals(
            "MOUNT SET",
            vm.mountTrimProvenance.value.chipLabel.substringBefore(" ·"),
        )
    }

    // =====================================================================
    // The live 3D map default, and item 29's display defaults
    // =====================================================================

    /**
     * Owner directive *"i need a live 3d mapping too"* + item 29, at the seam
     * where they are actually observable: the ViewModel's own initial state.
     */
    @Test
    fun `the capture tab opens on the live map, 3D orbit, intensity and one-pixel points`(): Unit = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "0.5.0")
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
        )

        // The Light preset's off-switch starts ON, because OPTIMAL is the
        // default preset and OPTIMAL draws the map.
        assertTrue("the live 3D map must be on out of the box", vm.liveMapEnabled.value)
        assertEquals(com.lidarscan.core.capture.PerformancePreset.OPTIMAL, vm.preset.value)

        // ROUND 10 (owner item 39): **3D ORBIT**, not FOLLOW. ROUND 8's
        // argument for FOLLOW — "a D6 walkthrough is a cloud you are MAKING,
        // and an orbit camera parked at the origin loses it within a few
        // metres" — is still on the ViewModel field, and it is still a good
        // argument; the owner asked for the orbit view they can turn by hand
        // while the camera is unused. Asserted through the flag, so reviving
        // FOLLOW does not leave a stale red test.
        assertEquals(
            if (com.lidarscan.core.FeatureFlags.FOLLOW_CAMERA_ENABLED) {
                com.lidarscan.app.render.CameraMode.FOLLOW
            } else {
                com.lidarscan.app.render.CameraMode.ORBIT
            },
            vm.cameraMode.value,
        )

        // Item 29's four numbers, at the ViewModel rather than only in core.
        assertEquals(com.lidarscan.core.render.ColorMode.INTENSITY, vm.colorMode.value)
        // ROUND 10 (owner item 39): "…intensity, grey scale". This was three
        // disagreeing sources before; now one.
        assertEquals(com.lidarscan.core.render.Colormap.GRAYSCALE, vm.colormap.value)
        assertEquals(1.0f, vm.pointSizePx.value, 0f)
        assertEquals(1.0f, vm.gamma.value, 0f)
        assertEquals(1.0f, vm.brightness.value, 0f)

        // …and they must survive the trip into the block the renderer and the
        // manifest actually read, `mode` included — the owner's project.json
        // recorded `"mode": "ADAPTIVE", "fixedPx": 2.5`, a control writing a
        // field its own declared mode says to ignore.
        val params = withTimeout(5_000) { vm.displayParams.first() }
        assertEquals(com.lidarscan.core.render.ColorMode.INTENSITY, params.colorMode)
        assertEquals(1.0f, params.pointSize.fixedPx, 0f)
        assertEquals(com.lidarscan.core.render.PointSizeMode.FIXED_PIXELS, params.pointSize.mode)

        // Light, and ONLY Light, turns the map off.
        vm.setPreset(com.lidarscan.core.capture.PerformancePreset.LIGHT)
        assertTrue("Light is raw preview only", !vm.liveMapEnabled.value)
        vm.setPreset(com.lidarscan.core.capture.PerformancePreset.FULL)
        assertTrue("Full draws the map", vm.liveMapEnabled.value)
        vm.setPreset(com.lidarscan.core.capture.PerformancePreset.OPTIMAL)
        assertTrue("and so does Optimal", vm.liveMapEnabled.value)
    }
}
