package com.lidarscan.app.ui.capture

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.NativePointCloudProvider
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineEvent
import com.lidarscan.core.engine.EngineTarget
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class CaptureStats(
    val pointsCaptured: Long = 0,
    val elapsedMillis: Long = 0,
    val pointsPerSecond: Double = 0.0,
    val recordingSizeBytes: Long = 0,
)

sealed interface CaptureUiState {
    data object Loading : CaptureUiState
    data class Loaded(val project: Project) : CaptureUiState
    data object NotFound : CaptureUiState
}

/**
 * B4: drives the full Capture screen over [EngineBridge] — live 3D view
 * (via [pointCloudSource], only populated when the bridge also implements
 * [NativePointCloudProvider]), status strip, Live-SLAM toggle, and
 * pause/resume/stop with a session-summary sheet.
 *
 * [isReplay] selects the "Replay synthetic capture" debug acceptance path
 * (see `com.lidarscan.app.engine.ReplayEngineBridge`): the ViewModel
 * auto-connects on init (there is no USB wizard for a replay session) and
 * [isReplaySession] tells the screen to hide the Pause control (replay has
 * no pause/resume primitive — see `replay_engine.h`'s header comment).
 */
class CaptureViewModel(
    private val engineBridge: EngineBridge,
    private val projectStore: ProjectStore,
    private val projectId: String,
    val isReplay: Boolean = false,
    /**
     * B7/B8. Null for a replay session and for any build where ARCore is not
     * wanted — the whole AR path (overlay, pose push, keyframes) is then
     * simply absent rather than half-present.
     */
    private val arController: com.lidarscan.app.ar.CaptureArController? = null,
    private val engineHandleProvider: () -> Long = { 0L },
    private val mountCalibrationFor: (com.lidarscan.core.model.SensorType) -> com.lidarscan.core.calib.MountCalibration? = { null },
    /**
     * B9: A10's georeferencing solution for the engine handle, read at capture
     * stop and snapshotted into the manifest. Defaults to "no georeference",
     * which is what a replay session and a build with no RTK have.
     */
    private val georefSnapshotProvider: (Long) -> com.lidarscan.core.gnss.GeorefRecord? = { null },
) : ViewModel() {

    private val _uiState = MutableStateFlow<CaptureUiState>(CaptureUiState.Loading)
    val uiState: StateFlow<CaptureUiState> = _uiState.asStateFlow()

    val connectionState: StateFlow<ConnectionState> = engineBridge.connectionState
    val captureState: StateFlow<CaptureState> = engineBridge.captureState
    val deviceHealth: StateFlow<DeviceHealth?> = engineBridge.deviceHealth
    val isReplaySession: Boolean get() = isReplay

    private val _stats = MutableStateFlow(CaptureStats())
    val stats: StateFlow<CaptureStats> = _stats.asStateFlow()

    /** Set once a stop completes; the screen shows the session-summary sheet while non-null, clears it on dismiss. */
    private val _sessionSummary = MutableStateFlow<CaptureStats?>(null)
    val sessionSummary: StateFlow<CaptureStats?> = _sessionSummary.asStateFlow()

    private val _pointCloudSource = MutableStateFlow<PointCloudSource?>(null)
    val pointCloudSource: StateFlow<PointCloudSource?> = _pointCloudSource.asStateFlow()

    // --- B10-adjacent display controls (A14's contract, RGB/height/intensity
    // + point size + camera mode — the subset B4 needs; a full display-params
    // panel is B10's job). Kept in the ViewModel (not Compose `remember`) so
    // rotating the device (landscape<->portrait, explicitly in B4's scope)
    // doesn't reset them.
    private val _colorMode = MutableStateFlow(ColorMode.RGB)
    val colorMode: StateFlow<ColorMode> = _colorMode.asStateFlow()
    private val _colormap = MutableStateFlow(Colormap.SPECTRUM)
    val colormap: StateFlow<Colormap> = _colormap.asStateFlow()
    private val _pointSizePx = MutableStateFlow(2.5f)
    val pointSizePx: StateFlow<Float> = _pointSizePx.asStateFlow()
    private val _cameraMode = MutableStateFlow(CameraMode.ORBIT)
    val cameraMode: StateFlow<CameraMode> = _cameraMode.asStateFlow()
    private val _liveSlam = MutableStateFlow(false)
    val liveSlam: StateFlow<Boolean> = _liveSlam.asStateFlow()

    // --- redesign: the Capture-settings sheet's own state ---------------------
    //
    // All three live here rather than in Compose `remember` for the same reason
    // the display controls above do: the sheet can be dismissed and reopened,
    // and the device can rotate, without any of them snapping back.

    /**
     * §3.5's camera keyframes, on by default (the mockup's `S.cap.keyframes`).
     * Gates [com.lidarscan.app.ar.KeyframeRecorder] mid-session; the written
     * count freezes rather than resetting when this goes off.
     */
    private val _keyframesEnabled = MutableStateFlow(true)
    val keyframesEnabled: StateFlow<Boolean> = _keyframesEnabled.asStateFlow()

    /** §3.5's 2–5 fps cadence, as the sheet's 2 / 3 / 5 row. */
    private val _keyframeRateFps = MutableStateFlow(3)
    val keyframeRateFps: StateFlow<Int> = _keyframeRateFps.asStateFlow()

    /**
     * §3.12's LOD budget, in millions of points.
     *
     * The mockup labels this slider `2 – 20` and reads it out as a percentage.
     * What this renderer actually implements is `DisplayParams.lodPointBudget`
     * — a page-admission ceiling in points, not a per-page decimation — so the
     * control keeps the mockup's range and its "sparse → every return" caption
     * but reads out in **M points**, which is the number the renderer obeys.
     * A percentage would have been a nicer-looking lie.
     */
    private val _lodBudgetMPoints = MutableStateFlow(20)
    val lodBudgetMPoints: StateFlow<Int> = _lodBudgetMPoints.asStateFlow()

    /**
     * The whole A14 parameter block the live viewport renders with, assembled
     * from the four controls above. One object rather than four setters so the
     * LOD budget can live-apply the same way colour and point size already do
     * — `PointCloudRenderer.setDisplayParams` owns all of them together.
     */
    val displayParams: StateFlow<com.lidarscan.core.render.DisplayParams> =
        kotlinx.coroutines.flow.combine(
            _colorMode,
            _colormap,
            _pointSizePx,
            _lodBudgetMPoints,
        ) { mode, cm, size, lodM ->
            com.lidarscan.core.render.DisplayParams(
                colorMode = mode,
                height = com.lidarscan.core.render.ScalarColorParams(colormap = cm, manualMin = 0f, manualMax = 3f),
                intensity = com.lidarscan.core.render.ScalarColorParams(colormap = cm),
                pointSize = com.lidarscan.core.render.PointSizeParams(fixedPx = size),
                lodPointBudget = (lodM.coerceIn(1, 200) * 1_000_000),
                // The redesign's viewport ground, so the live view and the
                // project thumbnails sit on the same black.
                background = com.lidarscan.core.render.Rgba(11, 14, 18, 255),
            )
        }.stateIn(
            viewModelScope,
            kotlinx.coroutines.flow.SharingStarted.Companion.WhileSubscribed(5_000),
            com.lidarscan.core.render.DisplayParams(),
        )

    /** B5: this project's profile-driven capture defaults, read once at load. */
    private val _captureDefaults = MutableStateFlow<com.lidarscan.core.model.CaptureDefaults?>(null)
    val captureDefaults: StateFlow<com.lidarscan.core.model.CaptureDefaults?> = _captureDefaults.asStateFlow()

    // --- B3: Mid-360 -------------------------------------------------------
    /** This project's sensor; drives which connect wizard opens and whether Pause is offered. */
    val sensor: StateFlow<com.lidarscan.core.model.SensorType>
        get() = _sensor.asStateFlow()
    private val _sensor = MutableStateFlow(com.lidarscan.core.model.SensorType.COIN_D6)

    /** `"<lidarIp>|<hostIp>"` from the manifest, or null when the wizard has not been run for this project. */
    private val _mid360Endpoint = MutableStateFlow<String?>(null)
    val mid360Endpoint: StateFlow<String?> = _mid360Endpoint.asStateFlow()

    /**
     * Connects the engine to this project's saved Mid-360 endpoint.
     *
     * Deliberately explicit rather than automatic on screen entry: bringing a
     * Mid-360 up runs SDK2's discovery + handshake + host-IP configuration
     * push and takes a process-wide singleton, and doing that as a side effect
     * of opening a screen is how a connect-wizard probe and a capture session
     * end up fighting over it.
     */
    fun connectMid360() {
        val endpoint = _mid360Endpoint.value ?: return
        viewModelScope.launch {
            engineBridge.connect(
                com.lidarscan.core.engine.EngineTarget(
                    com.lidarscan.core.model.SensorType.MID360,
                    transportHint = endpoint,
                ),
            )
        }
    }

    // --- B7/B8 state ---------------------------------------------------------
    private val _keyframeStats = MutableStateFlow(com.lidarscan.app.ar.KeyframeRecorder.Stats())
    val keyframeStats: StateFlow<com.lidarscan.app.ar.KeyframeRecorder.Stats> = _keyframeStats.asStateFlow()

    private val _pushbroomStats = MutableStateFlow<com.lidarscan.app.engine.NativePushbroomStats?>(null)
    val pushbroomStats: StateFlow<com.lidarscan.app.engine.NativePushbroomStats?> = _pushbroomStats.asStateFlow()

    private val _mountCalibrationApplied = MutableStateFlow<com.lidarscan.core.calib.MountCalibration?>(null)
    val mountCalibrationApplied: StateFlow<com.lidarscan.core.calib.MountCalibration?> =
        _mountCalibrationApplied.asStateFlow()

    /** True when this session can offer the §3.7 AR overlay at all. */
    val arAvailable: Boolean get() = arController != null && !isReplay

    val arStatus: StateFlow<com.lidarscan.app.ar.CaptureArController.ArStatus>? = arController?.status

    private var keyframeRecorder: com.lidarscan.app.ar.KeyframeRecorder? = null

    /** Held so the exact same function reference can be removed again — a method reference is a NEW object each time it is written. */
    private var keyframeFrameListener: ((com.google.ar.core.Frame) -> Unit)? = null

    private fun detachKeyframeListener() {
        val listener = keyframeFrameListener ?: return
        arController?.removeFrameListener(listener)
        keyframeFrameListener = null
    }

    private var lastStatsSampleMillis = 0L
    private var lastStatsSamplePoints = 0L

    init {
        viewModelScope.launch(Dispatchers.IO) {
            val project = projectStore.open(projectId)
            _uiState.value = if (project != null) CaptureUiState.Loaded(project) else CaptureUiState.NotFound
            if (project != null) {
                _sensor.value = project.manifest.sensor
                // B5: the profile stops being a label here. Live-SLAM's initial
                // position and the engine's session `profile` string both come
                // from the project's own CaptureDefaults rather than from a
                // hardcoded default (see EngineBridge.startCapture's KDoc).
                val defaults = project.manifest.effectiveCaptureDefaults()
                _liveSlam.value = defaults.liveSlam
                _captureDefaults.value = defaults
                _mid360Endpoint.value = project.manifest.mid360
                    ?.let { "${'$'}{it.lidarIp}|${'$'}{it.hostIp}" }
            }
        }

        engineBridge.events
            .filterIsInstance<EngineEvent.CaptureStats>()
            .onEach(::onCaptureStats)
            .launchIn(viewModelScope)

        engineBridge.connectionState
            .onEach {
                _pointCloudSource.value = (engineBridge as? NativePointCloudProvider)?.currentPointCloudSource()
            }
            .launchIn(viewModelScope)

        if (isReplay) {
            // No USB wizard for a replay session — the bridge itself has
            // nothing to "find" (see ReplayEngineBridge.connect, which just
            // creates the native replay engine handle). SensorType is
            // otherwise unused by ReplayEngineBridge.connect.
            viewModelScope.launch { engineBridge.connect(EngineTarget(SensorType.COIN_D6)) }
        }
    }

    private fun onCaptureStats(event: EngineEvent.CaptureStats) {
        val dtMillis = (event.elapsedMillis - lastStatsSampleMillis).coerceAtLeast(1L)
        val dPoints = (event.pointsCaptured - lastStatsSamplePoints).coerceAtLeast(0L)
        val instantaneousRate = dPoints * 1000.0 / dtMillis
        lastStatsSampleMillis = event.elapsedMillis
        lastStatsSamplePoints = event.pointsCaptured

        _stats.value = _stats.value.copy(
            pointsCaptured = event.pointsCaptured,
            elapsedMillis = event.elapsedMillis,
            pointsPerSecond = instantaneousRate,
        )

        // Recording size: sum bytes under streams/ — a plain directory walk,
        // throttled to once per stats tick, not per-point. For a replay
        // session this stays 0 (nothing new is written to disk — see
        // ReplayEngineBridge's doc comment), which is correct, not a bug.
        val project = (_uiState.value as? CaptureUiState.Loaded)?.project ?: return
        if (isReplay) return
        viewModelScope.launch(Dispatchers.IO) {
            val size = directorySizeBytes(project.streamsDir)
            _stats.value = _stats.value.copy(recordingSizeBytes = size)
        }
    }

    fun startCapture() {
        val project = (_uiState.value as? CaptureUiState.Loaded)?.project ?: return
        lastStatsSampleMillis = 0L
        lastStatsSamplePoints = 0L
        _stats.value = CaptureStats()
        _sessionSummary.value = null
        viewModelScope.launch {
            val started = engineBridge.startCapture(
                project.directory.absolutePath,
                _liveSlam.value,
                com.lidarscan.core.model.CaptureDefaults.engineProfileString(project.manifest.profile),
            )
            if (started.isFailure) return@launch
            startArPipelines(project)
        }
    }

    /**
     * B7/B8, in the one order that works:
     *
     *  1. point the ARCore controller at the now-live engine handle, so poses
     *     land in this session rather than nowhere;
     *  2. apply the stored mount extrinsic and enable the pushbroom — the
     *     engine refuses `pushbroom_enable` with `SCAN_ERR_INVALID_STATE`
     *     until an extrinsic exists, which is the whole reason the wizard has
     *     to run before a D6 capture;
     *  3. start the keyframe recorder, which needs the project directory that
     *     `startCapture` just opened as an `.lscan`.
     */
    private fun startArPipelines(project: Project) {
        val controller = arController ?: return
        val handle = engineHandleProvider()
        controller.engineHandle = handle

        val manifestCalibration = project.manifest.mountCalibration
            ?: mountCalibrationFor(project.manifest.sensor)
        if (handle != 0L && manifestCalibration != null) {
            val matrix = com.lidarscan.core.calib.Mat4(manifestCalibration.cameraFromLidar.copyOf())
            if (matrix.isRigid(1e-4)) {
                val err = com.lidarscan.app.engine.ScanEngineNative
                    .nativeSetMountExtrinsics(handle, matrix.m)
                if (err == com.lidarscan.app.engine.ScanEngineNative.ErrorCode.OK) {
                    com.lidarscan.app.engine.ScanEngineNative.nativePushbroomEnable(handle, true)
                    _mountCalibrationApplied.value = manifestCalibration
                }
            }
        }

        keyframeRecorder = com.lidarscan.app.ar.KeyframeRecorder(
            motion = controller.motion,
            // The sheet's cadence, applied at construction so the very first
            // slot of the session already runs at the selected rate; changing
            // it later goes through setKeyframeRateFps on the live recorder.
            selector = com.lidarscan.core.capture.KeyframeSelector(
                targetFps = _keyframeRateFps.value.toDouble(),
            ),
        ).also { recorder ->
            recorder.setEnabled(_keyframesEnabled.value)
            // One reference, stored and registered — `recorder::onFrame`
            // written twice would create two distinct objects, and
            // detachKeyframeListener would then remove neither (its own KDoc
            // says exactly this; the code did it anyway).
            val listener: (com.google.ar.core.Frame) -> Unit = recorder::onFrame
            keyframeFrameListener = listener
            controller.addFrameListener(listener)
            recorder.start(project.directory)
            viewModelScope.launch {
                recorder.stats.collect { _keyframeStats.value = it }
            }
        }
    }

    fun pauseCapture() = viewModelScope.launch { engineBridge.pauseCapture() }
    fun resumeCapture() = viewModelScope.launch { engineBridge.resumeCapture() }

    fun stopCapture() = viewModelScope.launch {
        val finalStats = _stats.value
        // Keyframes first: the recorder's index has to be flushed and closed
        // while the .lscan is still the live session's, and the ARCore frame
        // listener must stop before the engine handle goes away.
        detachKeyframeListener()
        keyframeRecorder?.stop()
        keyframeRecorder = null
        arController?.engineHandle = 0L

        val handle = engineHandleProvider()
        if (handle != 0L && _mountCalibrationApplied.value != null) {
            // Resolve every pending pushbroom point the poses allow before the
            // session closes. scan_engine_stop() does this too; calling it
            // explicitly means the stats below reflect the flushed totals.
            com.lidarscan.app.engine.ScanEngineNative.nativePushbroomFlush(handle)
            _pushbroomStats.value = com.lidarscan.app.engine.ScanEngineNative.nativePushbroomStats(handle)
        }

        engineBridge.stopCapture()
        _sessionSummary.value = finalStats

        // B5/B9: write what the capture actually produced back into the
        // manifest.
        //
        // `pointCountEstimate` has been flagged as unwired by B2, B4, B7 and B3
        // in turn — `ProjectStore.updateManifest` has existed since B7 and this
        // is the two lines those notes kept asking for. The georef snapshot is
        // A10 §9.6's request ("a periodic GeorefSolution + origin snapshot in
        // the manifest so a replay does not have to re-derive the alignment"),
        // and it is what makes B12's auto-merge possible at all: `merge/session.h`
        // needs each session's transform AND the ENU frame it is expressed in,
        // and neither survives the end of a capture any other way.
        val georef = georefSnapshotProvider(handle)

        // Redesign: the Projects card thumbnail draws this project's OWN cloud,
        // and this is where that becomes possible — the pages are still
        // resident here, one strided sample later they are a 48 KB file next to
        // the processed results. Doing it anywhere else would mean re-decoding
        // the raw streams just to draw a 108 dp tile. Best-effort by design: a
        // failed write costs a thumbnail, never a capture.
        val previewSource = _pointCloudSource.value
        val projectDir = (_uiState.value as? CaptureUiState.Loaded)?.project?.directory
        withContext(Dispatchers.IO) {
            if (projectDir != null && com.lidarscan.app.ui.projects.writeProjectPreview(projectDir, previewSource)) {
                com.lidarscan.app.ui.projects.ProjectPreviewCache.invalidate(projectId)
            }
        }

        withContext(Dispatchers.IO) {
            projectStore.updateManifest(projectId) { manifest ->
                manifest.copy(
                    pointCountEstimate = finalStats.pointsCaptured.takeIf { it > 0 } ?: manifest.pointCountEstimate,
                    georef = georef ?: manifest.georef,
                    crsEpsg = georef?.epsg?.takeIf { it != 0 } ?: manifest.crsEpsg,
                )
            }
        }
        // The in-memory project must not go stale, or a later start would
        // re-read the old manifest.
        projectStore.open(projectId)?.let { _uiState.value = CaptureUiState.Loaded(it) }
    }

    fun dismissSessionSummary() {
        _sessionSummary.value = null
    }

    fun setLiveSlam(enabled: Boolean) {
        _liveSlam.value = enabled
    }

    fun setColorMode(mode: ColorMode) {
        _colorMode.value = mode
    }

    fun setColormap(cm: Colormap) {
        _colormap.value = cm
    }

    fun setPointSizePx(px: Float) {
        _pointSizePx.value = px
    }

    fun setCameraMode(mode: CameraMode) {
        _cameraMode.value = mode
    }

    /** Sheet: camera keyframes on/off. Applies to a live recorder immediately. */
    fun setKeyframesEnabled(enabled: Boolean) {
        _keyframesEnabled.value = enabled
        keyframeRecorder?.setEnabled(enabled)
    }

    /** Sheet: 2 / 3 / 5 fps. Applies to a live recorder immediately. */
    fun setKeyframeRateFps(fps: Int) {
        _keyframeRateFps.value = fps
        keyframeRecorder?.setTargetFps(fps.toDouble())
    }

    /** Sheet: §3.12's LOD budget, in millions of points. */
    fun setLodBudgetMPoints(mPoints: Int) {
        _lodBudgetMPoints.value = mPoints.coerceIn(1, 200)
    }

    override fun onCleared() {
        detachKeyframeListener()
        keyframeRecorder?.shutdown()
        keyframeRecorder = null
        arController?.engineHandle = 0L
        super.onCleared()
    }

    private fun directorySizeBytes(dir: java.io.File): Long {
        if (!dir.exists()) return 0L
        return dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
    }
}
