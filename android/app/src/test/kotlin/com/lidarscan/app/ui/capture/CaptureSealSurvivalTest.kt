@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.capture

import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.PerformancePreset
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineTarget
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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 6, owner item 20 — **"the capture not saved to the phone, it just gone
 * and the app project not see any saved"**, driven through the real
 * `CaptureViewModel`.
 *
 * `CaptureViewModelMultiCycleTest` (ROUND 5 AUDIT) already proves two Start/Stop
 * cycles make two projects. It could not have caught this round's bug, because
 * `FakeEngineBridge` writes nothing to disk — and the bug is that the ENGINE
 * writes something to disk: its own `manifest.json`, on top of the app's.
 *
 * So the fake bridge here does what the real one does: it writes the engine's
 * container manifest into the project directory at start and at stop, byte-shape
 * for byte-shape with `engine/src/record/lscan.cpp`'s `write_manifest()`. Against
 * 0.2.1's `FileProjectStore` the assertions below fail exactly the way the field
 * report describes — a completed capture, and an empty Projects list.
 */
class CaptureSealSurvivalTest {

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

    /**
     * [FakeEngineBridge] plus the one behaviour that matters here: a real
     * `scan_engine_start()` opens a `FileRecordWriter` on the project directory,
     * which immediately writes the engine's `manifest.json` — and writes it
     * again, sealed, at `scan_engine_stop()`.
     */
    private class RecordingFakeBridge(
        private val delegate: FakeEngineBridge = FakeEngineBridge(),
    ) : EngineBridge by delegate {
        private var dir: File? = null

        override suspend fun startCapture(
            projectDirectory: String,
            liveSlam: Boolean,
            profile: String,
        ): Result<Unit> {
            val result = delegate.startCapture(projectDirectory, liveSlam, profile)
            if (result.isSuccess) {
                dir = File(projectDirectory).also { writeEngineManifest(it, sealed = false, profile = profile) }
            }
            return result
        }

        override suspend fun stopCapture(): Result<Unit> {
            val result = delegate.stopCapture()
            dir?.let { writeEngineManifest(it, sealed = true, profile = "quickscan") }
            return result
        }

        /**
         * Best-effort, exactly like `FileRecordWriter::write_manifest()`, which
         * returns a `Status` rather than throwing when the directory has gone.
         */
        private fun writeEngineManifest(directory: File, sealed: Boolean, profile: String) = runCatching {
            File(directory, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(
                """
                {
                  "schemaVersion": 1,
                  "formatVersion": 1,
                  "engineVersion": "0.9.0",
                  "createdAtUtcNs": 1755446000000000000,
                  "sealed": $sealed,
                  "profile": "$profile",
                  "sensors": [],
                  "mountCalibration": null,
                  "crs": null,
                  "clockOffsets": {},
                  "streams": {"lidar.bin": {"stream": 1}}
                }
                """.trimIndent(),
            )
        }.let { }
    }

    private fun tempRoot(): File = File.createTempFile("captureSeal", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    @Test
    fun `a completed capture is listed in the project store even though the engine rewrote manifest json`() =
        runBlocking {
            val root = tempRoot()
            val store = FileProjectStore(root, appVersion = "0.3.0")
            val series = AtomicInteger(0)
            val logLines = mutableListOf<String>()

            val vm = CaptureViewModel(
                engineBridge = RecordingFakeBridge(),
                projectStore = store,
                autoDetectors = listOf(ImmediateD6Detector()),
                claimSeriesNumber = { series.incrementAndGet() },
                peekSeriesNumber = { series.get() + 1 },
                logEvent = { tag, message -> synchronized(logLines) { logLines += "[$tag] $message" } },
            )

            withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

            vm.startCapture()
            withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
            kotlinx.coroutines.delay(700) // let a FakeEngineBridge stats tick land
            val projectId = (vm.uiState.value as CaptureUiState.Loaded).project.id

            vm.stopCapture()
            withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
            withTimeout(5_000) { vm.uiState.first { it is CaptureUiState.NewScan } }

            // The engine really did overwrite manifest.json — if it did not,
            // this test is not testing anything.
            val engineManifest = File(File(root, projectId), FileProjectStore.ENGINE_MANIFEST_FILE_NAME)
            assertTrue("the fake must have written the engine manifest", engineManifest.isFile)
            assertTrue(engineManifest.readText().contains("\"engineVersion\""))

            // THE assertion the field report is about.
            val listed = store.list()
            assertEquals("the finished capture must be in the Projects listing", 1, listed.size)
            assertEquals(projectId, listed.single().id)
            assertTrue(
                "and must be sealed with the points it captured",
                (listed.single().manifest.pointCountEstimate ?: 0L) > 0L,
            )
            assertFalse(
                "a project sealed by this version is a normal project, not a recovery",
                listed.single().manifest.recovered,
            )

            // No save error, a saved path to show, and evidence in the log.
            assertNull("a successful capture must not raise a save error", vm.saveError.value)
            assertNotNull("the summary must be able to say where it went", vm.lastSavedProject.value)
            val log = synchronized(logLines) { logLines.toList() }
            assertTrue("the start must be logged", log.any { it.startsWith("[session] start:") })
            assertTrue("the seal verdict must be logged", log.any { it.contains("sealed OK") })
        }

    @Test
    fun `a seal that cannot be written raises a loud save error instead of failing silently`() = runBlocking {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.3.0")
        val vm = CaptureViewModel(
            engineBridge = RecordingFakeBridge(),
            projectStore = store,
            autoDetectors = listOf(ImmediateD6Detector()),
            claimSeriesNumber = { 1 },
            peekSeriesNumber = { 2 },
        )
        withTimeout(5_000) { vm.autoConnectState!!.first { it.isPreviewing } }

        vm.startCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.RECORDING } }
        kotlinx.coroutines.delay(400)
        val project = (vm.uiState.value as CaptureUiState.Loaded).project

        // The phone loses the storage the project lives on mid-capture — a
        // removed SD card, a wiped external tree, a full disk. 0.2.1 discarded
        // `updateManifest`'s null and showed a triumphant summary sheet.
        project.directory.deleteRecursively()

        vm.stopCapture()
        withTimeout(5_000) { vm.captureState.first { it == CaptureState.IDLE } }
        withTimeout(5_000) { vm.saveError.first { it != null } }

        val error = vm.saveError.value
        assertNotNull("a failed seal must be surfaced", error)
        assertTrue(
            "the message must name where the data was, so it can be rescued: $error",
            error!!.contains(project.directory.absolutePath) || error.contains(project.id),
        )
        assertNull("and it must not claim a saved path", vm.lastSavedProject.value)
    }

    @Test
    fun `presets prefill every parameter and an individual edit survives as CUSTOM`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "0.3.0")
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            deviceTier = com.lidarscan.core.capture.DeviceTier.FLAGSHIP,
            displayCeilingHz = 120,
        )

        // Item 21: the default is never the maximum.
        assertEquals(PerformancePreset.OPTIMAL, vm.preset.value)
        assertTrue("default refresh must be capped, not Max", vm.refreshHz.value > 0)
        assertTrue("default point budget must be below the 20 M slider top", vm.lodBudgetMPoints.value < 20)

        // Item 22: Light prefills, and says what it changed.
        vm.setPreset(PerformancePreset.LIGHT)
        assertEquals(PerformancePreset.LIGHT, vm.preset.value)
        assertFalse("Light draws no live map", vm.liveMapEnabled.value)
        assertFalse("Light writes no keyframes", vm.keyframesEnabled.value)
        assertNotNull("switching must report what it changed", vm.presetChangeNote.value)
        assertTrue(vm.presetChangeNote.value!!.contains("live 3D map off"))

        // Item 22: an advanced user's own edit survives, and simply reads CUSTOM.
        vm.setLodBudgetMPoints(14)
        assertEquals(14, vm.lodBudgetMPoints.value)
        assertEquals(PerformancePreset.CUSTOM, vm.preset.value)
        assertFalse("the edit must not have reverted the rest of Light", vm.liveMapEnabled.value)

        // And going back to a preset re-prefills.
        vm.setPreset(PerformancePreset.FULL)
        assertEquals(PerformancePreset.FULL, vm.preset.value)
        assertEquals(20, vm.lodBudgetMPoints.value)
        assertTrue(vm.liveMapEnabled.value)
    }

    @Test
    fun `the live map is not drawn until a map actually exists, and Light suppresses it entirely`() = runBlocking {
        val store = FileProjectStore(tempRoot(), appVersion = "0.3.0")
        val vm = CaptureViewModel(
            engineBridge = FakeEngineBridge(),
            projectStore = store,
            deviceTier = com.lidarscan.core.capture.DeviceTier.STANDARD,
            displayCeilingHz = 60,
        )
        // No live SLAM, no pushbroom yet: raw only. This is the state a D6
        // session was permanently stuck in before ROUND 6, because the filter
        // read `liveSlam` (false on this tab) instead of "is a map being made".
        assertFalse(vm.liveMapRequested.value)

        vm.setLiveSlam(true)
        withTimeout(2_000) { vm.liveMapRequested.first { it } }
        assertTrue("live SLAM on means the map is drawn", vm.liveMapRequested.value)

        vm.setPreset(PerformancePreset.LIGHT)
        withTimeout(2_000) { vm.liveMapRequested.first { !it } }
        assertFalse("Light suppresses the map regardless of live SLAM", vm.liveMapRequested.value)
    }
}
