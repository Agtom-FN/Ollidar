package com.lidarscan.app.ui.review

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.data.Units
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.render.PointCloudRenderer
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.app.render.ProcessingCloudSource
import com.lidarscan.app.render.samplePoints
import com.lidarscan.core.measure.MeasureUnit
import com.lidarscan.core.measure.Measurement
import com.lidarscan.core.measure.Vec3
import com.lidarscan.core.measure.formatDistance
import com.lidarscan.core.measure.pickNearestPoint
import com.lidarscan.core.render.DisplayParams
import com.lidarscan.core.render.DisplayProfile
import com.lidarscan.core.render.clamped
import com.lidarscan.core.render.colorModeAvailability
import com.lidarscan.core.render.profileDefaults
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.isActive
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * ROUND 8 (owner item 27c): what Review is doing with the project it was handed.
 *
 * The states exist because "open a saved scan" is now three different actions
 * with three different latencies, and the operator is entitled to know which
 * one is happening rather than watching an empty viewport and guessing.
 */
enum class ReviewLoad {
    /** Reading the container to find out what is in it. Milliseconds. */
    PROBING,

    /** Reading back the resolved cloud the capture cached. Fast — this is the normal path. */
    LOADING_RECORDED,

    /** Re-resolving from the raw returns and the trajectory. Seconds. */
    RESOLVING,

    /** A 3D cloud is on screen. */
    READY,

    /**
     * A COIN-D6 capture with returns but no trajectory — every capture this app
     * made before 0.5.0. There is nothing to show and nothing that can be done
     * about it; see [ReviewUiState.loadMessage].
     */
    NO_TRAJECTORY,

    /** Something else went wrong; [ReviewUiState.loadMessage] says what. */
    FAILED,
}

data class ReviewUiState(
    val project: Project? = null,
    val display: DisplayParams = DisplayParams(),
    val totalPoints: Long = 0,
    val measureMode: Boolean = false,
    val firstPick: Vec3? = null,
    val measurement: Measurement? = null,
    val measureUnit: MeasureUnit = MeasureUnit.METERS,
    val pickMessage: String? = null,
    val colorModeReasons: Map<com.lidarscan.core.render.ColorMode, String?> = emptyMap(),
    val hasCloud: Boolean = false,
    val load: ReviewLoad = ReviewLoad.PROBING,
    /** One paragraph the viewport shows when there is nothing to draw. Never a bare error code. */
    val loadMessage: String? = null,
    /** What the container turned out to contain — drives everything above. */
    val probe: com.lidarscan.app.engine.ProjectProbe = com.lidarscan.app.engine.ProjectProbe.NONE,
    // --- ROUND 13: "Process this scan" ------------------------------------
    /** Section count from the capture's own `project.json`; >1 means the map is in pieces. */
    val sections: Int = 1,
    /** True once `processed/map_stitched.bin` exists — the cloud on screen is the corrected one. */
    val isStitched: Boolean = false,
    val processing: Boolean = false,
    val processProgress: Float = 0f,
    /** What the last run did, in the operator's words. Survives until they leave the screen. */
    val stitch: com.lidarscan.core.capture.StitchResult? = null,
    val processError: String? = null,
)

/**
 * B10 + B11 — the Review screen: §3.13's "Review (viewer, display params,
 * measure, plan view, export)".
 *
 * **Display parameters are saved per project, on change, with a debounce.**
 * §3.9 says "settings persist per project"; the debounce exists because a
 * slider drag emits a value per frame and each save is a manifest rewrite
 * (temp file + rename). 400 ms after the last change is imperceptible to a
 * user and turns a drag into one write.
 */
class ReviewViewModel(
    container: AppContainer,
    private val store: ProjectStore,
    private val settings: SettingsRepository,
    private val projectId: String,
) : ViewModel() {

    private val processing = container.processingRepository

    private val _uiState = MutableStateFlow(ReviewUiState())
    val uiState: StateFlow<ReviewUiState> = _uiState.asStateFlow()

    /** The processed cloud — the Review screen shows what processing produced, not a live capture. */
    val cloudSource: PointCloudSource = ProcessingCloudSource { processing.handleOrZero() }

    private var renderer: PointCloudRenderer? = null
    private var saveJob: Job? = null

    /**
     * The measure tool's candidate set. Sampled once per pick session rather
     * than per tap: a 200k-point sample of a multi-million-point cloud takes a
     * noticeable moment to copy out of the page buffers, and doing it per tap
     * would make every measurement feel broken.
     */
    private var pickCandidates: List<Vec3> = emptyList()

    /** ROUND 13: the Process run, so leaving the screen cancels it. */
    private var processJob: Job? = null

    // ── ROUND 13 (owner: "5 sections" must be fixable, not just counted) ────
    //
    // A capture that broke into sections is five maps in five world frames, and
    // until this runs the room genuinely is a metre apart — which is what the
    // owner saw and called "result not satisfy". The correction is analytic
    // (see engine slam/post/section_stitch.h): the frame change ARCore applied
    // is written down in the pose jump itself.
    //
    // It runs on Dispatchers.IO because it is the WHOLE offline resolve — tens
    // of seconds for a one-minute walk — and it publishes nothing into the
    // viewer's store while it runs, for the same reason the engine gives it its
    // own PageStore: a half-corrected map is two frames at once. When it
    // finishes, the project is simply re-opened, and `load_recorded_cloud`
    // prefers the stitched file from then on.
    fun processScan() {
        val p = _uiState.value.project ?: return
        if (_uiState.value.processing) return
        _uiState.value = _uiState.value.copy(
            processing = true,
            processProgress = 0f,
            processError = null,
            stitch = null,
        )
        processJob?.cancel()
        processJob = viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                processing.reprocessD6(p.directory) { f ->
                    // Hop to the main dispatcher rather than writing the flow
                    // from the native thread; and returning false here is how a
                    // cancelled ViewModel stops a run that would otherwise keep
                    // a core busy after the screen is gone.
                    viewModelScope.launch { setProgress(f) }
                    isActive
                }
            }
            if (result == null || !result.ran) {
                _uiState.value = _uiState.value.copy(
                    processing = false,
                    processError = "This scan could not be processed. Its recorded data may be " +
                        "incomplete — the raw files are untouched either way.",
                )
                return@launch
            }
            _uiState.value = _uiState.value.copy(
                processing = false,
                processProgress = 1f,
                stitch = result,
                isStitched = result.changedAnything,
            )
            // Re-open so the viewport draws the corrected map. Nothing else in
            // the load path changes: the engine prefers the stitched file at
            // the one function every reader already goes through.
            if (result.changedAnything) {
                processing.clearCloud()
                openProjectCloud(p)
            }
        }
    }

    private fun setProgress(f: Float) {
        val s = _uiState.value
        // Monotone: a progress bar that goes backwards reads as a bug even when
        // the underlying number is honest.
        if (s.processing && f > s.processProgress) {
            _uiState.value = s.copy(processProgress = f.coerceIn(0f, 1f))
        }
    }

    init {
        viewModelScope.launch {
            val p = withContext(Dispatchers.IO) { store.open(projectId) }
            _uiState.value = _uiState.value.copy(
                project = p,
                display = p?.manifest?.effectiveDisplayParams() ?: DisplayParams(),
            )
            if (p != null) {
                // ROUND 13: how many pieces the capture is in, and whether it
                // has already been put back together.
                val stitched = withContext(Dispatchers.IO) { processing.hasStitchedCloud(p.directory) }
                _uiState.value = _uiState.value.copy(
                    sections = p.manifest.sectionBreaks.size + 1,
                    isStitched = stitched,
                )
                openProjectCloud(p)
            }
        }
        viewModelScope.launch {
            settings.settings.collect { s ->
                _uiState.value = _uiState.value.copy(
                    measureUnit = if (s.units == Units.FEET) MeasureUnit.FEET else MeasureUnit.METERS,
                )
            }
        }
        viewModelScope.launch {
            while (true) {
                val n = processing.totalPoints()
                val s = _uiState.value
                // ROUND 8: a re-resolve publishes pages as it goes, so the
                // first page arriving IS "ready" — the room draws while it is
                // still being built, which is both truthful and much better to
                // watch than a spinner. 250 ms rather than the old 1 s for the
                // same reason.
                val load = if (n > 0 && (s.load == ReviewLoad.RESOLVING ||
                        s.load == ReviewLoad.LOADING_RECORDED || s.load == ReviewLoad.PROBING)
                ) {
                    ReviewLoad.READY
                } else {
                    s.load
                }
                _uiState.value = s.copy(
                    totalPoints = n,
                    hasCloud = n > 0,
                    load = load,
                    loadMessage = if (load == ReviewLoad.READY) null else s.loadMessage,
                    colorModeReasons = colorModeAvailability(gnssActive = false),
                )
                delay(250)
            }
        }
    }

    // --- ROUND 8: opening a saved scan shows the 3D map (owner item 27c) -----
    //
    // THE FIELD REPORT this replaces, verbatim: *"When i check the recording,
    // it still show a 2D scan. i need a 3d mapping."*
    //
    // What Review used to do was draw `ProcessingCloudSource` — the processing
    // engine's PageStore — and nothing whatsoever put a D6 project into it.
    // The only thing that ever filled that store was a post-process job, and
    // post-processing REFUSED a D6 project (ROUND 7 §6 made the refusal honest
    // but could not make it work: the engine had no D6 offline pipeline and,
    // more fundamentally, a D6 `.lscan` did not contain the trajectory that
    // pipeline would need). So the viewer was empty and the only 3D-looking
    // thing anywhere near a saved scan was the Projects-tab thumbnail, which
    // was drawing a 50/50 mix of the resolved map and the RAW SENSOR-FRAME FAN
    // — a flat 2D disc. That is what the owner was looking at.
    //
    // Three paths now, cheapest first, and the screen says which one it is on:
    //
    //  1. **The cached cloud.** A 0.5.0+ capture writes its resolved cloud into
    //     the container as it goes (`ChunkType::kPointsXyzRgba`), so opening a
    //     scan is a file read, not a computation.
    //  2. **Re-resolve.** No cache but a trajectory is present: run the offline
    //     pipeline over the raw returns + poses + mount extrinsic. Seconds, and
    //     it produces exactly what the live pass produced (proved bit-for-bit
    //     in engine/tests/test_round8_d6_reopen.cpp).
    //  3. **Say so.** A D6 capture with no trajectory cannot be made 3D by
    //     anything, ever — the third dimension was never recorded. Rather than
    //     show an empty box or fall back to the raw fan and let it be mistaken
    //     for the result, the viewport explains it in one paragraph.
    private fun openProjectCloud(p: Project) {
        viewModelScope.launch {
            val probe = withContext(Dispatchers.IO) { processing.probeProject(p.directory) }
            _uiState.value = _uiState.value.copy(probe = probe)

            if (!probe.opened) {
                setLoad(
                    ReviewLoad.FAILED,
                    "This project's data files could not be read. The .lscan directory is " +
                        "missing or unreadable:\n\n${p.directory.absolutePath}",
                )
                return@launch
            }

            if (probe.hasRecordedMap) {
                setLoad(ReviewLoad.LOADING_RECORDED, null)
                val n = withContext(Dispatchers.IO) {
                    processing.openRecordedCloud(projectId, p.directory)
                }
                if (n > 0) {
                    setLoad(ReviewLoad.READY, null)
                    return@launch
                }
                // The cache is a cache: an empty or unreadable one is not a
                // failure, it just means paying for the re-resolve below.
            }

            if (probe.predatesTrajectoryStorage) {
                setLoad(
                    ReviewLoad.NO_TRAJECTORY,
                    "Recorded before trajectory storage — showing raw sensor view.\n\n" +
                        "A COIN-D6 is a 2D lidar: the third dimension of a scan is entirely the " +
                        "phone's motion while you walk. This capture was made by LidarScan " +
                        "${p.manifest.appVersion.ifBlank { "before 0.5.0" }}, which recorded the " +
                        "returns but not the trajectory, so there is no way to rebuild the 3D " +
                        "map from it — not by this app and not by any later one.\n\n" +
                        "Scans taken from 0.5.0 on store the trajectory alongside the returns " +
                        "and open straight into 3D.",
                )
                return@launch
            }

            if (!probe.isD6) {
                // A Mid-360 project: A7's pipeline is the answer and it has
                // always been reachable from the Processing screen. Nothing to
                // auto-run here — a full LIO re-run is minutes of work and a
                // deliberate action, not something a screen starts by itself.
                setLoad(
                    ReviewLoad.FAILED,
                    "No cloud in memory. Run Process on this project — the Mid-360 pipeline " +
                        "re-runs the odometry from the raw returns, which takes a few minutes.",
                )
                return@launch
            }

            setLoad(ReviewLoad.RESOLVING, null)
            // The operator's persisted mount re-zero beats the container's own,
            // for the reason ProcessingRepository.submitPostProcess documents.
            val mount = withContext(Dispatchers.IO) { resolveMountMatrix(p) }
            val submitted = processing.submitPostProcess(projectId, p.directory, mount)
            if (submitted.isFailure) {
                setLoad(
                    ReviewLoad.FAILED,
                    "This scan could not be rebuilt: ${processing.lastError()}",
                )
            }
            // Success is observed by the points poll below, which flips the
            // state to READY the moment the job's first page lands — so the
            // cloud appears as it is built rather than after it is finished.
        }
    }

    /**
     * `phone_from_lidar` for this project, as the offline resolve should use it.
     * Null hands the decision to the engine, which reads the extrinsic the
     * capture itself recorded.
     */
    private fun resolveMountMatrix(p: Project): DoubleArray? {
        val sensor = p.manifest.sensor
        val measured = p.manifest.mountCalibration
            ?.let { com.lidarscan.core.calib.Mat4(it.cameraFromLidar.copyOf()) }
            ?.takeIf { it.isRigid(1e-4) }
        if (measured != null) return measured.m
        val trim = p.manifest.mountTrim?.takeIf { it.sensor == sensor } ?: return null
        return trim.composedWith(com.lidarscan.core.calib.BracketNominals.cadNominal(sensor)).m
    }

    private fun setLoad(load: ReviewLoad, message: String?) {
        _uiState.value = _uiState.value.copy(load = load, loadMessage = message)
    }

    fun onRendererReady(r: PointCloudRenderer) {
        renderer = r
    }

    fun updateDisplay(transform: (DisplayParams) -> DisplayParams) {
        val next = transform(_uiState.value.display).clamped()
        _uiState.value = _uiState.value.copy(display = next)
        saveJob?.cancel()
        saveJob = viewModelScope.launch {
            delay(400)
            withContext(Dispatchers.IO) {
                store.updateManifest(projectId) { it.copy(displayParams = next) }
            }
        }
    }

    fun applyProfile(profile: DisplayProfile) = updateDisplay { profileDefaults(profile) }

    fun resetToProfileDefault() {
        val p = _uiState.value.project ?: return
        applyProfile(p.manifest.effectiveCaptureDefaults().displayProfile)
    }

    // --- B11: measure --------------------------------------------------------

    fun toggleMeasure() {
        val on = !_uiState.value.measureMode
        _uiState.value = _uiState.value.copy(
            measureMode = on,
            firstPick = null,
            measurement = null,
            pickMessage = if (on) "Tap a point to start." else null,
        )
        if (on) {
            viewModelScope.launch(Dispatchers.Default) {
                pickCandidates = cloudSource.samplePoints(MAX_PICK_CANDIDATES)
                if (pickCandidates.isEmpty()) {
                    _uiState.value = _uiState.value.copy(pickMessage = "Nothing to measure — this project has no processed cloud yet.")
                }
            }
        } else {
            pickCandidates = emptyList()
        }
    }

    fun clearMeasurement() {
        _uiState.value = _uiState.value.copy(firstPick = null, measurement = null, pickMessage = "Tap a point to start.")
    }

    fun setMeasureUnit(unit: MeasureUnit) {
        _uiState.value = _uiState.value.copy(measureUnit = unit)
        viewModelScope.launch {
            settings.setUnits(if (unit == MeasureUnit.FEET) Units.FEET else Units.METERS)
        }
    }

    fun onTap(x: Float, y: Float) {
        if (!_uiState.value.measureMode) return
        val r = renderer ?: return
        val vp = r.viewProjectionRowMajor()
        if (vp == null) {
            _uiState.value = _uiState.value.copy(pickMessage = "The 3D view is not ready yet.")
            return
        }
        viewModelScope.launch(Dispatchers.Default) {
            val hit = pickNearestPoint(
                points = pickCandidates,
                viewProjection = vp,
                viewportW = r.viewportWidthPx(),
                viewportH = r.viewportHeightPx(),
                tapX = x,
                tapY = y,
                radiusPx = PICK_RADIUS_PX,
            )
            if (hit == null) {
                _uiState.value = _uiState.value.copy(
                    pickMessage = "No point within ${PICK_RADIUS_PX.toInt()} px of that tap. Zoom in and try again.",
                )
                return@launch
            }
            val first = _uiState.value.firstPick
            if (first == null) {
                _uiState.value = _uiState.value.copy(
                    firstPick = hit.point,
                    measurement = null,
                    pickMessage = "First point set. Tap the second.",
                )
            } else {
                val m = Measurement(first, hit.point)
                _uiState.value = _uiState.value.copy(
                    firstPick = null,
                    measurement = m,
                    pickMessage = null,
                )
            }
        }
    }

    fun formatted(metres: Double): String = formatDistance(metres, _uiState.value.measureUnit)

    private companion object {
        /**
         * The pick sample size. A tap does not need every point: at 200k
         * candidates spread over the whole cloud the nearest sampled return is
         * within a few centimetres of the drawn one at typical zoom, and the
         * readout says "nearest sampled point" rather than implying otherwise.
         */
        const val MAX_PICK_CANDIDATES = 200_000
        const val PICK_RADIUS_PX = 56f
    }
}
