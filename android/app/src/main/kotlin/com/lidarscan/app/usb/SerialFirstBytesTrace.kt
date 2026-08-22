package com.lidarscan.app.usb

import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.app.net.ConnectionDebugTrace
import com.lidarscan.core.net.SerialProbeRecord

/**
 * ROUND 31 item 176(c) — **the line the owner can send us when the retest
 * still fails.**
 *
 * When a serial device is opened, record what it identified itself as
 * (`VID:PID`, the product string the OS reports) and the **first
 * [HEX_BYTES] bytes it actually sent**, as hex, at the baud it was opened at.
 *
 * Why bytes and not a verdict: every remote diagnosis of the STL-27L so far
 * has been an argument about what the device *ought* to emit. Sixty-four bytes
 * of what it *did* emit settles it — `54 2C` at 921 600 is an STL-27L, `AA 55`
 * with a plausible LSN at 230 400 is a D6, printable ASCII is a GNSS receiver,
 * and all zeros means the cable or the divisor. None of those can be told
 * apart from "auto-detect found nothing".
 *
 * ## Bounded by construction
 *
 * * **Developer mode only** — [ConnectionDebugTrace.enabled] mirrors the
 *   seven-tap flag and every `note*` is a volatile read and a return for
 *   everyone else.
 * * **Once per instance.** One instance is created per port open, so the rate
 *   limit is structural rather than a clock: no timer to get wrong, and a
 *   reader thread delivering 200 chunks a second still writes one line.
 * * **[HEX_BYTES] bytes**, which is 192 characters of hex — one long log line,
 *   not a dump. A 47-byte STL-27L packet and a short D6 frame both fit whole.
 */
class SerialFirstBytesTrace(
    private val driver: UsbSerialDriver,
    /** What the caller was probing FOR, so the log says which attempt these bytes belong to. */
    private val sensor: String,
    private val baud: Int,
) {
    private var written = false

    /** The USB identity alone, for a path that opens a port without reading it (the manual connect). */
    fun noteOpen() {
        if (!ConnectionDebugTrace.enabled || written) return
        written = true
        ConnectionDebugTrace.noteSerialProbe(
            sensor = sensor,
            devicePath = driver.device.deviceName,
            outcome = SerialProbeRecord.OUTCOME_PORT_OPEN,
            detail = "$identity baud=$baud",
        )
    }

    /** The identity plus the first bytes off the wire. Only the first call in this instance's life writes. */
    fun note(chunk: ByteArray, len: Int) {
        if (!ConnectionDebugTrace.enabled || written || len <= 0) return
        written = true
        val n = minOf(len, HEX_BYTES)
        val hex = buildString(n * 3) {
            for (i in 0 until n) {
                if (i > 0) append(' ')
                append(HEX_DIGITS[(chunk[i].toInt() shr 4) and 0xF])
                append(HEX_DIGITS[chunk[i].toInt() and 0xF])
            }
        }
        ConnectionDebugTrace.noteSerialProbe(
            sensor = sensor,
            devicePath = driver.device.deviceName,
            outcome = SerialProbeRecord.OUTCOME_FIRST_BYTES,
            detail = "$identity baud=$baud first$n=$hex",
        )
    }

    private val identity: String
        get() = buildString {
            append("vid:pid=")
            append("%04x".format(driver.device.vendorId and 0xFFFF))
            append(':')
            append("%04x".format(driver.device.productId and 0xFFFF))
            val product = runCatching { driver.device.productName }.getOrNull()
            if (!product.isNullOrBlank()) append(" product=\"").append(product.replace('"', '\'')).append('"')
        }

    companion object {
        /** Enough for a whole 47-byte LD-series packet plus its neighbour's header. */
        const val HEX_BYTES = 64

        /** Probe labels, so a `[net-debug]` line says which attempt produced the bytes. */
        const val SENSOR_D6 = "COIN_D6"
        const val SENSOR_STL27L = "STL27L"
        const val SENSOR_MANUAL = "manual"

        private const val HEX_DIGITS = "0123456789abcdef"
    }
}
