package com.lidarscan.app.di

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import androidx.core.content.ContextCompat
import com.lidarscan.app.BuildConfig
import com.lidarscan.app.ar.ArInstaller
import com.lidarscan.app.ar.CaptureArController
import com.lidarscan.app.render.NativePointCloudProvider
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.core.calib.FileMountCalibrationStore
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.engine.RealEngineBridge
import com.lidarscan.app.engine.ReplayEngineBridge
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.merge.MergeRepository
import com.lidarscan.app.net.EthernetMonitor
import com.lidarscan.app.net.UdpMid360Detector
import com.lidarscan.app.processing.ProcessingRepository
import com.lidarscan.app.rtk.RtkManager
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineBridgeProvider
import com.lidarscan.core.engine.FakeEngineBridge
import com.lidarscan.core.store.FileProjectStore
import com.lidarscan.core.store.ProjectStore
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking

/**
 * Manual "DI-lite" composition root (no Hilt yet, per the B1 brief). One
 * instance is created in [com.lidarscan.app.LidarScanApplication] and handed
 * down to screens/ViewModels via `viewModel(factory = ...)` at each call
 * site.
 */
class AppContainer(context: Context) {

    private val appContext = context.applicationContext

    /**
     * ROUND 7: the application context, for the one class that needs it outside
     * a composition — [com.lidarscan.app.share.DownloadsExporter], which writes
     * finished exports into the shared Downloads collection. Exposed rather than
     * threaded through every ViewModel constructor because it is the same single
     * application context those ViewModels would be handed anyway.
     */
    val applicationContext: Context get() = appContext

    /**
     * The process-lifetime scope. Public since ROUND 22 item 90: the post-seal
     * auto-process used to run in `viewModelScope`, and item 88's navigation
     * cancelled that scope at the exact moment the seal navigated to Projects
     * — so the engine finished, wrote correct output, and the app reported a
     * failure that had not happened. Work that must outlive the screen that
     * started it belongs here, and now says so.
     *
     * `SupervisorJob` so one failed job never cancels the others.
     */
    val containerScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /**
     * `.lscan` projects live under app-specific external storage: no runtime
     * permission needed (scoped storage), plenty of room for multi-GB
     * captures, and it survives app updates. Falls back to internal storage
     * if external isn't mounted. Shown read-only on the Settings screen; a
     * user-facing location picker (SAF) is future work, not B1 scope.
     */
    val projectsRootDir: File = File(appContext.getExternalFilesDir(null) ?: appContext.filesDir, "Projects")

    /**
     * ROUND 6 (owner item 20): the persistent on-device capture log. Created
     * before the project store, because the store's own create/seal/recovery
     * events are the first thing worth recording.
     */
    val captureLog = com.lidarscan.app.debug.CaptureLog(appContext)

    val projectStore: ProjectStore = FileProjectStore(
        rootDir = projectsRootDir,
        appVersion = BuildConfig.VERSION_NAME,
        onDiagnostic = { line -> captureLog.log(com.lidarscan.app.debug.CaptureLog.TAG_PROJECT, line) },
    )

    /**
     * ROUND 6 (owner items 21 + 22): what this phone can carry, from what the
     * platform will tell us for free. Read once — RAM and core count do not
     * change, and the display ceiling only changes across a config change that
     * would rebuild the screens reading it anyway.
     */
    val deviceTier: com.lidarscan.core.capture.DeviceTier = run {
        val am = appContext.getSystemService(Context.ACTIVITY_SERVICE) as? android.app.ActivityManager
        val memInfo = android.app.ActivityManager.MemoryInfo().also { info ->
            runCatching { am?.getMemoryInfo(info) }
        }
        val totalRamMb = (memInfo.totalMem / (1024L * 1024L)).coerceAtLeast(0L)
        val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
        val ceilingHz = com.lidarscan.app.render.displayRefreshCeilingHz(appContext)
        com.lidarscan.core.capture.PerformancePresets.tierFor(totalRamMb, cores, ceilingHz).also { tier ->
            captureLog.log(
                com.lidarscan.app.debug.CaptureLog.TAG_STORE,
                "device tier=$tier ram=${totalRamMb}MB cores=$cores displayCeiling=${ceilingHz}Hz",
            )
        }
    }

    /**
     * ROUND 6 (owner item 21): the live `PageStore` sizing handed to
     * `scan_engine_create`, instead of B2's `0, 0` ("give me the desktop's
     * 16 MB pages and 1 GB ceiling"). See
     * [com.lidarscan.core.render.LivePageStoreSizing] for why that default made
     * a D6 live map stop growing after about a minute.
     */
    val livePageStoreSizing: com.lidarscan.core.render.LivePageStoreSizing =
        com.lidarscan.core.render.LivePageStoreSizing.forTier(deviceTier)

    /**
     * ROUND 6: the device profile key the performance preset is persisted
     * under. Model + tier, so the same account on a different phone does not
     * inherit a preset chosen for stronger hardware.
     */
    val deviceProfileKey: String = "${android.os.Build.MANUFACTURER}/${android.os.Build.MODEL}/$deviceTier"

    /**
     * ROUND 7 (field bug 1): a fresh id for this app process.
     *
     * The persisted mount trim carries the run id it was captured in
     * ([com.lidarscan.core.calib.StoredMountTrim]), so the capture panel can
     * tell "you set this two minutes ago on this screen" from "this came back
     * across an app restart" — the first is a confirmation, the second is worth
     * a sentence, and neither is a reason to throw the trim away, which is what
     * 0.3.0 did.
     *
     * A random id rather than a boot timestamp: a process can be killed and
     * restarted inside one boot (exactly what a walkthrough with the screen off
     * invites), and that is a restart the operator should hear about.
     */
    /**
     * ROUND 24 item 110(b) — **"replay the tutorial", asked for in Settings and
     * honoured on the Scan screen.**
     *
     * A tour of the Scan screen has to run ON the Scan screen, and Settings is
     * a different tab. This is the one-shot that carries the request across the
     * hop: Settings sets it and navigates; `CaptureRoute` collects it, starts
     * the tour and immediately clears it.
     *
     * On the container rather than in `SettingsRepository` because it is an
     * intent, not a preference — persisting it would mean a request made before
     * a crash re-fires the tour on the next launch, which is the sort of ghost
     * that takes a round to diagnose.
     */
    val tutorialReplayRequest = kotlinx.coroutines.flow.MutableStateFlow(false)

    val appRunId: String = java.util.UUID.randomUUID().toString()

    /** D6 USB device discovery, permission flow and open-connection registry (B2). */
    val d6UsbConnectionRegistry = D6UsbConnectionRegistry(appContext)

    /**
     * B3: USB-C Ethernet watcher for the Mid-360 connect wizard. Created here
     * (not per-screen) so the adapter's state survives navigation, but
     * `start()`/`stop()` are driven by the screen — an always-registered
     * `NetworkCallback` on a capture app is a battery cost with no payoff
     * while the user is in the projects list.
     */
    val ethernetMonitor = EthernetMonitor(appContext)

    /**
     * AUTO-DETECT: listens for the Mid-360's own heartbeat broadcast (port
     * 56201) so the connect wizard's first step can find a device with no
     * addresses typed in yet. A supplier lambda (not `ethernetMonitor.state
     * .value.network` captured once) because the Ethernet `Network` can
     * appear after this container is built — see `UdpMid360Detector`'s doc.
     */
    val mid360HeartbeatDetector = UdpMid360Detector { ethernetMonitor.state.value.network }

    init {
        // B3: MUST run before any Mid-360 device is added. The engine's SDK2
        // backend writes the config file `LivoxLidarSdkInit()` requires into
        // `std::filesystem::temp_directory_path()`, which resolves to nothing
        // writable on Android (no TMPDIR, no /tmp; the engine's fallback of
        // "." is the app's CWD, i.e. "/"). Setting TMPDIR to the app's own
        // cacheDir fixes that with no ABI change and no engine edit — see
        // `mid360_jni.cpp`'s header comment for the full write-up.
        //
        // Done in the container's init rather than lazily at connect time so
        // there is exactly one call site and no ordering question: by the
        // time any screen exists, the environment is already correct.
        if (ScanEngineNative.isAvailable) {
            ScanEngineNative.nativeSetTempDir(appContext.cacheDir.absolutePath)
        }
    }

    /**
     * Fired by [com.lidarscan.app.MainActivity]'s dynamically-registered
     * USB attach/detach receiver (the manifest's USB_DEVICE_ATTACHED
     * intent-filter only covers a cold/background launch — this covers the
     * app-already-in-foreground case). [com.lidarscan.app.ui.connect.ConnectWizardViewModel]
     * collects it to re-scan.
     */
    val usbAttachEvents = MutableSharedFlow<Unit>(extraBufferCapacity = 8)

    val settingsRepository: SettingsRepository = SettingsRepository(appContext).also { repo ->
        // ROUND 22 item 100: the display block's LOD budget is clamped to what
        // THIS device can hold, on load and on save. The tier is the one the
        // container already probed above — one definition of "what can this
        // phone do", not a second.
        repo.deviceTier = deviceTier
        repo.onDiagnostic = { line ->
            captureLog.log(com.lidarscan.app.debug.CaptureLog.TAG_STORE, line)
        }
    }

    /**
     * B2: [RealEngineBridge] when `scanengine_jni` loaded successfully and
     * neither the [BuildConfig.FORCE_FAKE_ENGINE] build flag nor the
     * persisted "use simulated engine" dev-settings toggle says otherwise;
     * [FakeEngineBridge] in every other case (including all JVM tests,
     * which never construct an [AppContainer] — `:core:test` runs against
     * [FakeEngineBridge] directly).
     *
     * The dev-settings toggle is read once, synchronously, here — a single
     * blocking DataStore read at app startup (same cost class as the
     * `Application.onCreate()` work already happening) rather than a live
     * swap, because [engineBridge] is handed out as a `val` to every
     * ViewModel via `container.engineBridge`; those references would go
     * stale if the bridge were swapped out from under them later. Toggling
     * the Settings switch therefore takes effect after an app restart —
     * the Settings screen says so.
     */
    val engineBridge: EngineBridge = run {
        val persisted = runBlocking { settingsRepository.settings.first() }
        // ROUND 7 (time-sync): the D6 transport latency is a property of this
        // phone's USB stack, so it is applied to the registry before any
        // connection can be opened rather than at connect time.
        d6UsbConnectionRegistry.setSensorLatencyMillis(persisted.d6SensorLatencyMs)
        val persistedUseFake = persisted.useFakeEngine
        val forceFake = BuildConfig.FORCE_FAKE_ENGINE || persistedUseFake
        val bridge: EngineBridge = if (!forceFake && ScanEngineNative.isAvailable) {
            RealEngineBridge(d6UsbConnectionRegistry, containerScope, livePageStoreSizing)
        } else {
            FakeEngineBridge()
        }
        EngineBridgeProvider.override(bridge)
        EngineBridgeProvider.get()
    }

    /**
     * ROUND 5.2: the phone's own location, used to georeference a capture when no
     * RTK rover is connected. Container-level because it is a device capability,
     * not a screen's — but it is **cold**: nothing touches the GPS until a capture
     * actually starts and the policy says no rover is present (see
     * `com.lidarscan.core.gnss.GeorefSourcePolicy`).
     */
    val phoneLocationSource = com.lidarscan.app.gnss.PhoneLocationSource(appContext)

    // ROUND 5: `D6ConnectController` is no longer constructed here — the standalone
    // connect wizard it drove is gone, and the Capture tab's own
    // `CaptureAutoConnectController` owns detect → connect → preview instead. The
    // class and its tests stay in `:core` (see android/NOTES.md's ROUND 5 section
    // for why it was kept rather than deleted).

    /**
     * B6/B11/B12: A15's job queue, A12's plan extractor and A13's merger, behind
     * one native handle. Device-level rather than per project: the queue has one
     * worker thread, and a 20-minute post-process must survive navigating away
     * from the screen that started it. The native handle itself is created
     * lazily on first submit — an app launch that never processes anything owns
     * no worker thread.
     */
    val processingRepository = ProcessingRepository(containerScope).also { repo ->
        // ROUND 22 item 90: a reprocess that throws now says so, in the same
        // log as the capture it belongs to.
        repo.onDiagnostic = { line ->
            captureLog.log(com.lidarscan.app.debug.CaptureLog.TAG_SEAL, line)
        }
    }

    /** B12: §3.10's georeferenced auto-merge, over the same native handle. */
    val mergeRepository = MergeRepository(processingRepository)

    /**
     * B9: the RTK rover link, the NTRIP client and the polled fix.
     * Engine-lifetime for exactly the reason `core/engine.h` gives for the
     * engine's own GNSS stack: the operator pairs the rover and waits for RTK
     * Fixed *before* pressing record, so this cannot belong to a session.
     */
    val rtkManager = RtkManager(appContext, containerScope)

    /**
     * B7: one ARCore session for the whole app. Both the Capture screen's AR
     * overlay and the mount-calibration wizard drive the *same* controller —
     * ARCore permits only one `Session` per process, and two screens each
     * creating their own is the classic way to get
     * `CameraNotAvailableException` when navigating between them.
     */
    val arController = CaptureArController(appContext).also { controller ->
        // ROUND 22 item 89: the AR gate's refusals land in the same log as the
        // capture narrative. Wired here rather than inside the controller so
        // the controller keeps knowing nothing about the logger.
        controller.attachDiagnosticSink { line ->
            captureLog.log(com.lidarscan.app.debug.CaptureLog.TAG_AR, line)
        }
    }

    /**
     * ROUND 11 (owner item 43): one cue player for the process, like the
     * controller above and for the same reason — it owns a `ToneGenerator`
     * (an AudioTrack allocation) and a vibrator handle, and building one per
     * Capture screen would put a 200 ms lag on the first cue of every capture.
     */
    val cuePlayer = com.lidarscan.app.capture.OperatorCuePlayer(appContext)

    /**
     * ROUND 13 (owner item 47): one Do Not Disturb guard for the process. It
     * holds the phone's pre-capture interruption filter, so it must outlive the
     * Capture ViewModel — a screen rebuilt mid-capture would otherwise lose the
     * value it has to restore.
     */
    val dndGuard = com.lidarscan.app.capture.DoNotDisturbGuard(appContext)

    /**
     * ROUND 9 (owner item 35): the phone's own gyro + accelerometer, feeding the
     * engine's IMU-densified pose interpolator. App-lifetime for the same reason
     * [arController] is — it holds a `SensorManager` and a `HandlerThread`, and
     * re-creating those per capture would churn a thread on every Start. It
     * registers listeners only between `start`/`stop`, so an idle app costs
     * nothing.
     */
    val phoneImuRecorder = com.lidarscan.app.ar.PhoneImuRecorder(appContext)

    /**
     * B7: the device-level, per-bracket calibration store (WIZARD.md §3 —
     * "Calibration belongs to the bracket, not the project"). Lives beside
     * the projects root rather than inside any project, keyed by
     * (phone model, bracket ID, lidar serial).
     */
    val mountCalibrationStore = FileMountCalibrationStore(
        File(projectsRootDir.parentFile ?: projectsRootDir, "mount_calibrations.json"),
    )

    fun hasCameraPermission(): Boolean =
        ContextCompat.checkSelfPermission(appContext, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED

    /** The ARCore install dance (see [com.lidarscan.app.ar.ArInstaller]); returns true when a session may be created now. */
    fun requestArInstall(activity: Activity): Boolean {
        val result = arInstaller.requestInstall(activity)
        arController.refreshAvailability()
        return result.getOrDefault(false)
    }

    private val arInstaller = ArInstaller()

    /**
     * The live capture engine's `scan_engine*`, or 0. Exposed so B7's wizard
     * can push mount extrinsics into whatever session is current without
     * holding a reference to the bridge implementation.
     */
    fun currentEngineHandle(): Long = (engineBridge as? RealEngineBridge)?.engineHandleOrZero() ?: 0L

    /** The live point-page source, or null — the wizard's lidar-side segmentation reads the board's returns through it. */
    fun currentPointCloudSource(): PointCloudSource? =
        (engineBridge as? NativePointCloudProvider)?.currentPointCloudSource()

    /**
     * B4: a fresh [ReplayEngineBridge] for the "Replay synthetic capture"
     * debug action — deliberately NOT [engineBridge]/[EngineBridgeProvider]
     * (those stay pointed at the real/fake live-capture path); each replay
     * session gets its own bridge instance + native `ReplayEngine` handle,
     * created on demand rather than at container-construction time.
     */
    fun newReplayEngineBridge(): ReplayEngineBridge = ReplayEngineBridge(appContext, scope = containerScope)
}
