package com.lidarscan.app.ui.connect

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.engine.Mid360LinkState
import com.lidarscan.app.engine.NativeMid360Probe
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.net.EthernetMonitor
import com.lidarscan.app.net.EthernetState
import com.lidarscan.app.net.NetworkBoundUdpSocket
import com.lidarscan.app.net.StaticIpGuidance
import com.lidarscan.core.net.Mid360Field
import com.lidarscan.core.net.Mid360SelfTest
import com.lidarscan.core.net.Mid360Settings
import com.lidarscan.core.net.Mid360Validation
import com.lidarscan.core.net.validateMid360Settings
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Screen state for the Mid-360 connect wizard.
 *
 * [selfTestLog] is append-only and capped — desktop C2 keeps a 500-line
 * timestamped log in the same dialog for the same reason: when a self-test
 * fails on a bench, the sequence of states is the diagnosis, and a single
 * "FAILED" label throws it away.
 */
data class Mid360ConnectUiState(
    val ethernet: EthernetState = EthernetState(),
    val settings: Mid360Settings = Mid360Settings(),
    val validation: Mid360Validation = Mid360Validation(emptyList()),
    val phase: Phase = Phase.IDLE,
    val verdict: Mid360SelfTest.Verdict? = null,
    val snapshot: NativeMid360Probe? = null,
    val selfTestLog: List<String> = emptyList(),
    val savedToProject: Boolean = false,
    val oem: StaticIpGuidance.Oem = StaticIpGuidance.detectOem(),
    val nativeAvailable: Boolean = ScanEngineNative.isAvailable,
) {
    enum class Phase { IDLE, TESTING, READY, FAILED }

    val canRunSelfTest: Boolean
        get() = phase == Phase.IDLE && validation.isUsable && nativeAvailable

    val usingPreboundSocket: Boolean
        get() = settings.backend == Mid360Settings.BACKEND_RAW_UDP

    val linkState: Mid360LinkState
        get() = snapshot?.link ?: Mid360LinkState.DOWN
}

/**
 * Drives the Mid-360 connect wizard (Tech Spec §3.1 Android row).
 *
 * ## Which engine the self-test runs on, and why it is the probe
 *
 * This runs the self-test against B3's standalone `Mid360Probe`
 * (`cpp/mid360_probe.{h,cpp}`), **not** against the capture engine, for two
 * reasons that pull the same way:
 *
 *  * the pre-bound-socket path is only reachable there —
 *    `UdpConfig::prebound_fd` has no representation in the C ABI at all, and
 *    the whole point of the Ethernet plumbing above is to exercise it;
 *  * the wizard is reachable before a project exists, and the capture engine
 *    is per-session.
 *
 * The cost is the Livox SDK2 singleton: an SDK2-backed probe holds a
 * process-global resource, so [stopProbe] is called on `onCleared`, when the
 * screen is left, and before capture starts. [ScanEngineNative.nativeMid360Sdk2Active]
 * is the check that turns a would-be `kBusy` deep inside the driver into a
 * sentence on screen.
 *
 * The capture path is separate and goes through the C ABI — see
 * `RealEngineBridge.connectMid360`.
 */
class Mid360ConnectViewModel(
    private val ethernetMonitor: EthernetMonitor,
    private val projectStore: ProjectStore,
    private val projectId: String?,
) : ViewModel() {

    private val _uiState = MutableStateFlow(Mid360ConnectUiState())
    val uiState: StateFlow<Mid360ConnectUiState> = _uiState.asStateFlow()

    private var probeHandle: Long = 0L
    private var pollJob: Job? = null
    private var socket: NetworkBoundUdpSocket? = null
    private var testStartMs = 0L
    private var baselinePointsOut = 0L

    init {
        ethernetMonitor.start()
        viewModelScope.launch {
            ethernetMonitor.state.collect { eth ->
                _uiState.value = _uiState.value.copy(ethernet = eth).revalidated()
            }
        }
        // Per-project persistence (Tech Spec §3.1's "Save per project"). A
        // project that has been through the wizard before re-opens with the
        // addresses that worked; a fresh one gets the 192.168.1.x defaults.
        viewModelScope.launch {
            val stored = projectId?.let { projectStore.open(it)?.manifest?.mid360 }
            if (stored != null) {
                _uiState.value = _uiState.value.copy(settings = stored, savedToProject = true).revalidated()
            } else {
                // Pre-fill the host IP from the interface's real address when
                // there is one — the default 192.168.1.5 is a guess, and a
                // wrong host IP is the failure that produces no error at all.
                val suggested = _uiState.value.ethernet.suggestedHostIp
                if (suggested != null) {
                    _uiState.value = _uiState.value
                        .copy(settings = _uiState.value.settings.copy(hostIp = suggested))
                        .revalidated()
                }
            }
        }
    }

    // --- field edits --------------------------------------------------------

    fun setLidarIp(value: String) = updateSettings { it.copy(lidarIp = value) }
    fun setHostIp(value: String) = updateSettings { it.copy(hostIp = value) }
    fun setDevicePointPort(value: Int) = updateSettings { it.copy(devicePointPort = value) }
    fun setDeviceImuPort(value: Int) = updateSettings { it.copy(deviceImuPort = value) }
    fun setDeviceCmdPort(value: Int) = updateSettings { it.copy(deviceCmdPort = value) }
    fun setBackend(value: Int) = updateSettings { it.copy(backend = value) }

    /** Adopts the address the Ethernet interface actually holds. */
    fun useInterfaceAddress() {
        val ip = _uiState.value.ethernet.suggestedHostIp ?: return
        updateSettings { it.copy(hostIp = ip) }
    }

    private fun updateSettings(transform: (Mid360Settings) -> Mid360Settings) {
        val next = transform(_uiState.value.settings)
        _uiState.value = _uiState.value.copy(settings = next, savedToProject = false).revalidated()
    }

    private fun Mid360ConnectUiState.revalidated(): Mid360ConnectUiState =
        copy(validation = validateMid360Settings(settings, ethernet.addresses))

    // --- the self-test ------------------------------------------------------

    /**
     * add_device + start → first-data-or-timeout, exactly the shape desktop
     * C2's `onTestDevice()` has. The engine work happens natively; this owns
     * the socket, the clock and the verdict.
     */
    fun runSelfTest() {
        val state = _uiState.value
        if (!state.canRunSelfTest) return
        if (!ScanEngineNative.isAvailable) {
            fail("The native engine library is not loaded — nothing to test against.")
            return
        }
        if (ScanEngineNative.nativeMid360Sdk2Active()) {
            fail(
                "The Livox SDK2 is already in use by another Mid-360 session in this process. " +
                    "Stop it before starting a self-test (the SDK's init/uninit and callbacks are global).",
            )
            return
        }

        stopProbe()
        _uiState.value = state.copy(
            phase = Mid360ConnectUiState.Phase.TESTING,
            verdict = null,
            snapshot = null,
            selfTestLog = state.selfTestLog + log("self-test started"),
        )

        val settings = state.settings
        var preboundFd = -1

        if (settings.backend == Mid360Settings.BACKEND_RAW_UDP) {
            // The pre-bound-socket path: create the socket here, bind it to
            // the Ethernet Network, and hand a dup down. Everything about
            // this half is the reason EthernetMonitor exists.
            val opened = NetworkBoundUdpSocket.open(
                network = state.ethernet.network,
                bindAddress = settings.hostIp.trim(),
                port = settings.hostPointPort,
            )
            val sock = opened.getOrElse { e ->
                fail("Could not create the bound UDP socket: ${e.message}")
                return
            }
            socket = sock
            preboundFd = sock.detachDupForNative()
            if (preboundFd < 0) {
                sock.close()
                socket = null
                fail("Could not duplicate the bound socket's descriptor for the engine.")
                return
            }
            appendLog(
                "bound UDP socket on ${sock.boundAddress}:${sock.boundPort}" +
                    (if (sock.networkBound) " → Ethernet network" else " (NOT bound to a Network — none available)") +
                    ", fd handed to the engine",
            )
        }

        probeHandle = ScanEngineNative.nativeMid360ProbeCreate()
        if (probeHandle == 0L) {
            releaseSocket()
            fail("Could not allocate the native Mid-360 probe.")
            return
        }

        val started = ScanEngineNative.nativeMid360ProbeStart(
            probeHandle,
            settings.lidarIp.trim(),
            settings.hostIp.trim(),
            settings.backend,
            settings.devicePointPort,
            settings.deviceImuPort,
            settings.deviceCmdPort,
            settings.hostPointPort,
            settings.hostImuPort,
            settings.hostCmdPort,
            preboundFd,
            /* publishImu = */ true,
        )
        if (!started) {
            val err = ScanEngineNative.nativeMid360ProbeLastError(probeHandle)
            stopProbe()
            fail(if (err.isBlank()) "The engine refused to start the Mid-360." else err)
            return
        }
        appendLog(
            "Mid-360 ${settings.lidarIp} → host ${settings.hostIp}, " +
                (if (settings.backend == Mid360Settings.BACKEND_SDK2) "SDK2 backend" else "raw-UDP backend") +
                ", session started (preview, not recording)",
        )

        testStartMs = System.currentTimeMillis()
        baselinePointsOut = 0L
        pollJob?.cancel()
        pollJob = viewModelScope.launch {
            while (true) {
                delay(POLL_INTERVAL_MS)
                val handle = probeHandle
                if (handle == 0L) break
                val snap = ScanEngineNative.nativeMid360ProbeSnapshot(handle) ?: continue
                val elapsed = System.currentTimeMillis() - testStartMs
                val sample = Mid360SelfTest.Sample(
                    elapsedMs = elapsed,
                    pointsOut = snap.pointsOut,
                    baselinePointsOut = baselinePointsOut,
                    pointDatagrams = snap.datagramsPoint,
                    pointsPerSec = snap.pointsPerSec,
                    imuHz = snap.imuHz,
                    lossPct = snap.lossPct,
                    deviceState = snap.deviceState,
                    deviceStateLabel = ScanEngineNative.DeviceState.label(snap.deviceState),
                    imuUnavailable = _uiState.value.usingPreboundSocket,
                )
                when (val verdict = Mid360SelfTest.evaluate(sample)) {
                    is Mid360SelfTest.Verdict.Testing ->
                        _uiState.value = _uiState.value.copy(verdict = verdict, snapshot = snap)

                    is Mid360SelfTest.Verdict.Passed -> {
                        _uiState.value = _uiState.value.copy(
                            verdict = verdict,
                            snapshot = snap,
                            phase = Mid360ConnectUiState.Phase.READY,
                            selfTestLog = _uiState.value.selfTestLog + log("self-test passed: ${verdict.detail}"),
                        )
                        // The device is deliberately LEFT STREAMING after a
                        // pass — same as desktop C2, which keeps the session
                        // and device up so the health readout stays live and
                        // the operator can watch loss % settle before
                        // committing to a capture.
                        break
                    }

                    is Mid360SelfTest.Verdict.Failed -> {
                        _uiState.value = _uiState.value.copy(
                            verdict = verdict,
                            snapshot = snap,
                            phase = Mid360ConnectUiState.Phase.FAILED,
                            selfTestLog = _uiState.value.selfTestLog + log("self-test failed: ${verdict.detail}"),
                        )
                        stopProbe()
                        break
                    }
                }
            }
        }
    }

    /** Keeps the health readout live after a pass; called by the screen's poll while READY. */
    fun refreshSnapshot() {
        val handle = probeHandle
        if (handle == 0L) return
        val snap = ScanEngineNative.nativeMid360ProbeSnapshot(handle) ?: return
        _uiState.value = _uiState.value.copy(snapshot = snap)
    }

    /**
     * Tears the probe down. **Call before starting a capture**: the SDK2
     * backend is a process-wide singleton and the capture engine needs it.
     */
    fun stopProbe() {
        pollJob?.cancel()
        pollJob = null
        if (probeHandle != 0L) {
            ScanEngineNative.nativeMid360ProbeStop(probeHandle)
            ScanEngineNative.nativeMid360ProbeDestroy(probeHandle)
            probeHandle = 0L
        }
        releaseSocket()
        if (_uiState.value.phase == Mid360ConnectUiState.Phase.TESTING) {
            _uiState.value = _uiState.value.copy(phase = Mid360ConnectUiState.Phase.IDLE)
        }
    }

    fun reset() {
        stopProbe()
        _uiState.value = _uiState.value.copy(
            phase = Mid360ConnectUiState.Phase.IDLE,
            verdict = null,
            snapshot = null,
        )
    }

    /**
     * Persists the settings into the project's `manifest.json` — and only
     * after a self-test has passed, mirroring desktop C2's rule that a config
     * which failed to add is never saved. Saving a set of addresses that
     * demonstrably do not work would make the next session's pre-fill worse
     * than the defaults.
     */
    fun saveToProject() {
        val id = projectId ?: return
        if (_uiState.value.phase != Mid360ConnectUiState.Phase.READY) return
        val settings = _uiState.value.settings
        viewModelScope.launch {
            // Only the mid360 field is written — deliberately NOT `sensor`.
            // The sensor is chosen at project creation and a `.lscan`'s
            // recorded streams are shaped by it; rewriting it from a network
            // wizard would silently relabel a capture.
            val updated = projectStore.updateManifest(id) { it.copy(mid360 = settings) }
            _uiState.value = _uiState.value.copy(
                savedToProject = updated != null,
                selfTestLog = _uiState.value.selfTestLog +
                    log(if (updated != null) "saved to project manifest" else "could not save to project manifest"),
            )
        }
    }

    /** `"<lidarIp>|<hostIp>"` for [com.lidarscan.core.engine.EngineTarget.transportHint]. */
    fun engineTransportHint(): String =
        "${_uiState.value.settings.lidarIp.trim()}|${_uiState.value.settings.hostIp.trim()}"

    fun issueFor(field: Mid360Field) = _uiState.value.validation.firstFor(field)

    override fun onCleared() {
        stopProbe()
        ethernetMonitor.stop()
        super.onCleared()
    }

    // --- helpers ------------------------------------------------------------

    private fun releaseSocket() {
        // The dup handed to native is closed by Mid360Probe::stop(); this
        // closes only the original this object kept. See
        // NetworkBoundUdpSocket's ownership note.
        socket?.close()
        socket = null
    }

    private fun fail(message: String) {
        _uiState.value = _uiState.value.copy(
            phase = Mid360ConnectUiState.Phase.FAILED,
            verdict = Mid360SelfTest.Verdict.Failed(0L, message, ""),
            selfTestLog = _uiState.value.selfTestLog + log(message),
        )
    }

    private fun appendLog(line: String) {
        _uiState.value = _uiState.value.copy(selfTestLog = _uiState.value.selfTestLog + log(line))
    }

    private fun log(line: String): String {
        val elapsed = if (testStartMs == 0L) 0.0 else (System.currentTimeMillis() - testStartMs) / 1000.0
        return "%6.2fs  %s".format(elapsed, line)
    }

    private companion object {
        /** Desktop C2 polls its health/self-test timer at 300 ms; matching it keeps the two readouts comparable. */
        const val POLL_INTERVAL_MS = 300L
    }
}
