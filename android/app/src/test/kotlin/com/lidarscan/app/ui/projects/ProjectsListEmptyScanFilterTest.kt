@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.projects

import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.store.FileProjectStore
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test

/**
 * ROUND 9, owner item 33 — the **list** half: 0-point strays are hidden, counted
 * and never silently dropped from the arithmetic.
 *
 * The filter lives here rather than in `FileProjectStore.list()` on purpose, and
 * this test is where that choice is pinned: the store is shared with merge,
 * processing and Settings' replay find-or-create, none of which may lose sight
 * of a project just because it has no points in it. So the same store, read
 * twice, must give a full list and a filtered one.
 */
class ProjectsListEmptyScanFilterTest {

    @Before
    fun setUp() {
        Dispatchers.setMain(Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private fun tempRoot(): File = File.createTempFile("round9-list", "").let {
        it.delete()
        it.mkdirs()
        it
    }

    /** One real scan and two `scan-012`/`scan-014`-style strays. */
    private fun storeWithOneRealScanAndTwoStrays(root: File): FileProjectStore {
        val store = FileProjectStore(root, appVersion = "0.5.0")
        val real = store.create("Real scan", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        store.updateManifest(real.id) { it.copy(pointCountEstimate = 216_000L) }
        // Never recorded into: pointCountEstimate stays null.
        store.create("Scan-012", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        // Recorded into and got nothing: an explicit zero.
        val zero = store.create("Scan-014", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        store.updateManifest(zero.id) { it.copy(pointCountEstimate = 0L) }
        return store
    }

    @Test
    fun `the projects list hides empty scans and reports how many`(): Unit = runBlocking {
        val root = tempRoot()
        val store = storeWithOneRealScanAndTwoStrays(root)

        val vm = ProjectsListViewModel(store, keepEmptyScans = { false })
        val state = withTimeout(5_000) { vm.uiState.first { !it.loading } }

        assertEquals("only the scan with points in it belongs in the library", 1, state.projects.size)
        assertEquals("Real scan", state.projects.single().manifest.name)
        assertEquals("both strays — the null one AND the explicit zero", 2, state.hiddenEmptyCount)
        assertEquals("…and the store itself still sees all three", 3, store.list().size)
    }

    @Test
    fun `keeping empty scans shows every project and hides nothing`(): Unit = runBlocking {
        val root = tempRoot()
        val store = storeWithOneRealScanAndTwoStrays(root)

        val vm = ProjectsListViewModel(store, keepEmptyScans = { true })
        val state = withTimeout(5_000) { vm.uiState.first { !it.loading } }

        assertEquals(3, state.projects.size)
        assertEquals(0, state.hiddenEmptyCount)
    }
}
