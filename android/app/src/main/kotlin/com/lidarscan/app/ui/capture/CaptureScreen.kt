package com.lidarscan.app.ui.capture

import android.content.res.Configuration
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
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
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.engine.DeviceHealth
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
    val mountCalibration by viewModel.mountCalibrationApplied.collectAsStateWithLifecycle()
    val sensor by viewModel.sensor.collectAsStateWithLifecycle()
    val mid360Endpoint by viewModel.mid360Endpoint.collectAsStateWithLifecycle()
    val arStatus = viewModel.arStatus?.collectAsStateWithLifecycle()?.value

    // B7: the AR session belongs to the whole app (one ARCore Session per
    // process), so the Capture screen resumes and pauses it around its own
    // lifetime rather than creating one.
    val context = LocalContext.current
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
        sessionSummary = sessionSummary,
        pointCloudSource = pointCloudSource,
        colorMode = colorMode,
        colormap = colormap,
        pointSizePx = pointSizePx,
        cameraMode = cameraMode,
        liveSlam = liveSlam,
        isReplaySession = viewModel.isReplaySession,
        arAvailable = viewModel.arAvailable,
        arTrackingHint = arStatus?.trackingHint,
        arPosesPushed = arStatus?.posesPushed ?: 0L,
        arTrackingLossEpisodes = arStatus?.trackingLossEpisodes ?: 0,
        keyframeStats = keyframeStats,
        mountCalibrationHeadline = mountCalibration?.readout()?.headline,
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
        onCameraModeChange = viewModel::setCameraMode,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CaptureScreen(
    uiState: CaptureUiState,
    connectionState: ConnectionState,
    captureState: CaptureState,
    stats: CaptureStats,
    health: DeviceHealth?,
    sessionSummary: CaptureStats?,
    pointCloudSource: PointCloudSource?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    cameraMode: CameraMode,
    liveSlam: Boolean,
    isReplaySession: Boolean,
    arAvailable: Boolean,
    arTrackingHint: String?,
    arPosesPushed: Long,
    arTrackingLossEpisodes: Int,
    keyframeStats: com.lidarscan.app.ar.KeyframeRecorder.Stats,
    mountCalibrationHeadline: String?,
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
    onCameraModeChange: (CameraMode) -> Unit,
) {
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(if (isReplaySession) "Capture — Replay" else "Capture") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        when (uiState) {
            CaptureUiState.Loading -> Box(Modifier.fillMaxSize().padding(padding))
            CaptureUiState.NotFound -> Box(
                modifier = Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center,
            ) { Text("Project not found") }
            is CaptureUiState.Loaded -> {
                val connected = connectionState == ConnectionState.CONNECTED
                if (isLandscape) {
                    Row(modifier = Modifier.fillMaxSize().padding(padding)) {
                        Box(modifier = Modifier.weight(1f).fillMaxHeight()) {
                            Live3DOrPlaceholder(
                                connected, pointCloudSource, colorMode, colormap, pointSizePx,
                                cameraMode, liveSlam, arAvailable, arTrackingHint, arOverlay,
                            )
                        }
                        Column(
                            modifier = Modifier.width(320.dp).fillMaxHeight().verticalScroll(rememberScrollState())
                                .padding(16.dp),
                        ) {
                            CaptureControlsColumn(
                                connectionState, captureState, stats, health, colorMode, colormap, pointSizePx,
                                cameraMode, liveSlam, isReplaySession, arAvailable, arPosesPushed,
                                arTrackingLossEpisodes, keyframeStats, mountCalibrationHeadline,
                                sensor, mid360Endpoint,
                                onConnectDevice, onConnectMid360, onStart, onPause, onResume,
                                onStop, onLiveSlamChange, onColorModeChange, onColormapChange, onPointSizeChange,
                                onCameraModeChange,
                            )
                        }
                    }
                } else {
                    Column(modifier = Modifier.fillMaxSize().padding(padding)) {
                        Box(modifier = Modifier.fillMaxWidth().weight(1f)) {
                            Live3DOrPlaceholder(
                                connected, pointCloudSource, colorMode, colormap, pointSizePx,
                                cameraMode, liveSlam, arAvailable, arTrackingHint, arOverlay,
                            )
                        }
                        Column(modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
                            CaptureControlsColumn(
                                connectionState, captureState, stats, health, colorMode, colormap, pointSizePx,
                                cameraMode, liveSlam, isReplaySession, arAvailable, arPosesPushed,
                                arTrackingLossEpisodes, keyframeStats, mountCalibrationHeadline,
                                sensor, mid360Endpoint,
                                onConnectDevice, onConnectMid360, onStart, onPause, onResume,
                                onStop, onLiveSlamChange, onColorModeChange, onColormapChange, onPointSizeChange,
                                onCameraModeChange,
                            )
                        }
                    }
                }

                if (sessionSummary != null) {
                    ModalBottomSheet(onDismissRequest = onDismissSummary) {
                        SessionSummaryContent(sessionSummary, onDismissSummary)
                    }
                }
            }
        }
    }
}

@Composable
private fun Live3DOrPlaceholder(
    connected: Boolean,
    source: PointCloudSource?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    cameraMode: CameraMode,
    liveSlam: Boolean,
    arAvailable: Boolean,
    arTrackingHint: String?,
    arOverlay: @Composable (Modifier) -> Unit,
) {
    // B7 (§3.7): AR mode is a different RENDERER, not a flag inside one — the
    // AR path stacks a translucent Filament surface over an ARCore camera
    // GLSurfaceView, and the free-orbit path is B4's single opaque surface
    // unchanged. Switching structurally means neither mode carries the
    // other's setup.
    if (cameraMode == CameraMode.AR && arAvailable) {
        Box(Modifier.fillMaxSize()) {
            arOverlay(Modifier.fillMaxSize())
            arTrackingHint?.let { hint ->
                Card(modifier = Modifier.align(Alignment.TopCenter).padding(12.dp)) {
                    Text(hint, Modifier.padding(12.dp), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        return
    }
    if (connected && source != null) {
        PointCloudView(
            source = source,
            colorMode = colorMode,
            colormap = colormap,
            pointSizePx = pointSizePx,
            cameraMode = cameraMode,
            // B3: Record-only draws the sensor-frame stream; Live-SLAM draws
            // the registered map (falling back to raw until the first mapped
            // page exists). Without this a Mid-360 live-SLAM session draws
            // both at once — see StreamFilter.
            streamFilter = StreamFilter.forSession(liveSlam),
            modifier = Modifier.fillMaxSize(),
        )
    } else {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                if (connected) "3D view needs the real engine (simulated-engine build)" else "Connect a device to see the live 3D view",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun CaptureControlsColumn(
    connectionState: ConnectionState,
    captureState: CaptureState,
    stats: CaptureStats,
    health: DeviceHealth?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    cameraMode: CameraMode,
    liveSlam: Boolean,
    isReplaySession: Boolean,
    arAvailable: Boolean,
    arPosesPushed: Long,
    arTrackingLossEpisodes: Int,
    keyframeStats: com.lidarscan.app.ar.KeyframeRecorder.Stats,
    mountCalibrationHeadline: String?,
    sensor: SensorType,
    mid360Endpoint: String?,
    onConnectDevice: () -> Unit,
    onConnectMid360: () -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
    onLiveSlamChange: (Boolean) -> Unit,
    onColorModeChange: (ColorMode) -> Unit,
    onColormapChange: (Colormap) -> Unit,
    onPointSizeChange: (Float) -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
) {
    if (!isReplaySession) {
        ConnectionCard(
            connectionState = connectionState,
            sensor = sensor,
            mid360Endpoint = mid360Endpoint,
            onConnectDevice = if (sensor == SensorType.MID360) onConnectMid360 else onConnectDevice,
        )
        Spacer(Modifier.height(16.dp))
    }

    if (connectionState == ConnectionState.CONNECTED) {
        if (captureState == CaptureState.IDLE) {
            LiveSlamToggle(liveSlam, onLiveSlamChange)
            Spacer(Modifier.height(12.dp))
        }
        // B3: Pause is offered only where it actually works. Replay has no
        // pause hook in ReplaySource (B4), and a Mid-360 cannot pause without
        // truncating the recording on resume (see RealEngineBridge).
        RecordingControls(
            captureState = captureState,
            pauseSupported = !isReplaySession && sensor != SensorType.MID360,
            isReplaySession = isReplaySession,
            onStart = onStart,
            onPause = onPause,
            onResume = onResume,
            onStop = onStop,
        )
        Spacer(Modifier.height(16.dp))
        StatusStripCard(stats, health)
        Spacer(Modifier.height(16.dp))
        if (arAvailable) {
            ArStatusCard(arPosesPushed, arTrackingLossEpisodes, keyframeStats, mountCalibrationHeadline)
            Spacer(Modifier.height(16.dp))
        }
        DisplayControlsCard(
            colorMode, colormap, pointSizePx, cameraMode, arAvailable,
            onColorModeChange, onColormapChange, onPointSizeChange, onCameraModeChange,
        )
    } else if (!isReplaySession) {
        Text(
            if (sensor == SensorType.MID360) {
                if (mid360Endpoint == null) {
                    "Run the Mid-360 connect wizard first — capture needs both a lidar IP and a host IP, and neither has a safe default."
                } else {
                    "Connect the Mid-360 ($mid360Endpoint) to start recording."
                }
            } else {
                "Connect a D6 to start recording."
            },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun ConnectionCard(
    connectionState: ConnectionState,
    sensor: SensorType,
    mid360Endpoint: String?,
    onConnectDevice: () -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.padding(16.dp).fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(sensor.displayName, style = MaterialTheme.typography.titleSmall)
                Text(
                    connectionState.name.lowercase().replaceFirstChar { it.uppercase() } +
                        // B3: naming the endpoint here is the cheapest possible
                        // guard against the Mid-360's silent failure — a capture
                        // that records nothing usually had the wrong host IP,
                        // and this is where that is visible before Start.
                        (if (sensor == SensorType.MID360 && mid360Endpoint != null) " · $mid360Endpoint" else ""),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            OutlinedButton(onClick = onConnectDevice) {
                Text(
                    when {
                        connectionState == ConnectionState.CONNECTED -> "Manage"
                        sensor == SensorType.MID360 && mid360Endpoint == null -> "Set up"
                        else -> "Connect"
                    },
                )
            }
        }
    }
}

@Composable
private fun LiveSlamToggle(liveSlam: Boolean, onChange: (Boolean) -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.padding(16.dp).fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(if (liveSlam) "Live-SLAM" else "Record-only", style = MaterialTheme.typography.titleSmall)
                Text(
                    "Tech Spec §3.1 capture toggle — binds scan_session_config.live_slam. Record-always holds either way.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(checked = liveSlam, onCheckedChange = onChange)
        }
    }
}

@Composable
private fun RecordingControls(
    captureState: CaptureState,
    pauseSupported: Boolean,
    isReplaySession: Boolean,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
) {
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        when (captureState) {
            CaptureState.IDLE -> Button(onClick = onStart) {
                Icon(Icons.Filled.FiberManualRecord, contentDescription = null, tint = Color(0xFFD32F2F))
                Spacer(Modifier.height(0.dp))
                Text(if (isReplaySession) "  Start replay" else "  Start recording")
            }
            CaptureState.RECORDING -> {
                if (pauseSupported) {
                    Button(onClick = onPause) {
                        Icon(Icons.Filled.Pause, contentDescription = null)
                        Text("  Pause")
                    }
                }
                OutlinedButton(onClick = onStop) {
                    Icon(Icons.Filled.Stop, contentDescription = null)
                    Text("  Stop")
                }
            }
            CaptureState.PAUSED -> {
                Button(onClick = onResume) {
                    Icon(Icons.Filled.PlayArrow, contentDescription = null)
                    Text("  Resume")
                }
                OutlinedButton(onClick = onStop) {
                    Icon(Icons.Filled.Stop, contentDescription = null)
                    Text("  Stop")
                }
            }
            CaptureState.STOPPING -> Text("Stopping…", style = MaterialTheme.typography.bodyMedium)
        }
    }
}

/** pts/s, total points, duration, storage MB, device health chip — the B4 brief's exact status-strip list. */
@Composable
private fun StatusStripCard(stats: CaptureStats, health: DeviceHealth?) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Live stats", style = MaterialTheme.typography.titleSmall)
                HealthChip(health)
            }
            Spacer(Modifier.height(8.dp))
            StatRow("Points captured", "%,d".format(stats.pointsCaptured))
            StatRow("Points/sec", "%.0f".format(stats.pointsPerSecond))
            StatRow("Duration", "%.1f s".format(stats.elapsedMillis / 1000.0))
            StatRow("Storage", "%.2f MB".format(stats.recordingSizeBytes / 1_000_000.0))
        }
    }
}

@Composable
private fun HealthChip(health: DeviceHealth?) {
    val (label, color) = when {
        health == null -> "No data" to MaterialTheme.colorScheme.surfaceVariant
        health.state == ScanEngineNative.DeviceState.STREAMING && health.checksumPassRate >= 0.995 ->
            "Healthy" to Color(0xFF2E7D32)
        health.state == ScanEngineNative.DeviceState.FAULT -> "Fault" to Color(0xFFC62828)
        health.state == ScanEngineNative.DeviceState.DEGRADED -> "Degraded" to Color(0xFFEF6C00)
        else -> ScanEngineNative.DeviceState.label(health.state) to MaterialTheme.colorScheme.surfaceVariant
    }
    Box(
        modifier = Modifier.padding(2.dp),
    ) {
        Card {
            Text(
                label,
                modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
                style = MaterialTheme.typography.labelMedium,
                color = color,
            )
        }
    }
}

/**
 * B7/B8's own strip: what the AR session and the keyframe pipeline are
 * actually doing. The tracking-loss episode count matters more than it looks
 * — Tech Spec §3.3 excludes points captured during tracking loss by default,
 * so a session with a high count has holes the user should walk back and
 * rescan while they are still on site.
 */
@Composable
private fun ArStatusCard(
    posesPushed: Long,
    trackingLossEpisodes: Int,
    keyframeStats: com.lidarscan.app.ar.KeyframeRecorder.Stats,
    mountCalibrationHeadline: String?,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("AR + camera keyframes", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            StatRow("AR poses pushed", "%,d".format(posesPushed))
            StatRow("Tracking-loss episodes", trackingLossEpisodes.toString())
            StatRow("Keyframes written", "%,d".format(keyframeStats.keyframesWritten))
            StatRow("Keyframe data", "%.1f MB".format(keyframeStats.bytesWritten / 1_000_000.0))
            StatRow("Skipped (turning too fast)", "%,d".format(keyframeStats.skippedMotion))
            StatRow(
                "Rolling shutter",
                if (keyframeStats.rollingShutterKnown) "measured per frame" else "not reported by this camera",
            )
            mountCalibrationHeadline?.let {
                Spacer(Modifier.height(6.dp))
                Text(it, style = MaterialTheme.typography.bodySmall)
            }
            keyframeStats.lastError?.let {
                Spacer(Modifier.height(6.dp))
                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            }
        }
    }
}

@Composable
private fun DisplayControlsCard(
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    cameraMode: CameraMode,
    arAvailable: Boolean,
    onColorModeChange: (ColorMode) -> Unit,
    onColormapChange: (Colormap) -> Unit,
    onPointSizeChange: (Float) -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Display (A14 contract)", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))

            Text("Color mode", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                val modes = listOf(ColorMode.RGB, ColorMode.HEIGHT, ColorMode.INTENSITY)
                modes.forEachIndexed { index, mode ->
                    SegmentedButton(
                        selected = colorMode == mode,
                        onClick = { onColorModeChange(mode) },
                        shape = SegmentedButtonDefaults.itemShape(index = index, count = modes.size),
                    ) { Text(mode.name.lowercase().replaceFirstChar { it.uppercase() }) }
                }
            }

            if (colorMode == ColorMode.HEIGHT || colorMode == ColorMode.INTENSITY) {
                Spacer(Modifier.height(12.dp))
                Text("Colormap", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    val maps = listOf(Colormap.GRAYSCALE, Colormap.SPECTRUM, Colormap.THERMAL)
                    maps.forEachIndexed { index, cm ->
                        SegmentedButton(
                            selected = colormap == cm,
                            onClick = { onColormapChange(cm) },
                            shape = SegmentedButtonDefaults.itemShape(index = index, count = maps.size),
                        ) { Text(cm.name.lowercase().replaceFirstChar { it.uppercase() }) }
                    }
                }
            }

            Spacer(Modifier.height(12.dp))
            Text(
                "Point size: %.1f px".format(pointSizePx),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Slider(value = pointSizePx, onValueChange = onPointSizeChange, valueRange = 0.5f..12f)

            Spacer(Modifier.height(12.dp))
            Text("View", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                // §3.7's "Toggle AR view <-> free-orbit 3D". AR is offered only
                // when there is an ARCore session to drive it — a dead toggle
                // is worse than an absent one.
                val modes = if (arAvailable) {
                    listOf(CameraMode.AR, CameraMode.ORBIT, CameraMode.FOLLOW)
                } else {
                    listOf(CameraMode.ORBIT, CameraMode.FOLLOW)
                }
                modes.forEachIndexed { index, mode ->
                    SegmentedButton(
                        selected = cameraMode == mode,
                        onClick = { onCameraModeChange(mode) },
                        shape = SegmentedButtonDefaults.itemShape(index = index, count = modes.size),
                    ) { Text(cameraModeLabel(mode)) }
                }
            }
        }
    }
}

@Composable
private fun SessionSummaryContent(summary: CaptureStats, onDismiss: () -> Unit) {
    Column(Modifier.padding(24.dp)) {
        Text("Session summary", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(16.dp))
        StatRow("Total points", "%,d".format(summary.pointsCaptured))
        StatRow("Duration", "%.1f s".format(summary.elapsedMillis / 1000.0))
        StatRow("Storage", "%.2f MB".format(summary.recordingSizeBytes / 1_000_000.0))
        StatRow("Avg points/sec", "%.0f".format(if (summary.elapsedMillis > 0) summary.pointsCaptured * 1000.0 / summary.elapsedMillis else 0.0))
        Spacer(Modifier.height(24.dp))
        Button(onClick = onDismiss, modifier = Modifier.fillMaxWidth()) { Text("Done") }
        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun StatRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodyMedium)
    }
}

/** "AR" reads as an initialism; the other two are words. */
private fun cameraModeLabel(mode: CameraMode): String = when (mode) {
    CameraMode.AR -> "AR"
    CameraMode.ORBIT -> "Orbit"
    CameraMode.FOLLOW -> "Follow"
}
