package com.lidarscan.core.net

/**
 * The pre-capture self-test's verdict logic (Tech Spec §3.1's Android row:
 * "static-IP wizard + per-OEM guidance + **pre-capture self-test**").
 *
 * Pure: it takes a sample of what the engine reports and returns what to
 * show. No coroutines, no Android, no engine handle — which is what makes it
 * unit-testable with no emulator and no hardware, and which is the only part
 * of the Mid-360 path that *can* be tested here at all (everything else is
 * the network).
 *
 * ## The rule, and why it is "first point", not "a rate"
 *
 * Desktop C2's Mid-360 self-test passes on `points_out > baseline` — the
 * **first** point, not a sustained rate — while its D6 self-test requires
 * ~3,000 pts/s over 3 s. That asymmetry is right and is preserved here: the
 * D6's failure mode is a bad serial link that decodes *some* bytes, so a rate
 * is the discriminator; the Mid-360's failure mode is total silence (wrong
 * host IP, wrong subnet, no link, device not configured for this host), and
 * against silence the first packet is the entire question. A3's own numbers
 * back this: handshake to first packet is **1.45 s** on the S2 loopback rig
 * and 1.59–1.63 s measured on desktop, and once a Mid-360 is streaming it
 * streams at 200,000 pts/s — there is no partial-credit regime to measure.
 *
 * ## Why the window is 8 s when the engine's own grace is 10 s
 *
 * [WINDOW_MS] is 8 s, deliberately shorter than
 * `Mid360ReconnectConfig::connect_timeout_ms` (10 s), matching desktop C2
 * exactly. The engine's timer is a different, longer-horizon thing: it is
 * what decides whether the *watchdog* forces an SDK re-init. The UI's job is
 * to stop asking the user to wait once the answer is clear, and 8 s is ~5x
 * the measured handshake time.
 *
 * ## Rates are quoted, never gated on
 *
 * A pass reports pts/s, IMU Hz and loss % because those are what tell an
 * operator whether the link is healthy — but the pass/fail gate is the first
 * point alone. Gating on 200,000 pts/s would fail a perfectly good link that
 * happens to be looking at a close wall, and gating on 200 Hz IMU would fail
 * the pre-bound-socket path, which cannot carry IMU at all (see
 * android/NOTES.md's B3 section).
 */
object Mid360SelfTest {
    /** Matches desktop C2's Mid-360 window exactly. See the class doc for why it is not 10 s. */
    const val WINDOW_MS = 8_000L

    /** A3 §7's soak figures, quoted in the readout as the reference a healthy link sits at. */
    const val NOMINAL_SENSOR_POINTS_PER_SEC = 200_000.0
    const val NOMINAL_STORE_POINTS_PER_SEC = 40_000.0
    const val NOMINAL_IMU_HZ = 200.0

    /**
     * `Mid360Config::max_loss_pct` — the level at which the driver itself
     * demotes to `DeviceState::kDegraded` (1% is ~20 dropped packets a
     * second, "well past anything attributable to scheduling, and the level
     * at which a voxel map starts thinning").
     */
    const val LOSS_DEGRADED_PCT = 1.0

    /** One observation of the engine, reduced to what the verdict needs. */
    data class Sample(
        val elapsedMs: Long,
        /** `DeviceHealth.points_out` — post-filter, post-decimation, into the PageStore. */
        val pointsOut: Long,
        /** Baseline captured at test start, so a re-test on a still-streaming device is honest. */
        val baselinePointsOut: Long,
        /** Pre-parse datagram count from the probe's `raw_sink` (0 when unavailable, e.g. the C-ABI path). */
        val pointDatagrams: Long = 0,
        val pointsPerSec: Double = 0.0,
        val imuHz: Double = 0.0,
        val lossPct: Double = 0.0,
        /** `scanengine::DeviceState` ordinal; used only for the failure detail line. */
        val deviceState: Int = 0,
        val deviceStateLabel: String = "unknown",
        /** True on the pre-bound-socket path, where IMU is structurally unavailable. */
        val imuUnavailable: Boolean = false,
    ) {
        val pointsGained: Long get() = (pointsOut - baselinePointsOut).coerceAtLeast(0L)
    }

    sealed interface Verdict {
        /** Still inside the window with no points yet. */
        data class Testing(val elapsedMs: Long, val progress: Float, val message: String) : Verdict

        data class Passed(val elapsedMs: Long, val detail: String) : Verdict

        data class Failed(val elapsedMs: Long, val detail: String, val diagnosis: String) : Verdict
    }

    fun evaluate(sample: Sample): Verdict {
        val elapsed = sample.elapsedMs.coerceAtLeast(0L)
        val seconds = elapsed / 1000.0

        if (sample.pointsGained > 0) {
            val parts = mutableListOf(
                "first packet after %.2f s".format(seconds),
                "%,.0f pts/s".format(sample.pointsPerSec),
            )
            parts += if (sample.imuUnavailable) {
                "IMU off (pre-bound socket)"
            } else {
                "%.2f Hz IMU".format(sample.imuHz)
            }
            parts += "%.3f%% loss".format(sample.lossPct)
            return Verdict.Passed(elapsed, parts.joinToString(" · "))
        }

        if (elapsed < WINDOW_MS) {
            return Verdict.Testing(
                elapsedMs = elapsed,
                progress = (elapsed.toFloat() / WINDOW_MS).coerceIn(0f, 1f),
                message = "Waiting for the first Mid-360 packet (%.1f / %d s)".format(seconds, WINDOW_MS / 1000),
            )
        }

        val detail = "no packet within %d s (device state: %s)".format(WINDOW_MS / 1000, sample.deviceStateLabel)
        return Verdict.Failed(elapsed, detail, diagnose(sample))
    }

    /**
     * Turns "nothing arrived" into the most specific thing the numbers
     * support. The ordering matters: datagrams-but-no-points is a *different*
     * fault from no-datagrams-at-all, and saying "check the cable" to someone
     * whose cable is fine and whose point port is wrong wastes the bench
     * session A3 §8 says is the only way to close the remaining risk.
     */
    fun diagnose(sample: Sample): String = when {
        sample.pointDatagrams > 0 ->
            "Datagrams ARE arriving (${sample.pointDatagrams}) but none decoded into points. " +
                "The wire and the addressing are fine; the point port or the packet format is not. " +
                "Check the device point port against the lidar's own configuration."

        sample.deviceState == DEVICE_STATE_FAULT ->
            "The driver reported a fault. Read the error beside it — an SDK init failure here usually " +
                "means the host IP is not an address this phone holds, or one of the host ports is already bound."

        else ->
            "No UDP datagram arrived at all. In order of likelihood: the host IP is not the address the " +
                "device was told to stream to; the Ethernet adapter is not actually up; the lidar IP is wrong; " +
                "or the device has no power (9–27 V, ~6.5 W — USB-C cannot supply it)."
    }

    /**
     * Health-line text for a streaming device, in desktop C2's shape and
     * order so the two apps read the same. C2 shows loss inverted as an "ok
     * rate"; this shows both, because the wizard has the room and because
     * "0.4% loss" is the number A3's tables are written in.
     */
    fun healthLine(
        stateLabel: String,
        pointsPerSec: Double,
        imuHz: Double,
        lossPct: Double,
        pointsTotal: Long,
        drops: Long,
        imuUnavailable: Boolean = false,
    ): String {
        val imu = if (imuUnavailable) "IMU n/a" else "%.2f Hz IMU".format(imuHz)
        return "%s · %,.0f pts/s · %s · %.3f%% loss (%.1f%% ok) · %,d pts · %,d drops".format(
            stateLabel, pointsPerSec, imu, lossPct, 100.0 - lossPct, pointsTotal, drops,
        )
    }

    /** `scanengine::DeviceState::kFault`. */
    const val DEVICE_STATE_FAULT = 6
    const val DEVICE_STATE_DEGRADED = 4
    const val DEVICE_STATE_STREAMING = 3
}
