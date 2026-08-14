package com.lidarscan.core.engine

import com.lidarscan.core.model.SensorType
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Pure-Kotlin state machine for the D6 USB connect wizard (B2, Tech Spec
 * §3.1's Android row: "usb-serial-for-android (CH340) → JNI; permission/
 * attach flow"). Deliberately has **no** Android/`UsbManager` dependency —
 * the real wizard screen (`com.lidarscan.app.ui.connect`, `:app`) feeds this
 * class USB lifecycle events (device found/lost, permission granted/denied)
 * and it owns only "what does the wizard show, and when does it call
 * [EngineBridge.connect]/[EngineBridge.disconnect]". That split is what
 * makes it exercisable in a plain JVM test against [FakeEngineBridge] (see
 * `D6ConnectControllerTest`) with no emulator/instrumentation — the same
 * shape as B1's `FakeEngineBridgeTest`, one layer up.
 *
 * [RealEngineBridge]'s USB glue (`:app`) is the only thing that knows what a
 * `UsbDevice`/`UsbSerialPort` is; by the time this controller sees a device,
 * it is already reduced to an opaque `devicePath` string, which becomes
 * [EngineTarget.transportHint].
 */
class D6ConnectController(
    private val bridge: EngineBridge,
    private val scope: CoroutineScope,
) {
    private val _state = MutableStateFlow<ConnectWizardState>(ConnectWizardState.NoDevice)
    val state: StateFlow<ConnectWizardState> = _state.asStateFlow()

    /** A CH340/D6-shaped USB device showed up (attach intent, or the initial device scan). */
    fun onDeviceFound(devicePath: String) {
        if (_state.value is ConnectWizardState.Connecting || _state.value is ConnectWizardState.Connected) return
        _state.value = ConnectWizardState.AwaitingPermission(devicePath)
    }

    /** The device was physically detached, or the OS revoked it (deliberately safe to call at any state). */
    fun onDeviceLost() {
        if (_state.value is ConnectWizardState.Connected) {
            scope.launch { bridge.disconnect() }
        }
        _state.value = ConnectWizardState.NoDevice
    }

    fun onPermissionDenied() {
        val current = _state.value
        if (current is ConnectWizardState.AwaitingPermission) {
            _state.value = ConnectWizardState.Failed(current.devicePath, "USB permission denied")
        }
    }

    /** Permission granted for [devicePath] — connect the engine to it over D6/serial. */
    fun onPermissionGranted(devicePath: String) {
        _state.value = ConnectWizardState.Connecting(devicePath)
        scope.launch {
            val result = bridge.connect(EngineTarget(SensorType.COIN_D6, transportHint = devicePath))
            _state.value = result.fold(
                onSuccess = { ConnectWizardState.Connected(devicePath) },
                onFailure = { e -> ConnectWizardState.Failed(devicePath, e.message ?: "connect failed") },
            )
        }
    }

    /** Re-attempt a connect after [ConnectWizardState.Failed], reusing the same device path. */
    fun retry(devicePath: String) {
        onPermissionGranted(devicePath)
    }

    fun disconnect() {
        scope.launch { bridge.disconnect() }
        _state.value = ConnectWizardState.NoDevice
    }
}

sealed interface ConnectWizardState {
    data object NoDevice : ConnectWizardState
    data class AwaitingPermission(val devicePath: String) : ConnectWizardState
    data class Connecting(val devicePath: String) : ConnectWizardState
    data class Connected(val devicePath: String) : ConnectWizardState
    data class Failed(val devicePath: String, val reason: String) : ConnectWizardState
}
