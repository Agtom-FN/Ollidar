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

    fun delete(projectId: String) {
        viewModelScope.launch(Dispatchers.IO) {
            ProjectPreviewCache.invalidate(projectId)
            projectStore.delete(projectId)
            refresh()
        }
    }
}
