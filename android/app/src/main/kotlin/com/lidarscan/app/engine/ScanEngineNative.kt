package com.lidarscan.app.engine

import java.nio.ByteBuffer

/**
 * JNI surface over `engine/capi/scanengine_c.h`, bound by
 * `android/app/src/main/cpp/scanengine_jni.cpp`. This is a thin 1:1
 * transliteration of the C ABI (engine handles become `Long`, `scan_error_t`
 * becomes `Int`, strings/buffers marshal as documented per-method) — no
 * business logic lives here. [RealEngineBridge] is the layer that turns this
 * into the `EngineBridge` contract.
 *
 * Pinned against `SCAN_ABI_VERSION` 1 as of B2 (checked again natively in
 * `nativeCreateEngine`); see android/NOTES.md's "C ABI gaps" section for
 * what the C ABI does *not* expose yet.
 */
object ScanEngineNative {

    /** True once [System.loadLibrary] for `scanengine_jni` has succeeded. Checked once, at class init. */
    val isAvailable: Boolean = try {
        System.loadLibrary("scanengine_jni")
        true
    } catch (e: UnsatisfiedLinkError) {
        false
    } catch (e: SecurityException) {
        false
    }

    // --- engine lifecycle ----------------------------------------------------
    external fun nativeAbiVersion(): Int
    external fun nativeVersionString(): String
    external fun nativeLastError(): String
    external fun nativeErrorStr(code: Int): String

    external fun nativeCreateEngine(
        appName: String?,
        logLevel: Int,
        pageCapacity: Int,
        maxPages: Int,
        eventQueueCapacity: Int,
    ): Long

    external fun nativeDestroyEngine(handle: Long)

    external fun nativeStartSession(handle: Long, lscanDir: String?, profile: String?, record: Boolean): Int
    external fun nativeStopSession(handle: Long): Int
    external fun nativeEngineState(handle: Long): Int

    // --- devices ---------------------------------------------------------------
    /** Returns the device id (>= 0), or -1 on failure (see [nativeLastError]). */
    external fun nativeAddD6Device(
        handle: Long,
        serialPortName: String?,
        baud: Int,
        sendStartStop: Boolean,
        writer: SerialWriter?,
    ): Int

    external fun nativeRemoveDevice(handle: Long, deviceId: Int): Int
    external fun nativeDeviceHealth(handle: Long, deviceId: Int): NativeDeviceHealth?

    /** `buffer` must be `ByteBuffer.allocateDirect(...)` — zero-copy across the JNI boundary. */
    external fun nativePushSerialBytes(handle: Long, deviceId: Int, buffer: ByteBuffer, len: Int, tMonoNs: Long): Int

    // --- events ----------------------------------------------------------------
    external fun nativeStartEventPump(handle: Long, listener: EngineEventListener): Boolean
    external fun nativeStopEventPump(handle: Long)

    /**
     * Engine → app write callback for D6's start/stop command bytes
     * (`scan_serial_write_cb` in the C ABI). Implemented in
     * [com.lidarscan.app.usb.D6SerialConnection] over the actual
     * `UsbSerialPort`. Return `SCAN_OK` (0) or an `SCAN_ERR_*` value.
     */
    fun interface SerialWriter {
        fun write(data: ByteArray): Int
    }

    /**
     * Delivered from the native event-pump thread (see scanengine_jni.cpp's
     * `event_pump_loop`) — do not block, and do not call back into
     * [ScanEngineNative] synchronously from here (mirrors the C ABI's own
     * "do not call back into the engine" rule for push-mode callbacks).
     * `i0..i4`/`d0` are interpreted per [type] — see [EventType] and
     * scanengine_jni.cpp's marshalling switch for the mapping.
     */
    fun interface EngineEventListener {
        fun onEvent(type: Int, sequence: Int, tMonoNs: Long, i0: Long, i1: Long, i2: Long, i3: Long, i4: Long, d0: Double)
    }

    /** Mirrors `scan_error_t` (`scanengine_c.h`). */
    object ErrorCode {
        const val OK = 0
        const val UNKNOWN = 1
        const val INVALID_ARGUMENT = 2
        const val INVALID_STATE = 3
        const val NOT_FOUND = 4
        const val ALREADY_EXISTS = 5
        const val NOT_SUPPORTED = 6
        const val UNIMPLEMENTED = 7
        const val OUT_OF_MEMORY = 8
        const val CANCELLED = 9
        const val TIMEOUT = 10
        const val BUSY = 11
        const val AGAIN = 12
        const val CAPACITY_EXCEEDED = 13
        const val IO = 20
        const val DISCONNECTED = 21
        const val PERMISSION_DENIED = 22
        const val NETWORK = 23
        const val DEVICE_NOT_RESPONDING = 30
        const val PROTOCOL = 31
        const val CHECKSUM = 32
        const val DEVICE_FAULT = 33
        const val CORRUPT_DATA = 40
        const val VERSION_MISMATCH = 41
        const val FILE = 42
    }

    /** Mirrors `SCAN_DEVICE_*` (`scanengine_c.h`). */
    object DeviceKind {
        const val UNKNOWN = 0
        const val D6 = 1
        const val MID360 = 2
        const val RTK_ROVER = 3
    }

    /** Mirrors `SCAN_DEV_*` device state (`scanengine_c.h`) — what the health panel's "state" label reads. */
    object DeviceState {
        const val DISCONNECTED = 0
        const val IDLE = 1
        const val STARTING = 2
        const val STREAMING = 3
        const val DEGRADED = 4
        const val STOPPING = 5
        const val FAULT = 6

        fun label(state: Int): String = when (state) {
            DISCONNECTED -> "Disconnected"
            IDLE -> "Idle"
            STARTING -> "Starting"
            STREAMING -> "Streaming"
            DEGRADED -> "Degraded"
            STOPPING -> "Stopping"
            FAULT -> "Fault"
            else -> "Unknown ($state)"
        }
    }

    /** Mirrors `SCAN_EVENT_*` (`scanengine_c.h`). */
    object EventType {
        const val NONE = 0
        const val EVENTS_DROPPED = 1
        const val ENGINE_STATE = 10
        const val SESSION_STATE = 11
        const val DEVICE_STATE = 20
        const val DEVICE_HEALTH = 21
        const val POINTS_AVAILABLE = 30
        const val ROTATION = 31
        const val POSE_UPDATE = 40
        const val GNSS_FIX = 50
        const val JOB_PROGRESS = 60
        const val ERROR = 90
    }
}
