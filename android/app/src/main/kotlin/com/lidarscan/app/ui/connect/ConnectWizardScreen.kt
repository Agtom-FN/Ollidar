package com.lidarscan.app.ui.connect

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
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
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.engine.ConnectWizardState
import com.lidarscan.core.engine.DeviceHealth

@Composable
fun ConnectWizardRoute(
    container: AppContainer,
    onBack: () -> Unit,
    onConnected: () -> Unit,
    onOpenMid360: () -> Unit = {},
) {
    val viewModel: ConnectWizardViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ConnectWizardViewModel(
                    registry = container.d6UsbConnectionRegistry,
                    controller = container.d6ConnectController,
                    engineBridge = container.engineBridge,
                    usbAttachEvents = container.usbAttachEvents,
                )
            }
        },
    )
    val drivers by viewModel.drivers.collectAsStateWithLifecycle()
    val wizardState by viewModel.wizardState.collectAsStateWithLifecycle()
    val health by viewModel.deviceHealth.collectAsStateWithLifecycle()

    ConnectWizardScreen(
        drivers = drivers,
        wizardState = wizardState,
        health = health,
        onRefresh = viewModel::refreshDevices,
        onConnect = viewModel::connect,
        onRetry = viewModel::retry,
        onBack = onBack,
        onDone = onConnected,
        onOpenMid360 = onOpenMid360,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConnectWizardScreen(
    drivers: List<UsbSerialDriver>,
    wizardState: ConnectWizardState,
    health: DeviceHealth?,
    onRefresh: () -> Unit,
    onConnect: (UsbSerialDriver) -> Unit,
    onRetry: (String) -> Unit,
    onBack: () -> Unit,
    onDone: () -> Unit,
    // B3: the Mid-360 is a completely different transport (Ethernet, not
    // USB-serial) with its own wizard, so this screen offers a door rather
    // than growing a tab. Defaulted so existing call sites/tests do not have
    // to change.
    onOpenMid360: () -> Unit = {},
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Connect D6") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
        ) {
            Mid360DoorCard(onOpenMid360)
            Spacer(Modifier.height(16.dp))

            when (wizardState) {
                is ConnectWizardState.Connected -> {
                    ConnectedCard(devicePath = wizardState.devicePath, onDone = onDone)
                    Spacer(Modifier.height(16.dp))
                    HealthPanel(health)
                }
                is ConnectWizardState.Connecting -> {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(modifier = Modifier.height(20.dp))
                        Spacer(Modifier.height(0.dp))
                        Text("  Connecting to ${wizardState.devicePath}…")
                    }
                }
                is ConnectWizardState.AwaitingPermission -> {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(modifier = Modifier.height(20.dp))
                        Text("  Waiting for USB permission…")
                    }
                }
                is ConnectWizardState.Failed -> {
                    FailedCard(wizardState) { onRetry(wizardState.devicePath) }
                }
                ConnectWizardState.NoDevice -> {
                    DeviceList(drivers = drivers, onRefresh = onRefresh, onConnect = onConnect)
                }
            }
        }
    }
}

@Composable
private fun DeviceList(
    drivers: List<UsbSerialDriver>,
    onRefresh: () -> Unit,
    onConnect: (UsbSerialDriver) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("USB serial devices", style = MaterialTheme.typography.titleSmall)
        OutlinedButton(onClick = onRefresh) { Text("Rescan") }
    }
    Spacer(Modifier.height(12.dp))

    if (drivers.isEmpty()) {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("No serial device found.", style = MaterialTheme.typography.bodyMedium)
                Spacer(Modifier.height(4.dp))
                Text(
                    "Plug in the D6 over USB-C OTG (CH340 adapter) and tap Rescan, " +
                        "or attaching it will bring you back here automatically.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    } else {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            drivers.forEach { driver ->
                Card(modifier = Modifier.fillMaxWidth(), onClick = { onConnect(driver) }) {
                    Row(
                        modifier = Modifier.padding(16.dp).fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(Icons.Filled.Usb, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                        Spacer(Modifier.height(0.dp))
                        Column(Modifier.weight(1f).padding(start = 12.dp)) {
                            Text(driver.device.deviceName, style = MaterialTheme.typography.bodyMedium)
                            Text(
                                "VID ${driver.device.vendorId} / PID ${driver.device.productId}",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        Button(onClick = { onConnect(driver) }) { Text("Connect") }
                    }
                }
            }
        }
    }
}

@Composable
private fun ConnectedCard(devicePath: String, onDone: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = Color(0xFF2E7D32))
                Spacer(Modifier.height(0.dp))
                Text("  Connected — $devicePath", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(12.dp))
            Button(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text("Go to Capture") }
        }
    }
}

@Composable
private fun FailedCard(state: ConnectWizardState.Failed, onRetry: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.Error, contentDescription = null, tint = MaterialTheme.colorScheme.error)
                Spacer(Modifier.height(0.dp))
                Text("  Connect failed", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(8.dp))
            Text(state.reason, style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(12.dp))
            Button(onClick = onRetry, modifier = Modifier.fillMaxWidth()) { Text("Retry") }
        }
    }
}

/** pts/s, rotation Hz, checksum pass rate, state — everything `scan_device_health` exposes (B2 brief). */
@Composable
private fun HealthPanel(health: DeviceHealth?) {
    Text("Device health", style = MaterialTheme.typography.titleSmall)
    Spacer(Modifier.height(8.dp))
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            if (health == null) {
                Text("Waiting for first sample…", color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                HealthRow("State", ScanEngineNative.DeviceState.label(health.state))
                HealthRow("Points/sec", "%.0f".format(health.pointsPerSec))
                HealthRow("Rotation", "%.2f Hz".format(health.rotationHz))
                HealthRow("Checksum pass rate", "%.2f%%".format(health.checksumPassRate * 100.0))
                HealthRow("Packets ok / bad", "${health.packetsOk} / ${health.packetsBad}")
                HealthRow("Bytes in", health.bytesIn.toString())
                if (health.lastError != 0) {
                    HealthRow("Last error", ScanEngineNative.nativeErrorStr(health.lastError))
                }
            }
        }
    }
}

@Composable
private fun HealthRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodyMedium)
    }
}


/**
 * B3: the Mid-360 lives on Ethernet, not USB-serial, so it gets its own
 * wizard rather than a second tab here — nothing on this screen (driver
 * enumeration, USB permission, CH340 detection) applies to it.
 */
@Composable
private fun Mid360DoorCard(onOpenMid360: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Livox Mid-360 (Ethernet)", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(4.dp))
            Text(
                "USB-C Ethernet adapter, static IP, and a pre-capture self-test. None of the USB-serial " +
                    "setup below applies to it.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(8.dp))
            OutlinedButton(onClick = onOpenMid360) { Text("Open Mid-360 connect") }
        }
    }
}
