package com.lidarscan.app.share

import android.content.Context
import com.lidarscan.app.processing.ProcessingRepository
import com.lidarscan.app.ui.common.exportBaseName
import com.lidarscan.core.jobs.JobState
import com.lidarscan.core.jobs.ProcessingJob
import com.lidarscan.core.model.ExportFormat
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * ROUND 23 item 104c — **one project's export, as something other than a
 * screen.**
 *
 * Group export needs to run the export path N times and collect the files it
 * produced. `ProcessingViewModel.export()` already does exactly that once, but
 * it is a ViewModel: it is bound to one `projectId` at construction, it reports
 * through `uiState.message`, and there is no way to ask it for the file. So the
 * *steps* move here and both callers share them —
 * [com.lidarscan.app.ui.processing.ProcessingViewModel] for the single-scan
 * button, [com.lidarscan.app.ui.projects.ProjectBatchViewModel] for the
 * selection bar's Export and Share.
 *
 * This is a **new caller, not a new pipeline**. Every step below is the one
 * `ProcessingViewModel` was already taking, in the same order and for the same
 * documented reasons:
 *
 *  * the A15 `kExport` job writes into `<project>.lscan/exports/`, because that
 *    is where the engine puts it and where a later cloud submit reads it from;
 *  * the CRS pair stays empty until the georef converges (A9's documented
 *    "embed the local-frame placeholder" input — a LAS labelled with a CRS it
 *    is not in opens fine and lands in the wrong place);
 *  * and the finished file is copied into `Downloads/LidarScan/` **before**
 *    anything is handed to a share sheet, which is ROUND 7's rule: the artifact
 *    exists somewhere the operator can reach whether or not they pick a target.
 */
class ProjectExporter(
    private val repo: ProcessingRepository,
    private val store: ProjectStore,
) {

    /** A finished export: the file on disk, and the path to say out loud. */
    data class Delivered(val file: File, val displayPath: String)

    /**
     * Exports one project and delivers it to Downloads.
     *
     * @param format null means "the project's own default", which is what the
     *   Processing screen and Review's format row both start from.
     * @return the delivered file, or a failure whose message is the reason —
     *   never a swallowed null. [com.lidarscan.core.projects.BatchExport] turns
     *   that reason into the batch report's failure line.
     */
    suspend fun exportToDownloads(
        context: Context,
        projectId: String,
        format: ExportFormat? = null,
        onProgress: (Float) -> Unit = {},
    ): Result<Delivered> = runCatching {
        val project = withContext(Dispatchers.IO) { store.open(projectId) }
            ?: error("This scan could not be opened.")
        val chosen = format
            ?: project.manifest.effectiveCaptureDefaults().exportFormat
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val out = File(
            File(project.directory, "exports").apply { mkdirs() },
            "${exportBaseName(project.id)}-$stamp.${chosen.extension}",
        )
        val g = project.manifest.georef
        val epsg = if (g?.converged == true && g.epsg != 0) "EPSG:${g.epsg}" else ""
        val jobId = repo.submitExport(projectId, chosen, out, "", epsg).getOrThrow()

        val job = awaitTerminal(repo, jobId, onProgress)
        if (job == null || job.state != JobState.DONE || !out.isFile) {
            error(job?.statusText ?: "the job disappeared")
        }
        val where = withContext(Dispatchers.IO) {
            DownloadsExporter.copyToDownloads(context, out)
        }.getOrThrow()
        Delivered(out, where)
    }

    companion object {

        /**
         * Polls a submitted job until it reaches a terminal state, forwarding
         * its progress.
         *
         * Extracted from `ProcessingViewModel.awaitJobThenDeliver` so the
         * single-scan and batch paths cannot drift into two different ideas of
         * "finished" — A15 reports terminality on the job, and this is the only
         * place that waits for it.
         *
         * @return the terminal job, or null if the job vanished from the queue.
         */
        suspend fun awaitTerminal(
            repo: ProcessingRepository,
            jobId: Long,
            onProgress: (Float) -> Unit = {},
            pollMillis: Long = 400,
        ): ProcessingJob? {
            var lastSeen: ProcessingJob? = null
            var misses = 0
            while (true) {
                val job = repo.jobs.value.firstOrNull { it.id == jobId }
                if (job == null) {
                    // A job that never appears is not the same as one that
                    // failed, but it is not a success either — give it a few
                    // polls (the queue is written from another thread) and then
                    // report the honest answer, which is null.
                    if (lastSeen != null) return lastSeen
                    if (++misses > MAX_MISSES) return null
                    delay(pollMillis)
                    continue
                }
                lastSeen = job
                onProgress(job.progress)
                if (job.state.isTerminal) return job
                delay(pollMillis)
            }
        }

        private const val MAX_MISSES = 25
    }
}
