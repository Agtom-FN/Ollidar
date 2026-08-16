package com.lidarscan.app.data

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.lidarscan.core.gnss.NtripSettings
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.settingsDataStore by preferencesDataStore(name = "settings")

/**
 * Persists the Settings screen's preferences via Preferences DataStore. Lives
 * in `:app` (not `:core`) because DataStore-preferences is an Android-only
 * dependency and `:core` is kept plain-JVM/testable without a device — see the
 * `:core` module's build.gradle.kts.
 *
 * **The cloud token is stored in plain preferences, and that is a stated
 * limitation, not an oversight.** The MVP service is single-tenant with one
 * owner token behind a reverse proxy (§3.8), the app's DataStore lives in
 * app-private storage, and the alternative worth having is the Android
 * Keystore — which is a real change (key generation, `EncryptedFile` or
 * `EncryptedSharedPreferences`, a migration for anyone who already saved one)
 * rather than a line. It is on the follow-up list in NOTES.md rather than
 * pretended away here.
 */
class SettingsRepository(private val context: Context) {

    private object Keys {
        val UNITS = stringPreferencesKey("units")
        val THEME_MODE = stringPreferencesKey("theme_mode")
        val USE_FAKE_ENGINE = booleanPreferencesKey("use_fake_engine")
        val CLOUD_BASE_URL = stringPreferencesKey("cloud_base_url")
        val CLOUD_TOKEN = stringPreferencesKey("cloud_token")
        val ALLOW_POOR_SYNC = booleanPreferencesKey("allow_poor_sync_colorize")
        val NTRIP_HOST = stringPreferencesKey("ntrip_host")
        val NTRIP_PORT = intPreferencesKey("ntrip_port")
        val NTRIP_MOUNT = stringPreferencesKey("ntrip_mountpoint")
        val NTRIP_USER = stringPreferencesKey("ntrip_username")
        val NTRIP_PASS = stringPreferencesKey("ntrip_password")
        val NTRIP_VERSION = intPreferencesKey("ntrip_version")
        val NTRIP_V1_FALLBACK = booleanPreferencesKey("ntrip_allow_v1_fallback")
        val NTRIP_GGA_INTERVAL_MS = intPreferencesKey("ntrip_gga_interval_ms")
        val NTRIP_AUTO_RECONNECT = booleanPreferencesKey("ntrip_auto_reconnect")
    }

    val settings: Flow<AppSettings> = context.settingsDataStore.data.map { prefs ->
        AppSettings(
            units = prefs[Keys.UNITS]?.toEnumOrNull<Units>() ?: Units.METERS,
            // Dark, not System — see AppSettings.themeMode for why. This is the
            // default that actually decides a fresh install, since the flow's
            // value replaces AppSettings()'s the moment DataStore emits.
            themeMode = prefs[Keys.THEME_MODE]?.toEnumOrNull<ThemeMode>() ?: ThemeMode.DARK,
            useFakeEngine = prefs[Keys.USE_FAKE_ENGINE] ?: false,
            cloudBaseUrl = prefs[Keys.CLOUD_BASE_URL].orEmpty(),
            cloudToken = prefs[Keys.CLOUD_TOKEN].orEmpty(),
            allowPoorSyncColorize = prefs[Keys.ALLOW_POOR_SYNC] ?: false,
            ntrip = NtripSettings(
                host = prefs[Keys.NTRIP_HOST].orEmpty(),
                port = prefs[Keys.NTRIP_PORT] ?: 2101,
                mountpoint = prefs[Keys.NTRIP_MOUNT].orEmpty(),
                username = prefs[Keys.NTRIP_USER].orEmpty(),
                password = prefs[Keys.NTRIP_PASS].orEmpty(),
                ntripVersion = prefs[Keys.NTRIP_VERSION] ?: 0,
                allowV1Fallback = prefs[Keys.NTRIP_V1_FALLBACK] ?: true,
                ggaIntervalMs = prefs[Keys.NTRIP_GGA_INTERVAL_MS] ?: 10_000,
                autoReconnect = prefs[Keys.NTRIP_AUTO_RECONNECT] ?: true,
            ),
        )
    }

    suspend fun setUnits(units: Units) {
        context.settingsDataStore.edit { it[Keys.UNITS] = units.name }
    }

    suspend fun setThemeMode(themeMode: ThemeMode) {
        context.settingsDataStore.edit { it[Keys.THEME_MODE] = themeMode.name }
    }

    suspend fun setUseFakeEngine(useFakeEngine: Boolean) {
        context.settingsDataStore.edit { it[Keys.USE_FAKE_ENGINE] = useFakeEngine }
    }

    suspend fun setCloud(baseUrl: String, token: String) {
        context.settingsDataStore.edit {
            it[Keys.CLOUD_BASE_URL] = baseUrl.trim().trimEnd('/')
            it[Keys.CLOUD_TOKEN] = token.trim()
        }
    }

    suspend fun setAllowPoorSyncColorize(allow: Boolean) {
        context.settingsDataStore.edit { it[Keys.ALLOW_POOR_SYNC] = allow }
    }

    suspend fun setNtrip(s: NtripSettings) {
        context.settingsDataStore.edit {
            it[Keys.NTRIP_HOST] = s.host.trim()
            it[Keys.NTRIP_PORT] = s.port
            it[Keys.NTRIP_MOUNT] = s.mountpoint.trim()
            it[Keys.NTRIP_USER] = s.username
            it[Keys.NTRIP_PASS] = s.password
            it[Keys.NTRIP_VERSION] = s.ntripVersion
            it[Keys.NTRIP_V1_FALLBACK] = s.allowV1Fallback
            it[Keys.NTRIP_GGA_INTERVAL_MS] = s.ggaIntervalMs
            it[Keys.NTRIP_AUTO_RECONNECT] = s.autoReconnect
        }
    }
}

private inline fun <reified T : Enum<T>> String.toEnumOrNull(): T? =
    runCatching { enumValueOf<T>(this) }.getOrNull()
