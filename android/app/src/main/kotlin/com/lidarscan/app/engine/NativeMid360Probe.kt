package com.lidarscan.app.engine

/**
 * One snapshot of the Mid-360 connect wizard's transport probe
 * (`android/app/src/main/cpp/mid360_probe.{h,cpp}`).
 *
 * Constructed from JNI via a cached constructor whose descriptor is
 * `(IIJJJJJDDDJJJJJJIJZI)V` (resolved in `scanengine_jni.cpp`'s `JNI_OnLoad`,
 * marshalled in `mid360_jni.cpp`'s `nativeMid360ProbeSnapshot`) — keep the
 * field order below in lock-step with `Mid360ProbeSnapshot` in
 * `mid360_probe.h`.
 *
 * Two halves, and the split is diagnostic:
 *
 *  * [deviceState] … [tLastDataNs] come from `Engine::device_health()`, i.e.
 *    the same `DeviceHealth` the C ABI's `scan_engine_device_health` exposes,
 *    with A3 §5's Mid-360 aliases already undone: the driver packs the IMU
 *    rate into `rotation_hz` and `1 − loss` into `checksum_pass_rate` so that
 *    "a D6 and a Mid-360 mean the same thing on the same dial", and this
 *    class un-packs them into [imuHz] and [lossPct].
 *  * [datagramsPoint] … [tLastDatagramNs] come from the probe's own
 *    `raw_sink`, which fires per UDP datagram BEFORE any parsing. That is the
 *    only signal available when bytes are arriving but nothing decodes, and
 *    the difference between "no datagrams at all" (cabling / addressing /
 *    the device is not streaming here) and "datagrams but no points" (wrong
 *    port, or a filter dropping everything) is the difference between two
 *    completely different things to tell the user.
 */
data class NativeMid360Probe(
    /** `scanengine::DeviceState` ordinal — see [ScanEngineNative.DeviceState]. */
    val deviceState: Int,
    /** `scanengine::ScanError` — see [ScanEngineNative.ErrorCode]. */
    val lastError: Int,
    val packetsOk: Long,
    /** Bad parses **plus** losses inferred from the free-running `udp_cnt` model. */
    val packetsBad: Long,
    /** Points that reached the PageStore: post-filter, post-decimation. */
    val pointsOut: Long,
    val drops: Long,
    val bytesIn: Long,
    val pointsPerSec: Double,
    /** `DeviceHealth.rotation_hz` — the Mid-360 has no revolutions; this is the IMU rate. Nominal 200.00. */
    val imuHz: Double,
    /** `100 * (1 - checksum_pass_rate)` — **lifetime** loss, not the current window. See the note below. */
    val lossPct: Double,
    val tLastDataNs: Long,
    val datagramsPoint: Long,
    val datagramsImu: Long,
    val datagramBytes: Long,
    val tFirstDatagramNs: Long,
    val tLastDatagramNs: Long,
    /** `scanengine::Mid360LinkState` ordinal — see [Mid360LinkState]. */
    val linkState: Int,
    val elapsedSinceStartNs: Long,
    val running: Boolean,
    /** `scanengine::Mid360Backend` ordinal — 0 = SDK2, 1 = raw UDP. */
    val backend: Int,
) {
    val link: Mid360LinkState get() = Mid360LinkState.fromOrdinal(linkState)

    /**
     * True when datagrams are arriving but nothing has reached the PageStore
     * — the "reachable but misconfigured" state, which no single engine
     * counter reports on its own.
     */
    val bytesButNoPoints: Boolean get() = datagramsPoint > 0 && pointsOut == 0L
}

/**
 * Mirror of `scanengine::Mid360LinkState` (`mid360_driver.h`), ordinal for
 * ordinal.
 *
 * This is **derived app-side**, not read from the driver: `Engine` exposes no
 * concrete-driver accessor, so `Mid360Stats::link` — along with the watchdog
 * trip count, the forced-re-init count and the per-window loss — is not
 * reachable through any API the app has (desktop's NOTES §8.3 records the
 * same constraint). `Mid360Probe::snapshot()` re-derives the state from
 * wall-clock silence using A3 §5's own thresholds (`data_timeout_ms` = 1 s,
 * `reinit_after_silence_ms` = 5 s) applied to `t_last_data_ns`, which is the
 * same observable and the same rule the driver applies internally.
 *
 * Wall clock and not the packet counter, because S2 measured **0 counted
 * losses across three separate 15-second cable pulls**: the device's counter
 * keeps advancing while the wire is down, so "0.0% loss" and "the cable is
 * out" are the same reading on `udp_cnt`. Silence is the only honest outage
 * signal.
 */
enum class Mid360LinkState(val label: String, val detail: String) {
    DOWN("Down", "Not started, or stopped."),
    WAITING("Waiting", "Started — no packet has arrived yet (discovery + handshake + config push)."),
    UP("Up", "Data is arriving within the 1 s watchdog window."),
    SILENT("Silent", "No data for over 1 s. A cable pull looks exactly like this."),
    REINITIALIZING("Re-initialising", "Silent for over 5 s — the driver is forcing a full SDK teardown and re-init."),
    ;

    companion object {
        fun fromOrdinal(value: Int): Mid360LinkState =
            entries.getOrElse(value) { DOWN }
    }
}
