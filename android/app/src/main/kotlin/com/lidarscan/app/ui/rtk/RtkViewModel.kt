package com.lidarscan.app.ui.rtk

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.rtk.RtkManager
import com.lidarscan.app.rtk.RtkRoverConnection
import com.lidarscan.core.gnss.CaptureGate
import com.lidarscan.core.gnss.CaptureGateVerdict
import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.gnss.GeorefRecord
import com.lidarscan.core.gnss.GnssFixSnapshot
import com.lidarscan.core.gnss.GnssStatsSnapshot
import com.lidarscan.core.gnss.NtripSettings
import com.lidarscan.core.gnss.NtripStatsSnapshot
import com.lidarscan.core.gnss.evaluateCaptureGate
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

data class RtkUiState(
    val bluetoothAvailable: Boolean = false,
    val bluetoothEnabled: Boolean = false,
    val permissionGranted: Boolean = false,
    val bondedRovers: List<RtkRoverConnection.BondedRover> = emptyList(),
    val connectedAddress: String? = null,
    val connecting: Boolean = false,
    val fix: GnssFixSnapshot = GnssFixSnapshot(),
    val stats: GnssStatsSnapshot = GnssStatsSnapshot(),
    val ntrip: NtripStatsSnapshot = NtripStatsSnapshot(),
    val ntripSettings: NtripSettings = NtripSettings(),
    val ntripBusy: Boolean = false,
    val mountpoints: List<RtkManager.Mountpoint> = emptyList(),
    val georef: GeorefRecord? = null,
    val bytesIn: Long = 0,
    val rtcmOut: Long = 0,
    val gate: CaptureGate = CaptureGate(CaptureGateVerdict.OK, "", ""),
    val message: String? = null,
)

/** B9's screen state. The manager is engine-lifetime; this only observes it. */
class RtkViewModel(
    private val container: AppContainer,
    private val settings: SettingsRepository,
) : ViewModel() {

    private val manager: RtkManager = container.rtkManager

    private val _uiState = MutableStateFlow(RtkUiState())
    val uiState: StateFlow<RtkUiState> = _uiState.asStateFlow()

    init {
        refreshDevices()
        viewModelScope.launch {
            settings.settings.collect { s -> _uiState.value = _uiState.value.copy(ntripSettings = s.ntrip) }
        }
        viewModelScope.launch { manager.fix.collect { onFix(it) } }
        viewModelScope.launch { manager.stats.collect { _uiState.value = _uiState.value.copy(stats = it) } }
        viewModelScope.launch { manager.ntrip.collect { _uiState.value = _uiState.value.copy(ntrip = it) } }
        viewModelScope.launch { manager.message.collect { m -> m?.let { _uiState.value = _uiState.value.copy(message = it) } } }
        viewModelScope.launch {
            while (true) {
                _uiState.value = _uiState.value.copy(
                    bytesIn = manager.rover.bytesIn,
                    rtcmOut = manager.rover.rtcmBytesOut,
                    georef = manager.georefRecord(container.currentEngineHandle()),
                )
                delay(1500)
            }
        }
    }

    private fun onFix(f: GnssFixSnapshot) {
        // The RTK screen itself has no project, so the gate is evaluated at the
        // profile-independent default: warn below RTK Float, never block. Each
        // Capture screen re-evaluates it with its own project's profile, which
        // is where blocking actually applies.
        _uiState.value = _uiState.value.copy(
            fix = f,
            gate = evaluateCaptureGate(
                fix = f.fix,
                required = FixType.RTK_FLOAT,
                enforce = false,
                rtkIsTrajectorySource = false,
            ),
        )
    }

    fun requiredPermission(): String? = manager.rover.requiredPermission

    fun onPermissionResult(granted: Boolean) {
        _uiState.value = _uiState.value.copy(
            permissionGranted = granted,
            message = if (granted) null else "Bluetooth access was refused, so no rover can be opened.",
        )
        if (granted) refreshDevices()
    }

    fun refreshDevices() {
        val r = manager.rover
        val needsPermission = r.requiredPermission != null
        val bonded = r.bondedRovers()
        _uiState.value = _uiState.value.copy(
            bluetoothAvailable = r.isBluetoothAvailable,
            bluetoothEnabled = r.isBluetoothEnabled,
            // An empty bonded list under a permission requirement is
            // indistinguishable from "no paired devices" via the platform API,
            // so a successful non-empty read is treated as proof of the grant
            // and the UI otherwise offers the request.
            permissionGranted = !needsPermission || bonded.isNotEmpty() || _uiState.value.permissionGranted,
            bondedRovers = bonded,
        )
    }

    fun connectRover(address: String) {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(connecting = true)
            val h = container.currentEngineHandle()
            val r = manager.connectRover(address, h)
            _uiState.value = _uiState.value.copy(
                connecting = false,
                connectedAddress = if (r.isSuccess) address else null,
                message = r.exceptionOrNull()?.message ?: "Rover connected.",
            )
        }
    }

    fun disconnectRover() {
        manager.disconnectRover()
        _uiState.value = _uiState.value.copy(connectedAddress = null)
    }

    fun updateNtrip(s: NtripSettings) {
        _uiState.value = _uiState.value.copy(ntripSettings = s)
        viewModelScope.launch { settings.setNtrip(s) }
    }

    fun connectNtrip() {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(ntripBusy = true)
            val r = manager.connectNtrip(_uiState.value.ntripSettings, container.currentEngineHandle())
            _uiState.value = _uiState.value.copy(
                ntripBusy = false,
                message = r.exceptionOrNull()?.message ?: "Connected to the caster.",
            )
        }
    }

    fun disconnectNtrip() {
        manager.disconnectNtrip()
        _uiState.value = _uiState.value.copy(message = "Corrections stopped.")
    }

    fun fetchSourcetable() {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(ntripBusy = true)
            val r = manager.fetchSourcetable(_uiState.value.ntripSettings)
            _uiState.value = _uiState.value.copy(
                ntripBusy = false,
                mountpoints = r.getOrDefault(emptyList()),
                message = r.exceptionOrNull()?.message,
            )
        }
    }

    fun dismissMessage() {
        _uiState.value = _uiState.value.copy(message = null)
    }
}
