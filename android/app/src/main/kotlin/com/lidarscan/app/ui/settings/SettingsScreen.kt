package com.lidarscan.app.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.data.AppSettings
import com.lidarscan.app.data.ThemeMode
import com.lidarscan.app.data.Units
import com.lidarscan.app.di.AppContainer

@Composable
fun SettingsRoute(
    container: AppContainer,
    onBack: () -> Unit,
    onReplaySyntheticCapture: (projectId: String) -> Unit = {},
) {
    val viewModel: SettingsViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                SettingsViewModel(
                    settingsRepository = container.settingsRepository,
                    projectStore = container.projectStore,
                    storageLocation = container.projectsRootDir.absolutePath,
                )
            }
        },
    )
    val settings by viewModel.settings.collectAsStateWithLifecycle()

    SettingsScreen(
        settings = settings,
        storageLocation = viewModel.storageLocation,
        nativeEngineAvailable = com.lidarscan.app.engine.ScanEngineNative.isAvailable,
        onUnitsChange = viewModel::setUnits,
        onThemeModeChange = viewModel::setThemeMode,
        onUseFakeEngineChange = viewModel::setUseFakeEngine,
        onReplaySyntheticCapture = { viewModel.replaySyntheticCapture(onReplaySyntheticCapture) },
        onBack = onBack,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    settings: AppSettings,
    storageLocation: String,
    nativeEngineAvailable: Boolean,
    onUnitsChange: (Units) -> Unit,
    onThemeModeChange: (ThemeMode) -> Unit,
    onUseFakeEngineChange: (Boolean) -> Unit,
    onReplaySyntheticCapture: () -> Unit = {},
    onBack: () -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(modifier = Modifier.fillMaxWidth().padding(padding).padding(16.dp)) {
            Text("Units", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                Units.entries.forEachIndexed { index, option ->
                    SegmentedButton(
                        selected = settings.units == option,
                        onClick = { onUnitsChange(option) },
                        shape = SegmentedButtonDefaults.itemShape(index = index, count = Units.entries.size),
                    ) {
                        Text(option.displayName)
                    }
                }
            }

            Spacer(Modifier.height(24.dp))
            Text("Theme", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                ThemeMode.entries.forEachIndexed { index, option ->
                    SegmentedButton(
                        selected = settings.themeMode == option,
                        onClick = { onThemeModeChange(option) },
                        shape = SegmentedButtonDefaults.itemShape(index = index, count = ThemeMode.entries.size),
                    ) {
                        Text(option.displayName)
                    }
                }
            }

            Spacer(Modifier.height(24.dp))
            Text("Engine (developer)", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = androidx.compose.foundation.layout.Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text("Use simulated engine", style = MaterialTheme.typography.bodyMedium)
                            Text(
                                if (nativeEngineAvailable) {
                                    "Takes effect after restarting the app."
                                } else {
                                    "Native engine not loaded on this build — always simulated regardless of this switch."
                                },
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        androidx.compose.material3.Switch(
                            checked = settings.useFakeEngine || !nativeEngineAvailable,
                            enabled = nativeEngineAvailable,
                            onCheckedChange = onUseFakeEngineChange,
                        )
                    }

                    Spacer(Modifier.height(16.dp))
                    androidx.compose.material3.HorizontalDivider()
                    Spacer(Modifier.height(16.dp))

                    // B4's acceptance path: drives the full Capture screen
                    // (live 3D view, status strip, session summary) from the
                    // bundled synthetic D6 capture (S1's d6synth output) via
                    // ReplayEngineBridge — no hardware needed. Requires the
                    // real native engine (same requirement as the switch
                    // above); disabled with an explanatory line otherwise.
                    Text("Replay synthetic capture", style = MaterialTheme.typography.bodyMedium)
                    Text(
                        if (nativeEngineAvailable) {
                            "Opens Capture driven by a bundled synthetic D6 recording — verifies the live 3D view end to end with no hardware."
                        } else {
                            "Native engine not loaded on this build — replay needs it, same as live capture."
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(8.dp))
                    androidx.compose.material3.OutlinedButton(
                        onClick = onReplaySyntheticCapture,
                        enabled = nativeEngineAvailable,
                    ) {
                        Text("Replay synthetic capture")
                    }
                }
            }

            Spacer(Modifier.height(24.dp))
            Text("Storage location", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            Card(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier.padding(16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(Icons.Filled.Folder, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                    Spacer(Modifier.width(12.dp))
                    Column {
                        Text(storageLocation, style = MaterialTheme.typography.bodyMedium)
                        Spacer(Modifier.height(4.dp))
                        Text(
                            "Where .lscan projects are stored on this device. A location picker is a future addition.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}
