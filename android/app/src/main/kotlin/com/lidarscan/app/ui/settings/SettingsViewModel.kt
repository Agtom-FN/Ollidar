package com.lidarscan.app.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.data.AppSettings
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.data.ThemeMode
import com.lidarscan.app.data.Units
import com.lidarscan.app.debug.REPLAY_PROJECT_NAME
import com.lidarscan.core.feedback.DeviceFacts
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
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
    /**
     * ROUND 25 item 118, **owner amendment**: the connection-detection sweep
     * behind the developer-mode "Connection debug" row. Null in a bare-JVM
     * test, and null is the whole degradation — the row simply reports that
     * there is nothing to sweep with.
     */
    private val connectionDebug: com.lidarscan.app.net.ConnectionDebugSweeper? = null,
) : ViewModel() {

    // ── ROUND 25 item 118 (owner amendment): Connection debug ──────────────
    //
    // The owner's Acer HY41-T9 hub did not work and the app said only "No
    // Ethernet adapter found." — which cannot distinguish "never enumerated on
    // USB" from "enumerated, no interface appeared" from "interface up on the
    // wrong subnet". The periodic `[net-debug]` sweeps fix that for whoever
    // reads the log afterwards; this row fixes it for whoever is standing
    // there with the hub in their hand, by running one full sweep NOW and
    // putting it on the screen.
    //
    // Deliberately NOT rate-limited (see ConnectionDebugSweeper.sweepNow): a
    // person pressing a button is entitled to an answer. It still writes to
    // the log as well as to the screen, because the two audiences are
    // different people at different times.

    private val _connectionDebugOutput = MutableStateFlow<String?>(null)

    /** The last sweep's rendered block, or null before one has been run. Shown monospace and scrollable. */
    val connectionDebugOutput: StateFlow<String?> = _connectionDebugOutput.asStateFlow()

    private val _connectionDebugRunning = MutableStateFlow(false)

    /** True while a sweep is in flight, so the row can say so rather than looking dead. */
    val connectionDebugRunning: StateFlow<Boolean> = _connectionDebugRunning.asStateFlow()

    /**
     * Runs one full detection sweep and shows it.
     *
     * The expected host IP comes from this device's last successfully detected
     * Mid-360 rather than a constant: the verdict's "wrong subnet" branch is
     * only meaningful against the address the lidar actually unicasts to, and
     * saying `192.168.1.5` when the device on site was configured for
     * `192.168.2.5` would turn the sweep into a confident wrong answer.
     */
    fun runConnectionDebugSweep() {
        val sweeper = connectionDebug
        if (sweeper == null) {
            _connectionDebugOutput.value = "Connection debug is unavailable in this build."
            return
        }
        if (_connectionDebugRunning.value) return
        _connectionDebugRunning.value = true
        viewModelScope.launch {
            // Turning the flag on here as well as in the Capture collector: a
            // person who has just unlocked developer mode and pressed this
            // button has not necessarily passed through the Capture tab since,
            // and a sweep that silently reported an empty discovery section
            // would be the same class of uninformative the amendment exists to
            // remove.
            sweeper.enabled = true
            val hostIp = runCatching {
                settingsRepository.settings.first().lastDetectedMid360HostIp
            }.getOrNull() ?: com.lidarscan.core.net.Mid360Settings.DEFAULT_HOST_IP
            _connectionDebugOutput.value = runCatching {
                sweeper.sweepNow(
                    context = com.lidarscan.app.net.ConnectionDebugSweeper.SweepContext(expectedHostIp = hostIp),
                )
            }.getOrElse { e -> "Connection debug sweep failed: ${e.javaClass.simpleName}: ${e.message}" }
            _connectionDebugRunning.value = false
        }
    }

    // ── ROUND 24 item 113: DETAIL, in Settings ─────────────────────────────
    //
    // The owner's grouping puts "detail" under Scanning, and this is that row.
    // It is deliberately NOT a fourth detail model: it reads and writes the
    // SAME persisted display block the Scan screen's Advanced sheet and
    // Review's panel already share (`SettingsRepository.displayParams`), and it
    // is clamped by the same ROUND 22 item 100 ceiling on the way in and out.
    // A device that can only hold Auto is offered only Auto, here as
    // everywhere.

    /** The rungs this device may be offered — item 100's ceiling, applied. */
    val detailLevels: List<com.lidarscan.core.capture.DetailLevel>
        get() = com.lidarscan.core.capture.DetailLevels.selectableOn(settingsRepository.deviceTier)

    /** "Limited by this device", or null when nothing is being limited. */
    val detailCeilingNote: String?
        get() = com.lidarscan.core.capture.DetailLevels.ceilingNote(settingsRepository.deviceTier)

    /**
     * ROUND 29 item 172 — what a rung reads out on this phone.
     *
     * The same `:core` function the Scan sheet calls, so the Detail row in
     * Settings and the Detail row in the Advanced sheet cannot describe the
     * same setting differently.
     */
    fun detailReadout(level: com.lidarscan.core.capture.DetailLevel): String =
        com.lidarscan.core.capture.DetailLevels.readoutFor(level, settingsRepository.deviceTier)

    private val _detailLevel = MutableStateFlow(com.lidarscan.core.capture.DetailLevels.DEFAULT)
    val detailLevel: StateFlow<com.lidarscan.core.capture.DetailLevel> = _detailLevel.asStateFlow()

    /** Re-read on entry: the Advanced sheet may have changed it since the last visit. */
    fun refreshDetailLevel() {
        viewModelScope.launch {
            val params = settingsRepository.displayParams()
                ?: com.lidarscan.core.render.DisplayParams.captureDefaults()
            _detailLevel.value = com.lidarscan.core.capture.DetailLevels.levelForBudget(
                params.lodPointBudget,
                settingsRepository.deviceTier,
            )
        }
    }

    fun setDetailLevel(level: com.lidarscan.core.capture.DetailLevel) {
        _detailLevel.value = level
        viewModelScope.launch {
            val current = settingsRepository.displayParams()
                ?: com.lidarscan.core.render.DisplayParams.captureDefaults()
            settingsRepository.setDisplayParams(
                current.copy(
                    lodPointBudget = com.lidarscan.core.capture.DetailLevels.budgetPointsFor(
                        level,
                        settingsRepository.deviceTier,
                    ),
                ),
            )
        }
    }

    /** ROUND 24 item 110(b): the Settings row that replays the tour. */
    fun markTutorialSeen() {
        viewModelScope.launch { settingsRepository.setTutorialSeen() }
    }

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

    // ── ROUND 28 item 164 (T1): what the Storage row says instead of a path ──
    //
    // The old first card in Settings was
    // `/storage/emulated/0/Android/data/com.lidarscan.app.debug/files/Projects`
    // in two lines of mono, above the first section header. It is the most
    // prominent position on the screen and it was spent on the one string on
    // the page that means nothing to anybody holding a phone. The path is not
    // deleted — it is developer-mode evidence and it survives there — but the
    // ordinary Storage row now answers the question an operator actually has:
    // how much of this phone are my scans using, and how many are there.
    //
    // Recomputed on entry rather than collected, for the same reason
    // `ProfileViewModel.refresh` is: it is a directory walk, and a figure that
    // recomputes on every recomposition costs a frame every time a switch on
    // this page moves.

    private val _storageBytes = MutableStateFlow(0L)

    /** Bytes under the projects root, as of the last [refreshStorage]. */
    val storageBytes: StateFlow<Long> = _storageBytes.asStateFlow()

    private val _scanCount = MutableStateFlow(0)

    /** How many projects are on the device, as of the last [refreshStorage]. */
    val scanCount: StateFlow<Int> = _scanCount.asStateFlow()

    fun refreshStorage() {
        viewModelScope.launch(Dispatchers.IO) {
            _scanCount.value = runCatching { projectStore.list().size }.getOrDefault(0)
            _storageBytes.value = runCatching {
                java.io.File(storageLocation).walkTopDown().filter { it.isFile }.sumOf { it.length() }
            }.getOrDefault(0L)
        }
    }

    // ── ROUND 9, owner item 33: empty scans ─────────────────────────────────
    //
    // "prune 0-point legacy strays (scan-012/-014 style) — offer/perform cleanup
    // of empty projects in the list". The switch below decides what happens to
    // NEW ones (Stop deletes a 0-point scan rather than keeping it, and the
    // Projects tab hides the ones already on disk); this counter and its action
    // are what actually get the legacy clutter off the phone.

    private val _emptyScanCount = MutableStateFlow(0)

    /** How many 0-point `.lscan` directories are on this device right now. */
    val emptyScanCount: StateFlow<Int> = _emptyScanCount.asStateFlow()

    /** What the last cleanup did, in one line. Null until one has been run. */
    private val _emptyScanNote = MutableStateFlow<String?>(null)
    val emptyScanNote: StateFlow<String?> = _emptyScanNote.asStateFlow()

    init {
        refreshEmptyScanCount()
    }

    fun refreshEmptyScanCount() {
        viewModelScope.launch(Dispatchers.IO) {
            _emptyScanCount.value = runCatching {
                projectStore.list().count { it.manifest.isEmptyScan }
            }.getOrDefault(0)
        }
    }

    /** ROUND 22 item 97: see [com.lidarscan.app.data.AppSettings.advancedFeatures]. */
    fun setAdvancedFeatures(enabled: Boolean) {
        viewModelScope.launch { settingsRepository.setAdvancedFeatures(enabled) }
    }

    /** ROUND 32 item 177: see [com.lidarscan.app.data.AppSettings.welcomeAnimation]. */
    fun setWelcomeAnimation(enabled: Boolean) {
        viewModelScope.launch { settingsRepository.setWelcomeAnimation(enabled) }
    }

    fun setKeepEmptyScans(keep: Boolean) {
        viewModelScope.launch { settingsRepository.setKeepEmptyScans(keep) }
    }

    /** ROUND 17 item 66: the seven-tap unlock, and its one setting. */
    fun setDeveloperMode(enabled: Boolean) {
        viewModelScope.launch { settingsRepository.setDeveloperMode(enabled) }
        // ROUND 25 item 118 (owner amendment): the `[net-debug]` channel rides
        // the same unlock. Mirrored synchronously here so re-locking stops the
        // logging at once rather than at the next Capture-tab collection —
        // a developer switch that keeps writing after it is switched off is
        // not a switch.
        connectionDebug?.enabled = enabled
        if (!enabled) _connectionDebugOutput.value = null
    }

    fun setCaptureDebugLog(enabled: Boolean) {
        viewModelScope.launch { settingsRepository.setCaptureDebugLog(enabled) }
    }

    /** ROUND 11 (owner item 43). */
    fun setOperatorCuesEnabled(enabled: Boolean) {
        viewModelScope.launch { settingsRepository.setOperatorCuesEnabled(enabled) }
    }

    /** ROUND 13 (owner item 47). */
    fun setDndDuringCapture(enabled: Boolean) {
        viewModelScope.launch { settingsRepository.setDndDuringCapture(enabled) }
    }

    // ── ROUND 20 item 82: the per-device mount profile ──────────────────────

    /** The trim half of the profile, for the Settings read-out. */
    private val _storedMountTrim =
        MutableStateFlow<com.lidarscan.core.calib.StoredMountTrim?>(null)
    val storedMountTrim: StateFlow<com.lidarscan.core.calib.StoredMountTrim?> =
        _storedMountTrim.asStateFlow()

    fun refreshMountProfile() {
        viewModelScope.launch {
            _storedMountTrim.value = runCatching { settingsRepository.storedMountTrim() }.getOrNull()
        }
    }

    /**
     * Persists the three lever-arm fields, clamped. Applied to the NEXT
     * capture (the Capture tab reads it at construction — device facts change
     * rarely, and mid-walk lever-arm edits are not a flow worth having).
     */
    fun setMountLeverArm(upCm: Double, behindCm: Double, rightCm: Double) {
        viewModelScope.launch {
            settingsRepository.setMountLeverArm(
                com.lidarscan.core.calib.MountLeverArm.clamped(
                    upCm = upCm,
                    behindCm = behindCm,
                    rightCm = rightCm,
                    nowMillis = System.currentTimeMillis(),
                ),
            )
        }
    }

    /** Back to the shipped defaults — provenance returns to "default". */
    fun resetMountLeverArm() {
        viewModelScope.launch {
            settingsRepository.setMountLeverArm(com.lidarscan.core.calib.MountLeverArm.DEFAULT)
        }
    }

    /**
     * Deletes every 0-point project on the device.
     *
     * Unconditionally destructive and deliberately not gated on
     * [AppSettings.keepEmptyScans]: pressing a button called "Clean up empty
     * scans" is a decision about these directories, not about the default. What
     * it removes has, by definition, no points in it — the manifest and an empty
     * `streams/` tree — and the capture log's `sealed OK … NO-DATA=true` lines
     * for those attempts survive it, since the log is not in the project.
     */
    fun cleanUpEmptyScans() {
        viewModelScope.launch(Dispatchers.IO) {
            val empties = runCatching { projectStore.list().filter { it.manifest.isEmptyScan } }
                .getOrDefault(emptyList())
            var deleted = 0
            empties.forEach { project ->
                com.lidarscan.app.ui.projects.ProjectPreviewCache.invalidate(project.id)
                if (runCatching { projectStore.delete(project.id) }.getOrDefault(false)) deleted++
            }
            _emptyScanCount.value = runCatching {
                projectStore.list().count { it.manifest.isEmptyScan }
            }.getOrDefault(0)
            _emptyScanNote.value = when {
                empties.isEmpty() -> "No empty scans to clean up."
                deleted == empties.size ->
                    "Deleted $deleted empty scan${if (deleted == 1) "" else "s"}. " +
                        "The Projects list refreshes when you go back to it."
                else ->
                    "Deleted $deleted of ${empties.size} empty scans — the rest could not be removed " +
                        "(check the storage location in this screen)."
            }
        }
    }

    fun dismissEmptyScanNote() {
        _emptyScanNote.value = null
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

/**
 * ROUND 28 items 164 / 165 — **the right-hand column of Settings, as pure
 * functions.**
 *
 * Every row on the rebuilt Settings page is `title … meta`, and the meta is a
 * value: `8.1 GB · 66 scans`, `0.9.13 (913)`, `Set · 91.0°`, `native · ABI 12`.
 * They live here rather than inline in the Composable for the reason
 * [com.lidarscan.core.render.PointCountFormat] exists: a string built at a call
 * site is a string that can only be checked by looking at a screenshot, and
 * these are the strings the review found wrong (T4's invisible version line,
 * T1's raw path standing in for a storage figure).
 *
 * Bytes go through [DeviceFacts.formatBytes] — the app already had one byte
 * formatter and it is on the Profile page, which is the other half of this
 * round. Two byte formatters is how `8.1 GB` and `8,100 MB` end up on two
 * screens describing the same directory.
 */
object SettingsFormat {

    /**
     * The Storage row: `8.1 GB · 66 scans`.
     *
     * The count is part of the value rather than a second row because the
     * figure only means anything against it — 8 GB is alarming for six scans
     * and unremarkable for sixty-six.
     */
    fun storageLine(bytes: Long, scans: Int): String = when {
        scans <= 0 -> "No scans yet"
        scans == 1 -> "${DeviceFacts.formatBytes(bytes)} · 1 scan"
        else -> "${DeviceFacts.formatBytes(bytes)} · $scans scans"
    }

    /**
     * The Version row: `0.9.13 (913)`, and `0.9.13 (913) · dev` once the seven
     * taps have landed.
     *
     * T4: this string used to render at roughly 1.5:1 in `outline`. The colour
     * is the screen's business; what belongs here is that the developer state
     * is part of the VALUE, not a second line — the mockup's own answer, and
     * the only indicator of an unlocked device that is visible without
     * scrolling to the section it unlocks.
     */
    fun versionLine(version: String, versionCode: Int, developerMode: Boolean): String =
        "$version ($versionCode)" + if (developerMode) " · dev" else ""

    /**
     * The Mount row: `Set · 91.0°`, or `Not set` when no hold-steady has ever
     * measured this rig.
     *
     * "Not set" rather than a red anything: an unmeasured mount is the state
     * every fresh install is in, and §C.6's rule is that not-applicable is
     * ink-mute, never `bad`.
     */
    fun mountLine(trimMagnitudeDeg: Double?): String =
        if (trimMagnitudeDeg == null) "Not set" else "Set · %.1f°".format(trimMagnitudeDeg)

    /** The developer Sensor-timing row: `D6 · 12 ms`. */
    fun sensorTimingLine(latencyMs: Int): String = "D6 · $latencyMs ms"

    /**
     * The developer Engine row: `native · ABI 12`, or `simulated`.
     *
     * Both halves are stated because they answer different questions and the
     * app has shipped builds where they disagreed: whether the `.so` loaded at
     * all, and whether the switch above is overriding it anyway.
     */
    fun engineLine(nativeAvailable: Boolean, useFakeEngine: Boolean, abi: Int): String = when {
        !nativeAvailable -> "simulated · no native library"
        useFakeEngine -> "simulated · ABI $abi available"
        else -> "native · ABI $abi"
    }

    /** The developer Capture-log row: `2.1 MB`, or `empty` before anything is written. */
    fun captureLogLine(bytes: Long): String =
        if (bytes <= 0L) "empty" else DeviceFacts.formatBytes(bytes)

    /**
     * The Empty-scans row's value.
     *
     * Zero is a sentence rather than a `0`, because "0" beside a Clean up
     * button reads as a button that has not been pressed yet rather than as
     * nothing to do.
     */
    fun emptyScanLine(count: Int): String = when {
        count <= 0 -> "None"
        count == 1 -> "1 scan"
        else -> "$count scans"
    }

    /** The Cloud row: the host, or that there is nothing configured. */
    fun cloudLine(baseUrl: String): String =
        if (baseUrl.isBlank()) "Not set" else baseUrl.trim().removePrefix("https://").removePrefix("http://").trimEnd('/')
}
