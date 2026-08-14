package com.lidarscan.app.data

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.settingsDataStore by preferencesDataStore(name = "settings")

/**
 * Persists the Settings screen's units/theme preferences via Preferences
 * DataStore. Lives in `:app` (not `:core`) because DataStore-preferences is
 * an Android-only dependency and `:core` is kept plain-JVM/testable without
 * a device — see the `:core` module's build.gradle.kts.
 */
class SettingsRepository(private val context: Context) {

    private object Keys {
        val UNITS = stringPreferencesKey("units")
        val THEME_MODE = stringPreferencesKey("theme_mode")
    }

    val settings: Flow<AppSettings> = context.settingsDataStore.data.map { prefs ->
        AppSettings(
            units = prefs[Keys.UNITS]?.toEnumOrNull<Units>() ?: Units.METERS,
            themeMode = prefs[Keys.THEME_MODE]?.toEnumOrNull<ThemeMode>() ?: ThemeMode.SYSTEM,
        )
    }

    suspend fun setUnits(units: Units) {
        context.settingsDataStore.edit { it[Keys.UNITS] = units.name }
    }

    suspend fun setThemeMode(themeMode: ThemeMode) {
        context.settingsDataStore.edit { it[Keys.THEME_MODE] = themeMode.name }
    }
}

private inline fun <reified T : Enum<T>> String.toEnumOrNull(): T? =
    runCatching { enumValueOf<T>(this) }.getOrNull()
