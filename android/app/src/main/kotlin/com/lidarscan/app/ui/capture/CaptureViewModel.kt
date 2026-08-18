package com.lidarscan.app.ui.capture

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.NativePointCloudProvider
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.core.capture.StitchResult
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
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.SharingStarted
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

/**
 * ROUND 15 item 55 — what auto-process is doing, for the summary card.
 *
 * [skipped] and [failed] are different states on purpose, and both are
 * different from "no result". Skipped means the fast path decided there was
 * nothing worth doing (one section, no warning) and the card should say so
 * rather than showing a bar that never moves. Failed means the run did not
 * complete, and the card must then say the one thing the operator can act on
 * — the scan itself is sealed, verified and untouched either way.
 */
data class AutoProcessState(
    val projectId: String? = null,
    val running: Boolean = false,
    val progress: Float = 0f,
    val willStitch: Boolean = false,
    val skipped: Boolean = false,
    val failed: Boolean = false,
    /**
     * ROUND 16 item 58(c) — why processing could not even be attempted.
     *
     * Distinct from [failed] with no reason: "it failed, try again" is an
     * instruction, and on a capture with no poses it is an instruction that can
     * only fail again. Non-null means the app knew beforehand and says what it
     * knew.
     */
    val blocked: String? = null,
    val result: StitchResult? = null,
) {
    val active: Boolean get() = projectId != null

    /** The line under the grade banner while this is happening or after it. */
    val line: String?
        get() = when {
            !active -> null
            running && willStitch -> "Putting the pieces back together…"
            running -> "Checking the scan…"
            // Before `failed`, because a blocked run IS failed and the specific
            // sentence is the useful one.
            blocked != null -> blocked
            failed ->
                "Processing failed — the scan is saved. Open it and tap Process to try again."
            // The RESULT outranks `skipped`: once processing has answered, its
            // own sentence is the honest one, and for a one-section scan that
            // sentence already says the scan was recorded in one piece. Written
            // the other way round first, which made the fast path hide its own
            // findings behind a description of the route it took.
            result != null -> result.headline
            skipped -> "Recorded in one piece — nothing needed aligning."
            else -> null
        }
}

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
    /**
     * ROUND 9 (owner item 35): the phone's own gyro + accelerometer, pushed into
     * the engine's IMU-densified pose interpolator for the whole of a capture.
     *
     * Null for a replay session (nothing live to densify) and for any build or
     * device without the sensors — the interpolator then falls back to plain
     * SLERP, which is degraded but correct. A separate constructor slot rather
     * than a field of [arController] because it is not an ARCore object and must
     * not be gated on there being a camera controller, the same lesson ROUND 8
     * item 30d learned about the mount extrinsic.
     */
    private val phoneImu: com.lidarscan.app.ar.PhoneImuRecorder? = null,
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
     * ROUND 17 item 66 — the per-capture debug log, as three lambdas for the
     * same reason [logEvent] is one.
     *
     * [beginDebugLog] is handed the capture's own `.lscan` directory and
     * decides for itself (from Developer Mode) whether to open a sink;
     * [logDebug] is a no-op whenever no sink is open, which is what lets it be
     * called freely from hot paths without a flag check at every site;
     * [endDebugLog] closes it at the seal.
     *
     * Verbose on purpose. `capture.log` has to stay readable because a person
     * reads the whole of it looking for one line; this one is read by grep,
     * travels inside the bundle it describes, and is explicitly not a stream.
     */
    private val beginDebugLog: (java.io.File, String) -> Unit = { _, _ -> },
    private val logDebug: (String, String) -> Unit = { _, _ -> },
    private val endDebugLog: (String) -> Unit = { },
    /**
     * ROUND 18 item 71 — the project-guarded variants, for writes that happen
     * AFTER the seal (auto-process verdicts). The sink now stays open through
     * auto-process, and a new capture may begin meanwhile; these write/close
     * only when the sink still belongs to the named bundle, so a late
     * completion can never scribble into the wrong capture's log.
     */
    private val logDebugFor: (java.io.File, String, String) -> Unit = { _, _, _ -> },
    private val endDebugLogFor: (java.io.File, String) -> Unit = { _, _ -> },
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
    /**
     * ROUND 7 (field bug 1): the mount re-zero the app last stored, if any. Read
     * once in `init` so a trim set before a trip to the Projects tab — or before
     * an app restart — is still in force at the next Start.
     */
    private val loadStoredMountTrim: suspend () -> com.lidarscan.core.calib.StoredMountTrim? = { null },
    /** ROUND 7: persists (or, with null, forgets) the mount re-zero. */
    private val persistMountTrim: suspend (com.lidarscan.core.calib.StoredMountTrim?) -> Unit = {},
    /** ROUND 7: this app process's id, so a restored trim can be told apart from one set on this screen. */
    private val appRunId: String = "",
    /**
     * ROUND 9 (owner item 33): the device setting behind "a scan that recorded
     * nothing is not a scan" — see [com.lidarscan.app.data.AppSettings.keepEmptyScans].
     *
     * A suspend supplier rather than a snapshot, read at Stop, so flipping the
     * switch in Settings applies to the very next capture rather than to the
     * next time this ViewModel is built. The default matches the app default
     * (`false` = prune), so a bare-JVM test sees shipped behaviour.
     */
    private val keepEmptyScans: suspend () -> Boolean = { false },
    /**
     * ROUND 11 (owner item 43): plays one operator cue — a vibration pattern
     * and a tone. A lambda for the same reason [logEvent] is one: the ViewModel
     * holds no `Context` (it is a plain `ViewModel`, not an `AndroidViewModel`),
     * and every Android capability it has arrives injected from `CaptureRoute`.
     * The default is a no-op so every JVM test builds cue-free.
     *
     * The implementation must not block: it is called from the 500 ms hint
     * ticker on the main dispatcher, and `Vibrator.vibrate` + `ToneGenerator`
     * are handed to a background executor by `OperatorCuePlayer`.
     */
    private val playCue: (com.lidarscan.core.capture.CueKind) -> Unit = {},
    /**
     * ROUND 11 (owner item 43): the Settings switch, default ON. A suspend
     * supplier, read on the same tick as the cue decision, so flipping it
     * applies to the capture already running.
     */
    private val cuesEnabled: suspend () -> Boolean = { true },
    /**
     * ROUND 13 (owner item 47): silence notifications for the duration of a
     * capture and put the filter back afterwards. A lambda pair rather than the
     * guard itself so `:app` unit tests can drive the lifecycle without a
     * framework NotificationManager; the real one is
     * [com.lidarscan.app.capture.DoNotDisturbGuard].
     *
     * `engageDnd` returns the state that goes in the session-start log line;
     * `releaseDnd` must be safe to call more than once and on a capture that
     * never engaged.
     */
    private val engageDnd: suspend () -> com.lidarscan.core.capture.DndState = {
        com.lidarscan.core.capture.DndState.DISABLED
    },
    private val releaseDnd: () -> Unit = {},
    /**
     * ROUND 14 (owner item 53) — the phone's Ethernet interface, as
     * `EthernetMonitor` sees it: (an adapter is present, the IPv4 addresses it
     * holds). A lambda rather than the monitor itself so the decision stays
     * testable without a ConnectivityManager. Defaults to "cannot tell", which
     * the preflight treats as no adapter — correct for a D6 rig, which never
     * consults it.
     */
    private val ethernetSnapshot: () -> Pair<Boolean, List<String>> = { false to emptyList() },
    /**
     * ROUND 15 item 55 — auto-process on seal.
     *
     * Takes the sealed container's directory and a progress sink (return false
     * to cancel) and returns what processing concluded, or null if it could
     * not run. A lambda for the same reason every other capability here is
     * one: the ViewModel holds no Context and no engine, and a bare-JVM test
     * must be able to drive the whole Stop -> process -> card -> Projects
     * choreography without a native library.
     *
     * The real implementation is `ProcessingRepository.reprocessD6`, which is
     * HANDLE-LESS: it opens its own PageStore inside the engine from a
     * directory. That is what makes it safe to run while the capture tab has
     * already re-armed and the next scan may be starting on the live engine
     * handle — the two share no state at all. The default is a no-op, so a
     * test that does not care sees the ROUND 10 flow unchanged.
     */
    private val runAutoProcess: suspend (java.io.File, (Float) -> Boolean) -> StitchResult? =
        { _, _ -> null },
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

    /**
     * ROUND 11 (owner item 44) — the scan summary card.
     *
     * A sibling of [sessionSummary] rather than a replacement for it: that flow
     * is the raw counters and is what three rounds of tests assert on, while
     * this is the graded, human-readable answer to "keep or rescan". Both are
     * snapshotted at the same instant inside the seal and both are cleared by
     * [dismissSessionSummary], so they can never describe different scans.
     *
     * The navigation to Projects WAITS on this being dismissed — see
     * `CaptureScreen`'s pending-navigation effect. Sealing and then jumping
     * away would show the card for one frame.
     */
    private val _scanSummary = MutableStateFlow<com.lidarscan.core.capture.ScanSummary?>(null)
    val scanSummary: StateFlow<com.lidarscan.core.capture.ScanSummary?> = _scanSummary.asStateFlow()

    private val _pointCloudSource = MutableStateFlow<PointCloudSource?>(null)
    val pointCloudSource: StateFlow<PointCloudSource?> = _pointCloudSource.asStateFlow()

    // --- B10-adjacent display controls (A14's contract, RGB/height/intensity
    // + point size + camera mode — the subset B4 needs; a full display-params
    // panel is B10's job). Kept in the ViewModel (not Compose `remember`) so
    // rotating the device (landscape<->portrait, explicitly in B4's scope)
    // doesn't reset them.
    //
    // ROUND 8 (owner item 29): the four defaults below are no longer this
    // file's own opinion — they are
    // [com.lidarscan.core.render.DisplayParams.Companion.captureDefaults], which
    // is where the reasoning and the field evidence live. What the owner's real
    // capture wrote into its own `project.json` was
    // `pointSize.fixedPx: 2.5` in `colorMode: RGB` — 2.5 px points on an indoor
    // cloud read as a solid smear, and RGB on a D6 return (which has no colour)
    // is a pass-through of nothing.
    private val _colorMode = MutableStateFlow(com.lidarscan.core.render.DisplayParams.CAPTURE_COLOR_MODE)
    val colorMode: StateFlow<ColorMode> = _colorMode.asStateFlow()
    // ROUND 10 (owner item 39): the colormap default now comes from the ONE
    // place it is stated (`DisplayParams.CAPTURE_COLORMAP` = GRAYSCALE) rather
    // than being a second, disagreeing literal here.
    private val _colormap = MutableStateFlow(com.lidarscan.core.render.DisplayParams.CAPTURE_COLORMAP)
    val colormap: StateFlow<Colormap> = _colormap.asStateFlow()
    private val _pointSizePx =
        MutableStateFlow(com.lidarscan.core.render.DisplayParams.CAPTURE_POINT_SIZE_PX)
    val pointSizePx: StateFlow<Float> = _pointSizePx.asStateFlow()

    /**
     * ROUND 8, owner directive *"i need a live 3d mapping too"* — **FOLLOW is
     * the default camera during a D6 capture.**
     *
     * A COIN-D6 walkthrough is the one case where the operator and the camera
     * are the same object: the phone IS the rig, the pushbroom is laying points
     * down in front of it, and an orbit camera parked at the origin means the
     * live map walks off screen within a few metres of the start. ORBIT is the
     * right default for a cloud you are inspecting; FOLLOW is the right default
     * for a cloud you are *making*.
     *
     * A replay session is inspection, not capture — it has no operator walking
     * anywhere — so it keeps ORBIT. What FOLLOW does to the camera is
     * `PointCloudRenderer`'s (a concurrent task owns that package); this sets
     * the default and the View row's initial selection, which is the half that
     * lives here.
     *
     * ## ROUND 10 (owner item 39): FOLLOW is PAUSED, so this is ORBIT
     *
     * *"disable the follow and rgb since we dont use the camera now. default
     * scan setting show be 3d orbit…"*. Everything above stays true and stays
     * the argument for reviving it — `FeatureFlags.FOLLOW_CAMERA_ENABLED` is
     * the one line that does. Until then the initial mode is ORBIT for capture
     * as well as replay, and the two Orbit/Follow controls are not drawn.
     */
    private val _cameraMode = MutableStateFlow(
        if (isReplay || !com.lidarscan.core.FeatureFlags.FOLLOW_CAMERA_ENABLED) {
            CameraMode.ORBIT
        } else {
            CameraMode.FOLLOW
        },
    )
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
    private val _gamma = MutableStateFlow(com.lidarscan.core.render.DisplayParams.CAPTURE_GAMMA)
    val gamma: StateFlow<Float> = _gamma.asStateFlow()

    /** A14's scalar `brightness` (0.1 – 3.0). */
    private val _brightness = MutableStateFlow(com.lidarscan.core.render.DisplayParams.CAPTURE_BRIGHTNESS)
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
    // ROUND 10 (owner item 39): colorization is paused, and camera keyframes
    // exist for nothing else. Gated at the STATE and not only at the UI, so a
    // preset switch (which writes this flow from `CaptureTuning`) cannot turn
    // the recorder back on behind a hidden control — see the `&& COLORIZE_ENABLED`
    // in `applyTuning` and in `setKeyframesEnabled` for the other two doors.
    private val _keyframesEnabled = MutableStateFlow(
        defaultTuning.keyframesEnabled && com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED,
    )
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
                // ROUND 8 (owner item 29): `mode` is set EXPLICITLY.
                //
                // This used to be `PointSizeParams(fixedPx = size)`, which
                // leaves `mode` on the data class's own default — `ADAPTIVE`.
                // The capture sheet's only point-size control is a 0.1–3.0 px
                // slider that writes `fixedPx`, and a renderer honouring the
                // mode reads `adaptiveMin/Max/Reference` and never looks at
                // `fixedPx` at all. So the control the owner was moving wrote a
                // field the declared mode said to ignore, and the owner's own
                // `project.json` recorded the contradiction verbatim:
                // `"mode": "ADAPTIVE", "fixedPx": 2.5`.
                pointSize = com.lidarscan.core.render.PointSizeParams(
                    mode = com.lidarscan.core.render.PointSizeMode.FIXED_PIXELS,
                    fixedPx = size,
                ),
                lodPointBudget = (lodM.coerceIn(1, 200) * 1_000_000),
                // The redesign's viewport ground, so the live view and the
                // project thumbnails sit on the same black.
                background = com.lidarscan.core.render.Rgba(11, 14, 18, 255),
            )
        }.stateIn(
            viewModelScope,
            kotlinx.coroutines.flow.SharingStarted.Companion.WhileSubscribed(5_000),
            // ROUND 8 (item 29): the seed a collector sees before the first
            // `combine` emission is the same block the controls are seeded from,
            // not `DisplayParams()`'s engine-side defaults (RGB / 2 px /
            // ADAPTIVE) — otherwise the very first frame of a capture is drawn
            // with a display nobody chose and the manifest can be written with
            // it too (`createProjectForThisScan` reads `displayParams.value`).
            com.lidarscan.core.render.DisplayParams.captureDefaults(),
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
                        // ROUND 14 (item 53): a Mid-360 detection IS a parsed
                        // heartbeat — `Mid360HeartbeatAutoDetector` cannot
                        // report one without having decoded a real beacon — so
                        // this is the honest timestamp for "the lidar was
                        // talking on the cable", and the preflight needs it.
                        if (detection.sensor == com.lidarscan.core.model.SensorType.MID360) {
                            lastMid360HeartbeatMillis = clock()
                        }
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

    /**
     * ROUND 16 item 59 — the walk as a coloured line strip, ready for the 3D
     * scene, recomputed only when the trail actually grows (the recorder
     * refuses points closer than 15 cm, so at walking pace that is about five
     * times a second against a 60 Hz frame).
     *
     * Kept beside [trailPoints] rather than replacing it: the 108 dp bird's-eye
     * tile and the in-cloud ribbon are two views of one walk and both are
     * useful — the tile answers "what shape did I cover" at a glance with the
     * cloud in the way, the ribbon answers "does the room agree with where I
     * was". They are built from the same snapshot in the same instant, so they
     * cannot disagree.
     */
    val trailRibbon: StateFlow<com.lidarscan.core.capture.TrajectoryRibbon.Ribbon> =
        trailRecorder.worldPoints
            .map { com.lidarscan.core.capture.TrajectoryRibbon.fromTrail(it) }
            .stateIn(
                viewModelScope,
                SharingStarted.Eagerly,
                com.lidarscan.core.capture.TrajectoryRibbon.EMPTY,
            )

    /**
     * ROUND 16 items 59 + 61 — the view control's toggle, default ON, and it is
     * `DisplayParams.showTrajectory` rather than a new Boolean.
     *
     * ON by default in the live view because that is where it is a working
     * instrument rather than a decoration: the operator is holding the phone
     * and can act on what it shows. Read from the same [displayParams] the
     * Capture sheet edits and the manifest persists, so the setting the
     * operator changes in Review is the setting they get on the next walk —
     * one switch, one meaning, which is what item 61 is about.
     */
    val showTrajectory: StateFlow<Boolean> =
        displayParams
            .map { it.showTrajectory }
            .stateIn(viewModelScope, SharingStarted.Eagerly, true)

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
     * ROUND 11 (owner item 43). One scheduler for the ViewModel's life, reset
     * per session — the lesson ROUND 10 item 38 spent a whole round on, applied
     * to the one new piece of per-session state this round adds.
     */
    private val cues = com.lidarscan.core.capture.CueScheduler()

    /**
     * ROUND 14 (owner item 50) — stateless, so it needs no per-session reset;
     * it reads the pose window the ROUND 12 start gate already keeps.
     */
    private val parallaxWatch = com.lidarscan.core.capture.ParallaxWatch()

    /** ROUND 14 (item 53) — when a Mid-360 heartbeat was last parsed, or null. */
    private var lastMid360HeartbeatMillis: Long? = null

    /**
     * ROUND 14 (item 53) — the Mid-360 addressing check, or null when this is
     * not a Mid-360 rig. Exposed so the capture screen can show the same
     * verdict before Start rather than only refusing at it.
     */
    fun mid360Preflight(): com.lidarscan.core.net.Mid360Preflight.Verdict? {
        if (_sensor.value != com.lidarscan.core.model.SensorType.MID360) return null
        val (adapter, addresses) = ethernetSnapshot()
        // The endpoint is "lidarIp|hostIp"; the host half is the address the
        // lidar has persisted and will unicast to.
        val host = _mid360Endpoint.value?.substringAfter('|', "")?.takeIf { it.isNotBlank() }
            ?: _manualHostIp.value.takeIf { it.isNotBlank() }
        return com.lidarscan.core.net.Mid360Preflight.evaluate(
            adapterPresent = adapter,
            interfaceAddresses = addresses,
            expectedHostIp = host,
            heartbeatAgeMillis = lastMid360HeartbeatMillis?.let { clock() - it },
        )
    }

    /** Mirrors what the scheduler last played, for the Diagnostics sheet and the log. */
    private val _lastCue = MutableStateFlow<com.lidarscan.core.capture.CueKind?>(null)
    val lastCue: StateFlow<com.lidarscan.core.capture.CueKind?> = _lastCue.asStateFlow()

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

    /**
     * ROUND 8, owner item 31 — **the id of the scan a Stop just sealed, once.**
     *
     * > "stop => seal => navigate to Projects with the new scan selected"
     *
     * The capture flow used to end nowhere. Stop sealed the project, showed a
     * summary sheet, and left the operator on a Capture tab whose viewport was
     * still drawing the scan they had just finished — with the scan itself
     * reachable only by remembering to tap Projects. Every one of the owner's
     * two lost field sessions was discovered *in the Projects tab*, which is
     * where a finished capture's evidence lives and therefore where a finished
     * capture should land you.
     *
     * A one-shot rather than a `StateFlow`: navigation is an event, and a
     * `StateFlow` holding the last sealed id would re-navigate on every
     * recomposition and every configuration change.
     *
     * ## ROUND 10 (owner item 38): `replay` was 0, and the comment that said
     * why was wrong
     *
     * The owner's words: *"when i finish the capture and click stop, it will
     * stay with the capture page but not heading to project."*
     *
     * This flow used to be `replay = 0`, justified in a comment which claimed
     * that the buffer meant *"a collector that is momentarily absent … does not
     * make the emit block or drop silently"*. **That is not what a
     * `MutableSharedFlow` does.** `extraBufferCapacity` is slack for a
     * subscriber that is SLOW; with **zero** subscribers, a `replay = 0` flow
     * discards the value and `tryEmit` still returns true. The buffer protects
     * against a collector that is behind, and against a collector that is not
     * there it does nothing at all.
     *
     * And the collector genuinely can be absent at the moment of the emit.
     * `stopCapture()` runs in `viewModelScope` under `NonCancellable`, so the
     * seal SURVIVES the composition being disposed — an Activity recreation
     * (configuration change, a system reclaim, the screen turning off and on
     * mid-seal) tears down the `LaunchedEffect` that collects this while the
     * seal carries on to completion. The scan is saved, the log says
     * `sealed OK`, and the navigation event is emitted into an empty room.
     * Which is precisely the report: sealed fine, never navigated.
     *
     * `replay = 1` is the fix and it is the right shape for this event: the id
     * is delivered to whichever collector attaches, whenever it attaches, and
     * because the buffer belongs to the ViewModel — which nav destroys on the
     * way to Projects — it cannot survive to re-navigate later. It is
     * re-delivered on a recomposition only if the whole ViewModel outlived one,
     * and in that case re-delivering is exactly what should happen.
     */
    private val _sealedProjectId = MutableSharedFlow<String>(
        replay = 1,
        extraBufferCapacity = 4,
        onBufferOverflow = kotlinx.coroutines.channels.BufferOverflow.DROP_OLDEST,
    )
    val sealedProjectId: SharedFlow<String> = _sealedProjectId.asSharedFlow()

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

    /**
     * ROUND 7 (field bug 2) — **a recording that is receiving nothing says so,
     * out loud, while it is still running.**
     *
     * The owner's log:
     *
     * ```
     * 22:53:40 [seal] sealed OK id=scan-008 … points=216653 elapsedMs=30543
     * 22:54:06 [session] start: project=scan-009 … sensor=COIN_D6
     * 22:54:16 [seal] sealed OK id=scan-009 … points=0 elapsedMs=0
     * ```
     *
     * A ten-second walk that recorded nothing at all, reported as `sealed OK`,
     * with a green Stop button and a session-summary sheet the whole way. The
     * cause was `RealEngineBridge`'s own transport latch (see its `startCapture`
     * — the first Stop turned the D6 reader's forwarding off and only the Pause
     * button ever turned it back on), and that specific bug is fixed. This is
     * the part that makes the *class* of bug impossible to ship silently again:
     * whatever the reason, a capture past [NO_DATA_GRACE_MS] with zero points is
     * a red banner naming what the transport is actually doing, and a line in
     * the capture log with the same numbers.
     *
     * Deliberately driven by the same `CaptureStats` and `DeviceHealth` the
     * screen already shows, so it cannot disagree with them, and cleared the
     * instant the first point lands.
     */
    private val _noDataAlert = MutableStateFlow<String?>(null)
    val noDataAlert: StateFlow<String?> = _noDataAlert.asStateFlow()

    /**
     * ROUND 7, item 3: how many contiguous sections this capture is in.
     *
     * 1 for a scan that never lost ARCore's frame, which is the normal case and
     * shows nothing on screen. Above 1 the capture panel says so inline, because
     * a seam is something the operator can act on while still in the room
     * ("walk that stretch again with more texture in view") and cannot act on at
     * all once the phone is back in a pocket.
     */
    /**
     * ROUND 15 item 54 — breaks the live healer could NOT absorb. The section
     * count still counts every break (they are all recorded, and the offline
     * stitch still runs on all of them); this counts the ones the operator can
     * actually see, and it is what the cue scheduler keys on.
     */
    private val _unhealedSectionBreaks = MutableStateFlow(0)
    val unhealedSectionBreaks: StateFlow<Int> = _unhealedSectionBreaks.asStateFlow()

    private val _sectionCount = MutableStateFlow(1)
    val sectionCount: StateFlow<Int> = _sectionCount.asStateFlow()

    val sectionHint: StateFlow<String?> = combine(_sectionCount, _unhealedSectionBreaks) { n, u ->
        com.lidarscan.core.capture.sectionHint(n, u)
    }.stateIn(viewModelScope, SharingStarted.Eagerly, null)

    /** Wall-clock start of the running capture, for the no-data watchdog. 0 when not recording. */
    private var recordingStartedAtMillis = 0L

    /** True once this session has warned; one log line per session, not one per tick. */
    private var noDataLogged = false

    // Declared ABOVE its first use on purpose: ROUND 6 lost an app launch to a
    // property that `init {}` dereferenced before its initializer had run.
    private var noDataWatchJob: kotlinx.coroutines.Job? = null

    private fun startNoDataWatchdog() {
        noDataWatchJob?.cancel()
        noDataLogged = false
        _noDataAlert.value = null
        recordingStartedAtMillis = clock()
        noDataWatchJob = viewModelScope.launch {
            while (true) {
                kotlinx.coroutines.delay(NO_DATA_TICK_MS)
                val state = engineBridge.captureState.value
                if (state != CaptureState.RECORDING) {
                    // A pause is not a fault: the operator asked for silence.
                    if (state != CaptureState.PAUSED) _noDataAlert.value = null
                    continue
                }
                if (_stats.value.pointsCaptured > 0L) {
                    _noDataAlert.value = null
                    continue
                }
                val elapsed = clock() - recordingStartedAtMillis
                if (elapsed < NO_DATA_GRACE_MS) continue

                val health = engineBridge.deviceHealth.value
                val bytes = health?.bytesIn ?: 0L
                val ok = health?.packetsOk ?: 0L
                val bad = health?.packetsBad ?: 0L
                val seconds = elapsed / 1000
                val message = when {
                    bytes == 0L ->
                        "NO DATA — ${seconds}s into this scan and the ${_sensor.value.displayName} has sent " +
                            "0 bytes. Nothing is being recorded. Stop, re-seat the USB-C cable, and start again."
                    ok == 0L ->
                        "NO DATA — ${seconds}s in: $bytes bytes arrived but not one valid packet " +
                            "($bad rejected). The cable is alive and the data is not the sensor's. " +
                            "Stop and start again; if it repeats, the baud or the cable is wrong."
                    else ->
                        "NO POINTS — ${seconds}s in: $ok packets decoded but no points reached the scan. " +
                            "Stop and start again, and send the capture log from Settings."
                }
                // The LOG first, then the screen. The log is the evidence the
                // next field report arrives with, and a UI collector resuming
                // on the state change must never be able to observe the alert
                // before the record of it exists.
                if (!noDataLogged) {
                    noDataLogged = true
                    logEvent(
                        LOG_TAG_SESSION,
                        "NO DATA after ${elapsed}ms: bytesIn=$bytes packetsOk=$ok packetsBad=$bad " +
                            "pointsOut=${health?.pointsOut ?: 0} state=$state sensor=${_sensor.value}",
                    )
                }
                _noDataAlert.value = message
            }
        }
    }

    // ── ROUND 16 item 58(b): THE POSE WATCHDOG ───────────────────────────────
    //
    // The same shape as the ROUND 7 no-data watchdog above, applied to the
    // other half of a D6 capture. That one answers "is the sensor sending?";
    // this one answers "does the app know where the sensor IS?", and until this
    // round nothing did.
    //
    // The owner's scan-039 is why. Fifty-one seconds of recording, 184,454
    // points, a green Stop button and a live point counter ticking up the whole
    // time — and not one ARCore pose. Every one of those returns was resolved
    // against nothing. The operator walked a flat for a minute with no way to
    // know, and found out when the exported bundle turned out to have no
    // `poses_ar.bin` in it.
    //
    // Lidar bytes flowing WITH no poses is the exact and only signature worth
    // interrupting a walk for, so the watchdog requires both: it never fires on
    // a scan that is receiving nothing (that is the no-data banner's job and
    // its instruction is different) and it never fires before there is anything
    // to compare.
    private val _noPoseAlert = MutableStateFlow<String?>(null)
    val noPoseAlert: StateFlow<String?> = _noPoseAlert.asStateFlow()

    private var noPoseWatchJob: kotlinx.coroutines.Job? = null
    private var noPoseLogged = false

    private fun startPoseWatchdog() {
        noPoseWatchJob?.cancel()
        noPoseLogged = false
        _noPoseAlert.value = null
        val controller = arController ?: return
        val armedAt = clock()
        noPoseWatchJob = viewModelScope.launch {
            while (true) {
                kotlinx.coroutines.delay(NO_POSE_TICK_MS)
                if (engineBridge.captureState.value != CaptureState.RECORDING) {
                    _noPoseAlert.value = null
                    continue
                }
                // Nothing is arriving at all: that is the no-data banner's
                // diagnosis, not this one's, and two red banners telling the
                // operator to fix two different things is worse than one.
                if (_stats.value.pointsCaptured <= 0L) {
                    _noPoseAlert.value = null
                    continue
                }
                val last = controller.lastAcceptedPoseAtMillis
                val quietMs =
                    if (last == 0L) clock() - armedAt
                    else android.os.SystemClock.elapsedRealtime() - last
                if (quietMs < NO_POSE_GRACE_MS) {
                    _noPoseAlert.value = null
                    continue
                }
                val total = controller.acceptedPoseCount
                if (!noPoseLogged) {
                    noPoseLogged = true
                    logEvent(
                        LOG_TAG_AR,
                        "NO POSES after ${quietMs}ms of recording with points arriving " +
                            "(posesAccepted=$total points=${_stats.value.pointsCaptured}) — " +
                            "this scan is 2D so far",
                    )
                }
                _noPoseAlert.value =
                    if (total <= 0L) {
                        "NO POSITION TRACKING — the sensor is recording but the camera has never " +
                            "said where it is. This scan will be 2D: no room, no floor plan, " +
                            "nothing to process. Stop, reopen the app, and start again."
                    } else {
                        "POSITION TRACKING LOST — ${quietMs / 1000}s with no camera position " +
                            "while the sensor keeps recording. Everything from here is 2D until " +
                            "it comes back: point the rear camera at something lit and detailed."
                    }
                // NO NEW BUZZ, and that is a decision rather than an
                // omission. The haptic for this condition already exists and
                // already fired: `TRACKING_DEGRADED` is scheduled from
                // `status.tracking == false`, which is exactly true in this
                // case, and the owner's scan-039 log carries thirteen of them
                // over its 51 seconds. The buzz was never the missing half —
                // the operator felt it and had no way to know it meant "this
                // scan has no room in it" rather than "the corner is dim".
                // Adding a second cue on the same condition would also spend
                // the ROUND 13 cue budget twice, and ROUND 13 measured a cue
                // buzz causing the very break the next cue reported.
            }
        }
    }

    private fun stopPoseWatchdog() {
        noPoseWatchJob?.cancel()
        noPoseWatchJob = null
        _noPoseAlert.value = null
    }

    fun dismissNoPoseAlert() {
        _noPoseAlert.value = null
    }

    private fun stopNoDataWatchdog() {
        noDataWatchJob?.cancel()
        noDataWatchJob = null
        _noDataAlert.value = null
        recordingStartedAtMillis = 0L
    }

    fun dismissNoDataAlert() {
        _noDataAlert.value = null
    }

    // --- ROUND 6 (owner item 23): the one-tap mount re-zero -------------------

    /**
     * The session's mount trim: how the D6 was actually sitting on the phone
     * for THIS scan, composed on top of `BracketNominals.cadNominal(COIN_D6)`.
     * Null until the operator taps "Set mount reference".
     */
    private val _storedMountTrim = MutableStateFlow<com.lidarscan.core.calib.StoredMountTrim?>(null)

    /**
     * ROUND 7 (field bug 1): the trim in force, whatever screen it was taken on.
     *
     * Backed by [_storedMountTrim], which is loaded from DataStore in `init` and
     * written on every capture/clear — so the trim now survives the Capture
     * tab's own ViewModel, a navigation to Projects and back, a process death
     * and a cold start, exactly as the owner expects ("re-zero when the mount
     * shifts, not before every capture").
     */
    val mountTrim: StateFlow<com.lidarscan.core.calib.MountTrim?> =
        _storedMountTrim.map { it?.trim }.stateIn(viewModelScope, SharingStarted.Eagerly, null)

    /**
     * ROUND 7: the trim's own sentence — magnitude, age and where it came from —
     * recomputed on a slow tick so "just now" becomes "3 min ago" without the
     * operator touching anything.
     */
    private val _mountTrimProvenance = MutableStateFlow(
        com.lidarscan.core.calib.MountTrimProvenances.describe(null, appRunId, clock()),
    )
    val mountTrimProvenance: StateFlow<com.lidarscan.core.calib.MountTrimProvenance> =
        _mountTrimProvenance.asStateFlow()

    private fun refreshMountTrimProvenance() {
        _mountTrimProvenance.value = com.lidarscan.core.calib.MountTrimProvenances.describe(
            stored = _storedMountTrim.value,
            currentAppRunId = appRunId,
            nowMillis = clock(),
        )
    }

    /** The last re-zero's outcome as one sentence — the confirmation or the refusal. */
    private val _mountTrimNote = MutableStateFlow<String?>(null)
    val mountTrimNote: StateFlow<String?> = _mountTrimNote.asStateFlow()

    /**
     * ROUND 8 (owner item 30b): true when [mountTrimNote] is a **refusal**.
     *
     * The capture screen shows a refusal as a loud amber banner and a
     * confirmation as a quiet teal line. That distinction did not exist before,
     * and the cost of not having it is in the owner's own log: eight taps in one
     * session, eight grey `mount re-zero refused: MOVING` lines, and a field
     * report that the control does not work.
     */
    private val _mountTrimNoteIsWarning = MutableStateFlow(false)
    val mountTrimNoteIsWarning: StateFlow<Boolean> = _mountTrimNoteIsWarning.asStateFlow()

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
    // ── ROUND 11 (owner item 45): the guided hold-still re-zero ──────────
    //
    // Same gate underneath — MountTrimSampler, unchanged, over the same 1 s
    // window — with two things added around it: the operator can SEE the gate's
    // verdict while they hold rather than after they tap, and the trim is
    // averaged over the whole hold instead of over one second.
    //
    // Why that matters is item 45c, and this round measured it: scan-020's trim
    // was accepted at spreadP90 = 2.40 deg, the gate's own 2.5 deg ceiling, and
    // `engine/tests/test_round11_mount_trim.cpp` shows a 2.4 deg trim error
    // painting an overhead feature 13.1 cm apart between the outbound and return
    // legs of an out-and-back walk (23.6 cm at 3 m), against 3.7 cm at 0.8 deg.
    // It reverses with the walk direction, which is exactly the owner's "when i
    // turn around the scan position shifted".

    private val refiner = com.lidarscan.core.calib.MountTrimRefiner()

    /** ROUND 12: the tracking-stable start gate. See [com.lidarscan.core.capture.TrackingWarmup]. */
    private val trackingWarmup = com.lidarscan.core.capture.TrackingWarmup()

    private val _startWarmup =
        MutableStateFlow<com.lidarscan.core.capture.TrackingWarmup.Verdict?>(null)

    /** Non-null only while Start is waiting for the tracker to settle. */
    /** ROUND 13 (owner item 47): what happened to the interruption filter. */
    private val _dndState =
        MutableStateFlow(com.lidarscan.core.capture.DndState.DISABLED)
    val dndState: StateFlow<com.lidarscan.core.capture.DndState> = _dndState.asStateFlow()

    private val _dndNote = MutableStateFlow<String?>(null)
    val dndNote: StateFlow<String?> = _dndNote.asStateFlow()

    /**
     * ROUND 14 (owner item 52) — called by the Capture screen when it learns
     * the grant state, including after the operator comes back from the system
     * settings screen. Before this, the only thing that could ever write the
     * note was a Start, so the screen was silent until the first capture and
     * the note never went away when the permission was finally granted.
     */
    fun refreshDndNote(enabled: Boolean, granted: Boolean) {
        _dndNote.value = when {
            !enabled -> null
            granted -> null
            else -> com.lidarscan.core.capture.CaptureFocus.note(
                com.lidarscan.core.capture.DndState.NO_PERMISSION,
            )
        }
    }

    val startWarmup: StateFlow<com.lidarscan.core.capture.TrackingWarmup.Verdict?> =
        _startWarmup.asStateFlow()

    /** Non-null while the operator is holding the ring; drives the ring's sweep and label. */
    private val _mountHold = MutableStateFlow<com.lidarscan.core.calib.MountTrimRefiner.Progress?>(null)
    val mountHold: StateFlow<com.lidarscan.core.calib.MountTrimRefiner.Progress?> = _mountHold.asStateFlow()

    private var mountHoldJob: kotlinx.coroutines.Job? = null

    /** ROUND 12: the tracking-stable start gate's wait loop, and its re-entry latch. */
    private var startGateJob: kotlinx.coroutines.Job? = null
    private var startPending = false

    /**
     * ROUND 17 item 64 — **Start is one press, however many times it is
     * pressed.**
     *
     * The owner's scan-045 recorded two `[session] start` lines for one project
     * six seconds apart, and the second one is the whole failure. There was no
     * guard of any kind: `startCapture()` never looked at [captureState], the
     * button was never disabled, and the ROUND 12 warmup gate takes four to
     * eight seconds during which — because `_startWarmup` was written and
     * rendered nowhere — **nothing on the screen changed at all**. An operator
     * who presses a button that does not respond presses it again. That is not
     * operator error; it is a button that lied.
     *
     * The second press then fell straight through both `if (!startPending)`
     * blocks and started the capture, leaving the FIRST press's gate job alive
     * (it is cancelled only when a new gate opens, which never happened). Four
     * seconds later that orphan re-entered `startCapture()` mid-recording and,
     * before the engine finally refused it with `invalid state`, had already
     * run `resetPoseCounters()`, `resetWorldFrame()` — destroying and rebuilding
     * the live ARCore session in the middle of the walk — and
     * `trailRecorder.clear()`. That last one is `pathM=0.0`, and a zero path
     * makes the grader think the operator was standing still on purpose, which
     * is how a wrecked capture came to be labelled GOOD.
     *
     * So: one atomic, claimed by the first press and held for the whole
     * sequence INCLUDING the gate's wait, released on every exit. Combined
     * with [starting] (which the button now renders), the operator both cannot
     * and no longer needs to press twice.
     */
    private val startInFlight = java.util.concurrent.atomic.AtomicBoolean(false)

    /**
     * ROUND 17 item 64 — whether `scan_engine_start` said yes for the capture
     * that is being sealed. Reset at the top of every Start attempt so a
     * previous scan's answer can never be read as this one's. See
     * [com.lidarscan.core.capture.ScanSummary.engineStarted].
     */
    private val _engineStarted = MutableStateFlow<Boolean?>(null)

    private val _starting = MutableStateFlow(false)

    /**
     * ROUND 17 item 64 — true from the moment Start is pressed until the
     * capture is recording or the attempt has failed. The Capture screen
     * disables the transport button on it and shows the warmup verdict, so the
     * four-to-eight second gate is a state the operator can SEE.
     */
    val starting: StateFlow<Boolean> = _starting.asStateFlow()

    /**
     * ROUND 17 item 64 — how many points reached the WORLD, read off the file.
     *
     * `streams/map.bin` is `StreamId::kSlamMap`: the resolved, world-frame
     * cloud, written only while the pushbroom is actually resolving. Its
     * chunked framing carries a 32-byte stream header and 16 bytes of overhead
     * per chunk around 16-byte `PointVertex` records, so this is an estimate of
     * the count and NOT an exact one — which is fine, because the only question
     * being asked of it is "is this zero".
     *
     * Returns `null` when the directory is unknown, because "I could not look"
     * must never be graded as "there was nothing there" (the same rule
     * `posesRecorded` follows).
     */
    private fun resolvedWorldPoints(dir: java.io.File?): Long? {
        if (dir == null) return null
        val f = java.io.File(java.io.File(dir, "streams"), "map.bin")
        return runCatching {
            if (!f.isFile) 0L else ((f.length() - 32L).coerceAtLeast(0L) / 16L)
        }.getOrNull()
    }

    /** ROUND 17 item 64: released on every exit from the start sequence. */
    private fun releaseStart() {
        startPending = false
        _starting.value = false
        _startWarmup.value = null
        startInFlight.set(false)
    }

    /**
     * Start (or restart) a hold. Called on press; [cancelMountHold] on release.
     *
     * The hold ANCHOR moves forward whenever the live gate fails, which is the
     * whole UX: wobble and the ring empties and starts again from now, so the
     * operator learns what "still" means with their hands instead of by reading
     * a refusal about a moment that has already passed.
     */
    fun beginMountHold(auto: Boolean = false) {
        val controller = arController
        if (controller == null) {
            _mountTrimNote.value = "Mount reference needs phone tracking, which this session does not have."
            _mountTrimNoteIsWarning.value = true
            return
        }
        mountHoldJob?.cancel()
        _mountTrimNote.value = null
        _mountTrimNoteIsWarning.value = false
        mountHoldJob = viewModelScope.launch {
            var anchorNs = controller.poseWindow().lastOrNull()?.tMonoNs ?: 0L
            // A hold the operator started and then walked away from must not
            // poll for the life of the ViewModel. `done` requires the gate to
            // pass, so a rig in motion never completes on its own; this gives
            // up after 30 s and says so.
            val giveUpAt = clock() + MOUNT_HOLD_GIVE_UP_MS
            while (true) {
                if (clock() > giveUpAt) {
                    logEvent(LOG_TAG_AR, "mount hold abandoned after ${MOUNT_HOLD_GIVE_UP_MS} ms of movement")
                    _mountHold.value = null
                    mountHoldJob = null
                    _mountTrimNote.value =
                        "Could not get a still enough hold. Brace the phone against your body and try again."
                    _mountTrimNoteIsWarning.value = true
                    return@launch
                }
                val window = controller.poseWindow()
                val newest = window.lastOrNull()?.tMonoNs
                if (newest != null && anchorNs == 0L) anchorNs = newest
                val progress = refiner.evaluate(window, anchorNs)
                // A hold is only a hold while the gate agrees. The instant it
                // does not, the anchor jumps to now and the ring empties.
                if (!progress.gatePasses && progress.holdMillis > 250L && newest != null) {
                    anchorNs = newest
                }
                _mountHold.value = progress
                if (progress.done) {
                    finishMountHold(anchorNs, auto)
                    return@launch
                }
                kotlinx.coroutines.delay(MOUNT_HOLD_TICK_MS)
            }
        }
    }

    /** Released early, or navigated away. Nothing is stored. */
    fun cancelMountHold() {
        mountHoldJob?.cancel()
        mountHoldJob = null
        val progress = _mountHold.value
        _mountHold.value = null
        if (progress != null && !progress.done) {
            logEvent(LOG_TAG_AR, "mount hold released early: ${progress.logSuffix}")
        }
    }

    private fun finishMountHold(anchorNs: Long, auto: Boolean) {
        val controller = arController ?: return
        val progress = _mountHold.value
        _mountHold.value = null
        mountHoldJob = null
        val result = refiner.capture(
            samples = controller.poseWindow(),
            holdStartedAtMonoNs = anchorNs,
            nowMillis = clock(),
            sensor = _sensor.value,
        )
        logEvent(
            LOG_TAG_AR,
            "mount hold ${if (auto) "(auto) " else ""}finished: ${progress?.logSuffix ?: "no progress"}",
        )
        applyMountTrimResult(result)
    }

    /**
     * The one-tap path, kept exactly as ROUND 8 left it so the old behaviour and
     * its tests survive: a single 1 s window, refused with its numbers.
     */
    fun setMountReference() {
        val controller = arController
        if (controller == null) {
            _mountTrimNote.value = "Mount reference needs phone tracking, which this session does not have."
            _mountTrimNoteIsWarning.value = true
            return
        }
        applyMountTrimResult(
            com.lidarscan.core.calib.MountTrimSampler.capture(
                samples = controller.poseWindow(),
                nowMillis = clock(),
                sensor = _sensor.value,
            ),
        )
    }

    /** The shared tail of the one-tap and hold-still paths. */
    private fun applyMountTrimResult(result: com.lidarscan.core.calib.MountTrimResult) {
        when (result) {
            is com.lidarscan.core.calib.MountTrimResult.Rejected -> {
                // ROUND 8 (owner item 30b): the refusal carries its MEASUREMENT,
                // to the screen and to the log.
                //
                // The entire on-device record of the owner's failed session was
                // eight lines of `mount re-zero refused: MOVING` — a name, with
                // no numbers, which cannot distinguish "you were 0.1° over" from
                // "the gate is broken" from "no samples are arriving". All three
                // were live hypotheses at the start of this round. The log line
                // is now, e.g.:
                //
                //   mount re-zero refused: MOVING p90=2.90deg max=5.10deg
                //   limit=2.50deg outlierLimit=6.00deg samples=31 spanMs=980
                //
                // and the panel gets the same numbers plus what to do about them
                // (`MountTrimResult.Rejected.sentence`).
                _mountTrimNote.value = result.sentence
                _mountTrimNoteIsWarning.value = true
                logEvent(
                    LOG_TAG_AR,
                    "mount re-zero refused: ${result.reason.name}" +
                        (result.measurement?.let { " ${it.logSuffix}" } ?: " (no samples)"),
                )
            }
            is com.lidarscan.core.calib.MountTrimResult.Captured -> {
                _mountTrimNoteIsWarning.value = false
                val stored = com.lidarscan.core.calib.StoredMountTrim(result.trim, appRunId)
                _storedMountTrim.value = stored
                refreshMountTrimProvenance()
                // ROUND 7: to disk, immediately. The 0.3.0 version wrote the
                // trim only into the manifest of a project that was already
                // recording — which on the Capture tab, where a re-zero is
                // taken BEFORE Start, is no project at all.
                viewModelScope.launch { runCatching { persistMountTrim(stored) } }
                _mountTrimNote.value =
                    ("Mount reference set — %.1f° from the bracket nominal, steady to %.2f° " +
                        "(worst frame %.2f°) over %d samples.")
                        .format(
                            result.trim.magnitudeDeg,
                            result.trim.spreadP90Deg,
                            result.trim.spreadDeg,
                            result.trim.sampleCount,
                        )
                // `spread=` keeps meaning the WORST deviation, unchanged since
                // ROUND 6, so the 0.3.0 field lines (`spread=0.47deg`) and these
                // remain directly comparable. `spreadP90=` is the new number the
                // ROUND 8 gate actually judges on.
                logEvent(
                    LOG_TAG_AR,
                    "mount re-zero captured: magnitude=%.2fdeg spread=%.2fdeg spreadP90=%.2fdeg samples=%d accuracyDeg=%s"
                        .format(
                            result.trim.magnitudeDeg,
                            result.trim.spreadDeg,
                            result.trim.spreadP90Deg,
                            result.trim.sampleCount,
                            result.trim.accuracyDeg?.let { "%.2f".format(it) } ?: "unmeasured",
                        ),
                )
                // ROUND 18 item 72 — the acceptance bar and the refine goal,
                // reconciled. The movement gate (p90 <= 2.50 deg) answers "was
                // the rig still enough to AVERAGE" and it was right to accept
                // the owner's 03:15:59 re-zero at spreadP90 2.24. The round-10
                // refine goal (0.8 deg) is a different claim — the split-half
                // accuracy of the resulting MEAN — and that capture measured
                // 1.35 deg, which round 11 puts at ~16 cm of paint error at
                // 3 m per 1.4 deg. Both bars did their jobs; what was missing
                // is this sentence, at the moment of acceptance, instead of a
                // silent seal-time trimAccuracyDeg nobody is looking at yet.
                if (result.trim.accuracyIsPoor) {
                    _mountTrimNote.value =
                        ("Mount reference set, but its measured accuracy is %.2f° — past the %.1f° " +
                            "goal. It will be used; a longer, stiller hold (tap Re-zero) would " +
                            "tighten it.")
                            .format(
                                result.trim.accuracyDeg ?: 0.0,
                                com.lidarscan.core.calib.MountTrimRefiner.DEFAULT_TARGET_STABILITY_DEG,
                            )
                    _mountTrimNoteIsWarning.value = true
                }
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
        _storedMountTrim.value = null
        refreshMountTrimProvenance()
        viewModelScope.launch { runCatching { persistMountTrim(null) } }
        _mountTrimNote.value = "Mount reference cleared — back to the bracket's CAD nominal."
        _mountTrimNoteIsWarning.value = false
        applyMountExtrinsicNow()
    }

    fun dismissMountTrimNote() {
        _mountTrimNote.value = null
        _mountTrimNoteIsWarning.value = false
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
        _keyframesEnabled.value =
            tuning.keyframesEnabled && com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED
        // ROUND 17 item 67: the already-gated flow, not the raw preset value —
        // one source of truth. See KeyframeRecorder.setEnabled.
        keyframeRecorder?.setEnabled(_keyframesEnabled.value)
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

    // ── ROUND 8: feeding the third-person Follow camera ──────────────────────

    /** Held so the exact same function reference can be removed again — see [detachKeyframeListener]. */
    private var rigPoseFrameListener: ((com.google.ar.core.Frame) -> Unit)? = null

    /**
     * ROUND 8, owner directive *"i need a live 3d mapping too"* — **hands the
     * renderer the rig's position, once per ARCore frame.**
     *
     * The FOLLOW camera lives in `PointCloudRenderer` / `core.render.FollowCamera`
     * (a concurrent task owns those); what only this class can supply is the
     * pose, because `CaptureArController`'s frame stream is a ViewModel-scoped
     * subscription and the renderer is a Compose-scoped object. This is the
     * seam between them, and it is a sink rather than a direct reference so
     * that nothing here has to know what the renderer does with it.
     *
     * Two deliberate choices, both from the renderer's own contract:
     *
     *  * **`frame.camera.pose`, not `getDisplayOrientedPose()`.** The latter
     *    bakes in the display rotation, which is a property of how the phone is
     *    held rather than of where the rig is.
     *  * **Position only.** The camera derives its heading from the trajectory,
     *    never from phone yaw — a walking operator yaws several degrees per step
     *    and glances around constantly, and either would swing the whole view.
     *
     * Without this the FOLLOW camera still works, on the renderer's documented
     * fallback (the centroid of the newest points, which for a D6's ring is
     * approximately the rig). This makes it exact.
     */
    fun setRigPoseSink(sink: ((Float, Float, Float, Long) -> Unit)?) {
        val controller = arController ?: return
        rigPoseFrameListener?.let { controller.removeFrameListener(it) }
        rigPoseFrameListener = null
        if (sink == null) return
        val listener: (com.google.ar.core.Frame) -> Unit = { frame ->
            // Cannot throw onto the GL thread: this runs inside
            // CaptureArController.onFrame's listener fan-out, and ROUND 6's
            // whole item 19 was about what an exception there costs (the
            // process). `camera.pose` on a frame whose camera is not tracking
            // is defined but meaningless, so it is skipped rather than fed in.
            runCatching {
                val camera = frame.camera
                if (camera.trackingState != com.google.ar.core.TrackingState.TRACKING) return@runCatching
                val t = camera.pose.translation
                sink(t[0], t[1], t[2], frame.timestamp)
            }
        }
        rigPoseFrameListener = listener
        controller.addFrameListener(listener)
    }

    private fun detachRigPoseListener() {
        val listener = rigPoseFrameListener ?: return
        arController?.removeFrameListener(listener)
        rigPoseFrameListener = null
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

        // ROUND 14 (owner item 50). The owner's complaint was that scans go
        // wrong when he stands and sweeps the phone instead of walking it, and
        // NEITHER of the two hints above can fire during one: `excessiveMotion`
        // is ARCore's own flag and `recentlySkipping` is about keyframe
        // coverage, while the measured fault is rotation WITHOUT translation.
        // In scan-034 the median linear speed was 4 cm/s, so the existing
        // "moving too fast" path is structurally incapable of noticing.
        // See ParallaxWatch for the measurement and the threshold.
        val parallax = if (captureState.value == CaptureState.RECORDING) {
            parallaxWatch.measure(arController?.poseWindow().orEmpty())
        } else {
            null
        }
        val starved = parallax?.starved == true

        _motionHint.value = when {
            excessiveMotion -> "Moving too fast — slow the walk so tracking can keep up."
            // Above the keyframe hint: a tracker about to lose the room is a
            // bigger problem than thin colour coverage, and the two would
            // otherwise both be true through the same sweep.
            starved -> com.lidarscan.core.capture.ParallaxWatch.HINT
            recentlySkipping && _keyframesEnabled.value ->
                "Turning too fast for colour frames — sweep more slowly to keep coverage."
            else -> null
        }

        // ROUND 11 (owner item 43): the same two signals the hint is written
        // from, plus the section counter, become something the operator can
        // feel. Deliberately fed from HERE rather than from a fourth ticker:
        // the hint and the cue must never disagree about whether the rig is
        // moving too fast, and one evaluation point is the only way to
        // guarantee that.
        //
        // Cues are only for a RECORDING session. Buzzing at someone who is
        // lining up a preview, or who has put the phone down on a table with
        // the app open, is how a default-ON feature gets turned off.
        val recording = captureState.value == CaptureState.RECORDING
        val conditions = com.lidarscan.core.capture.CueConditions(
            trackingDegraded = status != null && !status.tracking,
            movingTooFast = excessiveMotion || (recentlySkipping && _keyframesEnabled.value),
            // ROUND 15 item 54: only an UNHEALED break can reach the cue. A
            // healed break leaves the live map continuous, so there is nothing
            // for the operator to notice and nothing for them to do — and
            // ROUND 13 measured that the buzz for one break was itself the
            // cause of the next one.
            sectionBreaks = _unhealedSectionBreaks.value,
            turningWithoutMoving = starved,
        )
        val fired = cues.tick(conditions, nowMillis, enabled = recording && cuesArmed)
        if (fired != null) {
            _lastCue.value = fired
            playCue(fired)
            logEvent(LOG_TAG_AR, "cue: ${fired.name.lowercase()}")
        }
    }

    /**
     * ROUND 11: the Settings switch, sampled on the hint ticker rather than
     * collected, so there is exactly one place (the tick) where cue state is
     * read and no chance of a stale collector deciding.
     */
    @Volatile
    private var cuesArmed: Boolean = true

    private var lastStatsSampleMillis = 0L
    private var lastStatsSamplePoints = 0L

    init {
        // ── ROUND 10 (owner item 38): a fresh Capture tab shows a fresh map ──
        //
        // "Entering Capture = a new-scan context" has been the contract since
        // ROUND 9 item 33, and the ONE thing that never obeyed it was the live
        // cloud — because it does not live in this ViewModel. It lives in the
        // process-lifetime `scan_engine*`'s PageStore, which outlives every
        // ViewModel, every session and every navigation. So the tab was reset
        // in every respect except the one filling the screen.
        //
        // Not for a replay (its cloud IS the point of the screen) and not for
        // a project-scoped entry.
        if (!isReplay && projectId == null) {
            viewModelScope.launch { clearLiveViewport() }
        }

        // ROUND 7 (field bug 1): restore the mount re-zero FIRST, before any
        // project loads and long before a Start can reach applyMountExtrinsic.
        //
        // The owner's log has three good re-zeros at 22:53, a 216 k-point scan
        // that used one of them, and then `trim=none` on the very next capture
        // 57 s later — because the trim lived in this ViewModel and this
        // ViewModel is `viewModel(key = "capture-new-false")` on the Capture
        // tab's own NavBackStackEntry. Looking at the scan you just took was
        // enough to throw away 132 degrees of mount rotation, silently.
        viewModelScope.launch {
            val stored = runCatching { loadStoredMountTrim() }.getOrNull()
            if (stored != null) _storedMountTrim.value = stored
            refreshMountTrimProvenance()
            if (stored != null) {
                val p = _mountTrimProvenance.value
                logEvent(
                    LOG_TAG_AR,
                    "mount trim restored: ${p.logSuffix} warn=${p.warn}",
                )
                // A trim carried across an app restart is applied, not
                // discarded — but the operator is told, because a phone that
                // has been in a bag since the last scan may have a mount that
                // has moved. "Re-zero when the mount shifts, not before every
                // capture" is the owner's own rule; this is the exception to it
                // being stated rather than guessed at.
                if (p.fromPreviousRun) _mountTrimNote.value = p.label
            }
        }
        // "just now" has to become "3 min ago" on its own, or the age the panel
        // shows is only ever the age at the moment the screen opened.
        viewModelScope.launch {
            while (true) {
                kotlinx.coroutines.delay(TRIM_AGE_TICK_MS)
                refreshMountTrimProvenance()
            }
        }

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
                // ROUND 11 (item 43): the Settings switch is read on the same
                // tick that decides, so a toggle applies to the capture already
                // running and there is no second source of truth.
                cuesArmed = runCatching { cuesEnabled() }.getOrDefault(true)
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
     * ROUND 10 (owner item 38) — **empty the live viewport, for real.**
     *
     * The owner's words: *"when click capture after capture, it still show
     * with the previous capture."*
     *
     * Two things have to happen and neither is enough alone:
     *
     *  1. **The native pages** — `RealEngineBridge` holds ONE `scan_engine*`
     *     for the whole process and the engine's `PageStore` belongs to the
     *     engine, not to the session. `resetLiveView()` recycles it.
     *  2. **The GPU pages** — `PointCloudRenderer` only clears its uploaded
     *     pages when it is handed a NULL source (`setSource(null)`); handing
     *     it a different non-null source clears nothing. So the flow is
     *     null'd and then re-read, which is what makes the renderer let go of
     *     buffers whose pages no longer exist.
     *
     * Ordering matters: recycle first, then swap the source, or the renderer
     * can re-upload the pages between the two.
     *
     * This never touches the recording. Record-always (Tech Spec §3 key rule
     * 2) writes every point to the `.lscan` as it arrives, and the Projects
     * thumbnail is rendered from a snapshot taken before this runs.
     */
    private suspend fun clearLiveViewport() {
        runCatching { engineBridge.resetLiveView() }
            .onFailure { logEvent(LOG_TAG_SEAL, "live view reset failed: ${it.javaClass.simpleName}") }
        _pointCloudSource.value = null
        _pointCloudSource.value = (engineBridge as? NativePointCloudProvider)?.currentPointCloudSource()
    }

    /**
     * ROUND 11 (owner item 45b) — auto-recapture a stale trim at Start.
     *
     * > "At capture start, if stationary and trim is stale, auto-recapture the
     * >  trim so a stale reference never enters a scan silently."
     *
     * Three conditions, all of them cheap and none of them blocking:
     *
     *  * there is a trim and it is older than
     *    [com.lidarscan.core.calib.MountTrimRefiner.AUTO_REFRESH_AFTER_MS], or
     *    it came from a previous app run (`fromPreviousRun`, ROUND 7's own
     *    provenance flag) — a bracket that has been picked up and put down
     *    between two scans has moved, and the app cannot know it has not;
     *  * ARCore is tracking and the pose window is long enough to judge;
     *  * the ROUND 8 gate passes RIGHT NOW, i.e. the rig is genuinely still.
     *
     * If the gate refuses, the OLD trim stays and a note says so. Refusing to
     * start would be worse: the operator is holding a rig, ready to walk, and
     * an old trim is better than a missed capture. What must not happen — and
     * what happened before this — is the old trim being used with no mention
     * of its age anywhere except a log line nobody reads until afterwards.
     *
     * Deliberately NOT the guided hold: Start must be instant. This reads the
     * pose window that is already streaming and decides in microseconds.
     */
    private fun maybeAutoRefreshMountTrim() {
        val controller = arController ?: return
        val provenance = _mountTrimProvenance.value
        val trim = provenance?.trim
        val stale = trim == null ||
            provenance.fromPreviousRun ||
            trim.ageMillis(clock()) > com.lidarscan.core.calib.MountTrimRefiner.AUTO_REFRESH_AFTER_MS
        if (!stale) return
        if (trim == null) {
            // Nothing to refresh, and nothing to be quiet about either: with no
            // trim at all the app is on the bare CAD nominal, which the chip
            // already says in amber.
            return
        }
        val result = com.lidarscan.core.calib.MountTrimSampler.capture(
            samples = controller.poseWindow(),
            nowMillis = clock(),
            sensor = _sensor.value,
        )
        if (result is com.lidarscan.core.calib.MountTrimResult.Captured) {
            // ── ROUND 12: an auto-refresh may never make the trim WORSE. ─────
            //
            // As shipped in 0.7.0 this was unconditional: any one-second sample
            // that cleared the gate replaced the incumbent, so a trim measured
            // over a guided six-second hold could be overwritten at Start by a
            // 2.4 deg one taken from whatever second the operator happened to be
            // in. And because `fromPreviousRun` alone marks a trim stale — and
            // the app process restarts far more often than ten minutes — this
            // path runs constantly, not rarely.
            //
            // Comparison is on MountTrim.qualityRank (split-half accuracy where
            // it exists), never on spreadP90 alone: two p90s measured over
            // different hold lengths are not comparable, which is the whole of
            // ROUND 12's scan-028 finding.
            if (result.trim.qualityRank > trim.qualityRank) {
                // ROUND 18 item 72 — this line used to print the raw ranks,
                // and the rank of an UNMEASURED trim is `100 + spreadP90` (the
                // UNMEASURED_RANK_BASE penalty that makes "measured beats
                // unmeasured" sortable). The owner's 03:15 field log therefore
                // read "kept rank=0.78, candidate rank=100.55" — a penalty
                // constant leaking into a log line as if it were a
                // measurement, describing a candidate whose one-second window
                // (31 samples) simply cannot be split-half checked, under a
                // headline calling it "worse". It was not worse; it was
                // unverifiable, and the incumbent's 0.78 deg measured accuracy
                // rightly outranks an unverifiable sample. Say that.
                val kept = trim.accuracyDeg
                val cand = result.trim.accuracyDeg
                val why = when {
                    cand == null && kept != null ->
                        "the incumbent's accuracy is MEASURED (split-half %.2f deg) and a one-second start sample cannot be split-half checked — an unverifiable sample never replaces a verified one".format(kept)
                    cand != null && kept != null ->
                        "both are measured and the incumbent is better (%.2f deg vs %.2f deg split-half)".format(kept, cand)
                    else ->
                        "neither is measured and the incumbent's frame jitter is smaller (p90 %.2f deg vs %.2f deg)".format(trim.spreadP90Deg, result.trim.spreadP90Deg)
                }
                logEvent(
                    LOG_TAG_AR,
                    ("mount trim auto-refresh declined: kept %s — " + why +
                        ". candidateSpreadP90=%.2fdeg candidateSamples=%d")
                        .format(provenance.logSuffix, result.trim.spreadP90Deg, result.trim.sampleCount),
                )
                _mountTrimNote.value =
                    if (result.trim.accuracyDeg == null && trim.accuracyDeg != null) {
                        "Mount reference is ${provenance.ageLabel}. A fresh one-second sample was taken " +
                            "but its accuracy cannot be verified from so short a hold, so the measured " +
                            "one already set was kept — tap Re-zero and hold still for a few seconds " +
                            "if the bracket has moved."
                    } else {
                        "Mount reference is ${provenance.ageLabel}. A fresh sample was taken and was less " +
                            "steady than the one already set, so the better one was kept — tap Re-zero and " +
                            "hold still if the bracket has moved."
                    }
                _mountTrimNoteIsWarning.value = true
                return
            }
            logEvent(
                LOG_TAG_AR,
                ("mount trim auto-refreshed at start: was %s, now magnitude=%.2fdeg " +
                    "spreadP90=%.2fdeg stability=%.2fdeg samples=%d")
                    .format(
                        provenance.logSuffix,
                        result.trim.magnitudeDeg,
                        result.trim.spreadP90Deg,
                        result.trim.stabilityDeg,
                        result.trim.sampleCount,
                    ),
            )
            applyMountTrimResult(result)
            _mountTrimNote.value =
                "Mount reference was stale (%s) and the rig was still, so it was re-taken automatically — %.1f deg, steady to %.2f deg."
                    .format(provenance.ageLabel, result.trim.magnitudeDeg, result.trim.spreadP90Deg)
            _mountTrimNoteIsWarning.value = false
        } else {
            val measurement = (result as? com.lidarscan.core.calib.MountTrimResult.Rejected)?.measurement
            logEvent(
                LOG_TAG_AR,
                "mount trim auto-refresh declined (rig not still): ${provenance.logSuffix}" +
                    (measurement?.let { " ${it.logSuffix}" } ?: ""),
            )
            _mountTrimNote.value =
                "Scanning with a mount reference from ${provenance.ageLabel} — hold the rig still and " +
                    "tap Re-zero if the bracket has moved since."
            _mountTrimNoteIsWarning.value = true
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
        // ── ROUND 17 item 64: one press, one start. ─────────────────────────
        //
        // Claimed by the first press and held across the ROUND 12 gate's whole
        // wait, so the gate's own re-entry (which arrives with `startPending`
        // set) walks straight through instead of trying to take it again. See
        // [startInFlight] for scan-045.
        if (!startPending) {
            if (!startInFlight.compareAndSet(false, true)) {
                logEvent(
                    LOG_TAG_SESSION,
                    "start IGNORED: a start is already in flight for this screen — " +
                        "the tracking gate can take up to " +
                        "${2 * com.lidarscan.core.capture.TrackingWarmup.MAX_WAIT_MILLIS}ms",
                )
                return
            }
            val already = captureState.value
            if (already == com.lidarscan.core.engine.CaptureState.RECORDING ||
                already == com.lidarscan.core.engine.CaptureState.PAUSED
            ) {
                logEvent(LOG_TAG_SESSION, "start IGNORED: already $already")
                releaseStart()
                return
            }
            _starting.value = true
            _engineStarted.value = null
            // A gate job from an earlier press has no business surviving into
            // this one. It cannot happen while the atomic holds, and it is
            // cancelled here anyway: the failure it caused was silent and cost
            // the owner a capture.
            startGateJob?.cancel()
            startGateJob = null
        }

        // ── ROUND 14: every capture gets its own world. ─────────────────────
        //
        // The owner asked whether the origin zeroes at the start of each
        // capture. It did not. One ARCore Session was created per PROCESS and
        // only paused between scans, so scan N+1 opened in scan N's origin,
        // holding scan N's feature map. In the owner's 0.8.0 session that is
        // measurable: scan-035's first tracked pose is 7 cm from scan-034's
        // last, 49 s and a Stop/Start apart — and 22.5 s later the pose stream
        // jumped 1.631 m / 162.57° in 33 ms while the recorded gyro integrated
        // 1.56°. A 162° snap is a relocalisation onto a previously-mapped place,
        // and it needs a previous map to relocalise onto.
        //
        // This runs BEFORE the ROUND 12 warmup gate on purpose: the gate's job
        // is to refuse to start on a tracker that has not settled, and after
        // this line the tracker it should be judging is the new one. Resetting
        // after the gate would hand its guarantee to a session that no longer
        // exists. It also clears `motion` and the ArStatus counters, which is
        // why `trackingLossEpisodes` stops accumulating across captures (the
        // owner's log shows drops=1, 2, 3 on three consecutive scans — one
        // real drop, counted three times).
        //
        // Never blocks Start: a rig with no ARCore at all returns false here
        // and carries on to the same code it always ran.
        // ROUND 16 item 58: the reset now holds the session lock across the
        // rebuild (see CaptureArController.driveLock — the race that produced
        // scan-039), retries once, and REPORTS. `yielded` is how many render
        // frames the lock actually turned away, which is the evidence that the
        // mutual exclusion ran at all rather than being a claim in a comment.
        if (!startPending) {
            arController?.resetPoseCounters()
            arController?.let { controller ->
                val reset = controller.resetWorldFrame(
                    attempts = com.lidarscan.app.ar.CaptureArController.RESET_ATTEMPTS,
                )
                if (reset.ok) {
                    logEvent(
                        LOG_TAG_AR,
                        "world frame reset: new ARCore session for this capture " +
                            "(origin, feature map and anchors from the previous scan discarded) " +
                            "tries=${reset.attempts} framesYielded=${reset.yieldedFrames}",
                    )
                } else if (reset.attempts > 0) {
                    logEvent(
                        LOG_TAG_AR,
                        "world frame reset FAILED after ${reset.attempts} attempts — carrying on " +
                            "with no AR session; this scan will have no positions unless " +
                            "tracking recovers",
                    )
                }
            }
        }

        // ── ROUND 12: do not start on a tracker that has not settled. ───────
        //
        // The owner's scan-025 took a 2.015 m step 33 ms wide, 6.9 seconds
        // after Start, and scan-028 took a 0.30 m one 3.7 s in — ARCore
        // re-anchoring while VIO was still converging, with `tracking` reading
        // TRACKING and pose quality GOOD across both. Everything recorded
        // before such a jump is in a different world frame from everything
        // after it.
        //
        // This waits (it never refuses — see TrackingWarmup's header) and then
        // re-enters startCapture(). `startPending` makes the re-entry a plain
        // fall-through so the gate can never loop.
        if (!startPending) {
            val verdict = trackingWarmup.evaluate(arController?.poseWindow().orEmpty())
            if (!verdict.ready && arController != null) {
                logEvent(LOG_TAG_AR, "start gate: waiting for tracking — ${verdict.logSuffix}")
                _startWarmup.value = verdict
                startPending = true
                startGateJob?.cancel()
                startGateJob = viewModelScope.launch {
                    val giveUpAt =
                        clock() + com.lidarscan.core.capture.TrackingWarmup.MAX_WAIT_MILLIS
                    while (clock() < giveUpAt) {
                        val v = trackingWarmup.evaluate(arController?.poseWindow().orEmpty())
                        _startWarmup.value = v
                        if (v.ready) break
                        kotlinx.coroutines.delay(MOUNT_HOLD_TICK_MS)
                    }
                    val finalVerdict =
                        trackingWarmup.evaluate(arController?.poseWindow().orEmpty())
                    logEvent(
                        LOG_TAG_AR,
                        "start gate: ${if (finalVerdict.ready) "cleared" else "timed out"} — " +
                            finalVerdict.logSuffix,
                    )
                    // ── ROUND 16 item 58: NO_POSES is not "not settled yet". ──
                    //
                    // The other three blockers mean ARCore is running and the
                    // pose stream is not good enough yet, which is what waiting
                    // is for. NO_POSES means ARCore delivered NOTHING in four
                    // seconds — and that is the exact signature of the session
                    // the world-frame reset race leaves behind: created,
                    // resumed, reporting healthy, and never handed a camera
                    // frame. The owner's log shows it twice in one session
                    // (scan-037 and scan-039), and both times the app started
                    // recording anyway and produced a scan with no trajectory.
                    //
                    // So this blocker gets ONE rebuild and ONE more wait before
                    // the capture is allowed to begin. It still never refuses —
                    // ROUND 12's rule stands, and an app that will not start is
                    // worse than a warned one — but it no longer walks past its
                    // own diagnosis without acting on it.
                    var verdictNow = finalVerdict
                    if (!verdictNow.ready &&
                        verdictNow.blocker == com.lidarscan.core.capture.TrackingWarmup.Blocker.NO_POSES
                    ) {
                        val again = arController?.resetWorldFrame(
                            attempts = com.lidarscan.app.ar.CaptureArController.RESET_ATTEMPTS,
                        )
                        logEvent(
                            LOG_TAG_AR,
                            "start gate: NO_POSES after " +
                                "${com.lidarscan.core.capture.TrackingWarmup.MAX_WAIT_MILLIS}ms — " +
                                "the tracking session delivered no frames at all; rebuilding it " +
                                "once more (rebuilt=${again?.ok == true} tries=${again?.attempts ?: 0})",
                        )
                        arController?.resetPoseCounters()
                        val secondDeadline =
                            clock() + com.lidarscan.core.capture.TrackingWarmup.MAX_WAIT_MILLIS
                        while (clock() < secondDeadline) {
                            val v = trackingWarmup.evaluate(arController?.poseWindow().orEmpty())
                            _startWarmup.value = v
                            if (v.ready) break
                            kotlinx.coroutines.delay(MOUNT_HOLD_TICK_MS)
                        }
                        verdictNow = trackingWarmup.evaluate(arController?.poseWindow().orEmpty())
                        logEvent(
                            LOG_TAG_AR,
                            "start gate (after rebuild): " +
                                "${if (verdictNow.ready) "cleared" else "still blocked"} — " +
                                verdictNow.logSuffix,
                        )
                    }
                    if (!verdictNow.ready) {
                        _mountTrimNote.value = if (
                            verdictNow.blocker == com.lidarscan.core.capture.TrackingWarmup.Blocker.NO_POSES
                        ) {
                            "NO POSITION TRACKING — the camera has sent nothing at all, twice. " +
                                "Starting anyway, but this scan will be 2D: the returns will have " +
                                "no positions and there will be no room in the file. Stop, close " +
                                "the app and reopen it, then try again."
                        } else {
                            "Tracking had not settled after " +
                                "${com.lidarscan.core.capture.TrackingWarmup.MAX_WAIT_MILLIS / 1000}s — " +
                                "starting anyway. The first seconds of this scan may be in a " +
                                "different frame; more light and more texture help."
                        }
                        _mountTrimNoteIsWarning.value = true
                    }
                    _startWarmup.value = null
                    startCapture()
                }
                return
            }
            _startWarmup.value = null
        }
        // The gate is behind us; the atomic stays claimed until the sequence
        // resolves (ROUND 17 item 64), but `startPending` has done its job.
        startPending = false

        // ── ROUND 14 (owner item 53): do not record zero bytes. ─────────────
        //
        // The owner's scan-031 and scan-032 are two sealed Mid-360 scans of
        // nothing, each followed 2 s later by `NO DATA … bytesIn=0` and a seal
        // message telling him to re-seat a cable that was fine. A Mid-360
        // `connect()` is `Engine::add_device`, which does no I/O at all, so
        // CONNECTED was never evidence of a link and the first honest moment
        // came after the recording had started.
        //
        // This is the check that CAN be made beforehand, and it is the one that
        // most likely explains those two scans: the lidar unicasts to a host IP
        // it has persisted, and if the phone's Ethernet interface does not hold
        // that address the packets go to a machine that is not on the cable.
        // See `Mid360Preflight` for why this cannot be fixed in software and
        // has to be handed to the operator as a value to type.
        mid360Preflight()?.let { verdict ->
            logEvent(
                LOG_TAG_NET,
                "mid360 preflight: ${verdict.logToken} — ${verdict.summary}",
            )
            if (verdict.blocking) {
                _saveError.value = verdict.summary + (verdict.fix?.let { "\n\n$it" } ?: "")
                releaseStart()  // ROUND 17 item 64
                return
            }
        }

        // ROUND 11 (owner item 45b): a stale mount reference must never enter a
        // scan silently.
        maybeAutoRefreshMountTrim()
        lastStatsSampleMillis = 0L
        lastStatsSamplePoints = 0L
        _stats.value = CaptureStats()
        _sessionSummary.value = null
        _scanSummary.value = null
        // ROUND 5.3: one walk, one trail — the preview's framing path is not part
        // of the recorded walkthrough.
        trailRecorder.clear()
        // ROUND 6 (item 20): a new attempt clears the previous one's verdict.
        _saveError.value = null
        _lastSavedProject.value = null
        _liveMapFullNote.value = null
        viewModelScope.launch {
            // ROUND 9 (item 33): remember whether THIS Start is what put the
            // project on disk. Only a project this call created may be rolled
            // back below — a reopened/deep-linked one existed before the button
            // was pressed and is not ours to delete.
            val reopened = (_uiState.value as? CaptureUiState.Loaded)?.project
            val project = reopened
                ?: createProjectForThisScan()
                // ROUND 17 item 64: a start that never made a project is a
                // start that ended, and the button must arm again.
                ?: run { releaseStart(); return@launch }
            val createdByThisStart = reopened == null
            // ROUND 17 item 66: opened BEFORE the engine, so a capture whose
            // engine refuses to start still leaves a bundle that says why —
            // which is exactly the scan-045 case.
            beginDebugLog(
                project.directory,
                "capture debug log opened for ${project.id} — app " +
                    "v${com.lidarscan.app.BuildConfig.VERSION_NAME} " +
                    "(build ${com.lidarscan.app.BuildConfig.VERSION_CODE}), " +
                    "sensor=${project.manifest.sensor} profile=${project.manifest.profile} " +
                    "preset=${_preset.value} tier=$deviceTier liveSlam=${_liveSlam.value}",
            )
            // ROUND 13 (owner item 47). BEFORE the engine session, so the
            // filter is already in place for the first frame, and recorded in
            // the same line the field report is read from — an unprotected walk
            // has to be visible afterwards without anyone remembering.
            val dnd = runCatching { engageDnd() }
                .getOrDefault(com.lidarscan.core.capture.DndState.FAILED)
            _dndState.value = dnd
            _dndNote.value = com.lidarscan.core.capture.CaptureFocus.note(dnd)
            logEvent(
                LOG_TAG_SESSION,
                "start: project=${project.id} sensor=${project.manifest.sensor} " +
                    "profile=${project.manifest.profile} preset=${_preset.value} tier=$deviceTier " +
                    "liveSlam=${_liveSlam.value} " +
                    "dnd=${com.lidarscan.core.capture.CaptureFocus.logToken(dnd)} " +
                    "dir=${project.directory.absolutePath}",
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
                //
                // ROUND 9 (owner item 33): …and ROUND 6's own comment was the
                // whole bug — "the project directory exists at this point but
                // nothing will ever be written into it" is a stray, and a failed
                // Start is the easiest way to make one. It is now rolled back.
                val why = started.exceptionOrNull()
                logEvent(LOG_TAG_SEAL, "engine startCapture FAILED for ${project.id}: ${why?.message}")
                // The rollback happens BEFORE the banner is raised, so that the
                // moment the operator is told the scan did not start, the tab is
                // already back in its new-scan state and the directory is
                // already gone — no window in which the screen says "nothing was
                // recorded" while a half-made project is still listable.
                if (createdByThisStart) rollBackUnstartedProject(project)
                _saveError.value =
                    "The scan did not start (${why?.message ?: "the engine refused"}). Nothing is being " +
                        "recorded — check the sensor connection and press Start again."
                // ROUND 17 item 64: the seal must never be able to call this a
                // GOOD scan. ScanSummary.engineStartFailed is graded above
                // density, above sections, above everything.
                _engineStarted.value = false
                releaseStart()
                return@launch
            }
            _engineStarted.value = true
            releaseStart()
            sealPending.set(true)
            // ROUND 7 (field bug 2): from here on, a scan that receives nothing
            // has two seconds before it has to say so.
            startNoDataWatchdog()
            // ROUND 16 item 58(b): armed in the same breath as the no-data
            // watchdog, because the two together are the complete answer to
            // "is this scan real" — one watches the sensor, one watches the
            // tracker, and scan-039 passed the first and failed the second.
            startPoseWatchdog()
            // ROUND 7 (item 3): sections belong to a capture. A break detected
            // while framing the preview is not this scan's seam.
            arController?.let { controller ->
                controller.resetSections()
                _sectionCount.value = 1
                _unhealedSectionBreaks.value = 0
                // ROUND 18 item 70 (owner correction): the 3 AM losses were
                // NOT low light — the owner reports good lighting, and the
                // bytes say the operator was ~1.0-1.2 m from surfaces with
                // 63-70 % of returns under 1.5 m at every long loss (round
                // 13's measured re-anchor diet: close, feature-poor walls).
                // What the app never recorded is ARCore's OWN verdict, which
                // it hands over on every lost frame and which nothing
                // persisted. Into the bundle's debug log, so the next session
                // needs no guessing.
                controller.onTrackingLost = { arcoreReason ->
                    logDebug(LOG_TAG_AR, "tracking lost: arcoreReason=$arcoreReason")
                }
                controller.onSectionBreak = { br, healed ->
                    // ROUND 17 items 63 + 66: the re-anchor decision, with the
                    // numbers it was made on, into the bundle's own log. This
                    // is the line that would have made scan-040 obvious on the
                    // evening it was taken.
                    logDebug(
                        LOG_TAG_AR,
                        "section break: reason=${br.reason} gapMs=${br.gapMillis} " +
                            ("jump=%.3fm/%.2fdeg".format(br.positionJumpM, br.rotationJumpDeg)) +
                            " healed=$healed" +
                            (controller.lastReanchorSummary()?.let { " :: $it" } ?: ""),
                    )
                    _sectionCount.value = controller.sections.sectionCount()
                    // ROUND 15 item 54: only an UNHEALED break can reach the
                    // cue. The count the scheduler keys on is this one, not
                    // the section count, because a healed break is invisible
                    // on screen and there is nothing for the operator to do
                    // about it — buzzing would be telling them about work the
                    // app already did, and ROUND 13 measured that a cue buzz
                    // is itself capable of causing the next break.
                    if (!healed) _unhealedSectionBreaks.value = controller.unhealedBreakCount
                    // ROUND 18 item 68: the unhealed arm used to print a
                    // FABRICATED reason — "(no usable bracket)" — for every
                    // refusal, including the engine's own verdicts. The
                    // owner's whole 0.9.2 session read as bracket failures
                    // when the engine had in fact examined each gap and
                    // refused it with numbers (or, for scan-053, refused it
                    // wrongly for want of 46 ms of gyro coverage — a bug this
                    // line's wording helped hide for a round). Print what the
                    // engine decided; "no verdict recorded" is the honest
                    // fallback when the bracket really never reached it.
                    logEvent(
                        LOG_TAG_AR,
                        "SECTION BREAK #${controller.sections.sectionCount() - 1} " +
                            (if (healed) "HEALED live " else "NOT healed ") +
                            "reason=${br.reason} jump=%.3fm/%.2fdeg gapMs=${br.gapMillis} t=${br.tMonoNs}"
                                .format(br.positionJumpM, br.rotationJumpDeg) +
                            if (!healed) {
                                " :: " + (controller.lastReanchorSummary() ?: "no verdict recorded (bracket never reached the engine)")
                            } else {
                                ""
                            },
                    )
                }
            }
            // ROUND 8 (item 30d): the extrinsic + `pushbroom_enable`, before the
            // AR pipelines and independently of whether there is a camera
            // controller at all. For a D6 this is what makes the capture 3D
            // (ROUND 5 item 11), so it must not be a side effect of ARCore
            // starting — and its log line is the app's only record of which
            // extrinsic a scan actually ran on.
            applyMountExtrinsic(
                engineHandleProvider(),
                project.manifest.sensor,
                project.manifest.mountCalibration,
            )
            // ROUND 9 (item 35): the phone IMU, started for the same reason and
            // in the same place as the extrinsic — it is an ENGINE stream, not
            // an ARCore one, so it must not sit behind `arController ?: return`
            // inside startArPipelines (ROUND 8 item 30d's lesson) even though
            // what it densifies is ARCore's pose stream.
            startPhoneImu()
            startArPipelines(project)
            startPhoneGeorefIfNeeded(project)
        }
    }

    /**
     * ROUND 9 (owner item 35) — arms the phone-IMU stream for this capture.
     *
     * The `camera_from_imu` quaternion comes from the AR session's own camera
     * characteristics probe (`SENSOR_ORIENTATION`; see
     * [com.lidarscan.core.capture.CameraFromImu] for the derivation). With no AR
     * session there is no probe, so `resolve(null, false)` returns an
     * identity-with-a-reason and [com.lidarscan.app.ar.PhoneImuRecorder.start]
     * logs it loudly — never a silent guess.
     */
    private fun startPhoneImu() {
        val recorder = phoneImu ?: return
        if (isReplay) return
        val handle = engineHandleProvider()
        val extrinsics = arController?.status?.value?.cameraProbe?.cameraFromImu
            ?: com.lidarscan.core.capture.CameraFromImu.resolve(null, frontFacing = false)
        recorder.start(handle, extrinsics)
        logEvent(
            LOG_TAG_AR,
            "phone IMU start: handle=$handle available=${recorder.available} " +
                "camera_from_imu=${if (extrinsics.derived) "derived" else "IDENTITY"} (${extrinsics.why})",
        )
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
     * ROUND 9 (owner item 33): **undoes a project that never became a scan.**
     *
     * "Entering Capture = a new-scan context; leaving WITHOUT ever starting a
     * recording must leave NO project behind." Entry itself creates nothing
     * (item 9 moved creation to Start, and `onCleared` has nothing to undo), so
     * the one remaining way to leave a project behind without ever recording is
     * a Start that the engine refuses **after** the `.lscan` has been created —
     * which is exactly what ROUND 6's own comment on the failure branch admits
     * to. This deletes the directory and puts the tab back into its new-scan
     * state, so the Projects list never sees it.
     *
     * **The series number stays spent, deliberately.** It is a monotonic
     * device-level counter, not a count of directories
     * ([com.lidarscan.app.data.AppSettings.scanSeriesCounter] spells out why:
     * two `Scan-014-…` on one phone, taken weeks apart, is the confusion the
     * series exists to prevent). Handing the number back would mean the next
     * scan re-uses it, and a spent number costs nothing but a gap in the
     * sequence — a gap that is itself an honest record of a Start that failed.
     */
    private suspend fun rollBackUnstartedProject(project: Project) {
        val deleted = withContext(Dispatchers.IO) {
            com.lidarscan.app.ui.projects.ProjectPreviewCache.invalidate(project.id)
            runCatching { projectStore.delete(project.id) }.getOrDefault(false)
        }
        logEvent(
            LOG_TAG_STORE,
            "rolled back un-started project id=${project.id} deleted=$deleted " +
                "dir=${project.directory.absolutePath} (series number stays spent)",
        )
        // Back to a new-scan tab, with the auto-name the NEXT series number
        // implies — the Loaded state createProjectForThisScan() left behind
        // would otherwise make the next Start re-record into a project that no
        // longer exists on disk.
        _uiState.value = CaptureUiState.NewScan(
            autoName = com.lidarscan.core.capture.ScanAutoName.format(
                series = runCatching { peekSeriesNumber() }.getOrDefault(1),
                epochMillis = clock(),
            ),
        )
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

        // ROUND 8 (item 30d): `applyMountExtrinsic` moved OUT of here, up into
        // `startCapture`. It never belonged behind `arController ?: return`:
        // the extrinsic and `pushbroom_enable` are engine calls about the D6's
        // geometry, not ARCore calls, and gating them on the presence of a
        // camera controller meant a session without one silently recorded 2D
        // *and* wrote no record of having decided to. It also made the one
        // decision this round most needed to pin — "does a restored trim reach
        // the pushbroom?" — unreachable from a JVM test, because no JVM test
        // has an ARCore controller.

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
            // ROUND 17 item 67: `start()` creates `streams/frames/` and opens a
            // native index handle. Both are harmless with keyframes off (the
            // index FILE is written on the first record and there is never a
            // first record) — but an empty `frames/` directory in every bundle
            // invites exactly the question the capture screen now answers, so
            // it is not created unless something is going to be put in it.
            if (com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED) {
                recorder.start(project.directory)
            }
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
        val provenance = _mountTrimProvenance.value
        val trim = provenance.trim?.takeIf { it.sensor == sensor }
        val matrix = measuredMatrix
            ?: trim?.composedWith(nominalMatrix)
            ?: nominalMatrix
        val usingNominal = measuredMatrix == null
        val source = if (usingNominal) "nominal" else "measured"
        val trimSuffix = if (trim != null) provenance.logSuffix else "trim=none"

        if (handle == 0L) {
            _mountIsNominal.value = usingNominal
            // ROUND 8 (item 30d): logged even with no live engine handle.
            //
            // A DIFFERENT verb ("resolved", not "applied") on purpose — a field
            // log must never let "we worked out which extrinsic to use" be read
            // as "the engine is using it". What this line does give is the one
            // fact that was previously unobservable off-device: which trim the
            // session picked, and how old it was. That is what makes the
            // restored-trim path assertable on a bare JVM
            // (`CaptureRound8MountGateTest`) instead of only on a phone.
            logEvent(LOG_TAG_PUSHBROOM, "extrinsic resolved (no engine handle): source=$source $trimSuffix")
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
                "extrinsic applied: source=$source $trimSuffix pushbroomEnabled=$enabled",
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
        // ROUND 13 (owner item 47): FIRST, before anything that can throw. The
        // filter is the user's phone, not ours; a seal that fails must not
        // leave it silenced. `releaseDnd` is idempotent, so the onCleared path
        // calling it again costs nothing.
        runCatching { releaseDnd() }
        _dndState.value = com.lidarscan.core.capture.DndState.DISABLED
        // ROUND 14: the NOTE deliberately survives the seal. The filter has
        // been put back — that is what `_dndState` records — but the fact that
        // the walk just taken was unprotected is exactly the thing the operator
        // should still be reading afterwards, and it is equally true of the
        // next walk until they act on it. Clearing it here is half of why 0.8.0
        // showed the owner nothing; the other half was that nothing collected
        // it. It is cleared when the grant is actually obtained (see
        // `refreshDndNote`).
        val finalStats = _stats.value
        val noDataVerdict = _noDataAlert.value
        stopNoDataWatchdog()
        stopPoseWatchdog()
        val sectionBreaks = arController?.sections?.breaks().orEmpty()
        arController?.onSectionBreak = null
        // ROUND 5.2: the phone-GNSS fallback belongs to the session, so it stops
        // with it — before the engine handle goes away, since the recorder pushes
        // into that handle.
        stopPhoneGeoref()
        // ROUND 9 (item 35): the IMU stream pushes into the engine handle too,
        // so it stops here, before that handle goes away — same rule, same
        // reason. Its final counters are logged so a field session can be asked
        // afterwards what rate the device actually granted.
        phoneImu?.let { recorder ->
            recorder.stop()
            logEvent(LOG_TAG_AR, recorder.status.value.summary())
        }
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

        // ROUND 11 (owner item 44): the graded card, built from the SAME
        // instant's numbers as the raw summary above.
        //
        // `trackingLossEpisodes` had to be reached for here: it has existed on
        // `ArStatus` since ROUND 6 and fed only the Diagnostics sheet, so the
        // one number that says "there are holes in this room" was never part of
        // what a finished scan reported.
        val trackingDrops = arController?.status?.value?.trackingLossEpisodes ?: 0
        val summary = com.lidarscan.core.capture.ScanSummary(
            pointsCaptured = finalStats.pointsCaptured,
            elapsedMillis = finalStats.elapsedMillis,
            pathLengthMeters = trailRecorder.totalPathM.value.toDouble(),
            // ROUND 18 item 70: the teleports, named and kept out of pathM.
            jumpLengthMeters = trailRecorder.totalJumpM.value.toDouble(),
            sections = sectionBreaks.size + 1,
            trackingDrops = trackingDrops,
            recordingSizeBytes = finalStats.recordingSizeBytes,
            // ROUND 12: the two things the app can honestly say about geometry.
            mountTrimAccuracyDeg = _storedMountTrim.value?.trim?.accuracyDeg,
            loopEndGapMeters = trailRecorder.loopEndGapM?.toDouble(),
            // ROUND 16 item 58(c): THE SEAL HAS TO BE HONEST.
            //
            // The owner's scan-039 was graded FAIR. It had no poses, so it had
            // no path, so its density was computed as 368,908 points per metre
            // and its section count as 1 — three numbers that all look
            // excellent and all mean nothing. The one number that would have
            // said so was never on the card, because nothing counted it.
            //
            // NULLABLE, and the `?:` that is NOT here is the point. A rig with
            // no AR controller at all — a Mid-360 session, a replay, a unit
            // test — has not measured this, and "not measured" is a different
            // statement from "measured zero". Defaulting it to 0 made every
            // such capture 2D-only, which is how this line first broke four
            // round-15 tests: they run without an ARCore controller, and the
            // app promptly refused to process their scans for a reason that
            // was not true of them.
            posesRecorded = arController?.acceptedPoseCount,
            // ROUND 17 item 64: the two things scan-045 needed and did not have.
            //
            // `engineStarted` is what `scan_engine_start` answered; `false`
            // outranks every other measurement on the card.
            //
            // `worldPointsResolved` is read off the FILE, not off a counter —
            // `streams/map.bin` is the resolved world cloud (16 bytes per
            // PointVertex) and its absence is what "no room" literally means.
            // scan-045 sealed 55,228 returns with no map.bin at all, and the
            // ROUND 16 pose check could not see it: it had 225 poses. A counter
            // can be reset by a stray call; the bytes on disk cannot.
            engineStarted = _engineStarted.value,
            // ...and the SAME `?:`-that-is-not-here rule as the line above,
            // for the same reason and learned the same way. A rig with no AR
            // controller — a Mid-360 session, a replay, the four round-15 unit
            // tests — has no pushbroom writing `map.bin`, so its absence there
            // is not evidence of anything. Reading it as zero made every one of
            // those captures "NO ROOM", which is exactly the mistake ROUND 16's
            // comment three lines up was written to stop being repeated.
            worldPointsResolved = arController?.let {
                resolvedWorldPoints((_uiState.value as? CaptureUiState.Loaded)?.project?.directory)
            },
        )
        _scanSummary.value = summary
        // ROUND 17 item 66.
        logDebug(
            LOG_TAG_SEAL,
            "summary built: headline=${summary.headline} grade=${summary.grade} " +
                "engineStarted=${summary.engineStarted} " +
                "worldPoints=${summary.worldPointsResolved} poses=${summary.posesRecorded} " +
                ("pathM=%.2f jumpM=%.2f".format(summary.pathLengthMeters, summary.jumpLengthMeters)),
        )
        // ROUND 13, bug (A)+(B), and they were ONE bug.
        //
        // `"a" + "b".format(args)` does not format `"a" + "b"`: a method call
        // binds tighter than `+`, so `.format()` applied to the LAST literal
        // fragment only. The earlier fragment's `%.1f sections=%d drops=%d
        // ptsPerM=%.0f` was never substituted and printed verbatim, and the
        // six arguments were consumed by the two `%s` that WERE in scope —
        // so `trimAccuracyDeg=` printed pathLengthMeters and `loopEndGapM=`
        // printed the section count. That is where the owner's 0.7.1 log got
        // `trimAccuracyDeg=15.99` beside a stored `stabilityDeg=0.39`: 15.99
        // is 15.99 METRES WALKED, and `loopEndGapM=2` is 2 SECTIONS.
        //
        // Built as one string with one format call, so the arity is checked
        // in one place. The card itself was never affected — it reads the
        // `ScanSummary` fields directly — but the log is what field reports
        // are written from, so a wrong number there is a wrong round.
        logEvent(
            LOG_TAG_SEAL,
            (
                "summary: grade=%s points=%d durationMs=%d pathM=%.1f jumpM=%.1f poses=%d " +
                    "sectionsLive=%d drops=%d " +
                    "ptsPerM=%.0f trimAccuracyDeg=%s loopEndGapM=%s"
            ).format(
                summary.headline,
                summary.pointsCaptured,
                summary.elapsedMillis,
                summary.pathLengthMeters,
                summary.jumpLengthMeters,
                summary.posesRecorded ?: -1L,
                // ROUND 16 item 62: LABELLED `sectionsLive`, because that is
                // what it is — the count the live break detector arrived at
                // while walking. The owner's scan-038 sealed with `sections=3`
                // and auto-processed to `sections=2`, and the two lines used
                // the same word for two different measurements taken by two
                // different detectors over two different pose streams (live
                // ARCore poses as they arrived, versus the recorded stream
                // re-derived offline, where a 1.6 s TRACKING_REGAINED gap is
                // bridged rather than split). Neither number was wrong. The
                // word was.
                summary.sections,
                summary.trackingDrops,
                summary.pointsPerMeter,
                summary.mountTrimAccuracyDeg?.let { "%.2f".format(it) } ?: "none",
                summary.loopEndGapMeters?.let { "%.2f".format(it) } ?: "not-a-loop",
            ),
        )

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

        // ── ROUND 9, owner item 33: does this scan get to survive Stop? ──────
        //
        // "record+stop keeps + redirects (as shipped)" — but a capture that
        // recorded ZERO points is not a scan, it is the `scan-012`/`scan-014`
        // clutter the owner is asking to be rid of. Decided BEFORE the seal
        // reports success, because the two outcomes say different things on
        // screen and only one of them may claim the scan was saved.
        //
        // Three guards, all of them about not deleting something that is not
        // this capture's to delete:
        //  * `!isReplay` — a replay writes no new points by design
        //    (`ReplayEngineBridge`), so every replay would look empty;
        //  * `projectId == null` — a project-scoped/deep-link entry records into
        //    a project that existed before Start;
        //  * the setting, for the operator who is diagnosing a dead sensor and
        //    for whom the empty `.lscan` IS the evidence.
        val emptyScan = finalStats.pointsCaptured == 0L && !isReplay && projectId == null
        val pruneEmptyScan = emptyScan && !runCatching { keepEmptyScans() }.getOrDefault(false)

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
                    mountTrim = _storedMountTrim.value?.trim ?: manifest.mountTrim,
                    // ROUND 7 (item 3): the seams, so the post pipeline has the
                    // one fact that makes them alignable rather than mysterious.
                    sectionBreaks = sectionBreaks,
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
            // ROUND 17 item 66: closed on BOTH arms — a bundle whose seal
            // failed is the one a person is most likely to send.
            endDebugLog("capture debug log closed (seal failed)")
        } else {
            // ROUND 9 (item 33): a scan that is about to be pruned was not
            // "saved to <path>" — saying so would send the operator to a
            // directory this method deletes three lines from now.
            if (!pruneEmptyScan) _lastSavedProject.value = verified.directory.absolutePath
            logEvent(
                LOG_TAG_SEAL,
                "sealed OK id=$activeId name=\"${verified.manifest.name}\" " +
                    "points=${finalStats.pointsCaptured} elapsedMs=${finalStats.elapsedMillis} " +
                    "listable=true sectionsLive=${sectionBreaks.size + 1} " +
                    "dir=${verified.directory.absolutePath}" +
                    // ROUND 7: a zero-point seal is never again just "OK". The
                    // owner's scan-009 line said `sealed OK … points=0
                    // elapsedMs=0` and that was the entire record of a capture
                    // that recorded nothing.
                    if (finalStats.pointsCaptured == 0L) {
                        " NO-DATA=true reason=\"${noDataVerdict ?: "no sensor packets reached this session"}\""
                    } else {
                        ""
                    },
            )
            // ROUND 17 item 66 closed the log here, AFTER the summary and the
            // seal line — and thereby before auto-process, so the file never
            // carried the offline verdicts. ROUND 18 item 71: the log now
            // stays open through auto-process for exactly the capture that is
            // about to run one; scan-053's log would then have carried the
            // gap verdicts the owner's question needed. A scan about to be
            // PRUNED still closes here — the directory it lives in is deleted
            // three lines down.
            if (pruneEmptyScan) {
                endDebugLog(
                    "capture debug log closed — sealed OK (empty, about to be pruned), " +
                        "grade=${summary.headline}, points=${finalStats.pointsCaptured}",
                )
            } else {
                logDebug(
                    LOG_TAG_SEAL,
                    "sealed OK, grade=${summary.headline}, points=${finalStats.pointsCaptured} — " +
                        "log stays open for this capture's auto-process verdicts",
                )
            }
        }
        // ROUND 9 (item 33): the prune itself, after the seal has been written
        // and read back — so the capture log still carries the full `sealed OK …
        // NO-DATA=true` line for the scan that was discarded, and the operator
        // has a record of the attempt even though its directory is gone.
        //
        // A FAILED seal is never pruned, even when it is empty: the branch above
        // has just told the operator "its raw data IS on the phone at <path> —
        // do not delete it", and deleting it three lines later would make the
        // app a liar about the one screen it shows when something has gone
        // properly wrong. A scan whose manifest could not be written or read
        // back is not known to be empty; it is only known to be broken.
        val prunedEmptyScan = pruneEmptyScan && sealed != null && verified != null && withContext(Dispatchers.IO) {
            com.lidarscan.app.ui.projects.ProjectPreviewCache.invalidate(activeId)
            runCatching { projectStore.delete(activeId) }.getOrDefault(false)
        }
        if (prunedEmptyScan) {
            logEvent(
                LOG_TAG_STORE,
                "pruned EMPTY scan id=$activeId points=0 dir=${projectDir?.absolutePath} " +
                    "(Settings > Scans > \"Keep empty scans\" turns this off)",
            )
        }

        if (finalStats.pointsCaptured == 0L && !isReplay) {
            // ROUND 7: the scan is saved and it is empty, and the operator has
            // to hear that from the app rather than from the Projects list two
            // days later. The watchdog is stopped by now, so nothing clears it.
            //
            // ROUND 9 (item 33): two arms now, because the banner has to match
            // what actually happened to the directory.
            _noDataAlert.value = if (prunedEmptyScan) {
                "THIS SCAN RECORDED NO POINTS. ${noDataVerdict ?: "No sensor packets reached the session."} " +
                    "Nothing was saved — the empty scan was removed rather than left in the Projects list. " +
                    "Re-seat the USB-C cable and scan again. " +
                    "(Settings › Scans › \"Keep empty scans\" keeps failed attempts instead.)"
            } else {
                "THIS SCAN RECORDED NO POINTS. ${noDataVerdict ?: "No sensor packets reached the session."} " +
                    "The project was saved so the evidence is not lost — but there is nothing in it. " +
                    "Re-seat the USB-C cable and scan again."
            }
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
            // ── ROUND 8, owner item 31: the tab is RE-ARMED, not merely idle ──
            //
            // "the capture tab, when returned to, is RESET: fresh auto-name,
            // still connected/armed, live preview running, ready to Start again
            // immediately."
            //
            // The `NewScan` above was already ROUND 5 AUDIT's half of that (a
            // second Start must create project #2, not re-record into #1). These
            // three are the half that was missing, and the first is a real bug:
            //
            //  * **the typed name was never cleared.** `_scanName` is what
            //    `ScanAutoName.resolve(typedName = …)` prefers over the
            //    auto-name, so an operator who typed "Kitchen" once got
            //    "Kitchen" — the same name, for a different scan — on every
            //    subsequent Start of that session, with only the directory slug
            //    to tell them apart. A name is spent when the scan it named is
            //    sealed.
            //  * **the stats line kept the finished scan's numbers**, so a
            //    re-armed tab claimed 216 k points and 30 s of recording before
            //    the next Start. `_sessionSummary` was already snapshotted above
            //    from `finalStats`, so the sheet is unaffected.
            //  * **the section count belonged to the scan that just ended.**
            //
            // What is deliberately NOT touched: the connection, `autoConnect`'s
            // state, `_pointCloudSource`, the preset, the display parameters and
            // the mount trim. "Still connected/armed, live preview running" is
            // exactly the absence of code here — and the mount trim outliving a
            // capture is ROUND 7's field bug 1, which must stay fixed.
            _scanName.value = ""
            _stats.value = CaptureStats()
            lastStatsSampleMillis = 0L
            lastStatsSamplePoints = 0L
            _sectionCount.value = 1
            _unhealedSectionBreaks.value = 0
            // ROUND 11 (item 43): per-session, like everything else here. A
            // scheduler carried across captures would open the next one with
            // the previous one's section count as its baseline and buzz on the
            // first frame.
            cues.reset()
            // ── ROUND 10 (owner item 38) ────────────────────────────────
            // ...and the live map, which ROUND 8's comment above explicitly
            // listed as "deliberately NOT touched". That was wrong, and it is
            // the whole of "it still show with the previous capture": the
            // pages belong to the process-lifetime engine, not to the session
            // that made them, so leaving them meant the NEXT capture opened on
            // top of this one's cloud. The thumbnail was snapshotted from
            // `previewSource` well above this line, so nothing is lost.
            //
            // The connection, the preset, the display parameters and the mount
            // trim are still deliberately untouched — "still connected/armed,
            // live preview running" is the contract, and an empty live map IS
            // the running preview at the moment a scan ends.
            clearLiveViewport()
        } else {
            projectStore.open(activeId)?.let { _uiState.value = CaptureUiState.Loaded(it) }
        }

        // ROUND 8 (item 31): the seal is complete and verified — tell the
        // navigator which scan to open. Emitted last, and only on the verified
        // path: navigating away from a capture that FAILED to save would take
        // the operator off the one screen carrying the red banner that says so.
        //
        // ROUND 9 (item 33): …and only when the scan still exists. Navigating to
        // a project this Stop just deleted would land the operator on an empty
        // list entry that vanishes under them on the next refresh; the red
        // no-data banner on THIS screen is the thing they need to read.
        if (verified != null && !prunedEmptyScan) {
            // ── ROUND 15 item 55 ────────────────────────────────────────
            // AFTER the read-back verify and BEFORE the navigation emit. The
            // order is the whole safety argument: the container is on disk and
            // has been re-opened successfully, so nothing below can lose it,
            // and the navigation intent is still emitted whatever processing
            // does — a failed reprocess must not strand the operator on the
            // Capture tab with a scan they cannot reach.
            val mountWarned = _storedMountTrim.value?.trim?.accuracyDeg?.let { it > 1.0 } ?: false
            // ── ROUND 16 item 58(c): do not run a pipeline that cannot run. ──
            //
            // On scan-039 the app tried, failed inside the engine, and told the
            // operator "the scan is saved and untouched; open it and tap
            // Process" — an instruction that could only ever fail again, for a
            // reason nothing on screen or in the log ever named. Every stage of
            // `reprocess_d6_container` needs the pose stream: the resolve needs
            // it to place a return, the section stitch is a function of it, the
            // ruler measures a cloud that does not exist without it. With zero
            // poses the honest answer is available before the attempt.
            if (summary.engineStartFailed || summary.isNoRoom || summary.isTwoDimensionalOnly) {
                _autoProcess.value = AutoProcessState(
                    projectId = activeId,
                    running = false,
                    progress = 1f,
                    failed = true,
                    // ROUND 17 item 64: three reasons, three sentences. The
                    // owner's scan-045 hit the third one, said nothing about
                    // it, and then reported a bare "auto-process FAILED" —
                    // which reads as a bug in Process and was nothing of the
                    // kind. A refusal that names its reason is a diagnosis; a
                    // failure that does not is a mystery.
                    blocked = when {
                        summary.engineStartFailed ->
                            "The recording engine never started for this scan, so there is " +
                                "nothing to process. The raw sensor data is saved and untouched."
                        summary.isNoRoom ->
                            "This scan has no resolved map: the returns arrived but none of " +
                                "them were placed in space, so there is no cloud for Process " +
                                "to stitch or measure. The raw sensor data is saved and " +
                                "untouched."
                        else ->
                            "This scan has no camera positions, so there is nothing to " +
                                "process: every stage — placing the returns, stitching the " +
                                "pieces, measuring the map — needs the trajectory, and this " +
                                "capture recorded none. The raw sensor data is saved and " +
                                "untouched."
                    },
                )
                logEvent(
                    LOG_TAG_SEAL,
                    "auto-process SKIPPED for $activeId — " +
                        when {
                            summary.engineStartFailed ->
                                "scan_engine_start refused this capture; nothing was recorded " +
                                    "into a world frame"
                            summary.isNoRoom ->
                                "no streams/map.bin — ${summary.pointsCaptured} returns " +
                                    "arrived and none resolved to world points"
                            else ->
                                "zero poses recorded; the container has lidar.bin and " +
                                    "imu_phone.bin but no poses_ar.bin, so there is no " +
                                    "trajectory to resolve against"
                        } + " (Process would fail for the same reason)",
                )
                // ROUND 18 item 71: nothing more will be decided about this
                // capture, so the log closes here with the refusal inside it.
                endDebugLogFor(
                    verified.directory,
                    "capture debug log closed — auto-process refused before starting: " +
                        (_autoProcess.value.blocked ?: "no reason recorded"),
                )
            } else if (autoProcessPlan(sectionBreaks.size + 1, mountWarned)) {
                startAutoProcess(activeId, verified.directory, sectionBreaks.size + 1)
            } else {
                // The fast path still runs the cheap ruler — that is the
                // difference between "we skipped it" and "we have nothing to
                // tell you". It is the same call; what it skips is the seam
                // refinement and the second cloud write, neither of which has
                // anything to do on one section.
                startAutoProcess(activeId, verified.directory, 1)
                _autoProcess.value = _autoProcess.value.copy(skipped = true)
            }
            _sealedProjectId.tryEmit(activeId)
            // ROUND 10 (owner item 38): the navigation intent is LOGGED, so the
            // next field log answers "did the app try to navigate and the shell
            // ignore it, or did it never try?" without anyone guessing. The
            // absence of this line beside a `sealed OK` line is now itself the
            // diagnosis.
            logEvent(LOG_TAG_SEAL, "navigate -> Projects id=$activeId")
        } else {
            logEvent(
                LOG_TAG_SEAL,
                "staying on Capture (no navigation): verified=${verified != null} " +
                    "pruned=$prunedEmptyScan — the banner on this screen is the thing to read",
            )
        }
    }

    // ── ROUND 15 item 55: AUTO-PROCESS ON SEAL ──────────────────────────────
    //
    // > "Stop -> seal -> reprocess runs automatically, progress on the summary
    // >  card, card shows POST-process numbers."
    //
    // The choreography, and why it is this and not something simpler:
    //
    //  1. Stop seals the container and the ROUND 10 card appears. Nothing about
    //     that changes: the card is what holds navigation (CaptureScreen only
    //     navigates once `sessionSummary` goes null), so it is also the only
    //     place a progress bar can live without inventing a second modal.
    //  2. Processing starts on `Dispatchers.IO` against the SEALED DIRECTORY.
    //     It shares nothing with the capture engine — `reprocessD6` is
    //     handle-less and opens its own PageStore — which is what makes it
    //     safe for the tab to have already re-armed and for the operator to
    //     press Start again while it runs. The ROUND 14 `resetWorldFrame()`
    //     rebuilds the ARCore session on that Start; this job never touches it.
    //  3. The card grows a line and a bar while it runs, and swaps to the
    //     POST-process numbers when it finishes.
    //  4. Done -> Projects, exactly as before.
    //
    // AND IT NEVER LOSES A SCAN. The container is sealed and verified before
    // any of this starts; processing only ever ADDS `processed/` files. Every
    // failure path leaves the scan on disk and says the one thing the operator
    // can act on — open it and tap Process.
    private val _autoProcess = MutableStateFlow(AutoProcessState())
    val autoProcess: StateFlow<AutoProcessState> = _autoProcess.asStateFlow()

    /**
     * The FAST PATH (item 55): a capture that recorded in one piece and raised
     * no warning has nothing to stitch, so the expensive part is skipped — but
     * the cheap ruler is not, because "surfaces repeat within X cm" is the one
     * number on the card that is about the map rather than about the run, and
     * a clean capture is exactly the one whose owner will believe it.
     *
     * Implemented as a decision rather than a shortcut inside the engine: the
     * engine's own `stitch_sections` is provably a no-op on one section
     * (`SectionCorrection` is exactly identity), so this is about not paying
     * for the refinement pass and the second cloud write, not about
     * correctness.
     */
    private fun autoProcessPlan(sections: Int, mountWarned: Boolean): Boolean =
        sections > 1 || mountWarned

    private fun startAutoProcess(projectId: String, directory: java.io.File, sections: Int) {
        _autoProcess.value = AutoProcessState(
            projectId = projectId,
            running = true,
            progress = 0f,
            willStitch = sections > 1,
        )
        // NonCancellable and on viewModelScope, the same posture the seal
        // itself uses: an Activity recreation between Stop and Done must not
        // abandon a half-written `processed/` directory.
        viewModelScope.launch {
            val result = runCatching {
                withContext(Dispatchers.IO + kotlinx.coroutines.NonCancellable) {
                    runAutoProcess(directory) { f ->
                        _autoProcess.value = _autoProcess.value.copy(
                            progress = f.coerceIn(0f, 1f),
                        )
                        true
                    }
                }
            }.getOrNull()

            if (result == null || !result.ran) {
                _autoProcess.value = _autoProcess.value.copy(
                    running = false,
                    progress = 1f,
                    failed = true,
                )
                logEvent(
                    LOG_TAG_SEAL,
                    "auto-process FAILED for $projectId — the scan is saved and untouched; " +
                        "open it and tap Process",
                )
                // ROUND 18 item 71: the failure is a verdict too, and the
                // bundle's own log is where the person who exports this scan
                // will look for it. Guarded close — a newer capture may own
                // the sink by now.
                endDebugLogFor(
                    directory,
                    "capture debug log closed — auto-process FAILED (ran=${result?.ran ?: false}); " +
                        "raw streams intact, Process can be retried from Review",
                )
                return@launch
            }
            _autoProcess.value = _autoProcess.value.copy(
                running = false,
                progress = 1f,
                result = result,
            )
            logEvent(
                LOG_TAG_SEAL,
                "auto-process done for $projectId: sectionsProcessed=${result.sections} " +
                    "refined=${result.seamsRefined} points=${result.points} " +
                    "selfCheckMeasurable=${result.selfCheck?.measurable} " +
                    ("selfCheckCm=%.2f".format((result.selfCheck?.offsetMeters ?: 0.0) * 100)),
            )
            // ROUND 18 item 71: the offline verdicts, into the bundle's own
            // log, then close. `processed/stitch.json` beside this file has
            // the full record (including the round-18 `gapsExamined` array —
            // every blind gap the stitcher measured and what it decided);
            // this line is the summary a person greps for.
            logDebugFor(
                directory,
                LOG_TAG_SEAL,
                "auto-process verdicts: sectionsProcessed=${result.sections} " +
                    "refined=${result.seamsRefined} " +
                    "selfCheckMeasurable=${result.selfCheck?.measurable} " +
                    ("selfCheckCm=%.2f".format((result.selfCheck?.offsetMeters ?: 0.0) * 100)) +
                    " — gap-by-gap detail in processed/stitch.json (gapsExamined)",
            )
            endDebugLogFor(directory, "capture debug log closed — auto-process complete")
        }
    }

    fun dismissSessionSummary() {
        _sessionSummary.value = null
        _scanSummary.value = null
        // Deliberately NOT cleared: a reprocess that is still running keeps
        // running, and its result is on the project when Review opens it. What
        // is cleared is only the card.
        if (!_autoProcess.value.running) _autoProcess.value = AutoProcessState()
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
        _keyframesEnabled.value = enabled && com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED
        keyframeRecorder?.setEnabled(_keyframesEnabled.value)  // ROUND 17 item 67
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

    /**
     * ROUND 9 (owner item 33) — **the leave path, audited.**
     *
     * "Entering Capture = a new-scan context; leaving WITHOUT ever starting a
     * recording must leave NO project behind (no dir, no list entry)." Nothing
     * below deletes anything, and that is the correct implementation rather
     * than an omission: screen ENTRY creates no project at all (ROUND 5 item 9
     * moved creation into `startCapture`, and `_uiState` opens on
     * [CaptureUiState.NewScan]), so a tab that is opened and left has nothing
     * to undo. The two ways a project could otherwise be left behind are
     * handled where they happen — a refused Start rolls back
     * ([rollBackUnstartedProject]) and a 0-point Stop prunes
     * (`sealAndStopLocked`) — and both are pinned by `CaptureRound9FlowTest`.
     *
     * A project that HAS been recorded into is never touched here, even if the
     * screen is destroyed mid-capture: those bytes are the operator's.
     */
    override fun onCleared() {
        // ROUND 13 (owner item 47): the abandon path. A screen destroyed
        // mid-capture must not leave the phone silenced — this is the same
        // rule the phone-IMU stop below has followed since ROUND 9.
        runCatching { releaseDnd() }
        stopPhoneGeoref()
        // ROUND 9 (item 35): a HandlerThread and two registered SensorEventListeners
        // outlive a ViewModel quite happily if nobody unregisters them, and this
        // is the one path a mid-capture screen destruction takes.
        phoneImu?.stop()
        detachTrailListener()
        detachRigPoseListener()
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

        /**
         * ROUND 16 item 58(b). One second between checks, three before the
         * banner. Three, and not the ROUND 7 watchdog's two: ARCore genuinely
         * does go quiet for a second or so through a hard turn, and a banner
         * that cries wolf on an ordinary corner is a banner the operator learns
         * to look past. Three seconds of lidar arriving with no position at all
         * is not a corner.
         */
        const val NO_POSE_TICK_MS = 1_000L
        const val NO_POSE_GRACE_MS = 3_000L
        const val LOG_TAG_AR = "ar"

        /**
         * ROUND 14 (item 53). The owner's whole 0.8.0 log contains no network
         * tag at all — two dead Mid-360 sessions and not one line saying which
         * addresses were in play. A field report has to carry the diagnosis.
         */
        const val LOG_TAG_NET = "net"
        const val LOG_TAG_PUSHBROOM = "pushbroom"
        const val LOG_TAG_STORE = "store"

        /** ROUND 6 (item 21): how often the live page-store ceiling is checked. */
        const val LIVE_MAP_WATCH_MS = 1_000L

        /** How often the motion hint is re-evaluated (ROUND 5.3 item 18). */
        const val MOTION_HINT_TICK_MS = 500L

        /**
         * ROUND 11 (item 45a): the hold-still ring's tick. 100 ms is fast
         * enough that the ring visibly empties the instant the rig wobbles —
         * which is the feedback — and slow enough that re-running the gate's
         * percentile over ~30 samples costs nothing.
         */
        const val MOUNT_HOLD_TICK_MS = 100L

        /** How long a hold that never gets still is allowed to keep polling. */
        const val MOUNT_HOLD_GIVE_UP_MS = 30_000L

        /**
         * How long after the last motion-gated skip the hint stays up. Long enough
         * to be readable mid-walk, short enough that it clears within a couple of
         * steps of slowing down — a hint that lingers becomes wallpaper.
         */
        const val MOTION_HINT_LINGER_MS = 2_500L

        /** ROUND 7 (field bug 1): how often the trim's age label is recomputed. */
        const val TRIM_AGE_TICK_MS = 15_000L

        /**
         * ROUND 7 (field bug 2): how long a recording may produce **nothing**
         * before the screen says so.
         *
         * A COIN-D6 at 10 Hz with ~1000 returns a revolution produces its first
         * `POINTS_AVAILABLE` event within a few hundred milliseconds of the
         * spin-up command being ACKed. Two seconds is well past that and still
         * inside the window where the operator has not yet started walking, so
         * the warning arrives before the scan is wasted rather than after it.
         */
        const val NO_DATA_GRACE_MS = 2_000L

        /** How often the no-data watchdog re-checks. */
        const val NO_DATA_TICK_MS = 500L
    }
}
