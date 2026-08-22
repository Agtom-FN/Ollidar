package com.lidarscan.app.ui.capture

import android.content.res.Configuration
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.layout.boundsInParent
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.ar.ArOverlayView
import com.lidarscan.app.ar.ArPosePumpView
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.app.render.PointCloudView
import com.lidarscan.app.render.StreamFilter
import com.lidarscan.app.ui.common.fixColor
import com.lidarscan.app.ui.components.BackBar
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanCard
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.SegmentedPill
import com.lidarscan.app.ui.components.Stat
import com.lidarscan.app.ui.components.StatPanel
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.components.PostureBubbleSlot
import com.lidarscan.app.ui.components.PostureGhostIndicator
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanCountdown
import com.lidarscan.app.ui.components.ScanIconButton
import com.lidarscan.app.ui.components.ScanIcons
import com.lidarscan.app.ui.components.ScanRow
import com.lidarscan.app.ui.components.ScanRowCard
import com.lidarscan.app.ui.components.SectionLabel
import com.lidarscan.app.ui.components.StatusDot
import com.lidarscan.app.ui.theme.ScanMeta
import com.lidarscan.app.ui.theme.ScanTitle
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.MonoTabular
import com.lidarscan.app.ui.theme.OnEmber
import com.lidarscan.app.ui.theme.sensorBadgeColor
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.app.ui.theme.ScanMetaCaps
import com.lidarscan.core.capture.CaptureAutoConnectState
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.gnss.GeorefSourceState
import com.lidarscan.core.gnss.GnssFixSnapshot
import com.lidarscan.core.gnss.NtripStatsSnapshot
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/**
 * ROUND 5's Capture tab.
 *
 * [projectId] is **null** for the tab itself: the tab exists to create new scan
 * projects (item 8), and Start is what creates one (item 9). The replay/deep-link
 * path passes an id and records into (or replays from) a project that already
 * exists — the only remaining caller that does.
 */
/**
 * ROUND 7, owner directive — the AR overlay is archived out of the product.
 *
 * Flipping this to `false` restores the camera-passthrough capture view; the
 * `CameraMode.AR` option also has to go back into `CaptureSheets`' View row for
 * it to be reachable. Everything else about the AR path is intact.
 */
private const val AR_OVERLAY_ARCHIVED = true

@Composable
fun CaptureRoute(
    container: AppContainer,
    projectId: String? = null,
    isReplay: Boolean = false,
    onBack: () -> Unit,
    /** Optional door to the mount-calibration wizard, for a D6 running on the CAD nominal. */
    onOpenMountCalibration: ((String) -> Unit)? = null,
    /**
     * ROUND 8, owner item 31 — called once per **verified** seal with the id of
     * the scan that was just saved, so the shell can land the operator on it in
     * Projects. Null (the replay route) means "stay here".
     */
    onScanSealed: ((String) -> Unit)? = null,
    /**
     * ROUND 23 item 106(c) — the Advanced switch, read by the shell and passed
     * down so the Scan tab can ask [com.lidarscan.core.SimpleMode] the same
     * questions every other surface asks it.
     */
    advanced: Boolean = false,
    /**
     * ROUND 27 item 142(b) — the "Send logs" shortcut on the tracker-failure
     * card. Null where there is no Profile to reach (the replay route).
     */
    onOpenProfile: (() -> Unit)? = null,
    /** ROUND 23 item 106(c): the Mid-360 wizard, with no project in front of it. */
    onOpenMid360Setup: (() -> Unit)? = null,
    /** ROUND 23 item 106(c): the device-level RTK screen. */
    onOpenRtk: (() -> Unit)? = null,
) {
    // ROUND 5.2: the fine-location prompt, hoisted here because only an Activity
    // context can ask. The ViewModel calls `requestLocationPermission` at Start;
    // this bridges that suspend call to the launcher's callback.
    val locationPermissionRequest = remember { PermissionRequestBridge() }
    // ROUND 19 item 75: the live renderer, reachable from a ViewModel lambda.
    // The ViewModel must never hold a GL-thread object across its own lifetime
    // (the rig-pose rule), so it gets a READ once, at seal time, through this
    // holder the Compose side owns and clears with the view.
    val coverageRendererHolder = remember {
        java.util.concurrent.atomic.AtomicReference<com.lidarscan.app.render.PointCloudRenderer?>(null)
    }
    val locationLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> locationPermissionRequest.complete(granted) }

    // ROUND 6 (item 21): the panel's own ceiling, read once here so both the
    // ViewModel's preset table and the settings sheet's refresh row use exactly
    // the same number.
    val displayCeilingHz = com.lidarscan.app.render.displayRefreshCeilingHz(
        androidx.compose.ui.platform.LocalContext.current,
    )

    val viewModel: CaptureViewModel = viewModel(
        key = "capture-${projectId ?: "new"}-$isReplay",
        factory = viewModelFactory {
            initializer {
                val bridge = if (isReplay) container.newReplayEngineBridge() else container.engineBridge
                CaptureViewModel(
                    engineBridge = bridge,
                    projectStore = container.projectStore,
                    projectId = projectId,
                    isReplay = isReplay,
                    // B7: a replay session has no camera and no live engine to
                    // push poses into, so it gets no AR controller at all
                    // rather than one that would silently do nothing.
                    arController = if (isReplay) null else container.arController,
                    // ROUND 25 item 115: leaving the tab closes the tracking
                    // session outright rather than pausing it, so a
                    // backgrounded Scan tab costs no camera. A replay has no
                    // session to close and gets a no-op.
                    shutDownTracking = {
                        if (!isReplay) container.arController.close()
                    },
                    // ROUND 25 item 115: the seal that ran because the operator
                    // walked away announces itself on Projects instead of
                    // dragging them there.
                    onScanSavedInBackground = { sealedId ->
                        container.scanSavedNotice.value = sealedId
                    },
                    // ROUND 9 (item 35): the phone IMU densifies ARCore's ~30 Hz
                    // poses for the D6's 4000 Hz returns. A replay has neither.
                    phoneImu = if (isReplay) null else container.phoneImuRecorder,
                    engineHandleProvider = container::currentEngineHandle,
                    mountCalibrationFor = { sensor ->
                        container.mountCalibrationStore.find(
                            phoneModel = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}",
                            bracketId = com.lidarscan.core.calib.BracketNominals.DEFAULT_BRACKET_ID,
                            sensorSerial = null,
                        )?.takeIf { it.sensor == sensor }
                    },
                    // B9: A10's solution, snapshotted into the manifest at stop
                    // (see stopCapture). A replay session has no rover, so it
                    // reads null and the manifest keeps whatever it had.
                    georefSnapshotProvider = { handle ->
                        if (isReplay) null else container.rtkManager.georefRecord(handle)
                    },
                    // ROUND 5 (item 7): auto-detect on entry. Both probes, raced.
                    //
                    // ROUND 25 item 119: still exactly two entries. The serial
                    // one now walks D6 → STL-27L internally rather than being
                    // raced against a second USB probe — see
                    // `SerialLidarAutoDetector` for why two coroutines opening
                    // the same CH340 at different bauds would be a bug.
                    autoDetectors = if (isReplay) {
                        emptyList()
                    } else {
                        listOf(
                            com.lidarscan.app.capture.SerialLidarAutoDetector
                                .fromRegistry(
                                    container.d6UsbConnectionRegistry,
                                    connectionDebug = container.connectionDebugSweeper,
                                ),
                            com.lidarscan.app.capture.Mid360HeartbeatAutoDetector(
                                detector = container.mid360HeartbeatDetector,
                                ethernetMonitor = container.ethernetMonitor,
                                onFound = { lidarIp, hostIp, sn ->
                                    // AUTO-DETECT §3: last-detected addresses become
                                    // this device's capture defaults.
                                    container.settingsRepository.setLastDetectedMid360(lidarIp, hostIp, sn)
                                },
                                // ROUND 25 item 118 (owner amendment).
                                connectionDebug = container.connectionDebugSweeper,
                            ),
                        )
                    },
                    claimSeriesNumber = { container.settingsRepository.nextScanSeries() },
                    peekSeriesNumber = {
                        container.settingsRepository.settings.first().scanSeriesCounter + 1
                    },
                    attachedSerialDevices = {
                        container.d6UsbConnectionRegistry.findDrivers().map { driver ->
                            ManualSerialDevice(
                                path = driver.device.deviceName,
                                label = "${driver.device.deviceName.substringAfterLast('/')} · " +
                                    "VID ${driver.device.vendorId}/PID ${driver.device.productId}",
                            )
                        }
                    },
                    openSerialPort = { path, baud, lines ->
                        com.lidarscan.app.capture.openSerialPortByPath(
                            container.d6UsbConnectionRegistry,
                            path,
                            baud,
                            lines,
                        )
                    },
                    manualMid360Defaults = {
                        val s = container.settingsRepository.settings.first()
                        (s.lastDetectedMid360LidarIp ?: com.lidarscan.core.net.Mid360Settings.DEFAULT_LIDAR_IP) to
                            (s.lastDetectedMid360HostIp ?: com.lidarscan.core.net.Mid360Settings.DEFAULT_HOST_IP)
                    },
                    // ROUND 5.2: georeference source — rover first, phone second.
                    rtkFix = if (isReplay) null else container.rtkManager.fix,
                    phoneLocationFixes = if (isReplay) null else ({ container.phoneLocationSource.fixes() }),
                    hasLocationPermission = container.phoneLocationSource::hasPermission,
                    requestLocationPermission = if (isReplay) {
                        null
                    } else {
                        {
                            locationPermissionRequest.await {
                                locationLauncher.launch(android.Manifest.permission.ACCESS_FINE_LOCATION)
                            }
                        }
                    },
                    phoneGeorefRecorder = if (isReplay) null else com.lidarscan.app.gnss.PhoneGeorefRecorder(),
                    // ROUND 6 (item 20): every capture-survival event into the
                    // persistent on-device log, so the NEXT field failure
                    // arrives with evidence attached.
                    logEvent = container.captureLog::log,
                    // ROUND 17 item 66. The gate lives HERE, at the one place
                    // that can read a preference, so nothing downstream needs
                    // to know Developer Mode exists: with it off,
                    // `beginCaptureDebug` opens no sink and every `logDebug`
                    // call in the ViewModel is a null check that returns.
                    beginDebugLog = container.captureLog::beginCaptureDebug,
                    logDebug = container.captureLog::debug,
                    endDebugLog = container.captureLog::endCaptureDebug,
                    // ROUND 18 item 71: the project-guarded variants, for the
                    // auto-process verdicts written after the seal.
                    logDebugFor = container.captureLog::debugFor,
                    endDebugLogFor = container.captureLog::endCaptureDebugFor,
                    // ROUND 6 (items 21 + 22): this phone's class, its real
                    // display ceiling and the live page-store sizing its engine
                    // was created with — the three inputs the preset table and
                    // the conservative defaults are computed from.
                    deviceTier = container.deviceTier,
                    displayCeilingHz = displayCeilingHz,
                    pageStoreSizing = container.livePageStoreSizing,
                    loadPersistedPreset = {
                        container.settingsRepository.performancePreset(container.deviceProfileKey)
                    },
                    persistPreset = { preset ->
                        container.settingsRepository.setPerformancePreset(container.deviceProfileKey, preset)
                    },
                    // ROUND 7 (field bug 1): the mount re-zero now outlives this
                    // ViewModel, this navigation and this app process.
                    loadStoredMountTrim = { container.settingsRepository.storedMountTrim() },
                    persistMountTrim = { stored -> container.settingsRepository.setStoredMountTrim(stored) },
                    appRunId = container.appRunId,
                    // ROUND 9 (owner item 33): read at Stop, not here, so the
                    // switch applies to the next capture rather than the next
                    // time this tab is rebuilt.
                    keepEmptyScans = {
                        container.settingsRepository.settings.first().keepEmptyScans
                    },
                    // ROUND 11 (owner item 43): the ViewModel holds no Context,
                    // so the buzzing arrives as a lambda exactly the way
                    // `logEvent` does.
                    playCue = container.cuePlayer::play,
                    cuesEnabled = {
                        container.settingsRepository.settings.first().operatorCuesEnabled
                    },
                    // ROUND 13 (owner item 47). Read at Start, so the switch
                    // applies to the next capture and never changes the filter
                    // under a walk already in progress.
                    engageDnd = {
                        container.dndGuard.engage(
                            container.settingsRepository.settings.first().dndDuringCapture,
                        )
                    },
                    releaseDnd = container.dndGuard::release,
                    // ROUND 14 (owner item 53): the Mid-360 preflight's view of
                    // the cable. `EthernetMonitor` is already started by this
                    // screen (see the DisposableEffect above), so this is a
                    // read of state that exists rather than new machinery.
                    ethernetSnapshot = {
                        val eth = container.ethernetMonitor.state.value
                        eth.adapterPresent to eth.addresses.map { it.ip }
                    },
                    // ROUND 15 (item 55): auto-process on seal. Handle-less by
                    // construction — it takes a DIRECTORY and opens its own
                    // PageStore inside the engine — so it shares nothing with
                    // the capture engine and is safe to run while the tab has
                    // already re-armed for the next scan.
                    runAutoProcess = { dir, onProgress ->
                        container.processingRepository.reprocessD6(
                            lscanDir = dir,
                            refineSeams = true,
                            onProgress = onProgress,
                        )
                    },
                    // ROUND 22 item 90: the auto-process outlives this screen.
                    // Its old home, `viewModelScope`, was cancelled by the very
                    // navigation the seal performs.
                    autoProcessScope = container.containerScope,
                    // ROUND 19 item 76: the persisted device display block —
                    // the one source of truth Review's panel also writes.
                    loadDeviceDisplay = { container.settingsRepository.displayParams() },
                    persistDeviceDisplay = { p -> container.settingsRepository.setDisplayParams(p) },
                    // ROUND 19 item 77: the checklist's one persisted bit.
                    // ROUND 23 item 106(b): …and the flag that folded the sheet
                    // into the start panel. With
                    // `FeatureFlags.PRE_SCAN_CHECKLIST_SHEET` off this reads as
                    // "already dismissed", so the modal is never armed while the
                    // sheet, the API and the ROUND 19 tests stay exactly as they
                    // are. The checks themselves did not go anywhere — see
                    // `PreScanChecks` and `StartProgress.checks`.
                    preScanChecklistDismissed = {
                        !com.lidarscan.core.FeatureFlags.PRE_SCAN_CHECKLIST_SHEET ||
                            container.settingsRepository.preScanChecklistDismissed()
                    },
                    persistPreScanChecklistDismissed = {
                        container.settingsRepository.setPreScanChecklistDismissed()
                    },
                    // ROUND 19 item 75: the largest thin arc, read at seal time.
                    coverageAdviceProvider = {
                        coverageRendererHolder.get()?.coverageAdviceLine()
                    },
                    // ROUND 20 item 82: the per-device lever arm the extrinsic's
                    // translation comes from — user-editable in Settings.
                    loadMountLeverArm = { container.settingsRepository.mountLeverArm() },
                    // ROUND 20 items 80/82: the auto-level suggestion channel.
                    persistAutoLevelSuggestion = { s ->
                        container.settingsRepository.setMountAutoLevelSuggestion(s)
                    },
                )
            }
        },
    )
    // ── ROUND 14 (owner item 52): the ask-once flow ROUND 13 never built. ──
    //
    // 0.8.0 shipped `DoNotDisturbGuard.policyAccessIntent()` with a doc comment
    // reading "the caller shows this once" and NO CALLER anywhere in the app.
    // Every one of the owner's field sessions logged
    // `dnd=unprotected-no-permission`, correctly, and there was no screen
    // anywhere that could have got him the grant. The Settings switch even
    // said "Needs Do Not Disturb access" while offering no way to satisfy it.
    //
    // It fires on screen ENTRY and never at Start: `CaptureScreen`'s own rule
    // (and `DoNotDisturbGuard`'s) is that a modal mid-walk is the worst
    // possible interruption, and a permission dialog thrown up as the operator
    // presses Record would be exactly that.
    val dndScope = rememberCoroutineScope()
    var showDndExplainer by remember { mutableStateOf(false) }
    // ── ROUND 27 item 133(b): ARMED on entry, ASKED at the first Start ──────
    //
    // Round 24 fired the explainer from the screen's `LaunchedEffect(Unit)`,
    // and its stated reason was sound as far as it went: a permission dialog
    // thrown up as the operator presses Record is the worst interruption this
    // tab has. What it produced, though, was worse — a first-run operator opens
    // the app, taps Scan, and before they have touched anything the system asks
    // "Silence notifications while scanning?" about a scan that does not exist
    // and a scanner that is not plugged in. A permission with no context is a
    // permission that gets denied.
    //
    // So the CHECK still happens on entry (it is a settings read, and the
    // amber `Notifications are not silenced.` line depends on it) and only the
    // ASKING moves. The dialog is raised by the first Start press, where the
    // sentence is finally about something the operator is doing, and BOTH of
    // its buttons then continue into the start — "Not now" starts the scan
    // immediately, and "Open settings" leaves for the system screen, which is a
    // trip the operator chose. It is asked at most once either way
    // (`dndAccessAsked` is persisted), so the second scan is never interrupted.
    var dndAskArmed by remember { mutableStateOf(false) }
    val dndSettingsLauncher = rememberLauncherForActivityResult(
        // A special-access screen, not a runtime permission: the result code is
        // always RESULT_CANCELED, so the grant has to be RE-READ rather than
        // inferred from it.
        ActivityResultContracts.StartActivityForResult(),
    ) {
        dndScope.launch {
            viewModel.refreshDndNote(
                enabled = container.settingsRepository.settings.first().dndDuringCapture,
                granted = container.dndGuard.hasPolicyAccess,
            )
        }
    }
    LaunchedEffect(Unit) {
        val settings = container.settingsRepository.settings.first()
        val granted = container.dndGuard.hasPolicyAccess
        viewModel.refreshDndNote(enabled = settings.dndDuringCapture, granted = granted)
        if (com.lidarscan.core.capture.CaptureFocus.shouldAsk(
                enabled = settings.dndDuringCapture,
                granted = granted,
                alreadyAsked = settings.dndAccessAsked,
            )
            // ROUND 27 item 133(b), refined by the emulator: never over a
            // REPLAY session. A replay is a developer's errand against a file —
            // no walk, no tracker, nothing a notification buzz can shake — and
            // the two smoke tests that drive it click Start exactly once and
            // wait for a recording. An ask that intercepts that press is a
            // dialog raised about a risk that does not exist, in front of the
            // one path CI has for proving the decoder still works.
            && !isReplay
        ) {
            dndAskArmed = true
        }
    }
    if (showDndExplainer) {
        AlertDialog(
            // ROUND 16 item 61: dialogs inherited the theme's pill too.
            shape = RoundedCornerShape(ScanDims.DialogRadius),
            onDismissRequest = {
                showDndExplainer = false
                dndScope.launch { container.settingsRepository.setDndAccessAsked() }
                viewModel.startCapture()
            },
            title = { Text(com.lidarscan.core.capture.CaptureFocus.ASK_TITLE) },
            text = { Text(com.lidarscan.core.capture.CaptureFocus.ASK_BODY) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showDndExplainer = false
                        dndScope.launch { container.settingsRepository.setDndAccessAsked() }
                        runCatching {
                            dndSettingsLauncher.launch(container.dndGuard.policyAccessIntent())
                        }
                    },
                    modifier = Modifier.testTag("dndAskConfirm"),
                ) { Text(com.lidarscan.core.capture.CaptureFocus.ASK_CONFIRM) }
            },
            dismissButton = {
                TextButton(
                    onClick = {
                        // ROUND 27 item 133(b): "Not now" answers the question
                        // and then gets out of the way — the press that raised
                        // this dialog was a press of SCAN, and it still has to
                        // start a scan.
                        showDndExplainer = false
                        dndScope.launch { container.settingsRepository.setDndAccessAsked() }
                        viewModel.startCapture()
                    },
                    modifier = Modifier.testTag("dndAskDismiss"),
                ) { Text(com.lidarscan.core.capture.CaptureFocus.ASK_DISMISS) }
            },
            modifier = Modifier.testTag("dndAskDialog"),
        )
    }

    // ── ROUND 24 item 110(b): the tour ──────────────────────────────────────
    //
    // The state lives here rather than in `CaptureViewModel` on purpose: it is
    // pure screen chrome with no bearing on a capture, and the ViewModel is
    // 5,000 lines about recording. `rememberSaveable` is enough — a rotation
    // mid-tour keeps your place, and the persisted half (seen / offered) is in
    // `SettingsRepository` where every other device fact lives.
    val tutorialScope = androidx.compose.runtime.rememberCoroutineScope()
    var tutorialStepOrdinal by rememberSaveable { mutableStateOf(-1) }
    val tutorialState = com.lidarscan.core.capture.TutorialState(
        com.lidarscan.core.capture.TutorialStep.entries.getOrNull(tutorialStepOrdinal),
    )
    val tutorialSettings by container.settingsRepository.settings
        .collectAsStateWithLifecycle(initialValue = com.lidarscan.app.data.AppSettings())
    // Offered exactly once ever, on the screen it is about — and never over a
    // replay session, which is a developer's errand.
    var offerDismissed by rememberSaveable { mutableStateOf(false) }
    val offerTutorial = !isReplay && !offerDismissed && !tutorialState.running &&
        com.lidarscan.core.capture.ScanTutorial.shouldOffer(
            tutorialSeen = tutorialSettings.tutorialSeen,
            offerMade = tutorialSettings.tutorialOffered,
        )

    fun startTutorial() {
        tutorialStepOrdinal = 0
    }

    fun endTutorial() {
        tutorialStepOrdinal = -1
        // Skipping counts: item 110 says the offer never auto-repeats, and a
        // tour the operator started and abandoned is a tour they have met.
        tutorialScope.launch { container.settingsRepository.setTutorialSeen() }
    }

    // ROUND 24 item 110(b): Settings › About › Tutorial asks for a replay, and
    // the replay has to happen HERE because it is a tour of this screen. The
    // request is a one-shot on the container (it survives the tab hop that
    // carries it) and is spent the moment it is honoured.
    LaunchedEffect(Unit) {
        container.tutorialReplayRequest.collect { requested ->
            if (requested && !isReplay) {
                container.tutorialReplayRequest.value = false
                startTutorial()
            }
        }
    }

    // ── ROUND 27 item 142: THE TRACKER CAN BE DEAD, AND MUST SAY SO ────────
    //
    // The first user outside the owner — an OPPO CPH2499 — opened this tab six
    // times and never got a scan, because ColorOS handed ARCore the camera and
    // then took it back, and the app's only reaction was hundreds of identical
    // log lines. The rules are `ArTrouble` in `:core` (three seconds of a dead
    // camera is a fault; a blink is not; a missing APK explains everything
    // downstream of it); what lives here is the CLOCK and the three actions.
    //
    // `arErrorSince` is a plain remember rather than ViewModel state on
    // purpose: it is a property of what this screen has been looking at, it
    // must reset when the screen does, and a ViewModel that survives a tab hop
    // would carry a stale fault across one.
    val context = androidx.compose.ui.platform.LocalContext.current
    var arErrorSince by remember { mutableStateOf<Long?>(null) }
    val arStatusForTrouble by container.arController.status.collectAsStateWithLifecycle()
    LaunchedEffect(arStatusForTrouble.arError, arStatusForTrouble.sessionRunning) {
        arErrorSince = if (arStatusForTrouble.arError != null) {
            arErrorSince ?: android.os.SystemClock.elapsedRealtime()
        } else {
            null
        }
    }
    var arTroubleTick by remember { mutableStateOf(0L) }
    LaunchedEffect(arErrorSince) {
        // One tick a second while a failure is standing, so the three-second
        // threshold arrives on its own rather than waiting for the next
        // unrelated recomposition.
        while (arErrorSince != null) {
            arTroubleTick = android.os.SystemClock.elapsedRealtime()
            kotlinx.coroutines.delay(500)
        }
    }
    val arTrouble = com.lidarscan.core.capture.ArTrouble.kindFor(
        availabilityReady = arStatusForTrouble.availability.canRunAr,
        availabilityNeedsInstall =
            arStatusForTrouble.availability == com.lidarscan.app.ar.ArAvailability.NEEDS_INSTALL,
        availabilityUnsupported =
            arStatusForTrouble.availability == com.lidarscan.app.ar.ArAvailability.UNSUPPORTED,
        fatalSinceMillis = arErrorSince,
        nowMillis = maxOf(arTroubleTick, android.os.SystemClock.elapsedRealtime()),
    ).takeIf { viewModel.poseTrackingRequired }
        ?: com.lidarscan.core.capture.ArTroubleKind.NONE

    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val connectionState by viewModel.connectionState.collectAsStateWithLifecycle()
    val captureState by viewModel.captureState.collectAsStateWithLifecycle()
    // ROUND 17 item 64.
    val starting by viewModel.starting.collectAsStateWithLifecycle()
    // ROUND 20 item 78: the hold-steady stage.
    val startHold by viewModel.startHold.collectAsStateWithLifecycle()
    // ROUND 21 items 84/85: the unified start-progress panel — which stage the
    // start sequence is in, since when, and the gate's live verdict.
    val startProgress by viewModel.startProgress.collectAsStateWithLifecycle()
    val startWarmup by viewModel.startWarmup.collectAsStateWithLifecycle()
    // ── ROUND 23 ───────────────────────────────────────────────────────────
    val trackingBanner by viewModel.trackingBanner.collectAsStateWithLifecycle()
    val startTapRefusal by viewModel.startTapRefusal.collectAsStateWithLifecycle()
    val detailLevel by viewModel.detailLevel.collectAsStateWithLifecycle()
    // ROUND 20 item 83: the New-capture confirm.
    val showNewCaptureConfirm by viewModel.showNewCaptureConfirm.collectAsStateWithLifecycle()
    // ROUND 17 item 66: Developer Mode's answer, kept current on the one object
    // that acts on it. Collected here rather than passed into the ViewModel
    // because the flag has to be right at Start, and Start can happen long
    // after the ViewModel was built.
    LaunchedEffect(container) {
        container.settingsRepository.settings.collect {
            container.captureLog.developerCaptureDebug = it.developerMode && it.captureDebugLog
            // ROUND 25 item 118 (owner amendment): the connection-detection
            // debug channel rides the SAME seven-tap unlock, mirrored from the
            // same collector so there is one place the flag is read. Note it
            // is gated on `developerMode` alone and not on `captureDebugLog`:
            // the per-capture debug log is about a capture, and this is about
            // why a capture could not start at all.
            container.connectionDebugSweeper.enabled = it.developerMode
        }
    }
    val stats by viewModel.stats.collectAsStateWithLifecycle()
    val health by viewModel.deviceHealth.collectAsStateWithLifecycle()
    val sessionSummary by viewModel.sessionSummary.collectAsStateWithLifecycle()
    // ROUND 11 (owner item 44): the graded card.
    val scanSummary by viewModel.scanSummary.collectAsStateWithLifecycle()
    // ROUND 15 (item 55): auto-process on seal, reported on that same card.
    val autoProcess by viewModel.autoProcess.collectAsStateWithLifecycle()
    // ROUND 11 (owner item 45a): the hold-still ring.
    val mountHold by viewModel.mountHold.collectAsStateWithLifecycle()
    val pointCloudSource by viewModel.pointCloudSource.collectAsStateWithLifecycle()
    val colorMode by viewModel.colorMode.collectAsStateWithLifecycle()
    val colormap by viewModel.colormap.collectAsStateWithLifecycle()
    val pointSizePx by viewModel.pointSizePx.collectAsStateWithLifecycle()
    val cameraMode by viewModel.cameraMode.collectAsStateWithLifecycle()
    val liveSlam by viewModel.liveSlam.collectAsStateWithLifecycle()
    val liveView by viewModel.liveView.collectAsStateWithLifecycle()
    val refreshHz by viewModel.refreshHz.collectAsStateWithLifecycle()
    val refreshRequestToken by viewModel.refreshRequestToken.collectAsStateWithLifecycle()
    val gamma by viewModel.gamma.collectAsStateWithLifecycle()
    val brightness by viewModel.brightness.collectAsStateWithLifecycle()
    val keyframeStats by viewModel.keyframeStats.collectAsStateWithLifecycle()
    val keyframesEnabled by viewModel.keyframesEnabled.collectAsStateWithLifecycle()
    val keyframeRateFps by viewModel.keyframeRateFps.collectAsStateWithLifecycle()
    val lodBudgetMPoints by viewModel.lodBudgetMPoints.collectAsStateWithLifecycle()
    val displayParams by viewModel.displayParams.collectAsStateWithLifecycle()
    val sensor by viewModel.sensor.collectAsStateWithLifecycle()
    val profile by viewModel.profile.collectAsStateWithLifecycle()
    val scanName by viewModel.scanName.collectAsStateWithLifecycle()
    val autoConnectState = viewModel.autoConnectState?.collectAsStateWithLifecycle()?.value
    val manualDevices by viewModel.manualDevices.collectAsStateWithLifecycle()
    val manualLidarIp by viewModel.manualLidarIp.collectAsStateWithLifecycle()
    val manualHostIp by viewModel.manualHostIp.collectAsStateWithLifecycle()
    val mountIsNominal by viewModel.mountIsNominal.collectAsStateWithLifecycle()
    val trailPoints by viewModel.trailPoints.collectAsStateWithLifecycle()
    val trailLengthM by viewModel.trailLengthM.collectAsStateWithLifecycle()
    // ROUND 16 item 59: the walked path, drawn inside the live cloud.
    val trailRibbon by viewModel.trailRibbon.collectAsStateWithLifecycle()
    val showTrajectory by viewModel.showTrajectory.collectAsStateWithLifecycle()
    // ROUND 19 item 77.
    val showPreScanChecklist by viewModel.showPreScanChecklist.collectAsStateWithLifecycle()
    // ROUND 19 item 75: the guidance ring, polled at 1 Hz — deliberately not
    // per frame (round 13's cue budget applied to pixels: quiet, debounced,
    // and visual only).
    var coverageSectors by remember { mutableStateOf<FloatArray?>(null) }
    LaunchedEffect(Unit) {
        while (true) {
            kotlinx.coroutines.delay(1_000)
            coverageSectors = coverageRendererHolder.get()?.coverageSectors()
        }
    }
    var liveRibbonSink by remember {
        mutableStateOf<com.lidarscan.app.render.PointCloudRenderer?>(null)
    }
    LaunchedEffect(liveRibbonSink, trailRibbon, showTrajectory) {
        val r = liveRibbonSink ?: return@LaunchedEffect
        r.setTrailVisible(showTrajectory)
        r.setTrail(trailRibbon.xyz, trailRibbon.rgba, trailRibbon.count)
    }
    val motionHint by viewModel.motionHint.collectAsStateWithLifecycle()
    val refreshDownshiftNote by viewModel.refreshDownshiftNote.collectAsStateWithLifecycle()
    val georefSource by viewModel.georefSource.collectAsStateWithLifecycle()
    val georefNote by viewModel.georefNote.collectAsStateWithLifecycle()
    // ROUND 14 (owner item 52). 0.8.0 COMPUTED this sentence on every Start and
    // no composable ever collected it, so the owner's three field sessions all
    // recorded `dnd=unprotected-no-permission` with nothing on screen to say so.
    val dndNote by viewModel.dndNote.collectAsStateWithLifecycle()
    val arStatus = viewModel.arStatus?.collectAsStateWithLifecycle()?.value
    // ── ROUND 6 ─────────────────────────────────────────────────────────────
    val preset by viewModel.preset.collectAsStateWithLifecycle()
    val presetChangeNote by viewModel.presetChangeNote.collectAsStateWithLifecycle()
    val presetCaution by viewModel.presetCaution.collectAsStateWithLifecycle()
    val liveMapEnabled by viewModel.liveMapEnabled.collectAsStateWithLifecycle()
    val liveMapRequested by viewModel.liveMapRequested.collectAsStateWithLifecycle()
    val liveMapFullNote by viewModel.liveMapFullNote.collectAsStateWithLifecycle()
    val saveError by viewModel.saveError.collectAsStateWithLifecycle()
    val lastSavedProject by viewModel.lastSavedProject.collectAsStateWithLifecycle()
    val mountTrim by viewModel.mountTrim.collectAsStateWithLifecycle()
    val mountTrimNote by viewModel.mountTrimNote.collectAsStateWithLifecycle()
    // ── ROUND 7 ─────────────────────────────────────────────────────────────
    val mountTrimProvenance by viewModel.mountTrimProvenance.collectAsStateWithLifecycle()
    // ROUND 28 item 155.
    val startBlock by viewModel.startBlock.collectAsStateWithLifecycle()
    val noDataAlert by viewModel.noDataAlert.collectAsStateWithLifecycle()
    val noPoseAlert by viewModel.noPoseAlert.collectAsStateWithLifecycle()
    val sectionHint by viewModel.sectionHint.collectAsStateWithLifecycle()
    // ── ROUND 8 ─────────────────────────────────────────────────────────────
    val mountTrimNoteIsWarning by viewModel.mountTrimNoteIsWarning.collectAsStateWithLifecycle()

    // ROUND 8 (item 31): stop → seal → Projects, with the new scan selected.
    //
    // Keyed on the ViewModel so the collector is re-established with it and
    // never on two ViewModels at once; the flow itself has `replay = 0`, so
    // a recomposition cannot re-deliver a seal that has already navigated.
    //
    // ROUND 11 (owner item 44): ...and the jump WAITS for the summary card.
    //
    // The two features fight if this is left alone: ROUND 10 made the seal
    // navigate to Projects, and a card shown on the Capture screen at the same
    // instant would be visible for one frame before `goTab` disposes the whole
    // back-stack entry. So the id is HELD here and spent when the card is
    // dismissed. Nothing about the seal changes — `sealedProjectId` still
    // emits exactly when it did, still with `replay = 1`, so a recomposition
    // (or an Activity recreation mid-seal, which is ROUND 10's own bug) still
    // recovers the pending navigation.
    //
    // ── ROUND 23 item 101: AND IT IS SPENT WHEN IT IS USED ──────────────────
    //
    // This is the defect the owner reported as "the scan button is dead". The
    // paragraph above says the buffer "cannot survive to re-navigate later"
    // because nav destroys the ViewModel — and ROUND 22 item 88 stopped nav
    // destroying the ViewModel. So on every return to the Scan tab this
    // collector re-attached, the buffered id replayed, and the operator was
    // bounced straight back to Projects before he could press anything. There
    // is no log line for it because the `navigate -> Projects` line is written
    // at the emit. Killing the app was the only cure, and killing the app is
    // what he did between every scan.
    //
    // [CaptureViewModel.sealNavigationHandled] spends the event at the instant
    // it is acted on, and logs that it did. ROUND 10's property is untouched:
    // a collector that attaches LATE still receives the id, because the
    // buffer is only dropped by a collector that has used it.
    var pendingNavigationId by remember { mutableStateOf<String?>(null) }
    if (onScanSealed != null) {
        LaunchedEffect(viewModel) {
            viewModel.sealedProjectId.collect { pendingNavigationId = it }
        }
        LaunchedEffect(pendingNavigationId, sessionSummary) {
            val id = pendingNavigationId ?: return@LaunchedEffect
            // A seal with no summary (a path that produced none) navigates
            // immediately, exactly as ROUND 10 shipped it.
            if (sessionSummary != null) return@LaunchedEffect
            pendingNavigationId = null
            viewModel.sealNavigationHandled(id)
            onScanSealed(id)
        }
    }

    // ── ROUND 23 item 101(a): the tab re-arms on entry ──────────────────────
    //
    // Keyed on the ViewModel so it runs once per screen instance — which,
    // since item 88, is once per trip INTO the tab rather than once per
    // ViewModel. A live capture is left strictly alone; see
    // [CaptureViewModel.onScanScreenEntered].
    LaunchedEffect(viewModel) { viewModel.onScanScreenEntered() }

    // ── ROUND 24 item 111: and the teardown says WHY it is tearing down ─────
    //
    // A rotation and a tab switch are the same Compose sequence — dispose,
    // then compose again against the same (item-88-preserved) ViewModel — so
    // the screen cannot tell them apart and the Activity can. Read at dispose
    // time rather than captured earlier: `isChangingConfigurations` is only
    // true once the system has decided, which is exactly when this runs.
    val captureActivity = androidx.compose.ui.platform.LocalContext.current.findActivity()
    DisposableEffect(viewModel) {
        onDispose {
            viewModel.onScanScreenLeaving(captureActivity?.isChangingConfigurations == true)
        }
    }

    // ROUND 6 (owner item 19): the AR path degraded. Fall back to the 3D-orbit
    // view rather than leaving the operator staring at a black overlay — the
    // inline message below says what happened, and every other capture function
    // keeps working.
    LaunchedEffect(arStatus?.arError) {
        if (arStatus?.arError != null && cameraMode == CameraMode.AR) {
            viewModel.setCameraMode(CameraMode.ORBIT)
        }
    }

    // B9: the fix strip. A replay session has no rover, so it shows the same
    // "no fix" chips a disconnected one does — which is the truth, not a gap.
    val fix by container.rtkManager.fix.collectAsStateWithLifecycle()
    val ntrip by container.rtkManager.ntrip.collectAsStateWithLifecycle()

    // ROUND 5: the Mid-360 heartbeat detector prefers the Ethernet link's own
    // Network when one exists, so the monitor runs while this screen is up (and
    // only while it is up — an always-registered NetworkCallback is a battery cost
    // on the projects list).
    DisposableEffect(isReplay) {
        if (!isReplay) container.ethernetMonitor.start()
        onDispose { if (!isReplay) container.ethernetMonitor.stop() }
    }

    // B7: the AR session belongs to the whole app (one ARCore Session per
    // process), so the Capture screen resumes and pauses it around its own
    // lifetime rather than creating one.
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        // ROUND 6 (owner item 19, crash cause 1): this used to call
        // `createSession()` and STOP. Granting the camera permission — which,
        // since ROUND 5 moved the request off screen entry, is the normal first
        // time anyone enables the AR overlay — therefore produced a live but
        // NEVER-RESUMED ARCore session, while the overlay's GLSurfaceView was
        // already spinning at RENDERMODE_CONTINUOUSLY. Its first
        // `Session.update()` threw `SessionPausedException` on the render
        // thread, uncaught, and the process died. `ArSessionGate` now makes that
        // structurally impossible; resuming here is what makes AR actually
        // WORK after a grant rather than merely not crash.
        if (granted) {
            container.arController.createSession()
            container.arController.resume()
        }
    }

    // ROUND 5: the camera permission is asked for **when the camera is actually
    // needed**, not on entering the tab.
    //
    // Before this, opening Capture immediately threw up the system camera dialog —
    // which is a modal interruption in front of a screen whose whole point is that
    // it has no steps, and on a Mid-360 walk the camera may never be needed at all.
    // Now it is requested when one of three things is true: a phone-tracked D6 is
    // connected (its 3D depends on the pose stream), the AR view is selected, or a
    // recording is running with keyframes on. Caught by the emulator smoke test,
    // which found the dialog covering the Capture tab it was trying to walk.
    val needsArSession = viewModel.arAvailable && (
        cameraMode == CameraMode.AR ||
            (viewModel.poseTrackingRequired && autoConnectState?.isPreviewing == true) ||
            captureState == CaptureState.RECORDING ||
            captureState == CaptureState.PAUSED
        )

    // ── ROUND 22 item 89: pause only what this screen still owns ───────────
    //
    // `container.arController` is process-wide; this effect is per screen
    // instance. Before this round `onDispose` called `pause()` unconditionally,
    // so a Scan screen being replaced paused the session the REPLACEMENT had
    // already created and resumed — and with item 88's churn that replacement
    // happened on every tab switch. `beginSessionUse`/`endSessionUse` make it a
    // lease: a superseded screen's teardown is a no-op.
    val sessionLease = remember { java.util.concurrent.atomic.AtomicReference<Any?>(null) }
    LaunchedEffect(needsArSession) {
        if (!needsArSession) return@LaunchedEffect
        container.arController.refreshAvailability()
        if (!container.hasCameraPermission()) {
            permissionLauncher.launch(android.Manifest.permission.CAMERA)
        } else {
            sessionLease.set(container.arController.beginSessionUse())
            container.arController.createSession()
            container.arController.resume()
        }
    }
    DisposableEffect(needsArSession) {
        onDispose {
            if (viewModel.arAvailable) {
                container.arController.endSessionUse(sessionLease.getAndSet(null))
            }
        }
    }

    // ── ROUND 30 item 175: the attitude instrument's feed, and only then ────
    //
    // The same shape as the session lease above, on the predicate that decides
    // whether either placement of the instrument is drawn at all: the
    // hold-still card (`startHold != null`, `StartHoldModal`) and the recording
    // control strip (`live`, `ScanControlCluster`). Nothing else in the app
    // draws a needle, so nothing else needs a gravity listener — an idle Scan
    // tab, the Projects list and a backgrounded app all cost exactly zero.
    //
    // `DisposableEffect` rather than a collector: `acquire`/`release` are
    // ref-counted, and a screen leaving by a tab switch, by back, or by process
    // rebuild all end in the same `onDispose`. The ARCore pose tap is wired
    // once in `AppContainer` and is itself a no-op while nothing is acquired,
    // so there is one lifecycle here and not two.
    val attitudeWanted = startHold != null ||
        captureState == CaptureState.RECORDING ||
        captureState == CaptureState.PAUSED
    DisposableEffect(attitudeWanted) {
        if (attitudeWanted) container.attitudeSource.acquire()
        onDispose { if (attitudeWanted) container.attitudeSource.release() }
    }

    // ROUND 5.3 (item 18): the screen stays awake while a capture is running.
    // ── ROUND 26 item 124 (owner choice C): the tab bar, while scanning ────
    //
    // One boolean up to the shell, owned here. `starting` is included on
    // purpose: the start sequence can hold for four to eight seconds behind
    // the round-12 tracking gate, and a tab bar that waits until RECORDING to
    // get out of the way slides away underneath the operator's thumb at the
    // exact moment the hold-still card appears.
    //
    // The dispose arm is the safety half. A scan screen can leave the
    // composition by a tab switch, by system back, or by the process being
    // rebuilt; in every one of those the bar must come back, and clearing it
    // where the screen ends is the only place that covers all three.
    // (Round 25 item 115 has already stopped and sealed the capture itself by
    // then — this is the chrome catching up, not a second policy.)
    val scanBusy = captureState == CaptureState.RECORDING ||
        captureState == CaptureState.PAUSED ||
        starting
    DisposableEffect(scanBusy) {
        container.scanInProgress.value = scanBusy
        onDispose { container.scanInProgress.value = false }
    }

    // ── ROUND 26 item 125(c): the orientation LOCKS at GO ───────────────────
    //
    // Turning the phone mid-scan is SCANNING MOTION, not a UI event. Two
    // separate things go wrong if the Activity is allowed to follow it:
    //
    //  1. The mount reference is measured once, during the hold-still stage,
    //     against the orientation the operator started in. Re-deriving it
    //     mid-capture would move the extrinsic under a running recording; not
    //     re-deriving it while the UI rotates would tell the operator the app
    //     had adapted when it had not.
    //  2. This Activity declares no `configChanges`, so a rotation DESTROYS
    //     and rebuilds it — mid-walk, with an ARCore session and a USB serial
    //     stream attached. Round 24's `isChangingConfigurations` discriminator
    //     makes that survivable (the capture is deliberately not stopped by
    //     it), but "survivable" is not "free": it is a full Compose teardown
    //     and an AR re-attach during the seconds the operator is walking.
    //
    // So the simpler of item 125(c)'s two options ships: the UI FREEZES.
    // `SCREEN_ORIENTATION_LOCKED` pins whatever orientation the scan started
    // in; the release is `UNSPECIFIED`, not `SENSOR`, because `SENSOR` would
    // override a user who has auto-rotate switched off.
    DisposableEffect(scanBusy, captureActivity) {
        val activity = captureActivity
        if (activity != null && scanBusy) {
            activity.requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LOCKED
        }
        onDispose {
            activity?.requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
    }

    // A walkthrough is minutes of walking with the phone in one hand and nothing
    // being touched — the default screen timeout would black it out mid-walk, and
    // on some OEM skins that also stops the camera the D6's pose stream depends on.
    // Scoped to the recording, not the screen: an idle Capture tab still sleeps.
    val view = androidx.compose.ui.platform.LocalView.current
    val keepAwake = captureState == CaptureState.RECORDING || captureState == CaptureState.PAUSED
    DisposableEffect(keepAwake) {
        view.keepScreenOn = keepAwake
        onDispose { view.keepScreenOn = false }
    }

    CaptureScreen(
        uiState = uiState,
        connectionState = connectionState,
        captureState = captureState,
        stats = stats,
        health = health,
        fix = fix,
        ntrip = ntrip,
        georefSource = georefSource,
        georefNote = georefNote,
        dndNote = dndNote,
        sessionSummary = sessionSummary,
        scanSummary = scanSummary,
        autoProcess = autoProcess,
        mountHold = mountHold,
        pointCloudSource = pointCloudSource,
        colorMode = colorMode,
        colormap = colormap,
        pointSizePx = pointSizePx,
        lodBudgetMPoints = lodBudgetMPoints,
        displayParams = displayParams,
        cameraMode = cameraMode,
        liveSlam = liveSlam,
        liveView = liveView,
        refreshHz = refreshHz,
        refreshRequestToken = refreshRequestToken,
        gamma = gamma,
        brightness = brightness,
        isReplaySession = viewModel.isReplaySession,
        starting = starting,
        startHold = startHold,
        startProgress = startProgress,
        startWarmup = startWarmup,
        trackingBanner = trackingBanner,
        detailLevels = viewModel.detailLevels,
        detailLevel = detailLevel,
        detailCeilingNote = viewModel.detailCeilingNote,
        // ROUND 29 item 172.
        detailReadout = viewModel.detailReadout(detailLevel),
        onDetailChange = viewModel::setDetailLevel,
        // ROUND 23 item 106(c): contextual on the SENSOR, never on the switch —
        // `SimpleMode` has said so since round 22 and this is the surface that
        // finally asks. A D6 operator sees neither chip.
        onOpenMid360Setup = onOpenMid360Setup
            ?.takeIf { com.lidarscan.core.SimpleMode.showsMid360Connect(advanced, sensor) },
        onOpenRtk = onOpenRtk
            ?.takeIf { com.lidarscan.core.SimpleMode.showsRtk(advanced, sensor) },
        // ── ROUND 24 item 110(b) ────────────────────────────────────────
        tutorialState = tutorialState,
        onStartTutorial = { startTutorial() },
        onTutorialNext = {
            val next = com.lidarscan.core.capture.ScanTutorial.next(tutorialState)
            if (next.running) tutorialStepOrdinal = next.step!!.ordinal else endTutorial()
        },
        onTutorialSkip = { endTutorial() },
        offerTutorial = offerTutorial,
        onAcceptTutorialOffer = { startTutorial() },
        onDismissTutorialOffer = {
            offerDismissed = true
            tutorialScope.launch { container.settingsRepository.setTutorialOffered() }
        },
        // ── ROUND 27 item 142(b) ────────────────────────────────────────
        arTrouble = arTrouble,
        onRetryAr = {
            arErrorSince = null
            container.arController.refreshAvailability()
            dndScope.launch(kotlinx.coroutines.Dispatchers.Default) {
                container.arController.resetWorldFrame(
                    attempts = com.lidarscan.app.ar.CaptureArController.RESET_ATTEMPTS,
                )
            }
        },
        onUpdateArServices = {
            // The Play listing for Google Play Services for AR. `market://`
            // first because it opens the store app directly; the https fallback
            // is for a device with no store app, which is exactly the kind of
            // device this card is for.
            val ctx = context
            val ok = runCatching {
                ctx.startActivity(
                    android.content.Intent(
                        android.content.Intent.ACTION_VIEW,
                        Uri.parse("market://details?id=com.google.ar.core"),
                    ).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK),
                )
            }.isSuccess
            if (!ok) {
                runCatching {
                    ctx.startActivity(
                        android.content.Intent(
                            android.content.Intent.ACTION_VIEW,
                            Uri.parse("https://play.google.com/store/apps/details?id=com.google.ar.core"),
                        ).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK),
                    )
                }
            }
        },
        onSendLogs = onOpenProfile,
        startTapRefusal = startTapRefusal,
        onDismissStartTapRefusal = viewModel::dismissStartTapRefusal,
        onStartRefused = viewModel::reportStartTapRefused,
        showNewCaptureConfirm = showNewCaptureConfirm,
        onNewCapture = viewModel::requestNewCapture,
        onConfirmNewCapture = viewModel::confirmNewCapture,
        onDismissNewCaptureConfirm = viewModel::dismissNewCaptureConfirm,
        arAvailable = viewModel.arAvailable,
        arTracking = arStatus?.tracking == true,
        arSessionRunning = arStatus?.sessionRunning == true,
        arSessionWanted = needsArSession,
        arTrackingHint = arStatus?.trackingHint,
        arTrackingLossEpisodes = arStatus?.trackingLossEpisodes ?: 0,
        posesPushed = arStatus?.posesPushed ?: 0L,
        poseTrackingRequired = viewModel.poseTrackingRequired,
        mountIsNominal = mountIsNominal,
        keyframeStats = keyframeStats,
        keyframesEnabled = keyframesEnabled,
        keyframeRateFps = keyframeRateFps,
        sensor = sensor,
        profile = profile,
        scanName = scanName,
        autoConnectState = autoConnectState,
        manualDevices = manualDevices,
        manualLidarIp = manualLidarIp,
        manualHostIp = manualHostIp,
        trailPoints = trailPoints,
        trailLengthM = trailLengthM,
        showPreScanChecklist = showPreScanChecklist,
        onChecklistStart = viewModel::startFromChecklist,
        onChecklistDismiss = viewModel::dismissPreScanChecklist,
        coverageSectors = coverageSectors,
        motionHint = motionHint,
        refreshDownshiftNote = refreshDownshiftNote,
        onRefreshAutoDownshift = viewModel::onRefreshAutoDownshift,
        arOverlay = { modifier ->
            ArOverlayView(
                controller = container.arController,
                source = pointCloudSource,
                colorMode = colorMode,
                colormap = colormap,
                pointSizePx = pointSizePx,
                modifier = modifier,
            )
        },
        // ROUND 5 (item 11): the pose pump. Mounted alongside the 3D-orbit view so
        // a D6 capture records poses in EITHER view mode — see ArPosePumpView for
        // the bug this closes.
        arPosePump = { modifier -> ArPosePumpView(controller = container.arController, modifier = modifier) },
        onBack = onBack,
        // ROUND 27 item 133(b): the one place the deferred ask is raised.
        onStart = {
            if (dndAskArmed) {
                dndAskArmed = false
                showDndExplainer = true
            } else {
                viewModel.startCapture()
            }
        },
        onPause = viewModel::pauseCapture,
        onResume = viewModel::resumeCapture,
        onStop = viewModel::stopCapture,
        onDismissSummary = viewModel::dismissSessionSummary,
        onScanNameChange = viewModel::setScanName,
        onRetryAutoDetect = viewModel::retryAutoDetect,
        onShowManualEntry = viewModel::showManualEntry,
        onHideManualEntry = { viewModel.hideManualEntry() },
        onManualDeviceConnect = viewModel::connectManualSerialLidar,
        onManualLidarIpChange = viewModel::setManualLidarIp,
        onManualHostIpChange = viewModel::setManualHostIp,
        onManualMid360Connect = viewModel::connectManualMid360,
        onLiveViewChange = viewModel::setLiveView,
        onLiveSlamChange = viewModel::setLiveSlam,
        onProfileChange = viewModel::setProfile,
        onRefreshHzChange = viewModel::setRefreshHz,
        onGammaChange = viewModel::setGamma,
        onBrightnessChange = viewModel::setBrightness,
        onColorModeChange = viewModel::setColorMode,
        onColormapChange = viewModel::setColormap,
        onPointSizeChange = viewModel::setPointSizePx,
        onLodChange = viewModel::setLodBudgetMPoints,
        onCameraModeChange = viewModel::setCameraMode,
        onKeyframesEnabledChange = viewModel::setKeyframesEnabled,
        onKeyframeRateChange = viewModel::setKeyframeRateFps,
        // ROUND 8 (live 3D map): the FOLLOW camera's rig-pose feed. The
        // renderer is Compose-scoped and the ARCore frame stream is
        // ViewModel-scoped; this is where the two meet, and it is a lambda
        // rather than a renderer reference so the ViewModel never holds a
        // GL-thread object across its own lifetime.
        onRendererChanged = { renderer ->
            viewModel.setRigPoseSink(
                renderer?.let { r -> { x, y, z, t -> r.setRigPose(x, y, z, t) } },
            )
            // ROUND 19 item 75: same seam, same lifetime rule.
            coverageRendererHolder.set(renderer)
            // ROUND 16 item 59: the ribbon meets the renderer at the same seam
            // the rig pose does, and for the same reason — the ViewModel must
            // never hold a GL-thread object across its own lifetime, so the
            // Compose side owns the subscription and tears it down with the
            // view.
            liveRibbonSink = renderer
        },
        onOpenMountCalibration = (uiState as? CaptureUiState.Loaded)?.project?.id?.let { pid ->
            onOpenMountCalibration?.let { open -> { open(pid) } }
        },
        // ── ROUND 6 ────────────────────────────────────────────────────────
        preset = preset,
        presetChangeNote = presetChangeNote,
        presetCaution = presetCaution,
        deviceTierLabel = viewModel.deviceTierLabel,
        onPresetChange = viewModel::setPreset,
        onDismissPresetNote = viewModel::dismissPresetChangeNote,
        liveMapEnabled = liveMapEnabled,
        liveMapRequested = liveMapRequested,
        liveMapFullNote = liveMapFullNote,
        onLiveMapEnabledChange = viewModel::setLiveMapEnabled,
        arErrorMessage = arStatus?.arError,
        saveError = saveError,
        onDismissSaveError = viewModel::dismissSaveError,
        lastSavedProject = lastSavedProject,
        mountTrim = mountTrim,
        mountTrimNote = mountTrimNote,
        mountTrimNoteIsWarning = mountTrimNoteIsWarning,
        mountTrimProvenance = mountTrimProvenance,
        noDataAlert = noDataAlert,
        onDismissNoDataAlert = viewModel::dismissNoDataAlert,
        noPoseAlert = noPoseAlert,
        onDismissNoPoseAlert = viewModel::dismissNoPoseAlert,
        sectionHint = sectionHint,
        onSetMountReference = viewModel::setMountReference,
        onBeginMountHold = { viewModel.beginMountHold() },
        onCancelMountHold = viewModel::cancelMountHold,
        onClearMountReference = viewModel::clearMountReference,
        onDismissMountTrimNote = viewModel::dismissMountTrimNote,
        attitude = container.attitudeSource.attitude,
        // ROUND 33 item 179(b): the hold-still card's one haptic tick, on the
        // edge where the posture goes out of tolerance. Through the ViewModel
        // and not straight to the player, because that is where the operator's
        // "cues" setting is already read and armed once per session.
        onPostureLost = viewModel::onPostureLost,
        // ROUND 28 item 155: the start sequence's terminal states.
        startBlock = startBlock,
        onStartBlockRetry = viewModel::retryStartAfterBlock,
        onStartAnyway = viewModel::startAnyway,
        onDismissStartBlock = viewModel::dismissStartBlock,
        nowMillis = System.currentTimeMillis(),
    )
}

/**
 * ROUND 5's Capture screen — one screen, no wizard, no dialogs.
 *
 * Top to bottom: app bar → georeference + RTK chips → **the pre-capture strip**
 * (name field, auto-detect status, the inline manual fallback, the D6 mount hint)
 * → hero viewport → four mono stats → transport. The strip collapses to nothing
 * once a recording is running, so a live capture looks exactly like it did in
 * round 4 apart from the Live toggle.
 *
 * What used to be steps and is not any more:
 *  * the **new-project screen** (name/sensor/profile) — the name is one inline
 *    field with an auto-name placeholder, the sensor is whatever auto-detect
 *    found, and the profile lives in the settings sheet;
 *  * the **project picker** — the tab never records into an existing project;
 *  * the **connect wizards** — auto-detect connects and shows points, and when it
 *    finds nothing the manual fields are *already open* under the status line;
 *  * the **self-test gate** — round 5 item 7: live points are the proof.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CaptureScreen(
    uiState: CaptureUiState,
    connectionState: ConnectionState,
    captureState: CaptureState,
    stats: CaptureStats,
    health: DeviceHealth?,
    fix: GnssFixSnapshot,
    ntrip: NtripStatsSnapshot,
    georefSource: GeorefSourceState,
    georefNote: String?,
    dndNote: String?,
    sessionSummary: CaptureStats?,
    autoProcess: AutoProcessState = AutoProcessState(),
    scanSummary: com.lidarscan.core.capture.ScanSummary? = null,
    mountHold: com.lidarscan.core.calib.MountTrimRefiner.Progress? = null,
    pointCloudSource: PointCloudSource?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    lodBudgetMPoints: Int,
    displayParams: com.lidarscan.core.render.DisplayParams,
    cameraMode: CameraMode,
    liveSlam: Boolean,
    liveView: Boolean,
    refreshHz: Int,
    refreshRequestToken: Int,
    gamma: Float,
    brightness: Float,
    isReplaySession: Boolean,
    /** ROUND 17 item 64: a Start is in flight — see TransportRow's `starting`. */
    starting: Boolean = false,
    /** ROUND 20 item 78: the Start hold-steady stage's banner state, or null. */
    startHold: CaptureViewModel.StartHoldState? = null,
    /** ROUND 21 item 85: the start sequence's stage/elapsed/pulse state, or null. */
    startProgress: CaptureViewModel.StartProgress? = null,
    /** ROUND 21 item 85: the ROUND 12 gate's live verdict while it holds Start. */
    startWarmup: com.lidarscan.core.capture.TrackingWarmup.Verdict? = null,
    /** ROUND 23 item 105: amber while the tracker is blind, green for 2 s after. */
    trackingBanner: com.lidarscan.core.capture.TrackingBannerState =
        com.lidarscan.core.capture.TrackingBannerState(),
    // ── ROUND 23 item 106(a): DETAIL — Auto / High / Max ────────────────────
    detailLevels: List<com.lidarscan.core.capture.DetailLevel> =
        com.lidarscan.core.capture.DetailLevel.entries,
    detailLevel: com.lidarscan.core.capture.DetailLevel = com.lidarscan.core.capture.DetailLevels.DEFAULT,
    detailCeilingNote: String? = null,
    /** ROUND 29 item 172: `Fits this device`, or the rung's own budget. */
    detailReadout: String? = null,
    onDetailChange: (com.lidarscan.core.capture.DetailLevel) -> Unit = {},
    /** ROUND 23 item 106(c): non-null shows the Mid-360 setup chip on the Scan tab. */
    onOpenMid360Setup: (() -> Unit)? = null,
    /** ROUND 23 item 106(c): non-null shows the RTK chip on the Scan tab. */
    onOpenRtk: (() -> Unit)? = null,
    // ── ROUND 24 item 110(b): the guided tour ───────────────────────────────
    /** Which step is showing, or none. See [com.lidarscan.core.capture.ScanTutorial]. */
    tutorialState: com.lidarscan.core.capture.TutorialState =
        com.lidarscan.core.capture.TutorialState(),
    /** The ? button. */
    onStartTutorial: () -> Unit = {},
    onTutorialNext: () -> Unit = {},
    onTutorialSkip: () -> Unit = {},
    /** The one-time first-run offer. */
    offerTutorial: Boolean = false,
    onAcceptTutorialOffer: () -> Unit = {},
    onDismissTutorialOffer: () -> Unit = {},
    /** ROUND 23 item 101(b): the reason the last press could not start anything. */
    /** ROUND 27 item 142(b): what is wrong with position tracking, if anything. */
    arTrouble: com.lidarscan.core.capture.ArTroubleKind =
        com.lidarscan.core.capture.ArTroubleKind.NONE,
    onRetryAr: () -> Unit = {},
    onUpdateArServices: () -> Unit = {},
    onSendLogs: (() -> Unit)? = null,
    startTapRefusal: String? = null,
    onDismissStartTapRefusal: () -> Unit = {},
    onStartRefused: (String) -> Unit = {},
    /** ROUND 20 item 83: the New-capture confirm dialog (a capture is live). */
    showNewCaptureConfirm: Boolean = false,
    onNewCapture: () -> Unit = {},
    onConfirmNewCapture: () -> Unit = {},
    onDismissNewCaptureConfirm: () -> Unit = {},
    arAvailable: Boolean,
    arTracking: Boolean,
    arSessionRunning: Boolean,
    /** ROUND 23 item 101: this screen wants an AR session right now (see CaptureViewport). */
    arSessionWanted: Boolean = false,
    arTrackingHint: String?,
    arTrackingLossEpisodes: Int,
    posesPushed: Long,
    /** ROUND 5 item 11: true when this sensor's 3D depends on the phone's pose stream (the D6). */
    poseTrackingRequired: Boolean,
    /** True while the pushbroom is running on the CAD nominal rather than a measured calibration. */
    mountIsNominal: Boolean,
    keyframeStats: com.lidarscan.app.ar.KeyframeRecorder.Stats,
    keyframesEnabled: Boolean,
    keyframeRateFps: Int,
    sensor: SensorType,
    profile: WorkflowProfile,
    scanName: String,
    autoConnectState: CaptureAutoConnectState?,
    manualDevices: List<ManualSerialDevice>,
    manualLidarIp: String,
    manualHostIp: String,
    /** ROUND 5.3 (item 18): the walked path, fitted to 0..1, drawn over the live view. */
    trailPoints: List<com.lidarscan.core.capture.TrajectoryTrail.NormalizedPoint>,
    trailLengthM: Float,
    /** ROUND 5.3 (item 18): the gentle "slow down" line — inline; the numbers stay in Diagnostics. */
    motionHint: String?,
    /** ROUND 5.3 (item 17): non-null once the live view has been auto-eased down a notch. */
    refreshDownshiftNote: String?,
    onRefreshAutoDownshift: (Int) -> Unit,
    arOverlay: @Composable (Modifier) -> Unit,
    arPosePump: @Composable (Modifier) -> Unit,
    onBack: () -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
    onDismissSummary: () -> Unit,
    onScanNameChange: (String) -> Unit,
    onRetryAutoDetect: () -> Unit,
    onShowManualEntry: () -> Unit,
    onHideManualEntry: () -> Unit,
    onManualDeviceConnect: (ManualSerialDevice, SensorType) -> Unit,
    onManualLidarIpChange: (String) -> Unit,
    onManualHostIpChange: (String) -> Unit,
    onManualMid360Connect: () -> Unit,
    onLiveViewChange: (Boolean) -> Unit,
    onLiveSlamChange: (Boolean) -> Unit,
    onProfileChange: (WorkflowProfile) -> Unit,
    onRefreshHzChange: (Int) -> Unit,
    onGammaChange: (Float) -> Unit,
    onBrightnessChange: (Float) -> Unit,
    onColorModeChange: (ColorMode) -> Unit,
    onColormapChange: (Colormap) -> Unit,
    onPointSizeChange: (Float) -> Unit,
    onLodChange: (Int) -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
    onKeyframesEnabledChange: (Boolean) -> Unit,
    onKeyframeRateChange: (Int) -> Unit,
    /** ROUND 8: the live renderer, for the FOLLOW camera's rig-pose feed. */
    onRendererChanged: (com.lidarscan.app.render.PointCloudRenderer?) -> Unit = {},
    onOpenMountCalibration: (() -> Unit)? = null,
    // ── ROUND 6 (owner items 19–23) ─────────────────────────────────────────
    /** Item 22: the Light / Optimal / Full chip row's selection, or CUSTOM. */
    preset: com.lidarscan.core.capture.PerformancePreset =
        com.lidarscan.core.capture.PerformancePresets.DEFAULT,
    /** Item 22: "switching preset shows what it changed". */
    presetChangeNote: String? = null,
    /** Item 22: the inline caution for a preset this phone will struggle with. */
    presetCaution: String? = null,
    deviceTierLabel: String = "standard",
    onPresetChange: (com.lidarscan.core.capture.PerformancePreset) -> Unit = {},
    onDismissPresetNote: () -> Unit = {},
    /** Item 22 (Light): whether the live map is drawn at all. */
    liveMapEnabled: Boolean = true,
    /** Item 21: the live map is genuinely on screen — live SLAM or the D6 pushbroom, and not switched off. */
    liveMapRequested: Boolean = false,
    /** Item 21: non-null once the engine's live page store filled and the map stopped growing. */
    liveMapFullNote: String? = null,
    onLiveMapEnabledChange: (Boolean) -> Unit = {},
    /** Item 19: non-null once the AR path failed — shown inline, never a crash. */
    arErrorMessage: String? = null,
    /** Item 20: non-null when a capture could not be saved. The loudest thing on this screen. */
    saveError: String? = null,
    onDismissSaveError: () -> Unit = {},
    /** Item 20: where the last completed capture landed, for the summary sheet. */
    lastSavedProject: String? = null,
    /** Item 23: this session's mount trim, or null. */
    mountTrim: com.lidarscan.core.calib.MountTrim? = null,
    mountTrimNote: String? = null,
    /**
     * ROUND 8 (item 30b): true when [mountTrimNote] is a REFUSAL rather than a
     * confirmation, so the screen can shout it instead of whispering it. The
     * owner tapped Set mount reference eight times in one session against a
     * grey one-liner and concluded it did nothing.
     */
    mountTrimNoteIsWarning: Boolean = false,
    /** ROUND 7 (field bug 1): where the trim in force came from, and how old it is. */
    mountTrimProvenance: com.lidarscan.core.calib.MountTrimProvenance? = null,
    // ── ROUND 19 item 77: the pre-scan checklist ─────────────────────────────
    showPreScanChecklist: Boolean = false,
    /** The checklist's Start; the Boolean is "don't show again". */
    onChecklistStart: (Boolean) -> Unit = {},
    /** The checklist's back-out; same Boolean. */
    onChecklistDismiss: (Boolean) -> Unit = {},
    // ── ROUND 19 item 75: per-sector coverage for the trail tile's ring ──────
    coverageSectors: FloatArray? = null,
    /** ROUND 7 (field bug 2): non-null while a running capture is receiving nothing. */
    noDataAlert: String? = null,
    onDismissNoDataAlert: () -> Unit = {},
    noPoseAlert: String? = null,
    onDismissNoPoseAlert: () -> Unit = {},
    /** ROUND 7 (item 3): non-null once ARCore's frame has jumped and the scan is in sections. */
    sectionHint: String? = null,
    onSetMountReference: () -> Unit = {},
    onBeginMountHold: () -> Unit = {},
    onCancelMountHold: () -> Unit = {},
    onClearMountReference: () -> Unit = {},
    onDismissMountTrimNote: () -> Unit = {},
    /**
     * ROUND 28 item 168 + **ROUND 30 item 175**: the attitude instrument's
     * source, as a flow rather than as a value.
     *
     * Round 28 passed `startOrientation?.screenUpAngleDeg` here — round 26 item
     * 125(b)'s **property of the capture**, written once when the mount hold
     * settles and, by its own KDoc, "left alone for the rest of the capture".
     * The instrument was therefore fed a constant: nothing at all while the
     * hold-still card was up, and one frozen angle for the whole walk. It did
     * not move on the owner's Pixel because there was nothing moving in it.
     *
     * A `StateFlow` rather than a `Double?` so the 20 Hz read is scoped to the
     * two composables that draw a needle. Collecting it up here would recompose
     * the whole Scan screen twenty times a second to turn a 34 dp line.
     */
    attitude: kotlinx.coroutines.flow.StateFlow<com.lidarscan.core.calib.HoldOrientation?> =
        kotlinx.coroutines.flow.MutableStateFlow(null),
    /**
     * ROUND 33 item 179(b): fired once each time the hold-still card's posture
     * crosses from inside the tolerance to outside it. The recording strip has
     * no equivalent, on purpose — see `CueKind.POSTURE_OFF`.
     */
    onPostureLost: () -> Unit = {},
    /** ROUND 28 item 155: the start sequence stopped and is asking. */
    startBlock: CaptureViewModel.StartBlock? = null,
    onStartBlockRetry: () -> Unit = {},
    onStartAnyway: () -> Unit = {},
    onDismissStartBlock: () -> Unit = {},
    nowMillis: Long = 0L,
) {
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    var sheet by remember { mutableStateOf(CaptureSheet.NONE) }
    val configSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val settingsSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val diagSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    val project = (uiState as? CaptureUiState.Loaded)?.project
    val newScan = uiState as? CaptureUiState.NewScan
    val connected = connectionState == ConnectionState.CONNECTED
    val recording = captureState == CaptureState.RECORDING
    val paused = captureState == CaptureState.PAUSED
    val live = recording || paused

    // ── ROUND 23 item 101(b) ────────────────────────────────────────────────
    //
    // Why the scan button will not start anything, standing — not only after a
    // tap. "If it is disabled, the UI must say why in ≤6 words" is the rule,
    // and the reason is the SAME sentence the tap logs, so the screen and the
    // capture log can never disagree about one press. Computed here, next to
    // the states it is made of, and read by both the transport row and the
    // hint band.
    val startBlockedReason: String? = when {
        live || starting -> null
        captureState == CaptureState.STOPPING -> com.lidarscan.core.Wording.START_SEALING
        !connected -> com.lidarscan.core.Wording.START_NEEDS_SENSOR
        else -> null
    }
    val poseState = poseTrackingState(
        required = poseTrackingRequired,
        arAvailable = arAvailable,
        sessionRunning = arSessionRunning,
        tracking = arTracking,
        posesPushed = posesPushed,
        lossEpisodes = arTrackingLossEpisodes,
    )

    // The mount row is only for the sensor whose extrinsic the trim is ABOUT.
    val mountRowVisible = poseTrackingRequired && sensor.isPhoneTrackedPushbroom

    // ── ROUND 28 item 158: THE PRE-FLIGHT, IN THREE ROWS ───────────────────
    //
    // Six controls in two ragged rows, five visual treatments, none of them the
    // primary action, became three lines that each state their own state and
    // carry their own fix. The rules — which row blocks the FAB, which one wears
    // the bad colour when two fail, what the status bar says — are
    // `ScanReadiness` in `:core`, because "when may this app start recording" is
    // the most consequential question it asks and it must be answerable without
    // an emulator.
    //
    // The Mount row is present only for the sensor whose extrinsic the trim is
    // ABOUT (round 8 item 30c's rule, unchanged), and the Tracking row only
    // where the third dimension depends on the phone's pose stream — a Mid-360
    // carries its own IMU and would be answering a question nobody asked.
    val readiness = buildList {
        add(
            com.lidarscan.core.capture.ScanReadiness.Row(
                title = "Sensor",
                state = if (connected) {
                    com.lidarscan.core.capture.ScanReadiness.State.GOOD
                } else {
                    com.lidarscan.core.capture.ScanReadiness.State.BAD
                },
                value = if (connected) "${sensor.badgeLabel} connected" else "Not found",
                detail = if (connected) null else "Plug it in, then retry.",
                actionLabel = if (connected) null else "Retry",
            ),
        )
        if (mountRowVisible) {
            val hasTrim = mountTrim != null
            val warnTrim = mountTrimProvenance?.warn == true
            add(
                com.lidarscan.core.capture.ScanReadiness.Row(
                    title = "Mount",
                    state = when {
                        !hasTrim -> com.lidarscan.core.capture.ScanReadiness.State.WARN
                        warnTrim -> com.lidarscan.core.capture.ScanReadiness.State.WARN
                        else -> com.lidarscan.core.capture.ScanReadiness.State.GOOD
                    },
                    // ROUND 28 item 167: `MOUNT SET · 91.0°` was a WORD in
                    // instrument-panel caps beside sentence-case buttons. "Set"
                    // is a word; the angle is a number; neither is a code.
                    value = if (hasTrim) {
                        mountTrimProvenance?.chipLabel?.lowercase()
                            ?.replaceFirstChar { it.uppercase() } ?: "Set"
                    } else {
                        "Not set"
                    },
                    detail = if (hasTrim) null else "Re-zero before scanning.",
                    actionLabel = if (hasTrim) "Re-zero" else "Set",
                ),
            )
        }
        if (poseTrackingRequired) {
            add(
                com.lidarscan.core.capture.ScanReadiness.Row(
                    title = "Tracking",
                    state = when (poseState) {
                        PoseTrackingState.TRACKING -> com.lidarscan.core.capture.ScanReadiness.State.GOOD
                        PoseTrackingState.UNAVAILABLE -> com.lidarscan.core.capture.ScanReadiness.State.BAD
                        else -> com.lidarscan.core.capture.ScanReadiness.State.WARN
                    },
                    value = when (poseState) {
                        PoseTrackingState.TRACKING -> "Ready"
                        PoseTrackingState.INITIALIZING -> "Starting"
                        PoseTrackingState.LOST -> "Lost"
                        PoseTrackingState.UNAVAILABLE -> "Unavailable"
                        PoseTrackingState.NOT_REQUIRED -> "Not needed"
                    },
                    detail = when (poseState) {
                        PoseTrackingState.UNAVAILABLE -> "Scans will be flat."
                        PoseTrackingState.INITIALIZING -> "Move the phone slowly."
                        PoseTrackingState.LOST -> "Point at edges and furniture."
                        else -> null
                    },
                    actionLabel = if (poseState == PoseTrackingState.UNAVAILABLE) "Retry" else null,
                ),
            )
        }
    }

    // ── ROUND 8, owner item 28: the live scan view keeps 60 % of the screen ──
    //
    // The whole of the pre-capture stack — name field, auto-detect line, manual
    // panel, preset chips, the D6 mount paragraph — collapses to ONE chip row
    // plus the always-visible mount state, and the viewport takes what is left.
    // See [CaptureLayout] for the band-by-band budget and for why the compact
    // form is keyed off *connected* rather than being unconditional (the
    // disconnected screen's job is the connect flow, and it needs the room).
    val manualEntryOpen = autoConnectState?.manualEntryOpen == true
    val compact = CaptureLayout.useCompactChrome(connected = connected, manualEntryOpen = manualEntryOpen)

    /**
     * ROUND 29 item 170 — **is the connect flow already drawn behind a sheet?**
     *
     * Two pages answer it differently now. Landscape still keeps round 27's
     * `IdleScanLayout`, where the flow is the `!compact` chrome; portrait draws
     * it inside §D.1's Sensor row, which happens exactly when nothing is
     * connected. The Capture sheet needs the answer so it does not put a second
     * `scanNameField` or a second `manualLidarIpField` into the same tree.
     */
    val connectFlowOnScreen = if (isLandscape) !compact else !connected
    val screenHeightDp = LocalConfiguration.current.screenHeightDp.toFloat()
    // ROUND 26 item 124: the `BackBar` survives ONLY for the replay session,
    // which since round 23 is the one project-scoped entry into this screen
    // (see Routes.kt) and therefore the one with a real parent to go back to.
    // The Capture tab's own back arrow went to Projects, which the tab bar
    // already does, and 56 dp of a FULLSCREEN live view is not a rounding
    // error. The discriminator is read at the draw site now rather than being
    // hoisted into a flag that only one branch uses.

    // ── ROUND 24 item 112: a Box, so one thing can sit OVER the screen ─────
    //
    // The whole screen is unchanged inside this Box. What the Box buys is a
    // sibling drawn on top of all of it — the tracking-loss popup — rather
    // than a band competing for height inside the column, which is what the
    // round-23 banner was.
    Box(Modifier.fillMaxSize()) {
    // ROUND 24 item 110(b): while (and only while) the tour is running, the
    // controls it rings report their bounds into a registry the overlay reads.
    // Off-tour this provides nothing and the anchor modifiers are `Modifier`.
    TutorialAnchorScope(enabled = tutorialState.running) {
    // ── ROUND 26 item 124: THE SCREEN IS THE LIVE VIEW ──────────────────
    //
    // No insets here and no background padding: the viewport is a full-bleed
    // sibling at the bottom of this Box and every control is an overlay ON it,
    // each applying its OWN safe-area padding. Putting `statusBarsPadding()`
    // here instead — which is what this Column did for twenty rounds — is
    // exactly what a camera app must not do: it insets the PICTURE to make
    // room for chrome, and the picture is the product.
    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        // ── ROUND 27 item 129: the window, MEASURED ────────────────────
        //
        // `BoxWithConstraints` rather than `Configuration.screenHeightDp`.
        // The two are not the same number — the configuration height excludes
        // some insets on some devices and includes them on others — and round
        // 26 subtracted inset-shaped constants from the configuration height
        // and then ALSO applied `statusBarsPadding()` at the draw site, which
        // is how the chrome column ended up running past the control cluster
        // by exactly the insets. Everything below is arithmetic on THIS box.
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val windowHeightDp = maxHeight
            val windowWidthDp = maxWidth
            when (uiState) {
                CaptureUiState.Loading -> Box(Modifier.fillMaxSize())
                CaptureUiState.NotFound -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("Project not found", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }

                else -> {
                    // ROUND 8 (item 28): the four-cell `StatPanel` and its two
                    // 12 dp spacers — ~80 dp — are gone from here. The same four
                    // numbers are one mono line inside the transport row, where
                    // the "display only · recording unaffected" caption used to
                    // sit. Nothing is lost: the caption is still true and still
                    // written on the switch's own label, and mid-walk a single
                    // line of numbers under the Live switch is easier to read at
                    // a glance than a four-tile panel anyway. `StatPanel` itself
                    // stays — the session-summary sheet is where a four-tile
                    // read-out earns its height.
                    // ── ROUND 26 item 124: the two chrome BANDS ────────
                    //
                    // Declared here, above every lambda that needs them,
                    // because the VIEWPORT needs them too: its own four corner
                    // chips have to stay inside the picture rather than under
                    // the floating controls, and that is the same two numbers.
                    //
                    // The tab bar is only owed its 86 dp while it is on screen.
                    // Hiding it and keeping the reservation would leave the FAB
                    // stranded above a gap nothing draws in — round 25 item
                    // 120's mistake, one layer up.
                    // ROUND 27 item 136(a): the tab bar RESERVES its space at
                    // the shell (`LidarScanApp` insets the whole NavHost while
                    // the bar is up and gives the space back the moment it
                    // collapses), so this screen no longer pads for a bar that
                    // is drawn over it. What is left here is the minimal
                    // layout's own margin, which exists because the minimal
                    // layout is the one place the bar is gone entirely.
                    val bottomClearance = 12.dp
                    // ── ROUND 27 item 129: the three bands, MEASURED ────
                    //
                    // Round 26 reserved 96 dp at the top and 174 dp at the
                    // bottom as CONSTANTS, and then grew the status pill by a
                    // row (`LIVE VIEW OFF`), added a `BackBar` for replay,
                    // added an RTK chip strip below it and moved the health
                    // read-out into the pill. Every one of those makes the
                    // real band taller than the constant, and a constant that
                    // is too small does not fail a test — it draws the connect
                    // flow through the chip strip, which is what the owner saw.
                    //
                    // So each band now REPORTS its own bounds and the chrome
                    // is laid out against the report. The constants survive as
                    // first-frame floors (`CaptureLayout.CHROME_*_RESERVE_DP`),
                    // which is all a constant can honestly be here: the very
                    // first composition has no measurement yet, and a chrome
                    // column that flashes 4 dp taller for one frame is not a
                    // collision.
                    //
                    // `onGloballyPositioned` is hung FIRST in each chain, so
                    // what is reported is the band's OUTER rectangle — its own
                    // safe-area padding included — in this Box's coordinates.
                    // Reported at the end of the chain it would be the content
                    // box inside the padding, and the reserve would be short by
                    // exactly the inset again.
                    val density = LocalDensity.current
                    // Landscape's rail applies `statusBarsPadding()` itself, so
                    // the inset has to be counted into its reserve; portrait's
                    // top reserve is the MEASURED band, which already contains
                    // the inset because the band draws it.
                    val statusBarTopDp = WindowInsets.statusBars
                        .asPaddingValues().calculateTopPadding()
                    // ROUND 27 item 129(a): the stream read-out's fact, hoisted
                    // out of the viewport so the chip can be drawn in a corner
                    // the viewport does not own.
                    var hasSeenMappedPage by remember { mutableStateOf(false) }
                    var topBandBottomPx by remember { mutableFloatStateOf(0f) }
                    var bottomBandTopPx by remember { mutableFloatStateOf(0f) }
                    var endRailLeftPx by remember { mutableFloatStateOf(0f) }
                    val windowHeightPx = with(density) { windowHeightDp.toPx() }
                    val windowWidthPx = with(density) { windowWidthDp.toPx() }
                    // The replay session is the one entry that still draws a
                    // `BackBar`, and it is 56 dp tall — so the band has to start
                    // below it or the chip row prints through the status pill.
                    // Found on the AVD's synthetic replay, which is the only
                    // place on an emulator where both are on screen at once.
                    val topBandDp = with(density) { topBandBottomPx.toDp() }
                        .coerceAtLeast(
                            CaptureLayout.CHROME_TOP_RESERVE_DP.dp +
                                if (isReplaySession) ScanDims.BackBar else 0.dp,
                        )
                    val bottomBandDp = with(density) { (windowHeightPx - bottomBandTopPx).toDp() }
                        // The first-frame floor differs by orientation because
                        // the BAND differs: in portrait the bottom row carries
                        // the 88 dp FAB, and in landscape the FAB is on the end
                        // rail and the row is two chips over the tab-bar
                        // clearance. Using the portrait floor in landscape does
                        // no harm to a collision — it is a floor — but it costs
                        // the connect rail 54 dp of the ~200 dp it has, which on
                        // a landscape phone is the difference between seeing the
                        // Retry links and not.
                        .coerceAtLeast(
                            if (isLandscape) {
                                CaptureLayout.LANDSCAPE_BOTTOM_RESERVE_DP.dp
                            } else {
                                CaptureLayout.CHROME_BOTTOM_RESERVE_DP.dp
                            },
                        )
                    // The end rail the floating cluster owns in landscape. In
                    // portrait the cluster is in the bottom band and owns no
                    // column, so the rail is zero and the corner chips get
                    // their ordinary 14 dp.
                    val endRailDp = if (isLandscape) {
                        with(density) { (windowWidthPx - endRailLeftPx).toDp() }
                            .coerceAtLeast(CaptureLayout.CONTROL_RAIL_MIN_DP.dp)
                    } else {
                        0.dp
                    }
                    // Kept under its round-26 name because five call sites read
                    // it; it is the measured band now rather than 96 dp.
                    val chromeTopDp = topBandDp

                    // ROUND 26 item 124: the transport row is gone. Its five
                    // controls were a horizontal band that cost 80 dp of the
                    // picture; they are now three overlays in three different
                    // corners of it, and each one is a separate composable so a
                    // layout that re-anchors for landscape can move them
                    // independently instead of moving one rigid Row.
                    // ── ROUND 27 item 136: ONE status band, TWO placements ─
                    //
                    // The same composable in the flow (idle) and floating over
                    // the picture (recording). A second copy for the second
                    // placement is how the two drift into saying different
                    // things about the same session, which is half of item 138.
                    val statusBand: @Composable (Boolean) -> Unit = { floating ->
                        Column(Modifier.fillMaxWidth()) {
                            if (isReplaySession) {
                                BackBar(
                                    title = project?.manifest?.name ?: "Capture — Replay",
                                    subtitle = "${profile.displayName} · ${captureStateLabel(captureState)}",
                                    onBack = onBack,
                                )
                            }
                            Row(
                                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                                verticalAlignment = Alignment.Top,
                            ) {
                                ScanStatusPill(
                                    modifier = Modifier
                                        .widthIn(max = 280.dp)
                                        .testTag("scanStatusPill"),
                                    sensor = sensor,
                                    connected = connected,
                                    captureState = captureState,
                                    stats = stats,
                                    trailLengthM = trailLengthM,
                                    liveView = liveView,
                                    health = health,
                                    showHealth = true,
                                    // ROUND 27 item 138: the ONE tracking
                                    // status on this screen. It used to be
                                    // said twice — a viewport chip AND the
                                    // chip row — and the two are not even
                                    // guaranteed to agree mid-transition.
                                    poseState = poseState.takeIf { poseTrackingRequired && connected },
                                    onOpenDiagnostics = { sheet = CaptureSheet.DIAGNOSTICS },
                                )
                                Spacer(Modifier.weight(1f))
                                Spacer(Modifier.width(8.dp))
                                ScanGearButton(onOpenAdvanced = { sheet = CaptureSheet.SETTINGS })
                            }
                            // The RTK/georeference chips keep their round-8
                            // rule: shown when there is genuinely RTK to report.
                            if (!floating || georefSource.isRtk || ntrip.receiving) {
                                if (!compact || georefSource.isRtk || ntrip.receiving) {
                                    Box(Modifier.padding(horizontal = 4.dp)) {
                                        FixChipStrip(fix = fix, ntrip = ntrip, georefSource = georefSource)
                                    }
                                }
                            }
                        }
                    }

                    // ROUND 27 items 138 + 140(a): what stream is on screen, and
                    // NOT which device it came from — the device is the pill's
                    // to say, and saying it twice was the owner's "2 connected
                    // device model showing". The map-mode chip that used to sit
                    // beside this one is gone entirely (item 140(a)); the live
                    // map switch keeps its home in the Capture sheet.
                    val streamChip: @Composable () -> Unit = {
                        StreamModeChip(
                            liveMapEnabled = liveMapEnabled,
                            liveMapRequested = liveMapRequested,
                            hasSeenMappedPage = hasSeenMappedPage,
                        )
                    }

                    val controls: @Composable (Boolean) -> Unit = { vertical ->
                        ScanControlCluster(
                            vertical = vertical,
                            captureState = captureState,
                            connected = connected,
                            startBlockedReason = startBlockedReason,
                            onStartRefused = onStartRefused,
                            liveView = liveView,
                            isReplaySession = isReplaySession,
                            pauseSupported = !isReplaySession && sensor != SensorType.MID360,
                            starting = starting,
                            onLiveViewChange = onLiveViewChange,
                            onStart = onStart,
                            onPause = onPause,
                            onResume = onResume,
                            onStop = onStop,
                            attitude = attitude,
                        )
                    }

                    val viewport: @Composable (Modifier) -> Unit = { modifier ->
                        CaptureViewport(
                            fullBleed = true,
                            // The chrome's two bands, handed to the viewport so
                            // its own corner chips draw inside the picture
                            // rather than under the floating controls.
                            // ROUND 27 item 129(a): the chips get the bands
                            // the chrome ACTUALLY occupies. In landscape the
                            // start side is the connect rail from the status
                            // bar down — a chip inset by 14 dp there is drawn
                            // straight through the connect flow, which is what
                            // `RAW · D6` did — and the end side is the control
                            // rail the cluster owns.
                            chipInsets = androidx.compose.foundation.layout.PaddingValues(
                                start = if (isLandscape) {
                                    CaptureLayout.LANDSCAPE_RAIL_WIDTH_DP.dp + 16.dp
                                } else {
                                    14.dp
                                },
                                end = if (isLandscape) endRailDp else 14.dp,
                                top = chromeTopDp - 12.dp,
                                bottom = bottomBandDp + 8.dp,
                            ),
                            // ROUND 27 item 129(a): the two read-outs the SCREEN
                            // owns now. `No data` was a viewport chip in the
                            // bottom-END corner, which in landscape is the
                            // middle of the picture beside the SCAN button —
                            // the owner's "stray chip floating mid-screen". It
                            // lives in the status pill now. `RAW · D6` was the
                            // bottom-START chip, which in landscape is inside
                            // the connect rail; it joins the map-mode chip in
                            // the bottom corner. Neither is deleted and neither
                            // is duplicated: the viewport simply stops drawing
                            // the two the screen has taken over.
                            ownsStatusChips = false,
                            onMappedPageSeen = { hasSeenMappedPage = it },
                            // ROUND 24 item 110(b): the tracking-lost step
                            // rings the viewport, because that is where the
                            // popup lands and what the operator is watching.
                            modifier = modifier.then(rememberTutorialAnchor(com.lidarscan.core.capture.TutorialAnchor.VIEWPORT)),
                            connected = connected,
                            isReplaySession = isReplaySession,
                            source = pointCloudSource.takeIf { liveView },
                            liveView = liveView,
                            colorMode = colorMode,
                            colormap = colormap,
                            pointSizePx = pointSizePx,
                            displayParams = displayParams,
                            refreshHz = refreshHz,
                            refreshRequestToken = refreshRequestToken,
                            cameraMode = cameraMode,
                            liveSlam = liveSlam,
                            // ROUND 6 (owner item 21): the viewport's stream
                            // filter is driven by whether a MAP is actually
                            // being produced, not by `liveSlam` alone — see
                            // `CaptureViewModel.pushbroomActive` for the bug
                            // that made a D6 session draw raw fan slices and
                            // never the pushbroom-resolved cloud.
                            liveMapRequested = liveMapRequested,
                            liveMapEnabled = liveMapEnabled,
                            recording = recording,
                            sensor = sensor,
                            arAvailable = arAvailable,
                            arSessionWanted = arSessionWanted,
                            arTrackingHint = arTrackingHint,
                            arOverlay = arOverlay,
                            arPosePump = arPosePump,
                            poseTrackingRequired = poseTrackingRequired,
                            poseState = poseState,
                            health = health,
                            keyframesEnabled = keyframesEnabled,
                            keyframesWritten = keyframeStats.keyframesWritten,
                            trailPoints = trailPoints,
                            trailLengthM = trailLengthM,
                            coverageSectors = coverageSectors,
                            onRefreshAutoDownshift = onRefreshAutoDownshift,
                            onOpenDiagnostics = { sheet = CaptureSheet.DIAGNOSTICS },
                            onCameraModeChange = onCameraModeChange,
                            onRendererChanged = onRendererChanged,
                        )
                    }

                    // ROUND 6 (item 20) / ROUND 7 (field bug 2): the two failures
                    // that are allowed to shout, and the only two bands that are
                    // deliberately left OUT of [CaptureLayout]'s height budget.
                    //
                    // A capture that did not save, and a capture that is running
                    // and receiving nothing, are by definition not the "normal
                    // state" item 28's 60 % rule is written about — and shrinking
                    // the two messages that cost the owner two field sessions to
                    // protect a viewport that is showing nothing worth protecting
                    // would be the wrong trade in both directions.
                    val loudBanners: @Composable () -> Unit = {
                        // ── ROUND 27 item 142(b): THE TRACKER IS DEAD ───────
                        //
                        // First, above everything else this band carries, and
                        // one of the two things item 136's overlay ban still
                        // lets over the picture — because it is a warning, and
                        // because the alternative is what the OPPO user got:
                        // six attempts, no scan, and a progress line that never
                        // stopped being hopeful.
                        if (arTrouble != com.lidarscan.core.capture.ArTroubleKind.NONE) {
                            ArTroubleCard(
                                kind = arTrouble,
                                onRetry = onRetryAr,
                                onUpdate = onUpdateArServices,
                                onSendLogs = onSendLogs,
                            )
                        }
                        // ── ROUND 24 item 110(b): the one-time offer ────────
                        //
                        // A card, not a dialog: round 5's rule is that a modal
                        // is the worst possible interruption on this tab, and
                        // that applies most of all to the first time it opens.
                        //
                        // ROUND 29 item 170: §D.1's page has its own slot for
                        // this (item 166 put it directly under the status bar,
                        // where it never displaces the task), and it passes
                        // `loudBanners` as well — so on the bench the offer
                        // rendered TWICE, one line under the other. The band
                        // draws it only for the pages that have no slot:
                        // landscape, and the recording page.
                        if (offerTutorial && (isLandscape || live || starting)) {
                            TutorialOffer(
                                onAccept = onAcceptTutorialOffer,
                                onDismiss = onDismissTutorialOffer,
                            )
                        }
                        // ── ROUND 24 item 112 ───────────────────────────────
                        //
                        // The round-23 tracking-loss banner used to be FIRST
                        // in this band. It is not in the band at all any more:
                        // the owner's point is that a band at the top of a
                        // screen he is not looking at is not a warning, so it
                        // is a centered popup over the whole screen now. See
                        // `TrackingLossPopup` at the bottom of this file — the
                        // state machine behind it is untouched.
                        // ROUND 21 item 85 (owner request, verbatim: "i dont
                        // know what is the app loading with, show me the
                        // progress and tell me what i am waiting for and how
                        // long and what should i do while waiting"): ONE panel
                        // for the whole start sequence, from the instant of the
                        // press. It folds the ROUND 20 hold banner in — while it
                        // is up it IS the instruction — and the GO stage that
                        // ends it is the moment the walk begins.
                        if (startProgress != null || startHold != null) {
                            StartProgressPanel(
                                progress = startProgress,
                                warmup = startWarmup,
                                hold = startHold,
                            )
                        }
                        if (saveError != null) {
                            SaveErrorBanner(saveError, onDismissSaveError)
                        }
                        if (noDataAlert != null) {
                            LoudBanner(
                                title = "No sensor data",
                                message = noDataAlert,
                                onDismiss = onDismissNoDataAlert,
                                testTag = "noDataBanner",
                            )
                        }
                        // ROUND 16 item 58(b): the other half of "is this scan
                        // real". Below the no-data banner and never shown with
                        // it — the watchdog refuses to fire while no points are
                        // arriving, because that is the other banner's
                        // diagnosis and it has a different instruction.
                        if (noPoseAlert != null) {
                            LoudBanner(
                                title = "No position tracking",
                                message = noPoseAlert,
                                onDismiss = onDismissNoPoseAlert,
                                testTag = "noPoseBanner",
                            )
                        }
                        // ROUND 8 (item 30b): a refused re-zero is now as loud as
                        // a failed save, and for the same reason — the owner
                        // tapped this control eight times in one session, was told
                        // "Moving" in a grey one-liner every time, and reasonably
                        // concluded the button did nothing. The measured numbers
                        // and the instruction ("hold still ~1 s") are in the
                        // message; see MountTrimResult.Rejected.sentence.
                        if (mountTrimNote != null && mountTrimNoteIsWarning) {
                            LoudBanner(
                                title = "Mount reference not set",
                                message = mountTrimNote,
                                onDismiss = onDismissMountTrimNote,
                                testTag = "mountTrimRefusalBanner",
                                accent = ScanColors.warn,
                            )
                        }
                    }

                    // ROUND 5.2 / 5.3 / 6 / 7: the quiet inline lines. Every one
                    // is one sentence, every one clears itself, and none is ever a
                    // dialog — mid-walk, a modal is the worst possible
                    // interruption.
                    //
                    // ROUND 8 (item 28): they are now a BOUNDED band. Six of them
                    // can be live at once (georef + downshift + motion + AR +
                    // section + page-store), which was ~180 dp of capture screen
                    // taken by advisories about a scan the operator can no longer
                    // see. Capped at [CaptureLayout.HINT_BAND_MAX_DP] and
                    // scrollable: the first one is always readable, the rest are a
                    // flick away, and the viewport keeps its height.
                    val anyHint = georefNote != null || refreshDownshiftNote != null || motionHint != null ||
                        arErrorMessage != null || sectionHint != null || liveMapFullNote != null ||
                        dndNote != null || startTapRefusal != null || startBlockedReason != null ||
                        (mountTrimNote != null && !mountTrimNoteIsWarning)
                    val hints: @Composable () -> Unit = {
                        if (anyHint) {
                            // ROUND 27 item 129(b): NOT a 44 dp scroller inside
                            // a scroller. `HINT_BAND_MAX_DP` was round 8's
                            // answer to "six advisories must not eat the
                            // viewport", and in the fullscreen layout the
                            // CHROME COLUMN's own ceiling is that answer — it
                            // bounds banners, connect flow and hints together,
                            // which is the number that actually matters.
                            // Nested, the cap did the opposite of its job: it
                            // clipped `Allow Do Not Disturb in Settings` in the
                            // middle of the sentence and ate the drag that
                            // would have revealed the rest.
                            Column(Modifier.fillMaxWidth().testTag("scanHintBand")) {
                                // ROUND 23 item 101(b): the tap that was
                                // refused, in the words the log used, for four
                                // seconds. Amber and first, because it is the
                                // answer to something the operator did one
                                // second ago.
                                if (startTapRefusal != null) {
                                    // ── ROUND 27 item 132 ──────────────────
                                    //
                                    // The line is the SAME sentence in the same
                                    // amber it has been since round 23 — the
                                    // wording law owns the words and this round
                                    // does not touch them. Two things are
                                    // added, both about being SEEN rather than
                                    // about saying more.
                                    //
                                    // It scrolls itself into view. The chrome
                                    // column is bounded and scrollable (item
                                    // 129), so the answer to a press can
                                    // legitimately be below the fold — a
                                    // refusal the operator has to go looking
                                    // for is the same defect as no refusal at
                                    // all.
                                    //
                                    // And it gets a ground and a hairline, so
                                    // that at a glance it is a CARD that
                                    // appeared rather than a grey line that
                                    // changed colour. Still one line, still no
                                    // modal, still dismissible by tapping it.
                                    val refusalIntoView = remember { BringIntoViewRequester() }
                                    LaunchedEffect(startTapRefusal) {
                                        refusalIntoView.bringIntoView()
                                    }
                                    Hint(
                                        startTapRefusal,
                                        color = ScanColors.warn,
                                        modifier = Modifier
                                            .padding(horizontal = 10.dp, vertical = 3.dp)
                                            .bringIntoViewRequester(refusalIntoView)
                                            .background(
                                                ScanColors.warn.copy(alpha = 0.12f),
                                                RoundedCornerShape(ScanDims.TileRadius),
                                            )
                                            .border(
                                                1.dp,
                                                ScanColors.warn.copy(alpha = 0.55f),
                                                RoundedCornerShape(ScanDims.TileRadius),
                                            )
                                            .clickable(onClick = onDismissStartTapRefusal)
                                            .padding(horizontal = 10.dp, vertical = 7.dp)
                                            .testTag("startTapRefusalNote"),
                                    )
                                } else if (startBlockedReason != null && (isLandscape || live || starting)) {
                                    // The standing reason, quietly, so the
                                    // dimmed button is never a mystery in the
                                    // first place.
                                    //
                                    // ROUND 29 item 170: not on §D.1's page.
                                    // The owner photographed it there as
                                    // *"Connect the scanner first."* floating
                                    // under a row of pills with nothing to
                                    // attach to — and on that page the Sensor
                                    // row states the same fact, in red, with
                                    // the fix on it. Two sentences about one
                                    // blocker is how the old screen came to say
                                    // readiness six ways at once.
                                    Hint(
                                        startBlockedReason,
                                        color = ScanColors.inkFaint,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("startBlockedNote"),
                                    )
                                }
                                if (dndNote != null) {
                                    // ScanColors.warn, not ScanColors.inkFaint: this one is about
                                    // the measurement, not about a convenience.
                                    Hint(
                                        dndNote,
                                        color = ScanColors.warn,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("dndUnprotectedNote"),
                                    )
                                }
                                if (georefNote != null) {
                                    Hint(
                                        georefNote,
                                        color = ScanColors.inkFaint,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("georefDeniedNote"),
                                    )
                                }
                                if (refreshDownshiftNote != null) {
                                    Hint(
                                        refreshDownshiftNote,
                                        color = ScanColors.inkFaint,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("refreshDownshiftNote"),
                                    )
                                }
                                if (motionHint != null) {
                                    Hint(
                                        motionHint,
                                        color = ScanColors.warn,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("motionHint"),
                                    )
                                }
                                // ROUND 6 (owner item 19): the AR path failed. The
                                // view has already fallen back to 3D orbit — the
                                // app does not die and the capture keeps running.
                                if (arErrorMessage != null) {
                                    // ROUND 24 item 110(a). Was 24 words plus
                                    // an error string, read on every degrade.
                                    // The error itself is still shown — it is
                                    // the only part that is not generic.
                                    Hint(
                                        com.lidarscan.core.Wording.AR_DEGRADED + " " + arErrorMessage + "\n" +
                                            com.lidarscan.core.Wording.AR_DEGRADED_DETAIL,
                                        color = ScanColors.warn,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("arUnavailableNote"),
                                    )
                                }
                                // ROUND 7 (item 3): a tracking jump split the scan.
                                // Not a banner — the capture is still good and
                                // still recording — but it must be visible while
                                // the operator is still in the room and can walk
                                // the seam again.
                                if (sectionHint != null) {
                                    Hint(
                                        sectionHint,
                                        color = ScanColors.warn,
                                        modifier = Modifier
                                            .padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("sectionHint"),
                                    )
                                }
                                // ROUND 6 (owner item 21): the live page store
                                // filled and the map stopped growing. Says which
                                // half of the app it costs, because the answer is
                                // "the preview, not the scan".
                                if (liveMapFullNote != null) {
                                    Hint(
                                        liveMapFullNote,
                                        color = ScanColors.warn,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("liveMapFullNote"),
                                    )
                                }
                                // ROUND 6 (owner item 23): the re-zero's own
                                // verdict, when it is a confirmation. Refusals go
                                // to the loud banner above instead.
                                if (mountTrimNote != null && !mountTrimNoteIsWarning) {
                                    Hint(
                                        mountTrimNote,
                                        color = ScanColors.sensorD6,
                                        modifier = Modifier
                                            .padding(horizontal = 14.dp, vertical = 2.dp)
                                            .clickable(onClick = onDismissMountTrimNote)
                                            .testTag("mountTrimNote"),
                                    )
                                }
                            }
                        }
                    }

                    // ROUND 8 (items 28 + 30c): the compact chrome — the mount
                    // state row, which must be readable without opening anything,
                    // and one chip row for the three sheets.
                    val compactChrome: @Composable () -> Unit = {
                        if (mountRowVisible) {
                            MountStateRow(
                                provenance = mountTrimProvenance,
                                hasTrim = mountTrim != null,
                                hold = mountHold,
                                onSetMountReference = onSetMountReference,
                                onBeginMountHold = onBeginMountHold,
                                onCancelMountHold = onCancelMountHold,
                            )
                        }
                        CaptureChipRow(
                            preset = preset,
                            scanName = scanName.ifBlank { newScan?.autoName ?: project?.manifest?.name ?: "New scan" },
                            poseState = poseState,
                            // ROUND 27 item 138: the pill owns the tracking
                            // status. This chip was the second of the owner's
                            // "2 tracking status showing in the scan tab".
                            poseChipVisible = false,
                            showCaptureChip = true,
                            onOpenCapture = { sheet = CaptureSheet.CAPTURE },
                            onOpenDiagnostics = { sheet = CaptureSheet.DIAGNOSTICS },
                            onOpenMid360Setup = onOpenMid360Setup,
                            onOpenRtk = onOpenRtk,
                            onNewCapture = onNewCapture.takeUnless { isReplaySession },
                        )
                    }

                    // ROUND 5: the connect flow, inline. Kept EXACTLY as it was
                    // for the disconnected state — auto-detect status, the manual
                    // panel that opens itself when detection fails (owner addition
                    // 1), the name field — because that state is what the Capture
                    // tab is FOR when no sensor is attached, there is no live view
                    // to protect, and it is what the emulator smoke test walks.
                    val fullChrome: @Composable () -> Unit = {
                        PreCaptureStrip(
                            // ROUND 27 item 129: the chrome column above is the
                            // one bounded scroller on this screen.
                            maxHeight = androidx.compose.ui.unit.Dp.Unspecified,
                            autoName = newScan?.autoName,
                            scanName = scanName,
                            autoConnectState = autoConnectState,
                            sensor = sensor,
                            poseTrackingRequired = poseTrackingRequired,
                            poseState = poseState,
                            sensorStreaming = connected,
                            mountIsNominal = mountIsNominal,
                            manualDevices = manualDevices,
                            manualLidarIp = manualLidarIp,
                            manualHostIp = manualHostIp,
                            onScanNameChange = onScanNameChange,
                            onRetryAutoDetect = onRetryAutoDetect,
                            onShowManualEntry = onShowManualEntry,
                            onHideManualEntry = onHideManualEntry,
                            onManualDeviceConnect = onManualDeviceConnect,
                            onManualLidarIpChange = onManualLidarIpChange,
                            onManualHostIpChange = onManualHostIpChange,
                            onManualMid360Connect = onManualMid360Connect,
                            onOpenMountCalibration = onOpenMountCalibration,
                            mountTrim = mountTrim,
                            mountTrimProvenance = mountTrimProvenance,
                            nowMillis = nowMillis,
                            onSetMountReference = onSetMountReference,
                            onClearMountReference = onClearMountReference,
                        )
                        // Only the two sheets whose contents are NOT already on
                        // screen behind them. Offering the Capture sheet here as
                        // well would put a second `scanNameField` and a second
                        // `manualLidarIpField` in the tree at the same time.
                        CaptureChipRow(
                            preset = preset,
                            scanName = scanName,
                            poseState = poseState,
                            poseChipVisible = false,
                            // ROUND 27 item 133(c): the sheet's one door, in
                            // both states. See the `CaptureConfigSheet` call
                            // site for how the duplicate-node problem round 26
                            // avoided by hiding the chip is answered instead.
                            showCaptureChip = true,
                            onOpenCapture = { sheet = CaptureSheet.CAPTURE },
                            onOpenDiagnostics = { sheet = CaptureSheet.DIAGNOSTICS },
                            onOpenMid360Setup = onOpenMid360Setup,
                            onOpenRtk = onOpenRtk,
                            onNewCapture = onNewCapture.takeUnless { isReplaySession },
                        )
                    }

                    // ══ ROUND 26 item 124 — THE FULLSCREEN LAYOUT ══════════
                    //
                    // One full-bleed viewport, and every control an overlay on
                    // it. The three things this buys, in the order the owner
                    // asked for them: the live view is the whole screen; the
                    // controls are where a thumb already is; and nothing but
                    // the picture competes for height, so the round-8 "60 % of
                    // the screen" rule is satisfied at 100 % and the arithmetic
                    // that enforced it now budgets the FLOATING CHROME instead
                    // (see [CaptureLayout.chromeMaxHeightDp]).
                    //
                    // The chrome is deliberately NOT deleted. Everything the
                    // pre-capture screen needs — the name field, auto-detect,
                    // the manual panel, the mount row, the chip row, the hint
                    // band — floats as a card over the viewport instead of
                    // pushing it down the screen. A camera app that cannot find
                    // its camera shows a panel over the preview; it does not
                    // stop being a camera app.
                    // ══ ROUND 27 item 136 — THE OVERLAY BAN ════════════════
                    //
                    // The owner, on 0.9.11: *"Nothing should overlay except
                    // warning. … show the settings of connection in the main
                    // window and not overlay."* That revises round 26's central
                    // decision rather than tuning it, and it is the right
                    // revision: item 124's argument for floating everything was
                    // "a camera app shows a panel over the preview", and it is
                    // true of a camera app that HAS a picture. With nothing on
                    // the cable there is no picture — the viewport is empty
                    // ground — so what round 26 actually shipped was a settings
                    // form floating over a black rectangle, and every collision
                    // item 129 lists is a consequence of two things competing
                    // for pixels that a column would simply have divided.
                    //
                    // So the screen has TWO compositions and one predicate:
                    //
                    //  * **idle** — an ordinary laid-out page. A status band, a
                    //    preview that takes the room left over, the connection
                    //    section IN THE FLOW, and the controls in a band of
                    //    their own. Nothing overlaps anything, by construction
                    //    rather than by arithmetic.
                    //  * **recording or starting** — minimal and fullscreen.
                    //    The picture is the product now, so it takes the
                    //    screen, and the only things on it are the status pill,
                    //    the control cluster and whatever is warning you. This
                    //    is round 26's layout, kept for the state it was right
                    //    for.
                    //
                    // Warnings are the exception in both (`loudBanners`, the
                    // tracking-loss popup, the refusal cue), which is exactly
                    // the licence the owner granted.
                    val minimal = live || starting
                    // ── ROUND 27 item 136: the viewport MOVES, it is not rebuilt
                    //
                    // Start flips this screen from the idle page to the minimal
                    // one, and the viewport is a different node in a different
                    // subtree on either side of that flip. Compose's positional
                    // identity would therefore DISPOSE the old one and compose a
                    // new one — which for this composable means tearing down a
                    // `GLSurfaceView`, a Filament renderer and, on a replay
                    // session, the decode that has just been asked to start.
                    // The emulator suite found it immediately and exactly:
                    // `ReplayCaptureSmokeTest` clicked Start and then waited
                    // twenty seconds for a point count that never left zero.
                    //
                    // `movableContentOf` is the tool for precisely this — the
                    // same instance, with its state, relocated in the tree — and
                    // `rememberUpdatedState` is what keeps the moved content
                    // reading the CURRENT lambda rather than the one captured on
                    // the first frame.
                    val currentViewport by androidx.compose.runtime.rememberUpdatedState(viewport)
                    val movableViewport = remember {
                        androidx.compose.runtime.movableContentOf<Modifier> { m -> currentViewport(m) }
                    }
                    if (minimal && !isLandscape) {
                        // ── ROUND 28 item 159: THE RECORDING PAGE ───────────
                        //
                        // Round 26 floated every band on the picture and round
                        // 27 spent an item computing reserves so they would not
                        // collide. At 1080 × 2400 on this bench they collided
                        // anyway — status pill through DND advisory through
                        // refresh note, three strings on the same pixels. A
                        // column cannot overlap itself, and the viewport still
                        // gets ≥60 % because it is the only weighted child.
                        //
                        // Landscape keeps `MinimalScanLayout`: round 27 item
                        // 129(a) solved landscape with an end rail and a
                        // start rail, and a portrait column would undo it.
                        ScanRecordingPage(
                            telemetry = {
                                RecordingTelemetry(
                                    elapsed = formatDuration(stats.elapsedMillis),
                                    points = stats.pointsCaptured,
                                    metres = trailLengthM.toDouble(),
                                    // §B Job 2's missing number, from what is
                                    // known LIVE: a section break means the
                                    // trajectory came apart, and tracking loss
                                    // episodes are what produce the `drops` the
                                    // seal grade counts.
                                    quality = when {
                                        arTrackingLossEpisodes > 0 -> ScanColors.bad
                                        // `sectionHint` is non-null exactly
                                        // when the trajectory has come apart
                                        // and not healed — the live half of
                                        // what the seal grade counts as
                                        // `sections`.
                                        sectionHint != null -> ScanColors.warn
                                        else -> ScanColors.good
                                    },
                                    paused = paused,
                                    onOpenAdvanced = { sheet = CaptureSheet.SETTINGS },
                                )
                            },
                            viewport = movableViewport,
                            advisories = {
                                // Absent in the nominal case — §D.2: the
                                // advisory INSERTS, it does not reserve.
                                loudBanners()
                                hints()
                            },
                            controls = { controls(false) },
                        )
                    } else if (minimal) {
                        MinimalScanLayout(
                            isLandscape = isLandscape,
                            bottomClearance = bottomClearance,
                            endRailDp = endRailDp,
                            onEndRailMeasured = { endRailLeftPx = it },
                            onTopBandMeasured = { topBandBottomPx = it },
                            onBottomBandMeasured = { bottomBandTopPx = it },
                            viewport = movableViewport,
                            statusBand = statusBand,
                            controls = controls,
                            loudBanners = loudBanners,
                            hints = hints,
                            streamChip = streamChip,
                            tutorialChip = { TutorialChip(onOpenTutorial = onStartTutorial) },
                        )
                    } else if (!isLandscape) {
                        // ── ROUND 28 item 158: THE IDLE PAGE ────────────────
                        //
                        // The one state the review calls "the worst screen, and
                        // the one the owner opens first" — connected, portrait,
                        // not recording. Round 27 item 136 gave this state a
                        // guaranteed 60 % viewport because the sensor was
                        // attached; the sensor being attached is not the same
                        // question as whether there is anything to draw, and
                        // the answer to the second one, before Start, is no.
                        //
                        // §D.1's page instead: status bar, three readiness
                        // rows, one FAB. The live viewport is not
                        // composed here at all — there is nothing in it — which
                        // is what lets the FAB take the slack rather than a
                        // 940-px rectangle.
                        //
                        // ── ROUND 29 item 170 ──────────────────────────────
                        //
                        // Round 28 gated this page on `compact`, i.e. on
                        // `useCompactChrome(connected, …)`, and **no D6 connects
                        // to an emulator** — so the AVD, and the owner with the
                        // cable out, both fell to round 27's `IdleScanLayout`.
                        // The redesign he approved has, on his phone, never once
                        // been the screen he opens. The HOTFIX section of round
                        // 28 raised this as an owner call; he made it.
                        //
                        // Portrait idle is §D.1 in BOTH states now. The
                        // disconnected one is the mockup's own "scanner missing"
                        // phone: the Sensor row goes bad and the connect flow
                        // opens inside the card under it (`connectFlow`). What
                        // leaves with round 27's page is exactly what item 158
                        // listed and the owner photographed again on 0.9.13 —
                        // the `00:00 / 0 pts / 0.0 m / No data` card, the
                        // `NO GEOREF` + `No rover` chip strip, the dead black
                        // viewport, the corner `?`, the Capture/Diag/New-capture
                        // pills and the stray "Connect the scanner first." line.
                        //
                        // Landscape keeps `IdleScanLayout`: round 27 item 129(a)
                        // solved landscape with two rails and a portrait column
                        // would undo it.
                        ScanReadyPage(
                            statusLine = com.lidarscan.core.capture.ScanReadiness.statusLine(
                                sensorName = sensor.badgeLabel,
                                rows = readiness,
                            ),
                            blocked = !com.lidarscan.core.capture.ScanReadiness.canStart(readiness),
                            onOpenAdvanced = { sheet = CaptureSheet.SETTINGS },
                            readiness = readiness,
                            onReadinessAction = { title ->
                                when (title) {
                                    "Sensor" -> onRetryAutoDetect()
                                    "Mount" -> onBeginMountHold()
                                    "Tracking" -> onRetryAr()
                                }
                            },
                            // ROUND 29 item 170: only when there is nothing on
                            // the cable. The mount block is suppressed inside it
                            // because the Mount ROW above owns that fact now —
                            // drawing both is how the owner's 0.9.13 page ended
                            // up stating the trim twice, once in 24 words.
                            connectFlow = if (connected) {
                                null
                            } else {
                                {
                                    PreCaptureStrip(
                                        maxHeight = androidx.compose.ui.unit.Dp.Unspecified,
                                        autoName = newScan?.autoName,
                                        scanName = scanName,
                                        autoConnectState = autoConnectState,
                                        sensor = sensor,
                                        poseTrackingRequired = poseTrackingRequired,
                                        poseState = poseState,
                                        sensorStreaming = connected,
                                        mountIsNominal = mountIsNominal,
                                        manualDevices = manualDevices,
                                        manualLidarIp = manualLidarIp,
                                        manualHostIp = manualHostIp,
                                        onScanNameChange = onScanNameChange,
                                        onRetryAutoDetect = onRetryAutoDetect,
                                        onShowManualEntry = onShowManualEntry,
                                        onHideManualEntry = onHideManualEntry,
                                        onManualDeviceConnect = onManualDeviceConnect,
                                        onManualLidarIpChange = onManualLidarIpChange,
                                        onManualHostIpChange = onManualHostIpChange,
                                        onManualMid360Connect = onManualMid360Connect,
                                        onOpenMountCalibration = onOpenMountCalibration,
                                        mountTrim = mountTrim,
                                        mountTrimProvenance = mountTrimProvenance,
                                        nowMillis = nowMillis,
                                        onSetMountReference = onSetMountReference,
                                        onClearMountReference = onClearMountReference,
                                        showMountBlock = false,
                                    )
                                }
                            },
                            banners = {
                                loudBanners()
                                hints()
                            },
                            tutorialBanner = {
                                if (offerTutorial) {
                                    TutorialOffer(
                                        onAccept = onAcceptTutorialOffer,
                                        onDismiss = onDismissTutorialOffer,
                                    )
                                }
                            },
                            fab = { controls(false) },
                        )
                    } else {
                        IdleScanLayout(
                            isLandscape = isLandscape,
                            connected = compact,
                            windowHeightDp = windowHeightDp,
                            windowWidthDp = windowWidthDp,
                            mountRowVisible = mountRowVisible,
                            viewport = movableViewport,
                            statusBand = statusBand,
                            controls = controls,
                            chrome = {
                                loudBanners()
                                if (compact) compactChrome() else fullChrome()
                                hints()
                            },
                            tutorialChip = { TutorialChip(onOpenTutorial = onStartTutorial) },
                        )
                    }
                }
            }
        }
    }

    // ── ROUND 24 item 112 (owner request): the CENTERED popup ──────────────
    //
    // Last child of the Box, so it is drawn over the viewport, the chrome and
    // the transport row — including the STOP button, which is deliberately
    // still reachable THROUGH it. See `TrackingLossPopup`.
        TrackingLossPopup(trackingBanner)

        // ── ROUND 28 items 155 + 160: the start flow's modal ─────────────────
        //
        // Over everything, like the tracking popup and for the same reason: it
        // is the one thing on screen while it is up. Two states, ONE size —
        // see `StartModalCard`.
        val block = startBlock
        if (block != null) {
            StartBlockModal(
                block = block,
                onRetry = onStartBlockRetry,
                onStartAnyway = onStartAnyway,
                onCancel = onDismissStartBlock,
            )
        } else if (startHold != null) {
            val hold = startHold
            val fraction = hold.progress?.fraction ?: 0f
            val secondsLeft = (
                ((1f - fraction) * CaptureViewModel.START_HOLD_TIMEOUT_MS) / 1000f
                ).toInt().coerceAtLeast(0)
            StartHoldModal(
                secondsLeft = secondsLeft,
                fraction = fraction,
                label = hold.progress?.label ?: "Hold still",
                // ROUND 28 item 168: the attitude instrument, inside the card
                // that is telling him to hold still — and ROUND 30 item 175,
                // which is what finally makes it move: the live feed, not the
                // start orientation that does not exist yet at this point in
                // the sequence.
                attitude = attitude,
                onPostureLost = onPostureLost,
                onCancel = onDismissStartBlock,
            )
        }

        // ── ROUND 24 item 110(b): the tour, over everything ─────────────────
        //
        // Last, so it dims the whole Scan screen — but NOT the floating tab
        // bar, which `LidarScanApp` draws over this destination. That is what
        // makes the Projects step work without reaching into another
        // composable: on the last step the one bright thing left on a dark
        // screen is the tab bar the step is about.
        val tutorialAnchors = LocalTutorialAnchors.current
        if (tutorialState.running) {
            ScanTutorialOverlay(
                state = tutorialState,
                anchors = tutorialAnchors ?: emptyMap(),
                onNext = onTutorialNext,
                onSkip = onTutorialSkip,
            )
        }
    }
    }

    // ── ROUND 19 item 77: the pre-scan checklist, over everything ───────────
    //
    // Not part of the `when (sheet)` family below: it is not operator-opened
    // chrome, it is the intercepted Start press, and it must never be closed
    // by the same state the settings sheet uses.
    // ── ROUND 20 item 83: New-capture's one dialog — only over a LIVE capture.
    if (showNewCaptureConfirm) {
        AlertDialog(
            shape = RoundedCornerShape(ScanDims.DialogRadius),
            onDismissRequest = onDismissNewCaptureConfirm,
            title = { Text(com.lidarscan.core.Wording.NEW_CAPTURE_TITLE) },
            // ROUND 24 item 110(a): was 30 words in a dialog over a live
            // recording, which is the worst possible place for a paragraph.
            text = { Text(com.lidarscan.core.Wording.NEW_CAPTURE_BODY) },
            confirmButton = {
                TextButton(
                    onClick = onConfirmNewCapture,
                    modifier = Modifier.testTag("newCaptureConfirm"),
                ) { Text(com.lidarscan.core.Wording.NEW_CAPTURE_CONFIRM) }
            },
            dismissButton = {
                TextButton(
                    onClick = onDismissNewCaptureConfirm,
                    modifier = Modifier.testTag("newCaptureDismiss"),
                ) { Text(com.lidarscan.core.Wording.NEW_CAPTURE_DISMISS) }
            },
            modifier = Modifier.testTag("newCaptureConfirmDialog"),
        )
    }

    if (showPreScanChecklist) {
        PreScanChecklistSheet(
            mountTrim = mountTrim,
            dndNote = dndNote,
            trackingLabel = when (poseState) {
                PoseTrackingState.NOT_REQUIRED -> ""
                PoseTrackingState.UNAVAILABLE -> "No position tracking — this scan would be 2D"
                PoseTrackingState.INITIALIZING -> "Still settling — move the phone slowly"
                PoseTrackingState.TRACKING -> "Tracking steady"
                PoseTrackingState.LOST -> "Tracking lost — point at a textured surface"
            },
            trackingIsGood = poseState == PoseTrackingState.TRACKING ||
                poseState == PoseTrackingState.NOT_REQUIRED,
            onStart = onChecklistStart,
            onDismiss = onChecklistDismiss,
        )
    }

    // ── the sheets, mutually exclusive by construction ──────────────────────
    when (sheet) {
        // ROUND 8 (item 28): everything that configures the SCAN. See
        // CaptureConfigSheet, and CaptureLayout for why it is a sheet at all.
        CaptureSheet.CAPTURE -> CaptureConfigSheet(
            sheetState = configSheetState,
            preset = preset,
            deviceTierLabel = deviceTierLabel,
            presetChangeNote = presetChangeNote,
            presetCaution = presetCaution,
            onPresetChange = onPresetChange,
            onDismissPresetNote = onDismissPresetNote,
            // ── ROUND 27 item 133(c) ────────────────────────────────────
            //
            // The config chip is the sheet's one owner now (the map-mode chip
            // toggles instead of opening it), so the chip has to be offered on
            // the DISCONNECTED screen too — otherwise removing the second door
            // would have removed the only door there. Round 26's reason for not
            // offering it stands and is answered rather than overruled: the
            // sheet must not put a SECOND `scanNameField` or a second
            // `manualLidarIpField` in the tree while the connect flow is already
            // drawing them. So while the connect flow is on screen the sheet
            // simply does not draw its duplicates — `autoName = null` drops the
            // name field, and `connection` says where the controls are instead
            // of repeating them. Everything the sheet uniquely owns (preset,
            // profile, live map, live SLAM) is reachable in both states.
            // ROUND 29 item 170: `compact` was a proxy for "the connect flow is
            // on the screen behind this sheet", and it stopped being one the
            // moment portrait's disconnected page became §D.1 — see
            // `connectFlowOnScreen`.
            autoName = newScan?.autoName.takeIf { !connectFlowOnScreen },
            scanName = scanName,
            onScanNameChange = onScanNameChange,
            // The profile is only settable while the project does not exist yet
            // — afterwards it is a property of the .lscan, not a control.
            profile = if (project == null && !isReplaySession) profile else null,
            onProfileChange = onProfileChange,
            liveMapEnabled = liveMapEnabled,
            onLiveMapEnabledChange = onLiveMapEnabledChange,
            liveSlam = liveSlam,
            liveSlamEditable = !live && connected,
            // ROUND 13: a COIN-D6 can never feed the LIO this gates.
            // ROUND 25 item 119: a 2-D lidar cannot run live SLAM — one scan
            // plane does not constrain a 6-DoF pose — and that is true of the
            // STL-27L for exactly the reason it is true of the D6.
            liveSlamSupported = !sensor.isPhoneTrackedPushbroom,
            onLiveSlamChange = onLiveSlamChange,
            connection = {
                if (connectFlowOnScreen && autoConnectState != null) {
                    // The connect flow is on the screen BEHIND this sheet. A
                    // second copy of it here is two `manualLidarIpField` nodes
                    // and two "Retry" links driving one state.
                    Hint(
                        "The connect panel is on the screen behind this sheet.",
                        color = ScanColors.inkFaint,
                    )
                } else if (autoConnectState != null) {
                    AutoDetectLine(
                        state = autoConnectState,
                        onRetry = onRetryAutoDetect,
                        onShowManual = onShowManualEntry,
                        onHideManual = onHideManualEntry,
                    )
                    if (autoConnectState.manualEntryOpen) {
                        ManualEntryPanel(
                            devices = manualDevices,
                            lidarIp = manualLidarIp,
                            hostIp = manualHostIp,
                            busy = autoConnectState.phase ==
                                CaptureAutoConnectState.Phase.CONNECTING,
                            onDeviceConnect = onManualDeviceConnect,
                            onLidarIpChange = onManualLidarIpChange,
                            onHostIpChange = onManualHostIpChange,
                            onMid360Connect = onManualMid360Connect,
                        )
                    }
                } else {
                    Hint("Replay session — there is no device to connect.", color = ScanColors.inkFaint)
                }
            },
            mount = if (poseTrackingRequired && sensor.isPhoneTrackedPushbroom) {
                {
                    MountReferenceDetail(
                        mountTrim = mountTrim,
                        mountTrimProvenance = mountTrimProvenance,
                        mountIsNominal = mountIsNominal,
                        nowMillis = nowMillis,
                        onOpenMountCalibration = onOpenMountCalibration,
                        onClearMountReference = onClearMountReference,
                    )
                }
            } else {
                null
            },
            onDismiss = { sheet = CaptureSheet.NONE },
        )

        CaptureSheet.SETTINGS -> CaptureSettingsSheet(
            // ROUND 28 item 158: the eye moved off the transport row into this
            // sheet. See the `Live view` row in `CaptureSheets`.
            liveView = liveView,
            onLiveViewChange = onLiveViewChange,
            onOpenTutorial = onStartTutorial,
            sheetState = settingsSheetState,
            // ── ROUND 27 item 139: the Connection tab ───────────────────────
            //
            // The owner: *"put the connection setting in the advance button too
            // with seperate tab."* **Too** — so this is the same connect flow
            // the idle page draws in its main column (item 136b), passed as a
            // slot rather than reimplemented. Two doors, one implementation,
            // one state; there is no second `manualLidarIpField` because there
            // is no second panel, only a second place the same panel is called
            // from — and the two are mutually exclusive, because opening this
            // sheet covers the page that holds the other one.
            //
            // Null over a replay session: there is no device to connect.
            connection = if (autoConnectState != null && !isReplaySession) {
                {
                    AutoDetectLine(
                        state = autoConnectState,
                        onRetry = onRetryAutoDetect,
                        onShowManual = onShowManualEntry,
                        onHideManual = onHideManualEntry,
                    )
                    ManualEntryPanel(
                        devices = manualDevices,
                        lidarIp = manualLidarIp,
                        hostIp = manualHostIp,
                        busy = autoConnectState.phase == CaptureAutoConnectState.Phase.CONNECTING,
                        onDeviceConnect = onManualDeviceConnect,
                        onLidarIpChange = onManualLidarIpChange,
                        onHostIpChange = onManualHostIpChange,
                        onMid360Connect = onManualMid360Connect,
                        ownScroll = false,
                    )
                    Spacer(Modifier.height(10.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        onOpenMid360Setup?.let {
                            SecondaryPill(
                                text = com.lidarscan.core.Wording.MID360_SETUP,
                                height = 40.dp,
                                onClick = it,
                                modifier = Modifier.testTag("advancedMid360Chip"),
                            )
                        }
                        SecondaryPill(
                            text = "Diagnostics",
                            height = 40.dp,
                            onClick = { sheet = CaptureSheet.DIAGNOSTICS },
                            modifier = Modifier.testTag("advancedDiagChip"),
                        )
                    }
                }
            } else {
                null
            },
            cameraMode = cameraMode,
            arAvailable = arAvailable,
            arTrackingLabel = arTrackingLabel(arAvailable, arTracking, cameraMode, keyframesEnabled, poseTrackingRequired),
            arTrackingIsGood = arTracking,
            keyframesEnabled = keyframesEnabled,
            keyframeRateFps = keyframeRateFps,
            colorMode = colorMode,
            colormap = colormap,
            pointSizePx = pointSizePx,
            lodBudgetMPoints = lodBudgetMPoints,
            // ROUND 23 item 106(a): the round-22 Detail model, drawn.
            detailLevels = detailLevels,
            detailLevel = detailLevel,
            detailCeilingNote = detailCeilingNote,
            detailReadout = detailReadout,
            // ROUND 29 item 170: the Capture sheet's one door, now that no page
            // draws a chip row. Setting `sheet` SWAPS rather than stacks —
            // there is one `sheet` value and two modal surfaces can never be
            // open at once, which is what that nullable enum was for.
            onOpenCaptureSheet = { sheet = CaptureSheet.CAPTURE },
            onDetailChange = onDetailChange,
            refreshHz = refreshHz,
            // ROUND 5.3 (item 17): the ceiling is the panel's, read from the
            // display this composable is actually on.
            refreshOptions = com.lidarscan.core.render.RefreshGovernor.optionsFor(
                com.lidarscan.app.render.displayRefreshCeilingHz(androidx.compose.ui.platform.LocalContext.current),
            ),
            gamma = gamma,
            brightness = brightness,
            onRefreshHzChange = onRefreshHzChange,
            onGammaChange = onGammaChange,
            onBrightnessChange = onBrightnessChange,
            onCameraModeChange = onCameraModeChange,
            onKeyframesEnabledChange = onKeyframesEnabledChange,
            onKeyframeRateChange = onKeyframeRateChange,
            onColorModeChange = onColorModeChange,
            onColormapChange = onColormapChange,
            onPointSizeChange = onPointSizeChange,
            onLodChange = onLodChange,
            onDismiss = { sheet = CaptureSheet.NONE },
        )

        CaptureSheet.DIAGNOSTICS -> DiagnosticsSheet(
            sheetState = diagSheetState,
            device = deviceDiagnostics(health, captureState, stats, sensor),
            ar = arDiagnostics(
                arAvailable = arAvailable,
                arTracking = arTracking,
                cameraMode = cameraMode,
                keyframesEnabled = keyframesEnabled,
                keyframeRateFps = keyframeRateFps,
                keyframeStats = keyframeStats,
                trackingLossEpisodes = arTrackingLossEpisodes,
                poseTrackingRequired = poseTrackingRequired,
                posesPushed = posesPushed,
                mountIsNominal = mountIsNominal,
                georefSource = georefSource,
            ),
            onDismiss = { sheet = CaptureSheet.NONE },
        )

        CaptureSheet.NONE -> Unit
    }

    if (sessionSummary != null) {
        ModalBottomSheet(
            onDismissRequest = onDismissSummary,
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            // ROUND 16 item 61: the sheet the owner was pointing at — it is the
            // one the auto-process ("merge") progress lives in, and it was
            // inheriting the theme's pill.
            shape = RoundedCornerShape(
                topStart = ScanDims.SheetRadius,
                topEnd = ScanDims.SheetRadius,
            ),
        ) {
            // ROUND 6 (item 20): the summary now answers "is it saved?" rather
            // than only "how many points?" — the previous sheet was perfectly
            // confident about a capture that had vanished.
            SessionSummaryContent(
                summary = sessionSummary,
                scanSummary = scanSummary,
                savedPath = lastSavedProject,
                saveError = saveError,
                autoProcess = autoProcess,
                onDismiss = onDismissSummary,
            )
        }
    }
}

// ── ROUND 8: the compact chrome (item 28) + the mount state (item 30c) ──────

/**
 * ROUND 8, owner item 30c — **"Mount set · 132.8° · 2 min ago", always on
 * screen.**
 *
 * The owner's ROUND 7 session ran five captures on `trim=none` and was never
 * told. The panel's only mount affordance was a button reading "Set mount
 * reference", which is an *action*, not a *state*: it looks identical whether
 * the pushbroom is running on a measured re-zero or on a CAD nominal that is
 * 132° wrong, and 132° of unmodelled mount rotation is a scan of walls that
 * cannot be straight in any frame (ROUND 7 §2). So the state is now a chip that
 * says which, in words, at the top of the capture screen, with no sheet to open
 * and nothing to scroll.
 *
 * The age re-ticks: [com.lidarscan.core.calib.MountTrimProvenance] is
 * recomputed on `CaptureViewModel`'s 15 s tick, so "just now" becomes "2 min
 * ago" while the operator watches, which is the only thing that makes an age
 * worth printing.
 *
 * The colour is load-bearing too — teal for a trim in force, amber for none —
 * because "Mount not set · CAD nominal" in the same ink as everything else is
 * how a warning becomes wallpaper.
 */
@Composable
private fun MountStateRow(
    provenance: com.lidarscan.core.calib.MountTrimProvenance?,
    hasTrim: Boolean,
    hold: com.lidarscan.core.calib.MountTrimRefiner.Progress? = null,
    onSetMountReference: () -> Unit,
    onBeginMountHold: () -> Unit = onSetMountReference,
    onCancelMountHold: () -> Unit = {},
) {
    val shape = RoundedCornerShape(50)
    val holding = hold != null
    val accent = when {
        holding -> if (hold?.gatePasses == true) ScanColors.sensorD6 else ScanColors.warn
        !hasTrim -> ScanColors.warn
        provenance?.warn == true -> ScanColors.warn
        else -> ScanColors.sensorD6
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(CaptureLayout.MOUNT_ROW_DP.dp)
            .padding(horizontal = 14.dp, vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Box(
            Modifier
                .weight(1f)
                .height(38.dp)
                .background(accent.copy(alpha = 0.12f), shape)
                .border(1.dp, accent, shape)
                .padding(horizontal = 12.dp)
                .testTag("mountStateChip"),
            contentAlignment = Alignment.CenterStart,
        ) {
            // ROUND 11 (owner item 45a): while a hold is running, this chip IS
            // the ring — the same 46 dp row, no new chrome, no height budget to
            // renegotiate with CaptureLayout. The bar under the label sweeps
            // with the hold and empties the instant the gate stops passing,
            // which is the feedback the seven-refusals-in-44-seconds log was
            // missing.
            if (hold != null) {
                Column(Modifier.fillMaxWidth()) {
                    Text(
                        hold.label,
                        style = ScanMetaCaps,
                        color = accent,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.testTag("mountHoldLabel"),
                    )
                    Spacer(Modifier.height(4.dp))
                    androidx.compose.material3.LinearProgressIndicator(
                        progress = { hold.fraction },
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(4.dp)
                            .testTag("mountHoldRing"),
                        color = accent,
                        trackColor = accent.copy(alpha = 0.18f),
                    )
                }
            } else {
                Text(
                    provenance?.chipLabel ?: "Mount not set · CAD nominal",
                    style = ScanMetaCaps,
                    color = accent,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        // The Set button stays on the screen next to the state rather than
        // inside the Capture sheet: a re-zero is taken while holding the rig in
        // the pose you are about to walk with, and reaching it through a sheet
        // is one more hand movement during the exact second that has to be
        // still. The explanation and Clear live in the sheet; the tap does not.
        //
        // ROUND 11 (owner item 45a): the tap now STARTS a hold instead of
        // judging one that has already finished. Tap, hold the rig still,
        // watch it fill, and it sets itself; tap again to abandon. The
        // one-second gate underneath is unchanged — the difference is that the
        // operator can see it passing instead of being told afterwards that it
        // did not.
        SecondaryPill(
            text = when {
                holding -> "Cancel"
                hasTrim -> "Re-zero"
                else -> "Set mount ref"
            },
            height = 38.dp,
            onClick = if (holding) onCancelMountHold else onBeginMountHold,
            modifier = Modifier.testTag("setMountReferenceButton"),
        )
    }
}

/**
 * ROUND 8, owner item 28 — **the one row that replaces the settings stack.**
 *
 * `[Capture · Optimal] [Display] [Diag]`, plus the pose-tracking chip that used
 * to sit inside the pre-capture strip (it is a live quality read-out for a
 * phone-tracked D6, so it belongs where it can be seen during a walk, not in a
 * band that disappears when recording starts).
 *
 * The Capture chip carries the current preset as its own read-out, which is the
 * one thing ROUND 6's three-button `PERFORMANCE` row was really for: a preset
 * is switched perhaps once a session, but it is *checked* every time the phone
 * gets warm. A read-out costs 0 dp; three buttons cost 70.
 *
 * Horizontally scrollable, so a long scan name or a fourth chip can never wrap
 * the row into two and blow the height budget [CaptureLayout] depends on.
 */
@Composable
private fun CaptureChipRow(
    preset: com.lidarscan.core.capture.PerformancePreset,
    scanName: String,
    poseState: PoseTrackingState,
    poseChipVisible: Boolean,
    showCaptureChip: Boolean,
    onOpenCapture: () -> Unit,
    onOpenDiagnostics: () -> Unit,
    /** ROUND 23 item 106(c): non-null shows the Mid-360 setup chip. */
    onOpenMid360Setup: (() -> Unit)? = null,
    /** ROUND 23 item 106(c): non-null shows the RTK chip. */
    onOpenRtk: (() -> Unit)? = null,
    /** ROUND 20 item 83: non-null shows the New-capture chip (owner-requested). */
    onNewCapture: (() -> Unit)? = null,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(CaptureLayout.CHIP_ROW_DP.dp)
            // ROUND 24 item 110(b): the tour's "these chips show your state"
            // step rings this row.
            .then(rememberTutorialAnchor(com.lidarscan.core.capture.TutorialAnchor.CHIP_ROW))
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 14.dp, vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        // ROUND 26 item 124: the ? left this row for the viewport's bottom-end
        // corner ([TutorialChip]). It is drawn ONCE — a second `tutorialButton`
        // in the tree is not a duplicated affordance, it is an ambiguous test
        // selector, and `Round24UiTest` drives the whole tour through that tag.
        if (showCaptureChip) {
            SheetChip(
                label = scanName.takeIf { it.isNotBlank() } ?: "Capture",
                readout = if (preset.isSelectable) preset.displayName else "Custom",
                testTag = "captureConfigChip",
                onClick = onOpenCapture,
            )
        }
        // ── ROUND 23 item 102 ────────────────────────────────────────────────
        //
        // The "Display" chip opened `CaptureSheet.SETTINGS` — the SAME sheet as
        // the viewport's ⚙ and the transport row's Advanced button. Counting
        // the owner's complaint honestly, that was three doors to one room,
        // and round 22's whole point was that there should be one. The chip
        // row keeps the chips that go somewhere ELSE (the capture config sheet
        // and diagnostics) and the New-capture reset, and the parameter that
        // opened the settings sheet from here is gone with the chip.
        SheetChip(label = "Diag", readout = null, testTag = "diagnosticsSheetChip", onClick = onOpenDiagnostics)
        // ── ROUND 23 item 106(c): the Mid-360 doors, contextual ─────────────
        //
        // Shown only when a Mid-360 is the selected sensor (or Advanced is on)
        // — `SimpleMode.showsMid360Connect` / `showsRtk` decide, one tab up.
        // The owner must be able to reach the wizard and the rover WITHOUT
        // first discovering a switch in Settings, which is what round 22's own
        // item 97 promised and never wired to a surface.
        if (onOpenMid360Setup != null) {
            SheetChip(
                label = com.lidarscan.core.Wording.MID360_SETUP,
                readout = null,
                testTag = "mid360SetupChip",
                onClick = onOpenMid360Setup,
            )
        }
        if (onOpenRtk != null) {
            SheetChip(
                label = com.lidarscan.core.Wording.RTK_SETUP,
                readout = null,
                testTag = "rtkChip",
                onClick = onOpenRtk,
            )
        }
        // ROUND 20 item 83 — "add a new-capture button to clear all settings
        // and refresh for a new scan with new settings". Per-scan state and
        // per-scan choices reset; device facts (mount profile, DND, developer
        // prefs) deliberately survive — see CaptureViewModel.performNewCapture.
        if (onNewCapture != null) {
            SheetChip(
                label = "New capture",
                readout = null,
                testTag = "newCaptureChip",
                onClick = onNewCapture,
            )
        }
        if (poseChipVisible && poseState != PoseTrackingState.NOT_REQUIRED) {
            ScanChip(
                text = poseState.chipLabel,
                color = poseState.chipColor,
                showDot = true,
                modifier = Modifier.testTag("poseTrackingChip"),
            )
        }
    }
}

/** One chip-shaped door to a sheet: a name, an optional read-out, a 40 dp target. */
@Composable
private fun SheetChip(label: String, readout: String?, testTag: String, onClick: () -> Unit) {
    val shape = RoundedCornerShape(50)
    Row(
        Modifier
            .height(38.dp)
            .background(MaterialTheme.colorScheme.surfaceContainer, shape)
            .border(1.dp, MaterialTheme.colorScheme.outline, shape)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = "$label settings" }
            .padding(horizontal = 14.dp)
            .testTag(testTag),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            label,
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 13.sp,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.widthIn(max = 150.dp),
        )
        if (readout != null) {
            Spacer(Modifier.width(6.dp))
            Text(readout.uppercase(), style = ScanMetaCaps, color = Ember, maxLines = 1)
        }
    }
}

/**
 * Owner item 20's other half: **a capture that did not save says so, loudly.**
 *
 * A red-bordered banner at the very top of the capture body, not a `Hint` among
 * the other quiet inline lines and emphatically not a toast. The previous
 * behaviour — `updateManifest` returning null into a discarded value — is how a
 * whole field session ended with a confident session-summary sheet and an empty
 * Projects tab. It stays until tapped, and it names the on-disk path so the raw
 * capture can be rescued even when the metadata could not be written.
 */
@Composable
private fun SaveErrorBanner(message: String, onDismiss: () -> Unit) =
    LoudBanner("Scan not saved", message, onDismiss, "saveErrorBanner")

/**
 * ROUND 8: a third caller, and the reason [LoudBanner] grew an [accent].
 *
 * A refused mount re-zero is not a data-loss failure — nothing was lost, the
 * operator simply has to hold still and tap again — so it is amber rather than
 * red. It is a *banner* rather than a `Hint` because the owner tapped that
 * control eight times in one field session against a grey one-line refusal and
 * concluded the feature did not work, which is the same silence problem in a
 * quieter register.
 */

/**
 * The one shape on this screen that is allowed to shout: a red-bordered block
 * at the top of the capture body, not a `Hint` and not a toast, that stays
 * until it is tapped.
 *
 * ROUND 6 introduced it for "the capture did not save". ROUND 7 gives it a
 * second job — "the capture is running and receiving nothing" — because those
 * are the two failures that used to be silent, and silence is what cost the
 * owner two field sessions.
 */
/**
 * ROUND 24 item 111 — the `Activity` behind a Compose `Context`, or null.
 *
 * `ContextWrapper` chains are why this is a loop rather than a cast: a themed
 * context, a `ContextThemeWrapper` from an AppCompat host or a decor-view
 * context are all wrappers around the Activity, and a bare `as? Activity`
 * returns null for every one of them. Null is a real answer (a preview, a test
 * harness) and it reads as "not a configuration change", which is the safe
 * default: the worst it can do is start a new scan on a screen that had none.
 */
private tailrec fun android.content.Context.findActivity(): android.app.Activity? = when (this) {
    is android.app.Activity -> this
    is android.content.ContextWrapper -> baseContext.findActivity()
    else -> null
}

/**
 * ROUND 23 item 105 / **ROUND 24 item 112 — STOP WALKING, in the middle of the
 * screen.**
 *
 * The owner's original request: *"a warning need to tell user stop walking
 * while tracking lost until the tracking back."* Round 23 shipped it as a
 * full-width amber banner at the top of the loud band, and his verdict on 0.9.8
 * is the correction: a band at the top of a screen he is not looking at, while
 * walking with the phone at hip height, is not a warning. So it becomes what a
 * warning of this weight has to be — the screen dims and one amber card sits in
 * the middle of it.
 *
 * The measurement is unchanged and is still the reason this exists: scan-070's
 * 4.1 s gap carried **73.34°** of gyro turn against a reported 12.70°. Nothing
 * that runs afterwards can heal a gap that big, because the two sides of it no
 * longer share any geometry to agree about. Standing still keeps the gap
 * closeable; walking through it does not.
 *
 * ## Four properties, and each one is a decision
 *
 *  * **The scrim does not eat touches.** It is a `Box` with a background and
 *    no pointer-input modifier, so it draws over the transport row and
 *    consumes nothing: the **STOP button underneath stays tappable**, which
 *    item 112 requires outright — an operator may want to abandon the scan,
 *    and a modal that traps them in a bad capture is worse than the bad
 *    capture.
 *  * **It has no dismiss.** No X, no tap-to-close, no timeout. It goes when
 *    tracking returns or when the recording stops, and nothing else.
 *  * **It cannot appear outside a recording.** That gate is in
 *    [com.lidarscan.core.capture.TrackingLossBanners.next] — `!recording`
 *    returns the empty state — and it is where it belongs, because it is a
 *    rule rather than a rendering.
 *  * **The presentation is ALL that changed.** The state machine, its two-
 *    second green linger, the strong haptic on the lost edge (the existing
 *    `CueKind.TRACKING_DEGRADED` channel) and the light GO tick on recovery are
 *    exactly what round 23 shipped. The test tags are kept too, so the
 *    round-23 assertions still mean what they meant.
 */
@Composable
internal fun TrackingLossPopup(state: com.lidarscan.core.capture.TrackingBannerState) {
    if (state.banner == com.lidarscan.core.capture.TrackingBanner.NONE) return
    val lost = state.banner == com.lidarscan.core.capture.TrackingBanner.LOST
    // ROUND 25 item 116: the semantic CONTAINER pair, not a raw hex and not a
    // neutral card wearing a coloured ring. One token decides the ground, one
    // decides everything drawn on it, and both move together if the semantic
    // amber is ever retuned.
    val container = if (lost) ScanColors.warnContainer else ScanColors.goodContainer
    val onContainer = if (lost) ScanColors.warn else ScanColors.good

    // The amber card counts up at 250 ms, like the start panel's own tick, so
    // "how long have I been standing here" is answered without arithmetic.
    var tick by remember { mutableStateOf(System.currentTimeMillis()) }
    LaunchedEffect(state.banner, state.sinceMillis) {
        while (lost) {
            tick = System.currentTimeMillis()
            kotlinx.coroutines.delay(250)
        }
    }
    val elapsed = (tick - state.sinceMillis).coerceAtLeast(0L)

    Box(
        Modifier
            .fillMaxSize()
            // A scrim with NO pointer-input modifier. Compose only routes a
            // touch to a node that handles pointer input, so this darkens the
            // screen without stealing a single tap — which is what keeps STOP
            // live underneath. Deliberately not a `Dialog`: a Dialog is its own
            // window and would swallow every touch in the app.
            .background(Color.Black.copy(alpha = 0.62f))
            .testTag("trackingPopupScrim"),
        contentAlignment = Alignment.Center,
    ) {
        // ROUND 25 item 116 — this is now the app's OWN card component, with
        // the app's radius, the app's hairline weight and the app's padding,
        // instead of a `Column` that reproduced two of the three and invented a
        // 3 dp ring for the rest. The only geometry this card states for itself
        // is the one thing that IS particular to it: it floats over a scrim, so
        // it casts a shadow. Both banners get the identical call — item 116
        // asks for the same geometry on the green card, and the way to
        // guarantee that is to have one call site rather than two that agree.
        ScanCard(
            modifier = Modifier
                .padding(horizontal = 26.dp)
                // The round-23 tags, kept: the presentation moved, the meaning
                // did not, and a renamed tag would silently retire the
                // assertions that pin it.
                .testTag(if (lost) "trackingLostBanner" else "trackingBackBanner"),
            container = container,
            borderColor = onContainer.copy(alpha = 0.55f),
            // §C.5: a modal over a scrim is one of the three things that may
            // float. It was 16 dp; level 1 is 4 dp with a wide blur, which is
            // the same read at a fifth of the ink.
            floating = true,
            contentPadding = PaddingValues(horizontal = 20.dp, vertical = 22.dp),
        ) {
            // Status iconography consistent with the rest of the app, which is
            // the `Icons.Filled` family throughout — and `CheckCircle` for the
            // green edge is the SAME glyph the process-result lines and the
            // selection tick already use for "this is fine".
            Icon(
                if (lost) Icons.Filled.Warning else Icons.Filled.CheckCircle,
                contentDescription = null,
                tint = onContainer,
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .size(34.dp)
                    .testTag("trackingBannerIcon"),
            )
            Spacer(Modifier.height(10.dp))
            Text(
                if (lost) {
                    com.lidarscan.core.capture.TrackingLossBanners.LOST_TEXT
                } else {
                    com.lidarscan.core.capture.TrackingLossBanners.REGAINED_TEXT
                },
                // The app's type scale rather than a hand-set size: this is a
                // headline on a card, so it is `headlineSmall` — Display
                // family, semi-bold, the redesign's negative tracking — read at
                // hip height, so it is scaled up by the one factor the role
                // does not carry. 26 sp was the right SIZE and the wrong way to
                // ask for it: a hand-set `fontSize` with a hand-set weight is a
                // fourth typographic dialect on a screen that already has
                // three.
                style = MaterialTheme.typography.headlineSmall.copy(fontSize = 26.sp),
                color = onContainer,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .testTag("trackingBannerText"),
            )
            if (lost) {
                Spacer(Modifier.height(8.dp))
                Text(
                    com.lidarscan.core.capture.TrackingLossBanners.lostDetail(elapsed),
                    // Tabular figures: the count is centred and changes every
                    // second, and proportional digits would slide the whole
                    // line sideways under a card that is telling the operator
                    // to stand still. See `MonoTabular`.
                    style = MonoTabular,
                    color = onContainer,
                    modifier = Modifier
                        .align(Alignment.CenterHorizontally)
                        .testTag("trackingLostElapsed"),
                )
            }
        }
    }
}

/**
 * ROUND 21 (item 85) — the unified start-progress panel, owner request
 * verbatim: "i dont know what is the app loading with, show me the progress
 * and tell me what i am waiting for and how long and what should i do while
 * waiting".
 *
 * One panel, up from the instant Start is pressed, showing the whole
 * sequence — (1) new tracking session, (2) locking position tracking (the
 * ROUND 12 gate, with its live verdict in plain words), (3) measuring the
 * mount (the ROUND 20 hold-steady stage, folded in from what used to be
 * HoldSteadyBanner), (4) GO. Each stage carries a one-line instruction for
 * what the operator should DO, and the active stage shows elapsed against
 * what is expected. A press swallowed by the ROUND 17 one-press guard pulses
 * the panel ([CaptureViewModel.StartProgress.pulses]) — the app answering
 * "yes, I heard you, I'm already on it" instead of the silence that made the
 * owner press three times at 01:29–01:31.
 *
 * Not dismissible (it IS the start flow), never a modal (round 5's rule), and
 * it clears itself — GO lingers [CaptureViewModel.START_HOLD_GO_LINGER_MS]
 * into the walk and vanishes. Wording never blames light or the room
 * (round-19 owner correction): every instruction is the measured diet —
 * scanning pose, features an arm's length or more away.
 */
@Composable
private fun StartProgressPanel(
    progress: CaptureViewModel.StartProgress?,
    warmup: com.lidarscan.core.capture.TrackingWarmup.Verdict?,
    hold: CaptureViewModel.StartHoldState?,
) {
    val go = hold?.go == true
    val accent = if (go) ScanColors.good else ScanColors.sensorD6
    val shape = RoundedCornerShape(ScanDims.TileRadius)

    // Which stage is live. `hold` outlives `progress` by the GO linger, so GO
    // is derived from the hold state itself.
    val activeStage = when {
        go -> 3
        hold != null || progress?.stage == CaptureViewModel.StartStage.HOLD -> 2
        progress?.stage == CaptureViewModel.StartStage.GATE -> 1
        else -> 0
    }

    // Elapsed since the press, ticking while the panel is up.
    var nowMillis by remember { mutableStateOf(System.currentTimeMillis()) }
    LaunchedEffect(progress?.beganAtMillis) {
        while (true) {
            nowMillis = System.currentTimeMillis()
            kotlinx.coroutines.delay(250)
        }
    }
    val elapsedS = progress?.let { ((nowMillis - it.beganAtMillis) / 1000L).coerceAtLeast(0L) }

    // Item 85: a swallowed press flashes the panel and answers out loud.
    val pulses = progress?.pulses ?: 0
    var pulseFlash by remember { mutableStateOf(false) }
    LaunchedEffect(pulses) {
        if (pulses > 0) {
            pulseFlash = true
            kotlinx.coroutines.delay(900)
            pulseFlash = false
        }
    }

    val gateCapS = 2 * com.lidarscan.core.capture.TrackingWarmup.MAX_WAIT_MILLIS / 1000L
    val holdCapS = CaptureViewModel.START_HOLD_TIMEOUT_MS / 1000L

    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 14.dp, vertical = 6.dp)
            .background(accent.copy(alpha = if (pulseFlash) 0.26f else 0.14f), shape)
            .border(if (pulseFlash) 2.dp else 1.dp, accent, shape)
            .padding(12.dp)
            .testTag("startProgressPanel"),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (go) "Go — start walking" else "Starting scan",
                style = ScanMetaCaps,
                color = accent,
                modifier = Modifier.testTag(if (go) "holdSteadyGo" else "startProgressTitle"),
            )
            Spacer(Modifier.weight(1f))
            if (!go && elapsedS != null) {
                Text(
                    "${elapsedS}s · usually 4–8 s",
                    style = ScanMetaCaps,
                    color = ScanColors.inkFaint,
                )
            }
        }
        // ── ROUND 23 item 106(b): the checklist, folded in ──────────────────
        //
        // ROUND 19's modal showed four rows on the first press of every device,
        // most of them saying that everything was fine. These are the same
        // checks, in the panel that is already on screen, and only when one of
        // them has something to report. `PreScanChecklistSheet` and
        // `startCapture(skipChecklist)` stay compiled and tested behind
        // `FeatureFlags.PRE_SCAN_CHECKLIST_SHEET`.
        val checks = progress?.checks.orEmpty()
        if (checks.isNotEmpty()) {
            Spacer(Modifier.height(4.dp))
            Column(Modifier.testTag("startProgressChecks")) {
                checks.forEach { check ->
                    Text(
                        check,
                        style = MaterialTheme.typography.bodySmall,
                        color = ScanColors.warn,
                    )
                }
            }
        }
        if (pulseFlash) {
            Spacer(Modifier.height(4.dp))
            Text(
                com.lidarscan.core.Wording.START_HEARD_YOU,
                style = MaterialTheme.typography.bodySmall,
                color = accent,
                modifier = Modifier.testTag("startProgressPulseNote"),
            )
        }
        Spacer(Modifier.height(6.dp))

        // ── stage 1: the ROUND 14 world-frame reset ─────────────────────────
        StartStageRow(
            index = 0,
            activeStage = activeStage,
            title = "New tracking session",
            status = "Fresh world frame for this scan — under a second.",
            instruction = null,
            accent = accent,
        )
        // ── stage 2: the ROUND 12 tracking gate ─────────────────────────────
        StartStageRow(
            index = 1,
            activeStage = activeStage,
            title = "Locking position tracking",
            status = when {
                activeStage != 1 -> null
                warmup == null -> "Watching the tracker settle…"
                warmup.blocker == com.lidarscan.core.capture.TrackingWarmup.Blocker.NO_POSES ->
                    "Camera warming up — nothing from the tracker yet…"
                warmup.blocker == com.lidarscan.core.capture.TrackingWarmup.Blocker.NOT_TRACKING ->
                    "Tracking not locked yet…"
                else -> "Steady %.1f s of the 2 s needed".format(warmup.stableMillis / 1000.0)
            }?.plus("  (waits up to $gateCapS s)"),
            // ROUND 24 item 110(a): was 24 words, shown during the one stage
            // the operator IS watching the screen. The round-19 rule holds —
            // it names furniture, never the light.
            instruction = com.lidarscan.core.Wording.START_LOOK_AT,
            accent = accent,
            fraction = if (activeStage == 1) warmup?.fraction else null,
        )
        // ── stage 3: the ROUND 20 hold-steady stage (the old banner, folded in) ──
        val p = hold?.progress
        StartStageRow(
            index = 2,
            activeStage = activeStage,
            title = "Measuring the mount — hold still",
            status = when {
                activeStage != 2 -> null
                // ROUND 22 item 92: a refused reading is the FIRST thing this
                // row says. The owner held his phone perfectly still while the
                // pose slid out from under it, and this row told him
                // "Steady… 3.2° and improving" — which is the app reading a
                // tracking fault as a mount measurement and then congratulating
                // him on it.
                hold?.refusal != null ->
                    com.lidarscan.core.calib.StartHoldTrimGate.refusalStatus(hold.refusal)
                p == null -> "Hold the phone still in your scanning pose…"
                !p.gatePasses && p.holdMillis < 300L -> "Hold the phone still in your scanning pose…"
                !p.gatePasses -> "Moved a little — measuring again from now. Keep holding…"
                p.stabilityDeg >= 0.0 -> "Steady… %.1f° and improving. Keep holding.".format(p.stabilityDeg)
                else -> "Steady… keep holding."
            }?.plus("  (usually 1–2 s, up to $holdCapS s)"),
            instruction = "Stand still exactly as you will scan. The walk starts at GO.",
            accent = accent,
            fraction = if (activeStage == 2) p?.fraction else null,
            statusTag = if (activeStage == 2) "holdSteadyHolding" else null,
        )
        // ── stage 4: GO ──────────────────────────────────────────────────────
        StartStageRow(
            index = 3,
            activeStage = activeStage,
            title = "GO — start walking",
            status = when {
                !go -> null
                hold?.fallbackNote != null -> hold.fallbackNote
                else -> "Mount reference set in this scan's own frame — recording."
            },
            instruction = null,
            accent = accent,
        )
    }
}

/** One row of [StartProgressPanel]: ✓ done · ● live (status + instruction + bar) · ○ waiting. */
@Composable
private fun StartStageRow(
    index: Int,
    activeStage: Int,
    title: String,
    status: String?,
    instruction: String?,
    accent: Color,
    fraction: Float? = null,
    statusTag: String? = null,
) {
    val done = index < activeStage
    val live = index == activeStage
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            when {
                done -> "✓"
                live -> "●"
                else -> "○"
            },
            style = ScanMetaCaps,
            color = if (done || live) accent else ScanColors.inkFaint,
        )
        Spacer(Modifier.width(8.dp))
        Text(
            title,
            style = ScanMetaCaps,
            color = if (live) accent else if (done) MaterialTheme.colorScheme.onSurface else ScanColors.inkFaint,
        )
    }
    if (live && status != null) {
        Text(
            status,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier
                .padding(start = 20.dp)
                .let { m -> if (statusTag != null) m.testTag(statusTag) else m },
        )
    }
    if (live && instruction != null) {
        Text(
            instruction,
            style = MaterialTheme.typography.bodySmall,
            color = ScanColors.inkFaint,
            modifier = Modifier.padding(start = 20.dp),
        )
    }
    if (live && fraction != null) {
        Spacer(Modifier.height(4.dp))
        androidx.compose.material3.LinearProgressIndicator(
            progress = { fraction },
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 20.dp)
                .testTag(if (index == 2) "holdSteadyProgress" else "startProgressBar"),
            color = accent,
        )
    }
    Spacer(Modifier.height(4.dp))
}

@Composable
private fun LoudBanner(
    title: String,
    message: String,
    onDismiss: () -> Unit,
    testTag: String,
    accent: Color = ScanColors.bad,
) {
    val shape = RoundedCornerShape(ScanDims.TileRadius)
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 14.dp, vertical = 6.dp)
            .background(accent.copy(alpha = 0.14f), shape)
            .border(1.dp, accent, shape)
            .clickable(onClick = onDismiss)
            .padding(12.dp)
            .testTag(testTag),
    ) {
        Text(
            title,
            style = ScanMetaCaps,
            color = accent,
        )
        Spacer(Modifier.height(4.dp))
        Text(
            message,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.height(4.dp))
        Text("Tap to dismiss · Settings → Capture log has the full trace", style = ScanMetaCaps, color = ScanColors.inkFaint)
    }
}

// ── pre-capture strip (ROUND 5) ─────────────────────────────────────────────

/**
 * Everything the operator needs *before* pressing Start, in one strip: the name,
 * what auto-detect found, the inline manual fallback when it found nothing, and
 * the D6 mount hint.
 *
 * Deliberately not a card stack and not a sheet: it is the top of the same screen
 * the live preview is on, so a device that connects while you are reading it
 * shows points behind the text you are reading.
 */
@Composable
private fun PreCaptureStrip(
    /**
     * ROUND 27 item 129 — **the cap, and `Dp.Unspecified` for "the caller
     * already bounds me".**
     *
     * Round 8 gave this strip its own ceiling and its own `verticalScroll`,
     * which was right when it was one child of a non-scrolling capture column.
     * Round 26 moved it INSIDE the floating chrome column, which is itself
     * height-bounded and itself scrollable — and two scrollers on the same axis
     * do not compose: the inner one swallows every drag, so the outer column
     * cannot be scrolled at all, and the inner one clips at 46 % of the screen,
     * which is exactly where the AVD cut the USB SCANNER panel in half.
     *
     * `Dp.Unspecified` means "I have a bounded, scrolling parent — lay out at
     * my natural height and let it do the work". The mount-calibration wizard
     * and the tests that host this strip directly still pass a real number.
     */
    maxHeight: androidx.compose.ui.unit.Dp,
    autoName: String?,
    scanName: String,
    autoConnectState: CaptureAutoConnectState?,
    sensor: SensorType,
    poseTrackingRequired: Boolean,
    poseState: PoseTrackingState,
    /** True once a sensor is actually connected and streaming into the preview. */
    sensorStreaming: Boolean,
    mountIsNominal: Boolean,
    manualDevices: List<ManualSerialDevice>,
    manualLidarIp: String,
    manualHostIp: String,
    onScanNameChange: (String) -> Unit,
    onRetryAutoDetect: () -> Unit,
    onShowManualEntry: () -> Unit,
    onHideManualEntry: () -> Unit,
    onManualDeviceConnect: (ManualSerialDevice, SensorType) -> Unit,
    onManualLidarIpChange: (String) -> Unit,
    onManualHostIpChange: (String) -> Unit,
    onManualMid360Connect: () -> Unit,
    onOpenMountCalibration: (() -> Unit)?,
    /** ROUND 6 (item 23): this session's mount trim, or null when the rig has not been re-zeroed. */
    mountTrim: com.lidarscan.core.calib.MountTrim? = null,
    /** ROUND 7: the trim's own sentence — age, and whether it survived an app restart. */
    mountTrimProvenance: com.lidarscan.core.calib.MountTrimProvenance? = null,
    nowMillis: Long = 0L,
    onSetMountReference: () -> Unit = {},
    onClearMountReference: () -> Unit = {},
    /**
     * ROUND 29 item 170 — **false when a Mount ROW is already on screen.**
     *
     * §D.1's page states the mount as one of its three readiness rows, whose
     * tap IS the re-zero. Drawing this strip's mount block under it would put
     * the same fact on the screen twice — which is precisely what the owner
     * photographed on 0.9.13: a `Mount` row he could not see (round 27's page
     * had none) and, in its place, a 24-word amber paragraph beginning
     * *"Mount trim 91.0° · set 18 h ago — that is old."*
     *
     * The block is untouched for the landscape page and the calibration wizard,
     * which have no row to defer to.
     */
    showMountBlock: Boolean = true,
) {
    val ownScroll = maxHeight != androidx.compose.ui.unit.Dp.Unspecified
    Column(
        Modifier
            .fillMaxWidth()
            .then(
                if (ownScroll) {
                    Modifier.heightIn(max = maxHeight).verticalScroll(rememberScrollState())
                } else {
                    Modifier
                },
            )
            // ROUND 29 item 170: the screen margin, from the token. It was a
            // literal `14.dp` — one of the 69 the round-28 audit counted — and
            // inside §D.1's card it has to share an edge with the `ScanRow`s
            // above it, which are on `CardPadding`.
            .padding(horizontal = ScanDims.CardPadding)
            .testTag("preCaptureStrip"),
    ) {
        if (autoName != null) {
            OutlinedTextField(
                value = scanName,
                onValueChange = onScanNameChange,
                singleLine = true,
                label = { Text("Scan name — optional") },
                placeholder = { Text(autoName, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                modifier = Modifier.fillMaxWidth().testTag("scanNameField"),
            )
            Spacer(Modifier.height(6.dp))
        }

        if (autoConnectState != null) {
            AutoDetectLine(
                state = autoConnectState,
                onRetry = onRetryAutoDetect,
                onShowManual = onShowManualEntry,
                onHideManual = onHideManualEntry,
                // ROUND 29 item 170: the same flag that suppresses the mount
                // block suppresses the clause the Sensor row above already
                // carries — one page, one statement of one blocker.
                underReadinessRow = !showMountBlock,
            )

            if (autoConnectState.manualEntryOpen) {
                ManualEntryPanel(
                    devices = manualDevices,
                    lidarIp = manualLidarIp,
                    hostIp = manualHostIp,
                    busy = autoConnectState.phase == CaptureAutoConnectState.Phase.CONNECTING,
                    onDeviceConnect = onManualDeviceConnect,
                    onLidarIpChange = onManualLidarIpChange,
                    onHostIpChange = onManualHostIpChange,
                    onMid360Connect = onManualMid360Connect,
                    // ROUND 27 item 129: whoever bounds this strip bounds the
                    // panel too. See `ownScroll`.
                    ownScroll = ownScroll,
                )
            }
        }

        // ROUND 5 item 11: the mount hint. Short, and only for the sensors it
        // applies to — a 2-D lidar's mount geometry is the whole reason the
        // capture is 3D at all, which is as true of ROUND 25 item 119's
        // STL-27L as it is of the D6.
        if (showMountBlock && poseTrackingRequired && sensor.isPhoneTrackedPushbroom) {
            Spacer(Modifier.height(4.dp))
            // ROUND 24 item 110(a): was 33 words including "6-DoF". The rest
            // of the explanation is now TutorialStep.SCAN_BUTTON / START_HOLD.
            Hint(
                com.lidarscan.core.Wording.D6_MOUNT_HINT + "\n" + com.lidarscan.core.Wording.D6_MOUNT_DETAIL,
                color = ScanColors.inkFaint,
                modifier = Modifier.testTag("d6MountHint"),
            )
            // ── ROUND 6 (owner item 23): the one-tap mount re-zero ──────
            //
            // The D6 is clamped onto the phone by hand and comes off between
            // scans, so the real `phone_from_lidar` differs from the CAD
            // nominal by an unknown rotation every session — and that
            // rotation lands in every resolved point. Hold the rig the way
            // it will be carried, tap, and the current gravity-aligned
            // attitude becomes this session's trim.
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                SecondaryPill(
                    text = if (mountTrim == null) "Set mount reference" else "Re-zero mount",
                    height = 46.dp,
                    onClick = onSetMountReference,
                    modifier = Modifier.weight(1f).testTag("setMountReferenceButton"),
                )
                if (mountTrim != null) {
                    Spacer(Modifier.width(8.dp))
                    TextButton(
                        onClick = onClearMountReference,
                        modifier = Modifier.testTag("clearMountReferenceButton"),
                    ) { Text("Clear") }
                }
            }
            Hint(
                if (mountTrim == null) {
                    com.lidarscan.core.Wording.MOUNT_REF_HINT + "\n" + com.lidarscan.core.Wording.MOUNT_REF_DETAIL
                } else {
                    // ROUND 7: age AND provenance. A trim restored across an app
                    // restart is still applied — losing it silently is the bug
                    // this round fixed — but it is not the same claim as one set
                    // on this screen a minute ago, and the sentence says which.
                    mountTrimProvenance?.label
                        ?: "Mount trim %.1f° · set %s · travels with the project."
                            .format(mountTrim.magnitudeDeg, mountTrim.ageLabel(nowMillis))
                },
                color = when {
                    mountTrim == null -> ScanColors.inkFaint
                    mountTrimProvenance?.warn == true -> ScanColors.warn
                    else -> ScanColors.sensorD6
                },
                modifier = Modifier.testTag("mountTrimAge"),
            )

            // The tracking chip belongs to a session that exists: with nothing
            // connected, "Tracking · starting" is a status about a scan that
            // is not happening.
            if (poseState != PoseTrackingState.NOT_REQUIRED && sensorStreaming) {
                Spacer(Modifier.height(4.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    ScanChip(
                        text = poseState.chipLabel,
                        color = poseState.chipColor,
                        showDot = true,
                        modifier = Modifier.testTag("poseTrackingChip"),
                    )
                    if (mountIsNominal && onOpenMountCalibration != null) {
                        Spacer(Modifier.width(8.dp))
                        TextButton(onClick = onOpenMountCalibration) { Text("Calibrate mount") }
                    }
                }
                if (poseState == PoseTrackingState.UNAVAILABLE) {
                    Hint(
                        com.lidarscan.core.Wording.NO_TRACKING_HINT + "\n" + com.lidarscan.core.Wording.NO_TRACKING_DETAIL,
                        color = ScanColors.warn,
                    )
                }

            }
        }
        Spacer(Modifier.height(6.dp))
    }
}

/**
 * ROUND 8 (item 28): the mount re-zero's **explanation**, for the Capture
 * sheet.
 *
 * The split is deliberate. What has to be on the capture screen is the *state*
 * and the *tap* — item 30c, and the fact that a re-zero is taken while holding
 * the rig still, which is not a moment to be navigating. What belongs behind a
 * sheet is the paragraph telling you why, the Clear button (a destructive
 * action nobody performs mid-hold) and the door to the full calibration wizard.
 * Together with [MountStateRow] this is the same content the ROUND 6 strip had,
 * with the ~150 dp of prose off the critical screen.
 */
@Composable
private fun MountReferenceDetail(
    mountTrim: com.lidarscan.core.calib.MountTrim?,
    mountTrimProvenance: com.lidarscan.core.calib.MountTrimProvenance?,
    mountIsNominal: Boolean,
    nowMillis: Long,
    onOpenMountCalibration: (() -> Unit)?,
    onClearMountReference: () -> Unit,
) {
    Hint(
        mountTrimProvenance?.label
            ?: if (mountTrim == null) {
                com.lidarscan.core.Wording.MOUNT_REF_MISSING
            } else {
                "Mount trim %.1f° · set %s · travels with the project."
                    .format(mountTrim.magnitudeDeg, mountTrim.ageLabel(nowMillis))
            },
        color = when {
            mountTrim == null -> ScanColors.warn
            mountTrimProvenance?.warn == true -> ScanColors.warn
            else -> ScanColors.sensorD6
        },
        modifier = Modifier.testTag("mountTrimDetail"),
    )
    Spacer(Modifier.height(6.dp))
    // ROUND 24 item 110(a): was 69 words. The instruction half is on the
    // capture screen where the hold actually happens; this is the why.
    Hint(com.lidarscan.core.Wording.MOUNT_REF_WHY + " " + com.lidarscan.core.Wording.MOUNT_REF_HINT, color = ScanColors.inkFaint)
    Row(verticalAlignment = Alignment.CenterVertically) {
        if (mountTrim != null) {
            TextButton(
                onClick = onClearMountReference,
                modifier = Modifier.testTag("clearMountReferenceButton"),
            ) { Text("Clear mount reference") }
        }
        if (mountIsNominal && onOpenMountCalibration != null) {
            TextButton(onClick = onOpenMountCalibration) { Text("Calibrate mount") }
        }
    }
}

/**
 * The auto-detect status line: what is happening, plus Retry / Enter manually.
 *
 * @param underReadinessRow ROUND 29 item 170 — true when this line is drawn
 *   INSIDE §D.1's Sensor row, which already says `Sensor · Not found`, already
 *   carries *"Plug it in, then retry."* and already has its own **Retry**. The
 *   failure clause and the Retry link are therefore suppressed: repeating a
 *   blocker under itself is the defect item 158 removed from the top of this
 *   screen, and re-adding it eight rows down is not an improvement. Everything
 *   the row does NOT say — searching, connecting, the device it found — is
 *   still printed here, because that is state the row has no place for.
 */
@Composable
private fun AutoDetectLine(
    state: CaptureAutoConnectState,
    onRetry: () -> Unit,
    onShowManual: () -> Unit,
    onHideManual: () -> Unit,
    underReadinessRow: Boolean = false,
) {
    val redundant = underReadinessRow && state.phase == CaptureAutoConnectState.Phase.FAILED
    if (!redundant) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        if (state.isBusy) {
            CircularProgressIndicator(
                modifier = Modifier.size(16.dp),
                strokeWidth = 2.dp,
                color = MaterialTheme.colorScheme.primary,
            )
            Spacer(Modifier.width(8.dp))
        }
        Text(
            state.statusLine(),
            style = ScanMeta,
            color = when (state.phase) {
                CaptureAutoConnectState.Phase.PREVIEW -> ScanColors.good
                CaptureAutoConnectState.Phase.FAILED -> ScanColors.warn
                else -> MaterialTheme.colorScheme.onSurfaceVariant
            },
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).testTag("autoDetectStatus"),
        )
    }
    state.detection?.detail?.let {
        Text(it, style = ScanMetaCaps, color = ScanColors.inkFaint, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        if (state.phase == CaptureAutoConnectState.Phase.FAILED && !underReadinessRow) {
            // ROUND 29 item 170 (review finding S13): the recovery action keeps
            // the accent — it is the one thing on this row worth pressing.
            TextButton(onClick = onRetry, modifier = Modifier.testTag("retryAutoDetectButton")) {
                Text("Retry", color = ScanColors.primaryInk)
            }
        }
        // "Enter manually" stays reachable at every phase, including a successful
        // detect (owner addition 1) — a rig with two devices can auto-detect the
        // wrong one.
        //
        // S13: it used to be styled identically to Retry AND be wider, so the
        // disclosure out-shouted the fix. It is a disclosure; it reads like one.
        TextButton(
            onClick = if (state.manualEntryOpen) onHideManual else onShowManual,
            modifier = Modifier.testTag("manualEntryToggle"),
        ) {
            Text(
                if (state.manualEntryOpen) "Hide manual entry" else "Enter manually",
                color = ScanColors.inkMute,
            )
        }
    }
}

/**
 * The inline manual fallback (owner addition 1). Both transports, because at this
 * point the app does not know which one the operator has: a tap-to-connect list of
 * attached serial ports, and the two addresses for the Mid-360.
 *
 * A panel on the screen, never a dialog (item 7). Its own height is capped and it
 * scrolls, so a rig with six serial devices cannot push the live viewport off the
 * bottom of the screen.
 *
 * ## ROUND 25 item 119 — the sensor row
 *
 * The header used to read "COIN-D6 · USB" and every Connect button meant
 * "connect a COIN-D6". With two lidars on the same cable, the same connector
 * and the same driver class, the port itself no longer says which box is on the
 * end of it — only the person holding it does. So the panel asks, once, with a
 * two-option [SegmentedPill], and Connect acts on the answer.
 *
 * This is also the intended escape hatch from auto-detect's one honest weakness
 * (`SerialLidarAutoDetector`'s class doc): a probe that guesses wrong is
 * overridden here in one tap, without a wizard and without a reconnect dance.
 *
 * The choice is **local to the panel** rather than ViewModel state. It is a
 * question about the cable in front of the operator right now, it is answered
 * and consumed in the same gesture, and hoisting it would put a field into the
 * capture session that outlives the reason it was asked. `rememberSaveable` is
 * enough — it survives the rotation, which is the only lifecycle event that
 * happens between picking and tapping.
 */
@Composable
private fun ManualEntryPanel(
    devices: List<ManualSerialDevice>,
    lidarIp: String,
    hostIp: String,
    busy: Boolean,
    onDeviceConnect: (ManualSerialDevice, SensorType) -> Unit,
    onLidarIpChange: (String) -> Unit,
    onHostIpChange: (String) -> Unit,
    onMid360Connect: () -> Unit,
    /**
     * ROUND 27 item 129 — **the THIRD nested scroller.**
     *
     * `PreCaptureStrip` had one and the hint band had one; this panel had a
     * `heightIn(max = 260.dp).verticalScroll(...)` of its own, and it is the
     * one that actually cut the AVD's screenshot in half — "Connect Mid-360"
     * sheared through the middle of its letters at exactly 260 dp, with no way
     * to reach the rest, because a drag inside the panel was consumed by the
     * panel and a drag on the column was consumed by the panel too.
     *
     * Three scrollers on one axis inside one another is not a layout, it is
     * three layouts arguing. False means "my parent is a bounded scroller and
     * owns the gesture", which is every fullscreen-Scan-tab call site; the
     * Capture sheet, which hosts this panel inside a sheet of its own, keeps
     * the cap.
     */
    ownScroll: Boolean = true,
) {
    var manualSerialSensor by rememberSaveable { mutableStateOf(SensorType.COIN_D6) }
    val shape = RoundedCornerShape(ScanDims.TileRadius)
    Column(
        Modifier
            .fillMaxWidth()
            .then(
                if (ownScroll) {
                    Modifier.heightIn(max = 260.dp)
                } else {
                    Modifier
                },
            )
            .background(MaterialTheme.colorScheme.surfaceContainer, shape)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
            .then(if (ownScroll) Modifier.verticalScroll(rememberScrollState()) else Modifier)
            .padding(12.dp)
            .testTag("manualEntryPanel"),
    ) {
        // ROUND 29 item 170 (review finding G6): a passive section label is
        // not an action, and the owner's 0.9.13 connect flow spent two of its
        // oranges on these two words and two more on the links below. §C's
        // accent law: at most two oranges per screen, and they are the primary
        // action and the active tab.
        Text("USB scanner", style = ScanMetaCaps, color = ScanColors.inkMute)
        Spacer(Modifier.height(6.dp))
        // The two serial lidars, by the same badge labels the Projects cards
        // use — no new vocabulary for the operator to learn.
        SegmentedPill(
            options = listOf(
                SensorType.COIN_D6 to SensorType.COIN_D6.badgeLabel,
                SensorType.STL27L to SensorType.STL27L.badgeLabel,
            ),
            selected = manualSerialSensor,
            onSelect = { manualSerialSensor = it },
            enabled = !busy,
            modifier = Modifier.testTag("manualSensorSelector"),
        )
        Spacer(Modifier.height(4.dp))
        Hint(com.lidarscan.core.Wording.MANUAL_SERIAL_PICK, color = ScanColors.inkFaint)
        Spacer(Modifier.height(6.dp))
        if (devices.isEmpty()) {
            Hint(com.lidarscan.core.Wording.NO_USB_DEVICE + "\n" + com.lidarscan.core.Wording.NO_USB_DEVICE_DETAIL)
        } else {
            devices.forEach { device ->
                Row(
                    Modifier.fillMaxWidth().padding(vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        device.label,
                        style = ScanMeta,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    Spacer(Modifier.width(8.dp))
                    TextButton(
                        enabled = !busy,
                        onClick = { onDeviceConnect(device, manualSerialSensor) },
                        modifier = Modifier.testTag("manualConnectDevice"),
                    ) { Text("Connect") }
                }
            }
        }

        Spacer(Modifier.height(10.dp))
        Text("LIVOX MID-360 · ETHERNET", style = ScanMetaCaps, color = ScanColors.inkMute)
        Spacer(Modifier.height(6.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                value = lidarIp,
                onValueChange = onLidarIpChange,
                singleLine = true,
                label = { Text("Lidar IP") },
                modifier = Modifier.weight(1f).testTag("manualLidarIpField"),
            )
            OutlinedTextField(
                value = hostIp,
                onValueChange = onHostIpChange,
                singleLine = true,
                label = { Text("This phone") },
                modifier = Modifier.weight(1f).testTag("manualHostIpField"),
            )
        }
        Spacer(Modifier.height(8.dp))
        SecondaryPill(
            text = "Connect Mid-360",
            height = 46.dp,
            enabled = !busy,
            onClick = onMid360Connect,
            modifier = Modifier.fillMaxWidth().testTag("manualConnectMid360"),
        )
        Spacer(Modifier.height(2.dp))
        Hint(
            com.lidarscan.core.Wording.LIVE_VIEW_IS_THE_PROOF,
            color = ScanColors.inkFaint,
        )
    }
}

// ── viewport ────────────────────────────────────────────────────────────────

@Composable
private fun CaptureViewport(
    /**
     * ROUND 26 item 124 — edge-to-edge: no rounded corner, no hairline, no
     * inset. True on the Scan tab, where this composable IS the screen.
     */
    fullBleed: Boolean = false,
    /**
     * ROUND 26 item 124 — where the viewport's OWN chips may draw.
     *
     * The viewport has always carried four status chips in its four corners
     * (keyframes, pose, stream, health). Full bleed put the screen's floating
     * controls in the same four corners, and the first AVD recording printed
     * "BUILDING MAP…" straight through the map-mode chip and the health chip
     * through the `?`. The chips are not moved and none is removed — they are
     * simply told which band of the picture belongs to the chrome. Default is
     * the 12 dp the framed viewport used, so the calibration wizard's inset
     * card is unaffected.
     */
    chipInsets: androidx.compose.foundation.layout.PaddingValues =
        androidx.compose.foundation.layout.PaddingValues(12.dp),
    /**
     * ROUND 27 item 129(a) — true when the VIEWPORT owns the stream and health
     * read-outs (the calibration wizard's inset card, which has no chrome of
     * its own to put them in), false on the fullscreen Scan tab, where the
     * screen draws both in real corners. Never "draw them twice": the health
     * chip's `captureHealthChip` tag is asserted by three suites and two nodes
     * with that tag is an ambiguous selector, not a duplicated affordance.
     */
    ownsStatusChips: Boolean = true,
    /** ROUND 27 item 129(a) — reports the polled stream state to whoever draws the chip. */
    onMappedPageSeen: (Boolean) -> Unit = {},
    modifier: Modifier,
    connected: Boolean,
    isReplaySession: Boolean,
    source: PointCloudSource?,
    liveView: Boolean,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    displayParams: com.lidarscan.core.render.DisplayParams,
    refreshHz: Int,
    refreshRequestToken: Int,
    cameraMode: CameraMode,
    liveSlam: Boolean,
    /** ROUND 6 (item 21): a registered/pushbroom map is actually being produced AND the preset draws it. */
    liveMapRequested: Boolean,
    /** ROUND 6 (item 22, Light): false means "raw preview only" by operator choice, not by absence of a map. */
    liveMapEnabled: Boolean,
    recording: Boolean,
    sensor: SensorType,
    arAvailable: Boolean,
    /**
     * ROUND 23 item 101: true when this screen has asked for (and is holding)
     * an ARCore session — `CaptureRoute`'s `needsArSession`. The pose pump is
     * composed only when it is, so the pump can never spin against a session
     * that does not exist.
     */
    arSessionWanted: Boolean,
    arTrackingHint: String?,
    arOverlay: @Composable (Modifier) -> Unit,
    arPosePump: @Composable (Modifier) -> Unit,
    poseTrackingRequired: Boolean,
    poseState: PoseTrackingState,
    health: DeviceHealth?,
    keyframesEnabled: Boolean,
    keyframesWritten: Int,
    trailPoints: List<com.lidarscan.core.capture.TrajectoryTrail.NormalizedPoint>,
    trailLengthM: Float,
    /** ROUND 19 item 75: null (or all-zero: unmeasured) hides the ring. */
    coverageSectors: FloatArray? = null,
    onRefreshAutoDownshift: (Int) -> Unit,
    onOpenDiagnostics: () -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
    /**
     * ROUND 8: the live renderer, or null once it is gone. The FOLLOW camera
     * needs the rig's ARCore position once per frame and only `CaptureViewModel`
     * has the frame stream, so the renderer has to travel UP out of here — see
     * `CaptureViewModel.setRigPoseSink`.
     */
    onRendererChanged: (com.lidarscan.app.render.PointCloudRenderer?) -> Unit = {},
) {
    // ROUND 26 item 124: NO CARD FRAME. The viewport is the screen, and a
    // screen does not have a rounded corner and a hairline drawn inside its own
    // edge — that is a card, and a card is what the owner asked to be rid of.
    // `fullBleed` is a parameter rather than an unconditional change because
    // the mount-calibration wizard hosts the same composable inside a real
    // card, where the frame is correct.
    val shape = if (fullBleed) RectangleShape else RoundedCornerShape(ScanDims.CardRadius)

    // ROUND 5 AUDIT bugfix: the bottom-left "what stream is on screen" chip
    // used to read `liveSlam` alone (the requested mode) — see
    // `PointCloudRenderStats.hasSeenMappedPage`'s doc for why that mislabels
    // the whole stretch `StreamFilter.MAPPED_ONLY` spends falling back to raw
    // pages before the first registered/pushbroom-resolved page exists. This
    // polls the renderer's own stats at a cheap, UI-appropriate cadence (not
    // once per frame) so the chip only ever claims "Live map" once that is
    // actually true.
    var pointCloudRenderer by remember { mutableStateOf<com.lidarscan.app.render.PointCloudRenderer?>(null) }
    var hasSeenMappedPage by remember { mutableStateOf(false) }
    // ROUND 27 item 129(a): the SCREEN draws the stream chip now (in the
    // bottom-start corner, beside the map-mode chip, out of the landscape
    // connect rail). The poll stays here, because the renderer it polls is
    // here; only the fact travels.
    LaunchedEffect(hasSeenMappedPage) { onMappedPageSeen(hasSeenMappedPage) }
    // ROUND 5 AUDIT bugfix (task 2, multi-cycle recording): the same viewport
    // (and the same underlying PointCloudRenderer) survives a Stop -> Start
    // within one connect session, so this must re-arm for the SECOND
    // recording rather than staying stuck on the first one's "seen a mapped
    // page" the moment `recording` flips true again — see
    // `CaptureViewModel.stopCapture`'s own multi-cycle fix, which this pairs
    // with (a fresh session, fresh chip state).
    LaunchedEffect(recording) {
        if (!recording) return@LaunchedEffect
        hasSeenMappedPage = false
        // Poll rather than a single check: the renderer callback
        // (`onRendererReady`) and the first mapped page can both land after
        // this effect starts.
        while (!hasSeenMappedPage) {
            if (pointCloudRenderer?.stats()?.hasSeenMappedPage == true) {
                hasSeenMappedPage = true
            } else {
                kotlinx.coroutines.delay(300)
            }
        }
    }

    Box(
        modifier
            .background(ScanColors.viewport, shape)
            .then(
                if (fullBleed) Modifier
                else Modifier.border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape),
            )
            .clip(shape)
            .testTag("captureViewport"),
    ) {
        // B7 (§3.7): AR mode is a different RENDERER, not a flag inside one —
        // the AR path stacks a translucent Filament surface over an ARCore
        // camera GLSurfaceView, and the free-orbit path is B4's single opaque
        // surface unchanged.
        //
        // ── ROUND 7, OWNER DIRECTIVE: ARCHIVED ──────────────────────────────
        //
        // "The AR function archive not only from UI, work it like the 3d lidar."
        //
        // The phone + COIN-D6 is a 3D lidar, not an AR app that happens to have
        // a lidar on the back: connect, watch the map build in the viewport,
        // record, get a project — the same shape as a Mid-360, with no AR
        // vocabulary and no camera-passthrough view anywhere in it. So this
        // branch is switched off at [AR_OVERLAY_ARCHIVED] rather than deleted:
        // `ArOverlayView`, `ArSessionGate` and the ROUND 6 crash fixes are all
        // still compiled and still used by the mount-calibration wizard (which
        // genuinely needs to see the board through the camera), and reviving
        // the capture overlay is this one constant plus the `CameraMode.AR`
        // entry in `CaptureSheets`' View row.
        //
        // ARCore itself is untouched and must stay that way: it is the entire
        // third dimension of a D6 scan. `ArPosePumpView` still drives
        // `Session.update()` on every frame, every pose still reaches
        // `scan_engine_push_pose`, and camera keyframes still record for
        // colorization — none of that is an AR *feature*, it is the scanner's
        // tracking and the scanner's colour.
        if (!AR_OVERLAY_ARCHIVED && cameraMode == CameraMode.AR && arAvailable) {
            arOverlay(Modifier.fillMaxSize())
        } else if (connected && source != null) {
            PointCloudView(
                source = source,
                colorMode = colorMode,
                colormap = colormap,
                pointSizePx = pointSizePx,
                cameraMode = cameraMode,
                // B3: Record-only draws the sensor-frame stream; Live-SLAM
                // draws the registered map.
                // ROUND 6 (owner item 21): `StreamFilter.forSession(liveSlam)`
                // was the bug. On the Capture tab `liveSlam` is false until
                // somebody opens the sheet and toggles it (the manifest's own
                // default is only read on the project-scoped route), so a D6
                // session ran RAW_ONLY — which by construction rejects
                // `SCAN_STREAM_SLAM_MAP`, the stream A8's pushbroom publishes
                // its world-frame cloud on. The viewport drew the sensor-frame
                // fan and NEVER the registered cloud: "bearly maping… the point
                // are not really aligned", exactly.
                streamFilter = StreamFilter.forSession(liveMapRequested),
                // Redesign: the sheet's LOD slider needs a live path into the
                // renderer, and lodPointBudget only travels inside
                // DisplayParams. Passing the block also owns colour and point
                // size, which is why they are assembled together in the VM.
                displayParams = displayParams,
                // ROUND 5 (item 10): the operator's own refresh cap.
                maxRefreshHz = refreshHz,
                // ROUND 5 AUDIT bugfix: the "operator explicitly asked" signal
                // that lets a re-pick of the same option clear a downshift.
                refreshRequestToken = refreshRequestToken,
                // ROUND 5.3 (item 17): the ceiling is the panel's own, and the
                // renderer eases down from it rather than hitching.
                onRefreshDownshift = onRefreshAutoDownshift,
                // ROUND 5 AUDIT bugfix: lets the "what stream is on screen"
                // chip poll whether a mapped/pushbroom page has actually
                // landed yet, instead of just parroting the requested mode.
                onRendererReady = {
                    pointCloudRenderer = it
                    onRendererChanged(it)
                },
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            // ROUND 26 item 124: the placeholder is SUPPRESSED under the
            // floating connect flow.
            //
            // Fullscreen put the connect card in the middle of the viewport,
            // which is where this sentence was already centred — so a
            // disconnected screen drew "Connect a sensor to see the live 3D
            // view" straight through the panel that says "No scanner found.
            // Plug it in, then Retry." and offers the Retry button. Two
            // sentences saying the same thing, overlapping. The panel wins: it
            // is the one with a control on it. Every OTHER placeholder state
            // still draws, because none of them has a card in front of it.
            val suppressedByConnectCard = !connected && !isReplaySession && liveView
            if (!suppressedByConnectCard) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        when {
                            !liveView -> "Live view is off — the recording is unaffected"
                            connected -> "3D view needs the real engine (simulated-engine build)"
                            else -> "Starting the replay engine…"
                        },
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(28.dp),
                    )
                }
            }
        }

        // ROUND 5.3 (item 18): the trail — where this walk has already been,
        // drawn over whatever renderer is underneath. Top-down, aspect-preserved,
        // and dimmed where tracking was poor, so a stretch the engine will exclude
        // is visibly a stretch you should walk again.
        if (trailPoints.size >= 2) {
            TrajectoryTrailOverlay(
                points = trailPoints,
                lengthM = trailLengthM,
                coverageSectors = coverageSectors,
                // ROUND 26 item 124: the coverage arcs draw over the FULL
                // viewport, but 10 dp off the bottom edge is now exactly where
                // the SCAN button is. `chipInsets` is the chrome's own two
                // bands, so the mini-map lands just above the control cluster —
                // still bottom-centre, still the first thing under the thumb's
                // eyeline, and no longer under the thumb itself.
                // ROUND 27 item 129(a): the BOTTOM inset only. `chipInsets`
                // is asymmetric in landscape (the start side is the connect
                // rail, the end side is the control rail) and a bottom-CENTRE
                // overlay padded by both is shoved sideways into the cluster.
                modifier = Modifier.align(Alignment.BottomCenter)
                    .padding(bottom = chipInsets.calculateBottomPadding())
                    .size(width = 108.dp, height = 84.dp),
            )
        }

        // ROUND 5 (item 11): the pose pump, when poses are needed and the AR
        // overlay (which pumps ARCore itself) is not the renderer on screen.
        // Two pumps would call Session.update() from two threads, so this is an
        // either/or by construction.
        //
        // ── ROUND 23 item 101: …and only when there is a session to pump ────
        //
        // The owner's 12:00:52 line — `gate refused NO_SESSION asked=POSE_PUMP#2
        // owner=POSE_PUMP#2 created=false resumed=false (+59 more since the last
        // line)` — is a `GLSurfaceView` at `RENDERMODE_CONTINUOUSLY` calling
        // `Session.update()` sixty times a second against a session that does
        // not exist. The claim is correct, the owner is correct, and there is
        // simply nothing there: the screen creates the ARCore session only when
        // [arSessionWanted] (the same predicate `CaptureRoute` uses), and until
        // then the pump was spinning a real GL thread and a real camera-shaped
        // surface for nothing but log noise and battery.
        //
        // Composing the pump on the same predicate makes the two agree by
        // construction. The rate limiter stays — a few refusals still happen in
        // the window between this view's `factory` and `createSession()`
        // returning, and that window is exactly what it was written for.
        if (poseTrackingRequired && arAvailable && arSessionWanted && cameraMode != CameraMode.AR) {
            arPosePump(Modifier.align(Alignment.BottomStart).padding(start = 2.dp, bottom = 2.dp))
        }

        // ── top-left: the keyframe counter ──────────────────────────────
        //
        // It rides the TOP band deliberately (round 3's one documented
        // departure): the Capture-settings sheet covers the lower ~74 %, so a
        // bottom-corner chip would be hidden by the very sheet whose switch
        // controls it.
        // ROUND 10 (owner item 39): nothing writes keyframes with colorization
        // paused, so the counter would sit at 0 for every capture.
        if (recording && keyframesEnabled && com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED) {
            ScanChip(
                text = "KF $keyframesWritten",
                modifier = Modifier.align(Alignment.TopStart).padding(chipInsets).testTag("keyframeChip"),
            )
        }

        // ── top-right: orbit / follow ───────────────────────────────────
        //
        // ROUND 10 (owner item 39): with FOLLOW paused there is one camera
        // mode, and a one-option radio group is not a control — it is chrome
        // over the live map. The whole pill goes. `FeatureFlags
        // .FOLLOW_CAMERA_ENABLED` brings it back; note it is a SECOND control
        // for the same state as the Display sheet's View row, which is why
        // both had to be found.
        if (com.lidarscan.core.FeatureFlags.FOLLOW_CAMERA_ENABLED) {
        Row(
            Modifier
                .align(Alignment.TopEnd)
                .padding(12.dp)
                .background(MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.9f), RoundedCornerShape(50))
                .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(50))
                .padding(3.dp),
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            listOf(CameraMode.ORBIT to "Orbit", CameraMode.FOLLOW to "Follow").forEach { (mode, label) ->
                val selected = cameraMode == mode
                Box(
                    Modifier
                        .background(if (selected) Ember else Color.Transparent, RoundedCornerShape(50))
                        .clickable(role = Role.RadioButton) { onCameraModeChange(mode) }
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                ) {
                    Text(
                        label,
                        style = ScanMetaCaps,
                        color = if (selected) OnEmber else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        }

        // ── tracking quality, inline, because 3D quality depends on it ──
        //
        // ROUND 5 item 11: for a phone-tracked D6 this is not a diagnostic, it is
        // the state of the third dimension, so it rides the viewport during a
        // recording rather than living only in the diagnostics sheet.
        // Only once something is streaming: a tracking state on a viewport with no
        // sensor behind it is a status about a scan that is not happening. Short
        // labels, because this chip shares the top band with the camera control.
        // ROUND 27 item 138: the tracking chip that used to ride here is in the
        // status pill now — one fact, one owner. What stays is the AR hint,
        // which is a different sentence about a different thing (the camera's
        // own advice: "point at a textured surface") and is only ever shown in
        // AR mode.
        if (cameraMode == CameraMode.AR && arTrackingHint != null) {
            ScanChip(
                text = arTrackingHint,
                color = ScanColors.warn,
                modifier = Modifier.align(Alignment.TopCenter).padding(chipInsets).padding(top = 40.dp),
            )
        }

        // ── bottom-left: what stream is on screen ───────────────────────
        //
        // ROUND 5 AUDIT bugfix: `liveSlam` alone used to drive this label —
        // true as soon as the OPERATOR asked for live SLAM/pushbroom, even
        // while `StreamFilter.MAPPED_ONLY` was still falling back to raw
        // pages because no mapped page had resolved yet. See
        // `PointCloudRenderStats.hasSeenMappedPage`'s doc.
        // ROUND 6: the same three states, but keyed off what is actually on
        // screen rather than off `liveSlam` (a Mid-360 concept that is false for
        // every D6 session on this tab). "RAW" now also covers the Light
        // preset's deliberate raw-only view, which is a choice rather than a
        // missing map — and it says so, so nobody reads it as a failure.
        if (ownsStatusChips) {
            StreamModeChip(
                liveMapEnabled = liveMapEnabled,
                liveMapRequested = liveMapRequested,
                hasSeenMappedPage = hasSeenMappedPage,
                modifier = Modifier.align(Alignment.BottomStart).padding(chipInsets)
                    .padding(start = 6.dp),
            )
        }

        // ── bottom-right: the health chip, and the door to Diagnostics ──
        //
        // The chip's own ink is chip-sized; the 44 dp minimum comes from the
        // Box it sits in, so the target grows without the chip inflating.
        if (ownsStatusChips) {
            CaptureHealthChip(
                health = health,
                onOpenDiagnostics = onOpenDiagnostics,
                modifier = Modifier.align(Alignment.BottomEnd).padding(chipInsets),
            )
        }

        // ── ROUND 23 item 102: THE SECOND ADVANCED BUTTON IS GONE ───────────
        //
        // The owner, verbatim: *"there are 2 advance button in the scan."* He
        // is describing this one and the round-22 `advancedButton` in the
        // transport row: same `Icons.Filled.Tune`, same 1 dp hairline circle,
        // same `CaptureSheet.SETTINGS` behind both. Round 22 added the
        // labelled door beside the scan button and simply left this one
        // floating on the viewport's right edge, so the "gather everything
        // behind one tap" item shipped as two taps that do the same thing.
        //
        // The round-22 placement is the one that stays — it is beside the
        // control the operator is already looking at, it is 52 dp rather than
        // 48, and it is the one the brief named. The parameter that fed this
        // button is removed with it rather than left dangling: an unused door
        // in a signature is how a duplicate comes back.
    }
}

/**
 * ROUND 5.3 (item 18): the walked path, as a small top-down inset on the
 * viewport.
 *
 * A Compose `Canvas`, not a Filament overlay: the trail is 2D, it changes at ~5 Hz
 * (one point per 15 cm of walk), and drawing it in the 3D scene would mean a
 * second material, a second geometry upload path and a per-frame rebuild of a line
 * strip — for a picture that is intentionally *not* in the cloud's own frame. The
 * inset is deliberately small and bottom-centre: a walkthrough operator glances at
 * it, they do not study it.
 *
 * Points where ARCore was not tracking are drawn faint — those are the stretches
 * whose lidar returns the pushbroom flags and excludes by default (Tech Spec §3.3),
 * so "faint on the trail" and "missing from the cloud" are the same thing.
 */
@Composable
private fun TrajectoryTrailOverlay(
    points: List<com.lidarscan.core.capture.TrajectoryTrail.NormalizedPoint>,
    lengthM: Float,
    /**
     * ROUND 19 item 75 — per-sector coverage around the operator (12 world
     * azimuth sectors, 0..1 against the sector mean; see CoverageCompass).
     * Thin sectors draw as amber edge arcs POINTING at the uncovered walls —
     * the tile is already a top-down world map, so a world azimuth is a
     * canvas angle with no conversion (x right, z down, same as the path).
     * All-zero means "not enough evidence yet" and draws nothing: the ring
     * only ever says something it measured. Amber, not red — round 11's
     * language for "thin", kept.
     */
    coverageSectors: FloatArray? = null,
    modifier: Modifier = Modifier,
) {
    val shape = RoundedCornerShape(ScanDims.TileRadius)
    // ROUND 28 item 144: a `DrawScope` lambda is not a composition, so the
    // theme has to be read out here and closed over. Hoisting rather than
    // reaching for a constant is the whole point — the trail is amber and red
    // in the dark theme and a DIFFERENT amber and red in the light one.
    val badInk = ScanColors.bad
    val trackedInk = ScanColors.sensorD6
    val hereInk = ScanColors.primary
    val thinInk = ScanColors.coverageAmber
    Box(
        modifier
            .background(MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.72f), shape)
            .border(1.dp, MaterialTheme.colorScheme.outline, shape)
            .testTag("trajectoryTrail"),
    ) {
        androidx.compose.foundation.Canvas(Modifier.fillMaxSize().padding(6.dp)) {
            val w = size.width
            val h = size.height
            var previous: androidx.compose.ui.geometry.Offset? = null
            var previousTracking = true
            points.forEach { p ->
                val here = androidx.compose.ui.geometry.Offset(p.x * w, p.y * h)
                previous?.let { from ->
                    if (p.jump) {
                        // ROUND 18 item 70: the tracker was blind or teleported
                        // across this segment — a dashed red bridge, so the
                        // tile stops drawing a walk that never happened. Same
                        // verdict the 3D ribbon and the floor-plan sheet draw.
                        drawLine(
                            color = badInk.copy(alpha = 0.7f),
                            start = from,
                            end = here,
                            strokeWidth = 2.5f,
                            cap = androidx.compose.ui.graphics.StrokeCap.Round,
                            pathEffect = androidx.compose.ui.graphics.PathEffect
                                .dashPathEffect(floatArrayOf(6f, 6f)),
                        )
                    } else {
                        drawLine(
                            color = if (previousTracking && p.tracking) trackedInk else badInk.copy(alpha = 0.5f),
                            start = from,
                            end = here,
                            strokeWidth = 3f,
                            cap = androidx.compose.ui.graphics.StrokeCap.Round,
                        )
                    }
                }
                previous = here
                previousTracking = p.tracking
            }
            // Where you are NOW — the one thing worth finding at a glance.
            previous?.let { drawCircle(color = hereInk, radius = 4.5f, center = it) }

            // ROUND 19 item 75: the guidance ring. Amber arcs at the tile's
            // edge where the map around the walked path is thin.
            if (coverageSectors != null && coverageSectors.any { it > 0f }) {
                val sectors = coverageSectors.size
                val sweep = 360f / sectors
                val arcInset = 2.5f
                val arcSize = androidx.compose.ui.geometry.Size(w - 2 * arcInset, h - 2 * arcInset)
                for (s in 0 until sectors) {
                    if (coverageSectors[s] >= com.lidarscan.core.render.CoverageCompass.DEFAULT_THIN_FRACTION) continue
                    drawArc(
                        color = thinInk.copy(alpha = 0.9f),
                        startAngle = s * sweep + 1.5f,
                        sweepAngle = sweep - 3f,
                        useCenter = false,
                        topLeft = androidx.compose.ui.geometry.Offset(arcInset, arcInset),
                        size = arcSize,
                        style = androidx.compose.ui.graphics.drawscope.Stroke(
                            width = 3.5f,
                            cap = androidx.compose.ui.graphics.StrokeCap.Round,
                        ),
                    )
                }
            }
        }
        Text(
            "%.0f m".format(lengthM),
            style = ScanMetaCaps,
            color = ScanColors.inkFaint,
            modifier = Modifier.align(Alignment.TopEnd).padding(4.dp),
        )
    }
}

// ── transport ───────────────────────────────────────────────────────────────

/**
 * ROUND 5: **Live** switch on the left (viewport streaming, default on — item 10),
 * pause circle and the 64 dp ember record button on the right. Live SLAM moved
 * into the settings sheet with the rest of the session configuration.
 *
 * The record button's `contentDescription` is what names the action for
 * accessibility *and* for the emulator smoke test — the button itself is a
 * circle with no text, exactly as designed.
 */
/**
 * ROUND 26 item 124 — **the one dominant control, floating.**
 *
 * Round 22's `TransportRow` put five controls in a horizontal band 80 dp tall
 * across the bottom of the screen. The band is gone; the controls are not.
 * What was a Row is now three overlays — this cluster, [ScanStatusPill] and
 * [ScanGearButton] — placed independently by the fullscreen layout, which is
 * what lets landscape move the cluster to the end edge without moving the
 * status read-out with it.
 *
 * Everything the operator's hand knows is preserved deliberately: the ember
 * circle, its shadow, the round-5.3 grow-while-live sizes, the round-21 rule
 * that the button stays TAPPABLE during a start, the round-23 rule that a
 * press which cannot start still ANSWERS, and every test tag. An operator who
 * has used 0.9.10 must not have to learn this screen again — item 124 is a
 * relayout, not a new transport.
 *
 * [vertical] is the landscape form: the same three controls stacked up the end
 * edge instead of across the bottom, so the thumb of the hand that is holding
 * the phone reaches the FAB in either orientation (item 125(a)).
 */
@Composable
internal fun ScanControlCluster(
    vertical: Boolean,
    captureState: CaptureState,
    connected: Boolean,
    liveView: Boolean,
    isReplaySession: Boolean,
    pauseSupported: Boolean,
    /**
     * ROUND 17 item 64 — true from the press until the capture is recording or
     * the attempt has failed. The ROUND 12 tracking gate can hold Start for
     * four to eight seconds and, until that round, NOTHING on screen changed
     * for the whole of it. A control that is working has to look like it.
     */
    starting: Boolean = false,
    /**
     * ROUND 23 item 101(b) — non-null when a press cannot start anything, in
     * the six-word sentence the log line will carry. The button stays
     * TAPPABLE: an inert control is what three rounds of "the scan button is
     * dead" were made of.
     */
    startBlockedReason: String? = null,
    onStartRefused: (String) -> Unit = {},
    onLiveViewChange: (Boolean) -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
    /** ROUND 28 item 168 + ROUND 30 item 175: the live attitude, for the instrument. */
    attitude: kotlinx.coroutines.flow.StateFlow<com.lidarscan.core.calib.HoldOrientation?> =
        kotlinx.coroutines.flow.MutableStateFlow(null),
) {
    val recording = captureState == CaptureState.RECORDING
    val paused = captureState == CaptureState.PAUSED
    val live = recording || paused
    val stopping = captureState == CaptureState.STOPPING

    val secondaries: @Composable () -> Unit = {
        // ── ROUND 28 items 158 + 159: PAUSE IS A RECORDING CONTROL ──────────
        //
        // It used to be drawn idle as well, dimmed — the review's finding S7,
        // "three circles, three sizes, one row", where the 72 dp grey pause was
        // *indistinguishable from disabled* and meaningless with nothing
        // recording. Keeping the cluster's shape stable across sensors was the
        // reason it was present-and-dimmed, and that reason still holds
        // BETWEEN SENSORS while recording; it never justified a control on a
        // page where the thing it controls does not exist.
        //
        // §D.1 counts one FAB on the idle page and §D.2 counts two controls on
        // the recording one. Absent idle, present (and dimmed on a Mid-360 or a
        // replay, which genuinely cannot pause) while live.
        // Pause: offered only where it actually works. Replay has no pause hook
        // in ReplaySource (B4) and a Mid-360 cannot pause without truncating the
        // recording on resume (B2/B3) — so it is present and dimmed rather than
        // absent, which keeps the cluster's shape stable across sensors.
        val pauseEnabled = live && pauseSupported && !stopping
        SecondaryRoundButton(
            size = if (live) 56.dp else 48.dp,
            enabled = pauseEnabled,
            contentDescription = if (paused) "Resume recording" else "Pause recording",
            testTag = "pauseButton",
            onClick = { if (recording) onPause() else onResume() },
        ) {
            Icon(
                if (paused) Icons.Filled.PlayArrow else Icons.Filled.Pause,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }
    }

    @Suppress("UNUSED_VARIABLE")
    val liveToggle: @Composable () -> Unit = {
        // ROUND 26 item 124: the Live-view SWITCH became a round eye button,
        // and the honesty it carried moved rather than being dropped. "display
        // only · recording unaffected" was a caption beside a switch on a band
        // that no longer exists; the fact now lives in [ScanStatusPill], which
        // says LIVE VIEW OFF · STILL RECORDING in the one place the operator is
        // already looking. The tag is unchanged because the emulator suite
        // asserts on it, and the control is still editable during a recording —
        // that is the whole of item 10.
        SecondaryRoundButton(
            size = if (live) 56.dp else 48.dp,
            enabled = true,
            contentDescription = if (liveView) "Live view on, display only" else "Live view off, still recording",
            testTag = "liveViewSwitch",
            onClick = { onLiveViewChange(!liveView) },
        ) {
            Icon(
                if (liveView) Icons.Filled.Visibility else Icons.Filled.VisibilityOff,
                contentDescription = null,
                tint = if (liveView) Ember else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }

    val fab: @Composable () -> Unit = {
        ScanFab(
            live = live,
            starting = starting,
            stopping = stopping,
            connected = connected,
            isReplaySession = isReplaySession,
            startBlockedReason = startBlockedReason,
            onStartRefused = onStartRefused,
            onStart = onStart,
            onStop = onStop,
        )
    }

    // ── ROUND 28 items 159 + 168: what is beside STOP while walking ────────
    //
    // §D.2 cuts the recording row to **two** controls, and the owner's mockup
    // review puts a third thing in the third slot: the attitude instrument,
    // mirrored against the pause button either side of the FAB. It is not a
    // control — it cannot be pressed — which is exactly why it belongs there:
    // the one fact the operator cannot see while holding a rig at arm's length
    // is whether he is holding it square, and the seal grade only tells him
    // afterwards, when nothing can be done.
    //
    // The live-view eye moves to the Advanced sheet (§D.1's table): it is a
    // display setting, it is pressed once a session if ever, and it was one of
    // three grey circles in two sizes that the walking operator had to tell
    // apart.
    //
    // ROUND 33 item 179(c): and what sits in it is now the two-axis bubble. At
    // arm's length, mid-walk, a dot moving in a circle is the only thing that
    // survives — the ghost's hint and its notch are for the card, where the
    // operator is standing still and looking at the phone.
    val attitudeSlot: @Composable () -> Unit = {
        PostureBubbleSlot(attitude = attitude)
    }
    if (vertical) {
        Column(
            verticalArrangement = Arrangement.spacedBy(ScanDims.S3),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            if (live) attitudeSlot()
            fab()
            if (live) secondaries()
        }
    } else {
        Row(
            horizontalArrangement = Arrangement.spacedBy(ScanDims.S6),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (live) secondaries()
            fab()
            if (live) attitudeSlot()
        }
    }
}

/**
 * The secondary control shape — the circle, the ground and the hairline the
 * pause button has had since round 5.3. One composable rather than three
 * copies, because item 124 needs the same shape in three corners and a fourth
 * copy is how the gear and the pause button drift apart.
 */
@Composable
private fun SecondaryRoundButton(
    size: androidx.compose.ui.unit.Dp,
    enabled: Boolean,
    contentDescription: String,
    testTag: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Box(
        modifier
            .size(size)
            .alpha(if (enabled) 1f else 0.3f)
            .background(MaterialTheme.colorScheme.surfaceContainerHigh.copy(alpha = 0.92f), CircleShape)
            .border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { this.contentDescription = contentDescription }
            .testTag(testTag),
        contentAlignment = Alignment.Center,
    ) { content() }
}

/**
 * ROUND 22 item 95's dominant control, unchanged in behaviour and moved to the
 * middle of the bottom edge — where a camera app's shutter is, and where a
 * thumb lands without looking.
 */
@Composable
private fun ScanFab(
    live: Boolean,
    starting: Boolean,
    stopping: Boolean,
    connected: Boolean,
    isReplaySession: Boolean,
    startBlockedReason: String?,
    onStartRefused: (String) -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    // ROUND 5 item 9: Start creates a NEW project, and the label says so.
    // ROUND 22 item 95: the button's own word, on the button — SCAN while
    // idle, STOP while recording, CANCEL during the start sequence.
    val recordWord = when {
        live -> com.lidarscan.core.Wording.SCAN_BUTTON_RECORDING
        starting -> com.lidarscan.core.Wording.SCAN_BUTTON_STARTING
        else -> com.lidarscan.core.Wording.SCAN_BUTTON
    }
    val recordLabel = when {
        live -> "Stop recording"
        starting -> "Cancel this start"
        isReplaySession -> "Start replay"
        else -> "Start a scan"
    }
    // ROUND 17 item 64: armed = the press will start something new. ROUND 21
    // item 85: the button stays TAPPABLE while a start is in flight — the
    // press still cannot start anything, but it PULSES the start card instead
    // of vanishing into an inert control.
    val armed = connected && !stopping && !starting

    // §C.4's one disabled state, everywhere: ink-faint on trough. The armed
    // button keeps `OnEmber`, which is the only ink that is legible on the
    // brand orange in either theme.
    val fabInk = if (armed || live) OnEmber else ScanColors.inkFaint

    // ── ROUND 27 item 132: A REFUSED PRESS IS ANSWERED, VISIBLY ────────────
    //
    // Round 23 item 101(b) made the button never inert and never silent: a
    // press that cannot start anything still calls back, still logs, and still
    // recolours the standing reason from grey to amber. On the AVD that turned
    // out to be a four-word line, in 11 sp mono, six hundred pixels away from
    // the thumb that just pressed — the owner pressed SCAN and, as far as the
    // screen was concerned, nothing happened.
    //
    // What is added is deliberately NOT a dialog and NOT a toast: this screen's
    // rule since round 5 is that a modal is the worst interruption on it, and a
    // refusal is not an error worth a modal. The answer comes from the control
    // that was pressed — a 400 ms damped shake and one haptic tick — which is
    // the smallest thing that cannot be missed by someone looking at their own
    // thumb. The wording is untouched; this is the same sentence, delivered.
    var refusalTick by remember { mutableIntStateOf(0) }
    val shake = remember { Animatable(0f) }
    val haptics = LocalHapticFeedback.current
    LaunchedEffect(refusalTick) {
        if (refusalTick == 0) return@LaunchedEffect
        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
        shake.snapTo(0f)
        shake.animateTo(1f, tween(durationMillis = 420, easing = LinearEasing))
    }

    Box(
        Modifier
            .size(if (live) 96.dp else 88.dp)
            // ROUND 24 item 110(b): the tour's first two steps ring this.
            .then(rememberTutorialAnchor(com.lidarscan.core.capture.TutorialAnchor.SCAN_BUTTON))
            .graphicsLayer {
                val p = shake.value
                if (p > 0f && p < 1f) {
                    // Two full cycles, amplitude decaying to nothing: a shake
                    // that ENDS is a refusal; one that keeps going is a fault
                    // light. The pulse rides the same envelope so the button
                    // reads as recoiling rather than as vibrating in place.
                    val envelope = 1f - p
                    translationX = kotlin.math.sin(p * 4f * kotlin.math.PI.toFloat()) *
                        envelope * 10.dp.toPx()
                    val pulse = 1f + 0.05f * envelope
                    scaleX = pulse
                    scaleY = pulse
                }
            }
            // ── ROUND 29 item 170 (review finding S11) ─────────────────────
            //
            // A disabled FAB used to be the brand orange at 45 % with its glow
            // intact — "a muddy brown-orange, still the loudest object on
            // screen while being the one thing you cannot press", which reads
            // as broken rather than as not-yet. The mockup is explicit
            // (`.fab.off`): trough ground, ink-faint mark, **no shadow**. It is
            // still tappable and still answers a press with item 132's shake —
            // what changes is that it stops shouting.
            .then(
                if (armed || live) {
                    Modifier
                        .shadow(16.dp, CircleShape, ambientColor = Ember, spotColor = Ember)
                        .background(Ember, CircleShape)
                } else {
                    Modifier.background(ScanColors.trough, CircleShape)
                },
            )
            // ── ROUND 23 item 101(b): NEVER INERT, NEVER SILENT ────────────
            //
            // `enabled = true` unconditionally. A press with no sensor attached
            // — or during a seal — used to go nowhere at all: no ViewModel
            // call, no log line, no pixel changed. A button that will not act
            // must still ANSWER.
            .clickable(enabled = true, role = Role.Button) {
                when {
                    live -> onStop()
                    startBlockedReason != null -> {
                        // ROUND 27 item 132: the same callback (so the log line
                        // and the amber sentence are unchanged) plus the answer
                        // the thumb can feel. The tick is a counter rather than
                        // a boolean because the SECOND refused press must shake
                        // again — a boolean that is already true recomposes
                        // nothing, which is the classic version of this bug.
                        refusalTick++
                        onStartRefused(startBlockedReason)
                    }
                    else -> onStart()
                }
            }
            .semantics { contentDescription = recordLabel }
            .testTag("recordButton"),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            if (starting && !live) {
                CircularProgressIndicator(
                    modifier = Modifier.size(22.dp),
                    color = fabInk,
                    strokeWidth = 3.dp,
                )
                Spacer(Modifier.height(5.dp))
            } else {
                // The universal record/stop mark is KEPT above the word: a dot
                // while idle, a square while live. The word says what happens;
                // the mark says which state you are in.
                Box(
                    Modifier
                        .size(if (live) 22.dp else 20.dp)
                        .background(fabInk, if (live) RoundedCornerShape(5.dp) else CircleShape),
                )
                Spacer(Modifier.height(6.dp))
            }
            Text(
                recordWord,
                fontFamily = com.lidarscan.app.ui.theme.UiFontFamily,
                fontWeight = FontWeight.Bold,
                fontSize = 15.sp,
                letterSpacing = 0.08.em,
                color = fabInk,
                modifier = Modifier.testTag("recordButtonLabel"),
            )
        }
    }
}

/**
 * ROUND 26 item 124 — **one pill, four facts: sensor · time · points · metres.**
 *
 * These were a badge in the app bar, a duration inside a mono line, a point
 * count beside it and a trail length painted under the coverage arcs — four
 * widgets in three bands, none of which a walking operator could take in at a
 * glance. A fullscreen viewport has exactly one place for a status read-out,
 * and this is it.
 *
 * `pointsCapturedValue` MUST stay on the points number and MUST stay parseable
 * as either a grouped integer or "1.24 M" — the CI emulator smoke test polls
 * this exact node for ~20 s to prove the native decoder is landing points.
 *
 * The numbers are [MonoTabular]: they change once a second under a pill that
 * does not move, and proportional digits would make the whole pill twitch.
 */
@Composable
private fun ScanStatusPill(
    sensor: SensorType,
    connected: Boolean,
    captureState: CaptureState,
    stats: CaptureStats,
    trailLengthM: Float,
    liveView: Boolean,
    modifier: Modifier = Modifier,
    /**
     * ROUND 27 item 129(a) — the device-health read-out, or null when the
     * caller does not own it.
     *
     * Round 26 hung this in the viewport's bottom-END corner. In portrait that
     * is a real corner; in landscape it is the middle of the picture, one chip
     * away from the SCAN button — the owner's "stray 'No data' chip floating
     * mid-screen". A read-out belongs with the other read-outs, and this pill
     * is where the other four already are.
     */
    health: DeviceHealth? = null,
    /** ROUND 27 item 129(a) — draw the health chip in the pill at all. Null health still reads "No data". */
    showHealth: Boolean = false,
    /**
     * ROUND 27 item 138 — **the one tracking status on this screen**, or null
     * when there is nothing to say (no pose requirement, or nothing connected).
     *
     * The owner counted two. There were two: a `poseTrackingViewportChip` in
     * the viewport's top-centre corner and a second pose chip inside
     * `CaptureChipRow`, both alive on a connected D6, both derived from the
     * same `poseState` but composed in different subtrees — so mid-transition
     * they could even disagree for a frame. One fact, one owner, and the owner
     * is the pill the operator already reads for everything else.
     */
    poseState: PoseTrackingState? = null,
    onOpenDiagnostics: () -> Unit = {},
) {
    val live = captureState == CaptureState.RECORDING || captureState == CaptureState.PAUSED
    Column(
        modifier
            .background(MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.86f), RoundedCornerShape(14.dp))
            .border(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f), RoundedCornerShape(14.dp))
            .padding(horizontal = 11.dp, vertical = 7.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            ScanChip(
                text = sensor.badgeLabel.uppercase(),
                // ROUND 25 item 119: exhaustive, see `sensorBadgeColor`. Null
                // while disconnected is deliberate — an untinted chip is how
                // this says "nothing on the cable yet".
                color = if (connected) sensorBadgeColor(sensor) else null,
                showDot = true,
            )
            Spacer(Modifier.width(8.dp))
            Text(
                formatDuration(stats.elapsedMillis),
                style = MonoTabular.copy(fontSize = 14.sp),
                color = if (live) Ember else MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
            )
        }
        Spacer(Modifier.height(3.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                formatPoints(stats.pointsCaptured),
                style = ScanMeta,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                modifier = Modifier.testTag("pointsCapturedValue"),
            )
            Text(
                " pts · ${String.format(java.util.Locale.US, "%.1f", trailLengthM)} m",
                style = ScanMetaCaps,
                color = ScanColors.inkFaint,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        // The honesty the Live switch's caption used to carry, moved to where
        // the operator is already looking and said only when it is TRUE — a
        // permanent "display only" caption is noise; "still recording" printed
        // over a black viewport is the one sentence that stops a panic.
        if (!liveView) {
            Spacer(Modifier.height(2.dp))
            Text(
                if (live) "Live view off · still recording" else "Live view off",
                style = ScanMetaCaps,
                color = ScanColors.coverageAmber,
                maxLines = 1,
            )
        }
        if (showHealth || poseState != null) {
            Spacer(Modifier.height(3.dp))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                if (poseState != null) {
                    ScanChip(
                        text = poseState.chipLabel,
                        color = poseState.chipColor,
                        showDot = true,
                        modifier = Modifier.testTag("poseTrackingViewportChip"),
                    )
                }
                if (showHealth) {
                    CaptureHealthChip(health = health, onOpenDiagnostics = onOpenDiagnostics)
                }
            }
        }
    }
}

/**
 * ROUND 27 item 142(b) — **the card that would have saved the OPPO user six
 * attempts.**
 *
 * Its whole job is to end the state where a dead camera and a slow start look
 * identical. It is deliberately not a dialog (this tab's rule since round 5)
 * and not a hint (a hint is a note; this is a fault), so it takes the shape of
 * the loud band the save-failure banner already uses.
 *
 * The actions are what separate it from a message. **Retry** rebuilds the
 * ARCore session outright — the same `resetWorldFrame` the start gate uses,
 * which after item 143 will build one even when the old one is gone. **Update
 * AR services** goes to the Play listing, because a missing APK is a problem
 * this app cannot fix and the store can. **Send logs** is the Profile flow,
 * because the honest position on a device nobody here owns is that the next
 * step is a log.
 *
 * `UNSUPPORTED` gets no Retry, for round 27's own reason two items over: a
 * button that must fail is worse than no button.
 */
@Composable
private fun ArTroubleCard(
    kind: com.lidarscan.core.capture.ArTroubleKind,
    onRetry: () -> Unit,
    onUpdate: () -> Unit,
    onSendLogs: (() -> Unit)?,
) {
    val title = com.lidarscan.core.capture.ArTrouble.title(kind) ?: return
    val detail = com.lidarscan.core.capture.ArTrouble.detail(kind)
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 6.dp)
            .background(ScanColors.warn.copy(alpha = 0.12f), RoundedCornerShape(ScanDims.TileRadius))
            .border(1.dp, ScanColors.warn, RoundedCornerShape(ScanDims.TileRadius))
            .padding(horizontal = 14.dp, vertical = 12.dp)
            .testTag("arTroubleCard"),
    ) {
        Text(
            title,
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 16.sp,
            color = ScanColors.warn,
            modifier = Modifier.testTag("arTroubleTitle"),
        )
        if (detail != null) {
            Spacer(Modifier.height(4.dp))
            Text(
                detail,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.testTag("arTroubleDetail"),
            )
        }
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (com.lidarscan.core.capture.ArTrouble.retryable(kind)) {
                SecondaryPill(
                    text = com.lidarscan.core.capture.ArTrouble.RETRY,
                    height = 40.dp,
                    onClick = onRetry,
                    modifier = Modifier.testTag("arTroubleRetry"),
                )
            }
            if (kind == com.lidarscan.core.capture.ArTroubleKind.NEEDS_INSTALL) {
                SecondaryPill(
                    text = com.lidarscan.core.capture.ArTrouble.NEEDS_INSTALL_ACTION,
                    height = 40.dp,
                    onClick = onUpdate,
                    modifier = Modifier.testTag("arTroubleUpdate"),
                )
            }
            onSendLogs?.let {
                SecondaryPill(
                    text = com.lidarscan.core.capture.ArTrouble.SEND_LOGS,
                    height = 40.dp,
                    onClick = it,
                    modifier = Modifier.testTag("arTroubleSendLogs"),
                )
            }
        }
    }
}

/**
 * ROUND 27 item 136 — **the idle Scan page, laid out.**
 *
 * An ordinary column (portrait) or row (landscape). Nothing overlays anything,
 * and that is the point: the owner's *"nothing should overlay except warning"*
 * is satisfied by construction here rather than by the arithmetic item 129 had
 * to add to keep four floating elements out of each other's rectangles.
 *
 * The proportions are round 8's, which come back into force exactly where they
 * were right. `CaptureLayout.viewportMinHeightDp` is the live view's guaranteed
 * share; the chrome gets what is left and SCROLLS inside it, so a connect flow
 * longer than the window is a flick rather than a clipped sentence. On a
 * DISCONNECTED screen the split is deliberately the other way round — there is
 * no picture to protect, the tab's job is the connect flow, and the preview
 * shrinks to a strip that still says "nothing on the cable".
 *
 * ## Item 135, on the same lines
 *
 * There is no fixed-dp composition in here that can collide. Every band is
 * either weighted, wrapped or bounded-and-scrollable, the landscape rail is
 * `min(356 dp, 46 % of the window)` rather than 356 dp flat, and the control
 * column is a `wrapContentWidth` in a row that has already given the picture a
 * weight. A 360 × 640 dp phone gets a smaller version of this page, not a
 * broken one — asserted at that size by `Round27UiTest`.
 */
@Composable
private fun IdleScanLayout(
    isLandscape: Boolean,
    /**
     * ROUND 27 item 136 — which way the page splits.
     *
     * Round 8's rule and round 26's exception, both intact and now both drawn
     * by the same column. **Connected**, the live view keeps its 60 %: the
     * picture is the only thing that tells the operator whether the walk is
     * working, and the chrome is one mount row and one chip row. **Not
     * connected**, the trade inverts — there is no picture (the viewport is
     * empty ground saying "connect a sensor") and the tab's whole job is the
     * connect flow, so the preview shrinks to a strip that proves the viewport
     * is alive and the connect flow gets the window.
     */
    connected: Boolean,
    windowHeightDp: androidx.compose.ui.unit.Dp,
    windowWidthDp: androidx.compose.ui.unit.Dp,
    mountRowVisible: Boolean,
    viewport: @Composable (Modifier) -> Unit,
    statusBand: @Composable (Boolean) -> Unit,
    controls: @Composable (Boolean) -> Unit,
    chrome: @Composable () -> Unit,
    tutorialChip: @Composable () -> Unit,
) {
    val chromeScroll = rememberScrollState()
    // The chrome's own ceiling, so the preview is never squeezed to nothing on
    // a short phone. `heightIn(max = …)` plus `weight` on the viewport is the
    // whole of the responsive rule: whichever runs out first, both survive.
    val chromeMaxDp = (
        windowHeightDp.value - CaptureLayout.viewportMinHeightDp(
            screenHeightDp = windowHeightDp.value,
            mountRow = mountRowVisible,
            appBar = false,
        )
        ).coerceAtLeast(CaptureLayout.BAND_FLOOR_DP).dp

    Column(
        Modifier
            .fillMaxSize()
            .statusBarsPadding()
            .navigationBarsPadding()
            .testTag("scanIdlePage"),
    ) {
        // ROUND 27 items 135 + 136: in PORTRAIT the status band is a row across
        // the top. In LANDSCAPE it goes inside the start rail instead, because
        // a full-width band there costs ~120 dp of a 411 dp window — enough
        // that the control column below it could not fit its own three buttons
        // and the pause button was scrolled off the bottom of the screen. The
        // owner's word for this class of thing on his OPPO is "packed tight".
        if (!isLandscape) statusBand(false)
        if (isLandscape) {
            Row(Modifier.fillMaxWidth().weight(1f)) {
                // ROUND 27 item 135: a share of the window, capped — not 356 dp
                // on a phone that may only be 640 dp wide in landscape.
                val railWidth = minOf(
                    CaptureLayout.LANDSCAPE_RAIL_WIDTH_DP.dp,
                    // Disconnected the rail is the screen's job, so it may take
                    // more of the width; connected the picture wins.
                    windowWidthDp * (if (connected) 0.38f else 0.52f),
                )
                Column(Modifier.width(railWidth).fillMaxHeight()) {
                    // Pinned: the read-out does not scroll away from the flow
                    // it is about.
                    statusBand(false)
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .weight(1f)
                            .verticalScroll(chromeScroll)
                            // ROUND 28 item 157: the same reserve in landscape,
                            // where the rail's own bottom is the tab bar rather
                            // than the transport row.
                            .padding(
                                start = ScanDims.S3,
                                end = ScanDims.S3,
                                bottom = ScanDims.S6,
                            )
                            .testTag("scanChromeColumn"),
                    ) { chrome() }
                }
                Box(Modifier.weight(1f).fillMaxHeight().padding(horizontal = 8.dp)) {
                    viewport(Modifier.fillMaxSize())
                    Box(Modifier.align(Alignment.BottomEnd).padding(10.dp)) {
                        tutorialChip()
                    }
                }
                Column(
                    Modifier
                        .fillMaxHeight()
                        .padding(horizontal = 10.dp)
                        .verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) { controls(true) }
            }
        } else {
            Box(
                Modifier
                    .fillMaxWidth()
                    .then(
                        if (connected) {
                            Modifier.weight(1f, fill = true)
                        } else {
                            // A strip, not a stage. `min` so a short phone does
                            // not spend a third of itself on empty ground —
                            // item 135's rule, applied to the one band that is
                            // a fixed dp anywhere on this page.
                            Modifier.height(minOf(200.dp, windowHeightDp * 0.24f))
                        },
                    )
                    .padding(horizontal = 10.dp),
            ) {
                viewport(Modifier.fillMaxSize())
                Box(Modifier.align(Alignment.BottomEnd).padding(10.dp)) { tutorialChip() }
            }
            Column(
                Modifier
                    .fillMaxWidth()
                    .then(
                        if (connected) Modifier.heightIn(max = chromeMaxDp) else Modifier.weight(1f),
                    )
                    .verticalScroll(chromeScroll)
                    .padding(
                        start = ScanDims.S3,
                        end = ScanDims.S3,
                        // ── ROUND 28 item 157 (review §E-6) ─────────────────
                        //
                        // "Pick the scanner on this cable." was guillotined
                        // mid-line by the transport row: a scrollable band with
                        // no bottom padding for the controls that overlay it,
                        // so its last line could never be scrolled clear. The
                        // reserve is the control row's own height plus a
                        // section gap, which is the smallest number that
                        // guarantees the final line reaches daylight.
                        bottom = ScanDims.Fab + ScanDims.S6,
                    )
                    .testTag("scanChromeColumn"),
            ) { chrome() }
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S3),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) { controls(false) }
        }
    }
}

/**
 * ROUND 28 item 159 — **the recording page, per §D.2.**
 *
 * ```
 * ● REC  01:12   84.2 K pts   12.4 m       ●   56   telemetry, mono, tnum
 * ├──────────────────────────────────────────────┤
 * │              LIVE VIEWPORT                   │   weight(1f) — ≥60 % kept
 * ├──────────────────────────────────────────────┤
 * │ ⚠ Hold still — tracking lost            44   │   ONLY when not nominal
 * ├──────────────────────────────────────────────┤
 * │      ( ❚❚ )   ( ■ STOP )   ( ⊙ )         24  │   2 controls + the instrument
 * ```
 *
 * **Counts: 0 chips, 0 cards, 2 buttons.** The tab bar is hidden (round 26
 * item 124's choice, kept).
 *
 * ## Why it is a COLUMN and round 26's page was a stack
 *
 * Round 26 made the recording screen fullscreen and floated everything on top
 * of the picture; round 27 then spent an entire item (129) computing reserves
 * so that the floating things would not collide, and shipped a geometry test
 * to catch it when they did. On this bench, at 1080 × 2400, they collided
 * anyway: the status pill printed through the Do-Not-Disturb advisory and the
 * refresh-downshift note printed through both, three strings sharing the same
 * pixels.
 *
 * That is not a tuning failure, it is what a stack of independently-positioned
 * floating bands does the moment one of them grows a line — and every one of
 * them grows a line for a reason (round 27 item 129 lists four). A column
 * cannot overlap itself. The viewport keeps round 8's ≥60 % because it is the
 * only weighted child, which is the same arithmetic guarantee `CaptureLayout`
 * was written to make, restated where it cannot be undone by an inset.
 *
 * **The advisory INSERTS, it does not reserve.** In the nominal case it is
 * absent and the viewport is that much taller; §D.2 is explicit about this,
 * and it is the difference between a warning that means something and a band
 * that is usually empty.
 */
@Composable
private fun ScanRecordingPage(
    telemetry: @Composable () -> Unit,
    viewport: @Composable (Modifier) -> Unit,
    advisories: @Composable () -> Unit,
    controls: @Composable () -> Unit,
) {
    Column(
        Modifier
            .fillMaxSize()
            .background(ScanColors.page)
            .statusBarsPadding()
            .navigationBarsPadding()
            .testTag("scanRecordingPage"),
    ) {
        telemetry()
        Box(Modifier.fillMaxWidth().weight(1f)) { viewport(Modifier.fillMaxSize()) }
        advisories()
        Box(
            Modifier.fillMaxWidth().padding(vertical = ScanDims.S3),
            contentAlignment = Alignment.Center,
        ) { controls() }
    }
}

/**
 * §D.2's telemetry strip: one mono row, tabular figures, so nothing twitches
 * while the operator is walking.
 *
 * It replaces the floating status pill, which stated the same facts in five
 * components — a sensor badge, a filled tracking chip, an outlined state chip,
 * a health chip and a three-number line — and floated to do it.
 */
@Composable
private fun RecordingTelemetry(
    elapsed: String,
    points: Long,
    metres: Double,
    quality: Color,
    paused: Boolean,
    onOpenAdvanced: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(ScanDims.Row)
            .padding(horizontal = ScanDims.ScreenMargin)
            .testTag("scanTelemetryStrip"),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.S3),
    ) {
        StatusDot(if (paused) ScanColors.warn else ScanColors.bad)
        // `REC` is a code, not a word — §C.2's rule, and one of the few places
        // Meta Caps is correct.
        Text(
            if (paused) "PAUSED" else "REC",
            style = ScanMetaCaps,
            color = if (paused) ScanColors.warn else ScanColors.bad,
        )
        Text(
            elapsed,
            style = ScanMeta,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.testTag("elapsedValue"),
        )
        Text(
            com.lidarscan.core.render.PointCountFormat.compactPts(points),
            style = ScanMeta,
            color = ScanColors.inkMute,
            modifier = Modifier.testTag("pointsCapturedValue"),
        )
        Text("%.1f m".format(metres), style = ScanMeta, color = ScanColors.inkMute)
        Spacer(Modifier.weight(1f))
        // ROUND 28 item 159, §B Job 2's one missing number: a LIVE quality dot,
        // driven by the same inputs that produce `grade` at seal. The operator
        // learns the scan is going badly while he can still fix it, instead of
        // at seal time when nothing can be done.
        StatusDot(quality, Modifier.testTag("liveQualityDot"))
        // ── The Advanced door stays open during a recording ─────────────────
        //
        // §D.2's sketch draws no control up here, and the first cut of this
        // strip drew none — which silently deleted the one thing round 5 item
        // 10 is about: *"the display controls are adjustable against a LIVE
        // view — that is item 10's whole point."* Colour mode, point size and
        // the live-view toggle are exactly the settings whose effect can only
        // be judged while points are landing, and round 23 item 102 spent an
        // item establishing that there is ONE door to them.
        //
        // So the strip carries the door, at its end, as chrome rather than as a
        // control in the transport row — which is what §D.2's count of "two
        // controls" is protecting. The tag is `advancedButton`, unchanged,
        // because three emulator suites drive it mid-recording.
        ScanIconButton(
            icon = ScanIcons.AdvancedFaders,
            contentDescription = "Advanced",
            onClick = onOpenAdvanced,
            modifier = Modifier.testTag("advancedButton"),
        )
    }
}

/**
 * ROUND 28 item 160 — **the start flow's modal, per §D.3.**
 *
 * The owner's log measures a start sequence that was **silent for up to 21
 * seconds** and then recorded into a state it had already diagnosed as bad. The
 * screen had stages (`StartProgressPanel`, `StartStageRow`, `CaptureGateBanner`)
 * and no way to distinguish *nearly ready* from *never going to be ready*, so
 * the operator had no signal to give up and did not.
 *
 * §D.3 asks for three properties, and this composable is two of them (the third,
 * aborting on a terminal failure, is
 * [com.lidarscan.core.capture.StartGateDecision] in `:core`):
 *
 *  * **Every stage is named and bounded.** `Starting camera` (4 s) →
 *    `Hold still` (10 s) → `Recording`. Which stage, and how long is left.
 *  * **A terminal failure terminates**, in the same card, at the same size.
 *
 * **The two cards are deliberately identical in size** — the owner's explicit
 * note on the mockups. A failure card that is smaller than the progress card it
 * replaces makes the screen jump at the exact moment the operator is being told
 * something went wrong, and a jump is read as a glitch rather than as
 * information.
 *
 * The hold-still card carries the [PostureGhostIndicator], because "hold still"
 * is precisely the instruction an attitude reading helps obey (item 168) — and
 * ROUND 33 item 179(b) is why it is now a phone and not a needle: a rig can be
 * square in the screen plane and aimed at the floor, and one axis could not say
 * so.
 */
@Composable
private fun StartModalCard(
    content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            // A scrim with NO pointer-input modifier — the same trick
            // `TrackingLossPopup` uses and for the same reason: it darkens the
            // screen without stealing a tap, so nothing underneath goes dead.
            .background(Color.Black.copy(alpha = 0.45f))
            .testTag("startModalScrim"),
        contentAlignment = Alignment.Center,
    ) {
        ScanCard(
            modifier = Modifier
                .padding(horizontal = ScanDims.S8)
                // The one fixed dp on this card, and the reason it is fixed:
                // both states must be the same size. See the header.
                .heightIn(min = 172.dp)
                .testTag("startModalCard"),
            // §C.5: a modal over a scrim is one of the three things that float.
            floating = true,
            contentPadding = PaddingValues(horizontal = ScanDims.S6, vertical = ScanDims.S6),
            content = content,
        )
    }
}

/**
 * §D.3's hold-still card: attitude, title, countdown, progress, Cancel.
 *
 * `internal` rather than private for ROUND 30 item 175's emulator suite, which
 * composes **this** card — not a copy of it — against the real
 * `AppContainer.attitudeSource` so the AVD's virtual accelerometer drives the
 * production needle end to end. A screenshot of a reimplementation would prove
 * nothing about the screen the owner opens.
 */
@Composable
internal fun StartHoldModal(
    secondsLeft: Int,
    fraction: Float,
    label: String,
    attitude: kotlinx.coroutines.flow.StateFlow<com.lidarscan.core.calib.HoldOrientation?>,
    onCancel: () -> Unit,
    onPostureLost: () -> Unit = {},
) {
    StartModalCard {
        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
            // ROUND 33 item 179(b): the owner-approved 3D phone ghost, which
            // replaces round 28's single-axis dial IN THIS PLACEMENT. The card
            // is where the posture is learned — before GO, standing still, with
            // room for a picture of the phone and a word of correction — so it
            // is the placement that gets the literal instrument and the hint.
            PostureGhostIndicator(attitude = attitude, onPostureLost = onPostureLost)
        }
        Spacer(Modifier.height(ScanDims.S2))
        Text(
            label,
            style = ScanTitle,
            color = MaterialTheme.colorScheme.onSurface,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            modifier = Modifier.fillMaxWidth().testTag("startModalTitle"),
        )
        Text(
            // ROUND 28 item 160: a stage the operator must stand still through
            // is a foreground task with a deadline, and it had no countdown at
            // all. `tnum` is what stops the digit shifting the string sideways
            // once a second — round 25 item 116's finding, applied here.
            "${secondsLeft}s",
            style = ScanCountdown,
            color = MaterialTheme.colorScheme.onSurface,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = ScanDims.S2)
                .testTag("startModalCountdown"),
        )
        androidx.compose.material3.LinearProgressIndicator(
            progress = { fraction.coerceIn(0f, 1f) },
            modifier = Modifier.fillMaxWidth().height(ScanDims.S1),
            color = ScanColors.primary,
            trackColor = ScanColors.trough,
        )
        Spacer(Modifier.height(ScanDims.S3))
        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
            SecondaryPill(
                text = com.lidarscan.core.capture.StartGateDecision.CANCEL,
                onClick = onCancel,
                modifier = Modifier.testTag("startModalCancel"),
            )
        }
    }
}

/**
 * §D.3's terminal-failure card. Same size as [StartHoldModal] — see
 * [StartModalCard].
 *
 * `Start anyway` appears only for the NO_POSES case, and it is a **Secondary**:
 * recording a flat scan is a thing an operator may legitimately mean, and item
 * 155's whole point is that it becomes a decision rather than a surprise.
 */
@Composable
private fun StartBlockModal(
    block: CaptureViewModel.StartBlock,
    onRetry: () -> Unit,
    onStartAnyway: () -> Unit,
    onCancel: () -> Unit,
) {
    StartModalCard {
        Text(
            block.title,
            style = ScanTitle,
            color = ScanColors.bad,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            modifier = Modifier.fillMaxWidth().testTag("startBlockTitle"),
        )
        Spacer(Modifier.height(ScanDims.S2))
        Text(
            block.detail,
            style = ScanBody,
            color = ScanColors.inkMute,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            modifier = Modifier.fillMaxWidth().testTag("startBlockDetail"),
        )
        Spacer(Modifier.height(ScanDims.S4))
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(ScanDims.S2, Alignment.CenterHorizontally),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SecondaryPill(
                text = com.lidarscan.core.capture.StartGateDecision.CANCEL,
                onClick = onCancel,
                modifier = Modifier.testTag("startBlockCancel"),
            )
            if (block.startAnyway) {
                SecondaryPill(
                    text = com.lidarscan.core.capture.StartGateDecision.START_ANYWAY,
                    onClick = onStartAnyway,
                    modifier = Modifier.testTag("startBlockStartAnyway"),
                )
            }
            if (block.retryable) {
                PrimaryPill(
                    text = com.lidarscan.core.capture.StartGateDecision.RETRY,
                    onClick = onRetry,
                    modifier = Modifier.testTag("startBlockRetry"),
                )
            }
        }
    }
}

// ── ROUND 28 item 158: the idle page's own furniture ────────────────────────

/**
 * §D.1's status bar: `COIN-D6 · Ready` and the Advanced button, 56 dp, flat, on
 * the page ground, with a bottom hairline.
 *
 * It replaces the floating status card, which carried five components to
 * communicate "ready" and floated to do it. One clause and one control.
 */
@Composable
private fun ScanStatusBar(
    line: String,
    blocked: Boolean,
    onOpenAdvanced: () -> Unit,
) {
    Column(Modifier.fillMaxWidth().testTag("scanStatusBar")) {
        Row(
            Modifier
                .fillMaxWidth()
                .height(ScanDims.Row)
                .padding(horizontal = ScanDims.ScreenMargin),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                line,
                style = ScanTitle,
                color = if (blocked) ScanColors.bad else MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f).testTag("scanStatusLine"),
            )
            // ROUND 28 item 168: three VERTICAL faders, not the Settings tab's
            // glyph. The two used to be the same icon leading to different
            // places — see `ScanIcons`.
            ScanIconButton(
                icon = ScanIcons.AdvancedFaders,
                contentDescription = "Advanced",
                onClick = onOpenAdvanced,
                // The app's canonical Advanced tag — round 23 item 102 made it
                // "the one door", and three emulator suites drive it. The
                // GLYPH changed (item 168); the door did not.
                modifier = Modifier.testTag("advancedButton"),
            )
        }
        HorizontalDivider(thickness = ScanDims.Hair, color = MaterialTheme.colorScheme.outlineVariant)
    }
}

/**
 * §D.1's readiness row — the pattern that replaced the loose control row.
 *
 * The whole argument is in [com.lidarscan.core.capture.ScanReadiness]: a row
 * states its own state and carries its own fix, so a screen that used to need
 * six controls in two ragged rows needs three lines and no chips.
 */
@Composable
private fun ReadinessRow(
    row: com.lidarscan.core.capture.ScanReadiness.Row,
    isBlocker: Boolean,
    onAction: (() -> Unit)?,
) {
    val dot = when (row.state) {
        com.lidarscan.core.capture.ScanReadiness.State.GOOD -> ScanColors.good
        com.lidarscan.core.capture.ScanReadiness.State.WARN -> ScanColors.warn
        // Only the FIRST failing row wears the bad colour — a screen with two
        // red rows has stopped ranking its own problems.
        com.lidarscan.core.capture.ScanReadiness.State.BAD ->
            if (isBlocker) ScanColors.bad else ScanColors.warn
    }
    ScanRow(
        title = if (row.detail == null) row.title else "${row.title} · ${row.value}",
        detail = row.detail,
        meta = if (row.detail == null) row.value else null,
        leading = { StatusDot(dot) },
        modifier = Modifier.testTag("readinessRow_${row.title.lowercase()}"),
        trailing = row.actionLabel?.let { label ->
            if (onAction == null) return@let null
            {
                SecondaryPill(
                    text = label,
                    onClick = onAction,
                    height = ScanDims.S8,
                    modifier = Modifier.testTag("readinessAction_${row.title.lowercase()}"),
                )
            }
        },
    )
}

/**
 * ROUND 28 item 158 — **the idle Scan page, per §D.1.**
 *
 * ```
 * COIN-D6 · Ready                    [faders]   56   status bar, flat, hairline
 * READY TO SCAN                                      section label
 * ● Sensor      COIN-D6 connected           56       three ROWs, hairline-separated
 * ● Mount       Set · 91.0°                 56
 * ● Tracking    Ready                       56
 *                                          flex      absorbs ALL the slack
 *                  ( SCAN )                 88       the ONE FAB
 * ```
 *
 * **Counts: 0 chips, 0 pills, 1 card, 1 FAB, 3 rows** — against six chips and
 * pills in five treatments, one floating card, three circular buttons in two
 * sizes, one FAB and a floating tab pill, sharing no padding value between them.
 *
 * ## ROUND 34 item 180 — the LAST SCAN card is gone
 *
 * Item 158 gave this page a thumbnail of the newest sealed scan, on the
 * argument that ~940 px of dark rectangle should become the thing the operator
 * most often wants on opening the tab. The owner's order removes it, from both
 * idle variants. **Nothing takes its place.** The page keeps §D.1's order —
 * status bar → READY TO SCAN → flex → FAB → tab bar — and the flex region
 * absorbs the freed height, which is what it is for; the content block is
 * top-aligned inside the scroll column exactly as it already was, so on a tall
 * screen the readiness card sits under the status bar and the FAB keeps its
 * own band above the tab bar. Filling the gap with something invented would be
 * the mistake item 158 made in the other direction.
 *
 * `internal` rather than `private` for the reason [StartHoldModal] is: round
 * 34's screenshots of the CONNECTED variant have to be pictures of the page the
 * owner opens, and no COIN-D6 connects to an emulator, so the harness in
 * `Round34UiTest` composes **this** function rather than a copy of it.
 *
 * The removals, with destinations, because every one of them was somebody's
 * decision at the time: `00:00` / `0 pts` / `0.0 m` are deleted outright (never
 * render a zero-valued readout); the `D6`, `3D TRACKING` and `Idle` chips
 * become the status bar's one clause and the Tracking row; the mount pill and
 * `Re-zero` become the Mount row, whose tap IS the re-zero; the
 * `Scan-088-… OPTIMAL` pill is deleted (the scan is auto-named, the preset
 * lives in Advanced, rename lives in Review); `Diag`, the live-view eye and the
 * `?` move into the Advanced sheet; the pause circle returns during recording
 * where it means something; and `New capture` is deleted because item 156 makes
 * the tab do it and a control that repeats an automatic behaviour teaches the
 * operator that controls are decorative.
 */
@Composable
internal fun ScanReadyPage(
    statusLine: String,
    blocked: Boolean,
    onOpenAdvanced: () -> Unit,
    readiness: List<com.lidarscan.core.capture.ScanReadiness.Row>,
    onReadinessAction: (String) -> Unit,
    /**
     * ROUND 29 item 170 — **the connect flow, as a state of the Sensor row.**
     *
     * Non-null exactly when there is nothing on the cable. It is drawn INSIDE
     * the READY TO SCAN card, directly under the row that is red about it, so
     * "no scanner" and "here is how you attach one" are one object rather than
     * a status at the top of the screen and a form 900 px below it.
     */
    connectFlow: (@Composable () -> Unit)? = null,
    banners: @Composable () -> Unit,
    tutorialBanner: @Composable () -> Unit,
    fab: @Composable () -> Unit,
) {
    val blocker = com.lidarscan.core.capture.ScanReadiness.blocker(readiness)
    Column(
        Modifier
            .fillMaxSize()
            .statusBarsPadding()
            .navigationBarsPadding()
            .testTag("scanIdlePage"),
    ) {
        ScanStatusBar(line = statusLine, blocked = blocked, onOpenAdvanced = onOpenAdvanced)
        // ROUND 28 item 166: one line, under the status bar, that never
        // displaces the task. See `ScanTutorialBanner`.
        tutorialBanner()
        banners()
        Column(
            Modifier
                .fillMaxWidth()
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .testTag("scanChromeColumn"),
        ) {
            // ── ROUND 28 item 158, second cut: NO live viewport here ────────
            //
            // The first cut put the movable viewport in this slot for a replay
            // session, on the argument that it kept round 27's
            // `movableContentOf` fix alive across the idle → recording flip.
            // On the emulator that crashed the app: moving movable content out
            // of a subtree that is being removed in the same frame hit
            // `LayoutNode.onChildRemoved` with a null `layoutDelegate`, twice,
            // reproducibly, on the Start press.
            //
            // The fix is not to nurse the move — it is that there is no longer
            // a move to make. §D.1's page has no live view in it, so the
            // viewport is composed in exactly ONE place (`MinimalScanLayout`)
            // and its lifetime is the recording's: created at Start, disposed
            // at Stop, never relocated. Round 27's fix solved a problem this
            // layout does not have.
            SectionLabel("Ready to scan")
            ScanRowCard(
                modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin),
                rows = buildList<@Composable () -> Unit> {
                    for (row in readiness) {
                        add {
                            ReadinessRow(
                                row = row,
                                isBlocker = row === blocker,
                                onAction = if (row.actionLabel != null) {
                                    { onReadinessAction(row.title) }
                                } else {
                                    null
                                },
                            )
                        }
                        // ── ROUND 29 item 170: the SCANNER MISSING variant ──
                        //
                        // The mockup's second idle phone. The Sensor row goes
                        // bad, keeps its own Retry, and the connect flow opens
                        // under it as part of the same card — one object, not a
                        // page. Everything else on the page is unchanged, which
                        // is the point: round 28 built two different screens for
                        // one state of one screen, and the owner only ever saw
                        // the old one.
                        if (connectFlow != null && row.title == "Sensor") {
                            add {
                                Column(
                                    Modifier
                                        .fillMaxWidth()
                                        .padding(bottom = ScanDims.S3)
                                        .testTag("scanConnectFlow"),
                                ) { connectFlow() }
                            }
                        }
                    }
                },
            )
        }
        Box(
            Modifier
                .fillMaxWidth()
                .padding(vertical = ScanDims.S6),
            contentAlignment = Alignment.Center,
        ) { fab() }
    }
}

/**
 * ROUND 27 item 136 — **the recording page: round 26's fullscreen layout, kept
 * for the state it was right for.**
 *
 * Here the argument item 124 made is simply true. There IS a picture, it is the
 * only thing that tells the operator whether the walk is working, and the four
 * controls a walking thumb needs belong on top of it rather than in a band that
 * costs it 200 dp. What round 27 removes is everything that is not one of those
 * four (item 137): no scan-name field, no re-zero, no New-capture chip, no
 * preset chip, no connect flow — those are decisions taken before Start, and a
 * decision you cannot act on is clutter over the only thing you can.
 *
 * What remains, exhaustively: the status pill (sensor · time · points · metres,
 * plus tracking and health), the gear, the control cluster (STOP, pause, eye),
 * the stream chip, the `?`, the coverage mini-map the viewport draws, and
 * whatever is warning you.
 */
@Composable
private fun androidx.compose.foundation.layout.BoxScope.MinimalScanLayout(
    isLandscape: Boolean,
    bottomClearance: androidx.compose.ui.unit.Dp,
    endRailDp: androidx.compose.ui.unit.Dp,
    onEndRailMeasured: (Float) -> Unit,
    onTopBandMeasured: (Float) -> Unit,
    onBottomBandMeasured: (Float) -> Unit,
    viewport: @Composable (Modifier) -> Unit,
    statusBand: @Composable (Boolean) -> Unit,
    controls: @Composable (Boolean) -> Unit,
    loudBanners: @Composable () -> Unit,
    hints: @Composable () -> Unit,
    streamChip: @Composable () -> Unit,
    tutorialChip: @Composable () -> Unit,
) {
    viewport(Modifier.fillMaxSize())

    Column(
        Modifier
            .align(if (isLandscape) Alignment.TopEnd else Alignment.TopCenter)
            .onGloballyPositioned { onTopBandMeasured(it.boundsInParent().bottom) }
            .then(
                if (isLandscape) {
                    Modifier
                        .padding(end = endRailDp)
                        .width(CaptureLayout.LANDSCAPE_TOP_GROUP_WIDTH_DP.dp)
                } else {
                    Modifier.fillMaxWidth()
                },
            )
            .statusBarsPadding()
            .testTag("scanTopBand"),
    ) { statusBand(true) }

    // The one licence the overlay ban grants: a warning, and the four-second
    // answer to a press. Bounded and scrollable so six advisories at once
    // cannot become a wall over the picture.
    Column(
        Modifier
            .align(Alignment.TopCenter)
            .fillMaxWidth()
            .statusBarsPadding()
            .padding(top = 104.dp)
            .heightIn(max = 220.dp)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp)
            .testTag("scanChromeColumn"),
    ) {
        loudBanners()
        hints()
    }

    if (isLandscape) {
        Column(
            Modifier
                .align(Alignment.CenterEnd)
                .onGloballyPositioned { onEndRailMeasured(it.boundsInParent().left) }
                .navigationBarsPadding()
                .padding(end = 18.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) { controls(true) }
    }
    Row(
        Modifier
            .align(Alignment.BottomCenter)
            .onGloballyPositioned { onBottomBandMeasured(it.boundsInParent().top) }
            .fillMaxWidth()
            .navigationBarsPadding()
            .padding(horizontal = 14.dp)
            .padding(bottom = bottomClearance),
        verticalAlignment = Alignment.Bottom,
    ) {
        streamChip()
        Spacer(Modifier.weight(1f))
        if (!isLandscape) {
            controls(false)
            Spacer(Modifier.weight(1f))
        }
        Box(Modifier.padding(end = if (isLandscape) endRailDp else 0.dp)) { tutorialChip() }
    }
}

/**
 * ROUND 27 item 129(a) — the health read-out and the door to Diagnostics.
 *
 * Lifted out of `CaptureViewport` so the SCREEN can place it (in the status
 * pill) while the calibration wizard's inset card keeps it in a corner. One
 * composable, so the two placements cannot drift into two chips that say
 * different things — and exactly one `captureHealthChip` node in the tree,
 * because three suites select on that tag and two nodes is an ambiguous
 * selector rather than a duplicated affordance.
 *
 * The chip's own ink is chip-sized; the 44 dp minimum comes from the Box it
 * sits in, so the target grows without the chip inflating.
 */
@Composable
private fun CaptureHealthChip(
    health: DeviceHealth?,
    onOpenDiagnostics: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val (healthLabel, healthColor) = healthReadout(health)
    Box(
        modifier
            .height(ScanDims.Touch)
            .clickable(role = Role.Button, onClick = onOpenDiagnostics)
            .semantics { contentDescription = "Diagnostics — device health $healthLabel" }
            .testTag("captureHealthChip"),
        contentAlignment = Alignment.CenterStart,
    ) {
        ScanChip(text = healthLabel, color = healthColor)
    }
}

/**
 * ROUND 27 item 129(a) — **what stream is on screen**, in the bottom-start
 * corner beside the map-mode chip.
 *
 * ROUND 5 AUDIT bugfix, unchanged: `liveSlam` alone used to drive this label —
 * true as soon as the OPERATOR asked for live SLAM/pushbroom, even while
 * `StreamFilter.MAPPED_ONLY` was still falling back to raw pages because no
 * mapped page had resolved yet. ROUND 6 keyed it off what is actually on
 * screen, and "RAW" also covers the Light preset's deliberate raw-only view,
 * which is a choice rather than a missing map — and it says so, so nobody
 * reads it as a failure.
 *
 * What round 27 changed is only WHERE it is drawn. As a viewport chip it took
 * the bottom-START corner, which in landscape is inside the connect rail: on
 * the AVD it was printed straight through the manual-entry panel.
 */
@Composable
private fun StreamModeChip(
    liveMapEnabled: Boolean,
    liveMapRequested: Boolean,
    hasSeenMappedPage: Boolean,
    modifier: Modifier = Modifier,
) {
    ScanChip(
        // ROUND 27 item 138: no device name here. The owner counted TWO
        // connected-device models on this tab and this was the second — the
        // status pill's badge is the sensor's one owner, and `RAW · D6` said it
        // again three centimetres away, in a chip whose actual subject is the
        // STREAM.
        text = when {
            !liveMapEnabled -> "Raw · light preset"
            !liveMapRequested -> "Raw returns"
            hasSeenMappedPage -> "Live map · 3D"
            else -> "BUILDING MAP…"
        },
        color = ScanColors.sensorMid360,
        showDot = true,
        modifier = modifier.testTag("streamModeChip"),
    )
}

/**
 * ROUND 22 item 95's ADVANCED ⚙, top-end. The same `CaptureSettingsSheet` it
 * has always opened, and — asserted by `Round23ScanTabTest` — still the ONLY
 * `advancedButton` in the tree.
 */
@Composable
private fun ScanGearButton(onOpenAdvanced: () -> Unit) {
    SecondaryRoundButton(
        size = 48.dp,
        enabled = true,
        contentDescription = "Advanced settings",
        testTag = "advancedButton",
        modifier = Modifier.then(rememberTutorialAnchor(com.lidarscan.core.capture.TutorialAnchor.ADVANCED)),
        onClick = onOpenAdvanced,
    ) {
        Icon(Icons.Filled.Tune, contentDescription = null, tint = MaterialTheme.colorScheme.onSurface)
    }
}

/**
 * ROUND 27 item 140(a) — **REMOVED from the screen, kept as the record of a
 * decision.**
 *
 * The owner: *"The map indicatior seems useless."* He is right, and the reason
 * is worth writing down rather than just deleting: the chip reported
 * `liveMapEnabled`, which the operator sets once (via the preset, or the Live
 * 3D map switch) and then never changes — so it is a chip that says the same
 * word for an entire session. Round 27's item 133(c) had just made it a TOGGLE
 * to give it a job; the owner's answer is that the job was not worth a corner
 * of the picture either. The switch keeps its home in the Capture sheet, where
 * its explanation is, and the corner goes back to being picture.
 *
 * The composable is left here, uncalled and private, only long enough to carry
 * this note into the history; a future round that wants a live-map affordance
 * on the viewport should read [StreamModeChip] first — that one reports what is
 * actually on screen, which is the question an operator is really asking.
 *
 * ROUND 26 item 124 — the bottom-START corner: what the live view is actually
 * drawing.
 *
 * `MAP` means the engine's pushbroom is resolving the fan into a cloud;
 * `SLICES` means the viewport is showing raw returns because live mapping is
 * off or the map is full.
 *
 * ## ROUND 27 item 133(c) — it is the switch now
 *
 * Round 26 shipped TWO doors to the same Capture sheet: this chip and the chip
 * row's `captureConfigChip`. Round 26's argument for making this one a door was
 * that "flipping live SLAM mid-walk is not a one-tap decision" — but that is an
 * argument about `liveSlam`, the Mid-360 engine session, and this chip has
 * never controlled it. What it reports is `liveMapEnabled`, which is a VIEW
 * preference: whether the preview draws the resolved map or the raw fan. That
 * genuinely is a one-tap decision, it changes nothing about what is recorded,
 * and a chip that reads MAP / SLICES and opens a settings sheet is the one
 * shape a toggle must not have.
 *
 * So the two doors get one owner each: this chip toggles, and the config chip —
 * which is labelled for the sheet and lives in the chip row with the other
 * sheet doors — opens the sheet. The explanation the sheet carries is not lost;
 * it is still one tap away, on the row this chip is the shortcut for.
 */
@Suppress("UnusedPrivateMember")
@Composable
private fun MapModeChip(liveMapEnabled: Boolean, liveMapRequested: Boolean, onClick: () -> Unit) {
    ScanChip(
        text = when {
            liveMapEnabled -> "MAP"
            liveMapRequested -> "Map full"
            else -> "Slices"
        },
        color = if (liveMapEnabled) ScanColors.sensorD6 else null,
        modifier = Modifier
            .clickable(role = Role.Switch, onClick = onClick)
            .semantics {
                contentDescription =
                    if (liveMapEnabled) "Live map on, showing the map" else "Live map off, showing raw slices"
            }
            .testTag("mapModeChip"),
    )
}

/**
 * ROUND 26 item 124 — the bottom-END corner: the tour.
 *
 * Round 24 put this at the head of the chip row. In a fullscreen layout the
 * chip row floats in the middle of the picture and can be scrolled away from
 * the `?`, which is precisely the wrong place for the control someone who is
 * lost reaches for. A fixed corner cannot be scrolled away from.
 */
@Composable
private fun TutorialChip(onOpenTutorial: () -> Unit) {
    Box(
        Modifier
            .size(38.dp)
            .background(MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.92f), CircleShape)
            .border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
            .clickable(role = Role.Button, onClick = onOpenTutorial)
            .semantics { contentDescription = com.lidarscan.core.capture.ScanTutorial.HELP_LABEL }
            .testTag("tutorialButton"),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            "?",
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.Bold,
            fontSize = 16.sp,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

// ── the RTK / georeference chip strip ───────────────────────────────────────

@Composable
private fun FixChipStrip(fix: GnssFixSnapshot, ntrip: NtripStatsSnapshot, georefSource: GeorefSourceState) {
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 14.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        // ROUND 5.2: the georeference source chip comes FIRST, because it is the
        // one that says whether this scan is centimetres or metres. "RTK FIXED
        // ±2 cm" with a rover, "PHONE GPS ±4.2 m" on the automatic fallback — the
        // wording never lets the two be mistaken for each other.
        ScanChip(
            text = georefSource.chipLabel,
            color = when {
                georefSource.isRtk -> if (fix.hasFix) fixColor(fix.fix) else ScanColors.good
                georefSource.isPhoneFallback -> ScanColors.warn
                else -> null
            },
            showDot = true,
            modifier = Modifier.testTag("georefSourceChip"),
        )
        if (!georefSource.isRtk) {
            // No rover fix: say why in one word rather than showing three empty
            // RTK chips (round 5 item 7's "fewer things on screen").
            ScanChip(text = if (ntrip.receiving) "NTRIP live" else "No rover")
        } else {
            ScanChip(text = "NTRIP ${ntrip.state.name}")
            ScanChip(
                text = if (ntrip.receiving) "Corrections live" else "No RTCM",
                color = if (ntrip.receiving) ScanColors.good else null,
            )
            if (fix.hasFix) ScanChip(text = "${fix.satellites} SATS")
        }
    }
}

// ── session summary ─────────────────────────────────────────────────────────

@Composable
private fun SessionSummaryContent(
    summary: CaptureStats,
    scanSummary: com.lidarscan.core.capture.ScanSummary?,
    savedPath: String?,
    saveError: String?,
    autoProcess: AutoProcessState,
    onDismiss: () -> Unit,
) {
    Column(Modifier.padding(horizontal = 22.dp, vertical = 8.dp)) {
        Text(
            if (saveError != null) "Session ended — NOT saved" else "Scan summary",
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 22.sp,
            color = MaterialTheme.colorScheme.onSurface,
        )
        // ROUND 11 (owner item 44): the grade, first, because it is the whole
        // point of the card — "keep or rescan in five seconds". The numbers
        // below it are the evidence for the word above them.
        if (scanSummary != null && saveError == null) {
            Spacer(Modifier.height(12.dp))
            // ROUND 16 item 62: one truth on the card. See ScanGradeBanner.
            ScanGradeBanner(
                scanSummary,
                processedSections = autoProcess.result?.takeIf { it.ran }?.sections,
            )
        }
        // ROUND 15 (item 55): auto-process, right under the grade, because
        // what it produces REPLACES numbers above it — a scan that recorded in
        // five pieces is a different scan once they are back in one frame, and
        // the operator should not have to open Review to learn that.
        if (saveError == null && autoProcess.active) {
            Spacer(Modifier.height(10.dp))
            AutoProcessPanel(autoProcess)
        }
        Spacer(Modifier.height(16.dp))
        StatPanel(
            listOf(
                // ROUND 11: this is now the RESOLVED map count, not the raw
                // preview stream added to it. See PointCountTally.
                Stat(formatPoints(summary.pointsCaptured), "points"),
                Stat(formatDuration(summary.elapsedMillis), "duration"),
                Stat(
                    scanSummary?.let { "%.1f m".format(it.pathLengthMeters) } ?: "—",
                    "walked",
                ),
                Stat(formatMegabytes(summary.recordingSizeBytes), ".lscan"),
            ),
        )
        if (scanSummary != null) {
            Spacer(Modifier.height(10.dp))
            StatPanel(
                listOf(
                    Stat("${scanSummary.sections}", "sections"),
                    Stat("${scanSummary.trackingDrops}", "tracking drops"),
                    // ROUND 14: a from-the-spot scan showed "50124 pts / metre"
                    // here, which is not a density — it is how little the
                    // operator walked. The card now prints the figure the grade
                    // was actually computed from, in its own unit, so the two
                    // can never tell different stories. For a walking scan
                    // nothing changes.
                    if (scanSummary.isFromTheSpot) {
                        Stat("from the spot", "no walk to measure")
                    } else {
                        Stat("%.0f".format(scanSummary.pointsPerMeter), "pts / metre")
                    },
                    Stat(formatRate(scanSummary.pointsPerSecond), "avg pts/s"),
                ),
            )
        }
        Spacer(Modifier.height(12.dp))
        // ROUND 6 (item 20): the one line the owner could not get last time —
        // where it went, or why it did not.
        Text(
            when {
                saveError != null -> saveError
                // ROUND 29 item 170(d): item 164 removed a raw filesystem path
                // from the top of Settings; this sheet still printed one, two
                // lines of `/storage/emulated/0/Android/data/…` at the end of
                // every scan. The operator cannot use it, cannot read it and
                // cannot act on it — the scan's NAME is what he can, and it is
                // the word he will look for in the Projects tab one tap later.
                // The full path is still written to the capture log at seal,
                // which is where a path is actually read.
                savedPath != null ->
                    "Saved as ${savedPath.trimEnd('/').substringAfterLast('/')} — " +
                        "it is in the Projects tab now."
                else -> "Nothing was written for this session."
            },
            style = ScanMeta,
            color = if (saveError != null) ScanColors.bad else ScanColors.inkFaint,
            modifier = Modifier.testTag("sessionSavedPath"),
        )
        Spacer(Modifier.height(16.dp))
        // "Done" and not "Close": dismissing this card is what carries the
        // operator to Projects (see the pending-navigation effect in
        // CaptureRoute), so the button ends the scan rather than hiding a sheet.
        PrimaryPill(
            text = "Done",
            onClick = onDismiss,
            modifier = Modifier.fillMaxWidth().testTag("summaryDone"),
        )
        Spacer(Modifier.height(20.dp))
    }
}

/**
 * ROUND 11 (owner item 44) — one word and one sentence.
 *
 * The grade's thresholds are in `:core`
 * ([com.lidarscan.core.capture.ScanSummary]) and every one of them is a
 * consequence of a number this project measured rather than a number someone
 * liked; the sentence under the word names the WORST thing about the scan, in
 * the same order the grade decided, so the two can never disagree.
 */
/**
 * ROUND 15 item 55 + 57 — what processing did, on the card, in plain words.
 *
 * Three states and they read differently on purpose:
 *
 *  * RUNNING — a bar and a sentence. The card is modal and holds navigation
 *    (see the LaunchedEffect in CaptureScreen), so this is the one place a
 *    progress bar can live without a second dialog.
 *  * DONE — the POST-process headline, the mount warning if the watchdog
 *    fired, and item 57's repeat-accuracy line.
 *  * FAILED — the scan is saved; here is what to do. Never an error code.
 */
@Composable
private fun AutoProcessPanel(state: AutoProcessState) {
    val bg = if (state.failed) {
        MaterialTheme.colorScheme.errorContainer
    } else {
        MaterialTheme.colorScheme.surfaceVariant
    }
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(bg)
            .padding(horizontal = 14.dp, vertical = 12.dp)
            .testTag("autoProcessPanel"),
    ) {
        state.line?.let { line ->
            Text(
                line,
                fontSize = 14.sp,
                lineHeight = 19.sp,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.testTag("autoProcessLine"),
            )
        }
        if (state.running) {
            Spacer(Modifier.height(10.dp))
            androidx.compose.material3.LinearProgressIndicator(
                progress = { state.progress },
                modifier = Modifier
                    .fillMaxWidth()
                    .testTag("autoProcessProgress"),
            )
        }
        // ROUND 16 item 61: ONE implementation of these three sentences —
        // see ui/components/ProcessResultLines.kt for why there were two.
        com.lidarscan.app.ui.components.ProcessResultLines(
            stitch = state.result,
            detailTag = "autoProcessDetail",
            selfCheckTag = "autoProcessSelfCheck",
            mountWarningTag = "autoProcessMountWarning",
        )
    }
}

@Composable
private fun ScanGradeBanner(
    summary: com.lidarscan.core.capture.ScanSummary,
    /**
     * ROUND 16 item 62 — the POST-process section count, once there is one.
     *
     * The owner's scan-038 sealed with `sections=3` and auto-processed to
     * `sections=2`, and the card showed the first while the log went on to
     * print the second. Neither number was wrong: the live detector splits on
     * every discontinuity as it arrives, and the offline one re-derives the
     * seams from the recorded stream, where scan-038's 1.6 s TRACKING_REGAINED
     * gap is bridged rather than split. But the operator does not have two
     * detectors, they have one card, and the card must show one truth.
     *
     * The rule is: **whichever detector last spoke.** Before processing, the
     * live count is all there is and it is shown. After, the processed count is
     * the one describing the file that now exists on disk, and it replaces it.
     * The log keeps BOTH, under `sectionsLive=` and `sectionsProcessed=`, so a
     * field report can still tell the two apart — which is the only place the
     * distinction is useful.
     */
    processedSections: Int? = null,
) {
    val summary = if (processedSections != null && processedSections != summary.sections) {
        summary.copy(sections = processedSections)
    } else {
        summary
    }
    // ROUND 16 item 58(c): "Rescan" is right for a thin or broken scan and
    // wrong for this one — it invites "walk it again more carefully", and a
    // capture with no trajectory will not be better for anything the operator
    // does differently about walking. The card says what it is.
    val (accent, word) = when {
        // ROUND 17 item 64: two more ways a capture can be not-a-scan, and both
        // of them used to reach "Good scan" — scan-045 did exactly that.
        summary.engineStartFailed -> ScanColors.bad to "Not recorded"
        summary.isNoRoom -> ScanColors.bad to "No room — nothing was placed"
        summary.isTwoDimensionalOnly -> ScanColors.bad to "2D ONLY — NO ROOM"
        summary.grade == com.lidarscan.core.capture.ScanGrade.GOOD -> ScanColors.good to "Good scan"
        summary.grade == com.lidarscan.core.capture.ScanGrade.FAIR -> ScanColors.warn to "Usable"
        else -> ScanColors.bad to "Rescan"
    }
    Column(
        Modifier
            .fillMaxWidth()
            .background(accent.copy(alpha = 0.12f), RoundedCornerShape(12.dp))
            .padding(horizontal = 14.dp, vertical = 12.dp)
            .testTag("scanGradeBanner"),
    ) {
        Text(
            word,
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 16.sp,
            color = accent,
        )
        Spacer(Modifier.height(4.dp))
        Text(summary.gradeReason, style = ScanMeta, color = ScanColors.inkFaint)
        // ROUND 12: the conditional drift line, under the grade and not part of
        // it — the app cannot know whether the operator meant to finish where
        // they started, so it states the condition instead of assuming it.
        summary.loopReturnNote?.let { note ->
            Spacer(Modifier.height(4.dp))
            Text(
                note,
                style = ScanMeta,
                color = if ((summary.loopEndGapMeters ?: 0.0) >=
                    com.lidarscan.core.capture.LoopReturnTracker.WORTH_MENTIONING_M
                ) {
                    ScanColors.warn
                } else {
                    ScanColors.inkFaint
                },
                modifier = Modifier.testTag("scanSummaryLoopNote"),
            )
        }
        // ROUND 13 (owner item 49): what to DO before the next walk. The grade
        // describes the scan that just happened; this describes the next one.
        // "5 sections" is a count, not an instruction.
        summary.nextWalkAdvice?.let { advice ->
            Spacer(Modifier.height(6.dp))
            Text(
                advice,
                style = ScanMeta,
                color = ScanColors.warn,
                modifier = Modifier.testTag("scanSummaryNextWalk"),
            )
        }
    }
}

// ── pose tracking (ROUND 5 item 11) ─────────────────────────────────────────

/**
 * The inline tracking-quality state for a phone-tracked capture.
 *
 * `INITIALIZING → TRACKING → LOST` mirrors what ARCore actually reports, and the
 * labels say what it means for the *scan* rather than for the AR session, because
 * for a D6 this is the state of the third dimension.
 */
enum class PoseTrackingState(val chipLabel: String) {
    /** This sensor does not need phone tracking (Mid-360, replay). */
    NOT_REQUIRED(""),

    /** No ARCore at all — the capture will be fan slices, not a cloud. */
    UNAVAILABLE("No tracking"),

    /** Session up, no pose yet: move the phone slowly to let VIO converge. */
    INITIALIZING("TRACKING…"),

    /** Poses are flowing into the engine. */
    TRACKING("3D TRACKING"),

    /** Was tracking, is not now: points from this stretch are flagged and excluded by default. */
    LOST("Tracking lost"),
}

/**
 * ROUND 28 item 144 — **the colour left the constructor.**
 *
 * It was a constructor argument, which meant it was resolved once when the
 * class initialised and could only ever be one hex — the mechanism behind the
 * whole light-theme defect, in miniature. An enum entry is a *meaning*; the
 * paint for that meaning belongs to whichever theme is running when it is
 * drawn, so it is an extension property that reads the scheme.
 */
val PoseTrackingState.chipColor: Color?
    @androidx.compose.runtime.Composable
    @androidx.compose.runtime.ReadOnlyComposable
    get() = when (this) {
        PoseTrackingState.NOT_REQUIRED -> null
        PoseTrackingState.UNAVAILABLE -> ScanColors.bad
        PoseTrackingState.INITIALIZING -> ScanColors.warn
        PoseTrackingState.TRACKING -> ScanColors.good
        PoseTrackingState.LOST -> ScanColors.bad
    }

internal fun poseTrackingState(
    required: Boolean,
    arAvailable: Boolean,
    sessionRunning: Boolean,
    tracking: Boolean,
    posesPushed: Long,
    lossEpisodes: Int,
): PoseTrackingState = when {
    !required -> PoseTrackingState.NOT_REQUIRED
    !arAvailable -> PoseTrackingState.UNAVAILABLE
    tracking -> PoseTrackingState.TRACKING
    // "Lost" only after it was actually held: before the first pose the honest
    // word is initialising, and calling that "lost" would cry wolf on every
    // single capture's first two seconds.
    lossEpisodes > 0 || posesPushed > 0 -> PoseTrackingState.LOST
    sessionRunning -> PoseTrackingState.INITIALIZING
    else -> PoseTrackingState.INITIALIZING
}

// ── formatting + readouts ───────────────────────────────────────────────────

private fun captureStateLabel(state: CaptureState) = when (state) {
    CaptureState.IDLE -> "idle"
    CaptureState.RECORDING -> "recording"
    CaptureState.PAUSED -> "paused"
    CaptureState.STOPPING -> "stopping"
}

/**
 * Points, as the stat panel prints them.
 *
 * The mockup always reads out in millions. This keeps a **plain grouped
 * integer below a million** because the emulator smoke test polls this exact
 * node to assert the decoded count grows during a ~10 s replay of the bundled
 * synthetic capture (tens of thousands of points): at that scale a `0.03 M`
 * read-out would be flat for the whole window and the assertion would be
 * measuring the formatter, not the decoder.
 */
/**
 * ROUND 28 item 150 — a FOURTH variant, kept deliberately and named.
 *
 * The scan-summary panel is the one place a point count is a *figure being
 * audited* rather than a readout being glanced at: it sits beside `SECTIONS`
 * and `PTS / METRE` in a table the operator reads after the walk, and there
 * `120,300` is more useful than `120.3 K`. Every other site in the app goes
 * through [com.lidarscan.core.render.PointCountFormat]; this one states its
 * exception rather than being an accident, and it no longer disagrees about
 * the million (`%.2f M` and `PointCountFormat.compact`'s `%.1f M` were two
 * roundings of one number).
 */
internal fun formatPoints(points: Long): String =
    if (points >= 1_000_000) {
        com.lidarscan.core.render.PointCountFormat.compact(points)
    } else {
        "%,d".format(points)
    }

private fun formatRate(pointsPerSecond: Double): String = when {
    pointsPerSecond >= 1_000_000 -> "%.1fM".format(pointsPerSecond / 1_000_000.0)
    pointsPerSecond >= 1_000 -> "%.0fK".format(pointsPerSecond / 1_000.0)
    else -> "%.0f".format(pointsPerSecond)
}

private fun formatDuration(millis: Long): String {
    val totalSeconds = millis / 1000
    return "%02d:%02d".format(totalSeconds / 60, totalSeconds % 60)
}

private fun formatMegabytes(bytes: Long): String = "${(bytes / 1_000_000.0).toInt()} MB"

@androidx.compose.runtime.Composable
@androidx.compose.runtime.ReadOnlyComposable
private fun healthReadout(health: DeviceHealth?): Pair<String, Color?> = when {
    health == null -> "No data" to null
    health.state == ScanEngineNative.DeviceState.STREAMING && health.checksumPassRate >= 0.995 ->
        "Healthy" to ScanColors.good
    health.state == ScanEngineNative.DeviceState.FAULT -> "Fault" to ScanColors.bad
    health.state == ScanEngineNative.DeviceState.DEGRADED -> "Degraded" to ScanColors.warn
    else -> ScanEngineNative.DeviceState.label(health.state) to null
}

/**
 * The AR-tracking read-out, resolved once and shown in whichever sheet is open
 * — so the Capture-settings row and the Diagnostics row can never disagree.
 *
 * `off` when the ARCore session has no reason to run at all — which, since round
 * 5, also accounts for [poseTrackingRequired]: a phone-tracked D6 capture always
 * has a reason, whether or not the AR *view* is the one on screen.
 */
private fun arTrackingLabel(
    arAvailable: Boolean,
    tracking: Boolean,
    cameraMode: CameraMode,
    keyframesEnabled: Boolean,
    poseTrackingRequired: Boolean,
): String = when {
    !arAvailable -> "unavailable"
    cameraMode != CameraMode.AR && !keyframesEnabled && !poseTrackingRequired -> "off"
    tracking -> "TRACKING"
    else -> "Limited"
}

@androidx.compose.runtime.Composable
@androidx.compose.runtime.ReadOnlyComposable
private fun deviceDiagnostics(
    health: DeviceHealth?,
    captureState: CaptureState,
    stats: CaptureStats,
    sensor: SensorType,
): DeviceDiagnostics {
    val (stateText, stateColor) = when (captureState) {
        CaptureState.RECORDING -> "Streaming" to ScanColors.good
        CaptureState.PAUSED -> "Paused" to ScanColors.warn
        CaptureState.STOPPING -> "Stopping" to ScanColors.warn
        CaptureState.IDLE -> "Idle" to ScanColors.inkFaint
    }
    val checksum = health?.let { "%.2f%%".format(it.checksumPassRate * 100) } ?: "—"
    val checksumColor = when {
        health == null -> ScanColors.inkFaint
        health.checksumPassRate >= 0.995 -> ScanColors.good
        health.checksumPassRate >= 0.98 -> ScanColors.warn
        else -> ScanColors.bad
    }
    val dropped = health?.packetsBad ?: 0L
    val total = (health?.packetsOk ?: 0L) + dropped
    return DeviceDiagnostics(
        state = stateText,
        stateColor = stateColor,
        pointsPerSecond = formatRate(stats.pointsPerSecond),
        rotation = health?.let { "%.2f Hz".format(it.rotationHz) } ?: "—",
        // ROUND 5 item 11: the D6 has NO IMU. This row used to print the sensor's
        // badge here, which read as "the D6 has an IMU called COIN-D6".
        // ROUND 25 item 119: the STL-27L has no IMU either — the LD-series is
        // a spinning rangefinder and nothing else — so it reads the same line
        // as the D6. Written as its own branch rather than folded in with the
        // D6 or dropped into an `else`, because the next sensor to arrive must
        // be forced to answer this question rather than inherit an answer.
        imu = when (sensor) {
            SensorType.COIN_D6 -> "none on device · phone IMU via ARCore"
            SensorType.STL27L -> "none on device · phone IMU via ARCore"
            SensorType.MID360 -> "MID-360 built-in"
        },
        checksum = checksum,
        checksumColor = checksumColor,
        packetLoss = if (total > 0) {
            "%,d dropped · %.2f%%".format(dropped, dropped * 100.0 / total)
        } else {
            "0 dropped · 0.00%"
        },
    )
}

@androidx.compose.runtime.Composable
@androidx.compose.runtime.ReadOnlyComposable
private fun arDiagnostics(
    arAvailable: Boolean,
    arTracking: Boolean,
    cameraMode: CameraMode,
    keyframesEnabled: Boolean,
    keyframeRateFps: Int,
    keyframeStats: com.lidarscan.app.ar.KeyframeRecorder.Stats,
    trackingLossEpisodes: Int,
    poseTrackingRequired: Boolean,
    posesPushed: Long,
    mountIsNominal: Boolean,
    georefSource: GeorefSourceState,
): ArDiagnostics {
    val label = arTrackingLabel(arAvailable, arTracking, cameraMode, keyframesEnabled, poseTrackingRequired)
    return ArDiagnostics(
        tracking = label,
        trackingColor = when {
            !arAvailable || label == "off" -> ScanColors.inkFaint
            label == "TRACKING" -> ScanColors.good
            else -> ScanColors.warn
        },
        // With keyframes off the row says so rather than freezing a stale
        // integer that would read as a live number.
        keyframes = if (keyframesEnabled) {
            "%,d · $keyframeRateFps fps".format(keyframeStats.keyframesWritten)
        } else {
            "off — no colorization"
        },
        trackingLossEpisodes = "$trackingLossEpisodes",
        skippedTurning = "%,d".format(keyframeStats.skippedMotion),
        rollingShutter = if (keyframeStats.rollingShutterKnown) {
            "measured per frame"
        } else {
            "not reported by this camera"
        },
        // ROUND 5: the two numbers that say whether a phone-tracked D6 capture is
        // actually producing 3D — poses pushed into the engine, and whether the
        // extrinsic behind the pushbroom was measured or assumed.
        posesPushed = if (poseTrackingRequired || posesPushed > 0) "%,d".format(posesPushed) else "not needed",
        mountExtrinsic = if (mountIsNominal) "CAD nominal (uncalibrated)" else "measured calibration",
        mountExtrinsicColor = if (mountIsNominal) ScanColors.warn else ScanColors.good,
        georefSource = georefSource.chipLabel,
    )
}

/**
 * ROUND 5.2: bridges a `suspend` permission request to Compose's
 * `ActivityResultLauncher` callback, so the ViewModel can ask for fine location at
 * the exact moment a capture starts and simply await the answer.
 *
 * One in-flight request at a time; a second caller while one is pending gets the
 * same answer rather than launching a second system dialog.
 */
internal class PermissionRequestBridge {
    private var pending: kotlinx.coroutines.CompletableDeferred<Boolean>? = null

    suspend fun await(launch: () -> Unit): Boolean {
        pending?.let { return it.await() }
        val deferred = kotlinx.coroutines.CompletableDeferred<Boolean>()
        pending = deferred
        launch()
        return try {
            deferred.await()
        } finally {
            pending = null
        }
    }

    fun complete(granted: Boolean) {
        pending?.complete(granted)
    }
}
