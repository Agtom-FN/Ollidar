package com.lidarscan.app.ui.projects

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.share.ProjectExporter
import com.lidarscan.app.share.ShareTargets
import com.lidarscan.core.projects.BatchAction
import com.lidarscan.core.projects.BatchExport
import com.lidarscan.core.projects.BatchReport
import com.lidarscan.core.projects.ProjectSelection
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.io.File

data class ProjectBatchUiState(
    val selection: ProjectSelection = ProjectSelection.EMPTY,
    /** project id → 0..1 for the job running against it right now. */
    val running: Map<String, Float> = emptyMap(),
    /** True from the first job to the last, so the bar's actions cannot be double-tapped. */
    val busy: Boolean = false,
    /** [BatchReport.summary] once a run finishes; null otherwise. */
    val message: String? = null,
)

/**
 * ROUND 23 item 104c — **group export, group share, group delete.**
 *
 * The owner asked for group export explicitly. This is the screen half of it;
 * the decisions that are worth testing are not here:
 *
 *  * the **selection state machine** is [ProjectSelection] in `:core`;
 *  * the **sequencing rule** — one job at a time, keep going after a failure,
 *    report every failure — is [BatchExport] in `:core`;
 *  * the **export itself** is [ProjectExporter], which is
 *    `ProcessingViewModel.export`'s own steps: the same A15 job, the same
 *    `exports/` output, the same ROUND 7 copy into `Downloads/LidarScan/`.
 *
 * What is left here is the two things that genuinely need Android: a `Context`
 * for the MediaStore write, and the single `ACTION_SEND_MULTIPLE` at the end.
 *
 * **Share ends in ONE sheet.** The alternative — a sheet per scan — is three
 * choosers stacked on top of each other, and it re-creates exactly the ROUND 7
 * failure the Downloads copy exists to prevent. Every file is in Downloads
 * before the sheet opens, so dismissing the chooser loses nothing.
 *
 * The run is launched in [jobScope] (the container's, in the app) rather than
 * `viewModelScope`: a group export of six scans takes minutes and the operator
 * will leave the tab. That is ROUND 22 item 90's rule, and the reason
 * `reprocessProject` already does the same.
 */
class ProjectBatchViewModel(
    private val exporter: ProjectExporter,
    private val store: ProjectStore,
    /** Called after a batch that changed the list on disk, so the list re-reads it. */
    private val onProjectsChanged: () -> Unit = {},
    private val log: (String) -> Unit = {},
    private val jobScope: CoroutineScope? = null,
) : ViewModel() {

    private val _uiState = MutableStateFlow(ProjectBatchUiState())
    val uiState: StateFlow<ProjectBatchUiState> = _uiState.asStateFlow()

    // ── selection ───────────────────────────────────────────────────────────

    /** Long-press on a card. */
    fun enterSelection(projectId: String) {
        _uiState.update { it.copy(selection = it.selection.enter(projectId), message = null) }
    }

    /** Tap on a card **while selecting**; a no-op otherwise, so an ordinary tap still opens. */
    fun toggle(projectId: String) {
        _uiState.update { it.copy(selection = it.selection.toggle(projectId)) }
    }

    /** The X in the selection bar. */
    fun exitSelection() {
        _uiState.update { it.copy(selection = ProjectSelection.EMPTY) }
    }

    fun dismissMessage() {
        _uiState.update { it.copy(message = null) }
    }

    /** Drops ids the list no longer holds — see [ProjectSelection.retain]. */
    fun pruneSelection(existingIds: Collection<String>) {
        _uiState.update { it.copy(selection = it.selection.retain(existingIds)) }
    }

    // ── the batch ───────────────────────────────────────────────────────────

    /**
     * Runs [action] over the current selection, in [listOrder]'s order.
     *
     * @param listOrder the ids as the list draws them, so "2 of 3" means the
     *   same thing twice running.
     */
    fun runOnSelection(context: Context, action: BatchAction, listOrder: List<String>) {
        val state = _uiState.value
        if (state.busy) return
        val ids = state.selection.ordered(listOrder)
        if (ids.isEmpty()) return
        val appContext = context.applicationContext
        _uiState.update { it.copy(busy = true, message = null) }
        (jobScope ?: viewModelScope).launch(Dispatchers.IO) {
            val produced = mutableListOf<File>()
            val report = try {
                BatchExport.run(
                    action = action,
                    projectIds = ids,
                    onStep = { step ->
                        _uiState.update { it.copy(running = it.running + (step.projectId to 0f)) }
                    },
                ) { step ->
                    try {
                        when (action) {
                            BatchAction.DELETE -> deleteOne(step.projectId)
                            else -> exportOne(appContext, step.projectId).map { produced += it }
                        }
                    } finally {
                        _uiState.update { it.copy(running = it.running - step.projectId) }
                    }
                }
            } finally {
                _uiState.update { it.copy(busy = false, running = emptyMap()) }
            }

            report.failureLogLines().forEach { log("batch ${action.name} FAILED $it") }
            log("batch ${action.name} ${report.succeeded.size}/${report.outcomes.size} ok")

            if (action == BatchAction.SHARE && produced.isNotEmpty()) {
                // Downloads first (done, above), sheet second. Always.
                runCatching {
                    ShareTargets.shareFiles(
                        appContext,
                        produced,
                        com.lidarscan.core.projects.ProjectActionWording.sendFilesTitle(produced.size),
                    )
                }.onFailure { e -> log("batch SHARE sheet failed: ${e.javaClass.simpleName}") }
            }
            if (action == BatchAction.DELETE) onProjectsChanged()

            _uiState.update {
                it.copy(
                    message = report.summary(),
                    // A finished batch leaves selection mode: the operator asked
                    // for one thing and got it, and a bar still holding three
                    // now-exported scans invites doing it again by accident.
                    selection = ProjectSelection.EMPTY,
                )
            }
        }
    }

    private suspend fun exportOne(context: Context, projectId: String): Result<File> =
        exporter.exportToDownloads(
            context = context,
            projectId = projectId,
            onProgress = { f ->
                _uiState.update { it.copy(running = it.running + (projectId to f.coerceIn(0f, 1f))) }
            },
        ).map { it.file }

    private fun deleteOne(projectId: String): Result<Unit> = runCatching {
        ProjectPreviewCache.invalidate(projectId)
        check(store.delete(projectId)) { "this scan could not be deleted" }
    }
}
