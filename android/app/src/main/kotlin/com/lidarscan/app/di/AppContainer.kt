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
import com.lidarscan.app.processing.ProcessingRepository
import com.lidarscan.app.rtk.RtkManager
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.engine.D6ConnectController
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

    private val containerScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /**
     * `.lscan` projects live under app-specific external storage: no runtime
     * permission needed (scoped storage), plenty of room for multi-GB
     * captures, and it survives app updates. Falls back to internal storage
     * if external isn't mounted. Shown read-only on the Settings screen; a
     * user-facing location picker (SAF) is future work, not B1 scope.
     */
    val projectsRootDir: File = File(appContext.getExternalFilesDir(null) ?: appContext.filesDir, "Projects")

    val projectStore: ProjectStore = FileProjectStore(
        rootDir = projectsRootDir,
        appVersion = BuildConfig.VERSION_NAME,
    )

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

    val settingsRepository: SettingsRepository = SettingsRepository(appContext)

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
        val persistedUseFake = runBlocking { settingsRepository.settings.first().useFakeEngine }
        val forceFake = BuildConfig.FORCE_FAKE_ENGINE || persistedUseFake
        val bridge: EngineBridge = if (!forceFake && ScanEngineNative.isAvailable) {
            RealEngineBridge(d6UsbConnectionRegistry, containerScope)
        } else {
            FakeEngineBridge()
        }
        EngineBridgeProvider.override(bridge)
        EngineBridgeProvider.get()
    }

    /** Pure-Kotlin D6 connect-wizard state machine (`:core`), driving [engineBridge]. */
    val d6ConnectController = D6ConnectController(engineBridge, containerScope)

    /**
     * B6/B11/B12: A15's job queue, A12's plan extractor and A13's merger, behind
     * one native handle. Device-level rather than per project: the queue has one
     * worker thread, and a 20-minute post-process must survive navigating away
     * from the screen that started it. The native handle itself is created
     * lazily on first submit — an app launch that never processes anything owns
     * no worker thread.
     */
    val processingRepository = ProcessingRepository(containerScope)

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
    val arController = CaptureArController(appContext)

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
