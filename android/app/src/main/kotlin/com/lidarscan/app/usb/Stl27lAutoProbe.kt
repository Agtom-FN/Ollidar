package com.lidarscan.app.usb

import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.core.engine.SerialLidarBaud
import com.lidarscan.core.engine.Stl27lSignatureScanner
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Outcome of [Stl27lAutoProbe.probe]. Mirrors [D6AutoProbeResult] deliberately,
 * including ROUND 31 item 176(b)'s [evidence] line — the two probes have to be
 * readable side by side in one field log or the ladder's verdict cannot be
 * checked.
 */
sealed interface Stl27lAutoProbeResult {
    /** One line of `bytes=… headers=… packets=…`, the counters the probe decided on. */
    val evidence: String

    /**
     * Enough CRC-valid LD-series packets arrived — [devicePath] is already open
     * **at 921 600** (permission granted) and ready to hand to the engine.
     */
    data class Identified(
        val devicePath: String,
        override val evidence: String = "",
    ) : Stl27lAutoProbeResult

    /**
     * The port opened at 921 600 and read for the window, but no STL-27L-shaped
     * packets arrived.
     *
     * [sawPartialMatch] is true when at least one CRC-valid packet WAS seen and
     * the four-packet bar simply was not reached — the input to the ladder's
     * ambiguity verdict. A bare `54 2C` header does not count; the CRC is the
     * whole reason this probe is trustworthy (see [Stl27lSignatureScanner]).
     */
    data class NotIdentified(
        override val evidence: String = "",
        val sawPartialMatch: Boolean = false,
    ) : Stl27lAutoProbeResult

    /** The user denied the USB permission dialog. */
    data class PermissionDenied(override val evidence: String = "") : Stl27lAutoProbeResult

    data class Error(val message: String, override val evidence: String = "") : Stl27lAutoProbeResult
}

/**
 * ROUND 25 item 119 — the STL-27L half of serial auto-detect: reopen the one
 * attached serial device **at 921 600** and read for a short window to see
 * whether an LDROBOT STL-27L is on the other end.
 *
 * ## Why this is Kotlin and not a C-ABI call
 *
 * The engine has a real STL-27L probe in C++ (`discovery::ProbeSerialStl27l` /
 * `discovery::Stl27lSniffer`, tested there), but **its C mirror was
 * deliberately deferred**: adding `scan_probe_stl27l()` would have been a new
 * exported symbol and therefore ABI 13, and item 119's whole point is that the
 * ABI stays at 12. Nobody should go looking for a native probe to bind — there
 * isn't one, and this is the app's own. That is not a new pattern either: the
 * D6's auto-detect is Kotlin-side too ([D6AutoProbe] + `D6SignatureScanner`),
 * and has never called `scan_probe_d6`.
 *
 * ## What "identified" means here
 *
 * [PACKETS_TO_IDENTIFY] complete, CRC-valid 47-byte packets across the window
 * — **four**, matching the engine's own
 * `discovery::Stl27lSniffer::kPacketsToIdentify`, and for the reason stated
 * there: the LD frame's sync is only two bytes wide and its CRC only eight
 * bits, so one accepted packet is a ~1-in-16-million coincidence per byte
 * offset rather than the D6 frame's ~1-in-4-billion, and over a second of a
 * 921 600 link that is not negligible. Four of them is. See
 * [Stl27lSignatureScanner] for why the CRC gate is not optional for this
 * protocol.
 *
 * At the datasheet's ~1800 packets/s a genuine device clears four in about
 * 2 ms, so the bar costs a real STL-27L nothing.
 *
 * ## Contract, identical to [D6AutoProbe]'s
 *
 * On [Stl27lAutoProbeResult.Identified] the connection is **left open** in
 * [registry] at 921 600 so `RealEngineBridge.connect` — which refuses a path
 * the registry has no open connection for — can use it with no second open.
 * On anything else the speculatively-opened connection is closed again.
 *
 * UNVERIFIED: no STL-27L hardware exists. The framing, the CRC parameters and
 * the packet rate above are protocol-derived from the public LD-series
 * references, not observed.
 */
class Stl27lAutoProbe(private val registry: D6UsbConnectionRegistry) {

    suspend fun probe(driver: UsbSerialDriver, windowMs: Long = PROBE_WINDOW_MS): Stl27lAutoProbeResult {
        if (!registry.hasPermission(driver)) {
            val granted = registry.requestPermission(driver)
            if (!granted) return Stl27lAutoProbeResult.PermissionDenied()
        }

        val connection = try {
            // ROUND 31: blocking usbfs work (and, since item 176(a), a
            // reader-thread join when a previous connection has to be released
            // first) off the Main dispatcher the detectors run on.
            withContext(Dispatchers.IO) { registry.open(driver, SerialLidarBaud.STL27L) }
        } catch (e: Exception) {
            return Stl27lAutoProbeResult.Error(e.message ?: "could not open the serial port")
        }

        val found = CompletableDeferred<Unit>()
        var carry: Byte? = null
        var packets = 0
        var headers = 0
        var bytesSeen = 0L
        val firstBytes = SerialFirstBytesTrace(
            driver = driver,
            sensor = SerialFirstBytesTrace.SENSOR_STL27L,
            baud = SerialLidarBaud.STL27L,
        )

        connection.startReading { buffer, len, _ ->
            if (!found.isCompleted && len > 0) {
                buffer.rewind()
                val bytes = ByteArray(len)
                buffer.get(bytes, 0, len)
                firstBytes.note(bytes, len)
                bytesSeen += len
                // ROUND 31 item 176(b): the bare `54 2C` count is recorded for
                // the LOG only, never for the verdict. It is the number the
                // probe would have decided on if it were as weak as the D6's
                // was, and having both on one line is what lets a field log
                // show the CRC gate doing its job.
                headers += Stl27lSignatureScanner.headerPairCount(carry, bytes, len)
                packets += Stl27lSignatureScanner.validPacketCount(carry, bytes, len)
                carry = Stl27lSignatureScanner.lastByteOrNull(bytes, len)
                if (packets >= PACKETS_TO_IDENTIFY) found.complete(Unit)
            }
        }

        val identified = withTimeoutOrNull(windowMs) { found.await() } != null
        if (!identified) withContext(Dispatchers.IO) { registry.close(driver.device.deviceName) }
        return verdict(driver.device.deviceName, bytesSeen, headers, packets)
    }

    companion object {
        /**
         * ROUND 31 item 176(b) — counters to answer, in one place, shared by
         * the live probe and by [classify] so a byte-exact test is asserting on
         * the mapping the phone actually uses.
         */
        internal fun verdict(
            devicePath: String,
            bytesSeen: Long,
            headers: Int,
            packets: Int,
        ): Stl27lAutoProbeResult {
            val evidence = "bytes=$bytesSeen 542c=$headers packets=$packets (need packets>=$PACKETS_TO_IDENTIFY)"
            return if (packets >= PACKETS_TO_IDENTIFY) {
                Stl27lAutoProbeResult.Identified(devicePath, evidence)
            } else {
                Stl27lAutoProbeResult.NotIdentified(evidence = evidence, sawPartialMatch = packets > 0)
            }
        }

        /**
         * The probe's decision over canned chunks instead of a USB reader
         * thread — see [D6AutoProbe.classify] for what this covers and, more
         * importantly, what it does not.
         */
        @androidx.annotation.VisibleForTesting
        fun classify(devicePath: String, chunks: Iterable<ByteArray>): Stl27lAutoProbeResult {
            var carry: Byte? = null
            var packets = 0
            var headers = 0
            var bytesSeen = 0L
            for (chunk in chunks) {
                bytesSeen += chunk.size
                headers += Stl27lSignatureScanner.headerPairCount(carry, chunk, chunk.size)
                packets += Stl27lSignatureScanner.validPacketCount(carry, chunk, chunk.size)
                carry = Stl27lSignatureScanner.lastByteOrNull(chunk, chunk.size)
                if (packets >= PACKETS_TO_IDENTIFY) break
            }
            return verdict(devicePath, bytesSeen, headers, packets)
        }

        /** Four CRC-valid packets. See the class doc for the arithmetic behind the number. */
        const val PACKETS_TO_IDENTIFY = 4

        /**
         * Shorter than the D6's 1.5 s, and that is not a corner cut: the D6
         * emits ~10 packets/s so its window has to span several revolutions,
         * while the STL-27L emits ~1800/s and clears the four-packet bar in
         * milliseconds. The remaining 750 ms is entirely slack for USB
         * scheduling and for the device's own spin-up.
         */
        const val PROBE_WINDOW_MS = 750L
    }
}
