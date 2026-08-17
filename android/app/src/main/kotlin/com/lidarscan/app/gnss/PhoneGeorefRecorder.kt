package com.lidarscan.app.gnss

import android.util.Log
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.gnss.PhoneFix
import com.lidarscan.core.gnss.PhoneFixNmea
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ROUND 5.2: pushes phone-location fixes into the live engine so they are
 * **recorded into the `.lscan`** and fed to A10's georeferencing, exactly like a
 * rover's epochs.
 *
 * ### The seam, and why this is not a workaround
 *
 * `engine/capi/scanengine_c.h` exposes one GNSS *ingest* entry point —
 * `scan_engine_push_nmea(engine, device_id, bytes, len, t_mono_ns)` — reached
 * from Kotlin through the existing `nativeAddRtkRoverDevice` /
 * `nativePushNmea` JNI pair (B9, `gnss_jni.cpp`). There is no
 * "push a decoded fix" call, so the phone's `Location` is turned into the NMEA
 * burst the engine already parses ([PhoneFixNmea]). That is the *documented* path
 * rather than a side channel, and it buys three things a sidecar file would not:
 *
 *  1. **Record-always.** The bytes land in the `.lscan` as `kGnssNmea` chunks
 *     before anything parses them, so a re-process on a desktop sees the same
 *     input the phone did.
 *  2. **A4 time sync.** `t_mono_ns` is the fix's own
 *     `Location.elapsedRealtimeNanos` — CLOCK_BOOTTIME, already the engine's
 *     domain — so the epoch is placed in time by measurement, not by arrival.
 *  3. **Honest sigma end to end.** `Location.getAccuracy()` travels in GST, which
 *     is where A10's fusion reads its weights from.
 *
 * The **one** thing the C ABI cannot express is *which kind* of receiver a device
 * is: `scan_add_rtk_rover_device` is the only NMEA-capable device factory, so a
 * phone fix enters as a "rover". Recorded as an engine seam in
 * android/NOTES.md's ROUND 5 section (a `SCAN_DEVICE_PHONE_GNSS` kind, or a
 * source-tag field on the push, would let the engine and a later desktop tell a
 * 4 m phone epoch from a 2 cm rover epoch without inspecting the GGA quality
 * digit). Until then, the GGA quality digit is `1` (single) and the sigma is
 * metres — which is exactly how a desktop *can* tell them apart today, and why
 * [PhoneFixNmea] refuses to write anything better.
 */
class PhoneGeorefRecorder {

    private var engineHandle: Long = 0L
    private var deviceId: Int = -1

    /** Pushed-fix counters, for the diagnostics sheet. */
    @Volatile
    var fixesPushed: Long = 0L
        private set

    @Volatile
    var fixesRejected: Long = 0L
        private set

    /** Non-null when the engine had no NMEA device to give us and fixes are going to the sidecar instead. */
    @Volatile
    var sidecarFile: File? = null
        private set

    val isAttached: Boolean get() = engineHandle != 0L && deviceId >= 0

    /**
     * Attaches to a live capture session. Returns true when the engine accepted a
     * device; false means the fallback below (a sidecar) is in play, which is
     * still better than dropping the fixes.
     *
     * [projectDirectory] is only used for that fallback.
     */
    fun start(handle: Long, projectDirectory: File?): Boolean {
        stop()
        if (handle == 0L || !ScanEngineNative.isAvailable) {
            sidecarFile = openSidecar(projectDirectory)
            return false
        }
        engineHandle = handle
        deviceId = runCatching { ScanEngineNative.nativeAddRtkRoverDevice(handle) }.getOrDefault(-1)
        if (deviceId < 0) {
            Log.w(TAG, "scan_add_rtk_rover_device refused a device for the phone-GNSS fallback")
            engineHandle = 0L
            sidecarFile = openSidecar(projectDirectory)
            return false
        }
        return true
    }

    /**
     * One fix → one NMEA epoch. Safe to call from any thread that is not holding
     * an engine lock; `push_nmea` is documented as safe from a producer thread.
     */
    fun record(fix: PhoneFix) {
        val burst = PhoneFixNmea.burst(fix)
        val bytes = burst.toByteArray(Charsets.US_ASCII)

        val sidecar = sidecarFile
        if (sidecar != null) {
            runCatching { sidecar.appendText(burst) }
                .onSuccess { fixesPushed++ }
                .onFailure { fixesRejected++ }
            return
        }

        val handle = engineHandle
        val id = deviceId
        if (handle == 0L || id < 0) {
            fixesRejected++
            return
        }
        // A DIRECT buffer, per nativePushNmea's contract — a heap buffer has no
        // address the JNI side can read without a copy it does not do.
        val buffer = ByteBuffer.allocateDirect(bytes.size).order(ByteOrder.nativeOrder())
        buffer.put(bytes)
        buffer.rewind()
        val tMonoNs = if (fix.elapsedRealtimeNanos > 0) {
            fix.elapsedRealtimeNanos
        } else {
            android.os.SystemClock.elapsedRealtimeNanos()
        }
        val err = runCatching {
            ScanEngineNative.nativePushNmea(handle, id, buffer, bytes.size, tMonoNs)
        }.getOrDefault(-1)
        if (err == ScanEngineNative.ErrorCode.OK) fixesPushed++ else fixesRejected++
    }

    fun stop() {
        engineHandle = 0L
        deviceId = -1
        sidecarFile = null
    }

    /**
     * The documented fallback when the engine will not take a device (no native
     * library — the simulated-engine build — or `add_device` refused): the same
     * NMEA text, appended to `<project>/streams/phone_gnss.nmea`.
     *
     * Deliberately the *identical bytes*, so a later desktop can replay them
     * through the same parser; and deliberately a **sidecar with an obvious
     * name** rather than something that looks like an engine stream, because it
     * did not go through the engine's record-always path and its timestamps are
     * arrival stamps.
     */
    private fun openSidecar(projectDirectory: File?): File? {
        val dir = projectDirectory?.let { File(it, "streams") } ?: return null
        return runCatching {
            dir.mkdirs()
            File(dir, SIDECAR_NAME).also { if (!it.exists()) it.createNewFile() }
        }.getOrNull()
    }

    private companion object {
        const val TAG = "PhoneGeorefRecorder"
        const val SIDECAR_NAME = "phone_gnss.nmea"
    }
}
