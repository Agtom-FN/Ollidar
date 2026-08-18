package com.lidarscan.app.ui.plan

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.share.DownloadsExporter
import com.lidarscan.app.share.ShareTargets
import com.lidarscan.core.capture.FloorPlanResult
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
    // --- ROUND 15 item 56 -------------------------------------------------
    /** The rendered plan, from the sealed container. Null until [PlanViewModel.render]. */
    val rendered: FloorPlanResult? = null,
    val rendering: Boolean = false,
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

    /**
     * ROUND 15 item 56 — the plan the operator can actually LOOK AT and send.
     *
     * Distinct from [extract], and the difference matters:
     *
     *  * [extract] slices whatever cloud the process-wide ProcessingEngine
     *    happens to be holding — in practice the one Review loaded, but
     *    nothing scopes it to this project — and produces an in-memory model
     *    that only this screen's canvas can draw.
     *  * this runs against the sealed CONTAINER by path, opens its own
     *    PageStore inside the engine, prefers the ROUND 13 stitched map when
     *    the scan has been processed, and leaves a PNG (plus a PDF and a DXF
     *    when walls were fitted) in `processed/`. Nothing to share, nothing to
     *    preview, and no project mix-up is possible.
     *
     * The slice band is the project's own `planSliceMinM/MaxM`. The UP AXIS is
     * not a setting: a D6 session's world is ARCore's, where +Y is up, and the
     * engine fixes that (`slam/post/lscan_plan.h`).
     */
    fun render() {
        val p = _uiState.value.project ?: return
        if (_uiState.value.rendering) return
        _uiState.value = _uiState.value.copy(rendering = true, message = null)
        viewModelScope.launch {
            val o = _uiState.value.options
            val r = withContext(Dispatchers.IO) {
                repo.floorPlan(
                    lscanDir = p.directory,
                    sliceMinM = o.zMinM.toDouble(),
                    sliceMaxM = o.zMaxM.toDouble(),
                    gridResM = o.gridResM.toDouble(),
                    title = p.manifest?.name ?: p.id,
                )
            }
            _uiState.value = _uiState.value.copy(
                rendering = false,
                rendered = r,
                message = when {
                    r == null || !r.ran ->
                        "No floor plan could be made from this scan."
                    else -> r.headline
                },
            )
        }
    }

    /**
     * Copies one of the rendered files into Downloads/LidarScan and opens the
     * share sheet — the SAME delivery every other export in this app uses
     * (ProcessingViewModel). The old plan export called `ShareTargets.shareFile`
     * on a file inside the app's private storage only, so a share target that
     * did not resolve the content URI got nothing and the file was
     * unreachable afterwards.
     */
    fun shareRendered(context: Context, kind: RenderedKind) {
        val r = _uiState.value.rendered ?: return
        val p = _uiState.value.project ?: return
        val path = when (kind) {
            RenderedKind.PNG -> r.pngPath
            RenderedKind.PDF -> r.pdfPath
            RenderedKind.DXF -> r.dxfPath
        }
        if (path.isEmpty()) return
        val src = File(path)
        viewModelScope.launch {
            val name = "${exportBaseName(p.id)}-plan.${kind.extension}"
            val copied = withContext(Dispatchers.IO) {
                DownloadsExporter.copyToDownloads(context, src, name)
            }
            copied.fold(
                { shown ->
                    ShareTargets.shareFile(
                        context,
                        src,
                        ShareTargets.mimeFor(src),
                        "Send floor plan",
                    )
                    _uiState.value = _uiState.value.copy(message = "Saved to $shown")
                },
                {
                    _uiState.value = _uiState.value.copy(
                        message = it.message ?: "Could not save the floor plan.",
                    )
                },
            )
        }
    }

    enum class RenderedKind(val extension: String) { PNG("png"), PDF("pdf"), DXF("dxf") }

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
