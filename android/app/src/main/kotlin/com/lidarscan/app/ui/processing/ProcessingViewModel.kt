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
import com.lidarscan.core.jobs.JobKind
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
    /**
     * The transient banner Review still shows over the viewer.
     *
     * ROUND 28 item 163: the Jobs tab no longer reads this. It used to render
     * it as a `Snackbar` pinned above the tab bar — i.e. directly on top of the
     * queue rows describing the same event (J4) — with an unset `contentColor`
     * that drew at 1.05:1 in light and 1.25:1 in dark (J3), carrying a
     * filesystem path with a hash in it (J5). See [outcomes] for what replaced
     * it there. Review's own use is a different surface and is untouched.
     */
    val message: String? = null,
    /**
     * ROUND 28 item 163 — **the success IS the row** (§C.6).
     *
     * Job id → that job's ≤12-word result line, e.g. `Done · 813 KB ·
     * Downloads`. The delivery outcome is only known to this ViewModel (the
     * engine's job finishes when the bytes are written into the project; the
     * copy into `Downloads/LidarScan/` happens afterwards, here), so without
     * this map the queue could only ever say "Done" and the *useful* half of
     * the news had nowhere to live but a floating toast.
     *
     * The path and the byte count still go to the capture log, in full. What
     * the row shows is the size and the folder — no path, no hash.
     */
    val outcomes: Map<Long, String> = emptyMap(),
    val engineAvailable: Boolean = true,
    /** ROUND 19 item 76: a blocking D6 reprocess is running from this screen. */
    val d6Processing: Boolean = false,
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

    /** True once the operator has picked a mode themselves; stops [reload] re-defaulting it. */
    private var modeChosenByOperator = false

    /** The latest DataStore snapshot, mirrored so the gate recomputation is not itself suspending. */
    private var appSettings = com.lidarscan.app.data.AppSettings()

    /** ROUND 28 item 163: `recomputeGates` runs on every job tick; the log line does not. */
    private var lastLoggedGateReason: String? = null

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
            // ROUND 7 (owner field item): a COIN-D6 project opens on "Save to
            // phone". Its Process path is refused (§6 — the post pipeline is
            // Mid-360's) and the bundle is the only route off the phone, so
            // landing the operator on a mode that cannot run and then on a
            // refusal that mentions the cloud is how `scan-008` got stuck.
            // ROUND 25 item 119: an STL-27L container lands here too — its
            // Process path is refused for the same reason the D6's is, so it
            // must open on the mode that actually works.
            val defaultMode = if (project?.manifest?.sensor?.isPhoneTrackedPushbroom == true) {
                ProcessingMode.EXTRACT_FOR_TRANSFER
            } else {
                _uiState.value.mode
            }
            _uiState.value = _uiState.value.copy(
                project = project,
                mode = if (modeChosenByOperator) _uiState.value.mode else defaultMode,
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

            val postGate = ProcessingPolicy.postProcess(hasRaw, project.manifest.sensor)
            // ROUND 28 item 163: the paragraph that used to be on the screen is
            // now on exactly one surface — this one. `logReason` is written once
            // per distinct refusal so a support log still explains why an
            // offline re-run was never offered for a pushbroom scan, which is
            // the only question the 62 words ever answered.
            postGate.logReason?.takeIf { it != lastLoggedGateReason }?.let {
                lastLoggedGateReason = it
                log(it)
            }
            _uiState.value = current.copy(
                hasRawStreams = hasRaw,
                keyframeCount = keyframes,
                syncQuality = sync,
                postProcessGate = postGate,
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
        modeChosenByOperator = true
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
        // ── ROUND 19 item 76: ONE process pipeline for a D6 ─────────────────
        //
        // Round 16 named "three process surfaces over two different
        // pipelines" and left them; this closes the pipeline half. For a D6
        // container this button used to queue JobKind.POST_PROCESS — a plain
        // re-resolve into the shared viewer store, with none of the stitch /
        // rescue / loop-end corrections and nothing written to processed/ —
        // while Review's card and the seal's auto-process ran
        // reprocessD6. Same word on the button, different result on disk.
        // Now every D6 "Process" is reprocessD6: same per-container lock,
        // same derived files, same verdicts. The queue remains what it always
        // was for a Mid-360, whose pipeline genuinely is a queued LIO re-run.
        //
        // ROUND 25 item 119: this test stays `== COIN_D6` on purpose, and it is
        // one of the few that does. `reprocessD6` runs the engine's OFFLINE D6
        // pipeline, which identifies a container by its lidar stream id — an
        // STL-27L container carries `SCAN_STREAM_LIDAR_STL27L` and there is no
        // offline pipeline for it yet. Handing one to `reprocessD6` because
        // "the STL-27L behaves like a D6" would be the one place that phrase is
        // false. An STL-27L is refused below, by name, rather than falling
        // through to the Mid-360's LIO queue, which would be worse still.
        if (p.manifest.sensor == com.lidarscan.core.model.SensorType.STL27L) {
            _uiState.value = _uiState.value.copy(
                message = ProcessingPolicy.postProcess(
                    hasRawStreams = true,
                    sensor = p.manifest.sensor,
                ).reason,
            )
            return
        }
        if (p.manifest.sensor == com.lidarscan.core.model.SensorType.COIN_D6) {
            if (_uiState.value.d6Processing) return
            _uiState.value = _uiState.value.copy(d6Processing = true, message = "Processing…")
            viewModelScope.launch {
                val r = withContext(Dispatchers.IO) {
                    runCatching { repo.reprocessD6(p.directory) }.getOrNull()
                }
                _uiState.value = _uiState.value.copy(
                    d6Processing = false,
                    message = if (r?.ran == true) {
                        "Processed — open Review to see the corrected map."
                    } else {
                        "This scan could not be processed. Its recorded data may be " +
                            "incomplete — the raw files are untouched either way."
                    },
                )
            }
            return
        }
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

    /**
     * ROUND 7 (owner field item: "I exported scan-008 and the file is nowhere").
     *
     * The job still writes into `<project>.lscan/exports/`, which is where the
     * engine puts it and where the cloud path reads it from — but that directory
     * is under `Android/data/`, which the system Files app has refused to browse
     * since Android 11. So the export is now **followed to completion** and
     * copied into `Downloads/LidarScan/`, and the operator is told the
     * destination by name. See [com.lidarscan.app.share.DownloadsExporter].
     *
     * ROUND 23 item 104a — **[share] is the whole of the Review screen's new
     * Share button.**
     *
     * The owner's report is that export/share "vanished": Review grew a
     * full-width Export in round 22 and lost the hand-off to the system sheet
     * that the bundle path had always had. Rather than a second export
     * implementation next to this one, Share is this same call with one flag —
     * the same job, the same file, the same Downloads copy FIRST, and then the
     * sheet. That ordering is not incidental: it is what
     * `transferBundle(context, share = true)` has done since ROUND 7, for the
     * reason [awaitJobThenDeliver] documents.
     */
    fun export(context: Context, share: Boolean = false) {
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
            val submitted = repo.submitExport(projectId, format, out, "", epsg)
            if (submitted.isFailure) {
                report(submitted, "")
                return@launch
            }
            _uiState.value = _uiState.value.copy(message = "Exporting ${out.name}…")
            awaitJobThenDeliver(context, submitted.getOrThrow(), out, share)
        }
    }

    fun transferBundle(context: Context, share: Boolean) {
        val p = _uiState.value.project ?: return
        viewModelScope.launch {
            // ── ROUND 18 item 71: no silent half-bundle. ────────────────────
            //
            // The owner's scan-053 was exported before its auto-process
            // finished: the bundle carried processed/preview.f32 and nothing
            // else — no trajectory.bin, no map_stitched.bin — and nothing in
            // it said so. An export must hand over either the processed
            // results or a written statement of why they are absent. Three
            // arms: already processed -> proceed; processable -> process now
            // (the repository's per-container lock makes this WAIT for a
            // still-running auto-process rather than racing it); neither ->
            // write processed/UNPROCESSED.txt naming what is missing, so the
            // recipient of the bundle reads the reason instead of guessing.
            withContext(Dispatchers.IO) { ensureProcessedForExport(p.directory) }
            // ROUND 7: staged in the CACHE, not in `<project>/exports/`.
            // `zip_export()` walks the project directory recursively and takes
            // every regular file it finds — so a bundle written inside that
            // directory gets swallowed by the NEXT export, and a project
            // exported three times carries three nested copies of itself. The
            // deliverable is the Downloads copy; this is scratch, and the OS may
            // reclaim it.
            val out = File(bundleStagingDir(), "${exportBaseName(p.id)}.lscan.zip")
            val r = repo.submitTransferExport(projectId, p.directory, out, includeResults = true)
            if (r.isFailure) {
                report(r, "")
                return@launch
            }
            _uiState.value = _uiState.value.copy(message = "Packaging ${p.manifest.name} as ${out.name}…")
            awaitJobThenDeliver(context, r.getOrThrow(), out, share)
        }
    }

    /**
     * ROUND 7 — **every user-triggered file operation ends in a visible success
     * with a path, or a visible failure. There is no third outcome.**
     *
     * This replaces `awaitJobThenShare`, which had two silent exits: a share
     * sheet is a hand-off with no result callback (dismiss it and the app has
     * already forgotten), and on the plain Export path nothing was ever handed
     * off at all — the file simply appeared somewhere no file manager can
     * browse. Both are how the owner's scan-008 bundle "went nowhere".
     *
     * The order matters: the file is copied into `Downloads/LidarScan/` FIRST,
     * so the artifact exists in a place the operator can reach whether or not
     * they then pick anything from the share sheet.
     */
    private suspend fun awaitJobThenDeliver(context: Context, jobId: Long, file: File, share: Boolean) {
        // ROUND 23 item 104c: the wait itself moved to
        // ProjectExporter.awaitTerminal so the batch path and this one
        // cannot grow two different ideas of "finished". Everything below
        // — the messages, the log lines, the copy-failed fallback — is
        // unchanged.
        val job = com.lidarscan.app.share.ProjectExporter.awaitTerminal(repo, jobId)
        if (job == null || job.state != com.lidarscan.core.jobs.JobState.DONE || !file.isFile) {
            val why = job?.statusText ?: "the job disappeared"
            _uiState.value = _uiState.value.copy(
                message = "Export failed ($why). Nothing was written to Downloads.",
                outcomes = _uiState.value.outcomes + (jobId to "Failed · $why"),
            )
            log("export FAILED job=$jobId state=${job?.state} file=${file.name} reason=$why")
            return
        }

        val copied = withContext(Dispatchers.IO) {
            com.lidarscan.app.share.DownloadsExporter.copyToDownloads(context, file)
        }
        copied.fold(
            onSuccess = { where ->
                _uiState.value = _uiState.value.copy(
                    message = "Exported to $where (${humanBytes(file.length())}).",
                    // ROUND 28 item 163 (J5): the row gets the size and the
                    // folder. `$where` is `Downloads/LidarScan/scan-085-…-
                    // e035f2-20260821-212507.ply`, which was the *success*
                    // message, three lines long, and unreadable at a glance.
                    // The full path is one line down, in the log, where the
                    // person who needs it is looking.
                    outcomes = _uiState.value.outcomes +
                        (jobId to "Done · ${humanBytes(file.length())} · $DOWNLOADS_FOLDER"),
                )
                log("export OK job=$jobId ${file.name} -> $where bytes=${file.length()}")
                if (share) {
                    ShareTargets.shareFile(context, file, ShareTargets.mimeFor(file), "Send ${file.name}")
                    log("export share sheet opened for ${file.name}")
                }
            },
            onFailure = { e ->
                // The bytes DO exist — say where, even though that path is
                // not browsable, because adb and a desktop can still reach
                // it and a lost capture is worse than an ugly sentence.
                _uiState.value = _uiState.value.copy(
                    message = "Export finished but could not be copied to Downloads " +
                        "(${e.javaClass.simpleName}: ${e.message}). The file is on the phone at " +
                        "${file.absolutePath} — use the share sheet or a USB cable to get it off.",
                    // The row states the half the operator can act on. The
                    // absolute path is in the log line below it.
                    outcomes = _uiState.value.outcomes +
                        (jobId to "Done, but not in Downloads. Share it."),
                )
                log("export COPY FAILED ${file.name}: ${e.javaClass.name}: ${e.message}")
                // Offer the hand-off anyway: it is the only remaining route.
                runCatching {
                    ShareTargets.shareFile(context, file, ShareTargets.mimeFor(file), "Send ${file.name}")
                }
            },
        )
        return
    }

    /**
     * ROUND 18 item 71 — see [transferBundle]. Blocking (tens of seconds when
     * it has to process); call from Dispatchers.IO.
     */
    private fun ensureProcessedForExport(dir: File) {
        val marker = File(dir, "processed/UNPROCESSED.txt")
        if (repo.hasStitchedCloud(dir)) {
            // A marker from an earlier failed attempt is stale the moment the
            // results exist — a bundle carrying both would contradict itself.
            runCatching { marker.delete() }
            return
        }
        val hasPoses = File(dir, "streams/poses_ar.bin").isFile
        val hasMap = File(dir, "streams/map.bin").isFile
        if (hasPoses && hasMap) {
            _uiState.value = _uiState.value.copy(message = "Processing before export…")
            val r = runCatching { repo.reprocessD6(dir, refineSeams = true) }.getOrNull()
            if (r?.ran == true && repo.hasStitchedCloud(dir)) {
                runCatching { marker.delete() }
                return
            }
        }
        runCatching {
            marker.parentFile?.mkdirs()
            marker.writeText(
                "This bundle was exported WITHOUT processed results.\n\n" +
                    when {
                        !hasPoses ->
                            "Reason: the capture recorded no camera trajectory " +
                                "(streams/poses_ar.bin is absent), so there is nothing to " +
                                "resolve the returns against.\n"
                        !hasMap ->
                            "Reason: the capture resolved no world points " +
                                "(streams/map.bin is absent).\n"
                        else ->
                            "Reason: processing was attempted at export time and failed; " +
                                "the raw streams are intact — open the scan in ${com.lidarscan.core.Wording.APP_NAME} " +
                                "and tap Process to retry.\n"
                    } +
                    "The raw sensor streams under streams/ are complete and untouched; " +
                    "processed/map_stitched.bin, trajectory.bin and stitch.json are what " +
                    "a successful Process would add.\n",
            )
        }
    }

    /** Scratch space for `.lscan.zip` bundles — deliberately OUTSIDE the project directory. */
    private fun bundleStagingDir(): File =
        File(container.applicationContext.cacheDir, "bundles").apply { mkdirs() }

    private fun log(line: String) {
        runCatching { container.captureLog.log(com.lidarscan.app.debug.CaptureLog.TAG_EXPORT, line) }
    }

    private fun humanBytes(bytes: Long): String = when {
        bytes >= 1L shl 30 -> "%.1f GB".format(bytes / (1L shl 30).toDouble())
        bytes >= 1L shl 20 -> "%.1f MB".format(bytes / (1L shl 20).toDouble())
        else -> "%d KB".format(bytes / 1024)
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
            // ROUND 7: cache, not `<project>/exports/` — see transferBundle().
            val zip = File(bundleStagingDir(), "${exportBaseName(p.id)}-cloud.lscan.zip")
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
            // ROUND 7: the cloud result is a user-visible artifact too, and
            // `processed/` is under Android/data — i.e. exactly as unreachable
            // as the export bundle the owner could not find. Same rule: a path
            // the operator can actually open, or a stated reason why not.
            val delivered = withContext(Dispatchers.IO) {
                com.lidarscan.app.share.DownloadsExporter.copyToDownloads(
                    context = container.applicationContext,
                    source = dest,
                    fileName = "${exportBaseName(p.id)}-cloud-result.zip",
                )
            }
            val where = delivered.getOrNull()
            log(
                "cloud result ${dest.name} bytes=$bytes -> " +
                    (where ?: "COPY FAILED: ${delivered.exceptionOrNull()?.message}"),
            )
            _uiState.value = _uiState.value.copy(
                cloud = _uiState.value.cloud.copy(
                    running = false,
                    phase = if (where != null) {
                        "Done — ${bytes / 1000} kB. Saved to $where."
                    } else {
                        "Done — ${bytes / 1000} kB, but it could not be copied to Downloads. " +
                            "It is on the phone at ${dest.absolutePath}."
                    },
                    resultFile = dest,
                    serverState = finished.state,
                    serverProgress = 1f,
                ),
            )
        }
    }

    private fun cloudErrorText(e: Throwable): String = when (e) {
        is CloudError.PermissionDenied -> "The server rejected the token. Check it in Settings."
        // ROUND 28 item 163: "§3.8's hard limit" is a spec citation, and "§" is
        // on the wording law's own jargon list. The number is the useful half.
        is CloudError.TooLarge -> "This scan is too big for the server (2 GB cap)."
        is CloudError.Network -> "Could not reach the server: ${e.message}"
        is CloudError.Cancelled -> "Cancelled."
        is CloudError.JobFailed -> "The server reported a failure: ${e.status.message}"
        else -> e.message ?: e.toString()
    }

    // --- shared --------------------------------------------------------------

    fun cancelJob(id: Long) {
        repo.cancel(id)
    }

    /**
     * ROUND 28 item 163 — **the one control a read-only queue still needs.**
     *
     * §C.6's error pattern is "a Secondary button performing the fix, inline,
     * at the point of failure", and on a queue the point of failure is the row.
     * This is not the task launcher §D.6 deleted: it re-runs a job the operator
     * already started, from the row that reports it failed, rather than
     * offering a fresh one. Every *new* job now starts from Review or from the
     * Projects `⋯` menu, where the operator is looking at the scan.
     *
     * A failed job carries no re-submit handle (the engine's queue is keyed by
     * id and a dead id cannot be revived), so retry means "submit the same kind
     * again for this project" — which is exactly what the operator tapped the
     * first time.
     */
    fun retry(context: Context, job: ProcessingJob) {
        // Drop the stale outcome first: a row that says "Failed · Storage full"
        // while a fresh attempt is running is the queue lying about itself.
        _uiState.value = _uiState.value.copy(outcomes = _uiState.value.outcomes - job.id)
        when (job.kind) {
            JobKind.EXPORT_POINTS -> export(context)
            JobKind.TRANSFER_EXPORT -> transferBundle(context, share = false)
            JobKind.POST_PROCESS -> postProcess()
            JobKind.COLORIZE -> colorize()
            JobKind.CLOUD_SUBMIT -> cloudSubmit()
        }
    }

    private fun report(r: Result<Long>, ok: String) {
        _uiState.value = _uiState.value.copy(
            message = r.fold({ ok }, { it.message ?: "Failed." }),
        )
    }

    private companion object {
        val IDENTITY_4X4 = doubleArrayOf(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)

        /**
         * The word a person uses for where the file went. `DownloadsExporter`
         * writes into `Downloads/LidarScan/`; the sub-folder is a detail of
         * ours, and the operator opens Downloads.
         */
        const val DOWNLOADS_FOLDER = "Downloads"
    }
}
