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
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class SettingsViewModel(
    private val settingsRepository: SettingsRepository,
    private val projectStore: ProjectStore,
    val storageLocation: String,
) : ViewModel() {

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
