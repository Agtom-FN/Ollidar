package com.lidarscan.app.usb

import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.core.engine.SerialLidarBaud
import com.lidarscan.core.engine.SerialModemLines
import com.lidarscan.core.engine.SilentLineFallback
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
        /**
         * ROUND 32 item 178(b) — **the rate this port actually answered at.**
         *
         * Usually [SerialLidarBaud.STL27L]. When the datasheet's rate produced
         * a silent line and a fallback did not, this is the fallback, and it
         * has to travel all the way to `scan_engine_add_device` — the engine
         * derives per-point timing from `serial_baud`, so identifying at
         * 230 400 and then telling the engine 921 600 would be the exact
         * silent mis-timing `SerialLidarBaud`'s own doc warns about.
         */
        val baud: Int = SerialLidarBaud.STL27L,
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
 * [registry], at the rate that identified it, so `RealEngineBridge.connect` —
 * which refuses a path the registry has no open connection for — can use it
 * with no second open. On anything else the speculatively-opened connection is
 * closed again.
 *
 * ROUND 32 item 178(b): "the rate that identified it" is 921 600 unless the
 * silent-line fallback adopted an LD-family rate, which is why
 * [Stl27lAutoProbeResult.Identified] carries a baud at all — the port is open
 * at that divisor and the engine has to be told the same number.
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

        // ROUND 32 item 178(b): the ladder itself is `runLadder`, which knows
        // nothing about USB. This is the only line that binds it to hardware.
        return runLadder(driver.device.deviceName) { baud -> readWindow(driver, baud, windowMs) }
    }

    /**
     * Open at [baud], read for [windowMs], count.
     *
     * Leaves the connection OPEN when it identified — [Stl27lAutoProbe]'s
     * contract with `RealEngineBridge.connect`, unchanged since round 25 —
     * and closes it otherwise so the next rate can claim the interface.
     */
    private suspend fun readWindow(driver: UsbSerialDriver, baud: Int, windowMs: Long): WindowResult {
        val connection = try {
            withContext(Dispatchers.IO) {
                registry.open(driver, baud, SerialModemLines.STL27L)
            }
        } catch (e: Exception) {
            return WindowResult.Failed(e.message ?: "could not open the serial port")
        }

        val found = CompletableDeferred<Unit>()
        var carry: Byte? = null
        var packets = 0
        var headers = 0
        var bytesSeen = 0L
        val firstBytes = SerialFirstBytesTrace(
            driver = driver,
            sensor = SerialFirstBytesTrace.SENSOR_STL27L,
            baud = baud,
            // ROUND 32 item 178(a): what was actually put on the wire, so a
            // still-silent retest log answers "were the lines asserted?" in the
            // same line that shows the silence.
            lines = registry.lastModemLines,
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
        return WindowResult.Read(
            attempt = SilentLineFallback.Attempt(baud, bytesSeen, headers, packets),
            identified = identified,
        )
    }

    /** One rate's window. [Read] means the port opened and was listened to; [Failed] means it did not. */
    internal sealed interface WindowResult {
        data class Read(val attempt: SilentLineFallback.Attempt, val identified: Boolean) : WindowResult
        data class Failed(val message: String) : WindowResult
    }

    companion object {

        /**
         * ROUND 32 item 178(b) — **the whole silent-line ladder, with no USB in
         * it.**
         *
         * Rung one is exactly what round 25 shipped and round 31 tightened:
         * 921 600, four CRC-valid packets. What is new is what happens when
         * that window comes back SILENT rather than wrong — sixteen bytes, as
         * the owner's line did, on a sensor he could see spinning. That is not
         * a decline, it is an unanswered question, and the LD family runs at
         * more than one rate. See [SilentLineFallback] for the whole argument,
         * and for why a LOUD decline deliberately does not get this treatment.
         *
         * [readWindow] is the only hardware in the design, injected: the live
         * probe passes the real port reader and a test passes byte streams, so
         * what a test drives is this function and not a copy of it.
         */
        internal suspend fun runLadder(
            devicePath: String,
            readWindow: suspend (baud: Int) -> WindowResult,
        ): Stl27lAutoProbeResult {
            val attempts = mutableListOf<SilentLineFallback.Attempt>()

            val first = readWindow(SerialLidarBaud.STL27L)
            if (first is WindowResult.Failed) return Stl27lAutoProbeResult.Error(first.message)
            val firstWindow = first as WindowResult.Read
            attempts += firstWindow.attempt
            if (firstWindow.identified) {
                return Stl27lAutoProbeResult.Identified(
                    devicePath = devicePath,
                    evidence = evidenceFor(attempts, SerialLidarBaud.STL27L),
                    baud = SerialLidarBaud.STL27L,
                )
            }

            if (SilentLineFallback.isSilent(firstWindow.attempt.bytes)) {
                for (baud in SilentLineFallback.FALLBACK_BAUDS) {
                    when (val next = readWindow(baud)) {
                        // A rate that will not even open is not a reason to
                        // stop asking the next one — and it is not the port's
                        // verdict either, since 921 600 opened perfectly well a
                        // moment ago.
                        is WindowResult.Failed -> continue
                        is WindowResult.Read -> {
                            attempts += next.attempt
                            if (next.identified) {
                                return Stl27lAutoProbeResult.Identified(
                                    devicePath = devicePath,
                                    evidence = evidenceFor(attempts, baud),
                                    baud = baud,
                                )
                            }
                        }
                    }
                }
            }

            return Stl27lAutoProbeResult.NotIdentified(
                evidence = evidenceFor(attempts, null),
                // The partial-match signal — the input to round 31's ambiguity
                // verdict — is still the datasheet rate's alone. A CRC-valid
                // packet found at 460 800 is not evidence that the port is an
                // ambiguous STL-27L/D6; it is evidence that it IS an STL-27L,
                // and that branch has already returned above.
                sawPartialMatch = firstWindow.attempt.packets > 0,
            )
        }

        /**
         * Count one window's worth of chunks with the REAL scanner, exactly as
         * the reader-thread callback does. Shared by the live probe's byte path
         * and by the byte-exact tests.
         */
        internal fun countWindow(baud: Int, chunks: Iterable<ByteArray>): WindowResult {
            var carry: Byte? = null
            var packets = 0
            var headers = 0
            var bytesSeen = 0L
            var identified = false
            for (chunk in chunks) {
                bytesSeen += chunk.size
                headers += Stl27lSignatureScanner.headerPairCount(carry, chunk, chunk.size)
                packets += Stl27lSignatureScanner.validPacketCount(carry, chunk, chunk.size)
                carry = Stl27lSignatureScanner.lastByteOrNull(chunk, chunk.size)
                if (packets >= PACKETS_TO_IDENTIFY) {
                    identified = true
                    break
                }
            }
            return WindowResult.Read(
                SilentLineFallback.Attempt(baud, bytesSeen, headers, packets),
                identified,
            )
        }

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
         * ROUND 32 item 178(b) — the evidence line for a probe that may have
         * tried more than one rate.
         *
         * The single-rate case reads exactly as it did in round 31, so a
         * COIN-D6 rig's log is unchanged; the multi-rate case appends every
         * rate's counters and names the one that answered. `chosen` is the
         * rate being adopted, or null when none was.
         */
        internal fun evidenceFor(attempts: List<SilentLineFallback.Attempt>, chosen: Int?): String {
            // One rate tried is the round-31 line, character for character, so
            // a COIN-D6 rig's log does not change shape because a second sensor
            // grew a fallback ladder.
            attempts.singleOrNull()?.let { only ->
                return "bytes=${only.bytes} 542c=${only.headers} packets=${only.packets} " +
                    "(need packets>=$PACKETS_TO_IDENTIFY)"
            }
            val trail = SilentLineFallback.evidenceLine(attempts)
            val verdict = if (chosen == null) {
                "silent at ${SerialLidarBaud.STL27L}; no fallback rate answered"
            } else {
                "adopted $chosen"
            }
            return "$trail ($verdict)"
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
