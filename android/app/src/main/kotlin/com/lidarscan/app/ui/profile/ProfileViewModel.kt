package com.lidarscan.app.ui.profile

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.share.FeedbackSender
import com.lidarscan.core.feedback.DeviceFacts
import com.lidarscan.core.feedback.FeedbackConfig
import com.lidarscan.core.feedback.FeedbackResult
import com.lidarscan.core.feedback.FeedbackRoute
import com.lidarscan.core.feedback.FeedbackWording
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * ROUND 24 item 109 — **the Profile page's state.**
 *
 * Two jobs, and they are deliberately the same job: SEND LOGS is FEEDBACK with
 * an empty message. One code path, one progress bar, one honest last word — a
 * second implementation of "zip it and try to deliver it" is a second thing to
 * get wrong.
 *
 * The send runs on [jobScope] (`AppContainer.containerScope`) rather than
 * `viewModelScope`, for the reason ROUND 22 item 90 spent two rounds
 * establishing: a job the operator started must not die because they left the
 * screen. Leaving the Profile page mid-upload is a normal thing to do.
 */
class ProfileViewModel(
    private val settingsRepository: SettingsRepository,
    private val projectStore: ProjectStore,
    private val projectsRootDir: File,
    private val sender: FeedbackSender,
    private val jobScope: CoroutineScope,
    private val appVersion: String,
    private val versionCode: Int,
    private val deviceModel: String,
    private val androidVersion: String,
    private val engineAbi: Int,
) : ViewModel() {

    data class UiState(
        val facts: DeviceFacts = DeviceFacts(),
        /** Which way a send will go, so the page can say so BEFORE the tap. */
        val route: FeedbackRoute = FeedbackRoute.SHARE,
        /** 0..1 while a send runs, null otherwise. */
        val sending: Float? = null,
        /** The last word — [FeedbackWording.SENT] or [FeedbackWording.NOT_SENT]. */
        val result: String? = null,
    ) {
        val note: String get() = FeedbackWording.noteFor(route)
    }

    private val _uiState = MutableStateFlow(UiState())
    val uiState: StateFlow<UiState> = _uiState.asStateFlow()

    /** The typed message. Held here so rotation does not lose a paragraph. */
    private val _message = MutableStateFlow("")
    val message: StateFlow<String> = _message.asStateFlow()

    fun setMessage(text: String) {
        _message.value = text
    }

    fun dismissResult() {
        _uiState.value = _uiState.value.copy(result = null)
    }

    /**
     * Recount the device facts.
     *
     * The two expensive ones — the scan count and the bytes on disk — are a
     * directory walk, so this is called on entry rather than collected: a
     * storage figure that recomputes on every recomposition is a storage
     * figure that costs a frame each time the operator types a character into
     * the feedback box.
     */
    fun refresh() {
        viewModelScope.launch {
            val settings = settingsRepository.settings.first()
            val (count, bytes) = withContext(Dispatchers.IO) {
                val projects = runCatching { projectStore.list() }.getOrDefault(emptyList())
                projects.size to runCatching { sizeOf(projectsRootDir) }.getOrDefault(0L)
            }
            _uiState.value = _uiState.value.copy(
                facts = DeviceFacts(
                    appVersion = appVersion,
                    versionCode = versionCode,
                    deviceModel = deviceModel,
                    androidVersion = androidVersion,
                    engineAbi = engineAbi,
                    scanCount = count,
                    storageBytes = bytes,
                ),
                route = FeedbackConfig(settings.cloudBaseUrl, settings.cloudToken).route,
            )
        }
    }

    /**
     * SEND LOGS (empty [message]) and SEND FEEDBACK (a typed one) are the same
     * call. A second send while one is running is refused rather than queued —
     * two zips of the same log is not what a double tap means.
     */
    fun send(withMessage: Boolean) {
        if (_uiState.value.sending != null) return
        val text = if (withMessage) _message.value else ""
        _uiState.value = _uiState.value.copy(sending = 0f, result = null)
        jobScope.launch {
            val settings = settingsRepository.settings.first()
            val result: FeedbackResult = sender.send(
                config = FeedbackConfig(settings.cloudBaseUrl, settings.cloudToken),
                facts = _uiState.value.facts,
                message = text,
                onProgress = { fraction ->
                    _uiState.value = _uiState.value.copy(sending = fraction)
                },
            )
            _uiState.value = _uiState.value.copy(
                sending = null,
                result = FeedbackWording.resultFor(result),
                route = result.route,
            )
            // The message is spent on a successful send: leaving it in the box
            // is how the same paragraph gets sent three times.
            if (result.sent && withMessage) _message.value = ""
        }
    }

    /** Bytes under [dir], following the tree. Symlinks are not created by this app. */
    private fun sizeOf(dir: File): Long =
        dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
}
