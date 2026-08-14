package com.lidarscan.app.ui.capture

import android.content.res.Configuration
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
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.app.render.PointCloudView
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
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
) {
    val viewModel: CaptureViewModel = viewModel(
        key = "capture-$projectId-$isReplay",
        factory = viewModelFactory {
            initializer {
                val bridge = if (isReplay) container.newReplayEngineBridge() else container.engineBridge
                CaptureViewModel(bridge, container.projectStore, projectId, isReplay = isReplay)
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
        onBack = onBack,
        onConnectDevice = onConnectDevice,
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
    onBack: () -> Unit,
    onConnectDevice: () -> Unit,
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
                            Live3DOrPlaceholder(connected, pointCloudSource, colorMode, colormap, pointSizePx, cameraMode)
                        }
                        Column(
                            modifier = Modifier.width(320.dp).fillMaxHeight().verticalScroll(rememberScrollState())
                                .padding(16.dp),
                        ) {
                            CaptureControlsColumn(
                                connectionState, captureState, stats, health, colorMode, colormap, pointSizePx,
                                cameraMode, liveSlam, isReplaySession, onConnectDevice, onStart, onPause, onResume,
                                onStop, onLiveSlamChange, onColorModeChange, onColormapChange, onPointSizeChange,
                                onCameraModeChange,
                            )
                        }
                    }
                } else {
                    Column(modifier = Modifier.fillMaxSize().padding(padding)) {
                        Box(modifier = Modifier.fillMaxWidth().weight(1f)) {
                            Live3DOrPlaceholder(connected, pointCloudSource, colorMode, colormap, pointSizePx, cameraMode)
                        }
                        Column(modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
                            CaptureControlsColumn(
                                connectionState, captureState, stats, health, colorMode, colormap, pointSizePx,
                                cameraMode, liveSlam, isReplaySession, onConnectDevice, onStart, onPause, onResume,
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
) {
    if (connected && source != null) {
        PointCloudView(
            source = source,
            colorMode = colorMode,
            colormap = colormap,
            pointSizePx = pointSizePx,
            cameraMode = cameraMode,
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
    onConnectDevice: () -> Unit,
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
        ConnectionCard(connectionState, onConnectDevice)
        Spacer(Modifier.height(16.dp))
    }

    if (connectionState == ConnectionState.CONNECTED) {
        if (captureState == CaptureState.IDLE) {
            LiveSlamToggle(liveSlam, onLiveSlamChange)
            Spacer(Modifier.height(12.dp))
        }
        RecordingControls(captureState, isReplaySession, onStart, onPause, onResume, onStop)
        Spacer(Modifier.height(16.dp))
        StatusStripCard(stats, health)
        Spacer(Modifier.height(16.dp))
        DisplayControlsCard(colorMode, colormap, pointSizePx, cameraMode, onColorModeChange, onColormapChange, onPointSizeChange, onCameraModeChange)
    } else if (!isReplaySession) {
        Text(
            "Connect a D6 to start recording.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun ConnectionCard(connectionState: ConnectionState, onConnectDevice: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.padding(16.dp).fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column {
                Text("Device", style = MaterialTheme.typography.titleSmall)
                Text(
                    connectionState.name.lowercase().replaceFirstChar { it.uppercase() },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            OutlinedButton(onClick = onConnectDevice) {
                Text(if (connectionState == ConnectionState.CONNECTED) "Manage" else "Connect")
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
                if (!isReplaySession) {
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

@Composable
private fun DisplayControlsCard(
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    cameraMode: CameraMode,
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
            Text("Camera", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                val modes = listOf(CameraMode.ORBIT, CameraMode.FOLLOW)
                modes.forEachIndexed { index, mode ->
                    SegmentedButton(
                        selected = cameraMode == mode,
                        onClick = { onCameraModeChange(mode) },
                        shape = SegmentedButtonDefaults.itemShape(index = index, count = modes.size),
                    ) { Text(mode.name.lowercase().replaceFirstChar { it.uppercase() }) }
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
