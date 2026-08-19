package com.lidarscan.app.ui.projects

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class ProjectsUiState(
    val loading: Boolean = true,
    val projects: List<Project> = emptyList(),
    /**
     * ROUND 9 (owner item 33): how many 0-point strays this list is NOT showing.
     *
     * Surfaced quietly (the hero's aggregate line) rather than as a section of
     * its own: the point of hiding them is that they stop competing with real
     * scans for attention, and a "3 empty scans" panel would put the clutter
     * straight back. Zero on a phone with no strays, which is every phone after
     * Settings › Scans › "Clean up empty scans" has been used once.
     */
    val hiddenEmptyCount: Int = 0,
    /**
     * ROUND 22 item 96 — project id → progress (0..1) of a reprocess started
     * from that project's card, so the card can carry a small progress chip
     * instead of the operator having to go and find a Jobs screen that Simple
     * mode no longer shows.
     */
    val running: Map<String, Float> = emptyMap(),
)

/**
 * @param keepEmptyScans whether 0-point projects belong in this list — see
 *   [com.lidarscan.app.data.AppSettings.keepEmptyScans]. Read per refresh so the
 *   Settings switch takes effect on the next trip to this tab.
 *
 *   **Defaults to `true` (show everything).** The filter is opt-in per call
 *   site, and only the Projects tab opts in: this ViewModel also backs the
 *   project PICKER, where the list is a chooser for merge/processing rather than
 *   the operator's scan library, and silently hiding rows from a chooser is a
 *   different (worse) decision than hiding them from a library.
 *
 *   The filter deliberately does NOT live in `FileProjectStore.list()` — that
 *   one method is shared with merge, processing and the replay find-or-create in
 *   Settings, none of which should stop being able to see a project just because
 *   it has no points yet.
 */
class ProjectsListViewModel(
    private val projectStore: ProjectStore,
    private val keepEmptyScans: suspend () -> Boolean = { true },
    /**
     * ROUND 22 item 96 — the card's "Process again". The SAME handle-less
     * `ProcessingRepository.reprocessD6` the seal's auto-process uses, so this
     * is a new caller and not a new pipeline: it takes a directory, opens its
     * own PageStore, holds the per-container lock ROUND 18 added, and is
     * therefore safe to run while a capture is arming.
     *
     * Defaults to a no-op so the project PICKER (which shares this ViewModel)
     * and every JVM test keep working untouched.
     */
    private val reprocess: suspend (java.io.File, (Float) -> Boolean) -> Unit = { _, _ -> },
    /**
     * ROUND 22 items 90 + 96: the scope a reprocess runs in. It must outlive
     * this screen — the operator taps "Process again" and then, quite
     * reasonably, walks away from the tab. `AppContainer.containerScope` in the
     * app; `viewModelScope` when null, which is what tests want.
     */
    private val jobScope: kotlinx.coroutines.CoroutineScope? = null,
) : ViewModel() {

    private val _uiState = MutableStateFlow(ProjectsUiState())
    val uiState: StateFlow<ProjectsUiState> = _uiState.asStateFlow()

    init {
        refresh()
    }

    fun refresh() {
        viewModelScope.launch(Dispatchers.IO) {
            val all = projectStore.list()
            val keep = runCatching { keepEmptyScans() }.getOrDefault(true)
            val visible = if (keep) all else all.filterNot { it.manifest.isEmptyScan }
            _uiState.update {
                it.copy(
                    loading = false,
                    projects = visible,
                    hiddenEmptyCount = all.size - visible.size,
                )
            }
        }
    }

    /**
     * ROUND 22 item 96 — "Process again" from the card's ⋯ menu.
     *
     * Refuses a second run for the same project rather than queueing one: the
     * repository already serialises per container (ROUND 18's lock), so a
     * second tap would simply block a coroutine for the length of the first
     * run and then redo an idempotent job. Saying "it is already running" with
     * a progress chip is the same behaviour, visible.
     */
    fun reprocessProject(projectId: String) {
        if (_uiState.value.running.containsKey(projectId)) return
        val project = _uiState.value.projects.firstOrNull { it.id == projectId } ?: return
        _uiState.update { it.copy(running = it.running + (projectId to 0f)) }
        (jobScope ?: viewModelScope).launch(Dispatchers.IO) {
            try {
                reprocess(project.directory) { f ->
                    _uiState.update { s -> s.copy(running = s.running + (projectId to f.coerceIn(0f, 1f))) }
                    true
                }
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                // ROUND 22 item 90's rule, applied here too: cancellation is
                // never swallowed and never reported as a failure.
                throw cancelled
            } finally {
                _uiState.update { it.copy(running = it.running - projectId) }
                ProjectPreviewCache.invalidate(projectId)
                refresh()
            }
        }
    }

    fun delete(projectId: String) {
        viewModelScope.launch(Dispatchers.IO) {
            ProjectPreviewCache.invalidate(projectId)
            projectStore.delete(projectId)
            refresh()
        }
    }
}
