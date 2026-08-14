package com.lidarscan.app.ui.capture

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineEvent
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
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
 * B2: drives the real capture session over [EngineBridge] (Fake or the
 * JNI-backed [com.lidarscan.app.engine.RealEngineBridge], selected by
 * [com.lidarscan.app.di.AppContainer]). Rendering the live point cloud is
 * B4's job ("Arrives with B4" in the Tech Spec's B-workstream table) — this
 * screen shows the numeric stats the C ABI/engine already gives us: pts/s,
 * points total, recording size on disk.
 */
class CaptureViewModel(
    private val engineBridge: EngineBridge,
    private val projectStore: ProjectStore,
    private val projectId: String,
) : ViewModel() {

    private val _uiState = MutableStateFlow<CaptureUiState>(CaptureUiState.Loading)
    val uiState: StateFlow<CaptureUiState> = _uiState.asStateFlow()

    val connectionState: StateFlow<ConnectionState> = engineBridge.connectionState
    val captureState: StateFlow<CaptureState> = engineBridge.captureState
    val deviceHealth: StateFlow<DeviceHealth?> = engineBridge.deviceHealth

    private val _stats = MutableStateFlow(CaptureStats())
    val stats: StateFlow<CaptureStats> = _stats.asStateFlow()

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
        // throttled to once per stats tick (~500ms-1s from Fake/Real alike),
        // not per-point. Cheap enough at that cadence for a capture-in-progress
        // project (a handful of growing files).
        val project = (_uiState.value as? CaptureUiState.Loaded)?.project ?: return
        viewModelScope.launch(Dispatchers.IO) {
            val size = directorySizeBytes(project.streamsDir)
            _stats.value = _stats.value.copy(recordingSizeBytes = size)
        }
    }

    fun startCapture(liveSlam: Boolean) {
        val project = (_uiState.value as? CaptureUiState.Loaded)?.project ?: return
        lastStatsSampleMillis = 0L
        lastStatsSamplePoints = 0L
        _stats.value = CaptureStats()
        viewModelScope.launch { engineBridge.startCapture(project.directory.absolutePath, liveSlam) }
    }

    fun pauseCapture() = viewModelScope.launch { engineBridge.pauseCapture() }
    fun resumeCapture() = viewModelScope.launch { engineBridge.resumeCapture() }
    fun stopCapture() = viewModelScope.launch { engineBridge.stopCapture() }

    private fun directorySizeBytes(dir: java.io.File): Long {
        if (!dir.exists()) return 0L
        return dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
    }
}
