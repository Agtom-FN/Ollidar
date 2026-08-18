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
    /**
     * ROUND 5 (item 9): how many scans this device has auto-named so far.
     *
     * The Capture tab's Start always creates a NEW project, and an un-named one
     * is called `Scan-<series>-<date>-<time>` (see
     * [com.lidarscan.core.capture.ScanAutoName]). The counter is **device-level
     * and monotonic** — deliberately not "number of projects currently on disk",
     * because deleting a project must not make the next scan re-use its number:
     * two `.lscan` directories called `Scan-014-…` on the same phone, taken
     * weeks apart, is exactly the confusion the series exists to prevent.
     *
     * Incremented by [SettingsRepository.nextScanSeries] at the moment a project
     * is created, so a series number is only ever spent on a project that
     * actually exists.
     */
    val scanSeriesCounter: Int = 0,
    /**
     * ROUND 7 (time-sync): the constant transport delay subtracted from every
     * D6 byte-chunk arrival stamp, in milliseconds.
     *
     * Device-level, like [ntrip] and [useFakeEngine]: it describes a phone's USB
     * stack and a cable, not a project. See
     * [com.lidarscan.core.capture.D6TimeSync] for the whole derivation,
     * including why the default is 2 and why it is a setting at all.
     */
    val d6SensorLatencyMs: Int = com.lidarscan.core.capture.D6TimeSync.DEFAULT_SENSOR_LATENCY_MS,
    /**
     * ROUND 9 (owner item 33): keep a scan that recorded **zero points**?
     *
     * **Off by default, i.e. empty scans do not survive.** A capture that
     * received no sensor packets leaves a `.lscan` directory with a
     * `project.json`, an empty `streams/` tree and nothing else — and the
     * owner's phone had accumulated a column of them (`scan-012`, `scan-014`,
     * …), each one indistinguishable in the list from a real scan until you
     * open it. Stop now deletes such a project instead of keeping it, and the
     * Projects list hides the legacy ones that are already on disk (Settings has
     * a one-tap "clean up" that deletes those for good).
     *
     * Turning it ON restores ROUND 7's behaviour — "the project was saved so the
     * evidence is not lost" — which is the right setting while diagnosing a
     * sensor that produces nothing, because then the empty `.lscan` *is* the
     * evidence: it carries the manifest, the mount trim and the timestamps of an
     * attempt that failed.
     *
     * Never applies to a replay session or to a capture recorded into a project
     * that already existed before Start (the deep-link route): deleting either
     * would be deleting something this capture did not create.
     */
    val keepEmptyScans: Boolean = false,
    /**
     * ROUND 11 (owner item 43): haptic + audio operator cues, **default ON**.
     *
     * The phone is the sensor mount: on this rig it faces sideways at hip
     * height while the operator walks and looks where they are going, so every
     * on-screen hint the app has added since ROUND 5.3 is written for a reader
     * who is not there. Default ON because a cue the operator has to go and
     * find is a cue that is off during the one capture that needed it; the
     * switch exists because a scan in a quiet building is a real reason to
     * silence the tone.
     */
    val operatorCuesEnabled: Boolean = true,

    /**
     * ROUND 13 (owner item 47): silence notifications while a capture runs.
     *
     * A notification is not a distraction on this rig, it is a MEASUREMENT
     * ERROR — the buzz fires the vibration motor, which shakes the IMU and
     * smears the camera for the ~100-300 ms it lasts, at a phone that is being
     * used as an inertial sensor. Default ON, because the operator is walking
     * and cannot dismiss anything. Requires Do Not Disturb access, which is a
     * Settings grant; without it the capture runs unprotected and says so
     * rather than refusing.
     */
    val dndDuringCapture: Boolean = true,
)
