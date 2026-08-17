package com.lidarscan.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 6, owner item 20 — **the data-loss bug, proven against the REAL native
 * engine on a real device.**
 *
 * `CaptureSealSurvivalTest` (JVM) proves the same thing against a fake bridge
 * that imitates what `engine/src/record/lscan.cpp` writes. This one does not
 * imitate anything: it creates a genuine `scan_engine*` through the JNI, starts
 * a genuine recording session on a genuine `FileProjectStore` project
 * directory — which makes `FileRecordWriter::open()` write its OWN
 * `manifest.json` there, exactly as it does in the field — stops it (which
 * writes the sealed one), and then asks the only question the owner cares
 * about:
 *
 * > is the capture still in the Projects listing?
 *
 * Against 0.2.1's store this fails: `list()` returns empty, `open()` returns
 * null, and `updateManifest()` refuses to seal, because the app's project
 * metadata lived in the file the engine had just overwritten.
 *
 * No sensor is attached and none is needed. `Engine::start()` opens the
 * recorder before it starts any device (`engine.cpp`'s `if (cfg.record)` block
 * runs first), and it is the recorder — not a device — that writes the
 * colliding file.
 */
@RunWith(AndroidJUnit4::class)
class CaptureSurvivesEngineSessionTest {

    private lateinit var root: File
    private var engineHandle: Long = 0L

    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        // The same TMPDIR fix-up AppContainer does; harmless here and required
        // if the engine ever touches its temp path during create.
        if (ScanEngineNative.isAvailable) {
            ScanEngineNative.nativeSetTempDir(context.cacheDir.absolutePath)
        }
        root = File(context.getExternalFilesDir(null) ?: context.filesDir, "Round6SealTest").also {
            it.deleteRecursively()
            it.mkdirs()
        }
    }

    @After
    fun tearDown() {
        if (engineHandle != 0L) {
            ScanEngineNative.nativeStopSession(engineHandle)
            ScanEngineNative.nativeDestroyEngine(engineHandle)
            engineHandle = 0L
        }
        root.deleteRecursively()
    }

    @Test
    fun aRealEngineSessionLeavesTheProjectListableAndSealable() {
        assumeTrue("needs the native engine (scanengine_jni)", ScanEngineNative.isAvailable)

        val store = FileProjectStore(root, appVersion = BuildConfig.VERSION_NAME)
        val project = store.create(
            "Scan-999-2026-08-17-2359",
            SensorType.COIN_D6,
            WorkflowProfile.QUICK_SCAN,
        )
        assertTrue(
            "the app's own record must exist before the engine touches anything",
            File(project.directory, FileProjectStore.APP_MANIFEST_FILE_NAME).isFile,
        )

        engineHandle = ScanEngineNative.nativeCreateEngine("round6-seal-test", 2, 32 * 1024, 64, 0)
        assertTrue("scan_engine_create failed: ${ScanEngineNative.nativeLastError()}", engineHandle != 0L)

        // The real thing: this is what runs on every Start.
        val startErr = ScanEngineNative.nativeStartSession(
            engineHandle,
            project.directory.absolutePath,
            "quickscan",
            /* record = */ true,
            /* liveSlam = */ false,
        )
        assertEquals(
            "scan_engine_start failed: ${ScanEngineNative.nativeErrorStr(startErr)} " +
                "(${ScanEngineNative.nativeLastError()})",
            ScanEngineNative.ErrorCode.OK,
            startErr,
        )

        // Proof that the collision this test is about really happened: the
        // ENGINE wrote its own manifest.json into the app's project directory.
        val engineManifest = File(project.directory, FileProjectStore.ENGINE_MANIFEST_FILE_NAME)
        assertTrue("the engine must have written its container manifest", engineManifest.isFile)
        val engineJson = engineManifest.readText()
        assertTrue(
            "and it must be the ENGINE's schema, not the app's: $engineJson",
            engineJson.contains("\"engineVersion\"") && engineJson.contains("\"formatVersion\""),
        )

        val stopErr = ScanEngineNative.nativeStopSession(engineHandle)
        assertEquals(
            "scan_engine_stop failed: ${ScanEngineNative.nativeErrorStr(stopErr)}",
            ScanEngineNative.ErrorCode.OK,
            stopErr,
        )
        assertTrue(
            "the sealed engine manifest must be there too",
            engineManifest.readText().contains("\"sealed\""),
        )

        // ── the assertions the owner's report is about ─────────────────────
        val listed = store.list()
        assertEquals("the capture must still be in the Projects listing", 1, listed.size)
        assertEquals(project.id, listed.single().id)
        assertEquals("Scan-999-2026-08-17-2359", listed.single().manifest.name)
        assertEquals(SensorType.COIN_D6, listed.single().manifest.sensor)
        assertFalse(
            "a project created by this version must not need recovering",
            listed.single().manifest.recovered,
        )

        // And the seal — the write that used to silently return null — lands
        // and reads back.
        val sealed = store.updateManifest(project.id) { it.copy(pointCountEstimate = 4_242L) }
        assertNotNull("the seal must succeed after a real engine session", sealed)
        val reopened = store.open(project.id)
        assertNotNull("and the sealed project must re-open", reopened)
        assertEquals(4_242L, reopened!!.manifest.pointCountEstimate)
    }

    /**
     * The other half of item 20: a capture that an OLD build already lost — the
     * ones sitting on the owner's phone right now — comes back, on device,
     * with its metadata rebuilt and honestly flagged.
     */
    @Test
    fun aCaptureLostByAnOldBuildIsRecoveredOnDevice() {
        assumeTrue("needs the native engine (scanengine_jni)", ScanEngineNative.isAvailable)

        val store = FileProjectStore(root, appVersion = BuildConfig.VERSION_NAME)
        val project = store.create("Scan-998-2026-08-17-2358", SensorType.COIN_D6, WorkflowProfile.SURVEY)

        engineHandle = ScanEngineNative.nativeCreateEngine("round6-recovery-test", 2, 32 * 1024, 64, 0)
        assertTrue(engineHandle != 0L)
        ScanEngineNative.nativeStartSession(
            engineHandle, project.directory.absolutePath, "survey", true, false,
        )
        ScanEngineNative.nativeStopSession(engineHandle)

        // Recreate the 0.2.1 outcome exactly: the app's record is gone, because
        // the app had been keeping it in the file the engine just overwrote.
        assertTrue(File(project.directory, FileProjectStore.APP_MANIFEST_FILE_NAME).delete())

        val recovered = store.list()
        assertEquals("the lost capture must come back", 1, recovered.size)
        assertTrue("and must say its metadata was rebuilt", recovered.single().manifest.recovered)
        assertEquals(
            "the auto-name is recoverable from the directory the app itself chose",
            "Scan-998-2026-08-17-2358",
            recovered.single().manifest.name,
        )
        assertEquals(
            "the profile comes from the engine's own session string",
            WorkflowProfile.SURVEY,
            recovered.single().manifest.profile,
        )
    }
}
