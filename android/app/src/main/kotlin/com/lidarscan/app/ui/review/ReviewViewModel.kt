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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

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

    init {
        viewModelScope.launch {
            val p = withContext(Dispatchers.IO) { store.open(projectId) }
            _uiState.value = _uiState.value.copy(
                project = p,
                display = p?.manifest?.effectiveDisplayParams() ?: DisplayParams(),
            )
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
                _uiState.value = _uiState.value.copy(
                    totalPoints = n,
                    hasCloud = n > 0,
                    colorModeReasons = colorModeAvailability(gnssActive = false),
                )
                delay(1000)
            }
        }
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
