package com.lidarscan.app.data

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.lidarscan.core.capture.PerformancePreset
import com.lidarscan.core.gnss.NtripSettings
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
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
        val LAST_MID360_LIDAR_IP = stringPreferencesKey("last_mid360_lidar_ip")
        val LAST_MID360_HOST_IP = stringPreferencesKey("last_mid360_host_ip")
        val LAST_MID360_SN = stringPreferencesKey("last_mid360_serial_number")
        val SCAN_SERIES = intPreferencesKey("scan_series_counter")

        /** ROUND 7 (field bug 1): the D6 mount re-zero, so it outlives the Capture screen. */
        val MOUNT_TRIM = stringPreferencesKey("mount_trim")

        /** ROUND 7 (time-sync): the constant D6 transport latency, milliseconds. */
        val D6_SENSOR_LATENCY_MS = intPreferencesKey("d6_sensor_latency_ms")

        /** ROUND 9 (item 33): keep 0-point scans instead of pruning them. Default false. */
        val KEEP_EMPTY_SCANS = booleanPreferencesKey("keep_empty_scans")

        /** ROUND 11 (item 43): haptic + audio operator cues. Unset means ON. */
        val OPERATOR_CUES = booleanPreferencesKey("operator_cues_enabled")
        val DND_DURING_CAPTURE = booleanPreferencesKey("dnd_during_capture")
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
            lastDetectedMid360LidarIp = prefs[Keys.LAST_MID360_LIDAR_IP],
            lastDetectedMid360HostIp = prefs[Keys.LAST_MID360_HOST_IP],
            lastDetectedMid360SerialNumber = prefs[Keys.LAST_MID360_SN],
            scanSeriesCounter = prefs[Keys.SCAN_SERIES] ?: 0,
            d6SensorLatencyMs = prefs[Keys.D6_SENSOR_LATENCY_MS]
                ?: com.lidarscan.core.capture.D6TimeSync.DEFAULT_SENSOR_LATENCY_MS,
            // ROUND 9 (item 33): the default IS the fix — an unset preference
            // means empty scans are pruned.
            keepEmptyScans = prefs[Keys.KEEP_EMPTY_SCANS] ?: false,
            // ROUND 11 (item 43): default ON, so an unset preference buzzes.
            operatorCuesEnabled = prefs[Keys.OPERATOR_CUES] ?: true,
            dndDuringCapture = prefs[Keys.DND_DURING_CAPTURE] ?: true,
        )
    }

    /**
     * ROUND 5: claims the next scan series number, atomically.
     *
     * Read-modify-write **inside one `edit {}`** rather than "read the flow, add
     * one, write it back": DataStore serialises the transform, so two Starts
     * racing (a double-tap, or a Start while the previous project is still being
     * created) cannot both be handed the same number. Returns the number that was
     * just claimed, which is the one the project gets named with.
     */
    suspend fun nextScanSeries(): Int {
        var claimed = 1
        context.settingsDataStore.edit { prefs ->
            claimed = (prefs[Keys.SCAN_SERIES] ?: 0) + 1
            prefs[Keys.SCAN_SERIES] = claimed
        }
        return claimed
    }

    /**
     * ROUND 6 (owner item 22): the Light / Optimal / Full choice, **per device
     * profile**.
     *
     * Keyed by `<manufacturer>/<model>/<tier>` rather than stored as one global
     * value, because the whole point of the tiers is that "Full" means different
     * numbers on different hardware — restoring a preset chosen on a flagship
     * onto a modest phone would reintroduce exactly the "defaults are the
     * maximum" problem item 21 is about. A phone the app has not seen before
     * simply has no entry and starts on [com.lidarscan.core.capture.PerformancePresets.DEFAULT].
     *
     * A dynamic preference key rather than a field on [AppSettings]: the set of
     * device profiles is open-ended, and one row per phone this install has run
     * on is a handful of bytes.
     */
    suspend fun performancePreset(deviceProfileKey: String): PerformancePreset? {
        val stored = context.settingsDataStore.data.first()[presetKey(deviceProfileKey)] ?: return null
        return stored.toEnumOrNull<PerformancePreset>()?.takeIf { it.isSelectable }
    }

    suspend fun setPerformancePreset(deviceProfileKey: String, preset: PerformancePreset) {
        context.settingsDataStore.edit { it[presetKey(deviceProfileKey)] = preset.name }
    }

    private fun presetKey(deviceProfileKey: String) =
        stringPreferencesKey("perf_preset::$deviceProfileKey")

    /**
     * ROUND 7 (field bug 1): the mount re-zero, persisted.
     *
     * The trim used to live only in `CaptureViewModel`'s own
     * `MutableStateFlow`, i.e. in a `viewModel(key = "capture-new-false")`
     * scoped to the Capture tab's `NavBackStackEntry` — so a trip to the
     * Projects tab between two scans silently dropped 132° of mount rotation
     * out of the pushbroom, which is exactly what the owner's log shows
     * happening to `scan-009`. See
     * [com.lidarscan.core.calib.StoredMountTrim].
     *
     * Not per device profile (unlike the performance preset): there is one
     * bracket and one phone in the operator's hands, and a trim measured on it
     * is about the clamp, not the hardware class. Unparseable JSON — an older
     * shape, a truncated write — reads as "no trim" rather than throwing, so a
     * bad row can never keep the Capture tab from opening.
     */
    suspend fun storedMountTrim(): com.lidarscan.core.calib.StoredMountTrim? {
        val raw = context.settingsDataStore.data.first()[Keys.MOUNT_TRIM] ?: return null
        return runCatching {
            kotlinx.serialization.json.Json.decodeFromString<com.lidarscan.core.calib.StoredMountTrim>(raw)
        }.getOrNull()
    }

    /** Persists (or, with `null`, forgets) the mount re-zero. */
    suspend fun setStoredMountTrim(stored: com.lidarscan.core.calib.StoredMountTrim?) {
        context.settingsDataStore.edit { prefs ->
            if (stored == null) {
                prefs.remove(Keys.MOUNT_TRIM)
            } else {
                prefs[Keys.MOUNT_TRIM] = kotlinx.serialization.json.Json.encodeToString(
                    com.lidarscan.core.calib.StoredMountTrim.serializer(),
                    stored,
                )
            }
        }
    }

    /** ROUND 7: see [com.lidarscan.core.capture.D6TimeSync]. */
    suspend fun setD6SensorLatencyMs(millis: Int) {
        val clamped = com.lidarscan.core.capture.D6TimeSync.clampLatencyMs(millis)
        context.settingsDataStore.edit { it[Keys.D6_SENSOR_LATENCY_MS] = clamped }
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

    /** ROUND 9 (item 33): see [AppSettings.keepEmptyScans]. */
    suspend fun setKeepEmptyScans(keep: Boolean) {
        context.settingsDataStore.edit { it[Keys.KEEP_EMPTY_SCANS] = keep }
    }

    /** ROUND 11 (item 43). */
    suspend fun setOperatorCuesEnabled(enabled: Boolean) {
        context.settingsDataStore.edit { it[Keys.OPERATOR_CUES] = enabled }
    }

    suspend fun setDndDuringCapture(enabled: Boolean) {
        context.settingsDataStore.edit { it[Keys.DND_DURING_CAPTURE] = enabled }
    }

    suspend fun setAllowPoorSyncColorize(allow: Boolean) {
        context.settingsDataStore.edit { it[Keys.ALLOW_POOR_SYNC] = allow }
    }

    /** AUTO-DETECT: called once a Mid-360 heartbeat has actually been decoded — never from a manually-typed address. */
    suspend fun setLastDetectedMid360(lidarIp: String, hostIp: String, serialNumber: String) {
        context.settingsDataStore.edit {
            it[Keys.LAST_MID360_LIDAR_IP] = lidarIp
            it[Keys.LAST_MID360_HOST_IP] = hostIp
            it[Keys.LAST_MID360_SN] = serialNumber
        }
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
