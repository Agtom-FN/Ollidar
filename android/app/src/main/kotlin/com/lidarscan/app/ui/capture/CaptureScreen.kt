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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Tune
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
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.Stat
import com.lidarscan.app.ui.components.StatPanel
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.app.ui.theme.OnEmber
import com.lidarscan.app.ui.theme.PoseBlue
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.app.ui.theme.SemBad
import com.lidarscan.app.ui.theme.SemGood
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.app.ui.theme.ViewportGround
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
) {
    // ROUND 5.2: the fine-location prompt, hoisted here because only an Activity
    // context can ask. The ViewModel calls `requestLocationPermission` at Start;
    // this bridges that suspend call to the launcher's callback.
    val locationPermissionRequest = remember { PermissionRequestBridge() }
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
                    autoDetectors = if (isReplay) {
                        emptyList()
                    } else {
                        listOf(
                            com.lidarscan.app.capture.D6UsbAutoDetector(container.d6UsbConnectionRegistry),
                            com.lidarscan.app.capture.Mid360HeartbeatAutoDetector(
                                detector = container.mid360HeartbeatDetector,
                                ethernetMonitor = container.ethernetMonitor,
                                onFound = { lidarIp, hostIp, sn ->
                                    // AUTO-DETECT §3: last-detected addresses become
                                    // this device's capture defaults.
                                    container.settingsRepository.setLastDetectedMid360(lidarIp, hostIp, sn)
                                },
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
                    openSerialPort = { path ->
                        com.lidarscan.app.capture.openSerialPortByPath(container.d6UsbConnectionRegistry, path)
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
                )
            }
        },
    )
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val connectionState by viewModel.connectionState.collectAsStateWithLifecycle()
    val captureState by viewModel.captureState.collectAsStateWithLifecycle()
    val stats by viewModel.stats.collectAsStateWithLifecycle()
    val health by viewModel.deviceHealth.collectAsStateWithLifecycle()
    val sessionSummary by viewModel.sessionSummary.collectAsStateWithLifecycle()
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
    val motionHint by viewModel.motionHint.collectAsStateWithLifecycle()
    val refreshDownshiftNote by viewModel.refreshDownshiftNote.collectAsStateWithLifecycle()
    val georefSource by viewModel.georefSource.collectAsStateWithLifecycle()
    val georefNote by viewModel.georefNote.collectAsStateWithLifecycle()
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
    val sectionHint by viewModel.sectionHint.collectAsStateWithLifecycle()

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

    LaunchedEffect(needsArSession) {
        if (!needsArSession) return@LaunchedEffect
        container.arController.refreshAvailability()
        if (!container.hasCameraPermission()) {
            permissionLauncher.launch(android.Manifest.permission.CAMERA)
        } else {
            container.arController.createSession()
            container.arController.resume()
        }
    }
    DisposableEffect(needsArSession) {
        onDispose { if (viewModel.arAvailable) container.arController.pause() }
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
        sessionSummary = sessionSummary,
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
        arAvailable = viewModel.arAvailable,
        arTracking = arStatus?.tracking == true,
        arSessionRunning = arStatus?.sessionRunning == true,
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
        onManualDeviceConnect = viewModel::connectManualD6,
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
        mountTrimProvenance = mountTrimProvenance,
        noDataAlert = noDataAlert,
        onDismissNoDataAlert = viewModel::dismissNoDataAlert,
        sectionHint = sectionHint,
        onSetMountReference = viewModel::setMountReference,
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
    sessionSummary: CaptureStats?,
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
    arAvailable: Boolean,
    arTracking: Boolean,
    arSessionRunning: Boolean,
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
    onManualDeviceConnect: (ManualSerialDevice) -> Unit,
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
    /** ROUND 7 (field bug 1): where the trim in force came from, and how old it is. */
    mountTrimProvenance: com.lidarscan.core.calib.MountTrimProvenance? = null,
    /** ROUND 7 (field bug 2): non-null while a running capture is receiving nothing. */
    noDataAlert: String? = null,
    onDismissNoDataAlert: () -> Unit = {},
    /** ROUND 7 (item 3): non-null once ARCore's frame has jumped and the scan is in sections. */
    sectionHint: String? = null,
    onSetMountReference: () -> Unit = {},
    onClearMountReference: () -> Unit = {},
    onDismissMountTrimNote: () -> Unit = {},
    nowMillis: Long = 0L,
) {
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    var sheet by remember { mutableStateOf(CaptureSheet.NONE) }
    val settingsSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val diagSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    val project = (uiState as? CaptureUiState.Loaded)?.project
    val newScan = uiState as? CaptureUiState.NewScan
    val connected = connectionState == ConnectionState.CONNECTED
    val recording = captureState == CaptureState.RECORDING
    val paused = captureState == CaptureState.PAUSED
    val live = recording || paused
    val poseState = poseTrackingState(
        required = poseTrackingRequired,
        arAvailable = arAvailable,
        sessionRunning = arSessionRunning,
        tracking = arTracking,
        posesPushed = posesPushed,
        lossEpisodes = arTrackingLossEpisodes,
    )

    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
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
                    color = if (connected) {
                        if (sensor == SensorType.MID360) PoseBlue else ScanTeal
                    } else {
                        null
                    },
                    showDot = true,
                )
                Spacer(Modifier.width(8.dp))
            },
        )

        FixChipStrip(fix = fix, ntrip = ntrip, georefSource = georefSource)

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
                    val body: @Composable () -> Unit = {
                        StatPanel(
                            stats = listOf(
                                Stat(formatRate(stats.pointsPerSecond), "pts / s"),
                                // testTag: the CI emulator smoke test polls this
                                // node to assert the decoded point count grows
                                // during a replay session. It stays a plain grouped
                                // integer under a million so the assertion keeps
                                // seeing motion at replay-sized counts — see
                                // formatPoints.
                                Stat(formatPoints(stats.pointsCaptured), "points", testTag = "pointsCapturedValue"),
                                Stat(formatDuration(stats.elapsedMillis), "rec"),
                                Stat(formatMegabytes(stats.recordingSizeBytes), ".lscan"),
                            ),
                            modifier = Modifier.padding(horizontal = 14.dp),
                        )
                        Spacer(Modifier.height(12.dp))
                        TransportRow(
                            captureState = captureState,
                            connected = connected,
                            liveView = liveView,
                            isReplaySession = isReplaySession,
                            pauseSupported = !isReplaySession && sensor != SensorType.MID360,
                            onLiveViewChange = onLiveViewChange,
                            onStart = onStart,
                            onPause = onPause,
                            onResume = onResume,
                            onStop = onStop,
                        )
                    }

                    val viewport: @Composable (Modifier) -> Unit = { modifier ->
                        CaptureViewport(
                            modifier = modifier,
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
                            onRefreshAutoDownshift = onRefreshAutoDownshift,
                            onOpenSettings = { sheet = CaptureSheet.SETTINGS },
                            onOpenDiagnostics = { sheet = CaptureSheet.DIAGNOSTICS },
                            onCameraModeChange = onCameraModeChange,
                        )
                    }

                    // ROUND 5: everything that used to be a wizard, inline and
                    // collapsible — and gone entirely once recording starts.
                    val preCapture: @Composable () -> Unit = {
                        // ROUND 6 (owner item 20): the loudest thing on this
                        // screen, and it stays until dismissed. A capture that
                        // did not save must never be quieter than one that did.
                        if (saveError != null) {
                            SaveErrorBanner(saveError, onDismissSaveError)
                        }
                        // ROUND 7 (field bug 2): the scan is running and nothing
                        // is arriving. This lives OUTSIDE the collapsible part of
                        // the strip on purpose — it has to be readable at the
                        // exact moment the rest of the pre-capture UI is gone,
                        // which is while recording.
                        if (noDataAlert != null) {
                            LoudBanner(
                                title = "NO SENSOR DATA",
                                message = noDataAlert,
                                onDismiss = onDismissNoDataAlert,
                                testTag = "noDataBanner",
                            )
                        }
                        // ROUND 6 (owner item 22): the three chips. Above the
                        // scrolling strip and present during a recording too —
                        // switching mid-walk is a live-view decision, and the
                        // recording is never touched by it.
                        PresetChipRow(
                            preset = preset,
                            deviceTierLabel = deviceTierLabel,
                            onPresetChange = onPresetChange,
                        )
                        if (presetChangeNote != null) {
                            Hint(
                                presetChangeNote,
                                color = ScanTeal,
                                modifier = Modifier
                                    .padding(horizontal = 14.dp, vertical = 2.dp)
                                    .clickable(onClick = onDismissPresetNote)
                                    .testTag("presetChangeNote"),
                            )
                        }
                        if (presetCaution != null) {
                            Hint(
                                presetCaution,
                                color = SemWarn,
                                modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                    .testTag("presetCaution"),
                            )
                        }
                        if (!live && !isReplaySession) {
                            // Capped and scrollable: with the manual panel open the
                            // strip would otherwise squeeze the live viewport to a
                            // sliver — seen on a booted emulator, which is exactly
                            // the failure mode round 5 is trying to avoid (the
                            // points ARE the proof, so they must stay on screen).
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
                        }
                        // ROUND 5.2 / 5.3: the two quiet inline lines. Both are
                        // one sentence, both clear themselves, and neither is ever
                        // a dialog — mid-walk, a modal is the worst possible
                        // interruption.
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
                        // ROUND 6 (owner item 19): the AR path failed. One
                        // sentence, inline, and the view has already fallen back
                        // to 3D orbit — the app does not die and the capture
                        // keeps running.
                        if (arErrorMessage != null) {
                            Hint(
                                "Phone tracking degraded — $arErrorMessage. The recording is unaffected; a COIN-D6 needs tracking to build 3D, so stop and start again if this persists.",
                                color = SemWarn,
                                modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                    .testTag("arUnavailableNote"),
                            )
                        }
                        // ROUND 6 (owner item 21): the live page store filled and
                        // the map stopped growing. Says which half of the app it
                        // costs, because the answer is "the preview, not the scan".
                        // ROUND 7 (item 3): a tracking jump split the scan. Not a
                        // banner — the capture is still good and still recording
                        // — but it must be visible while the operator is still in
                        // the room and can walk the seam again.
                        if (sectionHint != null) {
                            Hint(
                                sectionHint,
                                color = SemWarn,
                                modifier = Modifier
                                    .padding(horizontal = 14.dp, vertical = 2.dp)
                                    .testTag("sectionHint"),
                            )
                        }
                        if (liveMapFullNote != null) {
                            Hint(
                                liveMapFullNote,
                                color = SemWarn,
                                modifier = Modifier.padding(horizontal = 14.dp, vertical = 2.dp)
                                    .testTag("liveMapFullNote"),
                            )
                        }
                        // ROUND 6 (owner item 23): the re-zero's own verdict.
                        if (mountTrimNote != null) {
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
                                preCapture()
                                body()
                            }
                        }
                    } else {
                        Column(Modifier.fillMaxSize()) {
                            preCapture()
                            // The hero cloud takes every pixel the rest of the
                            // column does not claim, with a floor so the inline
                            // manual panel can never squeeze it to nothing.
                            viewport(
                                Modifier.fillMaxWidth().weight(1f).heightIn(min = 140.dp)
                                    .padding(horizontal = 14.dp),
                            )
                            Spacer(Modifier.height(12.dp))
                            body()
                            Spacer(Modifier.height(ScanDims.TabBarClearance))
                        }
                    }
                }
            }
        }
    }

    // ── the two sheets, mutually exclusive by construction ──────────────────
    when (sheet) {
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
            refreshHz = refreshHz,
            // ROUND 5.3 (item 17): the ceiling is the panel's, read from the
            // display this composable is actually on.
            refreshOptions = com.lidarscan.core.render.RefreshGovernor.optionsFor(
                com.lidarscan.app.render.displayRefreshCeilingHz(androidx.compose.ui.platform.LocalContext.current),
            ),
            gamma = gamma,
            brightness = brightness,
            liveSlam = liveSlam,
            liveSlamEditable = !live && connected,
            liveMapEnabled = liveMapEnabled,
            onLiveMapEnabledChange = onLiveMapEnabledChange,
            // The profile is only settable while the project does not exist yet —
            // afterwards it is a property of the .lscan, not a control.
            profile = if (project == null && !isReplaySession) profile else null,
            onRefreshHzChange = onRefreshHzChange,
            onGammaChange = onGammaChange,
            onBrightnessChange = onBrightnessChange,
            onLiveSlamChange = onLiveSlamChange,
            onProfileChange = onProfileChange,
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
        ) {
            // ROUND 6 (item 20): the summary now answers "is it saved?" rather
            // than only "how many points?" — the previous sheet was perfectly
            // confident about a capture that had vanished.
            SessionSummaryContent(sessionSummary, savedPath = lastSavedProject, saveError = saveError, onDismissSummary)
        }
    }
}

// ── ROUND 6: performance presets + the save-failure banner ──────────────────

/**
 * Owner item 22's **fast select**: Light / Optimal / Full, on the capture screen
 * itself rather than three taps into a sheet.
 *
 * Deliberately *above* the collapsible pre-capture strip and present during a
 * recording as well as before one: switching preset is a live-view decision, the
 * recording is never touched by it, and mid-walk ("this is getting hot") is
 * exactly when an operator wants Light.
 *
 * The row shows [com.lidarscan.core.capture.PerformancePreset.CUSTOM] as a
 * fourth, non-tappable state once an individual parameter has been moved in the
 * settings sheet — item 22's "keep the full parameter for advance user setting"
 * means an edit must survive, not be snapped back to whichever chip is lit.
 */
@Composable
private fun PresetChipRow(
    preset: com.lidarscan.core.capture.PerformancePreset,
    deviceTierLabel: String,
    onPresetChange: (com.lidarscan.core.capture.PerformancePreset) -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 4.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("PERFORMANCE", style = MonoLabel, color = InkFaint)
            Spacer(Modifier.width(10.dp))
            // Weighted + ellipsised on THIS side: the tagline is the variable
            // half, so it is the half that must be allowed to shrink. Giving
            // the fixed label the weight instead let a long tagline overrun it.
            Text(
                if (preset == com.lidarscan.core.capture.PerformancePreset.CUSTOM) {
                    "custom · $deviceTierLabel phone"
                } else {
                    "${preset.tagline} · $deviceTierLabel phone"
                },
                style = MonoLabel,
                color = InkFaint,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = androidx.compose.ui.text.style.TextAlign.End,
                modifier = Modifier.weight(1f).testTag("presetReadout"),
            )
        }
        Spacer(Modifier.height(4.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            com.lidarscan.core.capture.PerformancePreset.entries
                .filter { it.isSelectable }
                .forEach { option ->
                    val selected = option == preset
                    Box(
                        Modifier
                            .weight(1f)
                            .height(ScanDims.Touch)
                            .background(
                                if (selected) Ember else MaterialTheme.colorScheme.surfaceContainer,
                                RoundedCornerShape(50),
                            )
                            .border(
                                1.dp,
                                if (selected) Ember else MaterialTheme.colorScheme.outline,
                                RoundedCornerShape(50),
                            )
                            .clickable(role = Role.RadioButton) { onPresetChange(option) }
                            .semantics { contentDescription = "${option.displayName} performance preset" }
                            .testTag("preset${option.name}"),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            option.displayName,
                            fontFamily = DisplayFontFamily,
                            fontWeight = FontWeight.SemiBold,
                            fontSize = 14.sp,
                            color = if (selected) OnEmber else MaterialTheme.colorScheme.onSurface,
                        )
                    }
                }
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
 * The one shape on this screen that is allowed to shout: a red-bordered block
 * at the top of the capture body, not a `Hint` and not a toast, that stays
 * until it is tapped.
 *
 * ROUND 6 introduced it for "the capture did not save". ROUND 7 gives it a
 * second job — "the capture is running and receiving nothing" — because those
 * are the two failures that used to be silent, and silence is what cost the
 * owner two field sessions.
 */
@Composable
private fun LoudBanner(title: String, message: String, onDismiss: () -> Unit, testTag: String) {
    val shape = RoundedCornerShape(ScanDims.TileRadius)
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 14.dp, vertical = 6.dp)
            .background(SemBad.copy(alpha = 0.14f), shape)
            .border(1.dp, SemBad, shape)
            .clickable(onClick = onDismiss)
            .padding(12.dp)
            .testTag(testTag),
    ) {
        Text(
            title,
            style = MonoLabel,
            color = SemBad,
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
    onManualDeviceConnect: (ManualSerialDevice) -> Unit,
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

        // ROUND 5 item 11: the mount hint. Short, and only for the sensor it
        // applies to — the D6's geometry is the whole reason the capture is 3D.
        if (poseTrackingRequired && sensor == SensorType.COIN_D6) {
            Spacer(Modifier.height(4.dp))
            Hint(
                "Mount the D6 flat on the BACK of the phone with its scan fan VERTICAL, then walk forward — " +
                    "the phone's camera + IMU supply the 6-DoF path and the engine sweeps the fan into 3D.",
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
                    "Hold the rig still in the pose you will walk with, then tap — the D6's angle on the " +
                        "phone is measured from the phone's own attitude and applied to this scan."
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
                        "Without phone tracking the D6 can only record flat fan slices — grant the camera " +
                            "permission (or install ARCore) for a 3D scan.",
                        color = SemWarn,
                    )
                }

            }
        }
        Spacer(Modifier.height(6.dp))
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
 * attached serial ports for the D6, and the two addresses for the Mid-360.
 *
 * A panel on the screen, never a dialog (item 7). Its own height is capped and it
 * scrolls, so a rig with six serial devices cannot push the live viewport off the
 * bottom of the screen.
 */
@Composable
private fun ManualEntryPanel(
    devices: List<ManualSerialDevice>,
    lidarIp: String,
    hostIp: String,
    busy: Boolean,
    onDeviceConnect: (ManualSerialDevice) -> Unit,
    onLidarIpChange: (String) -> Unit,
    onHostIpChange: (String) -> Unit,
    onMid360Connect: () -> Unit,
) {
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
        Text("COIN-D6 · USB", style = MonoLabel, color = Ember)
        Spacer(Modifier.height(6.dp))
        if (devices.isEmpty()) {
            Hint("No serial device attached. Plug the D6 into USB-C OTG — it appears here as soon as it does.")
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
                        onClick = { onDeviceConnect(device) },
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
            "No self-test step: the live view above is the proof. If points appear, the device works.",
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
    onRefreshAutoDownshift: (Int) -> Unit,
    onOpenSettings: () -> Unit,
    onOpenDiagnostics: () -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
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
                onRendererReady = { pointCloudRenderer = it },
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
                modifier = Modifier.align(Alignment.BottomCenter)
                    .padding(bottom = 10.dp)
                    .size(width = 108.dp, height = 84.dp),
            )
        }

        // ROUND 5 (item 11): the pose pump, when poses are needed and the AR
        // overlay (which pumps ARCore itself) is not the renderer on screen.
        // Two pumps would call Session.update() from two threads, so this is an
        // either/or by construction.
        if (poseTrackingRequired && arAvailable && cameraMode != CameraMode.AR) {
            arPosePump(Modifier.align(Alignment.BottomStart).padding(start = 2.dp, bottom = 2.dp))
        }

        // ── top-left: the keyframe counter ──────────────────────────────
        //
        // It rides the TOP band deliberately (round 3's one documented
        // departure): the Capture-settings sheet covers the lower ~74 %, so a
        // bottom-corner chip would be hidden by the very sheet whose switch
        // controls it.
        if (recording && keyframesEnabled) {
            ScanChip(
                text = "KF $keyframesWritten",
                modifier = Modifier.align(Alignment.TopStart).padding(12.dp).testTag("keyframeChip"),
            )
        }

        // ── top-right: orbit / follow ───────────────────────────────────
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

        // ── the 48 dp Display button: the single entry point for settings ─
        Box(
            Modifier
                .align(Alignment.CenterEnd)
                .padding(end = 14.dp)
                .size(48.dp)
                .shadow(8.dp, CircleShape)
                .background(MaterialTheme.colorScheme.surfaceContainer, CircleShape)
                .border(1.dp, MaterialTheme.colorScheme.outline, CircleShape)
                .clickable(role = Role.Button, onClick = onOpenSettings)
                .semantics { contentDescription = "Capture settings" }
                .testTag("captureSettingsButton"),
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Filled.Tune, contentDescription = null, tint = MaterialTheme.colorScheme.onSurface)
        }
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
                    drawLine(
                        color = if (previousTracking && p.tracking) ScanTeal else SemBad.copy(alpha = 0.5f),
                        start = from,
                        end = here,
                        strokeWidth = 3f,
                        cap = androidx.compose.ui.graphics.StrokeCap.Round,
                    )
                }
                previous = here
                previousTracking = p.tracking
            }
            // Where you are NOW — the one thing worth finding at a glance.
            previous?.let { drawCircle(color = Ember, radius = 4.5f, center = it) }
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
    onLiveViewChange: (Boolean) -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
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
            }
            Spacer(Modifier.height(2.dp))
            Text(
                "display only · recording unaffected",
                style = MonoLabel.copy(fontSize = 9.5.sp, letterSpacing = 0.06.em),
                color = InkFaint,
            )
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
        val recordLabel = when {
            live -> "Stop recording"
            isReplaySession -> "Start replay"
            else -> "Start new scan"
        }
        Box(
            Modifier
                .size(if (live) 76.dp else 64.dp)
                .alpha(if (connected && !stopping) 1f else 0.45f)
                .shadow(16.dp, CircleShape, ambientColor = Ember, spotColor = Ember)
                .background(Ember, CircleShape)
                .clickable(enabled = connected && !stopping, role = Role.Button) {
                    if (live) onStop() else onStart()
                }
                .semantics { contentDescription = recordLabel }
                .testTag("recordButton"),
            contentAlignment = Alignment.Center,
        ) {
            // A filled circle while idle, a square while live — the universal
            // record/stop pair, drawn rather than iconified so the ember ring
            // reads as one control.
            Box(
                Modifier
                    .size(if (live) 30.dp else 26.dp)
                    .background(OnEmber, if (live) RoundedCornerShape(7.dp) else CircleShape),
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
    savedPath: String?,
    saveError: String?,
    onDismiss: () -> Unit,
) {
    Column(Modifier.padding(horizontal = 22.dp, vertical = 8.dp)) {
        Text(
            if (saveError != null) "Session ended — NOT saved" else "Session summary",
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 22.sp,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.height(16.dp))
        StatPanel(
            listOf(
                Stat(formatPoints(summary.pointsCaptured), "points"),
                Stat(formatDuration(summary.elapsedMillis), "duration"),
                Stat(formatMegabytes(summary.recordingSizeBytes), ".lscan"),
                Stat(
                    formatRate(
                        if (summary.elapsedMillis > 0) summary.pointsCaptured * 1000.0 / summary.elapsedMillis else 0.0,
                    ),
                    "avg pts/s",
                ),
            ),
        )
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
        PrimaryPill(text = "Done", onClick = onDismiss, modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(20.dp))
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
        imu = when (sensor) {
            SensorType.COIN_D6 -> "none on device · phone IMU via ARCore"
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
