package com.lidarscan.app.ui.detail

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Cable
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.Preview
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.SatelliteAlt
import androidx.compose.material.icons.filled.Straighten
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.common.ProfileChip
import com.lidarscan.app.ui.common.SensorBadge
import com.lidarscan.app.ui.common.formatCreatedDate
import com.lidarscan.app.ui.common.formatPointCount
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.Project

@Composable
fun ProjectDetailRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
    onOpenCapture: (String) -> Unit,
    onOpenMountCalibration: (String) -> Unit,
    onOpenMid360Connect: (String) -> Unit = {},
    onOpenProcessing: (String) -> Unit = {},
    onOpenReview: (String) -> Unit = {},
    onOpenRtk: () -> Unit = {},
    onOpenMerge: () -> Unit = {},
) {
    val viewModel: ProjectDetailViewModel = viewModel(
        factory = viewModelFactory {
            initializer { ProjectDetailViewModel(container.projectStore, projectId) }
        },
    )
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    ProjectDetailScreen(
        uiState = uiState,
        onBack = onBack,
        onOpenCapture = onOpenCapture,
        onOpenMountCalibration = onOpenMountCalibration,
        onOpenMid360Connect = onOpenMid360Connect,
        onOpenProcessing = onOpenProcessing,
        onOpenReview = onOpenReview,
        onOpenRtk = onOpenRtk,
        onOpenMerge = onOpenMerge,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ProjectDetailScreen(
    uiState: ProjectDetailUiState,
    onBack: () -> Unit,
    onOpenCapture: (String) -> Unit = {},
    onOpenMountCalibration: (String) -> Unit = {},
    onOpenMid360Connect: (String) -> Unit = {},
    onOpenProcessing: (String) -> Unit = {},
    onOpenReview: (String) -> Unit = {},
    onOpenRtk: () -> Unit = {},
    onOpenMerge: () -> Unit = {},
) {
    val snackbarHostState = remember { SnackbarHostState() }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text((uiState as? ProjectDetailUiState.Loaded)?.project?.manifest?.name ?: "Project")
                },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState) },
    ) { padding ->
        when (uiState) {
            ProjectDetailUiState.Loading -> Box(Modifier.fillMaxSize().padding(padding))
            ProjectDetailUiState.NotFound -> Box(
                modifier = Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    "Project not found — it may have been deleted.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            is ProjectDetailUiState.Loaded -> ProjectDetailContent(
                project = uiState.project,
                modifier = Modifier.padding(padding),
                onOpenCapture = { onOpenCapture(uiState.project.id) },
                onOpenMountCalibration = { onOpenMountCalibration(uiState.project.id) },
                onOpenMid360Connect = { onOpenMid360Connect(uiState.project.id) },
                onOpenProcessing = { onOpenProcessing(uiState.project.id) },
                onOpenReview = { onOpenReview(uiState.project.id) },
                onOpenRtk = onOpenRtk,
                onOpenMerge = onOpenMerge,
            )
        }
    }
}

@Composable
private fun ProjectDetailContent(
    project: Project,
    modifier: Modifier = Modifier,
    onOpenCapture: () -> Unit,
    onOpenMountCalibration: () -> Unit,
    onOpenMid360Connect: () -> Unit,
    onOpenProcessing: () -> Unit,
    onOpenReview: () -> Unit,
    onOpenRtk: () -> Unit,
    onOpenMerge: () -> Unit,
) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
    ) {
        ManifestSummaryCard(project)

        Spacer(Modifier.height(20.dp))
        Text("Workspace", style = MaterialTheme.typography.titleSmall)
        Spacer(Modifier.height(8.dp))

        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            CaptureNavCard(onClick = onOpenCapture)
            if (project.manifest.sensor == SensorType.MID360) {
                Mid360ConnectNavCard(
                    settings = project.manifest.mid360,
                    onClick = onOpenMid360Connect,
                )
            }
            MountCalibrationNavCard(
                calibrated = project.manifest.mountCalibration != null,
                gateHeadline = project.manifest.mountCalibration?.readout()?.headline,
                onClick = onOpenMountCalibration,
            )
            NavCard(
                icon = Icons.Filled.SatelliteAlt,
                title = "RTK rover",
                subtitle = "Bluetooth rover, NTRIP corrections, fix status. Shared across projects — the rover attaches " +
                    "to the engine, not to one capture.",
                onClick = onOpenRtk,
            )
            NavCard(
                icon = Icons.Filled.Build,
                title = "Processing",
                subtitle = "Post-process, colorize, export, transfer bundle, cloud submit — and the queue running them.",
                onClick = onOpenProcessing,
            )
            NavCard(
                icon = Icons.Filled.Preview,
                title = "Review",
                subtitle = "Point-cloud viewer with display parameters, the measure tool, and the floor-plan viewer.",
                onClick = onOpenReview,
            )
            NavCard(
                icon = Icons.Filled.Layers,
                title = "Merge",
                subtitle = "Combine this capture with other georeferenced sessions.",
                onClick = onOpenMerge,
            )
        }
    }
}

/** A workspace entry point. The B1-era "Arrives with Bx" variant is gone — every card now goes somewhere. */
@Composable
private fun NavCard(icon: ImageVector, title: String, subtitle: String, onClick: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        onClick = onClick,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(12.dp))
                Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            }
            Spacer(Modifier.height(6.dp))
            HorizontalDivider()
            Spacer(Modifier.height(8.dp))
            Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun ManifestSummaryCard(project: Project) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SensorBadge(project.manifest.sensor)
                ProfileChip(project.manifest.profile)
            }
            Spacer(Modifier.height(12.dp))
            ManifestRow("Created", formatCreatedDate(project.manifest.createdAtEpochMillis))
            ManifestRow("Points captured", formatPointCount(project.manifest.pointCountEstimate))
            ManifestRow("App version", project.manifest.appVersion)
            ManifestRow("Schema version", project.manifest.schemaVersion.toString())
            ManifestRow("Directory", project.directory.absolutePath)
        }
    }
}

@Composable
private fun ManifestRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun CaptureNavCard(onClick: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        onClick = onClick,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.CameraAlt, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(12.dp))
                Text("Capture", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            }
            Spacer(Modifier.height(6.dp))
            HorizontalDivider()
            Spacer(Modifier.height(8.dp))
            Text(
                "Connect the sensor, start/stop a recording session, and watch the cloud build in the live 3D or " +
                    "AR view. Live-SLAM vs Record-only starts from this project's workflow profile.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/**
 * B3: Device setup -> Mid-360 connect. Only shown for a Mid-360 project —
 * the card exists to surface the SAVED addresses without opening the wizard,
 * because "which host IP was this capture taken with" is the first question
 * asked when a .lscan turns out empty, and a wrong host IP is the failure
 * that produces no error at all.
 */
@Composable
private fun Mid360ConnectNavCard(
    settings: com.lidarscan.core.net.Mid360Settings?,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        onClick = onClick,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.Cable, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(12.dp))
                Text("Mid-360 connect", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            }
            Spacer(Modifier.height(6.dp))
            HorizontalDivider()
            Spacer(Modifier.height(8.dp))
            Text(
                if (settings != null) {
                    "Lidar ${settings.lidarIp} -> host ${settings.hostIp} (saved). Re-run the self-test after " +
                        "moving sites: the host IP has to be an address this phone actually holds."
                } else {
                    "Not configured. Ethernet adapter, static IP and a pre-capture self-test — capture cannot " +
                        "start without both addresses."
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** B7: Device setup -> Mount calibration. Shows the stored gate verdict so a stale calibration is visible without opening the wizard. */
@Composable
private fun MountCalibrationNavCard(
    calibrated: Boolean,
    gateHeadline: String?,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        onClick = onClick,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.Straighten, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(12.dp))
                Text("Mount calibration", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            }
            Spacer(Modifier.height(6.dp))
            HorizontalDivider()
            Spacer(Modifier.height(8.dp))
            Text(
                gateHeadline ?: if (calibrated) {
                    "Calibrated"
                } else {
                    "Not calibrated. Print the checkerboard target and capture at least 8 poses, including tilt."
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

