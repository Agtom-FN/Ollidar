package com.lidarscan.app.ui.merge

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer

@Composable
fun MergeRoute(container: AppContainer, onBack: () -> Unit) {
    val vm: MergeViewModel = viewModel(
        factory = viewModelFactory { initializer { MergeViewModel(container, container.projectStore) } },
    )
    val state by vm.uiState.collectAsStateWithLifecycle()
    MergeScreen(state, vm, onBack)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MergeScreen(state: MergeUiState, vm: MergeViewModel, onBack: () -> Unit) {
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
                title = { Text("Merge sessions") },
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
            Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("Georeferenced auto-merge", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "Sessions are placed by composing through their shared CRS — no manual alignment. The desktop " +
                            "app's merge workbench is where 3-point and drag alignment live; offering a bad automatic " +
                            "answer here would put two clouds on top of each other at the origin, which looks like data.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        "Each selected session is post-processed first unless it already has been in this app session. " +
                            "That is the expensive part — the engine cannot read a processed cloud back out of a " +
                            ".lscan, because nothing writes one into it yet.",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Text("Projects", style = MaterialTheme.typography.titleSmall)
            if (state.candidates.isEmpty()) {
                Text(
                    "No projects yet.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            state.candidates.forEach { c ->
                Card(Modifier.fillMaxWidth()) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(
                            checked = state.selected.contains(c.project.id),
                            onCheckedChange = { vm.toggle(c.project.id) },
                            enabled = c.isGeoreferenced,
                        )
                        Column(Modifier.weight(1f)) {
                            Text(c.project.manifest.name, style = MaterialTheme.typography.bodyMedium)
                            val reason = c.reason
                            if (reason != null) {
                                Text(
                                    reason,
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.error,
                                )
                            } else {
                                Text(
                                    "${c.georef?.epsgText} · ±%.3f m (1σ)".format(c.georef?.horizontalSigmaM ?: 0.0) +
                                        if (c.chainFromJob != 0L) " · already post-processed" else "",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
            }

            if (state.running) {
                Text(state.progressLabel, style = MaterialTheme.typography.bodyMedium)
                LinearProgressIndicator(progress = { state.progress }, modifier = Modifier.fillMaxWidth())
                OutlinedButton(onClick = vm::cancel) { Text("Cancel") }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = vm::merge, enabled = state.selected.size >= 2) { Text("Merge selected") }
                    if (state.resultFile != null) {
                        OutlinedButton(onClick = { vm.shareResult(context) }) { Text("Share .ply") }
                    }
                }
                if (state.selected.size < 2) {
                    Text(
                        "Pick at least two georeferenced sessions.",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            state.summaryText?.let {
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text("Result", style = MaterialTheme.typography.titleSmall)
                        Text(it, style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
    }
}
