package com.lidarscan.app.ui.capture

import android.content.res.Configuration
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
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
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
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
import com.lidarscan.app.ui.theme.CoverageAmber
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.app.ui.theme.MonoTabular
import com.lidarscan.app.ui.theme.MonoValue
import com.lidarscan.app.ui.theme.OnSemGoodContainer
import com.lidarscan.app.ui.theme.OnSemWarnContainer
import com.lidarscan.app.ui.theme.OnEmber
import com.lidarscan.app.ui.theme.PoseBlue
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.app.ui.theme.SemBad
import com.lidarscan.app.ui.theme.SemGood
import com.lidarscan.app.ui.theme.SemGoodContainer
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.app.ui.theme.SemWarnContainer
import com.lidarscan.app.ui.theme.ViewportGround
import com.lidarscan.app.ui.theme.sensorBadgeColor
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
                    openSerialPort = { path, baud ->
                        com.lidarscan.app.capture.openSerialPortByPath(
                            container.d6UsbConnectionRegistry,
                            path,
                            baud,
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
        ) {
            showDndExplainer = true
        }
    }
    if (showDndExplainer) {
        AlertDialog(
            // ROUND 16 item 61: dialogs inherited the theme's pill too.
            shape = RoundedCornerShape(ScanDims.DialogRadius),
            onDismissRequest = {
                showDndExplainer = false
                dndScope.launch { container.settingsRepository.setDndAccessAsked() }
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
                        showDndExplainer = false
                        dndScope.launch { container.settingsRepository.setDndAccessAsked() }
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

    // ROUND 5.3 (item 18): the screen stays awake while a capture is running.
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
        onStart = viewModel::startCapture,
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
    val screenHeightDp = LocalConfiguration.current.screenHeightDp.toFloat()
    // The mount row is only for the sensor whose extrinsic the trim is ABOUT.
    val mountRowVisible = poseTrackingRequired && sensor.isPhoneTrackedPushbroom
    // The `BackBar` survives only where there is a real parent to go back to.
    // The Capture *tab* has none — its back arrow went to Projects, which the
    // floating tab bar already does — and 56 dp is 7 % of a phone screen.
    // `isReplaySession` is the discriminator because REPLAY_CAPTURE is the only
    // project-scoped entry into this screen left (see Routes.kt).
    val showAppBar = isReplaySession || !compact

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
    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        if (showAppBar) {
            BackBar(
                title = project?.manifest?.name
                    ?: if (isReplaySession) "Capture — Replay" else "New scan",
                subtitle = when {
                    project != null -> "${project.manifest.profile.displayName} · ${captureStateLabel(captureState)}"
                    else -> "${profile.displayName} · ${captureStateLabel(captureState)}"
                },
                onBack = onBack,
                actions = {
                    ScanChip(
                        text = sensor.badgeLabel.uppercase(),
                        // ROUND 25 item 119: exhaustive, see `sensorBadgeColor`.
                        // Null while disconnected is deliberate and unchanged —
                        // an untinted chip is how this bar says "nothing on the
                        // cable yet".
                        color = if (connected) sensorBadgeColor(sensor) else null,
                        showDot = true,
                    )
                    Spacer(Modifier.width(8.dp))
                },
            )
        }

        // ROUND 8 (item 28): three RTK chips are 32 dp of a screen that a D6
        // walkthrough — which has no rover at all — spends saying "NO ROVER".
        // Shown whenever there is genuinely RTK to report, and otherwise left to
        // the Diagnostics sheet's own `Georeference source` row.
        if (!compact || georefSource.isRtk || ntrip.receiving) {
            FixChipStrip(fix = fix, ntrip = ntrip, georefSource = georefSource)
        }

        // Weighted, not fillMaxSize: a `fillMaxSize` child of a Column takes
        // the FULL incoming height, not the height left over after the app bar
        // and the chip strip — which pushed the transport row under the
        // floating tab bar. Caught on a booted emulator, not in review.
        Box(Modifier.fillMaxWidth().weight(1f)) {
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
                    val body: @Composable () -> Unit = {
                        TransportRow(
                            // ROUND 22 item 95: the one Advanced door.
                            onOpenAdvanced = { sheet = CaptureSheet.SETTINGS },
                            captureState = captureState,
                            connected = connected,
                            // ROUND 23 item 101(b): a tap that cannot start
                            // must SAY so rather than land on an inert control.
                            startBlockedReason = startBlockedReason,
                            onStartRefused = onStartRefused,
                            liveView = liveView,
                            isReplaySession = isReplaySession,
                            pauseSupported = !isReplaySession && sensor != SensorType.MID360,
                            stats = stats,
                            starting = starting,
                            onLiveViewChange = onLiveViewChange,
                            onStart = onStart,
                            onPause = onPause,
                            onResume = onResume,
                            onStop = onStop,
                        )
                    }

                    val viewport: @Composable (Modifier) -> Unit = { modifier ->
                        CaptureViewport(
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
                        // ── ROUND 24 item 110(b): the one-time offer ────────
                        //
                        // A card, not a dialog: round 5's rule is that a modal
                        // is the worst possible interruption on this tab, and
                        // that applies most of all to the first time it opens.
                        if (offerTutorial) {
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
                                title = "NO SENSOR DATA",
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
                                title = "NO POSITION TRACKING",
                                message = noPoseAlert,
                                onDismiss = onDismissNoPoseAlert,
                                testTag = "noPoseBanner",
                            )
                        }
                        // ROUND 8 (item 30b): a refused re-zero is now as loud as
                        // a failed save, and for the same reason — the owner
                        // tapped this control eight times in one session, was told
                        // "MOVING" in a grey one-liner every time, and reasonably
                        // concluded the button did nothing. The measured numbers
                        // and the instruction ("hold still ~1 s") are in the
                        // message; see MountTrimResult.Rejected.sentence.
                        if (mountTrimNote != null && mountTrimNoteIsWarning) {
                            LoudBanner(
                                title = "MOUNT REFERENCE NOT SET",
                                message = mountTrimNote,
                                onDismiss = onDismissMountTrimNote,
                                testTag = "mountTrimRefusalBanner",
                                accent = SemWarn,
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
                            Column(
                                Modifier
                                    .fillMaxWidth()
                                    .heightIn(max = CaptureLayout.HINT_BAND_MAX_DP.dp)
                                    .verticalScroll(rememberScrollState()),
                            ) {
                                // ROUND 23 item 101(b): the tap that was
                                // refused, in the words the log used, for four
                                // seconds. Amber and first, because it is the
                                // answer to something the operator did one
                                // second ago.
                                if (startTapRefusal != null) {
                                    Hint(
                                        startTapRefusal,
                                        color = SemWarn,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .clickable(onClick = onDismissStartTapRefusal)
                                            .testTag("startTapRefusalNote"),
                                    )
                                } else if (startBlockedReason != null) {
                                    // The standing reason, quietly, so the
                                    // dimmed button is never a mystery in the
                                    // first place.
                                    Hint(
                                        startBlockedReason,
                                        color = InkFaint,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("startBlockedNote"),
                                    )
                                }
                                if (dndNote != null) {
                                    // SemWarn, not InkFaint: this one is about
                                    // the measurement, not about a convenience.
                                    Hint(
                                        dndNote,
                                        color = SemWarn,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("dndUnprotectedNote"),
                                    )
                                }
                                if (georefNote != null) {
                                    Hint(
                                        georefNote,
                                        color = InkFaint,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("georefDeniedNote"),
                                    )
                                }
                                if (refreshDownshiftNote != null) {
                                    Hint(
                                        refreshDownshiftNote,
                                        color = InkFaint,
                                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                            .testTag("refreshDownshiftNote"),
                                    )
                                }
                                if (motionHint != null) {
                                    Hint(
                                        motionHint,
                                        color = SemWarn,
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
                                        color = SemWarn,
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
                                        color = SemWarn,
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
                                        color = SemWarn,
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
                                        color = ScanTeal,
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
                            onOpenTutorial = onStartTutorial,
                            preset = preset,
                            scanName = scanName.ifBlank { newScan?.autoName ?: project?.manifest?.name ?: "New scan" },
                            poseState = poseState,
                            poseChipVisible = poseTrackingRequired && connected,
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
                            maxHeight = LocalConfiguration.current.screenHeightDp.dp * 0.46f,
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
                            onOpenTutorial = onStartTutorial,
                            preset = preset,
                            scanName = scanName,
                            poseState = poseState,
                            poseChipVisible = false,
                            showCaptureChip = false,
                            onOpenCapture = {},
                            onOpenDiagnostics = { sheet = CaptureSheet.DIAGNOSTICS },
                            onOpenMid360Setup = onOpenMid360Setup,
                            onOpenRtk = onOpenRtk,
                            onNewCapture = onNewCapture.takeUnless { isReplaySession },
                        )
                    }

                    if (isLandscape) {
                        Row(Modifier.fillMaxSize()) {
                            viewport(Modifier.weight(1f).fillMaxHeight().padding(start = 14.dp, bottom = 12.dp))
                            Column(
                                Modifier
                                    .width(340.dp)
                                    .fillMaxHeight()
                                    .verticalScroll(rememberScrollState())
                                    .padding(bottom = ScanDims.TabBarClearance),
                            ) {
                                loudBanners()
                                if (compact) compactChrome() else fullChrome()
                                hints()
                                body()
                            }
                        }
                    } else {
                        Column(Modifier.fillMaxSize()) {
                            loudBanners()
                            if (compact) compactChrome() else fullChrome()
                            // ROUND 8 (item 28): the hero cloud's share of the
                            // screen is now a NUMBER, not whatever is left over.
                            // `weight(1f)` still hands it every pixel the rest of
                            // the column does not claim — that part is unchanged —
                            // but the chrome above and below it has been budgeted
                            // so that "what is left over" is at least
                            // [CaptureLayout.MIN_VIEWPORT_FRACTION] of the screen,
                            // and the `heightIn` floor below is that budget made
                            // enforceable rather than merely intended.
                            viewport(
                                Modifier
                                    .fillMaxWidth()
                                    .weight(1f)
                                    .heightIn(
                                        min = if (compact) {
                                            CaptureLayout.viewportMinHeightDp(
                                                screenHeightDp = screenHeightDp,
                                                mountRow = mountRowVisible,
                                                appBar = showAppBar,
                                            ).dp
                                        } else {
                                            CaptureLayout.VIEWPORT_FLOOR_DP.dp
                                        },
                                    )
                                    .padding(horizontal = 14.dp),
                            )
                            hints()
                            Spacer(Modifier.height(6.dp))
                            body()
                            Spacer(Modifier.height(ScanDims.TabBarClearance))
                        }
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
            autoName = newScan?.autoName,
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
                if (autoConnectState != null) {
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
                    Hint("Replay session — there is no device to connect.", color = InkFaint)
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
            sheetState = settingsSheetState,
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
 * because "NO MOUNT REF · CAD NOMINAL" in the same ink as everything else is
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
        holding -> if (hold?.gatePasses == true) ScanTeal else SemWarn
        !hasTrim -> SemWarn
        provenance?.warn == true -> SemWarn
        else -> ScanTeal
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
                        style = MonoLabel,
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
                    provenance?.chipLabel ?: "NO MOUNT REF · CAD NOMINAL",
                    style = MonoLabel,
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
    /** ROUND 24 item 110(b): the ? that opens the tour. */
    onOpenTutorial: () -> Unit = {},
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
        // ── ROUND 24 item 110(b): the ? ─────────────────────────────────────
        //
        // First in the row and deliberately small: it is a door for someone
        // who is lost, not a control anyone uses twice. A circle rather than a
        // chip because every chip in this row opens a sheet ABOUT the scan and
        // this one does not.
        Box(
            Modifier
                .size(34.dp)
                .background(MaterialTheme.colorScheme.surfaceContainer, CircleShape)
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
                fontSize = 15.sp,
                color = MaterialTheme.colorScheme.onSurface,
            )
        }
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
            Text(readout.uppercase(), style = MonoLabel, color = Ember, maxLines = 1)
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
    LoudBanner("SCAN NOT SAVED", message, onDismiss, "saveErrorBanner")

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
    val container = if (lost) SemWarnContainer else SemGoodContainer
    val onContainer = if (lost) OnSemWarnContainer else OnSemGoodContainer

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
            elevation = 16.dp,
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
    val accent = if (go) SemGood else ScanTeal
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
                if (go) "GO — START WALKING" else "STARTING SCAN",
                style = MonoLabel,
                color = accent,
                modifier = Modifier.testTag(if (go) "holdSteadyGo" else "startProgressTitle"),
            )
            Spacer(Modifier.weight(1f))
            if (!go && elapsedS != null) {
                Text(
                    "${elapsedS}s · usually 4–8 s",
                    style = MonoLabel,
                    color = InkFaint,
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
                        color = SemWarn,
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
            style = MonoLabel,
            color = if (done || live) accent else InkFaint,
        )
        Spacer(Modifier.width(8.dp))
        Text(
            title,
            style = MonoLabel,
            color = if (live) accent else if (done) MaterialTheme.colorScheme.onSurface else InkFaint,
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
            color = InkFaint,
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
    accent: Color = SemBad,
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
            style = MonoLabel,
            color = accent,
        )
        Spacer(Modifier.height(4.dp))
        Text(
            message,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.height(4.dp))
        Text("Tap to dismiss · Settings → Capture log has the full trace", style = MonoLabel, color = InkFaint)
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
) {
    Column(
        Modifier
            .fillMaxWidth()
            .heightIn(max = maxHeight)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 14.dp),
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
                )
            }
        }

        // ROUND 5 item 11: the mount hint. Short, and only for the sensors it
        // applies to — a 2-D lidar's mount geometry is the whole reason the
        // capture is 3D at all, which is as true of ROUND 25 item 119's
        // STL-27L as it is of the D6.
        if (poseTrackingRequired && sensor.isPhoneTrackedPushbroom) {
            Spacer(Modifier.height(4.dp))
            // ROUND 24 item 110(a): was 33 words including "6-DoF". The rest
            // of the explanation is now TutorialStep.SCAN_BUTTON / START_HOLD.
            Hint(
                com.lidarscan.core.Wording.D6_MOUNT_HINT + "\n" + com.lidarscan.core.Wording.D6_MOUNT_DETAIL,
                color = InkFaint,
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
                    mountTrim == null -> InkFaint
                    mountTrimProvenance?.warn == true -> SemWarn
                    else -> ScanTeal
                },
                modifier = Modifier.testTag("mountTrimAge"),
            )

            // The tracking chip belongs to a session that exists: with nothing
            // connected, "TRACKING · INITIALISING" is a status about a scan that
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
                        color = SemWarn,
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
            mountTrim == null -> SemWarn
            mountTrimProvenance?.warn == true -> SemWarn
            else -> ScanTeal
        },
        modifier = Modifier.testTag("mountTrimDetail"),
    )
    Spacer(Modifier.height(6.dp))
    // ROUND 24 item 110(a): was 69 words. The instruction half is on the
    // capture screen where the hold actually happens; this is the why.
    Hint(com.lidarscan.core.Wording.MOUNT_REF_WHY + " " + com.lidarscan.core.Wording.MOUNT_REF_HINT, color = InkFaint)
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

/** The auto-detect status line: what is happening, plus Retry / Enter manually. */
@Composable
private fun AutoDetectLine(
    state: CaptureAutoConnectState,
    onRetry: () -> Unit,
    onShowManual: () -> Unit,
    onHideManual: () -> Unit,
) {
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
            style = MonoMeta,
            color = when (state.phase) {
                CaptureAutoConnectState.Phase.PREVIEW -> SemGood
                CaptureAutoConnectState.Phase.FAILED -> SemWarn
                else -> MaterialTheme.colorScheme.onSurfaceVariant
            },
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).testTag("autoDetectStatus"),
        )
    }
    state.detection?.detail?.let {
        Text(it, style = MonoLabel, color = InkFaint, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        if (state.phase == CaptureAutoConnectState.Phase.FAILED) {
            TextButton(onClick = onRetry, modifier = Modifier.testTag("retryAutoDetectButton")) { Text("Retry") }
        }
        // "Enter manually" stays reachable at every phase, including a successful
        // detect (owner addition 1) — a rig with two devices can auto-detect the
        // wrong one.
        TextButton(
            onClick = if (state.manualEntryOpen) onHideManual else onShowManual,
            modifier = Modifier.testTag("manualEntryToggle"),
        ) {
            Text(if (state.manualEntryOpen) "Hide manual entry" else "Enter manually")
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
) {
    var manualSerialSensor by rememberSaveable { mutableStateOf(SensorType.COIN_D6) }
    val shape = RoundedCornerShape(ScanDims.TileRadius)
    Column(
        Modifier
            .fillMaxWidth()
            .heightIn(max = 260.dp)
            .background(MaterialTheme.colorScheme.surfaceContainer, shape)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
            .verticalScroll(rememberScrollState())
            .padding(12.dp)
            .testTag("manualEntryPanel"),
    ) {
        Text("USB SCANNER", style = MonoLabel, color = Ember)
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
        Hint(com.lidarscan.core.Wording.MANUAL_SERIAL_PICK, color = InkFaint)
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
                        style = MonoMeta,
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
        Text("LIVOX MID-360 · ETHERNET", style = MonoLabel, color = Ember)
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
            color = InkFaint,
        )
    }
}

// ── viewport ────────────────────────────────────────────────────────────────

@Composable
private fun CaptureViewport(
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
    val shape = RoundedCornerShape(ScanDims.CardRadius)

    // ROUND 5 AUDIT bugfix: the bottom-left "what stream is on screen" chip
    // used to read `liveSlam` alone (the requested mode) — see
    // `PointCloudRenderStats.hasSeenMappedPage`'s doc for why that mislabels
    // the whole stretch `StreamFilter.MAPPED_ONLY` spends falling back to raw
    // pages before the first registered/pushbroom-resolved page exists. This
    // polls the renderer's own stats at a cheap, UI-appropriate cadence (not
    // once per frame) so the chip only ever claims "LIVE MAP" once that is
    // actually true.
    var pointCloudRenderer by remember { mutableStateOf<com.lidarscan.app.render.PointCloudRenderer?>(null) }
    var hasSeenMappedPage by remember { mutableStateOf(false) }
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
            .background(ViewportGround, shape)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
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
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(
                    when {
                        !liveView -> "Live view is off — the recording is unaffected"
                        connected -> "3D view needs the real engine (simulated-engine build)"
                        isReplaySession -> "Starting the replay engine…"
                        else -> "Connect a sensor to see the live 3D view"
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(28.dp),
                )
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
                modifier = Modifier.align(Alignment.BottomCenter)
                    .padding(bottom = 10.dp)
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
                modifier = Modifier.align(Alignment.TopStart).padding(12.dp).testTag("keyframeChip"),
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
                        style = MonoLabel.copy(fontSize = 11.sp, letterSpacing = 0.04.em),
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
        if (poseTrackingRequired && poseState != PoseTrackingState.NOT_REQUIRED && connected) {
            ScanChip(
                text = poseState.chipLabel,
                color = poseState.chipColor,
                showDot = true,
                modifier = Modifier.align(Alignment.TopCenter).padding(top = 12.dp)
                    .testTag("poseTrackingViewportChip"),
            )
        } else if (cameraMode == CameraMode.AR && arTrackingHint != null) {
            ScanChip(
                text = arTrackingHint,
                color = SemWarn,
                modifier = Modifier.align(Alignment.TopCenter).padding(top = 52.dp),
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
        ScanChip(
            text = when {
                !liveMapEnabled -> "RAW · LIGHT PRESET"
                !liveMapRequested -> "RAW · ${sensor.badgeLabel.uppercase()}"
                hasSeenMappedPage -> "LIVE MAP · 3D"
                else -> "BUILDING MAP…"
            },
            color = PoseBlue,
            showDot = true,
            modifier = Modifier.align(Alignment.BottomStart).padding(start = 12.dp, bottom = 12.dp, end = 12.dp)
                .padding(start = 6.dp),
        )

        // ── bottom-right: the health chip, and the door to Diagnostics ──
        //
        // The chip's own ink is chip-sized; the 44 dp minimum comes from the
        // Box it sits in, so the target grows without the chip inflating.
        val (healthLabel, healthColor) = healthReadout(health)
        Box(
            Modifier
                .align(Alignment.BottomEnd)
                .padding(end = 12.dp, bottom = 6.dp)
                .height(ScanDims.Touch)
                .clickable(role = Role.Button, onClick = onOpenDiagnostics)
                .semantics { contentDescription = "Diagnostics — device health $healthLabel" }
                .testTag("captureHealthChip"),
            contentAlignment = Alignment.Center,
        ) {
            ScanChip(text = healthLabel, color = healthColor, modifier = Modifier.padding(horizontal = 6.dp))
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
                            color = SemBad.copy(alpha = 0.7f),
                            start = from,
                            end = here,
                            strokeWidth = 2.5f,
                            cap = androidx.compose.ui.graphics.StrokeCap.Round,
                            pathEffect = androidx.compose.ui.graphics.PathEffect
                                .dashPathEffect(floatArrayOf(6f, 6f)),
                        )
                    } else {
                        drawLine(
                            color = if (previousTracking && p.tracking) ScanTeal else SemBad.copy(alpha = 0.5f),
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
            previous?.let { drawCircle(color = Ember, radius = 4.5f, center = it) }

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
                        color = CoverageAmber.copy(alpha = 0.9f),
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
            style = MonoLabel.copy(fontSize = 9.sp),
            color = InkFaint,
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
@Composable
private fun TransportRow(
    captureState: CaptureState,
    connected: Boolean,
    liveView: Boolean,
    isReplaySession: Boolean,
    pauseSupported: Boolean,
    /**
     * ROUND 8 (item 28): the four capture numbers, as one mono line under the
     * Live switch, replacing the standalone four-cell `StatPanel` and its
     * spacers (~80 dp). See the `body` lambda in [CaptureScreen].
     */
    stats: CaptureStats,
    /**
     * ROUND 17 item 64 — true from the press until the capture is recording or
     * the attempt has failed. The ROUND 12 tracking gate can hold Start for
     * four to eight seconds and, until this round, NOTHING on screen changed
     * for the whole of it: `_startWarmup` was computed and rendered nowhere.
     * The owner pressed again, which is what any reasonable person does to a
     * button that does not respond, and that second press is the entire
     * scan-045 failure. A control that is working has to look like it.
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
    /**
     * ROUND 22 item 95 — **the one ADVANCED button.**
     *
     * The owner asked for the power-user controls to be *gathered*, not
     * removed: "one button opening the full existing settings sheet — Detail
     * control, display options, New-scan reset, everything power-user —
     * nothing deleted, just gathered behind one tap." This opens the SAME
     * `CaptureSettingsSheet` the display icon has always opened; what changed
     * is that there is now one obvious door to it beside the scan button
     * instead of an icon in a strip.
     */
    onOpenAdvanced: () -> Unit = {},
) {
    val recording = captureState == CaptureState.RECORDING
    val paused = captureState == CaptureState.PAUSED
    val live = recording || paused
    val stopping = captureState == CaptureState.STOPPING

    Row(
        Modifier.fillMaxWidth().padding(horizontal = 18.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Switch(
                    checked = liveView,
                    // Always editable, before AND during a recording: that is the
                    // whole point of item 10's "capture itself also runs with live
                    // view" — the operator decides, mid-walk, whether the phone
                    // should keep drawing.
                    onCheckedChange = onLiveViewChange,
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = Color.White,
                        checkedTrackColor = Ember,
                        checkedBorderColor = Ember,
                    ),
                    modifier = Modifier.testTag("liveViewSwitch"),
                )
                Spacer(Modifier.width(10.dp))
                Text(
                    if (liveView) "Live view" else "Live view off",
                    fontFamily = DisplayFontFamily,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 15.sp,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.width(8.dp))
                // The caption the numbers displaced. Kept, because "the Live
                // switch does not touch the recording" is the single most
                // important thing to know about the only control on this screen
                // that looks like it might.
                Text(
                    "display only",
                    style = MonoLabel.copy(fontSize = 9.5.sp, letterSpacing = 0.06.em),
                    color = InkFaint,
                    maxLines = 1,
                )
            }
            Spacer(Modifier.height(3.dp))
            // ROUND 8 (item 28): points / rate / duration / size, on one line.
            //
            // `pointsCapturedValue` MUST stay on the points number and MUST stay
            // parseable as either a grouped integer or "1.24 M" — the CI
            // emulator smoke test polls this exact node for ~20 s to prove the
            // native decoder is landing points, and `formatPoints` documents why
            // it stays a plain integer below a million.
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    formatPoints(stats.pointsCaptured),
                    style = MonoValue.copy(fontSize = 13.sp),
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    modifier = Modifier.testTag("pointsCapturedValue"),
                )
                Text(
                    " pts · ${formatRate(stats.pointsPerSecond)}/s · " +
                        "${formatDuration(stats.elapsedMillis)} · ${formatMegabytes(stats.recordingSizeBytes)}",
                    style = MonoLabel.copy(fontSize = 10.sp, letterSpacing = 0.04.em),
                    color = InkFaint,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }

        // Pause: offered only where it actually works. Replay has no pause hook
        // in ReplaySource (B4) and a Mid-360 cannot pause without truncating the
        // recording on resume (B2/B3) — so it is present and dimmed rather than
        // absent, which keeps the transport's shape stable across sensors.
        // ROUND 5.3 (item 18): mid-walk, one-handed, the two controls that matter
        // grow. 52 → 64 dp pause and 64 → 76 dp stop while a session is live: a
        // thumb reaching across a phone that is also carrying a lidar is not the
        // same thumb that set the scan up on a bench.
        val pauseEnabled = live && pauseSupported && !stopping
        Box(
            Modifier
                .size(if (live) 64.dp else 52.dp)
                .alpha(if (pauseEnabled) 1f else 0.3f)
                .background(MaterialTheme.colorScheme.surfaceContainerHigh, CircleShape)
                .border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
                .clickable(enabled = pauseEnabled, role = Role.Button) {
                    if (recording) onPause() else onResume()
                }
                .semantics { contentDescription = if (paused) "Resume recording" else "Pause recording" }
                .testTag("pauseButton"),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                if (paused) Icons.Filled.PlayArrow else Icons.Filled.Pause,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }

        Spacer(Modifier.width(12.dp))

        // ROUND 5 item 9: Start creates a NEW project, and the label says so —
        // there is no state in which this button records into something that
        // already existed (bar the replay path, which says "replay").
        // ROUND 22 item 95: the button's own word, on the button.
        //
        // "Start new scan" / "Stop recording" were accessibility labels on a
        // circle that showed a dot or a square. The owner asked for ONE
        // dominant control that says what it does, so the word is drawn: SCAN
        // while idle, STOP while recording, CANCEL during the start sequence —
        // which is also the first time the start sequence has been
        // interruptible from the control that began it.
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
        // ROUND 17 item 64: armed = the press will start something new. While a
        // start is in flight the button is dimmed and spinning, so the operator
        // is told by the control itself rather than by a scan that comes out
        // wrong.
        //
        // ROUND 21 item 85: the button stays TAPPABLE while a start is in
        // flight. The press still cannot start anything — the ROUND 17 atomic
        // swallows it — but it now PULSES the start-progress panel ("heard
        // you, already on it") instead of vanishing into an inert control.
        // The owner pressed a silent button three times at 01:29–01:31; a
        // swallowed press must never again be indistinguishable from a dead one.
        val armed = connected && !stopping && !starting
        // ROUND 22 item 95: ONE DOMINANT CONTROL.
        //
        // Deliberately the SAME component as before — the ember circle, the
        // ember shadow, `recordButton`'s test tag, the round-21 rule that it
        // stays tappable during a start — scaled up and given its word. The
        // brief is explicit that this is restructuring and rewording, not a
        // restyle: an operator who has used 0.9.6 must recognise this button.
        Box(
            Modifier
                .size(if (live) 108.dp else 96.dp)
                // ROUND 24 item 110(b): the tour's first two steps ring this.
                .then(rememberTutorialAnchor(com.lidarscan.core.capture.TutorialAnchor.SCAN_BUTTON))
                .alpha(if (armed || live) 1f else 0.45f)
                .shadow(16.dp, CircleShape, ambientColor = Ember, spotColor = Ember)
                .background(Ember, CircleShape)
                // ── ROUND 23 item 101(b): NEVER INERT, NEVER SILENT ────────
                //
                // This used to be `enabled = connected && !stopping`, so a
                // press with no sensor attached — or during a seal — went
                // nowhere at all: no ViewModel call, no log line, no pixel
                // changed. That is precisely the shape of the owner's
                // complaint, and it is indistinguishable to him from the item
                // 101 navigation defect that actually caused this round's.
                // A button that will not act must still ANSWER.
                .clickable(enabled = true, role = Role.Button) {
                    when {
                        live -> onStop()
                        startBlockedReason != null -> onStartRefused(startBlockedReason)
                        else -> onStart()
                    }
                }
                .semantics { contentDescription = recordLabel }
                .testTag("recordButton"),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                if (starting && !live) {
                    androidx.compose.material3.CircularProgressIndicator(
                        modifier = Modifier.size(22.dp),
                        color = OnEmber,
                        strokeWidth = 3.dp,
                    )
                    Spacer(Modifier.height(5.dp))
                } else {
                    // The universal record/stop mark is KEPT above the word: a
                    // dot while idle, a square while live. The word says what
                    // happens; the mark says which state you are in, and losing
                    // it would make a glanced-at button ambiguous.
                    Box(
                        Modifier
                            .size(if (live) 22.dp else 20.dp)
                            .background(OnEmber, if (live) RoundedCornerShape(5.dp) else CircleShape),
                    )
                    Spacer(Modifier.height(6.dp))
                }
                Text(
                    recordWord,
                    fontFamily = com.lidarscan.app.ui.theme.UiFontFamily,
                    fontWeight = FontWeight.Bold,
                    fontSize = 15.sp,
                    letterSpacing = 0.08.em,
                    color = OnEmber,
                    modifier = Modifier.testTag("recordButtonLabel"),
                )
            }
        }

        Spacer(Modifier.width(12.dp))

        // ── ROUND 22 item 95: ADVANCED ⚙, one tap, nothing deleted ──────────
        //
        // The same `CaptureSettingsSheet` that has always held the display
        // block, the Detail control and the New-scan reset — reached from one
        // labelled door beside the scan button rather than from an icon the
        // operator had to already know about. Deliberately the existing
        // secondary-control shape (the pause button's circle, its ground, its
        // hairline) so nothing about the row's visual language changes.
        Box(
            Modifier
                .size(52.dp)
                .then(rememberTutorialAnchor(com.lidarscan.core.capture.TutorialAnchor.ADVANCED))
                .background(MaterialTheme.colorScheme.surfaceContainerHigh, CircleShape)
                .border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
                .clickable(role = Role.Button, onClick = onOpenAdvanced)
                .semantics { contentDescription = "Advanced settings" }
                .testTag("advancedButton"),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                Icons.Filled.Tune,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }
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
                georefSource.isRtk -> if (fix.hasFix) fixColor(fix.fix) else SemGood
                georefSource.isPhoneFallback -> SemWarn
                else -> null
            },
            showDot = true,
            modifier = Modifier.testTag("georefSourceChip"),
        )
        if (!georefSource.isRtk) {
            // No rover fix: say why in one word rather than showing three empty
            // RTK chips (round 5 item 7's "fewer things on screen").
            ScanChip(text = if (ntrip.receiving) "NTRIP LIVE" else "NO ROVER")
        } else {
            ScanChip(text = "NTRIP ${ntrip.state.name}")
            ScanChip(
                text = if (ntrip.receiving) "CORRECTIONS LIVE" else "NO RTCM",
                color = if (ntrip.receiving) SemGood else null,
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
                savedPath != null -> "Saved to $savedPath — it is in the Projects tab now."
                else -> "Nothing was written for this session."
            },
            style = MonoMeta,
            color = if (saveError != null) SemBad else InkFaint,
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
    // ROUND 16 item 58(c): "RESCAN" is right for a thin or broken scan and
    // wrong for this one — it invites "walk it again more carefully", and a
    // capture with no trajectory will not be better for anything the operator
    // does differently about walking. The card says what it is.
    val (accent, word) = when {
        // ROUND 17 item 64: two more ways a capture can be not-a-scan, and both
        // of them used to reach "GOOD SCAN" — scan-045 did exactly that.
        summary.engineStartFailed -> SemBad to "NOT RECORDED"
        summary.isNoRoom -> SemBad to "NO ROOM — NOTHING WAS PLACED"
        summary.isTwoDimensionalOnly -> SemBad to "2D ONLY — NO ROOM"
        summary.grade == com.lidarscan.core.capture.ScanGrade.GOOD -> SemGood to "GOOD SCAN"
        summary.grade == com.lidarscan.core.capture.ScanGrade.FAIR -> SemWarn to "USABLE"
        else -> SemBad to "RESCAN"
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
        Text(summary.gradeReason, style = MonoMeta, color = InkFaint)
        // ROUND 12: the conditional drift line, under the grade and not part of
        // it — the app cannot know whether the operator meant to finish where
        // they started, so it states the condition instead of assuming it.
        summary.loopReturnNote?.let { note ->
            Spacer(Modifier.height(4.dp))
            Text(
                note,
                style = MonoMeta,
                color = if ((summary.loopEndGapMeters ?: 0.0) >=
                    com.lidarscan.core.capture.LoopReturnTracker.WORTH_MENTIONING_M
                ) {
                    SemWarn
                } else {
                    InkFaint
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
                style = MonoMeta,
                color = SemWarn,
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
enum class PoseTrackingState(val chipLabel: String, val chipColor: Color?) {
    /** This sensor does not need phone tracking (Mid-360, replay). */
    NOT_REQUIRED("", null),

    /** No ARCore at all — the capture will be fan slices, not a cloud. */
    UNAVAILABLE("NO TRACKING", SemBad),

    /** Session up, no pose yet: move the phone slowly to let VIO converge. */
    INITIALIZING("TRACKING…", SemWarn),

    /** Poses are flowing into the engine. */
    TRACKING("3D TRACKING", SemGood),

    /** Was tracking, is not now: points from this stretch are flagged and excluded by default. */
    LOST("TRACKING LOST", SemBad),
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
internal fun formatPoints(points: Long): String =
    if (points >= 1_000_000) "%.2f M".format(points / 1_000_000.0) else "%,d".format(points)

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

private fun healthReadout(health: DeviceHealth?): Pair<String, Color?> = when {
    health == null -> "No data" to null
    health.state == ScanEngineNative.DeviceState.STREAMING && health.checksumPassRate >= 0.995 ->
        "Healthy" to SemGood
    health.state == ScanEngineNative.DeviceState.FAULT -> "Fault" to SemBad
    health.state == ScanEngineNative.DeviceState.DEGRADED -> "Degraded" to SemWarn
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
    else -> "LIMITED"
}

private fun deviceDiagnostics(
    health: DeviceHealth?,
    captureState: CaptureState,
    stats: CaptureStats,
    sensor: SensorType,
): DeviceDiagnostics {
    val (stateText, stateColor) = when (captureState) {
        CaptureState.RECORDING -> "Streaming" to SemGood
        CaptureState.PAUSED -> "Paused" to SemWarn
        CaptureState.STOPPING -> "Stopping" to SemWarn
        CaptureState.IDLE -> "Idle" to InkFaint
    }
    val checksum = health?.let { "%.2f%%".format(it.checksumPassRate * 100) } ?: "—"
    val checksumColor = when {
        health == null -> InkFaint
        health.checksumPassRate >= 0.995 -> SemGood
        health.checksumPassRate >= 0.98 -> SemWarn
        else -> SemBad
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
            !arAvailable || label == "off" -> InkFaint
            label == "TRACKING" -> SemGood
            else -> SemWarn
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
        mountExtrinsicColor = if (mountIsNominal) SemWarn else SemGood,
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
