package com.lidarscan.app.usb

import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.core.engine.SerialLidarBaud
import com.lidarscan.core.engine.Stl27lSignatureScanner
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeoutOrNull

/** Outcome of [Stl27lAutoProbe.probe]. Mirrors [D6AutoProbeResult] deliberately. */
sealed interface Stl27lAutoProbeResult {
    /**
     * Enough CRC-valid LD-series packets arrived — [devicePath] is already open
     * **at 921 600** (permission granted) and ready to hand to the engine.
     */
    data class Identified(val devicePath: String) : Stl27lAutoProbeResult

    /** The port opened at 921 600 and read for the window, but no STL-27L-shaped packets arrived. */
    data object NotIdentified : Stl27lAutoProbeResult

    /** The user denied the USB permission dialog. */
    data object PermissionDenied : Stl27lAutoProbeResult

    data class Error(val message: String) : Stl27lAutoProbeResult
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
            if (!granted) return Stl27lAutoProbeResult.PermissionDenied
        }

        val connection = try {
            registry.open(driver, SerialLidarBaud.STL27L)
        } catch (e: Exception) {
            return Stl27lAutoProbeResult.Error(e.message ?: "could not open the serial port")
        }

        val found = CompletableDeferred<Unit>()
        var carry: Byte? = null
        var packets = 0

        connection.startReading { buffer, len, _ ->
            if (!found.isCompleted && len > 0) {
                buffer.rewind()
                val bytes = ByteArray(len)
                buffer.get(bytes, 0, len)
                packets += Stl27lSignatureScanner.validPacketCount(carry, bytes, len)
                carry = Stl27lSignatureScanner.lastByteOrNull(bytes, len)
                if (packets >= PACKETS_TO_IDENTIFY) found.complete(Unit)
            }
        }

        val identified = withTimeoutOrNull(windowMs) { found.await() } != null
        return if (identified) {
            Stl27lAutoProbeResult.Identified(driver.device.deviceName)
        } else {
            registry.close(driver.device.deviceName)
            Stl27lAutoProbeResult.NotIdentified
        }
    }

    companion object {
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
