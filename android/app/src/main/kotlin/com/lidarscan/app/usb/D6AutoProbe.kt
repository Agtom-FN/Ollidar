package com.lidarscan.app.usb

import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.core.engine.D6FrameScanner
import com.lidarscan.core.engine.D6SignatureScanner
import com.lidarscan.core.engine.SerialLidarBaud
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Outcome of [D6AutoProbe.probe].
 *
 * ROUND 31 item 176(b): every branch now carries [evidence] — the counters the
 * probe actually decided on, in one line. A probe that can only say "no" is a
 * probe nobody can debug from a field log, and the owner's first STL-27L
 * session is exactly the log that needed it.
 */
sealed interface D6AutoProbeResult {
    /** One line of `bytes=… aa55=… frames=… chained=…`. See [D6FrameScanner.evidence]. */
    val evidence: String

    /** A real D6 frame chain was seen — [devicePath] is already open (permission granted) and ready for the engine. */
    data class Identified(val devicePath: String, override val evidence: String = "") : D6AutoProbeResult

    /**
     * Permission was granted (or already held) and the port opened, but no
     * D6-shaped frame chain arrived in the window.
     *
     * [sawPartialMatch] is true when *something* D6-shaped was there and was
     * refused — at least one well-formed header, but not [D6SignatureScanner
     * .FRAMES_TO_IDENTIFY] of them or none of them chained. That is the signal
     * the ladder needs to call a port AMBIGUOUS rather than empty.
     */
    data class NotIdentified(
        override val evidence: String = "",
        val sawPartialMatch: Boolean = false,
    ) : D6AutoProbeResult

    /** The user denied the USB permission dialog. */
    data class PermissionDenied(override val evidence: String = "") : D6AutoProbeResult

    data class Error(val message: String, override val evidence: String = "") : D6AutoProbeResult
}

/**
 * AUTO-DETECT (D6 wizard): open a CH340-class serial device at 230 400 and
 * read for a short window to confirm it is genuinely a COIN-D6 — rather than
 * blind-connecting to any attached CH340 device, which `android/NOTES.md`'s
 * GNSS-over-USB-serial gap note flags as a real ambiguity (a Unicore UM982
 * eval board enumerates as the same CH340 VID/PID).
 *
 * ## ROUND 31 item 176(b) — this probe used to say yes far too easily
 *
 * It decided on `D6SignatureScanner.containsSignature`, i.e. "were the bytes
 * `AA 55` adjacent anywhere in 1.5 s". Round 25's own report called that out
 * as the unproven direction, and on 2026-08-22 the owner's first real STL-27L
 * was misread as a COIN-D6 through exactly it. The window is ~34 500 bytes, so
 * that test says yes to about **41 %** of pure noise.
 *
 * It now decides on [D6FrameScanner]: four complete, well-formed D6 frames
 * with at least one correctly-chained pair. See [D6SignatureScanner] for the
 * arithmetic, for what a real device pays (~40 ms of the window), and for why
 * the D6's contested XOR checksum is deliberately not part of the bar.
 *
 * ## Contract, unchanged
 *
 * On [D6AutoProbeResult.Identified], the returned device path's connection is
 * **left open** in [registry] (with the probe's own read callback still
 * attached) so the caller can hand it straight to the engine without reopening
 * the port — `D6SerialConnection.startReading` documents that swapping the
 * callback later is safe. On anything else the speculatively-opened connection
 * is closed again — holding a GNSS receiver's port open on a guess would block
 * the RTK screen's own (future) use of it.
 */
class D6AutoProbe(private val registry: D6UsbConnectionRegistry) {

    suspend fun probe(driver: UsbSerialDriver, windowMs: Long = PROBE_WINDOW_MS): D6AutoProbeResult {
        if (!registry.hasPermission(driver)) {
            val granted = registry.requestPermission(driver)
            if (!granted) return D6AutoProbeResult.PermissionDenied()
        }

        val connection = try {
            // ROUND 31: the open path blocks (usbfs ioctls, and since item
            // 176(a) a reader-thread join when a previous connection has to be
            // released first). `CaptureAutoConnectController` runs its
            // detectors on `viewModelScope`, which is Main.
            withContext(Dispatchers.IO) { registry.open(driver, SerialLidarBaud.COIN_D6) }
        } catch (e: Exception) {
            return D6AutoProbeResult.Error(e.message ?: "could not open the serial port")
        }

        val found = CompletableDeferred<Unit>()
        val scanner = D6FrameScanner()
        val firstBytes = SerialFirstBytesTrace(
            driver = driver,
            sensor = SerialFirstBytesTrace.SENSOR_D6,
            baud = SerialLidarBaud.COIN_D6,
        )

        connection.startReading { buffer, len, _ ->
            if (!found.isCompleted && len > 0) {
                buffer.rewind()
                val bytes = ByteArray(len)
                buffer.get(bytes, 0, len)
                firstBytes.note(bytes, len)
                scanner.feed(bytes, len)
                if (scanner.identified) found.complete(Unit)
            }
        }

        val identified = withTimeoutOrNull(windowMs) { found.await() } != null
        if (!identified) withContext(Dispatchers.IO) { registry.close(driver.device.deviceName) }
        return verdict(driver.device.deviceName, scanner)
    }

    companion object {
        /**
         * ROUND 31 item 176(b) — **counters to answer, in one place.**
         *
         * Both the live probe above and [classify] below end here, so a test
         * that drives byte-exact chunks through [classify] is asserting on the
         * same mapping the phone uses. A second copy of this three-line
         * decision, living only in a test, would be a test of the copy.
         */
        internal fun verdict(devicePath: String, scanner: D6FrameScanner): D6AutoProbeResult =
            if (scanner.identified) {
                D6AutoProbeResult.Identified(devicePath, scanner.evidence())
            } else {
                D6AutoProbeResult.NotIdentified(
                    evidence = scanner.evidence(),
                    // "Something D6-shaped was here and I refused it" — the
                    // input to the ladder's ambiguity verdict. A bare `AA 55`
                    // does NOT count: that is the signal this round exists to
                    // stop trusting.
                    sawPartialMatch = scanner.frames > 0,
                )
            }

        /**
         * The probe's decision, over canned chunks instead of a USB reader
         * thread — everything [probe] does except owning a port.
         *
         * There is no STL-27L and no COIN-D6 on the machine this project is
         * built on, so this is how the round-31 probes are exercised: byte-exact
         * synthetic streams in, the real [D6FrameScanner] and the real [verdict]
         * in between, a real [D6AutoProbeResult] out. What it does NOT cover is
         * `UsbSerialDriver`, `registry.open` and the read loop — that seam is
         * the owner's retest, and item 176(c)'s first-64-bytes log line is what
         * makes its failure readable.
         *
         * Stops at the first chunk that satisfies the bar, exactly as the live
         * callback's `found.complete(Unit)` does.
         */
        @androidx.annotation.VisibleForTesting
        fun classify(devicePath: String, chunks: Iterable<ByteArray>): D6AutoProbeResult {
            val scanner = D6FrameScanner()
            for (chunk in chunks) {
                scanner.feed(chunk, chunk.size)
                if (scanner.identified) break
            }
            return verdict(devicePath, scanner)
        }

        /**
         * The D6 streams roughly 10 packets/s at typical scan-freq settings
         * (`FIELD_SESSION_2026-08-17.md`: "one [start packet] per
         * revolution", ~10 Hz) — 1.5 s is comfortably several revolutions,
         * long enough to survive one missed/corrupt first read.
         */
        const val PROBE_WINDOW_MS = 1_500L
    }
}
