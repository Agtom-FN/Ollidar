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
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 28 item 162, finding P1j — **empty scans are in the list.**
 *
 * This test used to assert the opposite. ROUND 9 item 33 hid 0-point strays and
 * counted them into a footnote on the hero's aggregate line, and the reasoning
 * was that they would otherwise compete with real scans for attention.
 *
 * The owner's own library refutes it twice: **7 of his 74 scans sealed with
 * `points=0`**, and the footnote they were relegated to lived in a subtitle
 * rendered at `maxLines = 1`, so what he actually saw was `2 empt…` (finding
 * P1d). An empty scan is a result — the walk happened and produced nothing —
 * and the operator has to be able to see it and delete it. Hiding it made the
 * app disagree with the file manager about what is on the phone.
 *
 * So the ViewModel has no filter and no `hiddenEmptyCount` any more, and the
 * row styles the empty case instead: ink-mute title, `Empty — no points` from
 * `PointCountFormat.rowClause`, and a `bad` EMPTY mark.
 *
 * The Settings switch called "keep empty scans" is untouched and still means
 * what it always meant on the side that matters — `CaptureViewModel` deletes a
 * 0-point project **from disk** at seal unless it is on. Not creating a stray
 * was always a stronger promise than hiding one.
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

    private fun tempRoot(): File = File.createTempFile("round28-list", "").let {
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
    fun `the projects list shows every scan, empties included`(): Unit = runBlocking {
        val root = tempRoot()
        val store = storeWithOneRealScanAndTwoStrays(root)

        val vm = ProjectsListViewModel(store)
        val state = withTimeout(5_000) { vm.uiState.first { !it.loading } }

        assertEquals("the null stray AND the explicit zero are both in the list", 3, state.projects.size)
        assertEquals("…which is exactly what the store holds", store.list().size, state.projects.size)
        assertTrue(
            "and the operator can find them by name to delete them",
            state.projects.map { it.manifest.name }.containsAll(listOf("Scan-012", "Scan-014")),
        )
    }

    @Test
    fun `an empty scan is marked EMPTY so the row can say so in bad`(): Unit = runBlocking {
        val root = tempRoot()
        val store = storeWithOneRealScanAndTwoStrays(root)

        val vm = ProjectsListViewModel(store)
        val state = withTimeout(5_000) { vm.uiState.first { !it.loading } }

        val empties = state.projects.filter { it.manifest.isEmptyScan }
        assertEquals(2, empties.size)
        for (project in empties) {
            assertEquals(ProjectRowGrade.EMPTY, ProjectRowGrade.of(project.manifest))
        }
        val real = state.projects.single { !it.manifest.isEmptyScan }
        assertEquals(
            "a clean one-section scan claims nothing the manifest cannot prove",
            null,
            ProjectRowGrade.of(real.manifest),
        )
    }
}
