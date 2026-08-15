package com.lidarscan.app.ui.processing

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.share.ShareTargets
import com.lidarscan.app.ui.common.exportBaseName
import com.lidarscan.core.cloud.CloudError
import com.lidarscan.core.cloud.CloudJobState
import com.lidarscan.core.cloud.CloudSubmitClient
import com.lidarscan.core.cloud.CloudSubmitConfig
import com.lidarscan.core.cloud.UrlConnectionHttpTransport
import com.lidarscan.core.jobs.ActionGate
import com.lidarscan.core.jobs.ProcessingJob
import com.lidarscan.core.jobs.ProcessingMode
import com.lidarscan.core.jobs.ProcessingPolicy
import com.lidarscan.core.jobs.SyncQuality
import com.lidarscan.core.model.ExportFormat
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

data class CloudUploadState(
    val running: Boolean = false,
    val phase: String = "",
    val uploadFraction: Float = 0f,
    val jobId: String? = null,
    val serverState: CloudJobState? = null,
    val serverProgress: Float = 0f,
    val resultFile: File? = null,
    val error: String? = null,
)

data class ProcessingUiState(
    val project: Project? = null,
    val jobs: List<ProcessingJob> = emptyList(),
    val mode: ProcessingMode = ProcessingMode.LOCAL,
    val postProcessGate: ActionGate = ActionGate.allowed,
    val colorizeGate: ActionGate = ActionGate.allowed,
    val exportGate: ActionGate = ActionGate.allowed,
    val transferGate: ActionGate = ActionGate.allowed,
    val cloudGate: ActionGate = ActionGate.allowed,
    val syncQuality: SyncQuality = SyncQuality.UNKNOWN,
    val keyframeCount: Int = 0,
    val hasRawStreams: Boolean = false,
    val exportFormat: ExportFormat = ExportFormat.PLY_BINARY,
    val exportNote: String? = null,
    val cloud: CloudUploadState = CloudUploadState(),
    val message: String? = null,
    val engineAvailable: Boolean = true,
)

/**
 * B6 — the Processing screen (Tech Spec §3.13's "Processing (mode chooser,
 * queue)", §3.8's three modes).
 *
 * The one structural decision worth stating: **Local and Extract-for-transfer
 * are engine jobs; Cloud is not.** A15 has a `kCloudSubmit` job kind and it
 * works, but the app's cloud client is Kotlin ([CloudSubmitClient]) because
 * the C++ one needs an `HttpTransport` implementation that would itself have to
 * be written in Kotlin and called back down through JNI — a round trip that
 * buys nothing over speaking the documented REST contract directly, and would
 * leave the app with two retry policies and two size caps. The Cloud action
 * therefore runs a real `kTransferExport` job to build the zip (one bundling
 * implementation, A5's) and then uploads it from Kotlin.
 */
class ProcessingViewModel(
    private val container: AppContainer,
    private val store: ProjectStore,
    private val settings: SettingsRepository,
    private val projectId: String,
) : ViewModel() {

    private val _uiState = MutableStateFlow(ProcessingUiState())
    val uiState: StateFlow<ProcessingUiState> = _uiState.asStateFlow()

    private val repo = container.processingRepository
    private var cloudCancelled = false

    /** The latest DataStore snapshot, mirrored so the gate recomputation is not itself suspending. */
    private var appSettings = com.lidarscan.app.data.AppSettings()

    init {
        viewModelScope.launch {
            repo.jobs.collect {
                _uiState.value = _uiState.value.copy(jobs = repo.jobsFor(projectId))
                recomputeGates()
            }
        }
        viewModelScope.launch {
            settings.settings.collect {
                appSettings = it
                recomputeGates()
            }
        }
        reload()
    }

    fun reload() {
        viewModelScope.launch {
            val project = withContext(Dispatchers.IO) { store.open(projectId) }
            _uiState.value = _uiState.value.copy(
                project = project,
                exportFormat = project?.manifest?.effectiveCaptureDefaults()?.exportFormat ?: ExportFormat.PLY_BINARY,
                engineAvailable = repo.isAvailable,
            )
            recomputeGates()
        }
    }

    private fun recomputeGates() {
        val project = _uiState.value.project ?: return
        viewModelScope.launch {
            val dir = project.directory
            val (hasRaw, keyframes) = withContext(Dispatchers.IO) {
                val streams = File(dir, "streams")
                val raw = streams.isDirectory && (streams.listFiles()?.any { it.isFile && it.length() > 0 } == true)
                val frames = File(dir, "streams/frames")
                val idx = File(frames, "frames.idx")
                val n = if (idx.isFile && idx.length() > 0) {
                    frames.listFiles()?.count { it.extension.equals("jpg", ignoreCase = true) } ?: 0
                } else {
                    0
                }
                raw to n
            }
            // A4's per-stream verdict has no C-ABI accessor at ABI 4 (there is
            // no scan_engine_timesync_quality), so it cannot be read for a
            // recorded session. The honest answer is what the mount calibration
            // recorded: a wizard clock sweep that was ACCEPTED is the only
            // evidence this app has that the camera and lidar clocks were tied
            // together, and A11 requires the caller to supply the verdict.
            val calib = project.manifest.mountCalibration
            val sync = if (calib?.clockOffsetNs != null) SyncQuality.GOOD else SyncQuality.UNKNOWN
            val current = _uiState.value
            val allowPoor = appSettings.allowPoorSyncColorize
            val cloudConfigured = appSettings.cloudBaseUrl.isNotBlank() && appSettings.cloudToken.isNotBlank()

            _uiState.value = current.copy(
                hasRawStreams = hasRaw,
                keyframeCount = keyframes,
                syncQuality = sync,
                postProcessGate = ProcessingPolicy.postProcess(hasRaw),
                colorizeGate = ProcessingPolicy.colorize(keyframes > 0, sync, allowPoor, repo.hasProcessedCloud(projectId)),
                exportGate = ProcessingPolicy.export(repo.hasProcessedCloud(projectId), repo.totalPoints() > 0),
                transferGate = ProcessingPolicy.transferBundle(hasRaw),
                cloudGate = ProcessingPolicy.cloudSubmit(hasRaw, cloudConfigured),
                exportNote = ProcessingPolicy.exportFormatNote(
                    current.exportFormat,
                    project.manifest.georef?.converged == true,
                ),
            )
        }
    }

    fun setMode(mode: ProcessingMode) {
        _uiState.value = _uiState.value.copy(mode = mode)
    }

    fun setExportFormat(format: ExportFormat) {
        val p = _uiState.value.project
        _uiState.value = _uiState.value.copy(
            exportFormat = format,
            exportNote = ProcessingPolicy.exportFormatNote(format, p?.manifest?.georef?.converged == true),
        )
    }

    fun dismissMessage() {
        _uiState.value = _uiState.value.copy(message = null)
    }

    // --- actions -------------------------------------------------------------

    fun postProcess() {
        val p = _uiState.value.project ?: return
        report(repo.submitPostProcess(projectId, p.directory), "Post-processing queued.")
    }

    fun colorize() {
        val p = _uiState.value.project ?: return
        val calib = p.manifest.mountCalibration
        // ROW-major, as the engine demands and as MountCalibration stores it —
        // the one conversion this app never does implicitly.
        val extrinsic = calib?.cameraFromLidar?.copyOf() ?: IDENTITY_4X4.copyOf()
        report(
            repo.submitColorize(
                projectId,
                p.directory,
                extrinsic,
                _uiState.value.syncQuality.code,
                appSettings.allowPoorSyncColorize,
                calib?.clockOffsetNs ?: 0L,
            ),
            "Colorization queued.",
        )
    }

    fun export() {
        val p = _uiState.value.project ?: return
        viewModelScope.launch {
            val format = _uiState.value.exportFormat
            val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            val out = File(File(p.directory, "exports").apply { mkdirs() }, "${exportBaseName(p.id)}-$stamp.${format.extension}")
            // A9's CRS seam. Both are empty until the georef converges, which is
            // exactly A9's documented "embed the local-frame placeholder" input
            // — a LAS labelled with a real CRS it is not in opens fine and lands
            // in the wrong place, which is worse than an honest placeholder.
            val g = p.manifest.georef
            val epsg = if (g?.converged == true && g.epsg != 0) "EPSG:${g.epsg}" else ""
            report(repo.submitExport(projectId, format, out, "", epsg), "Export queued -> ${out.name}")
        }
    }

    fun transferBundle(context: Context, share: Boolean) {
        val p = _uiState.value.project ?: return
        viewModelScope.launch {
            val out = File(File(p.directory, "exports").apply { mkdirs() }, "${exportBaseName(p.id)}.lscan.zip")
            val r = repo.submitTransferExport(projectId, p.directory, out, includeResults = true)
            if (r.isFailure) {
                report(r, "")
                return@launch
            }
            _uiState.value = _uiState.value.copy(
                message = "Packaging ${p.manifest.name} as ${out.name}. The share sheet opens when it is done.",
            )
            if (share) awaitJobThenShare(context, r.getOrThrow(), out)
        }
    }

    private suspend fun awaitJobThenShare(context: Context, jobId: Long, file: File) {
        while (true) {
            val job = repo.jobs.value.firstOrNull { it.id == jobId }
            if (job != null && job.state.isTerminal) {
                if (job.state == com.lidarscan.core.jobs.JobState.DONE && file.isFile) {
                    ShareTargets.shareFile(context, file, "application/zip", "Send .lscan bundle")
                } else {
                    _uiState.value = _uiState.value.copy(message = job.statusText)
                }
                return
            }
            kotlinx.coroutines.delay(400)
        }
    }

    // --- D3: the cloud path ---------------------------------------------------

    fun cancelCloud() {
        cloudCancelled = true
    }

    /**
     * §3.8's Cloud row, end to end: build the bundle with a real A15
     * `kTransferExport` job (so there is exactly one implementation of "what
     * goes in a `.lscan.zip`"), then upload/poll/download with the Kotlin
     * client against the documented REST contract.
     */
    fun cloudSubmit() {
        val p = _uiState.value.project ?: return
        cloudCancelled = false
        viewModelScope.launch {
            val cfgSettings = appSettings
            val zip = File(File(p.directory, "exports").apply { mkdirs() }, "${exportBaseName(p.id)}-cloud.lscan.zip")
            _uiState.value = _uiState.value.copy(
                cloud = CloudUploadState(running = true, phase = "Packaging the capture…"),
            )
            val bundleJob = repo.submitTransferExport(projectId, p.directory, zip, includeResults = false)
            if (bundleJob.isFailure) {
                _uiState.value = _uiState.value.copy(
                    cloud = CloudUploadState(error = bundleJob.exceptionOrNull()?.message),
                )
                return@launch
            }
            // Wait for the bundle before uploading — chaining these on the
            // engine side would need a kCloudSubmit job, which is the thing
            // this path deliberately does not use.
            while (true) {
                if (cloudCancelled) {
                    _uiState.value = _uiState.value.copy(cloud = CloudUploadState(error = "Cancelled."))
                    return@launch
                }
                val j = repo.jobs.value.firstOrNull { it.id == bundleJob.getOrThrow() }
                if (j != null && j.state.isTerminal) {
                    if (j.state != com.lidarscan.core.jobs.JobState.DONE) {
                        _uiState.value = _uiState.value.copy(cloud = CloudUploadState(error = j.statusText))
                        return@launch
                    }
                    break
                }
                kotlinx.coroutines.delay(400)
            }

            val client = CloudSubmitClient(
                transport = UrlConnectionHttpTransport(),
                config = CloudSubmitConfig(baseUrl = cfgSettings.cloudBaseUrl, token = cfgSettings.cloudToken),
            )
            _uiState.value = _uiState.value.copy(
                cloud = _uiState.value.cloud.copy(phase = "Uploading ${zip.length() / 1_000_000} MB…"),
            )
            val submitted = client.submit(
                zip,
                progress = { f ->
                    _uiState.value = _uiState.value.copy(cloud = _uiState.value.cloud.copy(uploadFraction = f))
                },
                cancelled = { cloudCancelled },
            )
            val jobId = submitted.getOrElse { e ->
                _uiState.value = _uiState.value.copy(cloud = CloudUploadState(error = cloudErrorText(e)))
                return@launch
            }
            _uiState.value = _uiState.value.copy(
                cloud = _uiState.value.cloud.copy(jobId = jobId, uploadFraction = 1f, phase = "Processing on the server…"),
            )
            val finished = client.awaitCompletion(
                jobId,
                onStatus = { s ->
                    _uiState.value = _uiState.value.copy(
                        cloud = _uiState.value.cloud.copy(serverState = s.state, serverProgress = s.progress),
                    )
                },
                cancelled = { cloudCancelled },
            ).getOrElse { e ->
                _uiState.value = _uiState.value.copy(cloud = _uiState.value.cloud.copy(running = false, error = cloudErrorText(e)))
                return@launch
            }
            val dest = File(File(p.directory, "processed").apply { mkdirs() }, "cloud-result-$jobId.zip")
            val bytes = client.downloadResult(jobId, dest).getOrElse { e ->
                _uiState.value = _uiState.value.copy(cloud = _uiState.value.cloud.copy(running = false, error = cloudErrorText(e)))
                return@launch
            }
            _uiState.value = _uiState.value.copy(
                cloud = _uiState.value.cloud.copy(
                    running = false,
                    phase = "Done — ${bytes / 1000} kB downloaded into processed/",
                    resultFile = dest,
                    serverState = finished.state,
                    serverProgress = 1f,
                ),
            )
        }
    }

    private fun cloudErrorText(e: Throwable): String = when (e) {
        is CloudError.PermissionDenied -> "The server rejected the token. Check it in Settings."
        is CloudError.TooLarge -> "The bundle is larger than the server's cap (§3.8's hard limit is 2 GiB)."
        is CloudError.Network -> "Could not reach the server: ${e.message}"
        is CloudError.Cancelled -> "Cancelled."
        is CloudError.JobFailed -> "The server reported a failure: ${e.status.message}"
        else -> e.message ?: e.toString()
    }

    // --- shared --------------------------------------------------------------

    fun cancelJob(id: Long) {
        repo.cancel(id)
    }

    private fun report(r: Result<Long>, ok: String) {
        _uiState.value = _uiState.value.copy(
            message = r.fold({ ok }, { it.message ?: "Failed." }),
        )
    }

    private companion object {
        val IDENTITY_4X4 = doubleArrayOf(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
    }
}
