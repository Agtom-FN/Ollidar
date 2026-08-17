package com.lidarscan.app.ui.connect

import androidx.compose.runtime.Composable
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer

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
