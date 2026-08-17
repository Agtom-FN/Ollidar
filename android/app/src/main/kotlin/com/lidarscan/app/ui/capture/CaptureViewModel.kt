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

    /**
     * Viewport refresh cap in fps (0 = uncapped). A *display* throttle: the
     * engine keeps decoding and writing at full rate either way — see
     * `PointCloudRenderer.setMaxRefreshHz`.
     */
    private val _refreshHz = MutableStateFlow(0)
    val refreshHz: StateFlow<Int> = _refreshHz.asStateFlow()

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
    private val _keyframesEnabled = MutableStateFlow(true)
    val keyframesEnabled: StateFlow<Boolean> = _keyframesEnabled.asStateFlow()

    /** §3.5's 2–5 fps cadence, as the sheet's 2 / 3 / 5 row. */
    private val _keyframeRateFps = MutableStateFlow(3)
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
    private val _lodBudgetMPoints = MutableStateFlow(20)
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
    private val trailRecorder = com.lidarscan.app.capture.TrajectoryTrailRecorder()
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
        viewModelScope.launch {
            val project = (_uiState.value as? CaptureUiState.Loaded)?.project
                ?: createProjectForThisScan()
                ?: return@launch
            val started = engineBridge.startCapture(
                project.directory.absolutePath,
                _liveSlam.value,
                com.lidarscan.core.model.CaptureDefaults.engineProfileString(project.manifest.profile),
            )
            if (started.isFailure) return@launch
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
        }.getOrNull() ?: return@withContext null

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

        val measured = project.manifest.mountCalibration
            ?: mountCalibrationFor(project.manifest.sensor)
        val nominalMatrix = com.lidarscan.core.calib.BracketNominals.cadNominal(project.manifest.sensor)
        val matrix = measured
            ?.let { com.lidarscan.core.calib.Mat4(it.cameraFromLidar.copyOf()) }
            ?.takeIf { it.isRigid(1e-4) }
            ?: nominalMatrix
        val usingNominal = measured == null ||
            !com.lidarscan.core.calib.Mat4(measured.cameraFromLidar.copyOf()).isRigid(1e-4)

        if (handle != 0L) {
            val err = com.lidarscan.app.engine.ScanEngineNative
                .nativeSetMountExtrinsics(handle, matrix.m)
            if (err == com.lidarscan.app.engine.ScanEngineNative.ErrorCode.OK) {
                com.lidarscan.app.engine.ScanEngineNative.nativePushbroomEnable(handle, true)
                _mountCalibrationApplied.value = measured
                _mountIsNominal.value = usingNominal
                pushbroomEnabled = true
            }
        }

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

    fun pauseCapture() = viewModelScope.launch { engineBridge.pauseCapture() }
    fun resumeCapture() = viewModelScope.launch { engineBridge.resumeCapture() }

    fun stopCapture() = viewModelScope.launch {
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
            pushbroomEnabled = false
            return@launch
        }

        withContext(Dispatchers.IO) {
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
                )
            }
        }
        pushbroomEnabled = false
        // The in-memory project must not go stale, or a later start would
        // re-read the old manifest.
        projectStore.open(activeId)?.let { _uiState.value = CaptureUiState.Loaded(it) }
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
    fun setRefreshHz(hz: Int) {
        _refreshHz.value = if (hz in 1..59) hz else 0
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
    }

    /** Sheet: 2 / 3 / 5 fps. Applies to a live recorder immediately. */
    fun setKeyframeRateFps(fps: Int) {
        _keyframeRateFps.value = fps
        keyframeRecorder?.setTargetFps(fps.toDouble())
    }

    /** Sheet: §3.12's LOD budget, in millions of points. */
    fun setLodBudgetMPoints(mPoints: Int) {
        _lodBudgetMPoints.value = mPoints.coerceIn(1, 200)
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
