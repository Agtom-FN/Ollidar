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
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch

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

    private var lastStatsSampleMillis = 0L
    private var lastStatsSamplePoints = 0L

    init {
        viewModelScope.launch(Dispatchers.IO) {
            val project = projectStore.open(projectId)
            _uiState.value = if (project != null) CaptureUiState.Loaded(project) else CaptureUiState.NotFound
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
        viewModelScope.launch { engineBridge.startCapture(project.directory.absolutePath, _liveSlam.value) }
    }

    fun pauseCapture() = viewModelScope.launch { engineBridge.pauseCapture() }
    fun resumeCapture() = viewModelScope.launch { engineBridge.resumeCapture() }

    fun stopCapture() = viewModelScope.launch {
        val finalStats = _stats.value
        engineBridge.stopCapture()
        _sessionSummary.value = finalStats
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

    private fun directorySizeBytes(dir: java.io.File): Long {
        if (!dir.exists()) return 0L
        return dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
    }
}
