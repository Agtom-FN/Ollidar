package com.lidarscan.app.ar

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 9, owner item 35 (B3) — the app-side half of the `camera_from_imu`
 * wiring: that [ArCameraCharacteristicsProbe]'s `SENSOR_ORIENTATION` /
 * `LENS_FACING` fields actually reach
 * [com.lidarscan.core.capture.CameraFromImu], which is where the derivation
 * itself is proved (`PhoneImuTest`, in `:core`).
 *
 * A plain JVM test: the probe is a data class, and nothing here calls into the
 * Camera2 framework — `probe()` itself needs a real `CameraManager` and a live
 * ARCore `Session`, neither of which exists off-device (and the capture AVD has
 * no real IMU either, so an instrumented test would prove nothing extra).
 */
class ArCameraImuExtrinsicsTest {

    private fun probe(orientation: Int?, front: Boolean = false) = ArCameraCharacteristicsProbe(
        cameraId = "0",
        timestampSourceIsRealtime = true,
        timestampSourceKnown = true,
        pixelArrayWidth = 4032,
        pixelArrayHeight = 3024,
        sensorOrientationDeg = orientation,
        lensFacingFront = front,
    )

    @Test
    fun `the usual rear camera at 90 degrees yields a derived Rz plus 90`() {
        val e = probe(90).cameraFromImu
        assertTrue(e.derived)
        // Rz(+90) as (x, y, z, w).
        assertEquals(0.0, e.quat.x, 1e-12)
        assertEquals(0.0, e.quat.y, 1e-12)
        assertEquals(0.70710678, e.quat.z, 1e-7)
        assertEquals(0.70710678, e.quat.w, 1e-7)
        assertEquals(4, e.toXyzw().size)
    }

    @Test
    fun `a missing SENSOR_ORIENTATION degrades to a warned identity, not a guessed 90`() {
        // The whole point of B3's "do not silently push a guess": 90 is the
        // common value, and assuming it on a device that did not report it
        // would be indistinguishable in any log from having measured it.
        val e = probe(null).cameraFromImu
        assertFalse(e.derived)
        assertEquals(0.0, e.quat.z, 1e-12)
        assertEquals(1.0, e.quat.w, 1e-12)
        assertTrue(e.why.isNotBlank())
    }

    @Test
    fun `a front-facing AR session is not derived from SENSOR_ORIENTATION alone`() {
        val e = probe(270, front = true).cameraFromImu
        assertFalse(e.derived)
        assertTrue(e.why.contains("front-facing"))
    }

    @Test
    fun `the probe defaults keep every pre-ROUND-9 construction site compiling and honest`() {
        // The two new fields default to "unknown", so any caller that has not
        // been taught about them reports NOT derived rather than a fabricated
        // extrinsic.
        val old = ArCameraCharacteristicsProbe(
            cameraId = "0",
            timestampSourceIsRealtime = true,
            timestampSourceKnown = true,
            pixelArrayWidth = 0,
            pixelArrayHeight = 0,
        )
        assertFalse(old.cameraFromImu.derived)
    }
}
