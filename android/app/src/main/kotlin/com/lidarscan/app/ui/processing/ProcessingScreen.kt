package com.lidarscan.app.ui.processing

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.core.jobs.ActionGate
import com.lidarscan.core.jobs.JobState
import com.lidarscan.core.jobs.ProcessingJob
import com.lidarscan.core.jobs.ProcessingMode
import com.lidarscan.core.model.ExportFormat

@Composable
fun ProcessingRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val vm: ProcessingViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ProcessingViewModel(container, container.projectStore, container.settingsRepository, projectId)
            }
        },
    )
    val state by vm.uiState.collectAsStateWithLifecycle()
    ProcessingScreen(state, vm, onBack, onOpenSettings)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ProcessingScreen(
    state: ProcessingUiState,
    vm: ProcessingViewModel,
    onBack: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val snackbar = remember { SnackbarHostState() }
    val context = LocalContext.current

    LaunchedEffect(state.message) {
        state.message?.let {
            snackbar.showSnackbar(it)
            vm.dismissMessage()
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Processing") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            if (!state.engineAvailable) {
                InfoCard(
                    "The native engine is not loaded in this build, so nothing can be processed. " +
                        "Check Settings → Engine (developer): the simulated engine has no processing pipeline behind it.",
                )
            }

            Text("Mode", style = MaterialTheme.typography.titleSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ProcessingMode.entries.forEach { m ->
                    FilterChip(
                        selected = state.mode == m,
                        onClick = { vm.setMode(m) },
                        label = { Text(m.displayName) },
                    )
                }
            }
            Text(
                state.mode.summary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            when (state.mode) {
                ProcessingMode.LOCAL -> LocalActions(state, vm)
                ProcessingMode.CLOUD -> CloudActions(state, vm, onOpenSettings)
                ProcessingMode.EXTRACT_FOR_TRANSFER -> TransferActions(state) { share ->
                    vm.transferBundle(context, share)
                }
            }

            Spacer(Modifier.height(4.dp))
            HorizontalDivider()
            Text("Queue", style = MaterialTheme.typography.titleSmall)
            if (state.jobs.isEmpty()) {
                Text(
                    "Nothing queued. The queue runs one job at a time — that is A15's design, not a limit of this screen.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                state.jobs.reversed().forEach { job -> JobRow(job) { vm.cancelJob(job.id) } }
            }
        }
    }
}

@Composable
private fun LocalActions(state: ProcessingUiState, vm: ProcessingViewModel) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        GatedAction(
            title = "Post-process",
            detail = "Full-density LIO re-run, loop closure and pose-graph optimization from the recorded streams. " +
                "This is the expensive one — expect minutes, and the phone to get warm.",
            gate = state.postProcessGate,
            onRun = vm::postProcess,
        )
        GatedAction(
            title = "Colorize",
            detail = "Samples camera keyframes onto the processed cloud. " +
                "${state.keyframeCount} keyframe(s) recorded · clock sync: ${state.syncQuality.label}.",
            gate = state.colorizeGate,
            onRun = vm::colorize,
        )
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Export", style = MaterialTheme.typography.titleMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ExportFormat.pointCloudFormats.forEach { f ->
                        FilterChip(
                            selected = state.exportFormat == f,
                            onClick = { vm.setExportFormat(f) },
                            label = { Text(f.displayName) },
                        )
                    }
                }
                Text(
                    state.exportFormat.description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                state.exportNote?.let {
                    Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.tertiary)
                }
                GateFooter(state.exportGate) { vm.export() }
            }
        }
    }
}

@Composable
private fun CloudActions(state: ProcessingUiState, vm: ProcessingViewModel, onOpenSettings: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Cloud submit", style = MaterialTheme.typography.titleMedium)
            Text(
                "The capture is packaged as a .lscan.zip (the same bundle Extract-for-transfer produces, minus the " +
                    "processed results), uploaded in resumable chunks, and the worker's results land in this project's " +
                    "processed/ directory.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val c = state.cloud
            if (c.running || c.jobId != null) {
                Text(c.phase, style = MaterialTheme.typography.bodyMedium)
                LinearProgressIndicator(
                    progress = {
                        if (c.uploadFraction < 1f) c.uploadFraction * 0.5f else 0.5f + c.serverProgress * 0.5f
                    },
                    modifier = Modifier.fillMaxWidth(),
                )
                c.jobId?.let { Text("Server job $it · ${c.serverState ?: "…"}", style = MaterialTheme.typography.bodySmall) }
            }
            c.error?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            }
            c.resultFile?.let {
                Text("Result: ${it.name}", style = MaterialTheme.typography.bodySmall)
            }
            if (!state.cloudGate.enabled) {
                Text(
                    state.cloudGate.reason.orEmpty(),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
                TextButton(onClick = onOpenSettings) { Text("Open Settings") }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = vm::cloudSubmit, enabled = !c.running) { Text("Upload and process") }
                    if (c.running) OutlinedButton(onClick = vm::cancelCloud) { Text("Cancel") }
                }
            }
        }
    }
}

@Composable
private fun TransferActions(state: ProcessingUiState, onRun: (Boolean) -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Transfer bundle", style = MaterialTheme.typography.titleMedium)
            Text(
                "Packages the whole project directory — manifest, raw streams, camera frames and any processed " +
                    "results — as a single .lscan.zip and hands it to the share sheet. The desktop app imports it, " +
                    "processes it, and exports a results bundle back. No server is involved.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (!state.transferGate.enabled) {
                Text(
                    state.transferGate.reason.orEmpty(),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { onRun(true) }) { Text("Package and share") }
                    OutlinedButton(onClick = { onRun(false) }) { Text("Package only") }
                }
            }
        }
    }
}

@Composable
private fun GatedAction(title: String, detail: String, gate: ActionGate, onRun: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            GateFooter(gate, onRun)
        }
    }
}

@Composable
private fun GateFooter(gate: ActionGate, onRun: () -> Unit) {
    if (gate.enabled) {
        Button(onClick = onRun) { Text("Run") }
    } else {
        Text(
            gate.reason.orEmpty(),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.error,
        )
    }
}

@Composable
private fun JobRow(job: ProcessingJob, onCancel: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(job.kind.displayName, style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
                AssistChip(onClick = {}, label = { Text("#${job.id}") })
                if (job.state.isActive) {
                    Spacer(Modifier.width(8.dp))
                    TextButton(onClick = onCancel) { Text("Cancel") }
                }
            }
            Text(
                job.statusText,
                style = MaterialTheme.typography.bodySmall,
                color = if (job.state == JobState.FAILED && !job.wasCancelled) {
                    MaterialTheme.colorScheme.error
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            if (job.state.isActive) {
                Spacer(Modifier.height(6.dp))
                LinearProgressIndicator(progress = { job.progress }, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}

@Composable
private fun InfoCard(text: String) {
    Card(Modifier.fillMaxWidth()) {
        Text(
            text,
            modifier = Modifier.padding(14.dp).fillMaxWidth(),
            style = MaterialTheme.typography.bodySmall,
            textAlign = TextAlign.Start,
        )
    }
}
