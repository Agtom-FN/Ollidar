package com.lidarscan.app.ui.capture

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.NativePointCloudProvider
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.core.engine.CaptureState
import com.lidarscan.core.engine.ConnectionState
import com.lidarscan.core.engine.DeviceHealth
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineEvent
import com.lidarscan.core.engine.EngineTarget
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * ROUND 5 manual fallback: one attached serial device, reduced to what the inline
 * panel shows and what the engine needs. Deliberately not a `UsbSerialDriver` —
 * the ViewModel stays free of the USB types (same line B2's `D6ConnectController`
 * draws), and the screen only ever renders strings.
 */
data class ManualSerialDevice(val path: String, val label: String)

data class CaptureStats(
    val pointsCaptured: Long = 0,
    val elapsedMillis: Long = 0,
    val pointsPerSecond: Double = 0.0,
    val recordingSizeBytes: Long = 0,
)

sealed interface CaptureUiState {
    data object Loading : CaptureUiState
    data class Loaded(val project: Project) : CaptureUiState
    data object NotFound : CaptureUiState

    /**
     * ROUND 5 (items 8 + 9): the Capture tab opened with **no project**, which
     * is now its only normal state — the tab exists to create new scans, and
     * Start is what creates one. [autoName] is what the name field shows as its
     * placeholder, i.e. what the project will be called if nothing is typed.
     */
    data class NewScan(val autoName: String) : CaptureUiState
}

/**
 * B4: drives the full Capture screen over [EngineBridge] — live 3D view
 * (via [pointCloudSource], only populated when the bridge also implements
 * [NativePointCloudProvider]), status strip, Live-SLAM toggle, and
 * pause/resume/stop with a session-summary sheet.
 *
 * [isReplay] selects the "Replay synthetic capture" debug acceptance path
 * (see `com.lidarscan.app.engine.ReplayEngineBridge`): the ViewModel
 * auto-connects on init (there is no USB wizard for a replay session) and
 * [isReplaySession] tells the screen to hide the Pause control (replay has
 * no pause/resume primitive — see `replay_engine.h`'s header comment).
 */
class CaptureViewModel(
    private val engineBridge: EngineBridge,
    private val projectStore: ProjectStore,
    /**
     * ROUND 5: **null on the Capture tab.** The tab creates a new project on
     * Start (item 9), so it opens with nothing to load; a non-null id is now
     * only the replay/deep-link path, which records into (or replays from) a
     * project that already exists.
     */
    private val projectId: String? = null,
    val isReplay: Boolean = false,
    /**
     * B7/B8. Null for a replay session and for any build where ARCore is not
     * wanted — the whole AR path (overlay, pose push, keyframes) is then
     * simply absent rather than half-present.
     */
    private val arController: com.lidarscan.app.ar.CaptureArController? = null,
    private val engineHandleProvider: () -> Long = { 0L },
    private val mountCalibrationFor: (com.lidarscan.core.model.SensorType) -> com.lidarscan.core.calib.MountCalibration? = { null },
    /**
     * B9: A10's georeferencing solution for the engine handle, read at capture
     * stop and snapshotted into the manifest. Defaults to "no georeference",
     * which is what a replay session and a build with no RTK have.
     */
    private val georefSnapshotProvider: (Long) -> com.lidarscan.core.gnss.GeorefRecord? = { null },
    /**
     * ROUND 5 (item 7): the auto-detect probes this screen races on entry. Empty
     * for a replay session (nothing to detect) and for a project-scoped entry.
     */
    private val autoDetectors: List<com.lidarscan.core.capture.SensorAutoDetector> = emptyList(),
    /** Claims (and consumes) the next scan series number — DataStore-backed in the app. */
    private val claimSeriesNumber: suspend () -> Int = { 1 },
    /** Peeks at what the next series number *would* be, for the name field's placeholder. */
    private val peekSeriesNumber: suspend () -> Int = { 1 },
    private val clock: () -> Long = System::currentTimeMillis,
    /**
     * ROUND 5 manual fallback: the serial devices currently attached, for the
     * inline manual panel's list. A supplier rather than a snapshot — devices
     * come and go while the panel is open.
     */
    private val attachedSerialDevices: () -> List<ManualSerialDevice> = { emptyList() },
    /**
     * Opens (and permissions) a serial port so the engine can be pointed at it —
     * `RealEngineBridge.connect` refuses a path the registry has no open
     * connection for, which is why the manual D6 path needs this step and the
     * auto-detect path does not (its probe already left the port open).
     */
    private val openSerialPort: suspend (String) -> Result<Unit> = { Result.failure(IllegalStateException("no USB")) },
    /**
     * Addresses the manual Mid-360 fields open with — the last auto-detected pair
     * (AUTO-DETECT §3's DataStore keys), else the factory defaults. Suspend and
     * read in `init` rather than a plain getter, because it is a DataStore read
     * and construction happens on the main thread.
     */
    private val manualMid360Defaults: suspend () -> Pair<String, String> = {
        com.lidarscan.core.net.Mid360Settings.DEFAULT_LIDAR_IP to com.lidarscan.core.net.Mid360Settings.DEFAULT_HOST_IP
    },
    /**
     * ROUND 5.2: the live RTK fix, so the georeference policy can rank a rover
     * above the phone. Null (a flow of "no fix") for a replay session.
     */
    private val rtkFix: StateFlow<com.lidarscan.core.gnss.GnssFixSnapshot>? = null,
    /**
     * ROUND 5.2: the phone's own location at ~1 Hz. Collected **only** while a
     * capture is running and no rover fix is present.
     */
    private val phoneLocationFixes: (() -> kotlinx.coroutines.flow.Flow<com.lidarscan.core.gnss.PhoneFix>)? = null,
    /** ROUND 5.2: true when fine-location permission is currently granted. */
    private val hasLocationPermission: () -> Boolean = { false },
    /**
     * ROUND 5.2: asks for fine location. Called **once**, at the moment a capture
     * starts with no rover present — never on screen entry. Returns granted/denied;
     * a denial is recorded and never asked again for this session.
     */
    private val requestLocationPermission: (suspend () -> Boolean)? = null,
    /** ROUND 5.2: records phone fixes into the live `.lscan` (see `PhoneGeorefRecorder`). */
    private val phoneGeorefRecorder: com.lidarscan.app.gnss.PhoneGeorefRecorder? = null,
    /**
     * ROUND 6 (owner item 20): one line per capture-survival event, into the
     * persistent on-device log (`com.lidarscan.app.debug.CaptureLog`). A lambda
     * rather than the class so this ViewModel stays constructible in a bare-JVM
     * test — which is what the new seal test needs.
     */
    private val logEvent: (String, String) -> Unit = { _, _ -> },
    /**
     * ROUND 6 (owner items 21 + 22): what this phone can carry. Drives the
     * preset table's per-device numbers and the conservative defaults that
     * replaced 0.2.1's "every control at its maximum".
     */
    private val deviceTier: com.lidarscan.core.capture.DeviceTier =
        com.lidarscan.core.capture.DeviceTier.STANDARD,
    /** ROUND 6: this display's real refresh ceiling in Hz, so a preset can never select a rate it cannot reach. */
    private val displayCeilingHz: Int = 60,
    /** ROUND 6 (owner item 21): the live `PageStore` sizing this session's engine was created with. */
    private val pageStoreSizing: com.lidarscan.core.render.LivePageStoreSizing =
        com.lidarscan.core.render.LivePageStoreSizing.forTier(com.lidarscan.core.capture.DeviceTier.STANDARD),
    /** ROUND 6 (owner item 22): the preset persisted for this device profile, or null for a fresh install. */
    private val loadPersistedPreset: suspend () -> com.lidarscan.core.capture.PerformancePreset? = { null },
    /** ROUND 6 (owner item 22): persists the operator's preset choice against this device profile. */
    private val persistPreset: suspend (com.lidarscan.core.capture.PerformancePreset) -> Unit = {},
) : ViewModel() {

    private val _uiState = MutableStateFlow<CaptureUiState>(CaptureUiState.Loading)
    val uiState: StateFlow<CaptureUiState> = _uiState.asStateFlow()

    val connectionState: StateFlow<ConnectionState> = engineBridge.connectionState
    val captureState: StateFlow<CaptureState> = engineBridge.captureState
    val deviceHealth: StateFlow<DeviceHealth?> = engineBridge.deviceHealth
    val isReplaySession: Boolean get() = isReplay

    private val _stats = MutableStateFlow(CaptureStats())
    val stats: StateFlow<CaptureStats> = _stats.asStateFlow()

    /** Set once a stop completes; the screen shows the session-summary sheet while non-null, clears it on dismiss. */
    private val _sessionSummary = MutableStateFlow<CaptureStats?>(null)
    val sessionSummary: StateFlow<CaptureStats?> = _sessionSummary.asStateFlow()

    private val _pointCloudSource = MutableStateFlow<PointCloudSource?>(null)
    val pointCloudSource: StateFlow<PointCloudSource?> = _pointCloudSource.asStateFlow()

    // --- B10-adjacent display controls (A14's contract, RGB/height/intensity
    // + point size + camera mode — the subset B4 needs; a full display-params
    // panel is B10's job). Kept in the ViewModel (not Compose `remember`) so
    // rotating the device (landscape<->portrait, explicitly in B4's scope)
    // doesn't reset them.
    private val _colorMode = MutableStateFlow(ColorMode.RGB)
    val colorMode: StateFlow<ColorMode> = _colorMode.asStateFlow()
    private val _colormap = MutableStateFlow(Colormap.SPECTRUM)
    val colormap: StateFlow<Colormap> = _colormap.asStateFlow()
    private val _pointSizePx = MutableStateFlow(2.5f)
    val pointSizePx: StateFlow<Float> = _pointSizePx.asStateFlow()
    private val _cameraMode = MutableStateFlow(CameraMode.ORBIT)
    val cameraMode: StateFlow<CameraMode> = _cameraMode.asStateFlow()
    private val _liveSlam = MutableStateFlow(false)
    val liveSlam: StateFlow<Boolean> = _liveSlam.asStateFlow()

    // --- ROUND 5 display controls (item 10) ---------------------------------
    //
    // "Before recording, stream live with ALL display parameters adjustable —
    // live refresh rate, point size, gamma, brightness, etc." The three below
    // join point size / colour / LOD, which already existed. All of them apply
    // to the pre-record preview and stay live during a recording.

    // --- ROUND 6 (owner items 21 + 22): presets and conservative defaults ----
    //
    // Every live-view default below used to be the MAXIMUM its control offered:
    // refresh "Max" (uncapped — 120 Hz on the owner's phone), LOD budget 20 M
    // (the top of its own 2–20 M slider), keyframes on at 3 fps, trail at its
    // full 600-point ring. Together that is "run everything flat out on a phone
    // that is also driving ARCore, a USB reader and a SLAM session, while being
    // carried" — item 21's complaint, and a fair one.
    //
    // The defaults are now `PerformancePresets.tuningFor(OPTIMAL, tier, ceiling)`
    // — genuinely mid-tier on every axis, per device class. The preset is a
    // starting point, never a cap: every individual control below stays
    // settable afterwards and moving one flips the chip row to CUSTOM.

    /** The tuning [PerformancePreset.OPTIMAL] resolves to on THIS device — the seed for every default below. */
    private val defaultTuning: com.lidarscan.core.capture.CaptureTuning =
        com.lidarscan.core.capture.PerformancePresets.tuningFor(
            com.lidarscan.core.capture.PerformancePresets.DEFAULT,
            deviceTier,
            displayCeilingHz,
        )

    private val _preset = MutableStateFlow(com.lidarscan.core.capture.PerformancePresets.DEFAULT)
    val preset: StateFlow<com.lidarscan.core.capture.PerformancePreset> = _preset.asStateFlow()

    /**
     * ROUND 6 (item 22): "switching preset shows what it changed" — one line
     * per parameter the switch actually moved, cleared on the next change or
     * when the operator moves a control themselves.
     */
    private val _presetChangeNote = MutableStateFlow<String?>(null)
    val presetChangeNote: StateFlow<String?> = _presetChangeNote.asStateFlow()

    /** ROUND 6 (item 22): the inline caution for a preset this device will struggle with, or null. */
    val presetCaution: StateFlow<String?> = _preset
        .map { com.lidarscan.core.capture.PerformancePresets.cautionFor(it, deviceTier) }
        .stateIn(viewModelScope, kotlinx.coroutines.flow.SharingStarted.Companion.WhileSubscribed(5_000), null)

    val deviceTierLabel: String get() = deviceTier.displayName

    /**
     * ROUND 6 (item 22, LIGHT): whether the viewport draws the registered /
     * pushbroom-resolved map at all, or only the sensor's own raw returns.
     * Never touches the recording — see [com.lidarscan.core.capture.CaptureTuning.liveMapEnabled].
     */
    private val _liveMapEnabled = MutableStateFlow(defaultTuning.liveMapEnabled)
    val liveMapEnabled: StateFlow<Boolean> = _liveMapEnabled.asStateFlow()

    /**
     * Viewport refresh cap in fps (0 = uncapped). A *display* throttle: the
     * engine keeps decoding and writing at full rate either way — see
     * `PointCloudRenderer.setMaxRefreshHz`.
     *
     * ROUND 6: defaults to the OPTIMAL preset's cap for this device (30 fps on
     * a standard/flagship phone), not `0` ("Max").
     */
    private val _refreshHz = MutableStateFlow(defaultTuning.refreshHz)
    val refreshHz: StateFlow<Int> = _refreshHz.asStateFlow()

    /**
     * ROUND 5 AUDIT bugfix: bumped on every [setRefreshHz] call, even when the
     * numeric value does not change.
     *
     * `PointCloudView` calls `renderer.setMaxRefreshHz(refreshHz, ...)`
     * unconditionally on every recomposition (cheap and idempotent BY
     * DESIGN — see that call site's own comment), so
     * `PointCloudRenderer.setMaxRefreshHz` has to ignore a repeat of the same
     * `hz` or it would fight `RefreshGovernor`'s own auto-downshift every
     * single frame. But `MutableStateFlow` also conflates a `.value =`
     * assignment that does not change the value, so re-selecting the SAME
     * option the operator already had chosen — the natural way to ask the
     * governor to recover after an auto-downshift, since the control still
     * shows that option as selected — used to be silently indistinguishable
     * from Compose merely recomposing with the unchanged value: neither this
     * flow nor the renderer's own cache would ever see a change, so
     * `RefreshGovernor.request()` was never called again and the downshift
     * was permanent for the rest of the session (the only way out was to pick
     * a genuinely DIFFERENT rate first, then flip back — a two-tap dance the
     * UI gave no hint was necessary). This token is the explicit "the
     * operator asked" signal the value alone cannot carry.
     */
    private val _refreshRequestToken = MutableStateFlow(0)
    val refreshRequestToken: StateFlow<Int> = _refreshRequestToken.asStateFlow()

    /** A14's scalar `gamma` (0.1 – 4.0), applied to whichever scalar block the colour mode selects. */
    private val _gamma = MutableStateFlow(1f)
    val gamma: StateFlow<Float> = _gamma.asStateFlow()

    /** A14's scalar `brightness` (0.1 – 3.0). */
    private val _brightness = MutableStateFlow(1f)
    val brightness: StateFlow<Float> = _brightness.asStateFlow()

    /**
     * ROUND 5: the **Live** toggle, on by default — "during capture, live view
     * stays available".
     *
     * Off detaches the viewport (the renderer stops being fed a source) without
     * touching the recording, which is the one thing a long walk on a hot phone
     * actually wants. Distinct from [liveSlam], which is an *engine* session
     * setting about building a registered map; that one moved into the settings
     * sheet, where the rest of the session configuration lives.
     */
    private val _liveView = MutableStateFlow(true)
    val liveView: StateFlow<Boolean> = _liveView.asStateFlow()

    /**
     * ROUND 5 (item 9): the name typed into the Capture tab's single field.
     * Blank is the normal case and means "auto-name it" — see
     * [com.lidarscan.core.capture.ScanAutoName].
     */
    private val _scanName = MutableStateFlow("")
    val scanName: StateFlow<String> = _scanName.asStateFlow()

    /**
     * The workflow profile a Start will stamp on the new project. Lives in the
     * capture-settings sheet rather than on a separate new-project screen — that
     * screen was the wizard step round 5 item 7 removed.
     */
    private val _profile = MutableStateFlow(com.lidarscan.core.model.WorkflowProfile.QUICK_SCAN)
    val profile: StateFlow<com.lidarscan.core.model.WorkflowProfile> = _profile.asStateFlow()

    // --- redesign: the Capture-settings sheet's own state ---------------------
    //
    // All three live here rather than in Compose `remember` for the same reason
    // the display controls above do: the sheet can be dismissed and reopened,
    // and the device can rotate, without any of them snapping back.

    /**
     * §3.5's camera keyframes, on by default (the mockup's `S.cap.keyframes`).
     * Gates [com.lidarscan.app.ar.KeyframeRecorder] mid-session; the written
     * count freezes rather than resetting when this goes off.
     */
    private val _keyframesEnabled = MutableStateFlow(defaultTuning.keyframesEnabled)
    val keyframesEnabled: StateFlow<Boolean> = _keyframesEnabled.asStateFlow()

    /**
     * §3.5's 2–5 fps cadence, as the sheet's 2 / 3 / 5 row.
     *
     * ROUND 6: seeded from the OPTIMAL preset (2 fps on a standard phone,
     * 3 on a flagship) rather than being pinned at 3 everywhere.
     */
    private val _keyframeRateFps = MutableStateFlow(defaultTuning.keyframeRateFps)
    val keyframeRateFps: StateFlow<Int> = _keyframeRateFps.asStateFlow()

    /**
     * §3.12's LOD budget, in millions of points.
     *
     * The mockup labels this slider `2 – 20` and reads it out as a percentage.
     * What this renderer actually implements is `DisplayParams.lodPointBudget`
     * — a page-admission ceiling in points, not a per-page decimation — so the
     * control keeps the mockup's range and its "sparse → every return" caption
     * but reads out in **M points**, which is the number the renderer obeys.
     * A percentage would have been a nicer-looking lie.
     */
    private val _lodBudgetMPoints = MutableStateFlow(defaultTuning.lodBudgetMPoints)
    val lodBudgetMPoints: StateFlow<Int> = _lodBudgetMPoints.asStateFlow()

    /**
     * Gamma + brightness as one flow, so [displayParams] below stays inside
     * `combine`'s five-flow **typed** overload. Six flows would fall through to
     * the `vararg Flow<*>` overload, where every parameter arrives as `Any?` and
     * has to be cast back — one nested combine is cheaper than five casts that
     * the compiler can no longer check.
     */
    private val toneParams: kotlinx.coroutines.flow.Flow<Pair<Float, Float>> =
        kotlinx.coroutines.flow.combine(_gamma, _brightness) { g, b -> g to b }

    /**
     * The whole A14 parameter block the live viewport renders with, assembled
     * from the controls above. One object rather than N setters so every control
     * live-applies the same way colour and point size already do —
     * `PointCloudRenderer.setDisplayParams` owns all of them together.
     */
    val displayParams: StateFlow<com.lidarscan.core.render.DisplayParams> =
        kotlinx.coroutines.flow.combine(
            _colorMode,
            _colormap,
            _pointSizePx,
            _lodBudgetMPoints,
            toneParams,
        ) { mode, cm, size, lodM, tone ->
            val (g, b) = tone
            com.lidarscan.core.render.DisplayParams(
                colorMode = mode,
                height = com.lidarscan.core.render.ScalarColorParams(
                    colormap = cm,
                    manualMin = 0f,
                    manualMax = 3f,
                    gamma = g,
                    brightness = b,
                ),
                intensity = com.lidarscan.core.render.ScalarColorParams(
                    colormap = cm,
                    gamma = g,
                    brightness = b,
                ),
                pointSize = com.lidarscan.core.render.PointSizeParams(fixedPx = size),
                lodPointBudget = (lodM.coerceIn(1, 200) * 1_000_000),
                // The redesign's viewport ground, so the live view and the
                // project thumbnails sit on the same black.
                background = com.lidarscan.core.render.Rgba(11, 14, 18, 255),
            )
        }.stateIn(
            viewModelScope,
            kotlinx.coroutines.flow.SharingStarted.Companion.WhileSubscribed(5_000),
            com.lidarscan.core.render.DisplayParams(),
        )

    /** B5: this project's profile-driven capture defaults, read once at load. */
    private val _captureDefaults = MutableStateFlow<com.lidarscan.core.model.CaptureDefaults?>(null)
    val captureDefaults: StateFlow<com.lidarscan.core.model.CaptureDefaults?> = _captureDefaults.asStateFlow()

    // --- B3: Mid-360 -------------------------------------------------------
    /** This project's sensor; drives which connect wizard opens and whether Pause is offered. */
    val sensor: StateFlow<com.lidarscan.core.model.SensorType>
        get() = _sensor.asStateFlow()
    private val _sensor = MutableStateFlow(com.lidarscan.core.model.SensorType.COIN_D6)

    /** `"<lidarIp>|<hostIp>"` from the manifest, or null when the wizard has not been run for this project. */
    private val _mid360Endpoint = MutableStateFlow<String?>(null)
    val mid360Endpoint: StateFlow<String?> = _mid360Endpoint.asStateFlow()

    /**
     * Connects the engine to this project's saved Mid-360 endpoint.
     *
     * Deliberately explicit rather than automatic on screen entry: bringing a
     * Mid-360 up runs SDK2's discovery + handshake + host-IP configuration
     * push and takes a process-wide singleton, and doing that as a side effect
     * of opening a screen is how a connect-wizard probe and a capture session
     * end up fighting over it.
     */
    fun connectMid360() {
        val endpoint = _mid360Endpoint.value ?: return
        viewModelScope.launch {
            engineBridge.connect(
                com.lidarscan.core.engine.EngineTarget(
                    com.lidarscan.core.model.SensorType.MID360,
                    transportHint = endpoint,
                ),
            )
        }
    }

    // --- B7/B8 state ---------------------------------------------------------
    private val _keyframeStats = MutableStateFlow(com.lidarscan.app.ar.KeyframeRecorder.Stats())
    val keyframeStats: StateFlow<com.lidarscan.app.ar.KeyframeRecorder.Stats> = _keyframeStats.asStateFlow()

    private val _pushbroomStats = MutableStateFlow<com.lidarscan.app.engine.NativePushbroomStats?>(null)
    val pushbroomStats: StateFlow<com.lidarscan.app.engine.NativePushbroomStats?> = _pushbroomStats.asStateFlow()

    private val _mountCalibrationApplied = MutableStateFlow<com.lidarscan.core.calib.MountCalibration?>(null)
    val mountCalibrationApplied: StateFlow<com.lidarscan.core.calib.MountCalibration?> =
        _mountCalibrationApplied.asStateFlow()

    /** True when this session can offer the §3.7 AR overlay at all. */
    val arAvailable: Boolean get() = arController != null && !isReplay

    val arStatus: StateFlow<com.lidarscan.app.ar.CaptureArController.ArStatus>? = arController?.status

    /**
     * ROUND 5 (item 11): **this capture needs the phone's pose stream to produce
     * 3D at all.**
     *
     * True for a D6 session, because the D6 has no IMU: the phone's ARCore VIO
     * *is* the trajectory and A8's pushbroom is what turns the vertical fan into
     * a cloud. A Mid-360 carries its own IMU and runs A6 LIO, so poses are
     * optional there (they still help colorization, which is why keyframes stay
     * available). False for a replay session, which has neither camera nor live
     * engine.
     *
     * The Capture screen uses this to decide two things: whether to run the
     * headless pose pump ([com.lidarscan.app.ar.ArPosePumpView]) alongside the
     * 3D-orbit view, and whether to surface tracking quality inline.
     */
    val poseTrackingRequired: Boolean
        get() = !isReplay && _sensor.value == com.lidarscan.core.model.SensorType.COIN_D6

    /**
     * ROUND 5: the auto-detect → connect → live-preview state machine (item 7).
     * Null when there is nothing to detect (a replay session).
     */
    val autoConnect: com.lidarscan.core.capture.CaptureAutoConnectController? =
        if (autoDetectors.isEmpty()) {
            null
        } else {
            com.lidarscan.core.capture.CaptureAutoConnectController(
                detectors = autoDetectors,
                connect = { detection ->
                    engineBridge.connect(
                        EngineTarget(detection.sensor, transportHint = detection.transportHint),
                    ).onSuccess {
                        // The detected sensor decides what the new project will
                        // be created as, and (for a Mid-360) which addresses get
                        // written into its manifest at Start.
                        _sensor.value = detection.sensor
                        _mid360Endpoint.value = detection.transportHint
                            ?.takeIf { detection.sensor == com.lidarscan.core.model.SensorType.MID360 }
                    }
                },
                scope = viewModelScope,
            )
        }

    val autoConnectState: StateFlow<com.lidarscan.core.capture.CaptureAutoConnectState>? = autoConnect?.state

    // --- ROUND 5 manual fallback (owner addition 1) -------------------------

    /** Attached serial devices for the manual panel's list; refreshed on entry and on demand. */
    private val _manualDevices = MutableStateFlow<List<ManualSerialDevice>>(emptyList())
    val manualDevices: StateFlow<List<ManualSerialDevice>> = _manualDevices.asStateFlow()

    private val _manualLidarIp = MutableStateFlow(com.lidarscan.core.net.Mid360Settings.DEFAULT_LIDAR_IP)
    val manualLidarIp: StateFlow<String> = _manualLidarIp.asStateFlow()

    private val _manualHostIp = MutableStateFlow(com.lidarscan.core.net.Mid360Settings.DEFAULT_HOST_IP)
    val manualHostIp: StateFlow<String> = _manualHostIp.asStateFlow()

    fun setManualLidarIp(value: String) { _manualLidarIp.value = value.trim() }
    fun setManualHostIp(value: String) { _manualHostIp.value = value.trim() }

    fun showManualEntry() {
        refreshManualDevices()
        autoConnect?.showManualEntry()
    }

    fun hideManualEntry() = autoConnect?.hideManualEntry()

    fun refreshManualDevices() {
        _manualDevices.value = runCatching { attachedSerialDevices() }.getOrDefault(emptyList())
    }

    /**
     * Manual D6: open the port the operator picked, then hand it to the engine.
     * A refused permission or a port that will not open surfaces in the same
     * status line an auto-detect failure does, rather than in a dialog.
     */
    fun connectManualD6(device: ManualSerialDevice) {
        val controller = autoConnect ?: return
        viewModelScope.launch {
            val opened = runCatching { openSerialPort(device.path) }.getOrElse { Result.failure(it) }
            if (opened.isFailure) {
                controller.connectManually(
                    com.lidarscan.core.capture.AutoDetection(
                        sensor = com.lidarscan.core.model.SensorType.COIN_D6,
                        transportHint = null, // makes the engine refuse, carrying the reason to the UI
                        label = "COIN-D6 · ${device.label}",
                    ),
                )
                return@launch
            }
            controller.connectManually(
                com.lidarscan.core.capture.AutoDetection(
                    sensor = com.lidarscan.core.model.SensorType.COIN_D6,
                    transportHint = device.path,
                    label = "COIN-D6 · ${device.label}",
                    detail = "3D scan · phone-tracked (ARCore VIO supplies the pose)",
                ),
            )
        }
    }

    /** Manual Mid-360: the two addresses, straight to the engine — no self-test gate (round 5 item 7). */
    fun connectManualMid360() {
        val controller = autoConnect ?: return
        val lidarIp = _manualLidarIp.value
        val hostIp = _manualHostIp.value
        controller.connectManually(
            com.lidarscan.core.capture.AutoDetection(
                sensor = com.lidarscan.core.model.SensorType.MID360,
                transportHint = "$lidarIp|$hostIp",
                label = "Mid-360 · $lidarIp",
                detail = "host $hostIp · typed",
            ),
        )
    }

    fun retryAutoDetect() {
        refreshManualDevices()
        autoConnect?.retry()
    }

    // --- ROUND 5.2: georeference source (RTK rover, else the phone) ----------

    /** The most recent phone fix, or null before the first one / when the fallback is not running. */
    private val _phoneFix = MutableStateFlow<com.lidarscan.core.gnss.PhoneFix?>(null)
    val phoneFix: StateFlow<com.lidarscan.core.gnss.PhoneFix?> = _phoneFix.asStateFlow()

    private val _locationPermissionDenied = MutableStateFlow(false)

    /**
     * Which georeference source this capture is on, and the chip the screen shows.
     * Ranked by [com.lidarscan.core.gnss.GeorefSourcePolicy] — a rover always wins,
     * including one that connects mid-session (the chip simply upgrades).
     */
    val georefSource: StateFlow<com.lidarscan.core.gnss.GeorefSourceState> =
        kotlinx.coroutines.flow.combine(
            rtkFix ?: MutableStateFlow(com.lidarscan.core.gnss.GnssFixSnapshot()),
            _phoneFix,
            engineBridge.captureState,
            _locationPermissionDenied,
        ) { fix, phone, capture, denied ->
            com.lidarscan.core.gnss.GeorefSourcePolicy.resolve(
                rtkFix = fix,
                phoneFix = phone,
                sessionActive = capture == CaptureState.RECORDING || capture == CaptureState.PAUSED,
                permissionDenied = denied,
            )
        }.stateIn(
            viewModelScope,
            kotlinx.coroutines.flow.SharingStarted.Companion.WhileSubscribed(5_000),
            com.lidarscan.core.gnss.GeorefSourceState(),
        )

    /** The one-line note shown when location was refused — never a dialog, never a block. */
    val georefNote: StateFlow<String?> = _locationPermissionDenied
        .map { if (it) com.lidarscan.core.gnss.GeorefSourcePolicy.PERMISSION_DENIED_NOTE else null }
        .stateIn(viewModelScope, kotlinx.coroutines.flow.SharingStarted.Companion.WhileSubscribed(5_000), null)

    private var phoneLocationJob: kotlinx.coroutines.Job? = null

    /**
     * True when the mount extrinsic in use is the **CAD nominal**, not a
     * measured calibration — see [startArPipelines]. Surfaced inline because it
     * is the difference between "3D, calibrated" and "3D, good enough to see
     * coverage": worth knowing before a survey, not worth blocking on.
     */
    private val _mountIsNominal = MutableStateFlow(false)
    val mountIsNominal: StateFlow<Boolean> = _mountIsNominal.asStateFlow()

    // --- ROUND 5.3: walkthrough-first (item 18) ------------------------------

    /**
     * The walked path, drawn over the live view during preview **and** capture.
     * Fed from the ARCore frame stream, which the round-5 pose pump keeps running
     * in both view modes.
     */
    private val trailRecorder = com.lidarscan.app.capture.TrajectoryTrailRecorder(
        // ROUND 6 (item 21): the ring used to be created at its full 600-point
        // default on every device. Sized by preset/tier now, like everything
        // else the live view spends.
        com.lidarscan.core.capture.TrajectoryTrail(capacity = defaultTuning.trailPoints),
    )
    val trailPoints: StateFlow<List<com.lidarscan.core.capture.TrajectoryTrail.NormalizedPoint>> =
        trailRecorder.points
    val trailLengthM: StateFlow<Float> = trailRecorder.pathLengthM

    /** Held so the exact same reference can be removed again (see [keyframeFrameListener]). */
    private var trailFrameListener: ((com.google.ar.core.Frame) -> Unit)? = null

    /**
     * ROUND 5.3 (item 18): the gentle inline "you are moving too fast" hint.
     *
     * Two sources, both already measured and neither of them new telemetry: ARCore's
     * own `EXCESSIVE_MOTION` tracking-failure reason, and B8's motion gate rejecting
     * keyframes (`skippedMotion` climbing). Round 3 put the *numbers* in the
     * Diagnostics sheet and that stands — this is one sentence, inline, and it clears
     * itself as soon as the motion settles.
     */
    private val _motionHint = MutableStateFlow<String?>(null)
    val motionHint: StateFlow<String?> = _motionHint.asStateFlow()

    private var lastSkippedMotion = 0L
    private var lastSkipGrowthMillis = 0L

    /**
     * ROUND 5.3 (item 17): non-null while the live view has been auto-eased below
     * what was asked for. Set by the renderer's governor through the screen.
     */
    private val _refreshDownshiftNote = MutableStateFlow<String?>(null)
    val refreshDownshiftNote: StateFlow<String?> = _refreshDownshiftNote.asStateFlow()

    /** Called by the viewport when the governor eases the live view down a notch. */
    fun onRefreshAutoDownshift(easedToHz: Int) {
        _refreshDownshiftNote.value =
            "Live view eased to $easedToHz fps — this phone could not sustain the requested rate on this " +
                "cloud. Recording is unaffected."
    }

    // --- ROUND 6 (owner item 20): saving is loud when it fails ---------------

    /**
     * Non-null when this capture's **seal** did not complete cleanly — the
     * manifest could not be written, or it was written and could not be read
     * back. 0.2.1 swallowed both (`updateManifest` returned null and nothing
     * looked at it), which is how a whole field session could end with a
     * confident session-summary sheet and nothing in Projects.
     *
     * Shown as a loud inline banner on the capture screen and repeated in the
     * session summary; cleared only by [dismissSaveError] or the next Start.
     */
    /**
     * ROUND 6 (owner item 20): true from a successful Start until that
     * session's seal has actually run.
     *
     * The Capture tab is not the only thing that can end a session. The replay
     * bridge stops itself when the synthetic file runs out; a future engine
     * fault could do the same. Before this, a session that ended WITHOUT the
     * Stop button being pressed was never sealed at all — the manifest kept
     * whatever it had at creation, and the operator was told nothing. Watching
     * the capture state and sealing on any transition out of a live session is
     * the belt to [stopCapture]'s braces, and it is the same rule the whole
     * item is about: a capture that happened must end up saved.
     */
    private val sealPending = java.util.concurrent.atomic.AtomicBoolean(false)
    private val sealMutex = kotlinx.coroutines.sync.Mutex()

    private val _saveError = MutableStateFlow<String?>(null)
    val saveError: StateFlow<String?> = _saveError.asStateFlow()

    fun dismissSaveError() {
        _saveError.value = null
    }

    /**
     * ROUND 6: where the last completed capture actually landed, for the
     * summary sheet. A path on screen is the cheapest possible answer to "is it
     * saved?", and it is the thing the owner could not get last time.
     */
    private val _lastSavedProject = MutableStateFlow<String?>(null)
    val lastSavedProject: StateFlow<String?> = _lastSavedProject.asStateFlow()

    // --- ROUND 6 (owner item 21): the D6 live map ----------------------------

    /**
     * True once `pushbroom_enable(true)` has been accepted for this session,
     * i.e. once the engine is resolving D6 fan returns into world-frame points
     * on `SCAN_STREAM_SLAM_MAP`.
     *
     * **This is what the viewport's stream filter must key off**, and it not
     * being so was the "the point are not really aligned" bug: the filter was
     * `StreamFilter.forSession(liveSlam)`, and on the Capture tab `liveSlam`
     * is `false` until somebody opens the settings sheet and toggles it (the
     * manifest's own default is only read on the project-scoped route). So a
     * D6 session ran `RAW_ONLY`, which by construction rejects
     * `SCAN_STREAM_SLAM_MAP` — the viewport drew the raw sensor-frame fan, in
     * the sensor's own frame, and never once drew the pushbroom-resolved cloud
     * the whole D6 pipeline exists to produce. `liveSlam` is a Mid-360 concept
     * (`CaptureDefaults`' own words); the D6's live map is gated on the
     * pushbroom, so that is what gates its filter now.
     */
    private val _pushbroomActive = MutableStateFlow(false)
    val pushbroomActive: StateFlow<Boolean> = _pushbroomActive.asStateFlow()

    /**
     * Whether the viewport should draw the registered/pushbroom map rather than
     * raw sensor pages. `liveMapEnabled` is the Light preset's off-switch;
     * beyond that it is live SLAM (Mid-360) or the pushbroom (D6).
     */
    val liveMapRequested: StateFlow<Boolean> =
        kotlinx.coroutines.flow.combine(_liveMapEnabled, _liveSlam, _pushbroomActive) { enabled, slam, pushbroom ->
            enabled && (slam || pushbroom)
        }.stateIn(
            viewModelScope,
            kotlinx.coroutines.flow.SharingStarted.Companion.WhileSubscribed(5_000),
            false,
        )

    /**
     * ROUND 6 (owner item 21): non-null once the engine's live `PageStore` has
     * filled and the map has stopped growing.
     *
     * `PageStore::append()` returns `kCapacityExceeded` and stores **nothing**
     * once `max_pages` pages exist — its own header says so, and points to a
     * future LOD/eviction policy that does not exist yet. Until it does, the
     * only honest thing is to see it and say it, which is what this is:
     * `pageCount()` reaching the ceiling the app itself chose
     * ([com.lidarscan.core.render.LivePageStoreSizing]) is a reliable,
     * engine-change-free detection.
     *
     * The line is emphatic that this costs the preview and not the scan —
     * record-always has already written the raw streams to the `.lscan`, and
     * post-processing reads those, not this buffer.
     */
    private val _liveMapFullNote = MutableStateFlow<String?>(null)
    val liveMapFullNote: StateFlow<String?> = _liveMapFullNote.asStateFlow()

    // --- ROUND 6 (owner item 23): the one-tap mount re-zero -------------------

    /**
     * The session's mount trim: how the D6 was actually sitting on the phone
     * for THIS scan, composed on top of `BracketNominals.cadNominal(COIN_D6)`.
     * Null until the operator taps "Set mount reference".
     */
    private val _mountTrim = MutableStateFlow<com.lidarscan.core.calib.MountTrim?>(null)
    val mountTrim: StateFlow<com.lidarscan.core.calib.MountTrim?> = _mountTrim.asStateFlow()

    /** The last re-zero's outcome as one sentence — the confirmation or the refusal. */
    private val _mountTrimNote = MutableStateFlow<String?>(null)
    val mountTrimNote: StateFlow<String?> = _mountTrimNote.asStateFlow()

    /**
     * ROUND 6 (item 23): captures the current phone attitude as this session's
     * mount reference, or refuses and says why.
     *
     * Averages ~1 s of the pose stream that is already running (see
     * [com.lidarscan.core.calib.MountTrimSampler]) — no extra ARCore work, no
     * new sensor subscription — and rejects a window in which the rig moved or
     * tracking dropped, because a trim taken mid-wobble is worse than none.
     *
     * Applies immediately to a live pushbroom session as well as to the next
     * one: `set_mount_extrinsics` is legal mid-session (`scan_engine_pushbroom_enable`'s
     * own note), so a re-zero during a walk takes effect from the next resolved
     * point rather than at the next Start.
     */
    fun setMountReference() {
        val controller = arController
        if (controller == null) {
            _mountTrimNote.value = "Mount reference needs phone tracking, which this session does not have."
            return
        }
        val result = com.lidarscan.core.calib.MountTrimSampler.capture(
            samples = controller.poseWindow(),
            nowMillis = clock(),
            sensor = _sensor.value,
        )
        when (result) {
            is com.lidarscan.core.calib.MountTrimResult.Rejected -> {
                _mountTrimNote.value = result.reason.message
                logEvent(LOG_TAG_AR, "mount re-zero refused: ${result.reason.name}")
            }
            is com.lidarscan.core.calib.MountTrimResult.Captured -> {
                _mountTrim.value = result.trim
                _mountTrimNote.value =
                    "Mount reference set — %.1f° from the bracket nominal, held to %.2f° over %d samples."
                        .format(result.trim.magnitudeDeg, result.trim.spreadDeg, result.trim.sampleCount)
                logEvent(
                    LOG_TAG_AR,
                    "mount re-zero captured: magnitude=%.2fdeg spread=%.2fdeg samples=%d"
                        .format(result.trim.magnitudeDeg, result.trim.spreadDeg, result.trim.sampleCount),
                )
                applyMountExtrinsicNow()
                // A trim taken mid-session belongs to the project being recorded.
                (_uiState.value as? CaptureUiState.Loaded)?.project?.let { project ->
                    viewModelScope.launch(Dispatchers.IO) {
                        projectStore.updateManifest(project.id) { it.copy(mountTrim = result.trim) }
                    }
                }
            }
        }
    }

    /** Drops the session trim, going back to the bare CAD nominal. */
    fun clearMountReference() {
        _mountTrim.value = null
        _mountTrimNote.value = "Mount reference cleared — back to the bracket's CAD nominal."
        applyMountExtrinsicNow()
    }

    fun dismissMountTrimNote() {
        _mountTrimNote.value = null
    }

    // --- ROUND 6 (owner item 22): the Light / Optimal / Full chips ------------

    /**
     * Applies a preset, prefilling every parameter it owns and reporting what
     * moved.
     *
     * Item 22's contract in one method: the preset is a **starting point**. It
     * writes the same `_`-flows the sheet's own controls write, so every value
     * stays individually editable afterwards; moving any of them flips
     * [preset] to [com.lidarscan.core.capture.PerformancePreset.CUSTOM] without
     * reverting anything.
     */
    fun setPreset(next: com.lidarscan.core.capture.PerformancePreset) {
        if (!next.isSelectable) return
        val before = currentTuning()
        val after = com.lidarscan.core.capture.PerformancePresets.tuningFor(next, deviceTier, displayCeilingHz)
        applyTuning(after)
        _preset.value = next
        val changes = com.lidarscan.core.capture.PerformancePresets.changes(before, after)
        _presetChangeNote.value = if (changes.isEmpty()) {
            "${next.displayName}: nothing changed — you were already on these settings."
        } else {
            "${next.displayName}: ${changes.joinToString(" · ")}"
        }
        logEvent(LOG_TAG_SESSION, "preset=$next tier=$deviceTier changes=${changes.joinToString("; ")}")
        viewModelScope.launch { runCatching { persistPreset(next) } }
    }

    fun dismissPresetChangeNote() {
        _presetChangeNote.value = null
    }

    /**
     * ROUND 6: the trail's ring size, tracked as state rather than read back
     * off the recorder — [currentTuning] compares against
     * [com.lidarscan.core.capture.PerformancePresets.tuningFor]'s whole value,
     * so a field that never moves would make `match()` permanently report
     * CUSTOM and every preset switch claim a spurious "trail length" change.
     */
    private val _trailPoints = MutableStateFlow(defaultTuning.trailPoints)

    private fun currentTuning() = com.lidarscan.core.capture.CaptureTuning(
        liveMapEnabled = _liveMapEnabled.value,
        refreshHz = _refreshHz.value,
        lodBudgetMPoints = _lodBudgetMPoints.value,
        keyframesEnabled = _keyframesEnabled.value,
        keyframeRateFps = _keyframeRateFps.value,
        trailEnabled = true,
        trailPoints = _trailPoints.value,
    )

    private fun applyTuning(tuning: com.lidarscan.core.capture.CaptureTuning) {
        _liveMapEnabled.value = tuning.liveMapEnabled
        _refreshHz.value = tuning.refreshHz
        _refreshRequestToken.value++
        _lodBudgetMPoints.value = tuning.lodBudgetMPoints
        _keyframesEnabled.value = tuning.keyframesEnabled
        keyframeRecorder?.setEnabled(tuning.keyframesEnabled)
        _keyframeRateFps.value = tuning.keyframeRateFps
        keyframeRecorder?.setTargetFps(tuning.keyframeRateFps.toDouble())
        _trailPoints.value = tuning.trailPoints
        trailRecorder.setCapacity(tuning.trailPoints)
    }

    /**
     * Called by every individual control's setter. Item 22 again: an advanced
     * user moving one slider must keep that value and simply stop being "on" a
     * preset — never have their edit snapped back.
     */
    private fun markCustomIfDiverged() {
        _presetChangeNote.value = null
        val matched = com.lidarscan.core.capture.PerformancePresets.match(
            currentTuning(),
            deviceTier,
            displayCeilingHz,
        )
        _preset.value = matched
    }

    private var keyframeRecorder: com.lidarscan.app.ar.KeyframeRecorder? = null

    /**
     * True once `pushbroom_enable(true)` has actually been accepted for this
     * session. Gates the flush at stop — which used to be gated on
     * [mountCalibrationApplied] being non-null, and would therefore have skipped
     * the flush for every ROUND 5 nominal-extrinsic D6 session, i.e. exactly the
     * sessions whose points only exist because of the pushbroom.
     */
    private var pushbroomEnabled = false

    /** Held so the exact same function reference can be removed again — a method reference is a NEW object each time it is written. */
    private var keyframeFrameListener: ((com.google.ar.core.Frame) -> Unit)? = null

    private fun detachKeyframeListener() {
        val listener = keyframeFrameListener ?: return
        arController?.removeFrameListener(listener)
        keyframeFrameListener = null
    }

    /**
     * ROUND 5.3: subscribes the trail to the pose stream for the whole life of this
     * screen — preview included, because a walkthrough operator frames the first
     * few metres before pressing record.
     */
    private fun attachTrailListener() {
        val controller = arController ?: return
        if (trailFrameListener != null) return
        val listener: (com.google.ar.core.Frame) -> Unit = trailRecorder::onFrame
        trailFrameListener = listener
        controller.addFrameListener(listener)
    }

    private fun detachTrailListener() {
        val listener = trailFrameListener ?: return
        arController?.removeFrameListener(listener)
        trailFrameListener = null
    }

    /**
     * ROUND 5.3 (item 18): keeps [motionHint] current from the two signals that
     * already exist. `skippedMotion` is a monotonic counter, so what matters is
     * whether it GREW recently — a session that skipped 40 frames an hour ago is
     * not moving too fast now.
     */
    private fun updateMotionHint(nowMillis: Long) {
        val skipped = _keyframeStats.value.skippedMotion
        if (skipped > lastSkippedMotion) {
            lastSkippedMotion = skipped
            lastSkipGrowthMillis = nowMillis
        }
        val recentlySkipping = lastSkipGrowthMillis != 0L &&
            nowMillis - lastSkipGrowthMillis < MOTION_HINT_LINGER_MS
        val status = arController?.status?.value
        val excessiveMotion = status != null && !status.tracking &&
            status.failureReason == com.google.ar.core.TrackingFailureReason.EXCESSIVE_MOTION

        _motionHint.value = when {
            excessiveMotion -> "Moving too fast — slow the walk so tracking can keep up."
            recentlySkipping && _keyframesEnabled.value ->
                "Turning too fast for colour frames — sweep more slowly to keep coverage."
            else -> null
        }
    }

    private var lastStatsSampleMillis = 0L
    private var lastStatsSamplePoints = 0L

    init {
        viewModelScope.launch(Dispatchers.IO) {
            // ROUND 5: no project id is the Capture tab's normal state — there is
            // nothing to open, and Start is what creates one (item 9). The name
            // field's placeholder is the name the project *would* get right now,
            // so the operator can see the series number before spending it.
            if (projectId == null) {
                _uiState.value = CaptureUiState.NewScan(
                    autoName = com.lidarscan.core.capture.ScanAutoName.format(
                        series = runCatching { peekSeriesNumber() }.getOrDefault(1),
                        epochMillis = clock(),
                    ),
                )
                return@launch
            }
            val project = projectStore.open(projectId)
            _uiState.value = if (project != null) CaptureUiState.Loaded(project) else CaptureUiState.NotFound
            if (project != null) {
                _sensor.value = project.manifest.sensor
                _profile.value = project.manifest.profile
                // B5: the profile stops being a label here. Live-SLAM's initial
                // position and the engine's session `profile` string both come
                // from the project's own CaptureDefaults rather than from a
                // hardcoded default (see EngineBridge.startCapture's KDoc).
                val defaults = project.manifest.effectiveCaptureDefaults()
                _liveSlam.value = defaults.liveSlam
                _captureDefaults.value = defaults
                _mid360Endpoint.value = project.manifest.mid360
                    ?.let { "${'$'}{it.lidarIp}|${'$'}{it.hostIp}" }
            }
        }

        // ROUND 5.3 (item 18): the trail follows the pose stream for as long as this
        // screen exists, so it is already drawing while the operator lines up the
        // first few metres.
        attachTrailListener()

        // The motion hint is polled rather than pushed: both of its inputs are
        // sampled state (an ARCore status flow and a monotonic skip counter), and a
        // 500 ms tick is what makes "recently" mean anything. Cheap, and it stops
        // with the ViewModel.
        viewModelScope.launch {
            while (true) {
                updateMotionHint(clock())
                kotlinx.coroutines.delay(MOTION_HINT_TICK_MS)
            }
        }

        // ROUND 6 (item 22): the preset persisted for this device profile. Applied
        // rather than merely recorded, so a phone the operator has already set
        // to Light opens on Light — including its defaults, not OPTIMAL's.
        viewModelScope.launch {
            val persisted = runCatching { loadPersistedPreset() }.getOrNull() ?: return@launch
            if (!persisted.isSelectable || persisted == _preset.value) return@launch
            applyTuning(
                com.lidarscan.core.capture.PerformancePresets.tuningFor(persisted, deviceTier, displayCeilingHz),
            )
            _preset.value = persisted
        }

        // ROUND 6 (item 21): watch for the engine's live PageStore filling. Once
        // it does, `PageStore::append()` stores nothing more (its own header:
        // "When full it appends nothing") and the live map silently stops
        // growing — which is a large part of "its bearly maping". Polled at
        // 1 Hz: it is one JNI call returning an int, and the transition is a
        // once-per-session event, not a per-frame one.
        viewModelScope.launch {
            while (true) {
                kotlinx.coroutines.delay(LIVE_MAP_WATCH_MS)
                val source = _pointCloudSource.value ?: continue
                if (!source.isAvailable) continue
                val pages = runCatching { source.pageCount() }.getOrDefault(0)
                if (pages >= pageStoreSizing.maxPages && _liveMapFullNote.value == null) {
                    _liveMapFullNote.value =
                        com.lidarscan.core.render.LivePageStoreSizing.fullNote(pageStoreSizing)
                    logEvent(
                        LOG_TAG_STORE,
                        "live page store FULL at $pages/${pageStoreSizing.maxPages} pages " +
                            "(${pageStoreSizing.residentPointCeiling} point ceiling) — live map stops growing; " +
                            "recording unaffected",
                    )
                }
            }
        }

        // The manual panel's Mid-360 fields open on the last auto-detected pair.
        viewModelScope.launch {
            val defaults = runCatching { manualMid360Defaults() }.getOrNull() ?: return@launch
            _manualLidarIp.value = defaults.first
            _manualHostIp.value = defaults.second
        }

        engineBridge.events
            .filterIsInstance<EngineEvent.CaptureStats>()
            .onEach(::onCaptureStats)
            .launchIn(viewModelScope)

        // ROUND 6 (item 20): the safety net. Any transition into a non-live
        // capture state with a seal still pending means the session ended
        // without Stop being pressed — seal it anyway, uncancellably, and log
        // that this is what happened.
        engineBridge.captureState
            .onEach { state ->
                if (state == CaptureState.RECORDING || state == CaptureState.PAUSED) return@onEach
                if (state == CaptureState.STOPPING) return@onEach
                if (!sealPending.get()) return@onEach
                logEvent(LOG_TAG_SEAL, "session ended without Stop (state=$state) — sealing anyway")
                kotlinx.coroutines.withContext(kotlinx.coroutines.NonCancellable) {
                    runCatching { sealAndStop() }
                        .onFailure { logEvent(LOG_TAG_SEAL, "auto-seal THREW: ${it.javaClass.name}: ${it.message}") }
                }
            }
            .launchIn(viewModelScope)

        engineBridge.connectionState
            .onEach { state ->
                _pointCloudSource.value = (engineBridge as? NativePointCloudProvider)?.currentPointCloudSource()
                // ROUND 5: a transport that drops mid-preview has to say so —
                // the auto-connect strip is the only place the operator can see
                // it, and it owns the Retry button.
                if (state == ConnectionState.DISCONNECTED || state == ConnectionState.ERROR) {
                    autoConnect?.onConnectionLost()
                }
            }
            .launchIn(viewModelScope)

        // ROUND 5 (item 7): auto-detect runs on entry, unprompted — detected →
        // connected → live preview with no taps. Skipped when the engine is
        // already connected (navigating back onto a live session).
        if (autoConnect != null && engineBridge.connectionState.value != ConnectionState.CONNECTED) {
            // The manual panel's device list is filled up front, so when
            // auto-detect comes back empty the fallback panel it opens is already
            // populated instead of showing an empty list for a frame.
            refreshManualDevices()
            autoConnect.start()
        }

        if (isReplay) {
            // No USB wizard for a replay session — the bridge itself has
            // nothing to "find" (see ReplayEngineBridge.connect, which just
            // creates the native replay engine handle). SensorType is
            // otherwise unused by ReplayEngineBridge.connect.
            viewModelScope.launch { engineBridge.connect(EngineTarget(SensorType.COIN_D6)) }
        }
    }

    private fun onCaptureStats(event: EngineEvent.CaptureStats) {
        val dtMillis = (event.elapsedMillis - lastStatsSampleMillis).coerceAtLeast(1L)
        val dPoints = (event.pointsCaptured - lastStatsSamplePoints).coerceAtLeast(0L)
        val instantaneousRate = dPoints * 1000.0 / dtMillis
        lastStatsSampleMillis = event.elapsedMillis
        lastStatsSamplePoints = event.pointsCaptured

        _stats.value = _stats.value.copy(
            pointsCaptured = event.pointsCaptured,
            elapsedMillis = event.elapsedMillis,
            pointsPerSecond = instantaneousRate,
        )

        // Recording size: sum bytes under streams/ — a plain directory walk,
        // throttled to once per stats tick, not per-point. For a replay
        // session this stays 0 (nothing new is written to disk — see
        // ReplayEngineBridge's doc comment), which is correct, not a bug.
        val project = (_uiState.value as? CaptureUiState.Loaded)?.project ?: return
        if (isReplay) return
        viewModelScope.launch(Dispatchers.IO) {
            val size = directorySizeBytes(project.streamsDir)
            _stats.value = _stats.value.copy(recordingSizeBytes = size)
        }
    }

    /**
     * ROUND 5 (items 8 + 9): **Start creates the project.**
     *
     * On the Capture tab there is nothing loaded, so this claims a series
     * number, creates the `.lscan` in the *same* store the Projects tab lists
     * (`container.projectStore` — there is only one), stamps the detected sensor
     * and (for a Mid-360) the detected addresses into its manifest, and only then
     * tells the engine to record into it. A replay/deep-link entry already has a
     * project and skips straight to the record call, unchanged.
     *
     * The project is created **at Start, not at screen entry**: opening the
     * Capture tab and walking away must not leave an empty project behind, and a
     * series number must only ever be spent on a scan that was actually taken.
     */
    fun startCapture() {
        lastStatsSampleMillis = 0L
        lastStatsSamplePoints = 0L
        _stats.value = CaptureStats()
        _sessionSummary.value = null
        // ROUND 5.3: one walk, one trail — the preview's framing path is not part
        // of the recorded walkthrough.
        trailRecorder.clear()
        // ROUND 6 (item 20): a new attempt clears the previous one's verdict.
        _saveError.value = null
        _lastSavedProject.value = null
        _liveMapFullNote.value = null
        viewModelScope.launch {
            val project = (_uiState.value as? CaptureUiState.Loaded)?.project
                ?: createProjectForThisScan()
                ?: return@launch
            logEvent(
                LOG_TAG_SESSION,
                "start: project=${project.id} sensor=${project.manifest.sensor} " +
                    "profile=${project.manifest.profile} preset=${_preset.value} tier=$deviceTier " +
                    "liveSlam=${_liveSlam.value} dir=${project.directory.absolutePath}",
            )
            val started = engineBridge.startCapture(
                project.directory.absolutePath,
                _liveSlam.value,
                com.lidarscan.core.model.CaptureDefaults.engineProfileString(project.manifest.profile),
            )
            if (started.isFailure) {
                // ROUND 6: this used to be a bare `return@launch`. The project
                // directory exists at this point but nothing will ever be
                // written into it, and the operator was shown nothing at all.
                val why = started.exceptionOrNull()
                _saveError.value =
                    "The scan did not start (${why?.message ?: "the engine refused"}). Nothing is being " +
                        "recorded — check the sensor connection and press Start again."
                logEvent(LOG_TAG_SEAL, "engine startCapture FAILED for ${project.id}: ${why?.message}")
                return@launch
            }
            sealPending.set(true)
            startArPipelines(project)
            startPhoneGeorefIfNeeded(project)
        }
    }

    /**
     * ROUND 5.2: arms the phone-location georeference **only** when this capture
     * has no RTK rover fix behind it.
     *
     * This is where the permission is asked for — at Start, once, and only in the
     * no-rover case, which is why opening the Capture tab with a rover attached
     * never shows a location prompt at all. A denial records the flag (so the chip
     * and the quiet note can say what happened) and returns; the capture that is
     * already running is not touched.
     */
    private suspend fun startPhoneGeorefIfNeeded(project: Project) {
        val fixes = phoneLocationFixes ?: return
        val recorder = phoneGeorefRecorder ?: return
        if (isReplay) return
        if (!com.lidarscan.core.gnss.GeorefSourcePolicy.shouldRunPhoneFallback(
                rtkFix = rtkFix?.value,
                sessionActive = true,
                permissionDenied = _locationPermissionDenied.value,
            )
        ) {
            return
        }

        if (!hasLocationPermission()) {
            val granted = requestLocationPermission?.invoke() ?: false
            if (!granted) {
                _locationPermissionDenied.value = true
                return
            }
        }

        recorder.start(engineHandleProvider(), project.directory)
        phoneLocationJob?.cancel()
        phoneLocationJob = viewModelScope.launch {
            fixes().collect { fix ->
                _phoneFix.value = fix
                // A rover that comes up mid-session takes over: its epochs are
                // centimetres and A10 weights by sigma, so continuing to push
                // metre-accurate epochs alongside would only add noise. The chip
                // upgrade and this stop are the same decision.
                if (!com.lidarscan.core.gnss.GeorefSourcePolicy.shouldRunPhoneFallback(
                        rtkFix = rtkFix?.value,
                        sessionActive = true,
                        permissionDenied = false,
                    )
                ) {
                    stopPhoneGeoref()
                    return@collect
                }
                recorder.record(fix)
            }
        }
    }

    private fun stopPhoneGeoref() {
        phoneLocationJob?.cancel()
        phoneLocationJob = null
        phoneGeorefRecorder?.stop()
    }

    /**
     * Creates (and loads) the project this Start is about to record into.
     * Returns null only if the store itself refused, in which case nothing is
     * started and the screen stays where it was.
     */
    private suspend fun createProjectForThisScan(): Project? = withContext(Dispatchers.IO) {
        val series = runCatching { claimSeriesNumber() }.getOrDefault(1)
        val name = com.lidarscan.core.capture.ScanAutoName.resolve(
            typedName = _scanName.value,
            series = series,
            epochMillis = clock(),
        )
        val created = runCatching {
            projectStore.create(name, _sensor.value, _profile.value)
        }.getOrElse { e ->
            // ROUND 6 (owner item 20): this used to be `.getOrNull() ?: return
            // null`, and `startCapture` then returned without a word. A Start
            // that silently does nothing is exactly the class of failure the
            // owner had no way to report — say it, log it, and let the record
            // button stay honest.
            _saveError.value =
                "Could not create the scan folder on this phone (${e.javaClass.simpleName}: ${e.message}). " +
                    "Nothing was recorded. Check free space and storage permissions."
            logEvent(LOG_TAG_SEAL, "project create FAILED for \"$name\": ${e.javaClass.simpleName}: ${e.message}")
            return@withContext null
        }

        // The detected Mid-360 addresses are what this capture is actually being
        // taken with, and §3.1's "save per project" is what makes a .lscan
        // re-openable on the site it came from. Written here rather than by a
        // wizard, because in round 5 there is no wizard on this path.
        val endpoint = _mid360Endpoint.value
        val withEndpoint = if (endpoint != null && _sensor.value == com.lidarscan.core.model.SensorType.MID360) {
            val (lidarIp, hostIp) = endpoint.split('|').let { it.getOrNull(0) to it.getOrNull(1) }
            if (lidarIp != null && hostIp != null) {
                projectStore.updateManifest(created.id) { manifest ->
                    manifest.copy(mid360 = com.lidarscan.core.net.Mid360Settings(lidarIp = lidarIp, hostIp = hostIp))
                }
            } else {
                null
            }
        } else {
            null
        }

        // ROUND 5 (owner addition 3): the display settings tuned against the live
        // PREVIEW are the ones this recording runs with — the sheet's state is
        // ViewModel-scoped, so nothing is re-read or reset at Start — and they are
        // persisted as the project's own default view (§3.9's "settings persist
        // per project"). A capture framed one way in preview therefore opens the
        // same way in Review, on this phone or on a desktop.
        val withDisplay = projectStore.updateManifest((withEndpoint ?: created).id) { manifest ->
            manifest.copy(displayParams = displayParams.value)
        }

        val project = withDisplay ?: withEndpoint ?: created
        val defaults = project.manifest.effectiveCaptureDefaults()
        _captureDefaults.value = defaults
        _uiState.value = CaptureUiState.Loaded(project)
        project
    }

    /**
     * B7/B8, in the one order that works:
     *
     *  1. point the ARCore controller at the now-live engine handle, so poses
     *     land in this session rather than nowhere;
     *  2. apply the mount extrinsic and enable the pushbroom — the engine
     *     refuses `pushbroom_enable` with `SCAN_ERR_INVALID_STATE` until an
     *     extrinsic exists, and for a D6 the pushbroom is not an extra: it is
     *     what makes the capture 3D at all (round 5 item 11);
     *  3. start the keyframe recorder, which needs the project directory that
     *     `startCapture` just opened as an `.lscan`.
     *
     * **ROUND 5 change — the D6 no longer silently records 2D.** Before this, a
     * missing mount calibration meant `pushbroom_enable` was never called, so a
     * D6 session wrote fan slices and poses that nothing ever combined. The
     * calibration wizard is still the right answer for survey accuracy, but "no
     * calibration yet" must not mean "no 3D": when a D6 session has no measured
     * calibration, the **CAD nominal** for the bracket
     * ([com.lidarscan.core.calib.BracketNominals.cadNominal] — scanner above the
     * camera, scan plane vertical, which is exactly the owner's phone-back mount)
     * is applied instead and [mountIsNominal] says so on screen. A nominal
     * extrinsic costs a few mm/deg of registration; not enabling the pushbroom
     * costs the entire third dimension.
     */
    private fun startArPipelines(project: Project) {
        val controller = arController ?: return
        val handle = engineHandleProvider()
        controller.engineHandle = handle

        applyMountExtrinsic(handle, project.manifest.sensor, project.manifest.mountCalibration)

        keyframeRecorder = com.lidarscan.app.ar.KeyframeRecorder(
            motion = controller.motion,
            // The sheet's cadence, applied at construction so the very first
            // slot of the session already runs at the selected rate; changing
            // it later goes through setKeyframeRateFps on the live recorder.
            selector = com.lidarscan.core.capture.KeyframeSelector(
                targetFps = _keyframeRateFps.value.toDouble(),
            ),
        ).also { recorder ->
            recorder.setEnabled(_keyframesEnabled.value)
            // One reference, stored and registered — `recorder::onFrame`
            // written twice would create two distinct objects, and
            // detachKeyframeListener would then remove neither (its own KDoc
            // says exactly this; the code did it anyway).
            val listener: (com.google.ar.core.Frame) -> Unit = recorder::onFrame
            keyframeFrameListener = listener
            controller.addFrameListener(listener)
            recorder.start(project.directory)
            viewModelScope.launch {
                recorder.stats.collect { _keyframeStats.value = it }
            }
        }
    }

    /**
     * ROUND 6 (owner item 23): resolves the extrinsic this session's pushbroom
     * runs on and pushes it into the engine, then enables the pushbroom.
     *
     * The ladder, most-trusted first:
     *  1. a **measured** mount calibration (the wizard's), if it is rigid;
     *  2. otherwise the bracket's **CAD nominal**, with the session's
     *     [mountTrim] composed on top when the operator has re-zeroed;
     *  3. the bare nominal.
     *
     * The trim is composed onto the nominal and NOT onto a measured
     * calibration: a measured extrinsic already contains the real geometry of
     * the bracket as it was when it was solved, and multiplying a fresh
     * attitude re-zero into it would double-count the part the solve already
     * knows. A re-zero is what you have *instead of* a calibration, not on top
     * of one — and [mountIsNominal] is exactly the flag that says which.
     *
     * Split out of [startArPipelines] so a mid-session re-zero can call it too:
     * `scan_engine_set_mount_extrinsics` is legal while a session runs, so a
     * trim taken during a walk applies from the next resolved point.
     */
    private fun applyMountExtrinsic(
        handle: Long,
        sensor: com.lidarscan.core.model.SensorType,
        projectCalibration: com.lidarscan.core.calib.MountCalibration?,
    ) {
        val measured = projectCalibration ?: mountCalibrationFor(sensor)
        val measuredMatrix = measured
            ?.let { com.lidarscan.core.calib.Mat4(it.cameraFromLidar.copyOf()) }
            ?.takeIf { it.isRigid(1e-4) }
        val nominalMatrix = com.lidarscan.core.calib.BracketNominals.cadNominal(sensor)
        val trim = _mountTrim.value?.takeIf { it.sensor == sensor }
        val matrix = measuredMatrix
            ?: trim?.composedWith(nominalMatrix)
            ?: nominalMatrix
        val usingNominal = measuredMatrix == null

        if (handle == 0L) {
            _mountIsNominal.value = usingNominal
            return
        }
        val err = com.lidarscan.app.engine.ScanEngineNative.nativeSetMountExtrinsics(handle, matrix.m)
        if (err == com.lidarscan.app.engine.ScanEngineNative.ErrorCode.OK) {
            val enableErr = com.lidarscan.app.engine.ScanEngineNative.nativePushbroomEnable(handle, true)
            val enabled = enableErr == com.lidarscan.app.engine.ScanEngineNative.ErrorCode.OK
            _mountCalibrationApplied.value = measured
            _mountIsNominal.value = usingNominal
            pushbroomEnabled = pushbroomEnabled || enabled
            _pushbroomActive.value = pushbroomEnabled
            logEvent(
                LOG_TAG_PUSHBROOM,
                "extrinsic applied: source=${if (usingNominal) "nominal" else "measured"} " +
                    "trim=${trim?.let { "%.2fdeg".format(it.magnitudeDeg) } ?: "none"} " +
                    "pushbroomEnabled=$enabled",
            )
        } else {
            logEvent(LOG_TAG_PUSHBROOM, "set_mount_extrinsics FAILED err=$err — no live 3D map this session")
        }
    }

    /** Re-applies the extrinsic to whatever session is live right now (used by the re-zero). */
    private fun applyMountExtrinsicNow() {
        val handle = engineHandleProvider()
        if (handle == 0L) return
        val project = (_uiState.value as? CaptureUiState.Loaded)?.project
        applyMountExtrinsic(handle, project?.manifest?.sensor ?: _sensor.value, project?.manifest?.mountCalibration)
    }

    fun pauseCapture() = viewModelScope.launch { engineBridge.pauseCapture() }
    fun resumeCapture() = viewModelScope.launch { engineBridge.resumeCapture() }

    /**
     * ROUND 6 (owner item 20) — **the seal is now uncancellable, verified and
     * loud.**
     *
     * Three things were wrong with the 0.2.1 version of this method, and all
     * three end the same way: a capture the operator watched complete, with
     * nothing in Projects afterwards.
     *
     *  1. **The whole seal ran in `viewModelScope`.** Stop is the one moment a
     *     walkthrough operator is most likely to also leave the screen, put the
     *     phone away, or have the process trimmed — and `viewModelScope` is
     *     cancelled the instant the ViewModel clears. Every `withContext` and
     *     every suspend call below is a cancellation point, so the sequence
     *     could stop halfway with the manifest never written. The body is now
     *     wrapped in [kotlinx.coroutines.NonCancellable]: once Stop is pressed,
     *     sealing runs to completion or reports why it did not.
     *  2. **`updateManifest`'s failure was discarded.** Its result was not even
     *     assigned. It returns null both when the project directory is gone and
     *     when the manifest cannot be parsed — which, because of the
     *     `manifest.json` collision this round also fixes
     *     (`FileProjectStore`'s header), it always was after a real capture.
     *     The capture ended, the summary sheet said "12.4 M points", and the
     *     project was invisible.
     *  3. **Nothing verified the result.** A seal that is not read back is a
     *     hope. This one re-opens the project through the same store the
     *     Projects tab lists with, and if it cannot be found there, it says so
     *     in the UI and writes it to the on-device log.
     */
    fun stopCapture() = viewModelScope.launch {
        kotlinx.coroutines.withContext(kotlinx.coroutines.NonCancellable) {
            try {
                sealAndStop()
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                throw cancelled
            } catch (e: Throwable) {
                // ROUND 6 (item 20): anything thrown in here — a JNI call, a
                // storage failure, a listener — used to escape into
                // `viewModelScope` and be swallowed by the scope's handler, so
                // the seal simply stopped happening and the operator was told
                // nothing whatsoever. That is the exact shape of the field
                // report. A crash on Stop is now a loud, on-screen, logged
                // failure with the on-disk path attached.
                val where = (_uiState.value as? CaptureUiState.Loaded)?.project?.directory?.absolutePath
                _saveError.value =
                    "Saving the scan failed (${e.javaClass.simpleName}: ${e.message}). " +
                        (where?.let { "Its raw data is at $it — do not delete it. " } ?: "") +
                        "The capture log in Settings has the full trace."
                logEvent(LOG_TAG_SEAL, "SEAL THREW: ${e.javaClass.name}: ${e.message}")
                pushbroomEnabled = false
                _pushbroomActive.value = false
            }
        }
    }

    private suspend fun sealAndStop() = sealMutex.withLock {
        // A session seals exactly once, whether the operator pressed Stop or the
        // engine ended the session on its own (see [sealPending]).
        if (!sealPending.compareAndSet(true, false)) return@withLock
        sealAndStopLocked()
    }

    private suspend fun sealAndStopLocked() {
        val finalStats = _stats.value
        // ROUND 5.2: the phone-GNSS fallback belongs to the session, so it stops
        // with it — before the engine handle goes away, since the recorder pushes
        // into that handle.
        stopPhoneGeoref()
        // Keyframes first: the recorder's index has to be flushed and closed
        // while the .lscan is still the live session's, and the ARCore frame
        // listener must stop before the engine handle goes away.
        detachKeyframeListener()
        keyframeRecorder?.stop()
        keyframeRecorder = null
        arController?.engineHandle = 0L

        val handle = engineHandleProvider()
        if (handle != 0L && pushbroomEnabled) {
            // Resolve every pending pushbroom point the poses allow before the
            // session closes. scan_engine_stop() does this too; calling it
            // explicitly means the stats below reflect the flushed totals.
            com.lidarscan.app.engine.ScanEngineNative.nativePushbroomFlush(handle)
            _pushbroomStats.value = com.lidarscan.app.engine.ScanEngineNative.nativePushbroomStats(handle)
        }

        engineBridge.stopCapture()
        _sessionSummary.value = finalStats

        // B5/B9: write what the capture actually produced back into the
        // manifest.
        //
        // `pointCountEstimate` has been flagged as unwired by B2, B4, B7 and B3
        // in turn — `ProjectStore.updateManifest` has existed since B7 and this
        // is the two lines those notes kept asking for. The georef snapshot is
        // A10 §9.6's request ("a periodic GeorefSolution + origin snapshot in
        // the manifest so a replay does not have to re-derive the alignment"),
        // and it is what makes B12's auto-merge possible at all: `merge/session.h`
        // needs each session's transform AND the ENU frame it is expressed in,
        // and neither survives the end of a capture any other way.
        val georef = georefSnapshotProvider(handle)

        // Redesign: the Projects card thumbnail draws this project's OWN cloud,
        // and this is where that becomes possible — the pages are still
        // resident here, one strided sample later they are a 48 KB file next to
        // the processed results. Doing it anywhere else would mean re-decoding
        // the raw streams just to draw a 108 dp tile. Best-effort by design: a
        // failed write costs a thumbnail, never a capture.
        val previewSource = _pointCloudSource.value
        // ROUND 5: the project id comes from the LOADED project, not the
        // constructor — on the Capture tab the constructor's id is null and the
        // project being recorded into is the one Start just created.
        val activeProject = (_uiState.value as? CaptureUiState.Loaded)?.project
        val activeId = activeProject?.id
        val projectDir = activeProject?.directory
        withContext(Dispatchers.IO) {
            if (projectDir != null &&
                activeId != null &&
                com.lidarscan.app.ui.projects.writeProjectPreview(projectDir, previewSource)
            ) {
                com.lidarscan.app.ui.projects.ProjectPreviewCache.invalidate(activeId)
            }
        }

        if (activeId == null) {
            // ROUND 6: there is no project to seal into. Before this, the method
            // simply returned and the operator was told nothing at all.
            pushbroomEnabled = false
            _pushbroomActive.value = false
            _saveError.value =
                "This session had no scan project to save into, so nothing was written. " +
                    "Press Start (not Stop) to begin a scan."
            logEvent(LOG_TAG_SEAL, "stop with NO active project — nothing sealed")
            return
        }

        val sealed = withContext(Dispatchers.IO) {
            projectStore.updateManifest(activeId) { manifest ->
                manifest.copy(
                    pointCountEstimate = finalStats.pointsCaptured.takeIf { it > 0 } ?: manifest.pointCountEstimate,
                    georef = georef ?: manifest.georef,
                    crsEpsg = georef?.epsg?.takeIf { it != 0 } ?: manifest.crsEpsg,
                    // ROUND 5 (owner addition 3): the view the operator actually
                    // recorded with, as this project's default view — including
                    // any change made *during* the recording, which is why it is
                    // written here as well as at creation.
                    displayParams = displayParams.value,
                    // ROUND 6 (item 23): the trim the pushbroom actually ran on,
                    // so post-processing uses the same one.
                    mountTrim = _mountTrim.value ?: manifest.mountTrim,
                )
            }
        }

        // ROUND 6 (item 20): VERIFY. Re-open through the same store the Projects
        // tab lists with — that is the only question worth asking here, and the
        // one nobody was asking.
        val verified = withContext(Dispatchers.IO) { runCatching { projectStore.open(activeId) }.getOrNull() }
        if (sealed == null || verified == null) {
            _saveError.value =
                "The scan finished but could not be saved to the project list. Its raw data IS on the phone at " +
                    "${projectDir?.absolutePath ?: activeId} — do not delete it; the capture log in Settings has " +
                    "the details."
            logEvent(
                LOG_TAG_SEAL,
                "SEAL FAILED id=$activeId sealedManifest=${sealed != null} readback=${verified != null} " +
                    "points=${finalStats.pointsCaptured} dir=${projectDir?.absolutePath}",
            )
        } else {
            _lastSavedProject.value = verified.directory.absolutePath
            logEvent(
                LOG_TAG_SEAL,
                "sealed OK id=$activeId name=\"${verified.manifest.name}\" " +
                    "points=${finalStats.pointsCaptured} elapsedMs=${finalStats.elapsedMillis} " +
                    "listable=true dir=${verified.directory.absolutePath}",
            )
        }
        pushbroomEnabled = false
        _pushbroomActive.value = false
        // ROUND 5 AUDIT bugfix (multi-cycle recording): on the Capture tab
        // (`projectId == null` — item 9's "Start creates the project"),
        // `startCapture()` treats `_uiState is Loaded` as "record into THIS
        // project" (`(_uiState.value as? Loaded)?.project ?:
        // createProjectForThisScan()`). Leaving `_uiState` as
        // `Loaded(sealedProject)` here — which is what this used to do
        // unconditionally — meant a second Start within the same connect
        // session silently re-opened and re-recorded into project #1 instead
        // of creating project #2: `createProjectForThisScan()` was never
        // reached, so the series counter was never spent and no second
        // `.lscan` directory was ever created. `autoConnect`'s own state (the
        // sensor is still connected and PREVIEWING) is untouched by this — the
        // connect session survives, only the "which project is Start about to
        // record into" state resets, exactly matching round 5's own "Start
        // creates the project" contract on every Start, not just the first.
        //
        // A project-scoped entry (`projectId != null` — the replay/deep-link
        // route, or a future "record into an existing project" flow) has no
        // such ambiguity: there is only ever the one project, so it keeps
        // refreshing `Loaded` with the manifest Stop just wrote, per the
        // original comment ("the in-memory project must not go stale, or a
        // later start would re-read the old manifest") this replaced.
        if (projectId == null) {
            _uiState.value = CaptureUiState.NewScan(
                autoName = com.lidarscan.core.capture.ScanAutoName.format(
                    series = runCatching { peekSeriesNumber() }.getOrDefault(1),
                    epochMillis = clock(),
                ),
            )
        } else {
            projectStore.open(activeId)?.let { _uiState.value = CaptureUiState.Loaded(it) }
        }
    }

    fun dismissSessionSummary() {
        _sessionSummary.value = null
    }

    fun setLiveSlam(enabled: Boolean) {
        _liveSlam.value = enabled
    }

    fun setColorMode(mode: ColorMode) {
        _colorMode.value = mode
    }

    fun setColormap(cm: Colormap) {
        _colormap.value = cm
    }

    /**
     * ROUND 5 (owner addition 2): point size is 0.1 – 3.0 px in 0.1 steps, and
     * the snap happens **here** rather than in the slider — so the value the
     * renderer, the read-out and the manifest all see is the same one, whichever
     * control moved it.
     */
    fun setPointSizePx(px: Float) {
        _pointSizePx.value = com.lidarscan.core.render.DisplayLimits.snapPointSize(px)
    }

    fun setCameraMode(mode: CameraMode) {
        _cameraMode.value = mode
    }

    /** ROUND 5: viewport refresh cap in fps, 0 = uncapped. Display-only; the recording is unaffected. */
    /**
     * ROUND 5 AUDIT bugfix: this used to clamp anything `>= 60` back to `0`
     * ("Max") — a leftover from BEFORE round 5.3 lifted the viewport refresh
     * cap's ceiling to the device's real rate (`PointCloudRenderer
     * .setMaxRefreshHz`'s own doc comment: "the cap is no longer clamped at
     * 59"). `RefreshGovernor.optionsFor()` offers 60/90/120 fps choices on a
     * fast phone, and every one of them silently became "Max" here instead —
     * see [refreshRequestToken] for the other half of the recovery fix this
     * sits next to.
     */
    fun setRefreshHz(hz: Int) {
        _refreshHz.value = if (hz > 0) hz else 0
        _refreshRequestToken.value++
        markCustomIfDiverged()
    }

    /** ROUND 6 (item 22, Light): draw the registered/pushbroom map, or only raw sensor pages. */
    fun setLiveMapEnabled(enabled: Boolean) {
        _liveMapEnabled.value = enabled
        markCustomIfDiverged()
    }

    fun setGamma(value: Float) {
        _gamma.value = value.coerceIn(
            com.lidarscan.core.render.DisplayLimits.GAMMA_MIN,
            com.lidarscan.core.render.DisplayLimits.GAMMA_MAX,
        )
    }

    fun setBrightness(value: Float) {
        _brightness.value = value.coerceIn(
            com.lidarscan.core.render.DisplayLimits.BRIGHTNESS_MIN,
            com.lidarscan.core.render.DisplayLimits.BRIGHTNESS_MAX,
        )
    }

    /** ROUND 5: the Live toggle — viewport streaming on/off, never the recording. */
    fun setLiveView(enabled: Boolean) {
        _liveView.value = enabled
    }

    /** ROUND 5: the one name field on the Capture tab. Blank means "auto-name it". */
    fun setScanName(value: String) {
        _scanName.value = value
    }

    /** ROUND 5: the profile the next Start stamps on the project it creates. */
    fun setProfile(profile: com.lidarscan.core.model.WorkflowProfile) {
        _profile.value = profile
    }

    /** Sheet: camera keyframes on/off. Applies to a live recorder immediately. */
    fun setKeyframesEnabled(enabled: Boolean) {
        _keyframesEnabled.value = enabled
        keyframeRecorder?.setEnabled(enabled)
        markCustomIfDiverged()
    }

    /** Sheet: 2 / 3 / 5 fps. Applies to a live recorder immediately. */
    fun setKeyframeRateFps(fps: Int) {
        _keyframeRateFps.value = fps
        keyframeRecorder?.setTargetFps(fps.toDouble())
        markCustomIfDiverged()
    }

    /** Sheet: §3.12's LOD budget, in millions of points. */
    fun setLodBudgetMPoints(mPoints: Int) {
        _lodBudgetMPoints.value = mPoints.coerceIn(1, 200)
        markCustomIfDiverged()
    }

    override fun onCleared() {
        stopPhoneGeoref()
        detachTrailListener()
        detachKeyframeListener()
        keyframeRecorder?.shutdown()
        keyframeRecorder = null
        arController?.engineHandle = 0L
        super.onCleared()
    }

    private fun directorySizeBytes(dir: java.io.File): Long {
        if (!dir.exists()) return 0L
        return dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
    }

    private companion object {
        /** ROUND 6 log tags — mirrors `com.lidarscan.app.debug.CaptureLog`'s, without depending on it. */
        const val LOG_TAG_SESSION = "session"
        const val LOG_TAG_SEAL = "seal"
        const val LOG_TAG_AR = "ar"
        const val LOG_TAG_PUSHBROOM = "pushbroom"
        const val LOG_TAG_STORE = "store"

        /** ROUND 6 (item 21): how often the live page-store ceiling is checked. */
        const val LIVE_MAP_WATCH_MS = 1_000L

        /** How often the motion hint is re-evaluated (ROUND 5.3 item 18). */
        const val MOTION_HINT_TICK_MS = 500L

        /**
         * How long after the last motion-gated skip the hint stays up. Long enough
         * to be readable mid-walk, short enough that it clears within a couple of
         * steps of slowing down — a hint that lingers becomes wallpaper.
         */
        const val MOTION_HINT_LINGER_MS = 2_500L
    }
}
