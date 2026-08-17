package com.lidarscan.app.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.data.AppSettings
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.data.ThemeMode
import com.lidarscan.app.data.Units
import com.lidarscan.app.debug.REPLAY_PROJECT_NAME
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class SettingsViewModel(
    private val settingsRepository: SettingsRepository,
    private val projectStore: ProjectStore,
    val storageLocation: String,
    /**
     * ROUND 6 (owner item 20): the persistent on-device capture log. Surfaced
     * here — path, size, live tail, export, clear — because the previous field
     * failure arrived with no evidence at all and there was nowhere on the
     * device to go and get any.
     */
    private val captureLog: com.lidarscan.app.debug.CaptureLog? = null,
    /** Where an exported copy is staged before the share sheet; the app's own cache. */
    private val shareCacheDir: java.io.File? = null,
    private val shareFile: ((java.io.File) -> Unit)? = null,
    /**
     * ROUND 7: pushes a changed sensor latency straight at the open D6
     * connection, so it applies to the next chunk rather than the next connect.
     */
    private val onSensorLatencyApplied: ((Int) -> Unit)? = null,
    /** ROUND 7: the application context the Downloads copy needs; null in a bare-JVM test. */
    private val downloadsContext: android.content.Context? = null,
) : ViewModel() {

    val captureLogPath: String get() = captureLog?.path ?: "(capture log unavailable)"

    val captureLogLastLine: StateFlow<String?> =
        captureLog?.lastLine ?: kotlinx.coroutines.flow.MutableStateFlow(null)

    fun captureLogSizeBytes(): Long = captureLog?.sizeBytes() ?: 0L

    /**
     * ROUND 7: the export-log button now ends in a visible outcome.
     *
     * Same rule as the project export (see
     * [com.lidarscan.app.share.DownloadsExporter]): a user-triggered file
     * operation ends in a success naming a path the operator can open, or a
     * stated failure. This used to `?: return` on a staging failure and hand off
     * to a share sheet that reports nothing — two silent exits on the one button
     * whose entire purpose is producing evidence.
     */
    private val _exportNote = MutableStateFlow<String?>(null)
    val exportNote: StateFlow<String?> = _exportNote.asStateFlow()

    fun dismissExportNote() {
        _exportNote.value = null
    }

    fun shareCaptureLog() {
        val log = captureLog ?: return
        val cache = shareCacheDir ?: return
        val share = shareFile
        viewModelScope.launch(Dispatchers.IO) {
            val file = log.exportTo(cache)
            if (file == null) {
                _exportNote.value = "Could not stage the capture log for export. It is still on the phone at " +
                    captureLogPath + "."
                return@launch
            }
            val appContext = downloadsContext
            val copied = appContext?.let {
                com.lidarscan.app.share.DownloadsExporter.copyToDownloads(it, file, fileName = file.name)
            }
            _exportNote.value = copied?.fold(
                onSuccess = { where -> "Capture log saved to $where." },
                onFailure = { e ->
                    "Could not save the capture log to Downloads (${e.javaClass.simpleName}). " +
                        "Use the share sheet, or read it at $captureLogPath."
                },
            ) ?: "Capture log staged at ${file.absolutePath}."
            captureLog.log(
                com.lidarscan.app.debug.CaptureLog.TAG_EXPORT,
                "capture log export -> ${copied?.getOrNull() ?: "share sheet only"}",
            )
            withContext(Dispatchers.Main) { share?.invoke(file) }
        }
    }

    fun clearCaptureLog() {
        captureLog?.clear()
    }

    val settings: StateFlow<AppSettings> = settingsRepository.settings.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = AppSettings(),
    )

    fun setUnits(units: Units) {
        viewModelScope.launch { settingsRepository.setUnits(units) }
    }

    fun setThemeMode(themeMode: ThemeMode) {
        viewModelScope.launch { settingsRepository.setThemeMode(themeMode) }
    }

    fun setUseFakeEngine(useFakeEngine: Boolean) {
        viewModelScope.launch { settingsRepository.setUseFakeEngine(useFakeEngine) }
    }

    /** D3: the Cloud processing mode's server URL and single-tenant token (§3.8). */
    fun setCloud(baseUrl: String, token: String) {
        viewModelScope.launch { settingsRepository.setCloud(baseUrl, token) }
    }

    /** B6: the operator override behind A11's `SCAN_SYNC_POOR` refusal. */
    /** ROUND 7 (time-sync): see [com.lidarscan.core.capture.D6TimeSync]. */
    fun setD6SensorLatencyMs(millis: Int) {
        viewModelScope.launch { settingsRepository.setD6SensorLatencyMs(millis) }
        onSensorLatencyApplied?.invoke(com.lidarscan.core.capture.D6TimeSync.clampLatencyMs(millis))
    }

    fun setAllowPoorSyncColorize(allow: Boolean) {
        viewModelScope.launch { settingsRepository.setAllowPoorSyncColorize(allow) }
    }

    /**
     * B4's debug-drawer acceptance path. Reuses an existing "Synthetic
     * Replay Demo" project if the user has already run this before (so
     * repeated taps don't pile up duplicate `.lscan` directories under
     * Projects), otherwise creates one — a real project, visible in the
     * Projects list like any other, which is deliberate: it makes the
     * replay path fully inspectable rather than a hidden side channel.
     */
    fun replaySyntheticCapture(onReady: (projectId: String) -> Unit) {
        viewModelScope.launch(Dispatchers.IO) {
            val existing = projectStore.list().firstOrNull { it.manifest.name == REPLAY_PROJECT_NAME }
            val project = existing
                ?: projectStore.create(REPLAY_PROJECT_NAME, SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
            withContext(Dispatchers.Main) { onReady(project.id) }
        }
    }
}
