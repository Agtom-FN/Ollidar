package com.lidarscan.app.di

import android.content.Context
import com.lidarscan.app.BuildConfig
import com.lidarscan.app.data.SettingsRepository
import com.lidarscan.core.engine.EngineBridge
import com.lidarscan.core.engine.EngineBridgeProvider
import com.lidarscan.core.store.FileProjectStore
import com.lidarscan.core.store.ProjectStore
import java.io.File

/**
 * Manual "DI-lite" composition root (no Hilt yet, per the B1 brief). One
 * instance is created in [com.lidarscan.app.LidarScanApplication] and handed
 * down to screens/ViewModels via `viewModel(factory = ...)` at each call
 * site. When B2/B4 introduce more dependencies (USB manager, CameraX, etc.)
 * they add fields here rather than each screen constructing its own.
 */
class AppContainer(context: Context) {

    private val appContext = context.applicationContext

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

    /**
     * The real engine bridge doesn't exist yet — [EngineBridgeProvider]
     * defaults to [com.lidarscan.core.engine.FakeEngineBridge]. B2/B3 call
     * `EngineBridgeProvider.override(...)` once at startup when A1's JNI
     * bridge is ready; nothing here needs to change.
     */
    val engineBridge: EngineBridge = EngineBridgeProvider.get()

    val settingsRepository: SettingsRepository = SettingsRepository(appContext)
}
