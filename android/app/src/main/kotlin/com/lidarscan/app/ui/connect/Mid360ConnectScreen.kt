package com.lidarscan.app.ui.connect

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
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Cable
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.OpenInNew
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.lidarscan.app.engine.Mid360LinkState
import com.lidarscan.app.net.StaticIpGuidance
import com.lidarscan.app.ui.common.InfoChip
import com.lidarscan.core.net.Mid360Field
import com.lidarscan.core.net.Mid360SelfTest
import com.lidarscan.core.net.Mid360Settings
import kotlinx.coroutines.delay

/**
 * The Mid-360 connect wizard (Tech Spec §3.1's Android row).
 *
 * Five sections, in the order an operator hits the problems:
 *
 *  1. **Interface** — is there an Ethernet adapter at all, and what address
 *     does it hold. This is first because every later field is meaningless
 *     without it, and because "no adapter" and "adapter with no address" are
 *     different problems with different fixes.
 *  2. **Static IP guidance** — per-OEM, with a Settings deep link. Guidance
 *     and not a form, because Android has no public API to set an Ethernet
 *     address (see [StaticIpGuidance]).
 *  3. **Addresses** — lidar IP, host IP, ports, backend. Validated live.
 *  4. **Self-test** — add_device + start, first-data-or-timeout in 8 s, with
 *     A3's link state and the health numbers.
 *  5. **Log** — the sequence, because on a bench the sequence is the
 *     diagnosis (desktop C2 keeps the same thing for the same reason).
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Mid360ConnectScreen(
    viewModel: Mid360ConnectViewModel,
    onBack: () -> Unit,
    onContinueToCapture: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current

    // Keep the health numbers live after a pass — the probe stays up and the
    // interesting part (does loss % stay at zero for 30 s?) happens after the
    // verdict, not before it.
    LaunchedEffect(state.phase) {
        if (state.phase == Mid360ConnectUiState.Phase.READY) {
            while (true) {
                delay(500)
                viewModel.refreshSnapshot()
            }
        }
    }

    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("Mid-360 connect") },
                navigationIcon = {
                    IconButton(onClick = {
                        // Always release the SDK2 singleton on the way out.
                        viewModel.stopProbe()
                        onBack()
                    }) {
                        Icon(Icons.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            InterfaceCard(state, onUseInterfaceAddress = viewModel::useInterfaceAddress)

            StaticIpCard(
                oem = state.oem,
                onOpenSettings = {
                    val opened = StaticIpGuidance.openSettings(context)
                    if (opened == null) {
                        // Nothing resolved — worth saying, it means Settings
                        // genuinely has no such screen on this build.
                    }
                },
            )

            AddressesCard(state, viewModel)

            SelfTestCard(state, viewModel, onContinueToCapture)

            if (state.selfTestLog.isNotEmpty()) LogCard(state)
        }
    }
}

@Composable
private fun InterfaceCard(state: Mid360ConnectUiState, onUseInterfaceAddress: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.Cable, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Ethernet interface", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.weight(1f))
                InfoChip(
                    text = if (state.ethernet.adapterPresent) "Present" else "Absent",
                    color = if (state.ethernet.adapterPresent) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.error
                    },
                )
            }

            if (!state.ethernet.adapterPresent) {
                Text(
                    "No Ethernet network. Plug a USB-C Ethernet adapter in — the Mid-360 has no USB or " +
                        "wireless path, and it needs its own 9–27 V supply (~6.5 W); USB-C cannot power it.",
                    style = MaterialTheme.typography.bodyMedium,
                )
            } else {
                Text(
                    "Interface: ${state.ethernet.interfaceName ?: "unknown"}",
                    style = MaterialTheme.typography.bodyMedium,
                )
                if (state.ethernet.hasAddress) {
                    Text(
                        "Addresses: " + state.ethernet.addresses.joinToString(", ") { "${it.ip}/${it.prefixLength}" },
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    TextButton(onClick = onUseInterfaceAddress) { Text("Use this as the host IP") }
                } else {
                    Text(
                        "The adapter is up but has no IPv4 address. There is no DHCP server on a direct " +
                            "lidar link, so a static IP has to be set in Settings — see below.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }

            Text(
                state.ethernet.lastEvent + " · " + state.ethernet.apiLevelNote +
                    (if (state.ethernet.usingRequest) "" else " · listening only (requestNetwork needs a permission this app cannot hold)"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun StaticIpCard(oem: StaticIpGuidance.Oem, onOpenSettings: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Static IP — ${oem.label}", style = MaterialTheme.typography.titleMedium)
            StaticIpGuidance.steps(oem).forEachIndexed { index, step ->
                Text("${index + 1}. $step", style = MaterialTheme.typography.bodyMedium)
            }
            Text(
                StaticIpGuidance.caveat(oem),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
            Text(
                "Menu paths are guidance from vendor documentation and vary by build — the addresses " +
                    "read back from the live interface above are the ground truth.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedButton(onClick = onOpenSettings) {
                Icon(Icons.Filled.OpenInNew, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Open network settings")
            }
        }
    }
}

@Composable
private fun AddressesCard(state: Mid360ConnectUiState, viewModel: Mid360ConnectViewModel) {
    val editable = state.phase == Mid360ConnectUiState.Phase.IDLE ||
        state.phase == Mid360ConnectUiState.Phase.FAILED

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Addresses", style = MaterialTheme.typography.titleMedium)
            Text(
                "Both addresses are mandatory. The Mid-360 is not discovered and does not discover this " +
                    "phone: it is told where to stream. A wrong host IP produces no error at all — just " +
                    "silence — which is why they are checked here rather than at capture time.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            val lidarIssue = state.validation.firstFor(Mid360Field.LIDAR_IP)
            OutlinedTextField(
                value = state.settings.lidarIp,
                onValueChange = viewModel::setLidarIp,
                label = { Text("Lidar IP") },
                placeholder = { Text(Mid360Settings.DEFAULT_LIDAR_IP) },
                singleLine = true,
                enabled = editable,
                isError = lidarIssue?.fatal == true,
                supportingText = lidarIssue?.let { { Text(it.message) } },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                modifier = Modifier.fillMaxWidth(),
            )

            val hostIssue = state.validation.firstFor(Mid360Field.HOST_IP)
            OutlinedTextField(
                value = state.settings.hostIp,
                onValueChange = viewModel::setHostIp,
                label = { Text("Host IP (this phone, static)") },
                placeholder = { Text(Mid360Settings.DEFAULT_HOST_IP) },
                singleLine = true,
                enabled = editable,
                isError = hostIssue?.fatal == true,
                supportingText = hostIssue?.let { { Text(it.message) } },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                modifier = Modifier.fillMaxWidth(),
            )

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PortField("Point", state.settings.devicePointPort, editable, viewModel::setDevicePointPort, Modifier.weight(1f))
                PortField("IMU", state.settings.deviceImuPort, editable, viewModel::setDeviceImuPort, Modifier.weight(1f))
                PortField("Command", state.settings.deviceCmdPort, editable, viewModel::setDeviceCmdPort, Modifier.weight(1f))
            }
            state.validation.firstFor(Mid360Field.PORTS)?.let {
                Text(it.message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            }
            Text(
                "Host-side ports follow Livox's +1 convention (${state.settings.hostPointPort} / " +
                    "${state.settings.hostImuPort} / ${state.settings.hostCmdPort}) and are not editable — " +
                    "they keep the two sides distinguishable in a packet capture.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            HorizontalDivider()

            Text("Transport", style = MaterialTheme.typography.titleSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(
                    selected = state.settings.backend == Mid360Settings.BACKEND_SDK2,
                    onClick = { viewModel.setBackend(Mid360Settings.BACKEND_SDK2) },
                    enabled = editable,
                    label = { Text("SDK2 (brings the device up)") },
                )
                FilterChip(
                    selected = state.settings.backend == Mid360Settings.BACKEND_RAW_UDP,
                    onClick = { viewModel.setBackend(Mid360Settings.BACKEND_RAW_UDP) },
                    enabled = editable,
                    label = { Text("Bound socket") },
                )
            }
            Text(
                if (state.usingPreboundSocket) {
                    "Bound-socket check: this app creates the UDP socket, binds it to the Ethernet Network " +
                        "with Network.bindSocket, and hands the descriptor to the engine. It is listen-only — " +
                        "no discovery, no handshake, no host-IP push — so it only sees a device already " +
                        "configured to stream here, and it carries no IMU."
                } else {
                    "SDK2 runs discovery, the handshake and the host-IP configuration push. It is the only " +
                        "transport that can start an out-of-the-box device, and the one capture uses. Its " +
                        "sockets are created inside the SDK, so they are not bound to the Ethernet Network " +
                        "per-socket — see NOTES.md."
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun PortField(
    label: String,
    value: Int,
    enabled: Boolean,
    onChange: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    OutlinedTextField(
        value = value.toString(),
        onValueChange = { text -> onChange(text.filter { it.isDigit() }.take(5).toIntOrNull() ?: 0) },
        label = { Text(label) },
        singleLine = true,
        enabled = enabled,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        modifier = modifier,
    )
}

@Composable
private fun SelfTestCard(
    state: Mid360ConnectUiState,
    viewModel: Mid360ConnectViewModel,
    onContinueToCapture: (() -> Unit)?,
) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Pre-capture self-test", style = MaterialTheme.typography.titleMedium)
            Text(
                "Adds the device and starts a preview session (nothing is recorded), then waits up to " +
                    "${Mid360SelfTest.WINDOW_MS / 1000} s for the first point. Handshake to first packet " +
                    "measured 1.45 s in simulation, so 8 s is about five times the expected time.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            if (!state.nativeAvailable) {
                Text(
                    "The native engine is not loaded (simulated engine selected in Settings, or the " +
                        "library failed to load). The self-test needs the real engine.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            when (val verdict = state.verdict) {
                is Mid360SelfTest.Verdict.Testing -> {
                    Text(verdict.message, style = MaterialTheme.typography.bodyLarge)
                    LinearProgressIndicator(
                        progress = { verdict.progress },
                        modifier = Modifier.fillMaxWidth(),
                    )
                }

                is Mid360SelfTest.Verdict.Passed -> {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            Icons.Filled.CheckCircle,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.primary,
                        )
                        Spacer(Modifier.width(8.dp))
                        Text(
                            "Self-test PASSED",
                            style = MaterialTheme.typography.titleSmall,
                            color = MaterialTheme.colorScheme.primary,
                        )
                    }
                    Text(verdict.detail, style = MaterialTheme.typography.bodyMedium)
                }

                is Mid360SelfTest.Verdict.Failed -> {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Filled.Error, contentDescription = null, tint = MaterialTheme.colorScheme.error)
                        Spacer(Modifier.width(8.dp))
                        Text(
                            "Self-test FAILED",
                            style = MaterialTheme.typography.titleSmall,
                            color = MaterialTheme.colorScheme.error,
                        )
                    }
                    Text(verdict.detail, style = MaterialTheme.typography.bodyMedium)
                    if (verdict.diagnosis.isNotBlank()) {
                        Text(verdict.diagnosis, style = MaterialTheme.typography.bodySmall)
                    }
                }

                null -> Unit
            }

            state.snapshot?.let { HealthReadout(state, it) }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Button(
                    onClick = viewModel::runSelfTest,
                    enabled = state.canRunSelfTest ||
                        (state.phase == Mid360ConnectUiState.Phase.FAILED && state.validation.isUsable),
                ) {
                    Text(if (state.phase == Mid360ConnectUiState.Phase.FAILED) "Retry self-test" else "Run self-test")
                }
                if (state.phase == Mid360ConnectUiState.Phase.TESTING) {
                    OutlinedButton(onClick = viewModel::stopProbe) { Text("Cancel") }
                    Spacer(Modifier.width(8.dp))
                    CircularProgressIndicator(Modifier.height(20.dp).width(20.dp))
                }
                if (state.phase == Mid360ConnectUiState.Phase.READY) {
                    OutlinedButton(onClick = viewModel::reset) { Text("Stop") }
                }
            }

            if (state.phase == Mid360ConnectUiState.Phase.READY) {
                HorizontalDivider()
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Button(onClick = viewModel::saveToProject, enabled = !state.savedToProject) {
                        Text(if (state.savedToProject) "Saved to project" else "Save to project")
                    }
                    if (onContinueToCapture != null) {
                        OutlinedButton(onClick = {
                            // Release the SDK2 singleton first — the capture
                            // engine needs it, and a second kSdk2 driver in
                            // this process gets kBusy.
                            viewModel.stopProbe()
                            onContinueToCapture()
                        }) {
                            Text("Continue to capture")
                        }
                    }
                }
                Text(
                    "Continuing stops this test's engine so the capture session can take over the Livox " +
                        "SDK — its init/uninit and callbacks are process-global, so only one may be live.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun HealthReadout(state: Mid360ConnectUiState, snapshot: com.lidarscan.app.engine.NativeMid360Probe) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            InfoChip(
                text = "Link: ${snapshot.link.label}",
                color = when (snapshot.link) {
                    Mid360LinkState.UP -> MaterialTheme.colorScheme.primary
                    Mid360LinkState.WAITING -> MaterialTheme.colorScheme.tertiary
                    else -> MaterialTheme.colorScheme.error
                },
            )
            InfoChip(
                text = com.lidarscan.app.engine.ScanEngineNative.DeviceState.label(snapshot.deviceState),
                color = when (snapshot.deviceState) {
                    Mid360SelfTest.DEVICE_STATE_STREAMING -> MaterialTheme.colorScheme.primary
                    Mid360SelfTest.DEVICE_STATE_DEGRADED, Mid360SelfTest.DEVICE_STATE_FAULT ->
                        MaterialTheme.colorScheme.error
                    else -> MaterialTheme.colorScheme.secondary
                },
            )
        }
        Text(snapshot.link.detail, style = MaterialTheme.typography.bodySmall)
        Text(
            Mid360SelfTest.healthLine(
                stateLabel = com.lidarscan.app.engine.ScanEngineNative.DeviceState.label(snapshot.deviceState),
                pointsPerSec = snapshot.pointsPerSec,
                imuHz = snapshot.imuHz,
                lossPct = snapshot.lossPct,
                pointsTotal = snapshot.pointsOut,
                drops = snapshot.drops,
                imuUnavailable = state.usingPreboundSocket,
            ),
            style = MaterialTheme.typography.bodyMedium,
            fontFamily = FontFamily.Monospace,
        )
        Text(
            "Datagrams: ${snapshot.datagramsPoint} point / ${snapshot.datagramsImu} IMU · " +
                "${snapshot.datagramBytes} B (counted before parsing)",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (snapshot.bytesButNoPoints) {
            Text(
                "Datagrams are arriving but none decode into points — the wire and addressing are fine, " +
                    "the point port or the packet format is not.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }
        if (snapshot.lossPct > Mid360SelfTest.LOSS_DEGRADED_PCT) {
            Text(
                "Loss is above the ${Mid360SelfTest.LOSS_DEGRADED_PCT}%% the driver treats as degraded — " +
                    "at that level a voxel map starts thinning.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }
        Text(
            "Reference (A3 §7 soak): ${"%,.0f".format(Mid360SelfTest.NOMINAL_SENSOR_POINTS_PER_SEC)} pts/s at the " +
                "sensor, ${"%,.0f".format(Mid360SelfTest.NOMINAL_STORE_POINTS_PER_SEC)} pts/s into the store " +
                "after the live budget, ${Mid360SelfTest.NOMINAL_IMU_HZ} Hz IMU, 0.0000% loss.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun LogCard(state: Mid360ConnectUiState) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Self-test log", style = MaterialTheme.typography.titleSmall)
            state.selfTestLog.takeLast(40).forEach { line ->
                Text(line, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        }
    }
}
