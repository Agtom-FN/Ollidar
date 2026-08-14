package com.lidarscan.app.ui.detail

import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.Preview
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
import androidx.compose.runtime.rememberCoroutineScope
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
import com.lidarscan.core.store.Project
import kotlinx.coroutines.launch

@Composable
fun ProjectDetailRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
) {
    val viewModel: ProjectDetailViewModel = viewModel(
        factory = viewModelFactory {
            initializer { ProjectDetailViewModel(container.projectStore, projectId) }
        },
    )
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    ProjectDetailScreen(uiState = uiState, onBack = onBack)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ProjectDetailScreen(
    uiState: ProjectDetailUiState,
    onBack: () -> Unit,
) {
    val snackbarHostState = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()

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
                onArrivesWith = { workstream ->
                    scope.launch { snackbarHostState.showSnackbar("Arrives with $workstream") }
                },
            )
        }
    }
}

@Composable
private fun ProjectDetailContent(
    project: Project,
    modifier: Modifier = Modifier,
    onArrivesWith: (String) -> Unit,
) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(16.dp),
    ) {
        ManifestSummaryCard(project)

        Spacer(Modifier.height(20.dp))
        Text("Workspace", style = MaterialTheme.typography.titleSmall)
        Spacer(Modifier.height(8.dp))

        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            StubNavCard(
                icon = Icons.Filled.CameraAlt,
                title = "Capture",
                subtitle = "Live 3D / AR overlay, live-SLAM vs record-only, RTK status strip.",
                arrivesWith = "B4",
                onClick = { onArrivesWith("B4") },
            )
            StubNavCard(
                icon = Icons.Filled.Build,
                title = "Processing",
                subtitle = "Mode chooser (local / cloud / transfer), job queue.",
                arrivesWith = "B6",
                onClick = { onArrivesWith("B6") },
            )
            StubNavCard(
                icon = Icons.Filled.Preview,
                title = "Review",
                subtitle = "Point-cloud viewer, display params, measure, plan view, export.",
                arrivesWith = "B6",
                onClick = { onArrivesWith("B6") },
            )
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
private fun StubNavCard(
    icon: ImageVector,
    title: String,
    subtitle: String,
    arrivesWith: String,
    onClick: () -> Unit,
) {
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
                ArrivesWithBadge(arrivesWith)
            }
            Spacer(Modifier.height(6.dp))
            HorizontalDivider()
            Spacer(Modifier.height(8.dp))
            Text(
                subtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun ArrivesWithBadge(workstream: String) {
    Text(
        text = "Arrives with $workstream",
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.primary,
    )
}
