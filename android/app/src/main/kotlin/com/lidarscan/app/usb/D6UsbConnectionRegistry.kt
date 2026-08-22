package com.lidarscan.app.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.lidarscan.core.engine.SerialModemLines
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import java.util.concurrent.ConcurrentHashMap
import kotlin.coroutines.resume
import kotlinx.coroutines.suspendCancellableCoroutine

private const val ACTION_USB_PERMISSION = "com.lidarscan.app.USB_PERMISSION"

/**
 * Owns D6 USB-serial device discovery, the runtime permission flow, and the
 * registry of open connections that [com.lidarscan.app.engine.RealEngineBridge]
 * looks up by device path when [com.lidarscan.core.engine.EngineTarget.transportHint]
 * names one (Tech Spec §3.1 Android row: "usb-serial-for-android (CH340) →
 * JNI; permission/attach flow"). One instance lives in [com.lidarscan.app.di.AppContainer]
 * for the app's lifetime.
 */
class D6UsbConnectionRegistry(context: Context) {
    private val appContext = context.applicationContext
    private val usbManager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
    private val openConnections = ConcurrentHashMap<String, D6SerialConnection>()

    private var permissionContinuation: ((Boolean) -> Unit)? = null
    private var receiverRegistered = false

    /**
     * ROUND 7 (time-sync): the constant transport latency applied to every open
     * connection, and to every one opened afterwards. Held here rather than
     * passed at [open] because the Settings screen can change it mid-session and
     * a D6 that is already streaming should pick it up on the next chunk, not on
     * the next reconnect. See [com.lidarscan.core.capture.D6TimeSync].
     */
    @Volatile
    private var sensorLatencyMs: Int = com.lidarscan.core.capture.D6TimeSync.DEFAULT_SENSOR_LATENCY_MS

    fun setSensorLatencyMillis(millis: Int) {
        val clamped = com.lidarscan.core.capture.D6TimeSync.clampLatencyMs(millis)
        sensorLatencyMs = clamped
        openConnections.values.forEach { it.setSensorLatencyMillis(clamped) }
    }

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(receiverContext: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            permissionContinuation?.invoke(granted)
            permissionContinuation = null
        }
    }

    /** CH340/D6-shaped serial drivers currently attached — permission not necessarily granted yet. */
    fun findDrivers(): List<UsbSerialDriver> = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)

    fun hasPermission(driver: UsbSerialDriver): Boolean = usbManager.hasPermission(driver.device)

    /** Suspends until the user answers the system USB-permission dialog (or it was already granted). */
    suspend fun requestPermission(driver: UsbSerialDriver): Boolean {
        if (usbManager.hasPermission(driver.device)) return true
        ensureReceiverRegistered()
        return suspendCancellableCoroutine { cont ->
            permissionContinuation = { granted -> if (cont.isActive) cont.resume(granted) }
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val pendingIntent = PendingIntent.getBroadcast(appContext, 0, Intent(ACTION_USB_PERMISSION), flags)
            usbManager.requestPermission(driver.device, pendingIntent)
        }
    }

    private fun ensureReceiverRegistered() {
        if (receiverRegistered) return
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            appContext.registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            appContext.registerReceiver(permissionReceiver, filter)
        }
        receiverRegistered = true
    }

    /**
     * Opens a serial lidar's port 8N1 with DTR cleared. Requires permission
     * already granted.
     *
     * ROUND 25 item 119 — [baud] became a parameter, defaulting to
     * [BAUD_RATE] (the COIN-D6's 230 400) so that **every existing caller
     * keeps its exact previous behaviour**: this is the code path every
     * recorded scan in `captures/` came through and it was not going to be
     * disturbed by a second sensor.
     *
     * The STL-27L needs 921 600 here, and this is the *host UART divisor* —
     * distinct from, and required to agree with, the `serial_baud` that
     * `RealEngineBridge` hands the engine. Both read
     * `com.lidarscan.core.engine.SerialLidarBaud` for exactly that reason: at
     * the wrong divisor the port still opens and the reader thread still
     * delivers bytes, they are simply framing garbage, so the failure looks
     * like a sensor that is present and silent rather than like an error.
     */
    fun open(
        driver: UsbSerialDriver,
        baud: Int = BAUD_RATE,
        // ROUND 32 item 178(a): DEFAULTED TO THE D6'S STATE, so every caller
        // that existed before this round opens the port exactly as it always
        // has. The STL-27L paths pass their own; nothing is inferred from the
        // baud, because item 178(b)'s fallback opens an STL-27L at the D6's
        // rate and inferring would hand it the D6's lines.
        lines: SerialModemLines.State = SerialModemLines.COIN_D6,
    ): D6SerialConnection {
        // ══════════════════════════════════════════════════════════════════
        // ROUND 31 item 176(a) — **THE BUG THE OWNER HIT, IN ONE LINE.**
        //
        // This method used to open the device and overwrite the map entry
        // without ever letting go of the connection that entry already held.
        // Every caller before round 25 opened a port that was closed, so it
        // never mattered. Item 119 created the caller for which it does:
        //
        //   1. auto-detect's D6 rung identifies (wrongly, pre-176(b)) and
        //      LEAVES THE PORT OPEN at 230 400 with its reader thread running
        //      — that is `D6AutoProbe`'s documented contract;
        //   2. the operator sees "COIN-D6", opens the Advanced sheet, picks
        //      STL-27L and taps Connect;
        //   3. `openSerialPortByPath` lands here with baud = 921 600.
        //
        // `findDrivers()` hands back a FRESH `UsbSerialDriver` every call, so
        // the new port object's own "Already open" guard does not fire. What
        // fires instead is one level down: `usbManager.openDevice` returns a
        // second `UsbDeviceConnection`, and `CommonUsbSerialPort.open` then
        // calls `claimInterface(iface, force = true)` on an interface the
        // first connection still holds. `force` only detaches KERNEL drivers;
        // against another claim in the same process usbfs answers EBUSY, and
        // the port throws `IOException("Could not claim interface 0")`.
        //
        // That exception became `Result.failure` in `openSerialPortByPath`,
        // which `CaptureViewModel.connectManualSerialLidar` deliberately turns
        // into a connect with `transportHint = null` so the reason reaches the
        // UI — and the D6 session the operator was trying to REPLACE was still
        // connected and streaming underneath it. From the operator's chair:
        // "even manual selection is not adopted."
        //
        // The fix is that a re-open is a re-open. Releasing the previous
        // connection first is also simply correct on its own terms: the old
        // reader thread would otherwise keep running, keep pushing bytes into
        // whatever device id the engine had, and never be reachable again once
        // the map entry was overwritten.
        // ══════════════════════════════════════════════════════════════════
        close(driver.device.deviceName)

        val connection = usbManager.openDevice(driver.device)
            ?: error("UsbManager.openDevice returned null for ${driver.device.deviceName}")
        val port = driver.ports.first()
        port.open(connection)
        port.setParameters(baud, UsbSerialPort.DATABITS_8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)

        // ══════════════════════════════════════════════════════════════════
        // ROUND 32 item 178(a) — **the silent line, and the two wires it was.**
        //
        // What stood here was `port.setDTR(false)` and nothing about RTS,
        // which usb-serial-for-android leaves de-asserted from `open()`. Both
        // control lines low, on every port this app has ever opened. The
        // COIN-D6's adapter does not care and a hundred field captures prove
        // it; the STL-27L arrives on a CH340 DEV-KIT BOARD (1a86:7523), and
        // that class of board commonly gates the sensor's enable or the level
        // shifter's output-enable on DTR and/or RTS.
        //
        // The owner's retest is the evidence: the sensor is SPINNING and the
        // line delivered 42 bytes in two seconds with `first1=00` and not one
        // `54 2C`. Powered, enabled, rotating, and saying nothing — which is
        // the signature of a data path held down rather than of a baud or a
        // protocol.
        //
        // Not asserted globally: see `SerialModemLines` for why the D6 keeps
        // the state it has field history with, and why the decision to unify
        // is left to the next log rather than taken here.
        //
        // Some drivers throw on a control-line write they cannot perform.
        // Neither line is required for a device that ignores them, so a
        // failure here must not lose a port that would otherwise work — it is
        // recorded and the open continues.
        // ══════════════════════════════════════════════════════════════════
        val linesApplied = applyModemLines(port, lines)

        lastModemLines = if (linesApplied) lines else null

        val wrapped = D6SerialConnection(driver.device.deviceName, port)
        wrapped.setSensorLatencyMillis(sensorLatencyMs)
        openConnections[driver.device.deviceName] = wrapped
        return wrapped
    }

    /**
     * ROUND 32 item 178(a): the modem-line state the last [open] actually put
     * on the wire, or null when the driver refused it. Read by
     * [SerialFirstBytesTrace] so the `[net-debug]` port-open line records what
     * was asserted rather than what was asked for.
     */
    @Volatile
    var lastModemLines: SerialModemLines.State? = null
        private set

    fun get(devicePath: String): D6SerialConnection? = openConnections[devicePath]

    /**
     * ROUND 7: registers an already-constructed connection under [devicePath].
     *
     * The one seam the transport-re-arm regression test needs. `RealEngineBridge`
     * looks a connection up here by path and refuses a path this registry has no
     * entry for, so a test that wants to exercise
     * **connect → start → stop → start** against the real engine — which is the
     * exact sequence that recorded nothing in the field — has no other way in.
     * Production code never calls it; [open] is the only production path.
     */
    @androidx.annotation.VisibleForTesting
    fun register(connection: D6SerialConnection) {
        connection.setSensorLatencyMillis(sensorLatencyMs)
        openConnections[connection.devicePath] = connection
    }

    fun close(devicePath: String) {
        openConnections.remove(devicePath)?.close()
    }

    companion object {
        /**
         * ROUND 32 item 178(a) — the two setter calls, extracted so they can be
         * asserted against a fake port without a USB stack.
         *
         * Returns false when the driver refused, which is not a failure of the
         * open: neither line is required by a device that ignores them, and
         * losing a port that would otherwise work in order to insist on a
         * control signal would be a worse bug than the one this fixes.
         */
        @androidx.annotation.VisibleForTesting
        internal fun applyModemLines(port: UsbSerialPort, lines: SerialModemLines.State): Boolean =
            runCatching {
                port.setDTR(lines.dtr)
                port.setRTS(lines.rts)
            }.isSuccess

        /**
         * The COIN-D6's 230 400, and therefore [open]'s default — the value
         * this registry has always used. Aliased to
         * `com.lidarscan.core.engine.SerialLidarBaud.COIN_D6` rather than
         * re-typed so that the host divisor and the `serial_baud` the engine is
         * told can never drift apart.
         */
        const val BAUD_RATE = com.lidarscan.core.engine.SerialLidarBaud.COIN_D6
    }
}
