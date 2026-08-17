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
    /**
     * **Dark by default, not System.**
     *
     * The redesign is a dark cockpit — an instrument UI shown in its own
     * world, with one ember accent that only reads against a near-black
     * ground (docs/design/lidarscan-interfaces.html says so in its first
     * comment, and every approved screenshot is dark). Defaulting to SYSTEM
     * meant a phone in light mode opened the app into the *derived* light
     * palette, which is the fallback, not the design.
     *
     * All three options still work and Light is still a faithful inversion of
     * the same tokens — this changes which one a fresh install starts on.
     */
    val themeMode: ThemeMode = ThemeMode.DARK,
    /**
     * B2 dev-settings flag: force [com.lidarscan.core.engine.FakeEngineBridge]
     * even when the real JNI bridge's native lib loaded successfully — lets a
     * developer without a D6 attached still exercise Capture/connect UI.
     * [com.lidarscan.app.di.AppContainer] applies this on top of its
     * BuildConfig-driven default (see `BuildConfig.FORCE_FAKE_ENGINE`).
     */
    val useFakeEngine: Boolean = false,
    /**
     * D3: where the Cloud processing mode uploads to (§3.8's "Cloud" row) and
     * the single-tenant bearer token the service requires on every request.
     *
     * Empty [cloudBaseUrl] is what gates the Cloud action off; the Processing
     * screen says "set the server URL and token in Settings" rather than
     * offering an upload that would 401.
     */
    val cloudBaseUrl: String = "",
    val cloudToken: String = "",
    /** B9: the NTRIP caster, device-level rather than per project — one account, many sites. */
    val ntrip: com.lidarscan.core.gnss.NtripSettings = com.lidarscan.core.gnss.NtripSettings(),
    /**
     * B6: the operator override behind A11's `SCAN_SYNC_POOR` refusal. Off by
     * default and it stays a *setting* rather than a per-run checkbox because
     * turning it on is a statement about what the results may be used for, not
     * a per-job choice.
     */
    val allowPoorSyncColorize: Boolean = false,
    /**
     * AUTO-DETECT (capture defaults): the addresses of the last Mid-360
     * successfully identified by the connect wizard's auto-detect step —
     * device-level, not per-project, matching how [ntrip] and
     * [useFakeEngine] are scoped. A fresh Mid-360 connect wizard (no
     * per-project `manifest.json` value yet — see
     * `com.lidarscan.app.ui.connect.Mid360ConnectViewModel`) prefers these
     * over the bare `192.168.1.100`/`192.168.1.5` factory-default constants
     * in [com.lidarscan.core.net.Mid360Settings] whenever they are set: a
     * rig that has been auto-detected once is far more likely to still be
     * wired the same way than to match the factory default.
     */
    val lastDetectedMid360LidarIp: String? = null,
    val lastDetectedMid360HostIp: String? = null,
    val lastDetectedMid360SerialNumber: String? = null,
)
