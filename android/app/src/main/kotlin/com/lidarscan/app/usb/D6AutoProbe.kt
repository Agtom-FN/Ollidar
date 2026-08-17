package com.lidarscan.app.usb

import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.core.engine.D6SignatureScanner
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeoutOrNull

/** Outcome of [D6AutoProbe.probe]. */
sealed interface D6AutoProbeResult {
    /** The `AA 55` preamble was seen — [devicePath] is already open (permission granted) and ready to hand to the engine. */
    data class Identified(val devicePath: String) : D6AutoProbeResult

    /** Permission was granted (or already held) and the port opened, but no D6-shaped bytes arrived in the window. */
    data object NotIdentified : D6AutoProbeResult

    /** The user denied the USB permission dialog. */
    data object PermissionDenied : D6AutoProbeResult

    data class Error(val message: String) : D6AutoProbeResult
}

/**
 * AUTO-DETECT (D6 wizard): on attach, opens a CH340-class serial device and
 * reads for a short window to confirm it is genuinely a COIN-D6 — rather
 * than blind-connecting to any attached CH340 device, which
 * `android/NOTES.md`'s GNSS-over-USB-serial gap note flags as a real
 * ambiguity (a Unicore UM982 eval board enumerates as the same CH340
 * VID/PID). See [D6SignatureScanner]'s doc for why `AA 55` is a safe,
 * cheap-to-check signature.
 *
 * On [D6AutoProbeResult.Identified], the returned device path's connection
 * is **left open** in [registry] (with the probe's own read callback still
 * attached) so the caller can hand it straight to
 * [com.lidarscan.core.engine.D6ConnectController.onPermissionGranted]
 * without reopening the port — `D6SerialConnection.startReading` documents
 * that swapping the callback later (once the engine wires its own) is safe.
 * On [D6AutoProbeResult.NotIdentified], the speculatively-opened connection
 * is closed again — holding a GNSS receiver's port open on a guess would
 * block the RTK screen's own (future) use of it.
 */
class D6AutoProbe(private val registry: D6UsbConnectionRegistry) {

    suspend fun probe(driver: UsbSerialDriver, windowMs: Long = PROBE_WINDOW_MS): D6AutoProbeResult {
        if (!registry.hasPermission(driver)) {
            val granted = registry.requestPermission(driver)
            if (!granted) return D6AutoProbeResult.PermissionDenied
        }

        val connection = try {
            registry.open(driver)
        } catch (e: Exception) {
            return D6AutoProbeResult.Error(e.message ?: "could not open the serial port")
        }

        val found = CompletableDeferred<Unit>()
        var carry: Byte? = null

        connection.startReading { buffer, len, _ ->
            if (!found.isCompleted && len > 0) {
                buffer.rewind()
                val bytes = ByteArray(len)
                buffer.get(bytes, 0, len)
                if (D6SignatureScanner.containsSignature(carry, bytes, len)) {
                    found.complete(Unit)
                } else {
                    carry = D6SignatureScanner.lastByteOrNull(bytes, len)
                }
            }
        }

        val identified = withTimeoutOrNull(windowMs) { found.await() } != null
        return if (identified) {
            D6AutoProbeResult.Identified(driver.device.deviceName)
        } else {
            registry.close(driver.device.deviceName)
            D6AutoProbeResult.NotIdentified
        }
    }

    private companion object {
        /**
         * The D6 streams roughly 10 packets/s at typical scan-freq settings
         * (`FIELD_SESSION_2026-08-17.md`: "one [start packet] per
         * revolution", ~10 Hz) — 1.5 s is comfortably several revolutions,
         * long enough to survive one missed/corrupt first read.
         */
        const val PROBE_WINDOW_MS = 1_500L
    }
}
