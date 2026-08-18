package com.lidarscan.core.capture

/**
 * ROUND 7 — **the D6's clock, and the one number in it that cannot be derived.**
 *
 * ## The chain, and why there is no clock-domain bug in it
 *
 * Three clocks could have disagreed on the D6 path, and none of them do:
 *
 *  * **ARCore poses.** `Frame.getTimestamp()` is nanoseconds in the same base
 *    as `SystemClock.elapsedRealtimeNanos()`, i.e. `CLOCK_BOOTTIME`.
 *  * **The engine.** `engine/include/scanengine/timesync/clock.h` calls
 *    `clock_gettime(CLOCK_BOOTTIME)` directly on Android *specifically* because
 *    bionic backs `steady_clock` with `CLOCK_MONOTONIC`, which stops during
 *    suspend. Same domain, no conversion, and a capture that survives the
 *    screen going off does not shear.
 *  * **The D6 itself.** It has none. It is a UART that emits framed packets
 *    with no device timestamp, which is why
 *    `TimeSync::stream_has_device_clock(kLidarD6)` returns **false** and A4's
 *    min-delay estimator is deliberately not on this path — there is no device
 *    clock to estimate an offset against. (A4 *is* wired for the Mid-360, the
 *    IMU and GNSS, which do have one.) Arrival time is the only time the D6
 *    has, so arrival time is what has to be right.
 *
 * ## What was wrong, and what is left
 *
 * The **variable** error was large and is now removed exactly. A phone reads
 * the D6 in chunks of up to 4096 bytes, and 4096 bytes at 230400 8N1 is 178 ms
 * of wire time; every return in that chunk used to claim the instant the chunk
 * was handed over. ROUND 7 back-dates each return from its own byte position at
 * the known baud (`D6Config::time_slice_bytes` in the engine), which is exact:
 * a UART delivers bytes at a constant rate, so byte position *is* time.
 *
 * What survives that is a genuinely **constant** delay — the microseconds
 * between a byte reaching the CH340's FIFO and this app reading the clock — and
 * a constant delay is not harmless. It translates the whole cloud along the
 * walk (at 1 m/s, 20 ms is 2 cm) and, because the *rotation* is also read late,
 * it bends corners in proportion to how fast the operator is turning. It is
 * also the one term that cannot be derived from first principles here, so it is
 * a setting.
 *
 * ## How [DEFAULT_SENSOR_LATENCY_MS] was chosen
 *
 * By construction, not by measurement — there is no D6 in this environment, and
 * saying otherwise would be inventing a number. Two terms are knowable:
 *
 *  1. **USB polling.** The CH340 is a full-speed bulk device; the host schedules
 *    bulk transfers on 1 ms frames, so the last byte of a chunk waits on average
 *    half a frame and at most one before the transfer completes. ~0.5–1 ms.
 *  2. **One thread wake-up.** `UsbDeviceConnection.bulkTransfer` returning to
 *    the reader thread, which then samples `elapsedRealtimeNanos()` as its very
 *    first act (`D6SerialConnection`). ~0.5–1 ms on a warm, non-throttled phone.
 *
 * ## ROUND 10: it was finally MEASURED, and 2 ms was about right
 *
 * The owner's `scan-020` carries the D6 stream, the ARCore poses AND 80,661
 * phone-IMU samples at 399.1 Hz in one container, which makes both clock
 * crossings measurable after the fact:
 *
 *  * **phone IMU vs ARCore pose: −1.5 ms** (r = 0.982), by cross-correlating
 *    the recorded gyro's angular rate against the rate implied by the poses
 *    over all 5,961 pose intervals. The CLOCK_BOOTTIME claim above is not just
 *    documented, it is true on the hardware.
 *  * **D6 vs ARCore pose: +4 ms**, by re-resolving the whole capture at a
 *    sweep of offsets and taking the crispest map
 *    (`engine_cli --d6-timesweep`). The entire ±30 ms window varies by 0.1 %,
 *    so the measurement's honest reading is "under 10 ms, sign uncertain".
 *
 * Either way it is millimetres at walking pace against the 4.8 cm of wall
 * thickness that capture actually has, so **this is not what makes a scan
 * shift when the operator turns around** — see `android/NOTES.md` ROUND 10 §1.
 * [DEFAULT_SENSOR_LATENCY_MS] is left at 2 because the data cannot tell 2 from
 * 0 from 6, and inventing precision here would be the same mistake in the
 * opposite direction.
 *
 * So **2 ms**, which at 1 m/s is 2 mm — an order of magnitude under the D6's own
 * range noise, and therefore not a number worth arguing about. It is exposed
 * anyway, because the derivation above assumes a healthy USB stack and a phone
 * that is not thermally throttled, and one afternoon with a plumb line and a
 * corridor beats any amount of reasoning. The range is generous in both
 * directions ([MIN_SENSOR_LATENCY_MS] is negative on purpose: if ARCore's own
 * pose timestamps turn out to lag *their* exposure, the correction is the other
 * way).
 */
object D6TimeSync {

    /** See the class header for the derivation. Milliseconds subtracted from each chunk's arrival stamp. */
    const val DEFAULT_SENSOR_LATENCY_MS = 2

    /** Negative is legal: the correction can go either way (see the header). */
    const val MIN_SENSOR_LATENCY_MS = -50

    const val MAX_SENSOR_LATENCY_MS = 50

    fun clampLatencyMs(millis: Int): Int =
        millis.coerceIn(MIN_SENSOR_LATENCY_MS, MAX_SENSOR_LATENCY_MS)

    /**
     * How far a constant latency of [millis] displaces the cloud along the walk,
     * in metres, at [speedMps]. This is what the settings row prints beside the
     * number, because "2 ms" means nothing and "2 mm at walking pace" means
     * everything.
     */
    fun displacementMetres(millis: Int, speedMps: Double = 1.0): Double =
        millis / 1000.0 * speedMps

    /** The settings row's own sentence. */
    fun describe(millis: Int): String {
        val cm = displacementMetres(millis) * 100.0
        return "%d ms — shifts the cloud %.1f cm along a 1 m/s walk".format(millis, cm)
    }

    /**
     * The wire duration of `bytes` at `baud`, 8N1, in nanoseconds — the span the
     * engine's per-byte back-dating removes, and the smear the app used to ship.
     * Exposed so the capture log can state it rather than the reader guessing.
     */
    fun chunkWireNanos(bytes: Int, baud: Int = D6_BAUD, bitsPerByte: Int = 10): Long =
        if (baud <= 0) 0L else bytes.toLong() * bitsPerByte * 1_000_000_000L / baud

    const val D6_BAUD = 230_400
}
