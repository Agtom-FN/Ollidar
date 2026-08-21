@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)

package com.lidarscan.app.ui.review

import com.lidarscan.app.engine.ProjectProbe
import com.lidarscan.app.ui.projects.ProjectsListViewModel
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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * ROUND 27 item 134 — **an instruction with no button, and a failure with no
 * words.**
 *
 * Two defects that only look like one. The Review screen has told the operator
 * to "Run Process on this project" since round 8 and has never drawn a Process
 * control in that state; and the one retry that did exist — the Projects card's
 * ⋯ › Process again — reported *nothing at all* when it failed, because the
 * lambda that ran it returned `Unit` and the engine's reason was left in
 * `lastError()` where no screen read it.
 *
 * Both halves are decisions in plain Kotlin (`ReviewUiState.canProcess`, and
 * what `reprocessProject` does with a returned reason), so both are tested here
 * rather than only on a booted emulator. The emulator half — that the button is
 * on screen and drives a job — is `Round27UiTest`.
 */
class Round27ProcessHonestyTest {

    @Before
    fun setUp() {
        Dispatchers.setMain(Dispatchers.Unconfined)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    // ── 134(a): the instruction and the affordance agree ───────────────────

    private fun state(
        load: ReviewLoad,
        hasCloud: Boolean = false,
        processing: Boolean = false,
    ) = ReviewUiState(load = load, hasCloud = hasCloud, processing = processing)

    @Test
    fun `the state that says Run Process offers Process`() {
        // This is the exact state the AVD reproduced: a Mid-360 container with
        // no resolved cloud, whose `loadMessage` is "No cloud in memory. Run
        // Process on this project…". Until round 27 there was no Process
        // control anywhere on the screen.
        assertTrue(state(ReviewLoad.FAILED).canProcess)
    }

    @Test
    fun `a screen that is still reading the container does not offer it`() {
        // Offering a second pipeline while the first is still deciding what the
        // container holds is how two engines end up writing one directory.
        for (busy in listOf(ReviewLoad.PROBING, ReviewLoad.LOADING_RECORDED, ReviewLoad.RESOLVING)) {
            assertFalse("$busy must not offer Process", state(busy).canProcess)
        }
        assertFalse(state(ReviewLoad.FAILED, processing = true).canProcess)
    }

    @Test
    fun `a scan with a cloud on screen does not offer it`() {
        assertFalse(state(ReviewLoad.READY, hasCloud = true).canProcess)
    }

    @Test
    fun `the one unfixable case is not offered a button that must fail`() {
        // A pre-0.5.0 capture recorded the returns and not the trajectory. No
        // pipeline can invent one — not this app's and not a later one's — and
        // a Process button there is a promise the app cannot keep.
        assertFalse(state(ReviewLoad.NO_TRAJECTORY).canProcess)
    }

    // ── 134(b): a failed run says what happened, and why ───────────────────

    private fun tempRoot(): File = File.createTempFile("round27-proc", "").let {
        it.delete(); it.mkdirs(); it
    }

    private suspend fun ProjectsListViewModel.awaitProjects() {
        withTimeout(5_000) {
            uiState.first { !it.loading && it.projects.isNotEmpty() }
        }
    }

    @Test
    fun `a reprocess that fails leaves its reason on the card`() = runBlocking<Unit> {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.9.12")
        val p = store.create("Synthetic Replay Demo", SensorType.MID360, WorkflowProfile.QUICK_SCAN)
        store.updateManifest(p.id) { it.copy(pointCountEstimate = 100_000L) }

        var runs = 0
        val vm = ProjectsListViewModel(
            projectStore = store,
            reprocess = { _, onProgress ->
                runs++
                onProgress(0.4f)
                // What `reprocessD6` does on an AVD: it cannot resolve, so the
                // screen's lambda turns that into the engine's own sentence.
                com.lidarscan.core.Wording.processFailed("the trajectory is empty")
            },
        )
        vm.awaitProjects()

        vm.reprocessProject(p.id)
        val failed = withTimeout(5_000) {
            vm.uiState.first { it.processFailures.containsKey(p.id) }
        }
        assertEquals(1, runs)
        val note = failed.processFailures[p.id]
        assertNotNull(note)
        assertTrue(
            "the reason itself must be in the line, not just the word 'failed': \"$note\"",
            note!!.contains("the trajectory is empty"),
        )
        assertTrue(
            "and an error says what to do next",
            note.contains("Process"),
        )
        assertFalse("the progress chip is gone", failed.running.containsKey(p.id))

        // ── the retry starts a NEW job, and clears the old verdict ──────────
        vm.reprocessProject(p.id)
        withTimeout(5_000) { vm.uiState.first { it.processFailures[p.id] != null && runs == 2 } }
        assertEquals("the retry ran a second job", 2, runs)

        vm.dismissProcessFailure(p.id)
        assertFalse(vm.uiState.value.processFailures.containsKey(p.id))
        root.deleteRecursively()
    }

    @Test
    fun `a reprocess that succeeds says nothing, which is the point`() = runBlocking<Unit> {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.9.12")
        val p = store.create("Good scan", SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
        store.updateManifest(p.id) { it.copy(pointCountEstimate = 216_000L) }

        val vm = ProjectsListViewModel(
            projectStore = store,
            reprocess = { _, onProgress -> onProgress(1f); null },
        )
        vm.awaitProjects()
        vm.reprocessProject(p.id)
        withTimeout(5_000) { vm.uiState.first { !it.running.containsKey(p.id) } }
        assertTrue(
            "a run that worked must not leave a failure line behind",
            vm.uiState.value.processFailures.isEmpty(),
        )
        root.deleteRecursively()
    }

    @Test
    fun `a thrown reprocess is still an answer, not a silence`() = runBlocking<Unit> {
        val root = tempRoot()
        val store = FileProjectStore(root, appVersion = "0.9.12")
        val p = store.create("Broken scan", SensorType.MID360, WorkflowProfile.QUICK_SCAN)
        store.updateManifest(p.id) { it.copy(pointCountEstimate = 1L) }

        val vm = ProjectsListViewModel(
            projectStore = store,
            reprocess = { _, _ -> throw IllegalStateException("page store is locked") },
        )
        vm.awaitProjects()
        vm.reprocessProject(p.id)
        val failed = withTimeout(5_000) {
            vm.uiState.first { it.processFailures.containsKey(p.id) }
        }
        assertTrue(failed.processFailures[p.id]!!.contains("page store is locked"))
        root.deleteRecursively()
    }

    // ── the probe decides which pipeline, and that is not a coin toss ──────

    @Test
    fun `the empty state on a Mid-360 container is not a D6 job`() {
        // `reprocessD6` is the offline D6 resolve. The state this button appears
        // in is by construction the non-D6 one ("the Mid-360 pipeline re-runs
        // the odometry"), so a button that only knew the D6 path would be a
        // button that could only fail. Pinned as the probe fact the ViewModel
        // branches on.
        assertFalse(ProjectProbe.NONE.isD6)
    }
}
