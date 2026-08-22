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
        /**
         * ROUND 28 item 165 (F6) — the two numbers the Projects header used to
         * carry, absorbed into the "This phone" table when item 151 deleted
         * that header.
         *
         * They are NOT on [DeviceFacts]: that class is the contract for what
         * leaves the phone in a bundle, it is pinned by `:core` tests, and
         * "how many of my scans have a CRS" is a fact about the library rather
         * than about the device. Adding it there would put a new field in every
         * feedback zip to satisfy a row on one screen.
         */
        val totalPoints: Long = 0L,
        val georeferencedCount: Int = 0,
    ) {
        /**
         * ROUND 29 item 173 — the Send-diagnostics row's second line.
         *
         * It used to predict the route, which was honest while the app chose
         * one. The operator chooses now, so the line says so; [route] is still
         * carried because it decides whether the **Your server** door is in the
         * chooser at all.
         */
        val note: String get() = FeedbackWording.CHOOSE_NOTE

        /** True when the cloud fields are filled in, i.e. a server door exists. */
        val serverConfigured: Boolean get() = route == FeedbackRoute.SERVER
    }

    private val _uiState = MutableStateFlow(UiState())
    val uiState: StateFlow<UiState> = _uiState.asStateFlow()

    /**
     * ROUND 29 item 173 — **a URL the screen must open, once.**
     *
     * The ViewModel builds it (the whole of `GitHubIssue` is `:core` and
     * testable) and the route launches it, because an `Intent` needs a Context
     * and a ViewModel holding an Activity is a leak with a nice name. It is
     * consumed rather than observed: a rotation that re-opened the browser
     * would be the classic version of this bug.
     */
    private val _pendingUrl = MutableStateFlow<String?>(null)
    val pendingUrl: StateFlow<String?> = _pendingUrl.asStateFlow()

    fun consumeUrl() {
        _pendingUrl.value = null
    }

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
            val walk = withContext(Dispatchers.IO) {
                val projects = runCatching { projectStore.list() }.getOrDefault(emptyList())
                LibraryWalk(
                    count = projects.size,
                    bytes = runCatching { sizeOf(projectsRootDir) }.getOrDefault(0L),
                    points = projects.sumOf { it.manifest.pointCountEstimate ?: 0L },
                    // The same test the Projects list uses for the EPSG badge:
                    // a manifest carrying 0 is a manifest that was written
                    // before the field meant anything, not a scan in EPSG 0.
                    georeferenced = projects.count { p ->
                        p.manifest.crsEpsg?.takeIf { it != 0 } != null
                    },
                )
            }
            val count = walk.count
            val bytes = walk.bytes
            _uiState.value = _uiState.value.copy(
                totalPoints = walk.points,
                georeferencedCount = walk.georeferenced,
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
     * ROUND 29 item 173(a) — **SEND FEEDBACK opens a GitHub issue.**
     *
     * Nothing is packed and nothing is uploaded: the operator's words and the
     * six device facts he can already read on the page go into a prefilled
     * `issues/new` URL, and his own browser session posts it under his own
     * account. The app has never held a GitHub credential and this is the
     * design that keeps it that way — see [GitHubIssue].
     *
     * The message is spent immediately rather than on success, because there is
     * no success to wait for: the app's part is finished the moment the browser
     * opens, and a box that still holds the paragraph after that is a box that
     * files it twice.
     */
    fun openFeedbackIssue() {
        val text = _message.value
        _pendingUrl.value = com.lidarscan.core.feedback.GitHubIssue.feedbackUrl(
            message = text,
            facts = _uiState.value.facts,
        )
        _message.value = ""
        _uiState.value = _uiState.value.copy(result = FeedbackWording.OPENED_GITHUB)
    }

    /**
     * ROUND 29 item 173(b) — **SEND DIAGNOSTICS, to the door he picked.**
     *
     * One code path for the destinations that produce a file, as round 24 had
     * for two: the zip is packed and lands in `Downloads/LidarScan` whatever
     * happens next, and only then does the route decide whether anything else
     * is attempted.
     *
     * **ROUND 35 item 186** carves [FeedbackRoute.GITHUB] out of that: it packs
     * nothing, writes nothing, and its whole payload is the URL built below
     * from the log itself. Round 29 wrote a zip on this path so the issue body
     * could name it — and the issue then asked the operator to go and find it
     * and drag it in, which is the manoeuvre the owner has removed.
     *
     * A second send while one is running is refused rather than queued — two
     * zips of the same log is not what a double tap means.
     */
    fun sendDiagnostics(route: FeedbackRoute) {
        if (_uiState.value.sending != null) return
        _uiState.value = _uiState.value.copy(sending = 0f, result = null)
        jobScope.launch {
            val settings = settingsRepository.settings.first()
            val facts = _uiState.value.facts
            val result: FeedbackResult = sender.send(
                config = FeedbackConfig(settings.cloudBaseUrl, settings.cloudToken),
                facts = facts,
                message = "",
                route = route,
                onProgress = { fraction ->
                    _uiState.value = _uiState.value.copy(sending = fraction)
                },
            )
            // ROUND 35 item 186: the issue carries the log AS TEXT, so the URL
            // is built from the log rather than from the name of a zip — and
            // there is no zip on this route any more to name.
            if (route == FeedbackRoute.GITHUB) {
                _pendingUrl.value = sender.diagnosticsIssueUrl(facts)
            }
            _uiState.value = _uiState.value.copy(
                sending = null,
                result = FeedbackWording.resultFor(result),
            )
        }
    }

    /** Bytes under [dir], following the tree. Symlinks are not created by this app. */
    private fun sizeOf(dir: File): Long =
        dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }

    /**
     * One directory walk, four numbers.
     *
     * A holder rather than four `runCatching` calls in [refresh] because the
     * walk is the expensive part and doing it once is the difference between
     * one pass over 66 project directories and four.
     */
    private data class LibraryWalk(
        val count: Int,
        val bytes: Long,
        val points: Long,
        val georeferenced: Int,
    )
}

/**
 * ROUND 28 item 165 — **the "This phone" table's four values, as pure
 * functions.**
 *
 * §A.8's finding F6 is the only ✅ in the whole review: *"the This phone spec
 * table is the best-built pattern in the app"*, and §C.4 made it the model for
 * `ScanRow` everywhere. So its content is kept verbatim and the strings it
 * shows move here, where they can be pinned by a test rather than by a
 * screenshot — the same treatment
 * [com.lidarscan.core.render.PointCountFormat] got for the same reason.
 *
 * Points go through `PointCountFormat.longForm`; there is no second point
 * formatter in this app any more, which is what item 150 was about.
 */
object ProfileFacts {

    /** `Ollidar 0.9.13 (913)`. */
    fun appLine(appName: String, version: String, versionCode: Int): String =
        "$appName $version ($versionCode)"

    /**
     * `Pixel 8 Pro · Android 16`.
     *
     * One row where there were two. The model and the OS release are always
     * read together — nobody has ever wanted one without the other — and F6's
     * table is a table of ANSWERS, so a row per field was a row too many.
     */
    fun deviceLine(model: String, androidVersion: String): String {
        val phone = model.trim().ifBlank { "Unknown phone" }
        return if (androidVersion.isBlank()) phone else "$phone · Android $androidVersion"
    }

    /**
     * `66 · 8.1 M points` — the scan count and the point total the Projects
     * header carried until item 151 removed it.
     */
    fun scansLine(scanCount: Int, totalPoints: Long): String = when {
        scanCount <= 0 -> "None yet"
        else -> "$scanCount · ${com.lidarscan.core.render.PointCountFormat.longForm(totalPoints)}"
    }

    /**
     * `65 of 66` — the third figure from that header.
     *
     * Stated as a fraction rather than a bare count because the number only
     * means something against the total: 65 georeferenced scans is excellent
     * out of 66 and alarming out of 400.
     */
    fun georeferencedLine(georeferenced: Int, scanCount: Int): String = when {
        scanCount <= 0 -> "None yet"
        georeferenced <= 0 -> "None of $scanCount"
        else -> "$georeferenced of $scanCount"
    }
}
