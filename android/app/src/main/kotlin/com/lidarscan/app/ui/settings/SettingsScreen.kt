package com.lidarscan.app.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.data.AppSettings
import com.lidarscan.app.data.ThemeMode
import com.lidarscan.app.data.Units
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.ScanRow
import com.lidarscan.app.ui.components.ScanRowCard
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.SectionLabel
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.app.ui.theme.ScanDisplay
import com.lidarscan.app.ui.theme.ScanMeta
import com.lidarscan.app.ui.theme.ScanTitle
import com.lidarscan.core.Wording
import com.lidarscan.core.model.CaptureDefaults
import com.lidarscan.core.model.WorkflowProfile
import kotlin.math.roundToInt

@Composable
fun SettingsRoute(
    container: AppContainer,
    onBack: () -> Unit,
    onReplaySyntheticCapture: (projectId: String) -> Unit = {},
    /** ROUND 24 item 109/113: the Profile row, now in About (ROUND 28 item 164). */
    onOpenProfile: () -> Unit = {},
    /** ROUND 24 item 110(b): the About section's Tutorial row. */
    onReplayTutorial: () -> Unit = {},
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    // ROUND 14: the grant is made on a system screen with no result callback,
    // so the only honest moment to re-read it is when this screen comes back.
    var dndAccessGranted by remember { mutableStateOf(container.dndGuard.hasPolicyAccess) }
    val lifecycleOwner = androidx.lifecycle.compose.LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = androidx.lifecycle.LifecycleEventObserver { _, event ->
            if (event == androidx.lifecycle.Lifecycle.Event.ON_RESUME) {
                dndAccessGranted = container.dndGuard.hasPolicyAccess
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    val viewModel: SettingsViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                SettingsViewModel(
                    settingsRepository = container.settingsRepository,
                    projectStore = container.projectStore,
                    storageLocation = container.projectsRootDir.absolutePath,
                    captureLog = container.captureLog,
                    shareCacheDir = java.io.File(context.cacheDir, "shared"),
                    onSensorLatencyApplied = container.d6UsbConnectionRegistry::setSensorLatencyMillis,
                    downloadsContext = container.applicationContext,
                    // ROUND 25 item 118 (owner amendment): the container's one
                    // sweeper — same instance, and therefore same rate limiter,
                    // as the Mid-360 wizard's poll and the Capture tab's
                    // auto-detect.
                    connectionDebug = container.connectionDebugSweeper,
                    shareFile = { file ->
                        com.lidarscan.app.share.ShareTargets.shareFile(
                            context,
                            file,
                            "text/plain",
                            "Share capture log",
                        )
                    },
                )
            }
        },
    )
    val settings by viewModel.settings.collectAsStateWithLifecycle()
    // ROUND 6 (item 20): the log's own live tail, so the screen shows that
    // logging is genuinely happening rather than just naming a path.
    val captureLogLastLine by viewModel.captureLogLastLine.collectAsStateWithLifecycle()
    val captureLogSize = remember(captureLogLastLine) { viewModel.captureLogSizeBytes() }
    // ROUND 7: the log export's own outcome — a path, or a reason.
    val exportNote by viewModel.exportNote.collectAsStateWithLifecycle()
    // ROUND 9 (item 33): how many 0-point strays are on the phone, recounted
    // every time this screen is entered — the Capture tab may have pruned some
    // since the last visit.
    val emptyScanCount by viewModel.emptyScanCount.collectAsStateWithLifecycle()
    val emptyScanNote by viewModel.emptyScanNote.collectAsStateWithLifecycle()
    androidx.compose.runtime.LaunchedEffect(Unit) { viewModel.refreshEmptyScanCount() }
    // ROUND 28 item 164 (T1): the Storage row's own two numbers, in place of
    // the raw path that used to open this page.
    val storageBytes by viewModel.storageBytes.collectAsStateWithLifecycle()
    val scanCount by viewModel.scanCount.collectAsStateWithLifecycle()
    androidx.compose.runtime.LaunchedEffect(Unit) { viewModel.refreshStorage() }
    // ROUND 20 item 82: the trim half of the mount profile, re-read on entry.
    val storedMountTrim by viewModel.storedMountTrim.collectAsStateWithLifecycle()
    androidx.compose.runtime.LaunchedEffect(Unit) { viewModel.refreshMountProfile() }
    // ROUND 24 item 113: the Detail row shares the persisted display block, so
    // it is re-read on entry rather than held — the Advanced sheet may have
    // moved it since the last visit.
    val detailLevel by viewModel.detailLevel.collectAsStateWithLifecycle()
    androidx.compose.runtime.LaunchedEffect(Unit) { viewModel.refreshDetailLevel() }
    // ROUND 25 item 118 (owner amendment): the on-demand connection sweep.
    val connectionDebugOutput by viewModel.connectionDebugOutput.collectAsStateWithLifecycle()
    val connectionDebugRunning by viewModel.connectionDebugRunning.collectAsStateWithLifecycle()

    SettingsScreen(
        settings = settings,
        storageLocation = viewModel.storageLocation,
        storageBytes = storageBytes,
        scanCount = scanCount,
        captureLogPath = viewModel.captureLogPath,
        captureLogSizeBytes = captureLogSize,
        captureLogLastLine = captureLogLastLine,
        exportNote = exportNote,
        onDismissExportNote = viewModel::dismissExportNote,
        onShareCaptureLog = viewModel::shareCaptureLog,
        onClearCaptureLog = viewModel::clearCaptureLog,
        nativeEngineAvailable = com.lidarscan.app.engine.ScanEngineNative.isAvailable,
        engineAbi = runCatching {
            if (com.lidarscan.app.engine.ScanEngineNative.isAvailable) {
                com.lidarscan.app.engine.ScanEngineNative.nativeAbiVersion()
            } else {
                0
            }
        }.getOrDefault(0),
        onUnitsChange = viewModel::setUnits,
        onThemeModeChange = viewModel::setThemeMode,
        onUseFakeEngineChange = viewModel::setUseFakeEngine,
        onCloudChange = viewModel::setCloud,
        onAllowPoorSyncChange = viewModel::setAllowPoorSyncColorize,
        onD6SensorLatencyChange = viewModel::setD6SensorLatencyMs,
        emptyScanCount = emptyScanCount,
        emptyScanNote = emptyScanNote,
        onKeepEmptyScansChange = viewModel::setKeepEmptyScans,
        onAdvancedFeaturesChange = viewModel::setAdvancedFeatures,
        onDeveloperModeChange = viewModel::setDeveloperMode,
        onCaptureDebugLogChange = viewModel::setCaptureDebugLog,
        connectionDebugOutput = connectionDebugOutput,
        connectionDebugRunning = connectionDebugRunning,
        onRunConnectionDebug = viewModel::runConnectionDebugSweep,
        onOperatorCuesChange = viewModel::setOperatorCuesEnabled,
        onDndDuringCaptureChange = viewModel::setDndDuringCapture,
        dndAccessGranted = dndAccessGranted,
        onGrantDndAccess = {
            runCatching { context.startActivity(container.dndGuard.policyAccessIntent()) }
        },
        onCleanUpEmptyScans = viewModel::cleanUpEmptyScans,
        onDismissEmptyScanNote = viewModel::dismissEmptyScanNote,
        onReplaySyntheticCapture = { viewModel.replaySyntheticCapture(onReplaySyntheticCapture) },
        storedMountTrim = storedMountTrim,
        onMountLeverArmChange = viewModel::setMountLeverArm,
        onMountLeverArmReset = viewModel::resetMountLeverArm,
        onOpenProfile = onOpenProfile,
        detailLevels = viewModel.detailLevels,
        detailLevel = detailLevel,
        detailCeilingNote = viewModel.detailCeilingNote,
        onDetailChange = viewModel::setDetailLevel,
        onReplayTutorial = {
            // Seeing it again is still seeing it: replaying retires the
            // first-run offer, so a tour watched from here on day one cannot
            // be followed by "New here?" on day two.
            viewModel.markTutorialSeen()
            onReplayTutorial()
        },
        onBack = onBack,
    )
}

/**
 * ROUND 28 item 164 — **Settings, rebuilt as §D.7.**
 *
 * ## What was wrong
 *
 * The review's §A.7 found seven things and six of them were one thing: the page
 * was built out of cards when it wanted rows.
 *
 *  * **T1** — the first card on the page was
 *    `/storage/emulated/0/Android/data/com.lidarscan.app.debug/files/Projects`
 *    in two lines of mono, above the first section header, belonging to no
 *    section. Maximum jargon in the most prominent position on a tab-bar
 *    screen. It is gone from the ordinary page; `Storage` is now a row that
 *    says `8.1 GB · 66 scans`, and the path survives under developer mode where
 *    it is evidence rather than decoration.
 *  * **T2** — `DISPLAY`, `ADVANCED FEATURES` and `ABOUT` were drawn in Agtom
 *    orange. Three passive labels spending the one brand accent is the whole
 *    mechanism of accent inflation. They are [SectionLabel] now: meta-caps,
 *    ink-mute. The accent law allows orange twice per screen, and on this
 *    screen it is spent on the active tab and nothing else.
 *  * **T3** — five sections meant five floating slabs with big gaps and no
 *    rhythm. Each section is now ONE [ScanRowCard] of [ScanRow]s with hairlines
 *    between them, which is the shape the review named as the app's best
 *    pattern (Profile's "This phone" table, finding F6).
 *  * **T4** — the version line rendered at about 1.5:1 in `outline`. It is a
 *    row like every other row, [ScanMeta] in `inkMute`, and it still takes the
 *    seven taps.
 *  * **T5** — the Advanced-features switch read as *disabled* rather than
 *    *off*, because the M3 default unchecked track is grey on a grey card. See
 *    [ScanSwitchRow]: the off state is now the trough with an ink thumb and a
 *    line border.
 *  * **T6** — only `Tutorial` had a chevron; the rest were cards that looked
 *    identical and did nothing. A chevron appears on exactly the rows that
 *    navigate or open a sheet.
 *  * **T7** — the dead space under the last card is gone because the page is
 *    now long enough to scroll: every setting that used to be a card with a
 *    title, a hint and a control is one 56 dp row.
 *
 * ## Where the controls went
 *
 * Nothing is deleted and nothing is re-plumbed. What used to be a card with a
 * heading and its control underneath is now a row whose value is on the right
 * and whose control is in a **sheet** behind it — the mount lever arm, the
 * three pickers, the cloud credentials, the sensor-latency slider, the
 * workflow-profile reference and the connection sweep. A sheet rather than a
 * screen because `ui/nav` belongs to the navigation graph and none of these is
 * a destination anyone would want to reach by URL.
 *
 * ## Developer mode
 *
 * Seven taps on the Version row still unlocks the section, which now holds the
 * six rows the mockup specifies plus the two raw paths. `AppSettings.developerMode`
 * already existed (ROUND 17 item 66) and is reused; nothing new is persisted.
 *
 * Settings is a **tab**, so it has no back arrow — the tab bar is the way out,
 * and [onBack] is what the system back gesture routes through.
 */
@Composable
fun SettingsScreen(
    settings: AppSettings,
    storageLocation: String,
    /** ROUND 28 item 164 (T1): the two numbers the Storage row shows instead of a path. */
    storageBytes: Long = 0L,
    scanCount: Int = 0,
    /** ROUND 6 (owner item 20): where the on-device capture log lives, and its size. */
    captureLogPath: String = "",
    captureLogSizeBytes: Long = 0L,
    captureLogLastLine: String? = null,
    /** ROUND 7: what the last log export actually did — a destination path, or why not. */
    exportNote: String? = null,
    onDismissExportNote: () -> Unit = {},
    onShareCaptureLog: () -> Unit = {},
    onClearCaptureLog: () -> Unit = {},
    nativeEngineAvailable: Boolean,
    /** ROUND 28 item 164: the developer Engine row's second half. */
    engineAbi: Int = 0,
    onUnitsChange: (Units) -> Unit,
    onThemeModeChange: (ThemeMode) -> Unit,
    onUseFakeEngineChange: (Boolean) -> Unit,
    onCloudChange: (String, String) -> Unit = { _, _ -> },
    onAllowPoorSyncChange: (Boolean) -> Unit = {},
    onD6SensorLatencyChange: (Int) -> Unit = {},
    /** ROUND 9 (owner item 33): how many 0-point strays are on the device right now. */
    emptyScanCount: Int = 0,
    /** What the last "clean up empty scans" actually did. */
    emptyScanNote: String? = null,
    onKeepEmptyScansChange: (Boolean) -> Unit = {},
    /** ROUND 22 item 97: the one Advanced-features switch. Default OFF. */
    onAdvancedFeaturesChange: (Boolean) -> Unit = {},
    /** ROUND 17 item 66: the seven-tap unlock and everything behind it. */
    onDeveloperModeChange: (Boolean) -> Unit = {},
    onCaptureDebugLogChange: (Boolean) -> Unit = {},
    /**
     * ROUND 25 item 118 (owner amendment): the last connection-detection sweep,
     * rendered. Null until one has been run.
     */
    connectionDebugOutput: String? = null,
    connectionDebugRunning: Boolean = false,
    onRunConnectionDebug: () -> Unit = {},
    onOperatorCuesChange: (Boolean) -> Unit = {},
    onDndDuringCaptureChange: (Boolean) -> Unit = {},
    /**
     * ROUND 14 (owner item 52) — whether the system has actually granted
     * notification-policy access. Re-read on every ON_RESUME, because the grant
     * happens in another Activity and there is no result to observe.
     */
    dndAccessGranted: Boolean = false,
    onGrantDndAccess: () -> Unit = {},
    onCleanUpEmptyScans: () -> Unit = {},
    onDismissEmptyScanNote: () -> Unit = {},
    onReplaySyntheticCapture: () -> Unit = {},
    /** ROUND 20 item 82: the mount profile — the trim in force, for the read-out. */
    storedMountTrim: com.lidarscan.core.calib.StoredMountTrim? = null,
    onMountLeverArmChange: (Double, Double, Double) -> Unit = { _, _, _ -> },
    onMountLeverArmReset: () -> Unit = {},
    /** ROUND 24 item 109: the Profile row. */
    onOpenProfile: () -> Unit = {},
    /** ROUND 24 item 113: DETAIL, clamped to this device by item 100's ceiling. */
    detailLevels: List<com.lidarscan.core.capture.DetailLevel> =
        com.lidarscan.core.capture.DetailLevel.entries,
    detailLevel: com.lidarscan.core.capture.DetailLevel =
        com.lidarscan.core.capture.DetailLevels.DEFAULT,
    detailCeilingNote: String? = null,
    onDetailChange: (com.lidarscan.core.capture.DetailLevel) -> Unit = {},
    /** ROUND 24 item 110(b): replays the Scan-screen tour. */
    onReplayTutorial: () -> Unit = {},
    onBack: () -> Unit,
) {
    // Which sheet is open, if any. One value rather than seven booleans: two
    // sheets can never be open at once, and a nullable enum is the only shape
    // that cannot express the state where they are.
    var sheet: SettingsSheet? by remember { mutableStateOf(null) }

    Column(
        Modifier
            .fillMaxSize()
            .background(ScanColors.page)
            .statusBarsPadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .testTag("settingsScreen"),
    ) {
        // §C.2: ONE ScanDisplay per screen. The developer sub-line is the
        // mockup's own: an unlocked device says so at the top rather than only
        // in the section three scrolls down that it unlocked.
        Column(
            Modifier
                .fillMaxWidth()
                .padding(
                    start = ScanDims.ScreenMargin,
                    end = ScanDims.ScreenMargin,
                    top = ScanDims.S4,
                    bottom = ScanDims.S2,
                ),
        ) {
            Text("Settings", style = ScanDisplay, color = ScanColors.ink)
            if (settings.developerMode) {
                Text("Developer mode on", style = ScanMeta, color = ScanColors.warn)
            }
        }

        // ── DISPLAY ─────────────────────────────────────────────────────────
        //
        // Units and Theme are neither developer settings nor advanced features
        // — they are what a person wants their app to look like. Two rows, two
        // values, two pickers behind them.
        SectionLabel("Display")
        SettingsCard {
            listOf<@Composable () -> Unit>(
                {
                    ScanRow(
                        title = "Units",
                        meta = settings.units.displayName,
                        trailing = { Chevron() },
                        onClick = { sheet = SettingsSheet.UNITS },
                        modifier = Modifier.testTag("settingsUnitsRow"),
                    )
                },
                {
                    ScanRow(
                        title = "Theme",
                        meta = settings.themeMode.displayName,
                        trailing = { Chevron() },
                        onClick = { sheet = SettingsSheet.THEME },
                        modifier = Modifier.testTag("settingsThemeRow"),
                    )
                },
            )
        }

        // ── SCANNING ────────────────────────────────────────────────────────
        SectionLabel("Scanning")
        SettingsCard {
            buildList<@Composable () -> Unit> {
                add {
                    // ROUND 20 item 82: whether this rig's rotation has ever
                    // been measured, and by how much. The three lever-arm
                    // fields and the convention schematic are behind it.
                    ScanRow(
                        title = "Mount",
                        meta = SettingsFormat.mountLine(storedMountTrim?.trim?.magnitudeDeg),
                        trailing = { Chevron() },
                        onClick = { sheet = SettingsSheet.MOUNT },
                        modifier = Modifier.testTag("settingsMountRow"),
                    )
                }
                add {
                    // ROUND 24 item 113: the SAME persisted display block the
                    // Scan screen's Advanced sheet and Review's panel share,
                    // clamped by item 100's ceiling. The ceiling note is the
                    // row's detail line rather than a hint under a card, so
                    // "why are there two rungs and not three" is answered
                    // where the question is asked.
                    ScanRow(
                        title = Wording.DETAIL_LABEL,
                        detail = detailCeilingNote,
                        meta = detailLevel.displayName,
                        trailing = { Chevron() },
                        onClick = { sheet = SettingsSheet.DETAIL },
                        modifier = Modifier.testTag("settingsDetailRow"),
                    )
                }
                add {
                    ScanSwitchRow(
                        // was: "Vibrate and beep during a scan" over 78 words.
                        title = "Vibrate and beep",
                        detail = "The phone faces away while you walk.",
                        checked = settings.operatorCuesEnabled,
                        onCheckedChange = onOperatorCuesChange,
                        modifier = Modifier.testTag("operatorCuesRow"),
                    )
                }
                add {
                    // ROUND 13's finding survives in one clause: a buzz shakes
                    // the IMU, so this protects the measurement, not your
                    // concentration.
                    ScanSwitchRow(
                        title = "Silence notifications",
                        detail = "A buzz shakes the sensor and blurs the scan.",
                        checked = settings.dndDuringCapture,
                        onCheckedChange = onDndDuringCaptureChange,
                        modifier = Modifier.testTag("dndDuringCaptureRow"),
                    )
                }
                // ROUND 14 (owner item 52): where you stand, and the way in.
                // The switch shipped in 0.8.0 stating a prerequisite nothing in
                // the app could satisfy or report. Not `bad` when ungranted —
                // §C.6: an unavailable capability is ink-mute or warn, and red
                // means an operation failed and the operator lost something.
                if (settings.dndDuringCapture) {
                    add {
                        ScanRow(
                            // ROUND 28 item 164: `accessStatus`'s two clauses,
                            // split. As one string it ellipsised into
                            // "Access not granted. Scans run unp…" — a warning
                            // that cannot be finished, beside the button that
                            // fixes it. See `CaptureFocus.accessState`.
                            title = com.lidarscan.core.capture.CaptureFocus.accessState(dndAccessGranted),
                            detail = com.lidarscan.core.capture.CaptureFocus.accessConsequence(dndAccessGranted),
                            titleColor = if (dndAccessGranted) ScanColors.inkMute else ScanColors.warn,
                            trailing = {
                                if (!dndAccessGranted) {
                                    SecondaryPill(
                                        text = "Allow",
                                        onClick = onGrantDndAccess,
                                        modifier = Modifier.testTag("dndGrantButton"),
                                    )
                                }
                            },
                            modifier = Modifier.testTag("dndAccessStatus"),
                        )
                    }
                }
                add {
                    // ROUND 22 item 97: the one Advanced-features switch,
                    // default OFF, so a fresh install opens the simple app.
                    ScanSwitchRow(
                        title = Wording.ADVANCED_TITLE,
                        detail = Wording.ADVANCED_DETAIL,
                        checked = settings.advancedFeatures,
                        onCheckedChange = onAdvancedFeaturesChange,
                        modifier = Modifier.testTag("advancedFeaturesSwitch"),
                    )
                }
                if (settings.advancedFeatures) {
                    add {
                        // ROUND 24 item 113: the cloud server lives WITH the
                        // switch that turns cloud processing on. It is also the
                        // pair of fields the Profile page's sender reads, which
                        // is why it is behind Advanced and not behind developer
                        // mode.
                        ScanRow(
                            title = "Cloud server",
                            meta = SettingsFormat.cloudLine(settings.cloudBaseUrl),
                            trailing = { Chevron() },
                            onClick = { sheet = SettingsSheet.CLOUD },
                            modifier = Modifier.testTag("settingsCloudRow"),
                        )
                    }
                }
                // ROUND 10 (owner item 39): with colorization paused this row
                // renders nothing at all, and a heading with nothing under it
                // is the clutter the owner keeps asking us to remove.
                if (com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED && settings.advancedFeatures) {
                    add {
                        ScanSwitchRow(
                            title = "Colorize with poor clock sync",
                            detail = "Worth looking at, not worth quoting.",
                            checked = settings.allowPoorSyncColorize,
                            onCheckedChange = onAllowPoorSyncChange,
                            modifier = Modifier.testTag("allowPoorSyncRow"),
                        )
                    }
                }
            }
        }

        // ── STORAGE ─────────────────────────────────────────────────────────
        //
        // §D.7's sketch puts the Storage row in About. It is here instead, with
        // the two controls that change the number it reports: a figure and the
        // thing that reduces it belong in one card, and "8.1 GB · 66 scans"
        // directly above "Empty scans · 3 · Clean up" is the one arrangement in
        // which both rows are self-explanatory.
        SectionLabel("Storage")
        SettingsCard {
            buildList<@Composable () -> Unit> {
                add {
                    ScanRow(
                        title = "Storage",
                        meta = SettingsFormat.storageLine(storageBytes, scanCount),
                        modifier = Modifier.testTag("settingsStorageRow"),
                    )
                }
                add {
                    // ROUND 9 item 33: a capture that received no packets used
                    // to be kept, which is defensible once and intolerable by
                    // the fifteenth time.
                    ScanSwitchRow(
                        title = "Keep empty scans",
                        detail = "Off: a scan with no points is deleted.",
                        checked = settings.keepEmptyScans,
                        onCheckedChange = onKeepEmptyScansChange,
                        modifier = Modifier.testTag("keepEmptyScansRow"),
                    )
                }
                add {
                    // The count is live rather than a promise: a "clean up"
                    // button that cannot say how many is asking for a blind tap.
                    ScanRow(
                        title = "Empty scans",
                        detail = if (emptyScanCount == 0) null else "Deleting cannot be undone.",
                        meta = SettingsFormat.emptyScanLine(emptyScanCount),
                        trailing = {
                            SecondaryPill(
                                text = "Clean up",
                                enabled = emptyScanCount > 0,
                                onClick = onCleanUpEmptyScans,
                                modifier = Modifier.testTag("cleanUpEmptyScansButton"),
                            )
                        },
                        modifier = Modifier.testTag("emptyScanCount"),
                    )
                }
                if (emptyScanNote != null) {
                    add {
                        // §C.6: success is an inline row at the point of action,
                        // never a toast floating over the list it describes.
                        NoteRow(emptyScanNote, "emptyScanCleanupNote", onDismissEmptyScanNote)
                    }
                }
            }
        }

        // ── ABOUT ───────────────────────────────────────────────────────────
        SectionLabel("About")
        // ROUND 17 item 66: seven taps on the version row unlocks the Developer
        // section. Android's own idiom, used for its own reason — a setting
        // nobody can reach by accident needs no warning beside it. The counter
        // is remembered against `developerMode` so re-locking starts the count
        // over rather than re-unlocking on the next single tap.
        var versionTaps by remember(settings.developerMode) { mutableStateOf(0) }
        SettingsCard {
            listOf<@Composable () -> Unit>(
                {
                    // ROUND 24 item 110(b): opens the Scan tab and starts the
                    // tour there, because a tour of a screen has to happen ON
                    // the screen.
                    ScanRow(
                        title = com.lidarscan.core.capture.ScanTutorial.SETTINGS_ROW,
                        detail = com.lidarscan.core.capture.ScanTutorial.SETTINGS_DETAIL,
                        trailing = { Chevron() },
                        onClick = onReplayTutorial,
                        modifier = Modifier.testTag("settingsTutorialRow"),
                    )
                },
                {
                    // ROUND 24 item 109: the second door to the Profile page —
                    // the first is the Projects avatar. §D.8 moved the version,
                    // the storage figure and Send logs onto that page, so this
                    // row is now about support rather than about "settings you
                    // could not fit above".
                    ScanRow(
                        title = "Profile",
                        detail = "This phone, diagnostics, feedback.",
                        trailing = { Chevron() },
                        onClick = onOpenProfile,
                        modifier = Modifier.testTag("settingsProfileRow"),
                    )
                },
                {
                    // ROUND 17 item 67: the app holds the rear camera open for
                    // every second of every walk, because that is what places
                    // each lidar return. An honesty statement is not clutter;
                    // it is two facts — it runs, and nothing is saved.
                    ScanRow(
                        title = "Camera is used for tracking",
                        detail = "No images are saved. Nothing leaves.",
                        modifier = Modifier.testTag("cameraHonestyCard"),
                    )
                },
                {
                    // T4: was `outline` at roughly 1.5:1. It is `inkMute` mono
                    // in a row now, and it is still the seven-tap unlock.
                    //
                    // `mergeDescendants`: the emulator suite reads this node's
                    // text to assert the shipped version, and an unmerged row
                    // carries its text on its children instead.
                    ScanRow(
                        title = "Version",
                        meta = SettingsFormat.versionLine(
                            com.lidarscan.app.BuildConfig.VERSION_NAME,
                            com.lidarscan.app.BuildConfig.VERSION_CODE,
                            settings.developerMode,
                        ),
                        onClick = {
                            versionTaps += 1
                            if (versionTaps >= DEVELOPER_UNLOCK_TAPS) {
                                versionTaps = 0
                                onDeveloperModeChange(!settings.developerMode)
                            }
                        },
                        modifier = Modifier
                            .testTag("app_version_footer")
                            .semantics(mergeDescendants = true) {},
                    )
                },
            )
        }

        // ── DEVELOPER ───────────────────────────────────────────────────────
        //
        // ROUND 28 item 164, from the owner-approved mockup. Every card that
        // used to live down here — the connection sweep, the capture log, the
        // D6 timing slider, the engine switch, the synthetic replay, the
        // workflow-profile reference and the two raw paths — is a row in one
        // card. This is a re-presentation, not a re-plumb: each row calls the
        // same ViewModel function its card called.
        if (settings.developerMode) {
            SectionLabel("Developer")
            SettingsCard {
                buildList<@Composable () -> Unit> {
                    add {
                        // ROUND 25 item 118 (owner amendment): one full sweep,
                        // on demand, for whoever is standing there with the hub
                        // in their hand.
                        ScanRow(
                            title = "Connection debug",
                            meta = if (connectionDebugRunning) "sweeping…" else "run sweep",
                            trailing = { Chevron() },
                            onClick = { sheet = SettingsSheet.CONNECTION_DEBUG },
                            modifier = Modifier.testTag("connectionDebugRow"),
                        )
                    }
                    add {
                        CaptureLogRow(
                            captureLogSizeBytes = captureLogSizeBytes,
                            captureLogLastLine = captureLogLastLine,
                            onShareCaptureLog = onShareCaptureLog,
                        )
                    }
                    if (exportNote != null) {
                        add {
                            // ROUND 7: no user-triggered file operation ends
                            // silently, and the failure says where the file is.
                            NoteRow(exportNote, "captureLogExportNote", onDismissExportNote)
                        }
                    }
                    add {
                        ScanRow(
                            title = "Sensor timing",
                            meta = SettingsFormat.sensorTimingLine(settings.d6SensorLatencyMs),
                            trailing = { Chevron() },
                            onClick = { sheet = SettingsSheet.SENSOR_TIMING },
                            modifier = Modifier.testTag("settingsSensorTimingRow"),
                        )
                    }
                    add {
                        ScanRow(
                            title = "Engine",
                            meta = SettingsFormat.engineLine(
                                nativeAvailable = nativeEngineAvailable,
                                useFakeEngine = settings.useFakeEngine,
                                abi = engineAbi,
                            ),
                            modifier = Modifier.testTag("settingsEngineRow"),
                        )
                    }
                    add {
                        ScanSwitchRow(
                            title = "Use simulated engine",
                            detail = if (nativeEngineAvailable) {
                                "Takes effect after restarting the app."
                            } else {
                                "No native library — always simulated."
                            },
                            checked = settings.useFakeEngine || !nativeEngineAvailable,
                            enabled = nativeEngineAvailable,
                            onCheckedChange = onUseFakeEngineChange,
                            modifier = Modifier.testTag("useFakeEngineRow"),
                        )
                    }
                    add {
                        // B4's acceptance path: drives the full Capture screen
                        // from the bundled synthetic D6 capture via
                        // ReplayEngineBridge — no hardware needed. The testTag
                        // is what the CI emulator smoke test taps.
                        ScanRow(
                            title = "Replay synthetic scan",
                            detail = if (nativeEngineAvailable) {
                                null
                            } else {
                                "Needs the native library, same as capture."
                            },
                            trailing = { Chevron() },
                            onClick = onReplaySyntheticCapture,
                            modifier = Modifier.testTag("replaySyntheticCaptureButton"),
                        )
                    }
                    add {
                        ScanRow(
                            title = "Workflow profiles",
                            meta = "${WorkflowProfile.entries.size}",
                            trailing = { Chevron() },
                            onClick = { sheet = SettingsSheet.WORKFLOW_PROFILES },
                            modifier = Modifier.testTag("workflowProfilesRow"),
                        )
                    }
                    add {
                        ScanSwitchRow(
                            title = "Per-capture debug log",
                            detail = "Writes a debug log inside each scan.",
                            checked = settings.captureDebugLog,
                            onCheckedChange = onCaptureDebugLogChange,
                            modifier = Modifier.testTag("captureDebugLogRow"),
                        )
                    }
                    add {
                        // T1's path, in the only place it was ever evidence.
                        PathRow(
                            title = "Scans folder",
                            path = storageLocation,
                            pathTag = "storageLocationPath",
                        )
                    }
                    add {
                        PathRow(
                            title = "Log file",
                            path = captureLogPath,
                            pathTag = "captureLogPath",
                            trailing = {
                                SecondaryPill(
                                    text = "Clear",
                                    onClick = onClearCaptureLog,
                                    modifier = Modifier.testTag("clearCaptureLogButton"),
                                )
                            },
                        )
                    }
                }
            }
        }

        // T7: the last card used to be followed by about 350 px of nothing.
        // What is left is the tab bar's own clearance and the section gap that
        // every other card on the page already carries above it.
        Spacer(Modifier.height(ScanDims.SectionGap))
        Spacer(Modifier.height(ScanDims.TabBarClearance))
    }

    // ── the sheets ──────────────────────────────────────────────────────────
    when (sheet) {
        null -> Unit
        SettingsSheet.UNITS -> OptionSheet(
            title = "Units",
            options = Units.entries.map { it to it.displayName },
            selected = settings.units,
            onSelect = onUnitsChange,
            onDismiss = { sheet = null },
            testTag = "unitsSheet",
        )
        SettingsSheet.THEME -> OptionSheet(
            title = "Theme",
            options = ThemeMode.entries.map { it to it.displayName },
            selected = settings.themeMode,
            onSelect = onThemeModeChange,
            onDismiss = { sheet = null },
            testTag = "themeSheet",
        )
        SettingsSheet.DETAIL -> OptionSheet(
            title = Wording.DETAIL_LABEL,
            options = detailLevels.map { it to it.displayName },
            selected = if (detailLevel in detailLevels) detailLevel else detailLevels.first(),
            onSelect = onDetailChange,
            onDismiss = { sheet = null },
            note = detailCeilingNote ?: Wording.DETAIL_BUDGET_HINT,
            testTag = "detailSheet",
        )
        SettingsSheet.MOUNT -> MountSheet(
            leverArm = settings.mountLeverArm,
            storedTrim = storedMountTrim,
            autoLevelSuggestion = settings.mountAutoLevelSuggestion,
            onChange = onMountLeverArmChange,
            onReset = onMountLeverArmReset,
            onDismiss = { sheet = null },
        )
        SettingsSheet.CLOUD -> CloudSheet(
            baseUrl = settings.cloudBaseUrl,
            token = settings.cloudToken,
            onChange = onCloudChange,
            onDismiss = { sheet = null },
        )
        SettingsSheet.CONNECTION_DEBUG -> ConnectionDebugSheet(
            output = connectionDebugOutput,
            running = connectionDebugRunning,
            onRun = onRunConnectionDebug,
            onDismiss = { sheet = null },
        )
        SettingsSheet.SENSOR_TIMING -> SensorTimingSheet(
            latencyMs = settings.d6SensorLatencyMs,
            onChange = onD6SensorLatencyChange,
            onDismiss = { sheet = null },
        )
        SettingsSheet.WORKFLOW_PROFILES -> WorkflowProfilesSheet(onDismiss = { sheet = null })
    }
}

/** ROUND 17 item 66: Android's own number, for Android's own reason. */
private const val DEVELOPER_UNLOCK_TAPS = 7

/**
 * The one sheet that can be open, or none.
 *
 * ROUND 28 item 164: eight cards became eight rows, and the control each card
 * carried had to go somewhere. A sheet is the only container that can hold a
 * three-field form or a twenty-line diagnostic dump without putting it on the
 * page an operator opens to change the theme.
 */
private enum class SettingsSheet {
    UNITS, THEME, DETAIL, MOUNT, CLOUD, CONNECTION_DEBUG, SENSOR_TIMING, WORKFLOW_PROFILES
}

/**
 * §C.4's card of rows at the page's own margin.
 *
 * [ScanRowCard] draws the hairlines and refuses to pad its rows (they carry
 * their own 16 dp); this adds the screen margin, which [SectionLabel] already
 * carries for itself.
 */
@Composable
private fun SettingsCard(rows: @Composable () -> List<@Composable () -> Unit>) {
    ScanRowCard(
        modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin),
        rows = rows(),
    )
}

/**
 * T6 — **a chevron means this row goes somewhere.**
 *
 * The old page had one chevron on `Tutorial` and none on four other cards that
 * looked identical to it, two of which did nothing at all. Every row that opens
 * a sheet or a screen draws this; no row that does not, draws it.
 */
@Composable
private fun Chevron() {
    Icon(
        Icons.AutoMirrored.Filled.KeyboardArrowRight,
        contentDescription = null,
        tint = ScanColors.inkFaint,
    )
}

/**
 * T5 — **a switch that is off must not look broken.**
 *
 * Material 3's default unchecked track is `surfaceVariant` with a
 * `outline` thumb, which on this app's card ground is grey on grey at about
 * 1.3:1 — the review read the Advanced-features switch as *disabled* rather
 * than *off*, and so did the owner. The off state is now the design system's
 * trough with an ink thumb and a line border, which is the same treatment the
 * segmented pill's unselected segments get.
 *
 * The `enabled = false` case keeps `inkFaint`, because there the grey IS the
 * message.
 */
@Composable
private fun ScanSwitchRow(
    title: String,
    detail: String?,
    checked: Boolean,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onCheckedChange: (Boolean) -> Unit,
) {
    ScanRow(
        title = title,
        detail = detail,
        modifier = modifier,
        trailing = {
            Switch(
                checked = checked,
                enabled = enabled,
                onCheckedChange = onCheckedChange,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = ScanColors.onPrimary,
                    checkedTrackColor = ScanColors.primary,
                    checkedBorderColor = ScanColors.primary,
                    uncheckedThumbColor = ScanColors.ink,
                    uncheckedTrackColor = ScanColors.trough,
                    uncheckedBorderColor = ScanColors.line,
                    disabledUncheckedThumbColor = ScanColors.inkFaint,
                    disabledUncheckedTrackColor = ScanColors.trough,
                ),
            )
        },
    )
}

/**
 * A developer row whose value is a filesystem path.
 *
 * Mono and selectable-looking rather than a [ScanRow] `meta`, because a path is
 * three lines long and the whole point of it is that somebody can read it
 * character by character against an `adb shell ls`. This is the ONLY place in
 * the app a path is drawn (T1), and it is behind seven taps.
 */
@Composable
private fun PathRow(
    title: String,
    path: String,
    pathTag: String,
    trailing: @Composable (() -> Unit)? = null,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S2),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = ScanBody, color = ScanColors.ink)
            Text(path, style = ScanMeta, color = ScanColors.inkMute, modifier = Modifier.testTag(pathTag))
        }
        trailing?.invoke()
    }
}

/**
 * The one-line outcome of a file operation, as a row inside the card that
 * started it.
 *
 * §C.6: a success is an **inline row at the point of action**, never a floating
 * toast over the list it describes. Not a [ScanRow]: these sentences come from
 * the ViewModel and name a destination path, so they wrap rather than ellipsise
 * — a "Saved to …" that ends in a "…" has told the operator nothing. Tapping
 * dismisses it, which is how it left the screen before and is still the only
 * gesture anyone tries.
 */
@Composable
private fun NoteRow(text: String, testTag: String, onDismiss: () -> Unit) {
    Text(
        text,
        style = ScanMeta,
        color = ScanColors.inkMute,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onDismiss)
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S3)
            .testTag(testTag),
    )
}

/**
 * The developer capture-log row: how big the log is, what it last wrote, and
 * one button that gets it off the phone.
 *
 * ROUND 28 item 168's "capture log Export must be reachable" is this button.
 * The ordinary operator has a better door — Profile › **Send diagnostics**,
 * which packages the same log with the device summary and delivers it — and
 * this is the developer's version: raw file, share sheet, no wrapper.
 *
 * The live tail is kept from ROUND 6 item 20: a size alone cannot distinguish
 * "logging works" from "logging stopped an hour ago and the file is still on
 * disk".
 */
@Composable
private fun CaptureLogRow(
    captureLogSizeBytes: Long,
    captureLogLastLine: String?,
    onShareCaptureLog: () -> Unit,
) {
    ScanRow(
        title = "Capture log",
        detail = captureLogLastLine,
        meta = SettingsFormat.captureLogLine(captureLogSizeBytes),
        trailing = {
            SecondaryPill(
                text = "Export",
                onClick = onShareCaptureLog,
                modifier = Modifier.testTag("exportCaptureLogButton"),
            )
        },
        modifier = Modifier.testTag("captureLogRow"),
    )
}

// ── sheets ──────────────────────────────────────────────────────────────────

/**
 * The one sheet frame: grabber, [ScanTitle] head, content, the app's sheet
 * radius and the card ground.
 *
 * Hand-built for the reason ROUND 16 item 61 established — Material hands
 * `ModalBottomSheet` the app's PILL `extraLarge` shape, and 50 % of a full-width
 * sheet's short side is an enormous curve.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SettingsSheetFrame(
    title: String,
    onDismiss: () -> Unit,
    testTag: String,
    content: @Composable () -> Unit,
) {
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        containerColor = ScanColors.card,
        contentColor = ScanColors.ink,
        shape = androidx.compose.foundation.shape.RoundedCornerShape(
            topStart = ScanDims.SheetRadius,
            topEnd = ScanDims.SheetRadius,
        ),
        modifier = Modifier.testTag(testTag),
    ) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(bottom = ScanDims.S8)
                .navigationBarsPadding(),
        ) {
            Text(
                title,
                style = ScanTitle,
                color = ScanColors.ink,
                modifier = Modifier.padding(
                    start = ScanDims.ScreenMargin,
                    end = ScanDims.ScreenMargin,
                    bottom = ScanDims.S3,
                ),
            )
            content()
        }
    }
}

/**
 * The picker behind `Units`, `Theme` and `Detail`.
 *
 * Rows with a tick rather than a segmented pill: the pill is a control for a
 * value that is ALWAYS on screen (the Scan sheet's view mode, the export
 * format), and here the value is already on the row that opened this. A list of
 * rows also survives a fourth option, which a three-segment pill does not.
 */
@Composable
private fun <T> OptionSheet(
    title: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    onDismiss: () -> Unit,
    testTag: String,
    note: String? = null,
) {
    SettingsSheetFrame(title = title, onDismiss = onDismiss, testTag = testTag) {
        Column {
            ScanRowCard(
                modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin),
                rows = options.map { (value, label) ->
                    {
                        ScanRow(
                            title = label,
                            trailing = {
                                if (value == selected) {
                                    Icon(
                                        Icons.Filled.Check,
                                        contentDescription = null,
                                        tint = ScanColors.primaryInk,
                                    )
                                }
                            },
                            onClick = {
                                onSelect(value)
                                onDismiss()
                            },
                        )
                    }
                },
            )
            if (note != null) {
                Text(
                    note,
                    style = ScanMeta,
                    color = ScanColors.inkMute,
                    modifier = Modifier.padding(
                        start = ScanDims.ScreenMargin,
                        end = ScanDims.ScreenMargin,
                        top = ScanDims.S2,
                    ),
                )
            }
        }
    }
}

/**
 * ROUND 20 (item 82) — **the per-device mount profile**, unchanged in function
 * and moved off the page (ROUND 28 item 164, T3).
 *
 * The owner's mandate: the D6's position on the phone back VARIES and the app
 * will be public, so nothing about the mount is hard-coded. The ROTATION is
 * measured at every Start by the hold-steady stage (item 78, gravity-referenced
 * per item 79) and reported on the row that opens this; the LEVER ARM is these
 * three centimetre fields; the one thing still assumed is the documented
 * convention the schematic states. An auto-level result (item 80) appears as a
 * SUGGESTION with provenance — never silently applied.
 */
@Composable
private fun MountSheet(
    leverArm: com.lidarscan.core.calib.MountLeverArm,
    storedTrim: com.lidarscan.core.calib.StoredMountTrim?,
    autoLevelSuggestion: String?,
    onChange: (Double, Double, Double) -> Unit,
    onReset: () -> Unit,
    onDismiss: () -> Unit,
) {
    SettingsSheetFrame(title = "Where the D6 sits", onDismiss = onDismiss, testTag = "mountSheet") {
        Column(Modifier.padding(horizontal = ScanDims.ScreenMargin)) {
            Text(
                SettingsFormat.mountLine(storedTrim?.trim?.magnitudeDeg),
                style = ScanMeta,
                color = ScanColors.inkMute,
                modifier = Modifier.testTag("mountProfileRotation"),
            )
            Spacer(Modifier.height(ScanDims.S3))
            // Local text state, committed on change when parseable; unparseable
            // text simply does not commit — the same tolerance every free-text
            // field in this app has.
            var up by remember(leverArm) { mutableStateOf("%.1f".format(leverArm.upCm)) }
            var behind by remember(leverArm) { mutableStateOf("%.1f".format(leverArm.behindCm)) }
            var right by remember(leverArm) { mutableStateOf("%.1f".format(leverArm.rightCm)) }
            fun commit() {
                val u = up.toDoubleOrNull()
                val b = behind.toDoubleOrNull()
                val r = right.toDoubleOrNull()
                if (u != null && b != null && r != null) onChange(u, b, r)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(ScanDims.S2)) {
                OutlinedTextField(
                    value = up,
                    onValueChange = { up = it; commit() },
                    label = { Text("Up (cm)") },
                    singleLine = true,
                    modifier = Modifier.weight(1f).testTag("leverUpField"),
                )
                OutlinedTextField(
                    value = behind,
                    onValueChange = { behind = it; commit() },
                    label = { Text("Behind (cm)") },
                    singleLine = true,
                    modifier = Modifier.weight(1f).testTag("leverBehindField"),
                )
                OutlinedTextField(
                    value = right,
                    onValueChange = { right = it; commit() },
                    label = { Text("Right (cm)") },
                    singleLine = true,
                    modifier = Modifier.weight(1f).testTag("leverRightField"),
                )
            }
            Spacer(Modifier.height(ScanDims.S2))
            // The provenance stays — it is a fact about THIS phone's numbers.
            Text(
                "Distance from the rear camera, in centimetres. Source: ${leverArm.provenance}.",
                style = ScanMeta,
                color = ScanColors.inkMute,
            )
            Spacer(Modifier.height(ScanDims.S2))
            // The schematic: the one assumed convention, stated.
            Text(
                "┌──────────┐   phone back, seen from behind\n" +
                    "│  ┌─D6─┐  │   0° mark points UP\n" +
                    "│  │ ◉  │  │   cap faces FORWARD (the walk)\n" +
                    "│  └────┘  │   everything else: measured\n" +
                    "└──────────┘   or typed above",
                style = ScanMeta,
                color = ScanColors.inkMute,
                modifier = Modifier.testTag("mountConventionSchematic"),
            )
            if (autoLevelSuggestion != null) {
                Spacer(Modifier.height(ScanDims.S2))
                Text(
                    "$autoLevelSuggestion A suggestion only, never applied by itself.",
                    style = ScanMeta,
                    color = ScanColors.sensorD6,
                    modifier = Modifier.testTag("mountAutoLevelSuggestion"),
                )
            }
            Spacer(Modifier.height(ScanDims.S3))
            SecondaryPill(
                text = "Reset offsets",
                onClick = onReset,
                modifier = Modifier.fillMaxWidth().testTag("leverArmResetButton"),
            )
        }
    }
}

/** D3 — where the Cloud processing mode uploads to, and where a log bundle is POSTed. */
@Composable
private fun CloudSheet(
    baseUrl: String,
    token: String,
    onChange: (String, String) -> Unit,
    onDismiss: () -> Unit,
) {
    var url by remember(baseUrl) { mutableStateOf(baseUrl) }
    var tok by remember(token) { mutableStateOf(token) }
    SettingsSheetFrame(title = "Cloud server", onDismiss = onDismiss, testTag = "cloudSheet") {
        Column(Modifier.padding(horizontal = ScanDims.ScreenMargin)) {
            OutlinedTextField(
                value = url,
                onValueChange = { url = it },
                label = { Text("Server URL") },
                placeholder = { Text("https://cloud.example.com") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().testTag("cloudUrlField"),
            )
            Spacer(Modifier.height(ScanDims.S3))
            OutlinedTextField(
                value = tok,
                onValueChange = { tok = it },
                label = { Text("Token") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth().testTag("cloudTokenField"),
            )
            Spacer(Modifier.height(ScanDims.S2))
            // ROUND 24 item 113: the limitation it states is real and stays;
            // the design-document citation it used to carry does not.
            Text(
                "Used for cloud processing and for sending logs.\n" +
                    "The token is stored unencrypted on this phone.",
                style = ScanMeta,
                color = ScanColors.inkMute,
            )
            Spacer(Modifier.height(ScanDims.S3))
            SecondaryPill(
                text = "Save",
                onClick = {
                    onChange(url, tok)
                    onDismiss()
                },
                modifier = Modifier.fillMaxWidth().testTag("cloudSaveButton"),
            )
        }
    }
}

/**
 * ROUND 25 item 118, **owner amendment** — the connection sweep, on a surface
 * of its own.
 *
 * ## Why it is not on the Settings page
 *
 * The block is twenty lines of hex and interface names. Inline it made the
 * Settings page as long as the USB device list, which is why it shipped with a
 * fixed-height inner scroll — a scroll inside a scroll, the one gesture
 * conflict a phone cannot resolve. A sheet gives it the whole screen and gives
 * the page back its rhythm.
 *
 * Copy is not a nicety. Nobody transcribes a hex dump off a phone screen, and a
 * diagnostic that cannot leave the device it diagnoses is a diagnostic nobody
 * sends you.
 *
 * ## Wording law
 *
 * The block in [output] is **exempt** from [com.lidarscan.core.WordingLaw] and
 * this is deliberate. It is developer-mode diagnostic output — reachable only
 * after seven taps on the version row, never shown to an operator — and it is
 * supposed to be dense with numbers, the same way
 * `StartHoldTrimGate.refusalLogLine` is. Do NOT rewrite it into six-word
 * instructions; the six-word law governs the Mid-360 wizard
 * ([com.lidarscan.core.net.Mid360Diagnosis]'s strings), a different surface read
 * by a different person.
 */
@Composable
private fun ConnectionDebugSheet(
    output: String?,
    running: Boolean,
    onRun: () -> Unit,
    onDismiss: () -> Unit,
) {
    val clipboard = androidx.compose.ui.platform.LocalClipboardManager.current
    SettingsSheetFrame(title = "Connection debug", onDismiss = onDismiss, testTag = "connectionDebugSheet") {
        Column(Modifier.padding(horizontal = ScanDims.ScreenMargin)) {
            Text(
                output ?: "USB, interfaces and discovery, in one sweep.",
                style = ScanMeta,
                color = if (output == null) ScanColors.inkMute else ScanColors.ink,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(if (output == null) ScanDims.Row else SWEEP_OUTPUT_HEIGHT)
                    .verticalScroll(rememberScrollState())
                    // Horizontal scroll as well: a USB descriptor line with four
                    // interfaces on it is long, and wrapping hex at the screen
                    // edge is how a readable dump becomes an unreadable one.
                    .horizontalScroll(rememberScrollState())
                    .testTag("connectionDebugOutput"),
            )
            Spacer(Modifier.height(ScanDims.S3))
            Row(horizontalArrangement = Arrangement.spacedBy(ScanDims.S2)) {
                SecondaryPill(
                    text = if (running) "Sweeping…" else "Run sweep",
                    onClick = onRun,
                    modifier = Modifier.weight(1f).testTag("connectionDebugSweep"),
                )
                SecondaryPill(
                    text = "Copy",
                    onClick = {
                        // No-op with nothing to copy, rather than putting an
                        // empty clipboard in front of someone who is about to
                        // paste it into a bug report.
                        output?.let { clipboard.setText(androidx.compose.ui.text.AnnotatedString(it)) }
                    },
                    modifier = Modifier.weight(1f).testTag("connectionDebugCopy"),
                )
            }
        }
    }
}

/** Tall enough for a verdict plus the USB section without swallowing the sheet. */
private val SWEEP_OUTPUT_HEIGHT = 280.dp

/**
 * ROUND 7 — the one D6 timing number that cannot be derived, exposed.
 *
 * Everything else about the D6's clock is handled without asking: the app
 * stamps each USB read with `elapsedRealtimeNanos()` (the same CLOCK_BOOTTIME
 * the engine and ARCore both use) and the engine back-dates every return inside
 * that read from its byte position at a known baud. What is left is a constant
 * transport delay, and a constant delay translates the whole cloud along the
 * walk and bends corners while turning.
 *
 * The read-out says what the number MEANS in centimetres, because "2 ms" is not
 * a quantity anyone can judge and "2 mm at walking pace" is.
 */
@Composable
private fun SensorTimingSheet(
    latencyMs: Int,
    onChange: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    SettingsSheetFrame(title = "Sensor timing", onDismiss = onDismiss, testTag = "sensorTimingSheet") {
        Column(Modifier.padding(horizontal = ScanDims.ScreenMargin)) {
            Text(
                com.lidarscan.core.capture.D6TimeSync.describe(latencyMs),
                style = ScanBody,
                color = ScanColors.ink,
                modifier = Modifier.testTag("d6SensorLatencyValue"),
            )
            Slider(
                value = latencyMs.toFloat(),
                onValueChange = { onChange(it.roundToInt()) },
                valueRange = com.lidarscan.core.capture.D6TimeSync.MIN_SENSOR_LATENCY_MS.toFloat()..
                    com.lidarscan.core.capture.D6TimeSync.MAX_SENSOR_LATENCY_MS.toFloat(),
                modifier = Modifier.testTag("d6SensorLatencySlider"),
            )
            Text(
                "The constant delay between a byte reaching the D6 cable and this phone reading its " +
                    "clock. The default (" +
                    "${com.lidarscan.core.capture.D6TimeSync.DEFAULT_SENSOR_LATENCY_MS} ms) is derived " +
                    "from one USB bulk frame plus one thread wake-up, not measured on your phone — if " +
                    "walls land consistently in front of or behind where they are, this is the knob. " +
                    "Applies to the next chunk of data, no reconnect needed.",
                style = ScanMeta,
                color = ScanColors.inkMute,
            )
        }
    }
}

/**
 * B5 — what each workflow profile actually sets, surfaced where a user can read
 * it (Tech Spec §3.9's "profiles set defaults").
 *
 * Deliberately a READ-ONLY reference rather than an editor. The defaults are
 * applied at project creation and then belong to the project
 * (`ProjectManifest.captureDefaults`), so a device-level editor here would
 * change nothing about any existing capture while looking like it did — and the
 * settings that matter per project are already editable where they are used.
 */
@Composable
private fun WorkflowProfilesSheet(onDismiss: () -> Unit) {
    SettingsSheetFrame(title = "Workflow profiles", onDismiss = onDismiss, testTag = "workflowProfilesSheet") {
        ScanRowCard(
            modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin),
            rows = WorkflowProfile.entries.map { profile ->
                {
                    val d = CaptureDefaults.forProfile(profile)
                    ScanRow(
                        title = profile.displayName,
                        detail = listOf(
                            if (d.liveSlam) "Live-SLAM" else "Record-only",
                            "export ${d.exportFormat.displayName}",
                            "display ${d.displayProfile.displayName}",
                            if (d.requireRtkFixForCapture) "blocks below ${d.minFixForCapture.label}" else "no RTK gate",
                        ).joinToString(" · "),
                    )
                }
            },
        )
    }
}
