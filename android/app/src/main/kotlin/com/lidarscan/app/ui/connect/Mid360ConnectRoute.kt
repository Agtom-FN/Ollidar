package com.lidarscan.app.ui.connect

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.net.UsbDeviceNames

/**
 * Nav entry point for the Mid-360 connect wizard.
 *
 * [projectId] is nullable on purpose: the wizard is useful before a project
 * exists (it is a transport check), and only the per-project *save* needs
 * one. `key` on [viewModel] is what keeps the two entry points from sharing a
 * ViewModel — and therefore a live native probe holding the SDK2 singleton —
 * across a navigation.
 */
@Composable
fun Mid360ConnectRoute(
    container: AppContainer,
    projectId: String?,
    onBack: () -> Unit,
    onContinueToCapture: (() -> Unit)? = null,
) {
    // ROUND 25 item 118. Built here rather than in AppContainer because it is
    // stateless, permission-free and used by exactly one screen — and because
    // it holds the application context, so a wizard-scoped instance leaks
    // nothing.
    val context = LocalContext.current
    val usbDevices = remember(context) { UsbDeviceNames(context) }

    val vm: Mid360ConnectViewModel = viewModel(
        key = "mid360-connect-${projectId ?: "no-project"}",
        factory = viewModelFactory {
            initializer {
                Mid360ConnectViewModel(
                    ethernetMonitor = container.ethernetMonitor,
                    projectStore = container.projectStore,
                    projectId = projectId,
                    settingsRepository = container.settingsRepository,
                    detector = container.mid360HeartbeatDetector,
                    usbDeviceNames = usbDevices::list,
                    // ROUND 25 item 118 (owner amendment): the container's one
                    // sweeper, so its rate limiter is shared with the Capture
                    // tab's auto-detect and the Settings row rather than each
                    // keeping a private window.
                    connectionDebug = container.connectionDebugSweeper,
                )
            }
        },
    )
    Mid360ConnectScreen(
        viewModel = vm,
        onBack = onBack,
        onContinueToCapture = onContinueToCapture,
    )
}
