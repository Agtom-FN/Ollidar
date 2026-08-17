package com.lidarscan.app.ui.connect

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.app.usb.D6AutoProbe
import com.lidarscan.app.usb.D6AutoProbeResult
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.ConnectWizardState
import com.lidarscan.core.engine.D6ConnectController
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import kotlinx.coroutines.Job
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
 *
 * AUTO-DETECT: [refreshDevices] also drives [autoProbe] — when exactly one
 * serial device is attached, it is opened and read for a short window to
 * confirm the `AA 55` D6 preamble ([D6AutoProbe]) *before* this wizard
 * connects the engine to it, so a passing signature check goes straight to
 * [ConnectWizardState.Connected] (the health panel) with no tap needed. The
 * manual "tap a device to Connect" path in [connect] is unchanged and stays
 * available throughout — including while an auto-probe is in flight, other
 * than for the exact device currently being probed (see [autoProbingDevicePath]).
 */
class ConnectWizardViewModel(
    private val registry: D6UsbConnectionRegistry,
    private val controller: D6ConnectController,
    engineBridge: EngineBridge,
    usbAttachEvents: SharedFlow<Unit>? = null,
    private val autoProbe: D6AutoProbe = D6AutoProbe(registry),
) : ViewModel() {

    private val _drivers = MutableStateFlow<List<UsbSerialDriver>>(emptyList())
    val drivers: StateFlow<List<UsbSerialDriver>> = _drivers.asStateFlow()

    val wizardState: StateFlow<ConnectWizardState> = controller.state

    val deviceHealth: StateFlow<DeviceHealth?> = engineBridge.deviceHealth.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = null,
    )

    /** Non-null while [autoProbe] is reading a specific device's signature — see the class doc. */
    private val _autoProbingDevicePath = MutableStateFlow<String?>(null)
    val autoProbingDevicePath: StateFlow<String?> = _autoProbingDevicePath.asStateFlow()

    private var autoProbeJob: Job? = null

    init {
        refreshDevices()
        usbAttachEvents?.onEach { onUsbEvent() }?.launchIn(viewModelScope)
    }

    fun refreshDevices() {
        _drivers.value = registry.findDrivers()
        maybeAutoProbe()
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

    /**
     * AUTO-DETECT: probes the sole attached device's serial signature and,
     * if it looks like a D6, connects straight through without a manual
     * "Connect" tap. Deliberately a no-op when more than one serial device
     * is attached (which one to guess is ambiguous — a D6 *and* a UM982 on
     * the same rig is exactly the case `android/NOTES.md` calls out) or when
     * the wizard is already past [ConnectWizardState.NoDevice].
     */
    private fun maybeAutoProbe() {
        if (wizardState.value != ConnectWizardState.NoDevice) return
        val candidates = _drivers.value
        if (candidates.size != 1) return
        val driver = candidates.first()
        val devicePath = driver.device.deviceName
        if (_autoProbingDevicePath.value == devicePath) return // already probing this one

        autoProbeJob?.cancel()
        _autoProbingDevicePath.value = devicePath
        autoProbeJob = viewModelScope.launch {
            val result = autoProbe.probe(driver)
            _autoProbingDevicePath.value = null
            if (result is D6AutoProbeResult.Identified && wizardState.value == ConnectWizardState.NoDevice) {
                connectAlreadyOpen(result.devicePath)
            }
            // NotIdentified/PermissionDenied/Error: leave the device in the
            // list — the manual "Connect" button is still there.
        }
    }

    /** Hands an already-open connection (from [autoProbe]) straight to the controller, with no second `registry.open`. */
    private fun connectAlreadyOpen(devicePath: String) {
        controller.onDeviceFound(devicePath)
        controller.onPermissionGranted(devicePath)
    }

    private fun currentDevicePathOrNull(): String? = when (val s = wizardState.value) {
        is ConnectWizardState.AwaitingPermission -> s.devicePath
        is ConnectWizardState.Connecting -> s.devicePath
        is ConnectWizardState.Connected -> s.devicePath
        is ConnectWizardState.Failed -> s.devicePath
        ConnectWizardState.NoDevice -> null
    }

    override fun onCleared() {
        autoProbeJob?.cancel()
        super.onCleared()
    }
}
