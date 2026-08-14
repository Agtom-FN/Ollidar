package com.lidarscan.app.ui.connect

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.ConnectWizardState
import com.lidarscan.core.engine.D6ConnectController
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/**
 * B2's D6 connect wizard: lists CH340/serial USB devices, drives the
 * runtime permission flow, and connects the engine once permission is
 * granted. State machine logic lives in [D6ConnectController] (`:core`,
 * plain Kotlin, JVM-tested against `FakeEngineBridge`); this ViewModel is
 * the Android-specific glue — `UsbManager`/`UsbSerialDriver` enumeration and
 * the permission dialog — that [D6ConnectController] deliberately knows
 * nothing about.
 */
class ConnectWizardViewModel(
    private val registry: D6UsbConnectionRegistry,
    private val controller: D6ConnectController,
    engineBridge: EngineBridge,
    usbAttachEvents: SharedFlow<Unit>? = null,
) : ViewModel() {

    private val _drivers = MutableStateFlow<List<UsbSerialDriver>>(emptyList())
    val drivers: StateFlow<List<UsbSerialDriver>> = _drivers.asStateFlow()

    val wizardState: StateFlow<ConnectWizardState> = controller.state

    val deviceHealth: StateFlow<DeviceHealth?> = engineBridge.deviceHealth.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = null,
    )

    init {
        refreshDevices()
        usbAttachEvents?.onEach { onUsbEvent() }?.launchIn(viewModelScope)
    }

    fun refreshDevices() {
        _drivers.value = registry.findDrivers()
    }

    /** Called when the attach/detach broadcast fires (see MainActivity) — re-scan and drop a lost device's state. */
    fun onUsbEvent() {
        refreshDevices()
        if (_drivers.value.none { it.device.deviceName == currentDevicePathOrNull() }) {
            controller.onDeviceLost()
        }
    }

    fun connect(driver: UsbSerialDriver) {
        val devicePath = driver.device.deviceName
        controller.onDeviceFound(devicePath)
        viewModelScope.launch {
            if (!registry.hasPermission(driver)) {
                val granted = registry.requestPermission(driver)
                if (!granted) {
                    controller.onPermissionDenied()
                    return@launch
                }
            }
            runCatching { registry.open(driver) }
                .onSuccess { controller.onPermissionGranted(devicePath) }
                .onFailure { controller.onPermissionDenied() }
        }
    }

    fun retry(devicePath: String) = controller.retry(devicePath)

    fun disconnect() = controller.disconnect()

    private fun currentDevicePathOrNull(): String? = when (val s = wizardState.value) {
        is ConnectWizardState.AwaitingPermission -> s.devicePath
        is ConnectWizardState.Connecting -> s.devicePath
        is ConnectWizardState.Connected -> s.devicePath
        is ConnectWizardState.Failed -> s.devicePath
        ConnectWizardState.NoDevice -> null
    }
}
