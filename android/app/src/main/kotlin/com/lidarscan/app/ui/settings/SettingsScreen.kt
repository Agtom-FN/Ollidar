package com.lidarscan.app.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.data.AppSettings
import com.lidarscan.core.Wording
import com.lidarscan.app.data.ThemeMode
import com.lidarscan.app.data.Units
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.HeroHeader
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.ScanCard
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.SegmentedPill
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.Ink
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.core.model.CaptureDefaults
import com.lidarscan.core.model.WorkflowProfile
import kotlin.math.roundToInt

@Composable
fun SettingsRoute(
    container: AppContainer,
    onBack: () -> Unit,
    onReplaySyntheticCapture: (projectId: String) -> Unit = {},
    /** ROUND 24 item 109/113: the Profile row at the top of the page. */
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
        captureLogPath = viewModel.captureLogPath,
        captureLogSizeBytes = captureLogSize,
        captureLogLastLine = captureLogLastLine,
        exportNote = exportNote,
        onDismissExportNote = viewModel::dismissExportNote,
        onShareCaptureLog = viewModel::shareCaptureLog,
        onClearCaptureLog = viewModel::clearCaptureLog,
        nativeEngineAvailable = com.lidarscan.app.engine.ScanEngineNative.isAvailable,
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
 * Settings, restyled to the redesign and otherwise unchanged.
 *
 * Every setting B1–D3 put here is still here and still does the same thing:
 * units, theme, the cloud server + token, the poor-clock-sync colorize policy,
 * the simulated-engine developer switch, the "Replay synthetic capture"
 * acceptance path (still tagged `replaySyntheticCaptureButton` for the
 * emulator smoke test), the read-only workflow-profile reference and the
 * storage location. What changed is the chrome: a hero title instead of a
 * `TopAppBar`, `ScanCard` panels, and the three-option rows became the same
 * segmented pill the rest of the app uses.
 *
 * Settings is a **tab** now, so it has no back arrow — the tab bar is the way
 * out, and [onBack] is what the system back gesture routes through.
 */
@Composable
fun SettingsScreen(
    settings: AppSettings,
    storageLocation: String,
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
    /** ROUND 17 item 66: the seven-tap unlock and the one thing behind it. */
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
    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .testTag("settingsScreen"),
    ) {
        HeroHeader(title = "Settings", subtitle = "this device")

        Column(
            Modifier.padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            // ── ROUND 24 item 109: PROFILE, first ───────────────────────────
            //
            // The version number, the storage figure and Send logs used to be
            // three separate cards in three separate sections of this page.
            // They are one page now, and this is its door — at the top,
            // because "what version am I on" is the most common reason anyone
            // opens Settings at all.
            SettingsSection("Profile") {
                ScanCard(onClick = onOpenProfile, modifier = Modifier.testTag("settingsProfileRow")) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            CardTitle("Profile")
                            Spacer(Modifier.height(4.dp))
                            Hint("Version, storage, send logs.", color = InkFaint)
                        }
                        Icon(
                            Icons.AutoMirrored.Filled.KeyboardArrowRight,
                            contentDescription = null,
                            tint = InkFaint,
                        )
                    }
                }
            }

            // ── SCANNING: the mount, the cues, the detail ───────────────────
            SettingsSection("Scanning") {
                MountProfileCard(
                    leverArm = settings.mountLeverArm,
                    storedTrim = storedMountTrim,
                    autoLevelSuggestion = settings.mountAutoLevelSuggestion,
                    onChange = onMountLeverArmChange,
                    onReset = onMountLeverArmReset,
                )
                Spacer(Modifier.height(12.dp))
                SoundsAndHapticsCard(
                    operatorCues = settings.operatorCuesEnabled,
                    dndDuringCapture = settings.dndDuringCapture,
                    dndAccessGranted = dndAccessGranted,
                    onOperatorCuesChange = onOperatorCuesChange,
                    onDndDuringCaptureChange = onDndDuringCaptureChange,
                    onGrantDndAccess = onGrantDndAccess,
                )
                Spacer(Modifier.height(12.dp))
                DetailCard(
                    levels = detailLevels,
                    selected = detailLevel,
                    ceilingNote = detailCeilingNote,
                    onChange = onDetailChange,
                )
            }

            // ── STORAGE ─────────────────────────────────────────────────────
            SettingsSection("Storage") {
                EmptyScansCard(
                    keepEmptyScans = settings.keepEmptyScans,
                    emptyScanCount = emptyScanCount,
                    note = emptyScanNote,
                    onKeepEmptyScansChange = onKeepEmptyScansChange,
                    onCleanUp = onCleanUpEmptyScans,
                    onDismissNote = onDismissEmptyScanNote,
                )
                Spacer(Modifier.height(12.dp))
                ScanCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Filled.Folder, contentDescription = null, tint = Ember)
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(
                                storageLocation,
                                style = MonoMeta,
                                color = MaterialTheme.colorScheme.onSurface,
                                modifier = Modifier.testTag("storageLocationPath"),
                            )
                            Spacer(Modifier.height(6.dp))
                            // was: "Where .lscan projects are stored on this
                            // device. A location picker is a future addition."
                            Hint("Where your scans are kept.", color = InkFaint)
                        }
                    }
                }
            }

            // ── DISPLAY: the two preferences that are genuinely preferences ──
            //
            // Units and Theme are neither developer settings nor advanced
            // features — they are what a person wants their app to look like,
            // and hiding them would be the simplification overshooting. Two
            // segmented pills in one card, where they were two sections.
            SettingsSection("Display") {
                ScanCard {
                    CardTitle("Units")
                    Spacer(Modifier.height(8.dp))
                    SegmentedPill(
                        options = Units.entries.map { it to it.displayName },
                        selected = settings.units,
                        onSelect = onUnitsChange,
                        height = 44.dp,
                    )
                    Spacer(Modifier.height(14.dp))
                    CardTitle("Theme")
                    Spacer(Modifier.height(8.dp))
                    SegmentedPill(
                        options = ThemeMode.entries.map { it to it.displayName },
                        selected = settings.themeMode,
                        onSelect = onThemeModeChange,
                        height = 44.dp,
                    )
                }
            }

            // ── ROUND 22 item 97: the one Advanced-features switch ──────────
            //
            // Default OFF, which means the app a fresh install opens is the
            // simple one. Nothing behind this switch is deleted.
            SettingsSection(Wording.ADVANCED_TITLE) {
                ScanCard {
                    ScanSwitchRow(
                        title = Wording.ADVANCED_TITLE,
                        detail = Wording.ADVANCED_DETAIL,
                        checked = settings.advancedFeatures,
                        onCheckedChange = onAdvancedFeaturesChange,
                        modifier = Modifier.testTag("advancedFeaturesSwitch"),
                    )
                }
                // ROUND 24 item 113: the cloud server lives WITH the switch
                // that turns cloud processing on, instead of being a section
                // of its own above the settings people actually use. It is
                // also the pair of fields ROUND 24 item 109's feedback sender
                // reads, which is why it is behind Advanced rather than behind
                // developer mode.
                if (settings.advancedFeatures) {
                    Spacer(Modifier.height(12.dp))
                    CloudCard(settings.cloudBaseUrl, settings.cloudToken, onCloudChange)
                }
                // ROUND 10 (owner item 39): with colorization paused this
                // renders nothing at all, and a heading with nothing under it
                // is the clutter the owner keeps asking us to remove.
                if (com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED && settings.advancedFeatures) {
                    Spacer(Modifier.height(12.dp))
                    ProcessingOptionsCard(settings.allowPoorSyncColorize, onAllowPoorSyncChange)
                }
            }

            // ── ABOUT: the tour, the camera sentence, the version ───────────
            SettingsSection("About") {
                // ROUND 24 item 110(b): replay the Scan-screen tour. Opens the
                // Scan tab and starts it there, because a tour of a screen has
                // to happen ON the screen.
                ScanCard(
                    onClick = onReplayTutorial,
                    modifier = Modifier.testTag("settingsTutorialRow"),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            CardTitle(com.lidarscan.core.capture.ScanTutorial.SETTINGS_ROW)
                            Spacer(Modifier.height(4.dp))
                            Hint(
                                com.lidarscan.core.capture.ScanTutorial.SETTINGS_DETAIL,
                                color = InkFaint,
                            )
                        }
                        Icon(
                            Icons.AutoMirrored.Filled.KeyboardArrowRight,
                            contentDescription = null,
                            tint = InkFaint,
                        )
                    }
                }
                Spacer(Modifier.height(12.dp))
                // ── ROUND 17 item 67: the camera sentence ────────────────────
                //
                // The app holds the rear camera open for every second of every
                // walk, because that is what places each lidar return. It is
                // kept — an honesty statement is not clutter — and shortened
                // to the two facts that matter: it runs, and nothing is saved.
                ScanCard {
                    CardTitle("Camera is used for tracking")
                    Spacer(Modifier.height(4.dp))
                    Hint(
                        "No images are saved. Nothing leaves your phone.",
                        modifier = Modifier.testTag("cameraHonestyCard"),
                    )
                }
            }

            // ── ROUND 17 item 66 / ROUND 24 item 113: DEVELOPER ─────────────
            //
            // Everything below is developer-only and now lives behind the
            // seven-tap unlock rather than in front of it: the simulated
            // engine and the synthetic replay, the capture log's path/export/
            // clear, the D6 sensor-latency slider, and the read-only workflow
            // profile reference. None of them is deleted, none of them changed,
            // and each returns exactly as it is the moment the version line is
            // tapped seven times.
            if (settings.developerMode) {
                SettingsSection("Developer") {
                    ScanCard {
                        ScanSwitchRow(
                            title = "Per-capture debug log",
                            detail = "Writes a debug log inside each scan's own bundle.",
                            checked = settings.captureDebugLog,
                            onCheckedChange = onCaptureDebugLogChange,
                            modifier = Modifier.testTag("captureDebugLogRow"),
                        )
                        Spacer(Modifier.height(8.dp))
                        Hint("Tap the version line seven times to lock this away.")
                    }
                    Spacer(Modifier.height(12.dp))
                    CaptureLogCard(
                        captureLogPath = captureLogPath,
                        captureLogSizeBytes = captureLogSizeBytes,
                        captureLogLastLine = captureLogLastLine,
                        exportNote = exportNote,
                        onShareCaptureLog = onShareCaptureLog,
                        onClearCaptureLog = onClearCaptureLog,
                        onDismissExportNote = onDismissExportNote,
                    )
                    Spacer(Modifier.height(12.dp))
                    ConnectionDebugCard(
                        output = connectionDebugOutput,
                        running = connectionDebugRunning,
                        onRun = onRunConnectionDebug,
                    )
                    Spacer(Modifier.height(12.dp))
                    D6TimingCard(settings.d6SensorLatencyMs, onD6SensorLatencyChange)
                    Spacer(Modifier.height(12.dp))
                    EngineCard(
                        useFakeEngine = settings.useFakeEngine,
                        nativeEngineAvailable = nativeEngineAvailable,
                        onUseFakeEngineChange = onUseFakeEngineChange,
                        onReplaySyntheticCapture = onReplaySyntheticCapture,
                    )
                    Spacer(Modifier.height(12.dp))
                    WorkflowProfilesCard()
                }
            }
        }

        Spacer(Modifier.height(16.dp))
        // ROUND 17 item 66: seven taps on the version line unlocks the
        // Developer section. Android's own idiom, used for its own reason — a
        // setting nobody can reach by accident needs no warning beside it. The
        // counter is remembered against `developerMode` so re-locking starts
        // the count over rather than re-unlocking on the next single tap.
        var versionTaps by remember(settings.developerMode) { mutableStateOf(0) }
        Text(
            "LidarScan v${com.lidarscan.app.BuildConfig.VERSION_NAME} " +
                "(build ${com.lidarscan.app.BuildConfig.VERSION_CODE})" +
                if (settings.developerMode) "  ·  developer" else "",
            style = MonoLabel,
            color = MaterialTheme.colorScheme.outline,
            modifier = Modifier
                .padding(horizontal = 16.dp)
                .testTag("app_version_footer")
                .clickable {
                    versionTaps += 1
                    if (versionTaps >= DEVELOPER_UNLOCK_TAPS) {
                        versionTaps = 0
                        onDeveloperModeChange(!settings.developerMode)
                    }
                },
        )

        Spacer(Modifier.height(ScanDims.TabBarClearance))
    }
}

/** ROUND 17 item 66: Android's own number, for Android's own reason. */
private const val DEVELOPER_UNLOCK_TAPS = 7

/**
 * ROUND 24 item 113 — **sounds & haptics, one card instead of a section.**
 *
 * Two switches that were 340 words of explanation between them. What survives
 * is what an operator needs at the moment they are deciding: the cues buzz
 * while you walk (and the phone faces away from you, which is why they exist),
 * and silencing notifications protects the measurement rather than your
 * concentration. The ROUND 14 grant row is unchanged — it is the one part of
 * the old block that was not prose but a working control.
 */
@Composable
private fun SoundsAndHapticsCard(
    operatorCues: Boolean,
    dndDuringCapture: Boolean,
    dndAccessGranted: Boolean,
    onOperatorCuesChange: (Boolean) -> Unit,
    onDndDuringCaptureChange: (Boolean) -> Unit,
    onGrantDndAccess: () -> Unit,
) {
    ScanCard {
        ScanSwitchRow(
            // was: "Vibrate and beep during a scan" over 78 words.
            title = "Vibrate and beep",
            detail = "The phone faces away while you walk. These are the hints.",
            checked = operatorCues,
            onCheckedChange = onOperatorCuesChange,
            modifier = Modifier.testTag("operatorCuesRow"),
        )
        Spacer(Modifier.height(14.dp))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Spacer(Modifier.height(14.dp))
        ScanSwitchRow(
            // was: "Silence notifications while scanning" over 84 words.
            // ROUND 13's finding survives in one clause: a buzz shakes the
            // IMU, so this protects the measurement, not your attention.
            title = "Silence notifications",
            detail = "A buzz shakes the sensor and blurs the scan.",
            checked = dndDuringCapture,
            onCheckedChange = onDndDuringCaptureChange,
            modifier = Modifier.testTag("dndDuringCaptureRow"),
        )
        // ROUND 14 (owner item 52): where you stand, and the way in. The
        // switch shipped in 0.8.0 stating a prerequisite nothing in the app
        // could satisfy or report.
        if (dndDuringCapture) {
            Spacer(Modifier.height(10.dp))
            Hint(
                com.lidarscan.core.capture.CaptureFocus.accessStatus(dndAccessGranted),
                color = if (dndAccessGranted) InkFaint else SemWarn,
                modifier = Modifier.testTag("dndAccessStatus"),
            )
            if (!dndAccessGranted) {
                Spacer(Modifier.height(10.dp))
                SecondaryPill(
                    text = "Allow Do Not Disturb",
                    height = 46.dp,
                    onClick = onGrantDndAccess,
                    modifier = Modifier.fillMaxWidth().testTag("dndGrantButton"),
                )
            }
        }
    }
}

/**
 * ROUND 24 item 113 — **DETAIL, where the owner said to put it.**
 *
 * The same three rungs the Scan screen's Advanced sheet draws
 * ([com.lidarscan.core.capture.DetailLevels]), reading and writing the same
 * persisted display block, clamped by the same ROUND 22 item 100 ceiling: a
 * rung this device cannot hold is ABSENT, not disabled, and the note underneath
 * says why there are two chips instead of three. There is no override, which is
 * the owner's own instruction from item 100.
 */
@Composable
private fun DetailCard(
    levels: List<com.lidarscan.core.capture.DetailLevel>,
    selected: com.lidarscan.core.capture.DetailLevel,
    ceilingNote: String?,
    onChange: (com.lidarscan.core.capture.DetailLevel) -> Unit,
) {
    ScanCard {
        CardTitle(Wording.DETAIL_LABEL)
        Spacer(Modifier.height(4.dp))
        Hint(Wording.DETAIL_BUDGET_HINT, color = InkFaint)
        Spacer(Modifier.height(10.dp))
        SegmentedPill(
            options = levels.map { it to it.displayName },
            selected = if (selected in levels) selected else levels.first(),
            onSelect = onChange,
            height = 44.dp,
            modifier = Modifier.testTag("settingsDetailRow"),
        )
        if (ceilingNote != null) {
            Spacer(Modifier.height(8.dp))
            Hint(ceilingNote, color = InkFaint, modifier = Modifier.testTag("settingsDetailCeilingNote"))
        }
    }
}

/**
 * ROUND 6 (owner item 20) — the field-evidence card, unchanged in function and
 * moved (ROUND 24 item 113) behind developer mode.
 *
 * It is not gone and it is not weaker: the log is still written on every run,
 * it is still the file ROUND 22 item 87's `[crash]` entries land in, and the
 * ordinary operator now has a better door to it than this one — Profile ›
 * **Send logs**, which packages it, names what is in it, and delivers it. This
 * card is the developer's version of the same thing: a path, a size, a live
 * tail, and Clear.
 */
@Composable
private fun CaptureLogCard(
    captureLogPath: String,
    captureLogSizeBytes: Long,
    captureLogLastLine: String?,
    exportNote: String?,
    onShareCaptureLog: () -> Unit,
    onClearCaptureLog: () -> Unit,
    onDismissExportNote: () -> Unit,
) {
    ScanCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Filled.Folder, contentDescription = null, tint = Ember)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    captureLogPath,
                    style = MonoMeta,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.testTag("captureLogPath"),
                )
                Spacer(Modifier.height(6.dp))
                Hint("${captureLogSizeBytes / 1024} KB, rolling.", color = InkFaint)
                if (captureLogLastLine != null) {
                    Spacer(Modifier.height(6.dp))
                    Text(
                        captureLogLastLine,
                        style = MonoLabel,
                        color = InkFaint,
                        maxLines = 2,
                        modifier = Modifier.testTag("captureLogLastLine"),
                    )
                }
            }
        }
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SecondaryPill(
                text = "Export log",
                height = 46.dp,
                onClick = onShareCaptureLog,
                modifier = Modifier.weight(1f).testTag("exportCaptureLogButton"),
            )
            SecondaryPill(
                text = "Clear",
                height = 46.dp,
                onClick = onClearCaptureLog,
                modifier = Modifier.weight(1f).testTag("clearCaptureLogButton"),
            )
        }
        // ROUND 7: no user-triggered file operation ends silently.
        if (exportNote != null) {
            Spacer(Modifier.height(8.dp))
            Text(
                exportNote,
                style = MaterialTheme.typography.bodySmall,
                color = Ink,
                modifier = Modifier
                    .clickable(onClick = onDismissExportNote)
                    .testTag("captureLogExportNote"),
            )
        }
    }
}

/**
 * ROUND 25 item 118, **owner amendment** — the "Connection debug" row.
 *
 * ## Why a screen and not only a log line
 *
 * The periodic `[net-debug]` sweeps answer the question "why did this fail?"
 * for whoever reads the capture log afterwards. This answers it for the person
 * standing there **with the hub in their hand**, which is a different problem
 * with a different deadline: the owner's Acer HY41-T9 was in front of him, and
 * the only thing the app would tell him was "No Ethernet adapter found." One
 * button, one full sweep, on screen.
 *
 * Copy is not a nicety. A sweep is a twenty-line block full of hex; nobody
 * transcribes that off a phone screen, and a diagnostic that cannot leave the
 * device it diagnoses is a diagnostic nobody sends you.
 *
 * ## Wording law
 *
 * The block in [output] is **exempt** from [com.lidarscan.core.WordingLaw] and
 * this is deliberate, not an oversight. It is developer-mode diagnostic output
 * — reachable only after seven taps on the version line, never shown to an
 * operator — and it is supposed to be dense with numbers, in the same way
 * `StartHoldTrimGate.refusalLogLine` is. Do NOT rewrite it into six-word
 * instructions; the six-word law governs the Mid-360 wizard
 * ([com.lidarscan.core.net.Mid360Diagnosis]'s strings), which is a different
 * surface read by a different person. The three chrome strings on this card
 * ("Connection debug", "Run sweep", "Copy") happen to satisfy the law anyway.
 *
 * The output box has a **fixed height and its own scroll**: an unbounded
 * `Text` here would make the whole Settings page as long as the USB device
 * list, and the page above it is what the operator came for.
 */
@Composable
private fun ConnectionDebugCard(
    output: String?,
    running: Boolean,
    onRun: () -> Unit,
) {
    val clipboard = androidx.compose.ui.platform.LocalClipboardManager.current
    ScanCard(modifier = Modifier.testTag("connectionDebugRow")) {
        CardTitle("Connection debug")
        Spacer(Modifier.height(4.dp))
        Hint("USB, interfaces and discovery, in one sweep.", color = InkFaint)
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SecondaryPill(
                text = if (running) "Sweeping…" else "Run sweep",
                height = 46.dp,
                onClick = onRun,
                modifier = Modifier.weight(1f).testTag("connectionDebugSweep"),
            )
            SecondaryPill(
                text = "Copy",
                height = 46.dp,
                onClick = {
                    // No-op with nothing to copy, rather than putting an empty
                    // clipboard in front of someone who is about to paste it
                    // into a bug report.
                    output?.let { clipboard.setText(androidx.compose.ui.text.AnnotatedString(it)) }
                },
                modifier = Modifier.weight(1f).testTag("connectionDebugCopy"),
            )
        }
        if (output != null) {
            Spacer(Modifier.height(10.dp))
            Column(
                Modifier
                    .fillMaxWidth()
                    .height(220.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                Text(
                    output,
                    style = MonoLabel,
                    color = MaterialTheme.colorScheme.onSurface,
                    // Horizontal scroll as well: a USB descriptor line with
                    // four interfaces on it is long, and wrapping hex at the
                    // screen edge is how a readable dump becomes an unreadable
                    // one.
                    modifier = Modifier
                        .horizontalScroll(rememberScrollState())
                        .testTag("connectionDebugOutput"),
                )
            }
        }
    }
}

/** A mono, uppercase, ember-tinted section rule + its content. */
@Composable
private fun SettingsSection(title: String, content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        Text(title.uppercase(), style = MonoLabel, color = Ember)
        Spacer(Modifier.height(6.dp))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Spacer(Modifier.height(12.dp))
        content()
    }
}

@Composable
private fun CardTitle(text: String) {
    Text(
        text,
        fontFamily = DisplayFontFamily,
        fontWeight = FontWeight.SemiBold,
        fontSize = 16.sp,
        color = MaterialTheme.colorScheme.onSurface,
    )
}

@Composable
private fun ScanSwitchRow(
    title: String,
    detail: String,
    checked: Boolean,
    enabled: Boolean = true,
    modifier: Modifier = Modifier,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            CardTitle(title)
            Spacer(Modifier.height(4.dp))
            Hint(detail, color = InkFaint)
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            enabled = enabled,
            onCheckedChange = onCheckedChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = androidx.compose.ui.graphics.Color.White,
                checkedTrackColor = Ember,
                checkedBorderColor = Ember,
            ),
        )
    }
}

/**
 * B5 — what each workflow profile actually sets, surfaced where a user can read
 * it (Tech Spec §3.9's "profiles set defaults").
 *
 * This is deliberately a READ-ONLY reference rather than an editor. The
 * defaults are applied at project creation and then belong to the project
 * (`ProjectManifest.captureDefaults`), so a device-level editor here would
 * change nothing about any existing capture while looking like it did — and
 * the settings that matter per project are already editable where they are
 * used: Live-SLAM on the Capture screen, the export format on Processing, and
 * the whole display block in Review's panel.
 */
@Composable
private fun WorkflowProfilesCard() {
    var expanded by remember { mutableStateOf(false) }
    ScanCard {
        Hint(
            "A profile is a bundle of capture and display defaults, applied when a project is created. " +
                "Changing a project afterwards changes that project only.",
            color = InkFaint,
        )
        if (expanded) {
            WorkflowProfile.entries.forEach { profile ->
                val d = CaptureDefaults.forProfile(profile)
                Spacer(Modifier.height(14.dp))
                CardTitle(profile.displayName)
                Spacer(Modifier.height(3.dp))
                Hint(profile.description, color = InkFaint)
                Spacer(Modifier.height(4.dp))
                Text(
                    listOf(
                        if (d.liveSlam) "Live-SLAM" else "Record-only",
                        "export ${d.exportFormat.displayName}",
                        "display ${d.displayProfile.displayName}",
                        if (d.captureCameraKeyframes && com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED) {
                            "camera keyframes on"
                        } else {
                            "no camera"
                        },
                        if (d.requireRtkFixForCapture) "blocks below ${d.minFixForCapture.label}" else "no RTK gate",
                    ).joinToString(" · "),
                    style = MonoMeta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.height(8.dp))
        TextButton(onClick = { expanded = !expanded }, contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
            Text(
                if (expanded) "Hide details" else "Show what each profile sets",
                color = Ember,
            )
        }
    }
}

/** D3 — where the Cloud processing mode uploads to (§3.8). */
@Composable
private fun CloudCard(baseUrl: String, token: String, onChange: (String, String) -> Unit) {
    var url by remember(baseUrl) { mutableStateOf(baseUrl) }
    var tok by remember(token) { mutableStateOf(token) }
    ScanCard {
        OutlinedTextField(
            value = url,
            onValueChange = { url = it },
            label = { Text("Server URL") },
            placeholder = { Text("https://cloud.example.com") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(
            value = tok,
            onValueChange = { tok = it },
            label = { Text("Token") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(8.dp))
        // ROUND 24 item 113: was 42 words carrying a design-document section
        // number onto a screen. The limitation it states is real and stays;
        // the citation does not. This pair also feeds ROUND 24 item 109's
        // feedback sender, which is why the second line says so.
        Hint(
            "Used for cloud processing and for sending logs.\n" +
                "The token is stored unencrypted on this phone.",
            color = InkFaint,
        )
        Spacer(Modifier.height(10.dp))
        SecondaryPill(text = "Save", height = 44.dp, onClick = { onChange(url, tok) })
    }
}

/** B6 — the one processing setting that is a policy statement rather than a per-run choice. */
@Composable
private fun ProcessingOptionsCard(allowPoorSync: Boolean, onChange: (Boolean) -> Unit) {
    ScanCard {
        ScanSwitchRow(
            title = "Colorize with poor clock sync",
            detail = "Above 15 ms of jitter, time sync alone eats most of the reprojection budget (S6), and the " +
                "colorizer refuses by default. Turning this on says the result is worth looking at, not worth " +
                "quoting.",
            checked = allowPoorSync,
            onCheckedChange = onChange,
        )
    }
}

/**
 * ROUND 9, owner item 33 — **empty scans.**
 *
 * A capture that received no sensor packets used to be kept ("the project was
 * saved so the evidence is not lost"), which is defensible once and intolerable
 * by the fifteenth time: the owner's phone ended up with a column of
 * `scan-012` / `scan-014` directories that read like real scans in the list
 * until you opened one. The default is now the opposite — Stop deletes a
 * 0-point scan and the Projects tab hides the legacy ones — and this card is
 * both halves of the escape hatch: the switch that restores the old behaviour,
 * and the button that gets the existing strays off the device for good.
 *
 * The count is live rather than a promise: a "clean up" button that cannot say
 * how many is asking for a blind tap.
 */
@Composable
private fun EmptyScansCard(
    keepEmptyScans: Boolean,
    emptyScanCount: Int,
    note: String?,
    onKeepEmptyScansChange: (Boolean) -> Unit,
    onCleanUp: () -> Unit,
    onDismissNote: () -> Unit,
) {
    ScanCard {
        // ROUND 24 item 113: was 48 words. The switch is a yes/no about
        // scans that recorded nothing; the essay about when to turn it on
        // belongs to whoever is chasing a dead sensor, not to this row.
        ScanSwitchRow(
            title = "Keep empty scans",
            detail = "Off: a scan with no points is deleted at Stop.",
            checked = keepEmptyScans,
            onCheckedChange = onKeepEmptyScansChange,
            modifier = Modifier.testTag("keepEmptyScansRow"),
        )
        Spacer(Modifier.height(12.dp))
        Hint(
            if (emptyScanCount == 0) {
                "No empty scans on this device."
            } else {
                "$emptyScanCount scan${if (emptyScanCount == 1) "" else "s"} recorded no points. " +
                    "Deleting cannot be undone."
            },
            color = InkFaint,
            modifier = Modifier.testTag("emptyScanCount"),
        )
        Spacer(Modifier.height(10.dp))
        SecondaryPill(
            text = if (emptyScanCount == 0) "Clean up" else "Clean up $emptyScanCount",
            height = 46.dp,
            onClick = onCleanUp,
            modifier = Modifier.fillMaxWidth().testTag("cleanUpEmptyScansButton"),
        )
        if (note != null) {
            Spacer(Modifier.height(8.dp))
            Text(
                note,
                style = MaterialTheme.typography.bodySmall,
                color = Ink,
                modifier = Modifier
                    .clickable(onClick = onDismissNote)
                    .testTag("emptyScanCleanupNote"),
            )
        }
    }
}

/**
 * ROUND 7 — the one D6 timing number that cannot be derived, exposed.
 *
 * Everything else about the D6's clock is now handled without asking: the app
 * stamps each USB read with `elapsedRealtimeNanos()` (the same CLOCK_BOOTTIME
 * the engine and ARCore both use, so there is nothing to convert), and the
 * engine back-dates every return inside that read from its byte position at a
 * known baud. What is left is a constant transport delay, and a constant delay
 * translates the whole cloud along the walk and bends corners while turning.
 *
 * See [com.lidarscan.core.capture.D6TimeSync] for the derivation of the
 * default. The row shows what the number MEANS in centimetres, because "2 ms"
 * is not a quantity anyone can judge and "2 mm at walking pace" is.
 */
/**
 * ROUND 20 (item 82) — **the per-device mount profile.**
 *
 * The owner's mandate: the D6's position on the phone back VARIES (his sits
 * toward the middle, not the top edge the old CAD placeholder assumed) and
 * the app will be public — so nothing about the mount is hard-coded. The
 * ROTATION is measured at every Start by the hold-steady stage (item 78,
 * gravity-referenced per item 79); the LEVER ARM is these three centimetre
 * fields; the one thing still assumed is the documented convention the
 * schematic text states (0° mark up, cap forward). An auto-level result
 * (item 80) appears below as a SUGGESTION with provenance — never silently
 * applied.
 */
@Composable
private fun MountProfileCard(
    leverArm: com.lidarscan.core.calib.MountLeverArm,
    storedTrim: com.lidarscan.core.calib.StoredMountTrim?,
    autoLevelSuggestion: String?,
    onChange: (Double, Double, Double) -> Unit,
    onReset: () -> Unit,
) {
    ScanCard {
        CardTitle("Where the D6 sits")
        val trim = storedTrim?.trim
        // ROUND 24 item 113: was 15 words of process description. What the
        // operator needs is whether it is measured, and the number if it is.
        Text(
            if (trim != null) {
                "Rotation measured: %.1f°.".format(trim.magnitudeDeg)
            } else {
                "Rotation not measured yet."
            },
            style = MaterialTheme.typography.bodyMedium,
            color = Ink,
            modifier = Modifier.testTag("mountProfileRotation"),
        )
        Spacer(Modifier.height(10.dp))
        // Three centimetre fields. Local text state, committed on change when
        // parseable; unparseable text simply does not commit (the same
        // tolerance every free-text field in this app has).
        var up by remember(leverArm) { mutableStateOf("%.1f".format(leverArm.upCm)) }
        var behind by remember(leverArm) { mutableStateOf("%.1f".format(leverArm.behindCm)) }
        var right by remember(leverArm) { mutableStateOf("%.1f".format(leverArm.rightCm)) }
        fun commit() {
            val u = up.toDoubleOrNull()
            val b = behind.toDoubleOrNull()
            val r = right.toDoubleOrNull()
            if (u != null && b != null && r != null) onChange(u, b, r)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
        Spacer(Modifier.height(6.dp))
        // ROUND 24 item 113: was 47 words. The provenance stays — it is a
        // fact about THIS phone's numbers — and the essay does not.
        Text(
            "Distance from the rear camera, in centimetres. " +
                "Source: ${leverArm.provenance}.",
            style = MaterialTheme.typography.bodySmall,
            color = InkFaint,
        )
        Spacer(Modifier.height(8.dp))
        // The schematic: the one assumed convention, stated.
        Text(
            "┌──────────┐   phone back, seen from behind\n" +
                "│  ┌─D6─┐  │   0° mark points UP\n" +
                "│  │ ◉  │  │   cap faces FORWARD (the walk)\n" +
                "│  └────┘  │   everything else: measured\n" +
                "└──────────┘   or typed above",
            style = MaterialTheme.typography.bodySmall.copy(
                fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
            ),
            color = InkFaint,
            modifier = Modifier.testTag("mountConventionSchematic"),
        )
        if (autoLevelSuggestion != null) {
            Spacer(Modifier.height(8.dp))
            Hint(
                autoLevelSuggestion + " A suggestion only, never applied by itself.",
                color = ScanTeal,
                modifier = Modifier.testTag("mountAutoLevelSuggestion"),
            )
        }
        Spacer(Modifier.height(10.dp))
        SecondaryPill(
            text = "Reset offsets",
            height = 42.dp,
            onClick = onReset,
            modifier = Modifier.fillMaxWidth().testTag("leverArmResetButton"),
        )
    }
}

@Composable
private fun D6TimingCard(latencyMs: Int, onChange: (Int) -> Unit) {
    ScanCard {
        CardTitle("COIN-D6 sensor latency")
        Text(
            com.lidarscan.core.capture.D6TimeSync.describe(latencyMs),
            style = MaterialTheme.typography.bodyMedium,
            color = Ink,
            modifier = Modifier.testTag("d6SensorLatencyValue"),
        )
        Spacer(Modifier.height(8.dp))
        Slider(
            value = latencyMs.toFloat(),
            onValueChange = { onChange(it.roundToInt()) },
            valueRange = com.lidarscan.core.capture.D6TimeSync.MIN_SENSOR_LATENCY_MS.toFloat()..
                com.lidarscan.core.capture.D6TimeSync.MAX_SENSOR_LATENCY_MS.toFloat(),
            modifier = Modifier.testTag("d6SensorLatencySlider"),
        )
        Text(
            "The constant delay between a byte reaching the D6 cable and this phone reading its clock. " +
                "The default (" +
                "${com.lidarscan.core.capture.D6TimeSync.DEFAULT_SENSOR_LATENCY_MS} ms) is derived from " +
                "one USB bulk frame plus one thread wake-up, not measured on your phone — if walls land " +
                "consistently in front of or behind where they are, this is the knob. Applies to the next " +
                "chunk of data, no reconnect needed.",
            style = MaterialTheme.typography.bodySmall,
            color = InkFaint,
        )
    }
}

@Composable
private fun EngineCard(
    useFakeEngine: Boolean,
    nativeEngineAvailable: Boolean,
    onUseFakeEngineChange: (Boolean) -> Unit,
    onReplaySyntheticCapture: () -> Unit,
) {
    ScanCard {
        ScanSwitchRow(
            title = "Use simulated engine",
            detail = if (nativeEngineAvailable) {
                "Takes effect after restarting the app."
            } else {
                "Native engine not loaded on this build — always simulated regardless of this switch."
            },
            checked = useFakeEngine || !nativeEngineAvailable,
            enabled = nativeEngineAvailable,
            onCheckedChange = onUseFakeEngineChange,
        )

        Spacer(Modifier.height(14.dp))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Spacer(Modifier.height(14.dp))

        // B4's acceptance path: drives the full Capture screen (live 3D view,
        // stat panel, session summary) from the bundled synthetic D6 capture
        // (S1's d6synth output) via ReplayEngineBridge — no hardware needed.
        CardTitle("Replay synthetic capture")
        Spacer(Modifier.height(4.dp))
        Hint(
            if (nativeEngineAvailable) {
                "Opens Capture driven by a bundled synthetic D6 recording — verifies the live 3D view end to " +
                    "end with no hardware."
            } else {
                "Native engine not loaded on this build — replay needs it, same as live capture."
            },
            color = InkFaint,
        )
        Spacer(Modifier.height(10.dp))
        SecondaryPill(
            text = "Replay synthetic capture",
            height = 44.dp,
            enabled = nativeEngineAvailable,
            onClick = onReplaySyntheticCapture,
            // testTag: the CI emulator smoke test's UI-navigation variant taps
            // this node directly — two Text nodes on this screen share the
            // string "Replay synthetic capture" (this button's label and the
            // heading above it), so a plain text match is ambiguous.
            modifier = Modifier.testTag("replaySyntheticCaptureButton"),
        )
    }
}
