package com.lidarscan.core.store

import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import java.io.File
import kotlin.io.path.createTempDirectory
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class FileProjectStoreTest {

    private lateinit var root: File
    private lateinit var store: FileProjectStore

    @Before
    fun setUp() {
        root = createTempDirectory(prefix = "lscan-test-").toFile()
        store = FileProjectStore(rootDir = root, appVersion = "0.1.0-test", clock = { FIXED_TIME })
    }

    @After
    fun tearDown() {
        root.deleteRecursively()
    }

    @Test
    fun `create makes an lscan directory with the spec's subdirectory layout`() {
        val project = store.create("Office Survey", SensorType.MID360, WorkflowProfile.SURVEY)

        assertTrue(project.directory.isDirectory)
        assertTrue(project.id.endsWith(".lscan"))
        assertTrue(File(project.directory, "manifest.json").isFile)
        assertTrue(File(project.directory, "streams").isDirectory)
        assertTrue(File(project.directory, "streams/frames").isDirectory)
        assertTrue(File(project.directory, "processed").isDirectory)
        assertTrue(File(project.directory, "merged").isDirectory)
        assertTrue(File(project.directory, "exports").isDirectory)
    }

    @Test
    fun `create stamps sensor, profile, created time, app version and schema version`() {
        val project = store.create("Warehouse", SensorType.COIN_D6, WorkflowProfile.FLOOR_PLAN)

        assertEquals(SensorType.COIN_D6, project.manifest.sensor)
        assertEquals(WorkflowProfile.FLOOR_PLAN, project.manifest.profile)
        assertEquals(FIXED_TIME, project.manifest.createdAtEpochMillis)
        assertEquals("0.1.0-test", project.manifest.appVersion)
        assertEquals(1, project.manifest.schemaVersion)
        assertNull(project.manifest.pointCountEstimate)
    }

    @Test
    fun `two projects with the same name get distinct directories`() {
        val a = store.create("Backyard", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        val b = store.create("Backyard", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)

        assertTrue(a.id != b.id)
        assertTrue(a.directory.exists())
        assertTrue(b.directory.exists())
    }

    @Test
    fun `list returns created projects newest first`() {
        val older = store.create("Older", SensorType.COIN_D6, WorkflowProfile.RESEARCH)
        val storeLater = FileProjectStore(rootDir = root, appVersion = "0.1.0-test", clock = { FIXED_TIME + 10_000 })
        val newer = storeLater.create("Newer", SensorType.MID360, WorkflowProfile.SURVEY)

        val listed = store.list()

        assertEquals(listOf(newer.id, older.id), listed.map { it.id })
    }

    @Test
    fun `open round-trips a project by id`() {
        val created = store.create("Round Trip", SensorType.MID360, WorkflowProfile.RESEARCH)

        val opened = checkNotNull(store.open(created.id))

        assertEquals(created.manifest, opened.manifest)
        assertEquals(created.directory, opened.directory)
    }

    @Test
    fun `open returns null for an unknown id`() {
        assertNull(store.open("does-not-exist.lscan"))
    }

    @Test
    fun `delete removes the project directory and returns true`() {
        val created = store.create("To Delete", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)

        val deleted = store.delete(created.id)

        assertTrue(deleted)
        assertFalse(created.directory.exists())
        assertNull(store.open(created.id))
    }

    @Test
    fun `delete returns false for an unknown id`() {
        assertFalse(store.delete("nope.lscan"))
    }

    @Test
    fun `list skips lscan directories with no readable manifest`() {
        File(root, "corrupt.lscan").mkdirs()
        val good = store.create("Good", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)

        val listed = store.list()

        assertEquals(listOf(good.id), listed.map { it.id })
    }

    companion object {
        private const val FIXED_TIME = 1_755_100_000_000L
    }
}
