package com.lidarscan.app.ui.rtk

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.common.CaptureGateBanner
import com.lidarscan.app.ui.common.FixStatusStrip
import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.gnss.IssueSeverity

@Composable
fun RtkRoute(container: AppContainer, onBack: () -> Unit) {
    val vm: RtkViewModel = viewModel(
        factory = viewModelFactory { initializer { RtkViewModel(container, container.settingsRepository) } },
    )
    val state by vm.uiState.collectAsStateWithLifecycle()
    RtkScreen(state, vm, onBack)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RtkScreen(state: RtkUiState, vm: RtkViewModel, onBack: () -> Unit) {
    val snackbar = remember { SnackbarHostState() }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> vm.onPermissionResult(granted) }

    LaunchedEffect(state.message) {
        state.message?.let {
            snackbar.showSnackbar(it)
            vm.dismissMessage()
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("RTK rover") },
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
            FixStatusStrip(state.fix, state.ntrip)
            CaptureGateBanner(state.gate)

            // --- rover ---------------------------------------------------
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Rover (Bluetooth SPP)", style = MaterialTheme.typography.titleMedium)
                    when {
                        !state.bluetoothAvailable -> Text(
                            "This device has no Bluetooth adapter, so no rover can be attached.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.error,
                        )
                        !state.permissionGranted -> {
                            Text(
                                "Bluetooth permission is needed to open a serial connection to an already-paired " +
                                    "rover. This app never scans, so it does not ask for the scan or location " +
                                    "permissions — pair the receiver once in the system Bluetooth settings.",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Button(onClick = {
                                val p = vm.requiredPermission() ?: Manifest.permission.BLUETOOTH_CONNECT
                                permissionLauncher.launch(p)
                            }) { Text("Grant Bluetooth access") }
                        }
                        !state.bluetoothEnabled -> Text(
                            "Bluetooth is off. Turn it on, then refresh.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.error,
                        )
                        state.bondedRovers.isEmpty() -> Text(
                            "No paired devices. Pair the receiver in the system Bluetooth settings first — the PIN " +
                                "dialog belongs to the OS, and duplicating it here would still hand you back to it.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        else -> state.bondedRovers.forEach { d ->
                            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text(d.name, style = MaterialTheme.typography.bodyMedium)
                                    Text(
                                        d.address,
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                                if (state.connectedAddress == d.address) {
                                    OutlinedButton(onClick = vm::disconnectRover) { Text("Disconnect") }
                                } else {
                                    Button(onClick = { vm.connectRover(d.address) }, enabled = !state.connecting) {
                                        Text("Connect")
                                    }
                                }
                            }
                        }
                    }
                    TextButton(onClick = vm::refreshDevices) { Text("Refresh") }
                    if (state.connectedAddress != null) {
                        Text(
                            "NMEA in: ${state.bytesIn} bytes · RTCM out: ${state.rtcmOut} bytes · " +
                                "${state.stats.sentencesOk} sentences ok, ${state.stats.checksumFailed} checksum failures",
                            style = MaterialTheme.typography.labelSmall,
                        )
                        if (!state.stats.timeConverged) {
                            Text(
                                "Clock correlation has not converged yet — normal for roughly the first 16 s of a 1 Hz " +
                                    "stream, and not an error.",
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
            }

            // --- NTRIP ---------------------------------------------------
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("NTRIP corrections", style = MaterialTheme.typography.titleMedium)
                    val n = state.ntripSettings
                    OutlinedTextField(
                        value = n.host,
                        onValueChange = { vm.updateNtrip(n.copy(host = it)) },
                        label = { Text("Caster host") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(
                            value = n.port.toString(),
                            onValueChange = { v -> vm.updateNtrip(n.copy(port = v.toIntOrNull() ?: 0)) },
                            label = { Text("Port") },
                            singleLine = true,
                            modifier = Modifier.weight(1f),
                        )
                        OutlinedTextField(
                            value = n.mountpoint,
                            onValueChange = { vm.updateNtrip(n.copy(mountpoint = it)) },
                            label = { Text("Mountpoint") },
                            singleLine = true,
                            modifier = Modifier.weight(2f),
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(
                            value = n.username,
                            onValueChange = { vm.updateNtrip(n.copy(username = it)) },
                            label = { Text("Username") },
                            singleLine = true,
                            modifier = Modifier.weight(1f),
                        )
                        OutlinedTextField(
                            value = n.password,
                            onValueChange = { vm.updateNtrip(n.copy(password = it)) },
                            label = { Text("Password") },
                            singleLine = true,
                            visualTransformation = PasswordVisualTransformation(),
                            modifier = Modifier.weight(1f),
                        )
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                        Text("Auto-reconnect", style = MaterialTheme.typography.bodyMedium)
                        Switch(checked = n.autoReconnect, onCheckedChange = { vm.updateNtrip(n.copy(autoReconnect = it)) })
                    }

                    n.validate().forEach { issue ->
                        Text(
                            "${issue.field}: ${issue.message}",
                            style = MaterialTheme.typography.labelSmall,
                            color = when (issue.severity) {
                                IssueSeverity.FATAL -> MaterialTheme.colorScheme.error
                                IssueSeverity.WARNING -> MaterialTheme.colorScheme.tertiary
                                IssueSeverity.NOTE -> MaterialTheme.colorScheme.onSurfaceVariant
                            },
                        )
                    }

                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = vm::connectNtrip, enabled = n.isConnectable && !state.ntripBusy) {
                            Text("Connect")
                        }
                        OutlinedButton(onClick = vm::disconnectNtrip) { Text("Disconnect") }
                        OutlinedButton(onClick = vm::fetchSourcetable, enabled = n.host.isNotBlank() && !state.ntripBusy) {
                            Text("Mountpoints")
                        }
                    }

                    if (state.mountpoints.isNotEmpty()) {
                        HorizontalDivider()
                        Text(
                            "Nearest first — baseline length is the dominant term in Fixed-vs-Float.",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        state.mountpoints.take(12).forEach { m ->
                            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text("${m.mountpoint} · ${m.format}", style = MaterialTheme.typography.bodySmall)
                                    Text(
                                        listOfNotNull(
                                            m.identifier.takeIf { it.isNotBlank() },
                                            m.country.takeIf { it.isNotBlank() },
                                            if (m.needsGga) "wants GGA (VRS)" else null,
                                            if (m.fee) "fee" else null,
                                        ).joinToString(" · "),
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                                TextButton(onClick = { vm.updateNtrip(n.copy(mountpoint = m.mountpoint)) }) { Text("Use") }
                            }
                        }
                    }

                    if (state.ntrip.framesOk > 0 || state.ntrip.framesCrcFailed > 0) {
                        Text(
                            "${state.ntrip.framesOk} RTCM frames ok · ${state.ntrip.framesCrcFailed} CRC failures · " +
                                "${state.ntrip.ggaSent} GGA uploads · ${state.ntrip.reconnects} reconnects",
                            style = MaterialTheme.typography.labelSmall,
                        )
                        if (state.ntrip.framesCrcFailed > 0) {
                            Text(
                                "CRC failures separate bad corrections from bad sky: corruption on this hop leaves the " +
                                    "rover silently in Float.",
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.tertiary,
                            )
                        }
                    }
                }
            }

            // --- georeferencing ------------------------------------------
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("Georeferencing", style = MaterialTheme.typography.titleMedium)
                    val g = state.georef
                    if (g == null) {
                        Text(
                            "Nothing yet — a georeference needs fixes and a trajectory to pair them against.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else if (!g.converged) {
                        Text("Not converged: ${g.blocker.ifBlank { "not enough usable fixes yet" }}", style = MaterialTheme.typography.bodySmall)
                        Text(
                            "Heading is only observable from a baseline, so a stationary rover reports this no matter " +
                                "how good its fixes are. Walk a few metres.",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        Text("Converged · ${g.epsgText}", style = MaterialTheme.typography.bodyMedium)
                        Text(
                            "±%.3f m horizontal (1σ) · CEP95 %.3f m · %d samples · %.1f m span"
                                .format(g.horizontalSigmaM, g.cep95M, g.samples, g.spanM),
                            style = MaterialTheme.typography.labelSmall,
                        )
                        Text(
                            "That sigma is deliberately conservative: it includes the fixes' own accuracy at full " +
                                "strength, because a session's fixes share a base-station error that does not average out.",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }

            Text(
                "The capture gate compares the live fix against the project profile's threshold. Survey blocks below " +
                    "${FixType.RTK_FLOAT.label}; the other profiles only warn.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
