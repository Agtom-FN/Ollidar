package com.lidarscan.app.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import com.hoho.android.usbserial.driver.UsbSerialDriver
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
    fun open(driver: UsbSerialDriver, baud: Int = BAUD_RATE): D6SerialConnection {
        val connection = usbManager.openDevice(driver.device)
            ?: error("UsbManager.openDevice returned null for ${driver.device.deviceName}")
        val port = driver.ports.first()
        port.open(connection)
        port.setParameters(baud, UsbSerialPort.DATABITS_8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
        port.setDTR(false)

        val wrapped = D6SerialConnection(driver.device.deviceName, port)
        wrapped.setSensorLatencyMillis(sensorLatencyMs)
        openConnections[driver.device.deviceName] = wrapped
        return wrapped
    }

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
         * The COIN-D6's 230 400, and therefore [open]'s default — the value
         * this registry has always used. Aliased to
         * `com.lidarscan.core.engine.SerialLidarBaud.COIN_D6` rather than
         * re-typed so that the host divisor and the `serial_baud` the engine is
         * told can never drift apart.
         */
        const val BAUD_RATE = com.lidarscan.core.engine.SerialLidarBaud.COIN_D6
    }
}
