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
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
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
import com.lidarscan.app.ui.theme.OnEmber
import com.lidarscan.app.ui.theme.PoseBlue
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.app.ui.theme.SemBad
import com.lidarscan.app.ui.theme.SemGood
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.app.ui.theme.ViewportGround
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.gnss.GnssFixSnapshot
import com.lidarscan.core.gnss.NtripState
import com.lidarscan.core.gnss.NtripStatsSnapshot
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap

@Composable
fun CaptureRoute(
    container: AppContainer,
    projectId: String,
    isReplay: Boolean = false,
    onBack: () -> Unit,
    onConnectDevice: () -> Unit,
    /** B3: opens the Mid-360 wizard for this project. No-op default keeps the replay route unchanged. */
    onOpenMid360Connect: () -> Unit = {},
) {
    val viewModel: CaptureViewModel = viewModel(
        key = "capture-$projectId-$isReplay",
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
    val keyframeStats by viewModel.keyframeStats.collectAsStateWithLifecycle()
    val keyframesEnabled by viewModel.keyframesEnabled.collectAsStateWithLifecycle()
    val keyframeRateFps by viewModel.keyframeRateFps.collectAsStateWithLifecycle()
    val lodBudgetMPoints by viewModel.lodBudgetMPoints.collectAsStateWithLifecycle()
    val displayParams by viewModel.displayParams.collectAsStateWithLifecycle()
    val sensor by viewModel.sensor.collectAsStateWithLifecycle()
    val mid360Endpoint by viewModel.mid360Endpoint.collectAsStateWithLifecycle()
    val arStatus = viewModel.arStatus?.collectAsStateWithLifecycle()?.value

    // B9: the fix strip. A replay session has no rover, so it shows the same
    // "no fix" chips a disconnected one does — which is the truth, not a gap.
    val fix by container.rtkManager.fix.collectAsStateWithLifecycle()
    val ntrip by container.rtkManager.ntrip.collectAsStateWithLifecycle()

    // B7: the AR session belongs to the whole app (one ARCore Session per
    // process), so the Capture screen resumes and pauses it around its own
    // lifetime rather than creating one.
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) container.arController.createSession() }

    LaunchedEffect(viewModel.arAvailable) {
        if (!viewModel.arAvailable) return@LaunchedEffect
        container.arController.refreshAvailability()
        if (!container.hasCameraPermission()) {
            permissionLauncher.launch(android.Manifest.permission.CAMERA)
        } else {
            container.arController.createSession()
            container.arController.resume()
        }
    }
    DisposableEffect(viewModel.arAvailable) {
        onDispose { if (viewModel.arAvailable) container.arController.pause() }
    }

    CaptureScreen(
        uiState = uiState,
        connectionState = connectionState,
        captureState = captureState,
        stats = stats,
        health = health,
        fix = fix,
        ntrip = ntrip,
        sessionSummary = sessionSummary,
        pointCloudSource = pointCloudSource,
        colorMode = colorMode,
        colormap = colormap,
        pointSizePx = pointSizePx,
        lodBudgetMPoints = lodBudgetMPoints,
        displayParams = displayParams,
        cameraMode = cameraMode,
        liveSlam = liveSlam,
        isReplaySession = viewModel.isReplaySession,
        arAvailable = viewModel.arAvailable,
        arTracking = arStatus?.tracking == true,
        arTrackingHint = arStatus?.trackingHint,
        arTrackingLossEpisodes = arStatus?.trackingLossEpisodes ?: 0,
        keyframeStats = keyframeStats,
        keyframesEnabled = keyframesEnabled,
        keyframeRateFps = keyframeRateFps,
        sensor = sensor,
        mid360Endpoint = mid360Endpoint,
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
        onBack = onBack,
        onConnectDevice = onConnectDevice,
        onConnectMid360 = {
            // Not connected yet and no saved endpoint -> the wizard; saved
            // endpoint -> just connect. Two very different actions behind one
            // button, chosen by what the project actually has.
            if (mid360Endpoint == null) onOpenMid360Connect() else viewModel.connectMid360()
        },
        onStart = viewModel::startCapture,
        onPause = viewModel::pauseCapture,
        onResume = viewModel::resumeCapture,
        onStop = viewModel::stopCapture,
        onDismissSummary = viewModel::dismissSessionSummary,
        onLiveSlamChange = viewModel::setLiveSlam,
        onColorModeChange = viewModel::setColorMode,
        onColormapChange = viewModel::setColormap,
        onPointSizeChange = viewModel::setPointSizePx,
        onLodChange = viewModel::setLodBudgetMPoints,
        onCameraModeChange = viewModel::setCameraMode,
        onKeyframesEnabledChange = viewModel::setKeyframesEnabled,
        onKeyframeRateChange = viewModel::setKeyframeRateFps,
    )
}

/**
 * The redesign's Capture screen (mockup v7).
 *
 * Read top to bottom it is: app bar → RTK chip strip → **hero viewport (~50 %
 * of the screen)** → four mono stats → transport. Nothing else. The five-row
 * "AR + camera keyframes" telemetry block that used to sit under the stats is
 * gone from the body entirely — it lives in the Diagnostics sheet behind the
 * viewport's health chip (round 3's item 4), and the height it freed went to
 * the live cloud and to the record cluster's clearance over the tab bar.
 *
 * Two doors on the viewport, and only ever one open at a time:
 *  * the **48 dp Display button** opens Capture settings (view + AR/camera +
 *    display), which live-applies to the view behind it;
 *  * the **health chip**, hung on a 44 dp hit target, opens Diagnostics.
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
    sessionSummary: CaptureStats?,
    pointCloudSource: PointCloudSource?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    lodBudgetMPoints: Int,
    displayParams: com.lidarscan.core.render.DisplayParams,
    cameraMode: CameraMode,
    liveSlam: Boolean,
    isReplaySession: Boolean,
    arAvailable: Boolean,
    arTracking: Boolean,
    arTrackingHint: String?,
    arTrackingLossEpisodes: Int,
    keyframeStats: com.lidarscan.app.ar.KeyframeRecorder.Stats,
    keyframesEnabled: Boolean,
    keyframeRateFps: Int,
    /**
     * B3: this project's sensor. Two things depend on it — which connect
     * wizard the Connect button opens, and whether Pause is offered at all
     * (a Mid-360 cannot pause; see RealEngineBridge.pauseCapture for why
     * faking it would truncate the recording).
     */
    sensor: SensorType,
    /** B3: `"<lidarIp>|<hostIp>"` saved in the manifest, or null if the wizard has not been run. */
    mid360Endpoint: String?,
    arOverlay: @Composable (Modifier) -> Unit,
    onBack: () -> Unit,
    onConnectDevice: () -> Unit,
    /** B3: connects the engine to the saved Mid-360 endpoint (no USB permission dance to run first). */
    onConnectMid360: () -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
    onDismissSummary: () -> Unit,
    onLiveSlamChange: (Boolean) -> Unit,
    onColorModeChange: (ColorMode) -> Unit,
    onColormapChange: (Colormap) -> Unit,
    onPointSizeChange: (Float) -> Unit,
    onLodChange: (Int) -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
    onKeyframesEnabledChange: (Boolean) -> Unit,
    onKeyframeRateChange: (Int) -> Unit,
) {
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    var sheet by remember { mutableStateOf(CaptureSheet.NONE) }
    val settingsSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val diagSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    val project = (uiState as? CaptureUiState.Loaded)?.project
    val connected = connectionState == ConnectionState.CONNECTED
    val recording = captureState == CaptureState.RECORDING

    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        BackBar(
            title = project?.manifest?.name ?: if (isReplaySession) "Capture — Replay" else "Capture",
            subtitle = project?.let {
                "${it.manifest.profile.displayName} · ${captureStateLabel(captureState)}"
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

        FixChipStrip(fix = fix, ntrip = ntrip)

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

                is CaptureUiState.Loaded -> {
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
                            liveSlam = liveSlam,
                            isReplaySession = isReplaySession,
                            pauseSupported = !isReplaySession && sensor != SensorType.MID360,
                            onLiveSlamChange = onLiveSlamChange,
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
                            source = pointCloudSource,
                            colorMode = colorMode,
                            colormap = colormap,
                            pointSizePx = pointSizePx,
                            displayParams = displayParams,
                            cameraMode = cameraMode,
                            liveSlam = liveSlam,
                            recording = recording,
                            sensor = sensor,
                            arAvailable = arAvailable,
                            arTrackingHint = arTrackingHint,
                            arOverlay = arOverlay,
                            health = health,
                            keyframesEnabled = keyframesEnabled,
                            keyframesWritten = keyframeStats.keyframesWritten,
                            onOpenSettings = { sheet = CaptureSheet.SETTINGS },
                            onOpenDiagnostics = { sheet = CaptureSheet.DIAGNOSTICS },
                            onCameraModeChange = onCameraModeChange,
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
                                if (!connected && !isReplaySession) {
                                    ConnectPrompt(sensor, mid360Endpoint, onConnectDevice, onConnectMid360)
                                }
                                body()
                            }
                        }
                    } else {
                        Column(Modifier.fillMaxSize()) {
                            if (!connected && !isReplaySession) {
                                ConnectPrompt(sensor, mid360Endpoint, onConnectDevice, onConnectMid360)
                            }
                            // ~50 % of the screen, and it takes every pixel the
                            // rest of the column does not claim — the reclaimed
                            // telemetry block went here.
                            viewport(Modifier.fillMaxWidth().weight(1f).padding(horizontal = 14.dp))
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
            arTrackingLabel = arTrackingLabel(arAvailable, arTracking, cameraMode, keyframesEnabled),
            arTrackingIsGood = arTracking,
            keyframesEnabled = keyframesEnabled,
            keyframeRateFps = keyframeRateFps,
            colorMode = colorMode,
            colormap = colormap,
            pointSizePx = pointSizePx,
            lodBudgetMPoints = lodBudgetMPoints,
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
            SessionSummaryContent(sessionSummary, onDismissSummary)
        }
    }
}

// ── viewport ────────────────────────────────────────────────────────────────

@Composable
private fun CaptureViewport(
    modifier: Modifier,
    connected: Boolean,
    isReplaySession: Boolean,
    source: PointCloudSource?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    displayParams: com.lidarscan.core.render.DisplayParams,
    cameraMode: CameraMode,
    liveSlam: Boolean,
    recording: Boolean,
    sensor: SensorType,
    arAvailable: Boolean,
    arTrackingHint: String?,
    arOverlay: @Composable (Modifier) -> Unit,
    health: DeviceHealth?,
    keyframesEnabled: Boolean,
    keyframesWritten: Int,
    onOpenSettings: () -> Unit,
    onOpenDiagnostics: () -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
) {
    val shape = RoundedCornerShape(ScanDims.CardRadius)
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
        if (cameraMode == CameraMode.AR && arAvailable) {
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
                streamFilter = StreamFilter.forSession(liveSlam),
                // Redesign: the sheet's LOD slider needs a live path into the
                // renderer, and lodPointBudget only travels inside
                // DisplayParams. Passing the block also owns colour and point
                // size, which is why they are assembled together in the VM.
                displayParams = displayParams,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(
                    if (connected) {
                        "3D view needs the real engine (simulated-engine build)"
                    } else if (isReplaySession) {
                        "Starting the replay engine…"
                    } else {
                        "Connect a device to see the live 3D view"
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(28.dp),
                )
            }
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

        // ── AR tracking hint, when the AR renderer is up ────────────────
        if (cameraMode == CameraMode.AR && arTrackingHint != null) {
            ScanChip(
                text = arTrackingHint,
                color = SemWarn,
                modifier = Modifier.align(Alignment.TopCenter).padding(top = 52.dp),
            )
        }

        // ── bottom-left: what stream is on screen ───────────────────────
        ScanChip(
            text = if (liveSlam) "LIVE MAP · SLAM" else "RAW · ${sensor.badgeLabel.uppercase()}",
            color = PoseBlue,
            showDot = true,
            modifier = Modifier.align(Alignment.BottomStart).padding(12.dp),
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

// ── transport ───────────────────────────────────────────────────────────────

/**
 * Live-SLAM switch on the left, pause circle and the 64 dp ember record button
 * on the right. This is the last thing on the screen, so it carries the tab
 * bar's clearance the removed telemetry body used to provide.
 *
 * The record button's `contentDescription` is what names the action for
 * accessibility *and* for the emulator smoke test — the button itself is a
 * circle with no text, exactly as designed.
 */
@Composable
private fun TransportRow(
    captureState: CaptureState,
    connected: Boolean,
    liveSlam: Boolean,
    isReplaySession: Boolean,
    pauseSupported: Boolean,
    onLiveSlamChange: (Boolean) -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
) {
    val idle = captureState == CaptureState.IDLE
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
                    checked = liveSlam,
                    // §3.1's toggle binds scan_session_config.live_slam, which
                    // is read once when the session starts — so it is editable
                    // while idle and locked during a session, and the caption
                    // says which.
                    enabled = idle && connected,
                    onCheckedChange = onLiveSlamChange,
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = Color.White,
                        checkedTrackColor = Ember,
                        checkedBorderColor = Ember,
                    ),
                    modifier = Modifier.testTag("liveSlamSwitch"),
                )
                Spacer(Modifier.width(10.dp))
                Text(
                    if (liveSlam) "Live SLAM" else "Record-only",
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
                if (idle) "editable while idle" else "locked during a session",
                style = MonoLabel.copy(fontSize = 9.5.sp, letterSpacing = 0.06.em),
                color = InkFaint,
            )
        }

        // Pause: offered only where it actually works. Replay has no pause hook
        // in ReplaySource (B4) and a Mid-360 cannot pause without truncating the
        // recording on resume (B2/B3) — so it is present and dimmed rather than
        // absent, which keeps the transport's shape stable across sensors.
        val pauseEnabled = live && pauseSupported && !stopping
        Box(
            Modifier
                .size(52.dp)
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

        val recordLabel = when {
            live -> "Stop recording"
            isReplaySession -> "Start replay"
            else -> "Start recording"
        }
        Box(
            Modifier
                .size(64.dp)
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
                    .size(if (live) 24.dp else 26.dp)
                    .background(OnEmber, if (live) RoundedCornerShape(7.dp) else CircleShape),
            )
        }
    }
}

// ── the RTK chip strip ──────────────────────────────────────────────────────

@Composable
private fun FixChipStrip(fix: GnssFixSnapshot, ntrip: NtripStatsSnapshot) {
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 14.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        ScanChip(
            text = fixChipText(fix),
            color = if (fix.hasFix) fixColor(fix.fix) else SemBad,
            showDot = true,
        )
        ScanChip(text = "NTRIP ${ntrip.state.name}")
        ScanChip(
            text = if (ntrip.receiving) "CORRECTIONS LIVE" else "NO RTCM",
            color = if (ntrip.receiving) SemGood else null,
        )
        if (fix.hasFix) {
            ScanChip(text = "${fix.satellites} SATS")
        }
    }
}

// ── connect prompt (unchanged behaviour, restyled) ──────────────────────────

@Composable
private fun ConnectPrompt(
    sensor: SensorType,
    mid360Endpoint: String?,
    onConnectDevice: () -> Unit,
    onConnectMid360: () -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 4.dp)) {
        Hint(
            when {
                sensor == SensorType.MID360 && mid360Endpoint == null ->
                    "Run the Mid-360 connect wizard first — capture needs both a lidar IP and a host IP, " +
                        "and neither has a safe default."
                sensor == SensorType.MID360 -> "Connect the Mid-360 ($mid360Endpoint) to start recording."
                else -> "Connect a D6 to start recording."
            },
        )
        Spacer(Modifier.height(8.dp))
        SecondaryPill(
            text = if (sensor == SensorType.MID360 && mid360Endpoint == null) "Set up Mid-360" else "Connect",
            height = 46.dp,
            onClick = if (sensor == SensorType.MID360) onConnectMid360 else onConnectDevice,
            modifier = Modifier.testTag("connectDeviceButton"),
        )
        Spacer(Modifier.height(6.dp))
    }
}

// ── session summary ─────────────────────────────────────────────────────────

@Composable
private fun SessionSummaryContent(summary: CaptureStats, onDismiss: () -> Unit) {
    Column(Modifier.padding(horizontal = 22.dp, vertical = 8.dp)) {
        Text(
            "Session summary",
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
        Spacer(Modifier.height(22.dp))
        PrimaryPill(text = "Done", onClick = onDismiss, modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(20.dp))
    }
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

/**
 * The fix chip's own short form. `accuracyText()` is a full sentence built for
 * the RTK screen's strip ("±1.9 cm horizontal (measured, GST)") — correct
 * there, far too long for a chip that shares a scrolling row with three
 * others, so the chip keeps the number and drops the provenance. Tapping
 * through to the RTK screen is where the sentence still lives.
 */
private fun fixChipText(fix: GnssFixSnapshot): String {
    if (!fix.hasFix || fix.fix == com.lidarscan.core.gnss.FixType.NONE) return "NO FIX · no accuracy"
    val sigma = fix.sigmaHorizontalM
    val accuracy = when {
        sigma <= 0f -> "accuracy unknown"
        sigma < 1f -> "%.1f cm".format(sigma * 100)
        else -> "%.2f m".format(sigma)
    }
    return "${fix.fix.label.uppercase()} · $accuracy"
}

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
 * `off` when the ARCore session has no reason to run at all (neither the AR
 * view nor keyframes), which is the mockup's own rule.
 */
private fun arTrackingLabel(
    arAvailable: Boolean,
    tracking: Boolean,
    cameraMode: CameraMode,
    keyframesEnabled: Boolean,
): String = when {
    !arAvailable -> "unavailable"
    cameraMode != CameraMode.AR && !keyframesEnabled -> "off"
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
        imu = sensor.badgeLabel.uppercase(),
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
): ArDiagnostics {
    val label = arTrackingLabel(arAvailable, arTracking, cameraMode, keyframesEnabled)
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
    )
}
