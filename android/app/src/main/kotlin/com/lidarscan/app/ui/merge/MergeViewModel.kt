package com.lidarscan.app.ui.merge

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.merge.MergeRepository
import com.lidarscan.app.share.ShareTargets
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

data class MergeUiState(
    val candidates: List<MergeRepository.Candidate> = emptyList(),
    val selected: Set<String> = emptySet(),
    val running: Boolean = false,
    val progress: Float = 0f,
    val progressLabel: String = "",
    val summaryText: String? = null,
    val resultFile: File? = null,
    val message: String? = null,
)

/** B12 — §3.10's "Android offers georeferenced auto-merge only". */
class MergeViewModel(
    private val container: AppContainer,
    private val store: ProjectStore,
) : ViewModel() {

    private val repo = container.mergeRepository

    private val _uiState = MutableStateFlow(MergeUiState())
    val uiState: StateFlow<MergeUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            val projects = withContext(Dispatchers.IO) { store.list() }
            _uiState.value = _uiState.value.copy(
                candidates = projects.map { p ->
                    MergeRepository.Candidate(
                        project = p,
                        georef = p.manifest.georef,
                        chainFromJob = 0L,
                    )
                },
            )
        }
    }

    fun toggle(projectId: String) {
        val s = _uiState.value.selected
        _uiState.value = _uiState.value.copy(
            selected = if (s.contains(projectId)) s - projectId else s + projectId,
        )
    }

    fun dismissMessage() {
        _uiState.value = _uiState.value.copy(message = null)
    }

    fun cancel() {
        repo.cancel()
    }

    fun merge() {
        val chosen = _uiState.value.candidates.filter { _uiState.value.selected.contains(it.project.id) }
        if (chosen.size < 2) return
        viewModelScope.launch {
            // The merged product goes in the FIRST selected project's merged/
            // directory — the .lscan container already reserves it ("merged/
            // merge graphs + results", §3.11) and B1's store creates it.
            val anchor = chosen.first().project
            val out = File(File(anchor.directory, "merged").apply { mkdirs() }, "merged.ply")
            _uiState.value = _uiState.value.copy(running = true, progress = 0f, summaryText = null, resultFile = null)
            val r = repo.merge(chosen, out) { p ->
                _uiState.value = _uiState.value.copy(progress = p.fraction, progressLabel = p.label)
            }
            _uiState.value = r.fold(
                { s ->
                    _uiState.value.copy(
                        running = false,
                        progress = 1f,
                        resultFile = if (s.ok && out.isFile) out else null,
                        summaryText = if (s.ok) {
                            buildString {
                                appendLine(s.message)
                                appendLine("${s.mergedPoints} merged points from ${s.inputPoints} input points.")
                                appendLine("${s.pairsConverged}/${s.pairsRefined} pairs converged; worst RMS %.3f m, worst overlap %.0f%%."
                                    .format(s.worstRmsM, s.worstOverlap * 100))
                                if (s.pairsLowOverlap > 0) {
                                    appendLine(
                                        "${s.pairsLowOverlap} pair(s) were reported but NOT merged — too little overlap " +
                                            "to refine honestly.",
                                    )
                                }
                                if (s.epsgMismatch) {
                                    appendLine(
                                        "The sessions carry different EPSG codes. They are still composable through " +
                                            "ECEF, but this usually means two different project CRSs were picked.",
                                    )
                                }
                            }
                        } else {
                            s.message
                        },
                        message = if (s.ok) "Merged into ${out.name}" else null,
                    )
                },
                { _uiState.value.copy(running = false, message = it.message ?: "Merge failed.") },
            )
        }
    }

    fun shareResult(context: Context) {
        val f = _uiState.value.resultFile ?: return
        ShareTargets.shareFile(context, f, ShareTargets.mimeFor(f), "Send merged cloud")
    }
}
