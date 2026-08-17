package com.lidarscan.core.store

import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 6, owner item 20 — **the data-loss regression suite.**
 *
 * > "the capture not saved to the phone, it just gone and the app project not
 * > see any saved"
 *
 * Every test below writes a byte-for-byte copy of what
 * `engine/src/record/lscan.cpp`'s `FileRecordWriter::write_manifest()` actually
 * emits into `<project>.lscan/manifest.json` at `scan_engine_start()` and again
 * at `scan_engine_stop()`. That is the file the app used to keep its OWN
 * metadata in, and the collision is why a real capture made its project
 * disappear from the Projects tab.
 *
 * The first test is the reproduction: it fails against the 0.2.1
 * `FileProjectStore` and passes against this one.
 */
class ProjectManifestCollisionTest {

    private lateinit var root: File

    @Before
    fun setUp() {
        root = File.createTempFile("projectStoreRound6", "").let {
            it.delete()
            it.mkdirs()
            it
        }
    }

    private fun store() = FileProjectStore(root, appVersion = "0.3.0")

    /**
     * Exactly the JSON `write_manifest()` produces: same keys, same order, same
     * types. Nothing here is invented — see `lscan.cpp` lines 409-455.
     */
    private fun engineManifestJson(
        profile: String = "quickscan",
        createdAtUtcNs: Long = 1_755_000_000_000_000_000L,
        sealed: Boolean = true,
    ): String = """
        {
          "schemaVersion": 1,
          "formatVersion": 1,
          "engineVersion": "0.9.0",
          "createdAtUtcNs": $createdAtUtcNs,
          "sealed": $sealed,
          ${if (sealed) "\"sealedAtUtcNs\": ${createdAtUtcNs + 90_000_000_000L}," else ""}
          "profile": "$profile",
          "sensors": [],
          "mountCalibration": null,
          "crs": null,
          "clockOffsets": {},
          "streams": {"lidar.bin": {"stream": 1}, "pose_ar.bin": {"stream": 5}}
        }
    """.trimIndent()

    /** Simulates a real capture: the engine overwrites `manifest.json` in the project directory. */
    private fun simulateEngineCapture(project: Project, json: String = engineManifestJson()) {
        File(project.directory, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(json)
        File(project.directory, "streams").mkdirs()
        File(project.directory, "streams/lidar.bin").writeBytes(ByteArray(4096))
    }

    // ── the reproduction ───────────────────────────────────────────────────

    @Test
    fun `a project survives the engine writing its own manifest into the same directory`() {
        val store = store()
        val created = store.create("Scan-014-2026-08-17-1932", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)

        simulateEngineCapture(created)

        // THE assertion. Before ROUND 6 this list was empty: the engine's
        // manifest.json could not be decoded as the app's schema, readProject
        // returned null, and the capture was invisible.
        val listed = store.list()
        assertEquals("the capture must still be listed after a real recording", 1, listed.size)
        assertEquals(created.id, listed.single().id)
        assertEquals("Scan-014-2026-08-17-1932", listed.single().manifest.name)
        assertFalse("a project created by this version is not a recovery", listed.single().manifest.recovered)
    }

    @Test
    fun `the seal after a real capture is written and reads back`() {
        val store = store()
        val created = store.create("Scan-015", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        simulateEngineCapture(created)

        val sealed = store.updateManifest(created.id) { it.copy(pointCountEstimate = 12_400_000L) }
        assertNotNull("the seal must not fail because the engine touched manifest.json", sealed)
        assertEquals(12_400_000L, sealed!!.manifest.pointCountEstimate)

        val reopened = store.open(created.id)
        assertNotNull("the sealed project must be re-openable", reopened)
        assertEquals(12_400_000L, reopened!!.manifest.pointCountEstimate)
        assertEquals("Scan-015", reopened.manifest.name)
    }

    @Test
    fun `the app never writes the engine's manifest file`() {
        val store = store()
        val created = store.create("Scan-016", SensorType.MID360, WorkflowProfile.SURVEY)
        assertFalse(
            "manifest.json belongs to the engine — the app must not create it",
            File(created.directory, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).exists(),
        )
        assertTrue(
            "the app's record is project.json",
            File(created.directory, FileProjectStore.APP_MANIFEST_FILE_NAME).isFile,
        )
    }

    // ── legacy migration ───────────────────────────────────────────────────

    @Test
    fun `a pre-round-6 project whose manifest json is still the app's is read and migrated`() {
        val store = store()
        val created = store.create("Legacy scan", SensorType.COIN_D6, WorkflowProfile.RESEARCH)
        // Recreate the 0.2.1 on-disk shape: app manifest under the ENGINE's name.
        val appJson = File(created.directory, FileProjectStore.APP_MANIFEST_FILE_NAME).readText()
        File(created.directory, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(appJson)
        File(created.directory, FileProjectStore.APP_MANIFEST_FILE_NAME).delete()

        val listed = store.list()
        assertEquals(1, listed.size)
        assertEquals("Legacy scan", listed.single().manifest.name)
        assertEquals(SensorType.COIN_D6, listed.single().manifest.sensor)
        assertEquals(WorkflowProfile.RESEARCH, listed.single().manifest.profile)
        assertFalse("a readable legacy manifest is a migration, not a recovery", listed.single().manifest.recovered)
        assertTrue(
            "reading it must move it out of the engine's way immediately",
            File(created.directory, FileProjectStore.APP_MANIFEST_FILE_NAME).isFile,
        )
    }

    // ── recovery of captures already lost in the field ─────────────────────

    @Test
    fun `a capture already clobbered by an old build is recovered and flagged`() {
        // No project.json at all — this is what the owner's phone has: a
        // directory the app created, whose only manifest is the engine's.
        val dir = File(root, "scan-014-2026-08-17-1932-a1b2c3.lscan")
        dir.mkdirs()
        File(dir, "streams").mkdirs()
        File(dir, "streams/lidar.bin").writeBytes(ByteArray(2048))
        File(dir, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(
            engineManifestJson(profile = "survey", createdAtUtcNs = 1_755_446_000_000_000_000L),
        )

        val listed = store().list()
        assertEquals("a lost capture must come back", 1, listed.size)
        val recovered = listed.single()
        assertTrue("and must say it was rebuilt", recovered.manifest.recovered)
        assertEquals(
            "the auto-name survives in the directory name",
            "Scan-014-2026-08-17-1932",
            recovered.manifest.name,
        )
        assertEquals("the profile comes from the engine's own string", WorkflowProfile.SURVEY, recovered.manifest.profile)
        assertEquals(
            "createdAt comes from the engine's createdAtUtcNs",
            1_755_446_000_000L,
            recovered.manifest.createdAtEpochMillis,
        )
        assertEquals("no IMU stream means the D6", SensorType.COIN_D6, recovered.manifest.sensor)
    }

    @Test
    fun `a recovered capture with an IMU stream is identified as a Mid-360`() {
        val dir = File(root, "site-b-ffffff.lscan")
        dir.mkdirs()
        File(dir, "streams").mkdirs()
        File(dir, "streams/imu.bin").writeBytes(ByteArray(64))
        File(dir, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(engineManifestJson())

        val recovered = store().list().single()
        assertEquals(SensorType.MID360, recovered.manifest.sensor)
    }

    @Test
    fun `recovery persists, so it happens once and the project behaves normally afterwards`() {
        val dir = File(root, "scan-021-2026-08-17-2101-0f0f0f.lscan")
        dir.mkdirs()
        File(dir, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(engineManifestJson())

        val store = store()
        assertEquals(1, store.list().size)
        assertTrue(
            "recovery must be written down, not recomputed on every list()",
            File(dir, FileProjectStore.APP_MANIFEST_FILE_NAME).isFile,
        )

        // And it is a normal project from then on: renaming it sticks.
        val renamed = store.updateManifest(dir.name) { it.copy(name = "Warehouse east") }
        assertNotNull(renamed)
        assertEquals("Warehouse east", store.open(dir.name)!!.manifest.name)
    }

    @Test
    fun `a directory with no readable manifest at all is still skipped, not invented`() {
        val dir = File(root, "junk-000000.lscan")
        dir.mkdirs()
        File(dir, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText("{}")
        assertTrue("an empty JSON object is not evidence of a capture", store().list().isEmpty())

        val noManifest = File(root, "empty-111111.lscan")
        noManifest.mkdirs()
        assertTrue(store().list().isEmpty())
    }

    // ── the diagnostic sink, which is what leaves evidence next time ────────

    @Test
    fun `create, recovery and update failures all emit a diagnostic line`() {
        val lines = mutableListOf<String>()
        val store = FileProjectStore(root, appVersion = "0.3.0", onDiagnostic = lines::add)

        store.create("Scan-030", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        assertTrue("creation must be logged", lines.any { it.startsWith("project created") })

        lines.clear()
        val dir = File(root, "lost-abcdef.lscan")
        dir.mkdirs()
        File(dir, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(engineManifestJson())
        store.list()
        assertTrue("a recovery must be logged", lines.any { it.contains("RECOVERED") })

        lines.clear()
        assertNull(store.updateManifest("does-not-exist.lscan") { it })
        assertTrue("a failed update must be logged", lines.any { it.contains("updateManifest FAILED") })
    }

    @Test
    fun `readAppManifestOnly does not migrate or recover - it answers what is genuinely on disk`() {
        val store = store()
        val dir = File(root, "scan-040-bbbbbb.lscan")
        dir.mkdirs()
        File(dir, FileProjectStore.ENGINE_MANIFEST_FILE_NAME).writeText(engineManifestJson())
        assertNull(
            "an engine-only project has no app manifest yet",
            store.readAppManifestOnly(dir.name),
        )
        store.list() // triggers recovery
        assertNotNull("after recovery it does", store.readAppManifestOnly(dir.name))
    }
}
