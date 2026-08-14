package com.lidarscan.app.usb

import com.hoho.android.usbserial.driver.UsbSerialPort
import java.io.IOException
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * One open D6 USB-serial connection: 230400 8N1, DTR cleared (Tech Spec
 * §3.1's Android row; matches the CH340 bring-up the S1 spike proved).
 *
 * Owns the reader thread that feeds bytes to
 * `ScanEngineNative.nativePushSerialBytes` via a reusable *direct*
 * [ByteBuffer] — zero-copy at the JNI boundary (see scanengine_jni.cpp's
 * `nativePushSerialBytes`, which calls `GetDirectBufferAddress` on it rather
 * than pinning/copying a `byte[]`). The USB read call itself only hands back
 * a plain `byte[]` — that is usb-serial-for-android's synchronous API, it
 * has no direct-buffer read — so one `arraycopy` from that array into the
 * reused direct buffer is unavoidable on this side; "zero-copy" here means
 * specifically "no further copy happens crossing into native code."
 */
class D6SerialConnection(
    val devicePath: String,
    private val port: UsbSerialPort,
) {
    private var readerThread: Thread? = null
    private val running = AtomicBoolean(false)
    private val forwarding = AtomicBoolean(true)
    private val onBytes = AtomicReference<((ByteBuffer, Int, Long) -> Unit)?>(null)

    /** The engine's write-command callback target (D6 start/stop bytes, `AA 55 F0 0F` etc.). */
    fun write(data: ByteArray): Int = try {
        port.write(data, WRITE_TIMEOUT_MS)
        SCAN_OK
    } catch (e: IOException) {
        SCAN_ERR_IO
    }

    /** Starts the reader loop if not already running; safe to call again to swap [callback]. */
    fun startReading(callback: (buffer: ByteBuffer, len: Int, tMonoNs: Long) -> Unit) {
        onBytes.set(callback)
        if (running.getAndSet(true)) return

        val thread = Thread({
            val readBuf = ByteArray(READ_CHUNK_BYTES)
            val direct = ByteBuffer.allocateDirect(READ_CHUNK_BYTES)
            while (running.get()) {
                val n = try {
                    port.read(readBuf, READ_TIMEOUT_MS)
                } catch (e: IOException) {
                    break // device gone — D6ConnectController's onDeviceLost() path handles the state transition
                }
                if (n > 0 && forwarding.get()) {
                    direct.clear()
                    direct.put(readBuf, 0, n)
                    // t_mono_ns = 0 => "stamp on arrival" (scanengine_c.h's push_serial_bytes contract).
                    onBytes.get()?.invoke(direct, n, 0L)
                }
            }
        }, "d6-usb-reader-$devicePath")
        thread.isDaemon = true
        readerThread = thread
        thread.start()
    }

    /** Pause capture (B4/B2): keep the port open, stop forwarding bytes into the engine. See RealEngineBridge. */
    fun pauseForwarding() = forwarding.set(false)
    fun resumeForwarding() = forwarding.set(true)

    fun stopReading() {
        running.set(false)
        readerThread?.join(READER_JOIN_TIMEOUT_MS)
        readerThread = null
    }

    fun close() {
        stopReading()
        runCatching { port.close() }
    }

    private companion object {
        const val READ_CHUNK_BYTES = 4096
        const val READ_TIMEOUT_MS = 500
        const val WRITE_TIMEOUT_MS = 500
        const val READER_JOIN_TIMEOUT_MS = 1000L

        // scan_error_t values (scanengine_c.h) — kept local to avoid a
        // dependency from :app/usb onto :app/engine for two constants.
        const val SCAN_OK = 0
        const val SCAN_ERR_IO = 20
    }
}
