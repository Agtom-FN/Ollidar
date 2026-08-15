package com.lidarscan.app.rtk

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.os.Build
import android.util.Log
import com.lidarscan.app.engine.ScanEngineNative
import java.io.IOException
import java.nio.ByteBuffer
import java.util.UUID

/**
 * B9 — the Bluetooth SPP link to an RTK rover (Tech Spec §2.3: "u-blox ZED-F9P
 * class boards and Emlid Reach RX/RS — **NMEA 0183 over Bluetooth** (SPP on
 * Android)"; §3.1's "RTK rover: Bluetooth SPP (Android); NMEA in, RTCM3 out").
 *
 * ### Bonded devices only, and that is not a shortcut
 *
 * This lists `BluetoothAdapter.getBondedDevices()` and does **not** run
 * discovery. Pairing an SPP rover means entering a PIN in the system dialog,
 * which the system Bluetooth settings already do properly; a discovery UI in
 * this app would add `BLUETOOTH_SCAN` (and, before API 31, a *location*
 * permission — the classic "why does my survey app want my location" prompt,
 * on top of the GNSS the rover is already providing) and would still hand the
 * user to the system dialog to finish. So the flow is: pair once in Settings,
 * pick it here.
 *
 * ### The socket
 *
 * `createRfcommSocketToServiceRecord` with the well-known SPP UUID
 * `00001101-0000-1000-8000-00805F9B34FB`. `connect()` blocks, so it runs off
 * the main thread, and `cancelDiscovery()` is called first because an
 * in-progress discovery (started by any app) makes a connect attempt slow and
 * flaky — that is the one documented interaction worth defending against here.
 *
 * ### Bytes
 *
 * NMEA in goes straight to `scan_engine_push_nmea` on the **capture engine's**
 * handle, in whatever chunk the socket produced — the engine's framer handles
 * arbitrary chunking, which is exactly what SPP's 20–990-byte MTU fragments
 * need, and record-always means those bytes hit the `.lscan` as `kGnssNmea`
 * chunks *before* they are parsed. RTCM3 out arrives on the NTRIP receive
 * thread as whole CRC-valid frames and is written to the same socket.
 */
class RtkRoverConnection(private val context: Context) {

    data class BondedRover(val name: String, val address: String)

    private var socket: BluetoothSocket? = null
    private var reader: Thread? = null
    @Volatile private var running = false
    @Volatile private var engineHandle: Long = 0L
    @Volatile private var deviceId: Int = -1

    /** Bytes forwarded into the engine since connect — the "is anything arriving at all" number. */
    @Volatile var bytesIn: Long = 0L
        private set

    /** RTCM bytes written back to the rover. If this stays 0 while NTRIP streams, the rover is not being corrected. */
    @Volatile var rtcmBytesOut: Long = 0L
        private set

    @Volatile var lastError: String? = null
        private set

    val isConnected: Boolean get() = socket?.isConnected == true && running

    private val adapter: BluetoothAdapter?
        get() = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? android.bluetooth.BluetoothManager)?.adapter

    val isBluetoothAvailable: Boolean get() = adapter != null
    val isBluetoothEnabled: Boolean get() = adapter?.isEnabled == true

    /** The runtime permission this needs on this OS version, or null when none is required. */
    val requiredPermission: String?
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            android.Manifest.permission.BLUETOOTH_CONNECT
        } else {
            // Pre-31 BLUETOOTH/BLUETOOTH_ADMIN are install-time permissions:
            // declared in the manifest, granted at install, nothing to ask for.
            null
        }

    @SuppressLint("MissingPermission")
    fun bondedRovers(): List<BondedRover> = try {
        adapter?.bondedDevices.orEmpty().map { BondedRover(it.name ?: it.address, it.address) }
    } catch (e: SecurityException) {
        lastError = "Bluetooth permission was refused: ${e.message}"
        emptyList()
    }

    /**
     * Opens the SPP socket and starts forwarding NMEA into [engineHandle].
     * **Blocking** — call from `Dispatchers.IO`.
     */
    @SuppressLint("MissingPermission")
    fun connect(address: String, engineHandle: Long): Result<Unit> {
        disconnect()
        val a = adapter ?: return Result.failure(IllegalStateException("This device has no Bluetooth adapter."))
        if (!a.isEnabled) return Result.failure(IllegalStateException("Bluetooth is off. Turn it on and try again."))
        if (engineHandle == 0L) {
            return Result.failure(
                IllegalStateException(
                    "No capture engine yet. The rover attaches to the engine that records the session, so open a " +
                        "project and let the engine start before connecting.",
                ),
            )
        }
        return try {
            val device: BluetoothDevice = a.getRemoteDevice(address)
            // An in-progress discovery makes RFCOMM connects slow and flaky —
            // the one documented interaction worth defending against.
            runCatching { a.cancelDiscovery() }
            val s = device.createRfcommSocketToServiceRecord(SPP_UUID)
            s.connect()
            socket = s
            this.engineHandle = engineHandle

            val id = ScanEngineNative.nativeAddRtkRoverDevice(engineHandle)
            if (id < 0) {
                s.close()
                socket = null
                return Result.failure(IllegalStateException("The engine refused the rover device: ${ScanEngineNative.nativeLastError()}"))
            }
            deviceId = id
            bytesIn = 0
            rtcmBytesOut = 0
            lastError = null
            running = true
            reader = Thread({ readLoop(s) }, "rtk-nmea-reader").apply {
                isDaemon = true
                start()
            }
            Result.success(Unit)
        } catch (e: IOException) {
            Result.failure(
                IOException(
                    "Could not open an SPP connection to the rover (${e.message}). Check that it is powered, in range, " +
                        "and still paired — a rover that was re-paired to another phone drops this one.",
                    e,
                ),
            )
        } catch (e: SecurityException) {
            Result.failure(SecurityException("Bluetooth permission was refused: ${e.message}"))
        }
    }

    private fun readLoop(s: BluetoothSocket) {
        // One reused direct buffer: the native side takes its address straight
        // from GetDirectBufferAddress, so nothing is pinned or copied across
        // the JNI boundary. The one arraycopy below is unavoidable —
        // BluetoothSocket's InputStream is byte[]-based with no direct-buffer
        // read, exactly like B2's usb-serial situation.
        val direct = ByteBuffer.allocateDirect(READ_BUFFER_BYTES)
        val scratch = ByteArray(READ_BUFFER_BYTES)
        val input = try {
            s.inputStream
        } catch (e: IOException) {
            lastError = "Could not read from the rover: ${e.message}"
            running = false
            return
        }
        while (running) {
            val n = try {
                input.read(scratch)
            } catch (e: IOException) {
                if (running) lastError = "The rover link dropped: ${e.message}"
                break
            }
            if (n <= 0) {
                if (n < 0) {
                    lastError = "The rover closed the connection."
                    break
                }
                continue
            }
            direct.clear()
            direct.put(scratch, 0, n)
            val rc = ScanEngineNative.nativePushNmea(engineHandle, deviceId, direct, n, 0L)
            if (rc != ScanEngineNative.ErrorCode.OK) {
                Log.w(TAG, "push_nmea returned $rc: ${ScanEngineNative.nativeErrorStr(rc)}")
            } else {
                bytesIn += n
            }
        }
        running = false
    }

    /** The RTCM3 sink handed to the NTRIP client. Runs on the NTRIP receive thread — quick, no re-entry. */
    fun rtcmSink(): ScanEngineNative.RtcmSink = ScanEngineNative.RtcmSink { data ->
        val s = socket ?: return@RtcmSink
        try {
            s.outputStream.write(data)
            rtcmBytesOut += data.size
        } catch (e: IOException) {
            // Do NOT tear the link down from this thread: it belongs to the
            // NTRIP client and the contract is "quick, must not re-enter". The
            // reader thread will notice the same broken socket on its next read.
            lastError = "Could not forward corrections to the rover: ${e.message}"
        }
    }

    fun disconnect() {
        running = false
        val s = socket
        socket = null
        runCatching { s?.close() }
        reader?.interrupt()
        reader = null
        if (engineHandle != 0L && deviceId >= 0) {
            ScanEngineNative.nativeRemoveDevice(engineHandle, deviceId)
        }
        deviceId = -1
        engineHandle = 0L
    }

    companion object {
        private const val TAG = "RtkRover"
        private const val READ_BUFFER_BYTES = 4096

        /** The well-known Serial Port Profile UUID. Every F9P/Emlid SPP rover advertises this. */
        val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
    }
}
