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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
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
        onDeveloperModeChange = viewModel::setDeveloperMode,
        onCaptureDebugLogChange = viewModel::setCaptureDebugLog,
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
    /** ROUND 17 item 66: the seven-tap unlock and the one thing behind it. */
    onDeveloperModeChange: (Boolean) -> Unit = {},
    onCaptureDebugLogChange: (Boolean) -> Unit = {},
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
        HeroHeader(title = "Settings", subtitle = "device-level · applies to every project")

        Column(
            Modifier.padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(22.dp),
        ) {
            SettingsSection("Workflow profiles") { WorkflowProfilesCard() }

            SettingsSection("Units") {
                SegmentedPill(
                    options = Units.entries.map { it to it.displayName },
                    selected = settings.units,
                    onSelect = onUnitsChange,
                    height = 44.dp,
                )
            }

            SettingsSection("Theme") {
                SegmentedPill(
                    options = ThemeMode.entries.map { it to it.displayName },
                    selected = settings.themeMode,
                    onSelect = onThemeModeChange,
                    height = 44.dp,
                )
                Spacer(Modifier.height(8.dp))
                Hint(
                    "The redesign is a dark cockpit by intent; Light is a faithful inversion of the same " +
                        "tokens, not a second design.",
                    color = InkFaint,
                )
            }

            SettingsSection("Cloud processing") {
                CloudCard(settings.cloudBaseUrl, settings.cloudToken, onCloudChange)
            }

            // ROUND 10 (owner item 39): the only setting in this section is a
            // colorize policy, so with colorization paused the section itself
            // has nothing to say. Hidden whole rather than left as an empty
            // heading — "owner hates clutter" (ROUND 9).
            if (com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED) {
                SettingsSection("Processing") {
                    ProcessingOptionsCard(settings.allowPoorSyncColorize, onAllowPoorSyncChange)
                }
            }

            // ROUND 9 (owner item 33): "owner hates clutter". One switch and one
            // button — the switch decides what Stop does with a scan that
            // recorded nothing, the button gets rid of the ones already there.
            SettingsSection("Scans") {
                EmptyScansCard(
                    keepEmptyScans = settings.keepEmptyScans,
                    emptyScanCount = emptyScanCount,
                    note = emptyScanNote,
                    onKeepEmptyScansChange = onKeepEmptyScansChange,
                    onCleanUp = onCleanUpEmptyScans,
                    onDismissNote = onDismissEmptyScanNote,
                )
            }

            // ROUND 11 (owner item 43): the phone is the sensor mount and faces
            // away from the operator, so the app has to be able to speak.
            SettingsSection("Operator cues") {
                ScanCard {
                    ScanSwitchRow(
                        title = "Vibrate and beep during a scan",
                        detail = "On (the default). Two buzzes when phone tracking degrades, three short " +
                            "buzzes when the scan breaks into a new section, one long soft buzz when you " +
                            "are moving too fast for the returns to keep up. The phone faces away from you " +
                            "while you walk, so these are the only hints you can actually receive. Turn " +
                            "them off for a scan somewhere quiet — everything they say is on screen too.",
                        checked = settings.operatorCuesEnabled,
                        onCheckedChange = onOperatorCuesChange,
                        modifier = Modifier.testTag("operatorCuesRow"),
                    )
                }
                ScanCard {
                    ScanSwitchRow(
                        title = "Silence notifications while scanning",
                        detail = "On (the default). A notification does not just distract you — the " +
                            "buzz fires the vibration motor, and this phone IS the scan's inertial " +
                            "sensor, so a 200 ms buzz shakes the IMU and smears the camera mid-" +
                            "measurement. Your own scan cues still buzz. Needs Do Not Disturb " +
                            "access; without it the scan runs anyway and the log records that it " +
                            "was unprotected. Your previous setting is restored when the scan ends.",
                        checked = settings.dndDuringCapture,
                        onCheckedChange = onDndDuringCaptureChange,
                        modifier = Modifier.testTag("dndDuringCaptureRow"),
                    )
                    // ── ROUND 14 (owner item 52) ────────────────────────────
                    //
                    // The switch above shipped in 0.8.0 stating a prerequisite
                    // ("Needs Do Not Disturb access") that nothing in the app
                    // could satisfy and nothing in the app could report. Every
                    // session in the owner's field log recorded
                    // `dnd=unprotected-no-permission` and he never saw a prompt.
                    // This row is both halves: where you stand, and the way in.
                    if (settings.dndDuringCapture) {
                        Spacer(Modifier.height(10.dp))
                        Hint(
                            com.lidarscan.core.capture.CaptureFocus.accessStatus(dndAccessGranted),
                            color = if (dndAccessGranted) InkFaint else SemWarn,
                            modifier = Modifier.testTag("dndAccessStatus"),
                        )
                        if (!dndAccessGranted) {
                            Spacer(Modifier.height(10.dp))
                            SecondaryPill(
                                text = "Grant Do Not Disturb access",
                                height = 46.dp,
                                onClick = onGrantDndAccess,
                                modifier = Modifier.fillMaxWidth().testTag("dndGrantButton"),
                            )
                        }
                    }
                }
            }

            // ROUND 20 item 82: the per-device mount profile — no hard-coded
            // geometry for a public app.
            SettingsSection("Mount profile (COIN-D6)") {
                MountProfileCard(
                    leverArm = settings.mountLeverArm,
                    storedTrim = storedMountTrim,
                    autoLevelSuggestion = settings.mountAutoLevelSuggestion,
                    onChange = onMountLeverArmChange,
                    onReset = onMountLeverArmReset,
                )
            }

            SettingsSection("Sensor timing (advanced)") {
                D6TimingCard(settings.d6SensorLatencyMs, onD6SensorLatencyChange)
            }

            SettingsSection("Engine (developer)") {
                EngineCard(
                    useFakeEngine = settings.useFakeEngine,
                    nativeEngineAvailable = nativeEngineAvailable,
                    onUseFakeEngineChange = onUseFakeEngineChange,
                    onReplaySyntheticCapture = onReplaySyntheticCapture,
                )
            }

            // ROUND 6 (owner item 20): the field-evidence section. The last
            // capture failure arrived with no stack trace and no logcat,
            // because logcat on a phone in the field is gone by the time
            // anyone asks. This is the persistent copy, its path, and a share
            // button — so the NEXT report can carry the trace with it.
            SettingsSection("Capture log") {
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
                            Hint(
                                "Project creation, session start/stop, pushbroom, and every seal — written " +
                                    "here and kept across restarts (${captureLogSizeBytes / 1024} KB, rolling). " +
                                    "Attach it to a bug report.",
                                color = InkFaint,
                            )
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

            SettingsSection("Storage location") {
                ScanCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Filled.Folder, contentDescription = null, tint = Ember)
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(storageLocation, style = MonoMeta, color = MaterialTheme.colorScheme.onSurface)
                            Spacer(Modifier.height(6.dp))
                            Hint(
                                "Where .lscan projects are stored on this device. A location picker is a " +
                                    "future addition.",
                                color = InkFaint,
                            )
                        }
                    }
                }
            }
        }

        // ── ROUND 17 item 67: the camera sentence ───────────────────────────
        //
        // The app holds the rear camera open for every second of every walk,
        // because ARCore's visual-inertial odometry is what places each lidar
        // return. Nothing anywhere told the operator that, and the one place
        // the camera WAS mentioned — the profile reference card — said "no
        // camera", which is true about storage and false about the lens.
        //
        // Audited in ROUND 17 across every path that can touch an ARCore image
        // (KeyframeRecorder, the calibration wizard, the GL background
        // renderer, the C++ colorizer and the container's chunk types). With
        // colorization off, no frame is acquired, encoded or written by any of
        // them, and KeyframeRecorder now refuses at the source rather than
        // relying on its callers.
        SettingsSection("Camera") {
            ScanCard {
                CardTitle("Camera is used for position tracking only")
                Hint(
                    "No images are saved. The rear camera runs for the whole of a scan because " +
                        "that is what tells the app where the sensor is — every lidar return is " +
                        "placed using it. The frames go to the tracker and nowhere else: nothing " +
                        "is written to the scan file, nothing is kept in the app, and nothing " +
                        "leaves the phone.",
                    modifier = Modifier.testTag("cameraHonestyCard"),
                )
            }
        }

        // ── ROUND 17 item 66: Developer Mode ────────────────────────────────
        if (settings.developerMode) {
            SettingsSection("Developer") {
                ScanCard {
                    ScanSwitchRow(
                        title = "Per-capture debug log",
                        detail = "Writes debug/capture-debug.log inside each scan's own .lscan " +
                            "bundle: session lifecycle, pose acceptance, re-anchor decisions " +
                            "with their numbers, watchdog transitions, cues and preset changes. " +
                            "It travels with the scan, so a bundle you send is the whole story. " +
                            "Capped at 5 MB; not a recorded stream and not part of the replay " +
                            "guarantee.",
                        checked = settings.captureDebugLog,
                        onCheckedChange = onCaptureDebugLogChange,
                        modifier = Modifier.testTag("captureDebugLogRow"),
                    )
                    Hint("Tap the version line below seven times again to lock this away.")
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
        Hint(
            "Single-tenant, token auth, one worker — the MVP boundaries §3.8 makes contractual. The token is " +
                "stored in app-private preferences, not the Android Keystore; that is a stated limitation, and " +
                "it is why the service is meant to sit behind TLS.",
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
        ScanSwitchRow(
            title = "Keep empty scans",
            detail = "Off (the default): a scan that records 0 points is deleted when you press Stop, and " +
                "0-point projects already on the phone stay out of the Projects list. On: every attempt is " +
                "kept as evidence — the right setting while chasing a sensor that produces nothing.",
            checked = keepEmptyScans,
            onCheckedChange = onKeepEmptyScansChange,
            modifier = Modifier.testTag("keepEmptyScansRow"),
        )
        Spacer(Modifier.height(12.dp))
        Hint(
            if (emptyScanCount == 0) {
                "No empty scans on this device."
            } else {
                "$emptyScanCount scan${if (emptyScanCount == 1) "" else "s"} on this device recorded no " +
                    "points. Deleting them removes the .lscan directories permanently; there is nothing in " +
                    "them but a manifest."
            },
            color = InkFaint,
            modifier = Modifier.testTag("emptyScanCount"),
        )
        Spacer(Modifier.height(10.dp))
        SecondaryPill(
            text = if (emptyScanCount == 0) "Clean up empty scans" else "Clean up $emptyScanCount empty scans",
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
        CardTitle("Where the D6 sits on this phone")
        val trim = storedTrim?.trim
        Text(
            if (trim != null) {
                "Rotation: measured — %.1f° trim, re-measured at every capture start during the hold."
                    .format(trim.magnitudeDeg)
            } else {
                "Rotation: not measured yet — the hold-steady stage at your first capture start " +
                    "will measure it."
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
        Text(
            "Where the D6's optical centre sits relative to the REAR CAMERA, in centimetres: " +
                "above it, behind it (away from the scene), and toward your right in the " +
                "scanning hold. Source: ${leverArm.provenance}. At walking pace these offsets " +
                "move the map by millimetres — rotation is what matters, and it is measured.",
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
                autoLevelSuggestion +
                    " This is a suggestion from processing, never applied by itself — the next " +
                    "capture's hold re-measures the rotation.",
                color = ScanTeal,
                modifier = Modifier.testTag("mountAutoLevelSuggestion"),
            )
        }
        Spacer(Modifier.height(10.dp))
        SecondaryPill(
            text = "Reset offsets to defaults",
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
