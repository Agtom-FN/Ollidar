package com.lidarscan.app.ui.capture

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
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
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.app.engine.ScanEngineNative

@Composable
fun CaptureRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
    onConnectDevice: () -> Unit,
) {
    val viewModel: CaptureViewModel = viewModel(
        factory = viewModelFactory {
            initializer { CaptureViewModel(container.engineBridge, container.projectStore, projectId) }
        },
    )
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val connectionState by viewModel.connectionState.collectAsStateWithLifecycle()
    val captureState by viewModel.captureState.collectAsStateWithLifecycle()
    val stats by viewModel.stats.collectAsStateWithLifecycle()
    val health by viewModel.deviceHealth.collectAsStateWithLifecycle()

    CaptureScreen(
        uiState = uiState,
        connectionState = connectionState,
        captureState = captureState,
        stats = stats,
        health = health,
        onBack = onBack,
        onConnectDevice = onConnectDevice,
        onStart = { viewModel.startCapture(liveSlam = false) },
        onPause = viewModel::pauseCapture,
        onResume = viewModel::resumeCapture,
        onStop = viewModel::stopCapture,
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
    onBack: () -> Unit,
    onConnectDevice: () -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Capture") },
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
            is CaptureUiState.Loaded -> Column(
                modifier = Modifier.fillMaxSize().padding(padding).padding(16.dp),
            ) {
                ConnectionCard(connectionState, onConnectDevice)
                Spacer(Modifier.height(16.dp))

                if (connectionState == ConnectionState.CONNECTED) {
                    RecordingControls(captureState, onStart, onPause, onResume, onStop)
                    Spacer(Modifier.height(16.dp))
                    StatsCard(stats)
                    Spacer(Modifier.height(16.dp))
                    HealthCard(health)
                } else {
                    Text(
                        "Connect a D6 to start recording.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }

                // Live 3D / AR overlay preview is B4's job (Tech Spec §3.13,
                // "Capture (live 3D / AR overlay toggle...)") — the point
                // pipeline this needs (Filament streaming pages) lands with
                // B4/S3, not here.
            }
        }
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
private fun RecordingControls(
    captureState: CaptureState,
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
                Text("  Start recording")
            }
            CaptureState.RECORDING -> {
                Button(onClick = onPause) {
                    Icon(Icons.Filled.Pause, contentDescription = null)
                    Text("  Pause")
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

@Composable
private fun StatsCard(stats: CaptureStats) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Live stats", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            StatRow("Points captured", "%,d".format(stats.pointsCaptured))
            StatRow("Points/sec", "%.0f".format(stats.pointsPerSecond))
            StatRow("Elapsed", "%.1f s".format(stats.elapsedMillis / 1000.0))
            StatRow("Recording size", formatBytes(stats.recordingSizeBytes))
        }
    }
}

@Composable
private fun HealthCard(health: DeviceHealth?) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Device health", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            if (health == null) {
                Text("Waiting for first sample…", color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                StatRow("State", ScanEngineNative.DeviceState.label(health.state))
                StatRow("Rotation", "%.2f Hz".format(health.rotationHz))
                StatRow("Checksum pass rate", "%.2f%%".format(health.checksumPassRate * 100.0))
            }
        }
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

private fun formatBytes(bytes: Long): String = when {
    bytes >= 1_000_000_000L -> "%.2f GB".format(bytes / 1_000_000_000.0)
    bytes >= 1_000_000L -> "%.2f MB".format(bytes / 1_000_000.0)
    bytes >= 1_000L -> "%.1f KB".format(bytes / 1_000.0)
    else -> "$bytes B"
}
