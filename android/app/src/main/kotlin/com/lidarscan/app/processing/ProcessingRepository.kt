package com.lidarscan.app.processing

import android.util.Log
import com.lidarscan.app.engine.NativePlanArrays
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.jobs.JobKind
import com.lidarscan.core.jobs.JobState
import com.lidarscan.core.jobs.ProcessingJob
import com.lidarscan.core.model.ExportFormat
import com.lidarscan.core.plan.PlanModel
import com.lidarscan.core.plan.PlanOptions
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * B6/B11/B12 — the app's single door onto `cpp/processing_engine.{h,cpp}`.
 *
 * **One instance per process**, held by `AppContainer`, created lazily: the
 * native side starts a JobQueue worker thread on first use and the
 * overwhelming majority of app launches never process anything, which is the
 * same reasoning `Engine::jobs()` itself gives for being lazy.
 *
 * **It is not per project.** The queue is a device-level resource (one worker
 * thread, one at a time) and a job outlives the screen that submitted it —
 * navigating away from Processing must not cancel a 20-minute post-process.
 * Each job therefore carries the project it belongs to on the Kotlin side
 * ([jobProjects]); the engine's `Job` has no such field and does not need one.
 */
class ProcessingRepository(private val scope: CoroutineScope) {

    private var handle: Long = 0L

    private val _jobs = MutableStateFlow<List<ProcessingJob>>(emptyList())
    val jobs: StateFlow<List<ProcessingJob>> = _jobs.asStateFlow()

    private val _plan = MutableStateFlow<PlanModel?>(null)
    val plan: StateFlow<PlanModel?> = _plan.asStateFlow()

    private val _planRunning = MutableStateFlow(false)
    val planRunning: StateFlow<Boolean> = _planRunning.asStateFlow()

    private val _planProgress = MutableStateFlow(0f)
    val planProgress: StateFlow<Float> = _planProgress.asStateFlow()

    /** job id -> project id. The engine's `Job` carries no project, so the app keeps the mapping. */
    private val jobProjects = mutableMapOf<Long, String>()

    /** project id -> its most recent finished post-process job, for chaining. */
    private val lastPostProcess = mutableMapOf<String, Long>()

    /** project id -> its most recent finished colorize job. */
    private val lastColorize = mutableMapOf<String, Long>()

    private var pollJob: kotlinx.coroutines.Job? = null

    val isAvailable: Boolean get() = ScanEngineNative.isAvailable

    @Synchronized
    private fun ensureHandle(): Long {
        if (handle != 0L) return handle
        if (!ScanEngineNative.isAvailable) return 0L
        handle = ScanEngineNative.nativeProcCreate()
        if (handle == 0L) {
            Log.e(TAG, "nativeProcCreate returned 0 — the processing engine could not be created")
            return 0L
        }
        // Progress arrives on the JobQueue worker thread via the engine's own
        // EventBus (EventType::kJobProgress). It updates the *snapshot* rather
        // than the list, because a queue row also carries a stage label and an
        // error that only `list()` has — so the event is what makes the bar
        // move promptly and the poll below is what keeps the text honest.
        ScanEngineNative.nativeProcSetJobProgressListener(handle) { id, progress, state ->
            _jobs.value = _jobs.value.map {
                if (it.id == id) it.copy(progress = progress, state = JobState.fromCode(state)) else it
            }
        }
        startPolling()
        return handle
    }

    private fun startPolling() {
        if (pollJob != null) return
        pollJob = scope.launch(Dispatchers.Default) {
            while (true) {
                refresh()
                // 500 ms: fast enough that a stage change reads as immediate,
                // slow enough that a JNI array allocation per tick is free. The
                // progress *bar* does not wait for this — the event listener
                // above drives it.
                delay(500)
            }
        }
    }

    private fun refresh() {
        val h = handle
        if (h == 0L) return
        val native = ScanEngineNative.nativeProcJobs(h) ?: return
        val list = native.map {
            ProcessingJob(
                id = it.id,
                kind = JobKind.fromCode(it.kind),
                state = JobState.fromCode(it.state),
                progress = it.progress,
                stage = it.stage,
                error = it.error,
                message = it.message,
            )
        }
        for (j in list) {
            if (j.state == JobState.DONE) {
                val pid = jobProjects[j.id] ?: continue
                when (j.kind) {
                    JobKind.POST_PROCESS -> lastPostProcess[pid] = j.id
                    JobKind.COLORIZE -> lastColorize[pid] = j.id
                    else -> Unit
                }
            }
        }
        _jobs.value = list
    }

    fun jobsFor(projectId: String): List<ProcessingJob> =
        _jobs.value.filter { jobProjects[it.id] == projectId }

    fun lastError(): String {
        val h = handle
        return if (h == 0L) "the native processing engine is not available in this build" else ScanEngineNative.nativeProcLastError(h)
    }

    /** True once a post-process job for [projectId] has finished — i.e. there is a cloud to colorize/export/slice. */
    fun hasProcessedCloud(projectId: String): Boolean = lastPostProcess.containsKey(projectId)

    fun totalPoints(): Long = if (handle == 0L) 0L else ScanEngineNative.nativeProcTotalPoints(handle)

    // --- submissions ---------------------------------------------------------

    fun submitPostProcess(projectId: String, lscanDir: File): Result<Long> = submit(projectId) { h ->
        ScanEngineNative.nativeProcSubmitPostProcess(h, lscanDir.absolutePath)
    }

    /**
     * @param cameraFromLidar the mount calibration's ROW-MAJOR extrinsic.
     * @param syncQuality A4's verdict for the lidar stream. Passing 0 (UNKNOWN)
     *   makes the engine refuse — deliberately, and the UI gates on it before
     *   getting here so the refusal is explained rather than relayed.
     */
    fun submitColorize(
        projectId: String,
        lscanDir: File,
        cameraFromLidar: DoubleArray,
        syncQuality: Int,
        allowPoorSync: Boolean,
        clockOffsetNs: Long,
    ): Result<Long> = submit(projectId) { h ->
        ScanEngineNative.nativeProcSubmitColorize(
            h,
            lastPostProcess[projectId] ?: 0L,
            lscanDir.absolutePath,
            cameraFromLidar,
            syncQuality,
            allowPoorSync,
            clockOffsetNs,
        )
    }

    fun submitExport(
        projectId: String,
        format: ExportFormat,
        outputFile: File,
        crsWkt: String,
        crsEpsg: String,
    ): Result<Long> = submit(projectId) { h ->
        // Chain from the colorized store when there is one, else the
        // post-processed one — exporting the *painted* cloud is what a user who
        // just ran Colorize expects, and A15 resolves the chain to the same
        // PageStore either way.
        val chain = lastColorize[projectId] ?: lastPostProcess[projectId] ?: 0L
        ScanEngineNative.nativeProcSubmitExport(h, chain, format.code, outputFile.absolutePath, crsWkt, crsEpsg)
    }

    fun submitTransferExport(projectId: String, projectDir: File, zipFile: File, includeResults: Boolean): Result<Long> =
        submit(projectId) { h ->
            ScanEngineNative.nativeProcSubmitTransferExport(
                h,
                projectDir.absolutePath,
                zipFile.absolutePath,
                includeResults,
            )
        }

    private inline fun submit(projectId: String, block: (Long) -> Long): Result<Long> {
        val h = ensureHandle()
        if (h == 0L) return Result.failure(IllegalStateException(lastError()))
        val id = block(h)
        if (id == 0L) return Result.failure(IllegalStateException(ScanEngineNative.nativeProcLastError(h)))
        jobProjects[id] = projectId
        refresh()
        return Result.success(id)
    }

    fun cancel(jobId: Long): Boolean {
        val h = handle
        if (h == 0L) return false
        val ok = ScanEngineNative.nativeProcCancelJob(h, jobId)
        refresh()
        return ok
    }

    // --- B11: floor plan -----------------------------------------------------

    /**
     * Runs A12's extraction on the processed cloud. BLOCKING natively, so it is
     * dispatched off the main thread here; the progress flow is polled from the
     * native side because `extract_floor_plan()`'s progress callback is a plain
     * function pointer and bouncing every tick across JNI would cost more than
     * it buys at this granularity.
     */
    suspend fun extractPlan(opts: PlanOptions): Result<PlanModel> = withContext(Dispatchers.Default) {
        val h = ensureHandle()
        if (h == 0L) return@withContext Result.failure(IllegalStateException(lastError()))
        _planRunning.value = true
        _planProgress.value = 0f
        val ticker = scope.launch(Dispatchers.Default) {
            while (true) {
                _planProgress.value = ScanEngineNative.nativeProcPlanProgress(h)
                delay(200)
            }
        }
        try {
            val ok = ScanEngineNative.nativeProcRunPlan(
                h,
                opts.zMinM,
                opts.zMaxM,
                opts.gridResM,
                opts.snapOrthogonal,
                opts.snapToleranceDeg,
                opts.minCellPoints,
                opts.windowSillCheck,
                opts.detectRooms,
                opts.detectOpenings,
            )
            if (!ok) return@withContext Result.failure(IllegalStateException(ScanEngineNative.nativeProcLastError(h)))
            val model = NativePlanArrays.decode(
                wallsD = ScanEngineNative.nativeProcPlanWallsD(h),
                wallsI = ScanEngineNative.nativeProcPlanWallsI(h),
                openingsD = ScanEngineNative.nativeProcPlanOpeningsD(h),
                openingsI = ScanEngineNative.nativeProcPlanOpeningsI(h),
                roomsD = ScanEngineNative.nativeProcPlanRoomsD(h),
                roomsI = ScanEngineNative.nativeProcPlanRoomsI(h),
                roomPolygons = ScanEngineNative.nativeProcPlanRoomPolygons(h),
                roomLabels = ScanEngineNative.nativeProcPlanRoomLabels(h),
                summary = ScanEngineNative.nativeProcPlanSummary(h),
            )
            _plan.value = model
            Result.success(model)
        } finally {
            ticker.cancel()
            _planRunning.value = false
            _planProgress.value = 1f
        }
    }

    fun cancelPlan() {
        if (handle != 0L) ScanEngineNative.nativeProcCancelPlan(handle)
    }

    suspend fun writePlanDxf(dest: File): Result<File> = withContext(Dispatchers.IO) {
        val h = handle
        if (h == 0L) return@withContext Result.failure(IllegalStateException(lastError()))
        if (ScanEngineNative.nativeProcWritePlanDxf(h, dest.absolutePath)) {
            Result.success(dest)
        } else {
            Result.failure(IllegalStateException(ScanEngineNative.nativeProcLastError(h)))
        }
    }

    suspend fun writePlanPdf(dest: File, title: String, project: String, date: String): Result<File> =
        withContext(Dispatchers.IO) {
            val h = handle
            if (h == 0L) return@withContext Result.failure(IllegalStateException(lastError()))
            if (ScanEngineNative.nativeProcWritePlanPdf(h, dest.absolutePath, title, project, date)) {
                Result.success(dest)
            } else {
                Result.failure(IllegalStateException(ScanEngineNative.nativeProcLastError(h)))
            }
        }

    /** The handle, for the Review screen's point-page reads. 0 when unavailable. */
    fun handleOrZero(): Long = ensureHandle()

    companion object {
        private const val TAG = "ProcessingRepo"
    }
}
