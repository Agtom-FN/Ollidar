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
    /**
     * ROUND 28 item 161 — the ≤6-word headline over [loadMessage].
     *
     * §C.6's empty state is `icon → Title (≤6 words) → Body → one Primary`, and
     * until this round the viewport had only the Body: round 8's honest
     * paragraphs (up to ~110 words for the pre-0.5.0 no-trajectory case) with
     * nothing above them. A paragraph with no headline is a paragraph an
     * operator does not read, so the *reason* the screen is empty — which is
     * the whole point of those three states existing separately — arrived only
     * for someone who read to the end.
     *
     * The headline lives beside the message rather than being derived in the
     * composable because the two have to agree, and the state that knows which
     * of the three load paths failed is here.
     */
    val loadHeadline: String? = null,
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
    /**
     * ROUND 27 item 134 — what a running job is DOING, in its own words
     * ("Running — odometry"), or null. `processProgress` alone is a bar that
     * sits at 0 % for the first thirty seconds of a Mid-360 resolve.
     */
    val processStage: String? = null,
) {
    /**
     * ROUND 27 item 134(a) — **is Process the answer to what is on screen?**
     *
     * The Review screen has told the operator to "Run Process on this project"
     * since round 8 and has never drawn a Process control in that state: the
     * round-13 card is gated on `sections > 1`, which is a DIFFERENT question
     * ("is the map in pieces?") that happens to be false in exactly the state
     * the paragraph is about. The instruction and the affordance disagreed, and
     * the only way to act on the instruction was a ⋯ menu on another screen.
     *
     * So the gate is the honest one: there is nothing to draw, the container is
     * not still being read, and it is not the one case that genuinely cannot be
     * fixed (a pre-0.5.0 capture with no trajectory — no pipeline can invent
     * one, and offering a button that must fail is worse than offering none).
     */
    val canProcess: Boolean
        get() = !hasCloud &&
            !processing &&
            load != ReviewLoad.PROBING &&
            load != ReviewLoad.LOADING_RECORDED &&
            load != ReviewLoad.RESOLVING &&
            load != ReviewLoad.NO_TRAJECTORY

    /**
     * ROUND 28 item 161 — **"Process again", and the honest answer when it
     * would do nothing.**
     *
     * §D.6 moves the Jobs screen's `Post-process` card into Review's `⋯` menu
     * as one row, and moves its **62-word red paragraph** into that row's
     * disabled state. The paragraph was red, and red in this app means *an
     * operation failed and the operator lost something* (item 163) — nothing
     * had failed. It was explaining that a phone-tracked pushbroom's cloud is
     * assembled while you walk, so there is no offline pipeline left to run.
     * That is "not applicable", which is [ReviewProcessWording.ALREADY_FINAL]
     * in ink-mute, five words, per §C.6.
     *
     * The gate is not "is this a D6": it is **is there anything a re-run could
     * change**. Three things can make the answer yes, and each is a state the
     * operator can see on the same screen:
     *
     *  * there is nothing drawn (this is [canProcess]'s case, unchanged);
     *  * the capture is in more than one piece, so the stitch has work;
     *  * `processed/map_stitched.bin` is absent, so the round-15 auto-process
     *    never ran or never finished.
     *
     * A sealed single-section scan that already carries its processed file is
     * the normal case since round 15, and for it a re-run reproduces the same
     * bytes — so the row is disabled and says why.
     */
    val canProcessAgain: Boolean
        get() = !processing &&
            load != ReviewLoad.PROBING &&
            load != ReviewLoad.LOADING_RECORDED &&
            load != ReviewLoad.RESOLVING &&
            load != ReviewLoad.NO_TRAJECTORY &&
            (!hasCloud || sections > 1 || !isStitched)
}

/**
 * ROUND 28 item 161 — Review's own strings for the `⋯` menu's Process row.
 *
 * Declared beside the gate that decides when they are shown, so a change to one
 * cannot silently outlive the other. They belong in `:core`'s `Wording` (where
 * `WordingLaw` would count their words for us) and should move there next time
 * that file is open — this round it is owned by another agent.
 */
/**
 * ROUND 28 item 163 — the two strings, **now owned by `:core`'s [Wording]** so
 * `WordingLaw` counts their words like every other operator-facing sentence.
 *
 * Kept as forwarders rather than deleted: the call sites read better as
 * `ReviewProcessWording.ALREADY_FINAL` at the point where the disabled row is
 * built, and the guard follows the constant rather than the alias.
 */
object ReviewProcessWording {
    /** ≤6 words. The row itself. */
    const val PROCESS_AGAIN = com.lidarscan.core.Wording.PROCESS_AGAIN

    /**
     * The disabled detail. **Ink-mute, never `bad`** — see [ReviewUiState.canProcessAgain].
     * Five words; §C.6 allows twelve.
     */
    const val ALREADY_FINAL = com.lidarscan.core.Wording.ALREADY_FINAL
}

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
            processStage = null,
            stitch = null,
        )
        processJob?.cancel()
        // ── ROUND 27 item 134: the RIGHT pipeline for this container ────────
        //
        // `reprocessD6` is the offline D6 resolve. Handing it a Mid-360
        // container is not a slow answer, it is the wrong one — and the state
        // this button now appears in ("No cloud in memory. Run Process on this
        // project — the Mid-360 pipeline re-runs the odometry…") is by
        // construction the non-D6 case, so a button that only knew the D6 path
        // would have been a button that could only fail.
        if (!_uiState.value.probe.isD6) {
            processJob = viewModelScope.launch { runPostProcess(p) }
            return
        }
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
                    // ROUND 27 item 134(b): the same sentence plus the engine's
                    // own reason. "May be incomplete" is a guess printed where
                    // a fact was available.
                    processStage = null,
                    processError = com.lidarscan.core.Wording.processFailed(
                        processing.lastError()
                            .ifBlank { "the scan's recorded data may be incomplete" },
                    ) + " The raw files are untouched either way.",
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

    /**
     * ROUND 27 item 134(b) — **the Mid-360 resolve, with its progress and its
     * reason both on screen.**
     *
     * The failure this replaces was silent in the strictest sense: the ⋯ menu's
     * "Process again" ran, failed, cleared its chip and returned the operator
     * to the same "No cloud in memory" paragraph, with the real error sitting
     * in the native engine's `lastError()` where nothing read it. An operation
     * that can fail must say that it failed and why — that is the same rule as
     * round 23's "a button that will not act must still ANSWER", one screen
     * over.
     *
     * The job is watched rather than awaited: `submitPostProcess` enqueues on
     * the native queue and returns an id, and the queue is polled by
     * `ProcessingRepository` already. Watching it is what gives the operator a
     * real fraction and a real stage name instead of a spinner.
     */
    private suspend fun runPostProcess(p: Project) {
        val mount = withContext(Dispatchers.IO) { resolveMountMatrix(p) }
        val submitted = processing.submitPostProcess(p.id, p.directory, mount)
        val jobId = submitted.getOrNull()
        if (jobId == null) {
            _uiState.value = _uiState.value.copy(
                processing = false,
                processStage = null,
                // The reason, not a shrug. `lastError()` is the engine's own
                // sentence; the prefix says which half of the app is speaking.
                processError = com.lidarscan.core.Wording.processFailed(
                    submitted.exceptionOrNull()?.message?.ifBlank { null } ?: processing.lastError(),
                ),
            )
            return
        }
        while (kotlinx.coroutines.currentCoroutineContext().isActive) {
            val job = processing.jobsFor(p.id).firstOrNull { it.id == jobId }
            if (job != null) {
                setProgress(job.progress)
                _uiState.value = _uiState.value.copy(processStage = job.statusText)
                if (job.state.isTerminal) {
                    if (job.state == com.lidarscan.core.jobs.JobState.FAILED) {
                        _uiState.value = _uiState.value.copy(
                            processing = false,
                            processStage = null,
                            processError = com.lidarscan.core.Wording.processFailed(
                                job.message.ifBlank { processing.lastError() },
                            ),
                        )
                    } else {
                        _uiState.value = _uiState.value.copy(
                            processing = false,
                            processProgress = 1f,
                            processStage = null,
                        )
                        openProjectCloud(p)
                    }
                    return
                }
            }
            kotlinx.coroutines.delay(400)
        }
    }

    private fun setProgress(f: Float) {
        val s = _uiState.value
        // Monotone: a progress bar that goes backwards reads as a bug even when
        // the underlying number is honest.
        if (s.processing && f >= s.processProgress) {
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
        // ROUND 16 item 59: independent of which of the three cloud paths below
        // runs — the path belongs to the container, not to how its cloud was
        // obtained.
        loadTrajectory(p.directory)
        viewModelScope.launch {
            val probe = withContext(Dispatchers.IO) { processing.probeProject(p.directory) }
            _uiState.value = _uiState.value.copy(probe = probe)

            if (!probe.opened) {
                setLoad(
                    ReviewLoad.FAILED,
                    "This project's data files could not be read. The .lscan directory is " +
                        "missing or unreadable:\n\n${p.directory.absolutePath}",
                    // ROUND 28 item 161 — §C.6's ≤6-word headline. The paragraph
                    // below it is unchanged; it names the directory, which is the
                    // only thing that lets anyone act on this.
                    headline = "Could not open this scan",
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
                        "phone's motion while you walk. This capture was made by ${com.lidarscan.core.Wording.APP_NAME} " +
                        "${p.manifest.appVersion.ifBlank { "before 0.5.0" }}, which recorded the " +
                        "returns but not the trajectory, so there is no way to rebuild the 3D " +
                        "map from it — not by this app and not by any later one.\n\n" +
                        "Scans taken from 0.5.0 on store the trajectory alongside the returns " +
                        "and open straight into 3D.",
                    // Six words. Round 8's paragraph stays verbatim underneath —
                    // it is the one state nothing can fix, and the reason has to
                    // survive being summarised.
                    headline = "No 3D map in this scan",
                )
                return@launch
            }

            if (!probe.isD6) {
                // A Mid-360 project: A7's pipeline is the answer and it has
                // always been reachable from the Processing screen. Nothing to
                // auto-run here — a full LIO re-run is minutes of work and a
                // deliberate action, not something a screen starts by itself.
                // ── ROUND 28 item 163's rule, on this screen ────────────
                //
                // This was 25 words naming `Mid-360 pipeline`, `odometry` and
                // `raw returns` — the same class of engineering confession the
                // Jobs screen was rewritten to delete, surviving one screen
                // over because it was generated in a ViewModel rather than
                // declared in `Wording`. §C.6 caps an empty state's body at
                // twelve words and its headline at six, and the operator's
                // question here is not *which pipeline*, it is *what do I press
                // and how long does it take*. The Primary button beside it
                // already says `Process`.
                setLoad(
                    ReviewLoad.FAILED,
                    "Building the map takes a few minutes.",
                    headline = "Not processed yet",
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
                    headline = "Could not rebuild this scan",
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

    /**
     * ROUND 28 item 161: [headline] is the ≤6-word title §C.6's empty state puts
     * over [ReviewUiState.loadMessage]. `null` for the states that draw a
     * spinner instead of an empty state (there is no headline for "still
     * reading the container" — the stage name is the whole message).
     */
    private fun setLoad(load: ReviewLoad, message: String?, headline: String? = null) {
        _uiState.value = _uiState.value.copy(
            load = load,
            loadHeadline = headline,
            loadMessage = message,
        )
    }

    fun onRendererReady(r: PointCloudRenderer) {
        renderer = r
        pushTrajectory()
    }

    // ── ROUND 16 item 59: THE WALKED PATH, IN REVIEW ────────────────────────
    //
    // > *"i want to see the path of mine showing in the pointcloud too for me
    // >  to check if the scan is right"* — owner, on 0.9.0.
    //
    // Read from `processed/trajectory.bin`, which `reprocess_d6_container`
    // writes beside `processed/map_stitched.bin`. That pairing is the whole
    // reason this is a file read and not an ABI call: the trajectory in that
    // file is the CORRECTED one — section-stitched, and loop-end-closed when
    // the closer fired — written by the same pass that wrote the cloud beside
    // it. Drawing an uncorrected path over a corrected cloud would be a lie
    // that looked exactly like a diagnosis, and it is the one failure mode this
    // feature must not have, because the operator will use the disagreement
    // between path and room to judge the scan.
    //
    // A scan that has never been processed has no file and shows no path, and
    // says so rather than drawing a straight line between two points it
    // invented. Since ROUND 15 every sealed scan auto-processes, so the normal
    // case has one.
    private val _trajectory =
        MutableStateFlow(com.lidarscan.core.capture.TrajectoryRibbon.EMPTY)
    val trajectory: StateFlow<com.lidarscan.core.capture.TrajectoryRibbon.Ribbon> =
        _trajectory.asStateFlow()

    // ROUND 16 items 59 + 61: the toggle is `DisplayParams.showTrajectory`,
    // which has existed since the desktop viewer and has been persisted per
    // project all along under a switch whose own subtitle admitted "the overlay
    // itself is desktop-only so far". It is not desktop-only any more, and
    // adding a second Boolean beside it would have been exactly the kind of
    // duplicated surface item 61 is about. Default ON, from
    // `DisplayParams`' own default.

    /**
     * ROUND 17 item 65 — why there is no path, when there is no path.
     *
     * ROUND 16's comment above says this "says so rather than drawing a
     * straight line", and it did not: a missing `processed/trajectory.bin`
     * produced `EMPTY`, `setTrail(count = 0)` quietly removed the entity from
     * the scene, and the operator got a cloud with no path and no explanation —
     * which looks precisely like the bug the owner reported. EVERY container
     * processed by a pre-ROUND-16 engine is in that state, which is every scan
     * he already owns, so the silent case was not an edge case at all.
     */
    private val _trajectoryNote = MutableStateFlow<String?>(null)
    val trajectoryNote: StateFlow<String?> = _trajectoryNote.asStateFlow()

    private fun loadTrajectory(directory: java.io.File) {
        viewModelScope.launch {
            val file = java.io.File(directory, "processed/trajectory.bin")
            val existed = withContext(Dispatchers.IO) { runCatching { file.isFile }.getOrDefault(false) }
            val ribbon = withContext(Dispatchers.IO) {
                runCatching {
                    com.lidarscan.app.processing.TrajectoryFile.read(file)
                }.getOrNull() ?: com.lidarscan.core.capture.TrajectoryRibbon.EMPTY
            }
            _trajectory.value = ribbon
            _trajectoryNote.value = when {
                ribbon.count >= 2 -> null
                !existed ->
                    "No walked path in this scan yet — tap Process to build one. (Scans taken " +
                        "before this version were saved without one.)"
                else ->
                    "This scan's walked path could not be read, so the cloud is shown without it."
            }
            pushTrajectory()
        }
    }

    private fun pushTrajectory() {
        val r = renderer ?: return
        val ribbon = _trajectory.value
        r.setTrailVisible(_uiState.value.display.showTrajectory)
        r.setTrail(ribbon.xyz, ribbon.rgba, ribbon.count)
    }

    fun updateDisplay(transform: (DisplayParams) -> DisplayParams) {
        // ── ROUND 28 item 153: the write path stamps ────────────────────────
        //
        // Round 27 item 141 migrates a persisted height grayscale to Turbo on
        // READ and stamps `DisplayParams.migration` so it happens once. Review
        // also WRITES display params — to the manifest AND to the per-device
        // block, two stores, both below — and this path never stamped.
        //
        // The consequence is the exact failure mode item 141 was written to
        // stop: an operator who deliberately chooses grayscale for a height
        // ramp in Review gets it saved unstamped, and the next read migrates
        // his choice away to Turbo. A migration that can run twice is not a
        // migration, it is a preference that keeps resetting.
        //
        // `stamp` is idempotent and returns the same instance when it has
        // nothing to do, so this costs an already-stamped write nothing.
        val next = com.lidarscan.core.render.DisplayMigrations.stamp(
            transform(_uiState.value.display).clamped(),
        )
        _uiState.value = _uiState.value.copy(display = next)
        // ROUND 16 item 59: the path toggle lives in DisplayParams, so this is
        // where it reaches the scene.
        renderer?.setTrailVisible(next.showTrajectory)
        saveJob?.cancel()
        saveJob = viewModelScope.launch {
            delay(400)
            withContext(Dispatchers.IO) {
                store.updateManifest(projectId) { it.copy(displayParams = next) }
                // ROUND 19 item 76: the DEVICE display block too — the same
                // store the live view loads its base from, which is what
                // finally carries this panel's toggles (the walked path, EDL,
                // the clip block) onto the next walk. The manifest write above
                // stays: it is the PROJECT's record of how it is displayed.
                settings.setDisplayParams(next)
            }
        }
    }

    /**
     * ROUND 28 item 161 — **edit the scalar block a colour mode actually
     * reads**, through [updateDisplay] and nowhere else.
     *
     * The display sheet had this `when (p.colorMode)` copy-pasted into four
     * call sites (colormap, gamma, brightness, auto-range), each with its own
     * `else -> p` fall-through. Four copies of one routing decision is four
     * chances for the ramp row to write the height block while the gamma
     * slider writes the intensity block, and there was no way to test it.
     *
     * Non-scalar modes return the params untouched: `DisplayParams.activeScalar`
     * hands RGB / fix-quality / coverage a neutral identity mapping that the
     * shader never reads, so there is nothing there to edit.
     */
    fun updateScalar(
        mode: com.lidarscan.core.render.ColorMode,
        transform: (com.lidarscan.core.render.ScalarColorParams) -> com.lidarscan.core.render.ScalarColorParams,
    ) = updateDisplay { p ->
        when (mode) {
            com.lidarscan.core.render.ColorMode.HEIGHT -> p.copy(height = transform(p.height))
            com.lidarscan.core.render.ColorMode.INTENSITY -> p.copy(intensity = transform(p.intensity))
            com.lidarscan.core.render.ColorMode.TIME -> p.copy(time = transform(p.time))
            else -> p
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
