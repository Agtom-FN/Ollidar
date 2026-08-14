package com.lidarscan.app.data

/** Distance unit shown throughout the app (measure tool, display params — B10/B11 land these later). */
enum class Units(val displayName: String, val abbreviation: String) {
    METERS(displayName = "Meters", abbreviation = "m"),
    FEET(displayName = "Feet", abbreviation = "ft"),
}

/** Theme preference. [SYSTEM] follows the device's light/dark setting. */
enum class ThemeMode(val displayName: String) {
    SYSTEM("System"),
    LIGHT("Light"),
    DARK("Dark"),
}

data class AppSettings(
    val units: Units = Units.METERS,
    val themeMode: ThemeMode = ThemeMode.SYSTEM,
    /**
     * B2 dev-settings flag: force [com.lidarscan.core.engine.FakeEngineBridge]
     * even when the real JNI bridge's native lib loaded successfully — lets a
     * developer without a D6 attached still exercise Capture/connect UI.
     * [com.lidarscan.app.di.AppContainer] applies this on top of its
     * BuildConfig-driven default (see `BuildConfig.FORCE_FAKE_ENGINE`).
     */
    val useFakeEngine: Boolean = false,
)
