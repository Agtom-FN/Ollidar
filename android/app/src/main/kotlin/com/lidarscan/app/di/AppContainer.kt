package com.lidarscan.app.di

import android.content.Context
import com.lidarscan.app.BuildConfig
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.app.engine.RealEngineBridge
import com.lidarscan.app.engine.ScanEngineNative
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
}
