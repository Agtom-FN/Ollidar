package com.lidarscan.app.ui.plan

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.share.ShareTargets
import com.lidarscan.app.ui.common.exportBaseName
import com.lidarscan.core.plan.PlanModel
import com.lidarscan.core.plan.PlanOptions
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

data class PlanUiState(
    val project: Project? = null,
    val options: PlanOptions = PlanOptions(),
    val plan: PlanModel? = null,
    val running: Boolean = false,
    val progress: Float = 0f,
    val message: String? = null,
)

/**
 * B11 — §3.6's "Primary UX on desktop; **view + export on Android**".
 *
 * The slice band is seeded from the project's own profile defaults
 * ([com.lidarscan.core.model.CaptureDefaults.planSliceMinM]) rather than from
 * A12's constant, so a Floor-plan project opens on the band it was configured
 * for. The editor's include/exclude regions (A12's `PlanRegion`) are **not**
 * offered here: they need a draw-on-the-plan interaction that belongs to the
 * desktop workspace, and the slice-height slider — which §3.6 calls out by name
 * as editor v1 — is the half that carries most of the value.
 */
class PlanViewModel(
    container: AppContainer,
    private val store: ProjectStore,
    private val projectId: String,
) : ViewModel() {

    private val repo = container.processingRepository

    private val _uiState = MutableStateFlow(PlanUiState())
    val uiState: StateFlow<PlanUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            val p = withContext(Dispatchers.IO) { store.open(projectId) }
            val d = p?.manifest?.effectiveCaptureDefaults()
            _uiState.value = _uiState.value.copy(
                project = p,
                options = PlanOptions(
                    zMinM = d?.planSliceMinM ?: 1.0f,
                    zMaxM = d?.planSliceMaxM ?: 1.5f,
                ),
                plan = repo.plan.value,
            )
        }
        viewModelScope.launch { repo.planProgress.collect { _uiState.value = _uiState.value.copy(progress = it) } }
        viewModelScope.launch { repo.planRunning.collect { _uiState.value = _uiState.value.copy(running = it) } }
        viewModelScope.launch { repo.plan.collect { _uiState.value = _uiState.value.copy(plan = it) } }
    }

    fun setOptions(o: PlanOptions) {
        _uiState.value = _uiState.value.copy(options = o)
    }

    fun dismissMessage() {
        _uiState.value = _uiState.value.copy(message = null)
    }

    fun extract() {
        viewModelScope.launch {
            val r = repo.extractPlan(_uiState.value.options)
            _uiState.value = _uiState.value.copy(
                message = r.fold(
                    { m -> if (m.isEmpty) "No walls found — see the note on screen." else "Extracted ${m.walls.size} walls." },
                    { it.message ?: "Extraction failed." },
                ),
            )
        }
    }

    fun cancel() {
        repo.cancelPlan()
    }

    fun exportDxf(context: Context) = export(context, "dxf") { dest -> repo.writePlanDxf(dest) }

    fun exportPdf(context: Context) = export(context, "pdf") { dest ->
        val p = _uiState.value.project
        repo.writePlanPdf(
            dest,
            title = p?.manifest?.name ?: "Floor plan",
            project = p?.id.orEmpty(),
            // A12's PDF writer derives NOTHING from the clock so it can be
            // deterministic; the date is the caller's, and the honest one is
            // the capture's, not "now".
            date = p?.manifest?.createdAtEpochMillis?.let {
                SimpleDateFormat("yyyy-MM-dd", Locale.US).format(Date(it))
            }.orEmpty(),
        )
    }

    private fun export(context: Context, extension: String, write: suspend (File) -> Result<File>) {
        val p = _uiState.value.project ?: return
        viewModelScope.launch {
            val dir = File(p.directory, "exports").apply { mkdirs() }
            val dest = File(dir, "${exportBaseName(p.id)}-plan.$extension")
            val r = write(dest)
            r.fold(
                { f ->
                    ShareTargets.shareFile(context, f, ShareTargets.mimeFor(f), "Send floor plan")
                    _uiState.value = _uiState.value.copy(message = "Wrote ${f.name}")
                },
                { _uiState.value = _uiState.value.copy(message = it.message ?: "Export failed.") },
            )
        }
    }
}
