package com.lidarscan.app.ar

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.util.Log
import com.google.ar.core.Frame
import com.google.ar.core.ImageMetadata
import com.google.ar.core.Session
import com.google.ar.core.exceptions.NotYetAvailableException

/**
 * Everything `frames.idx` needs about the camera that is not the pose — and,
 * more usefully, **a written record of what comes from ARCore and what needs
 * Camera2 interop**, since the B8 brief asks for exactly that distinction.
 *
 * ### From ARCore directly
 *
 * | field | source |
 * | --- | --- |
 * | `fx, fy, cx, cy`, `width`, `height` | `Frame.getCamera().getImageIntrinsics()` |
 * | `t_engine_ns` | `Frame.getTimestamp()` (CLOCK_BOOTTIME, start of exposure of row 0) |
 * | pose | `Camera.getPose()` |
 * | `exposure_ns` | `Frame.getImageMetadata()`, `SENSOR_EXPOSURE_TIME` |
 * | `iso` | same, `SENSOR_SENSITIVITY` |
 * | **`row_time_ns`** | same, **`SENSOR_ROLLING_SHUTTER_SKEW`** / (height − 1) |
 * | AE lock | same, `CONTROL_AE_LOCK` |
 *
 * The rolling-shutter row time is the interesting one: it is worth 6.8 px of
 * S6's 20.2 px colorization budget (WIZARD.md §4) and it is the field a naive
 * implementation writes as 0. It does **not** need Camera2 interop —
 * `com.google.ar.core.ImageMetadata` re-exposes the underlying
 * `CaptureResult` tags, including `SENSOR_ROLLING_SHUTTER_SKEW` (the duration
 * between the start of exposure of the first row and of the last row). Row
 * time is that skew divided by `height − 1`.
 *
 * ### What genuinely needs Camera2 interop
 *
 * Only the *static* characteristics, which are per-camera rather than
 * per-frame and therefore absent from `ImageMetadata`:
 *
 * * **`SENSOR_INFO_TIMESTAMP_SOURCE`** — whether the sensor stamps in
 *   `CLOCK_BOOTTIME` (`REALTIME`) or `CLOCK_MONOTONIC` (`UNKNOWN`). The whole
 *   engine clock domain rests on ARCore timestamps being BOOTTIME (A4/A8
 *   §3.5 install a *passthrough* estimator for `kPoseAr` on that basis), so
 *   this is checked once per session and logged loudly if it disagrees — it
 *   is the one assumption that, if wrong, silently mis-times every keyframe
 *   and every pushbroom point on the device.
 * * **`SENSOR_ORIENTATION` + `LENS_FACING`** (ROUND 9, owner item 35) — the
 *   camera is mounted at an angle to the display (usually 90 degrees), so the
 *   ARCore camera frame and the Android sensor/device frame that
 *   `TYPE_GYROSCOPE` reports in are NOT the same. This is the only thing on the
 *   device that says by how much, and it is what
 *   [com.lidarscan.core.capture.CameraFromImu] turns into the `camera_from_imu`
 *   quaternion the engine's IMU densifier needs.
 * * `SENSOR_INFO_PIXEL_ARRAY_SIZE` / `LENS_INFO_...` — context only.
 *
 * ### What nobody exposes
 *
 * **Distortion coefficients for the image ARCore hands out.** ARCore's
 * `CameraIntrinsics` describe an *already-rectified* image, so the correct
 * `distortion` for a keyframe from `acquireCameraImage()` is all-zero — which
 * is what B8 writes. `ImageMetadata.LENS_RADIAL_DISTORTION` exists, but it
 * describes the RAW sensor and attaching it to ARCore's rectified intrinsics
 * would double-correct. That is a deliberate zero, not a missing feature.
 */
data class ArCameraMetadata(
    val exposureNs: Long,
    val iso: Float,
    /** Per-row readout delay, ns. 0 means "unknown or global shutter" — see [rowTimeKnown]. */
    val rowTimeNs: Float,
    val rowTimeKnown: Boolean,
    val autoExposureLocked: Boolean,
) {
    val exposureKnown: Boolean get() = exposureNs > 0L || iso > 0f

    companion object {
        val UNKNOWN = ArCameraMetadata(0L, 0f, 0f, rowTimeKnown = false, autoExposureLocked = false)

        /**
         * Reads what this frame's `CaptureResult` carries. Every tag is
         * optional on a given device, so each is fetched independently and a
         * missing one degrades that field alone rather than the whole read.
         */
        fun of(frame: Frame, imageHeight: Int): ArCameraMetadata {
            val metadata: ImageMetadata = try {
                frame.imageMetadata
            } catch (e: NotYetAvailableException) {
                return UNKNOWN
            }

            fun long(tag: Int): Long? = try {
                metadata.getLong(tag)
            } catch (e: Exception) {
                null
            }

            fun int(tag: Int): Int? = try {
                metadata.getInt(tag)
            } catch (e: Exception) {
                null
            }

            fun byte(tag: Int): Byte? = try {
                metadata.getByte(tag)
            } catch (e: Exception) {
                null
            }

            val skewNs = long(ImageMetadata.SENSOR_ROLLING_SHUTTER_SKEW)
            val rows = (imageHeight - 1).coerceAtLeast(1)
            val rowTime = skewNs?.let { it.toDouble() / rows }

            return ArCameraMetadata(
                exposureNs = long(ImageMetadata.SENSOR_EXPOSURE_TIME) ?: 0L,
                iso = (int(ImageMetadata.SENSOR_SENSITIVITY) ?: 0).toFloat(),
                rowTimeNs = rowTime?.toFloat() ?: 0f,
                // A device that reports no skew is NOT thereby a global-shutter
                // device; it is a device that did not tell us. The flag keeps
                // those two apart, because frames.idx encodes 0 as "global
                // shutter" and a phone is never that.
                rowTimeKnown = rowTime != null && rowTime > 0.0,
                autoExposureLocked = (byte(ImageMetadata.CONTROL_AE_LOCK)?.toInt() ?: 0) != 0,
            )
        }
    }
}

/** The static, per-camera facts — the only part that needs Camera2 interop. */
data class ArCameraCharacteristicsProbe(
    val cameraId: String,
    /** True when the sensor stamps in CLOCK_BOOTTIME, which is what the engine's clock domain assumes. */
    val timestampSourceIsRealtime: Boolean,
    val timestampSourceKnown: Boolean,
    val pixelArrayWidth: Int,
    val pixelArrayHeight: Int,
    /**
     * ROUND 9 (item 35): `CameraCharacteristics.SENSOR_ORIENTATION`, degrees
     * clockwise, or null when the tag is absent. Feeds
     * [com.lidarscan.core.capture.CameraFromImu.resolve].
     */
    val sensorOrientationDeg: Int? = null,
    /** True when `LENS_FACING` is FRONT — `camera_from_imu` is not derivable from SENSOR_ORIENTATION alone there. */
    val lensFacingFront: Boolean = false,
    /**
     * ROUND 20 (item 81) — the FACTORY camera↔IMU calibration, verbatim from
     * `CameraCharacteristics`, or nulls where the device does not carry the
     * tags (every emulator, and plenty of budget hardware):
     *
     *  * `LENS_POSE_ROTATION` — per-unit quaternion `(x, y, z, w)`;
     *  * `LENS_POSE_TRANSLATION` — metres, the lever arm the rotation is
     *    solved with (recorded for the manifest; the densifier's C ABI takes
     *    rotation only);
     *  * `LENS_POSE_REFERENCE` — 0 = PRIMARY_CAMERA, 1 = GYROSCOPE, 2 =
     *    UNDEFINED; the gyroscope reference is the one the densifier wants;
     *  * `LENS_INTRINSIC_CALIBRATION` — `[fx, fy, cx, cy, s]`, recorded so a
     *    future colorize pass can prefer the factory intrinsics.
     */
    val lensPoseRotationXyzw: DoubleArray? = null,
    val lensPoseTranslationM: DoubleArray? = null,
    val lensPoseReference: Int? = null,
    val lensIntrinsicCalibration: DoubleArray? = null,
) {
    /**
     * `camera_from_imu` for this camera, or an identity-with-a-reason. Computed
     * rather than stored so the derivation lives in exactly one place
     * ([com.lidarscan.core.capture.CameraFromImu], which is plain-JVM testable).
     *
     * ROUND 20 (item 81): prefers the factory LENS_POSE_ROTATION when present
     * and convention-consistent; falls back to the coarse SENSOR_ORIENTATION
     * rotation otherwise — the `why` names which source is in force, and
     * `startPhoneImu`'s log line carries it into the capture log.
     */
    val cameraFromImu: com.lidarscan.core.capture.CameraFromImuExtrinsics
        get() = com.lidarscan.core.capture.CameraFromImu.resolveWithFactory(
            lensPoseRotationXyzw,
            sensorOrientationDeg,
            lensFacingFront,
        )

    companion object {
        private const val TAG = "ArCameraProbe"

        fun probe(context: Context, session: Session): ArCameraCharacteristicsProbe? {
            val cameraId = try {
                session.cameraConfig.cameraId
            } catch (e: Exception) {
                return null
            }
            val manager = context.getSystemService(Context.CAMERA_SERVICE) as? CameraManager ?: return null
            return try {
                val characteristics = manager.getCameraCharacteristics(cameraId)
                val source = characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE)
                val size = characteristics.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
                val realtime = source == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
                if (source != null && !realtime) {
                    // Loud, because this invalidates an assumption the whole
                    // pipeline is built on rather than degrading one field.
                    Log.w(
                        TAG,
                        "Camera $cameraId stamps in SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN (CLOCK_MONOTONIC), " +
                            "not REALTIME. ARCore frame timestamps may not share the engine's CLOCK_BOOTTIME " +
                            "domain, which A4/A8 assume for StreamId.kPoseAr.",
                    )
                }
                // ROUND 9 (item 35). Same posture as the timestamp source: read
                // it once per session, and if it is missing say so rather than
                // assume the usual 90 — an assumed extrinsic would rotate every
                // integrated gyro increment 90 degrees off, which bends the
                // densified path sideways instead of following the walk.
                val orientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION)
                val facing = characteristics.get(CameraCharacteristics.LENS_FACING)
                val front = facing == CameraCharacteristics.LENS_FACING_FRONT
                if (orientation == null) {
                    Log.w(
                        TAG,
                        "Camera $cameraId reports no SENSOR_ORIENTATION — camera_from_imu cannot be " +
                            "derived, so the IMU densifier will run on IDENTITY and its path shape " +
                            "will be degraded (StreamId phone-imu).",
                    )
                }
                // ROUND 20 (item 81): the factory calibration tags, each read
                // independently so a missing one degrades that field alone —
                // the same posture as ArCameraMetadata.of.
                val poseRotation = runCatching {
                    characteristics.get(CameraCharacteristics.LENS_POSE_ROTATION)
                }.getOrNull()
                val poseTranslation = runCatching {
                    characteristics.get(CameraCharacteristics.LENS_POSE_TRANSLATION)
                }.getOrNull()
                val poseReference = runCatching {
                    characteristics.get(CameraCharacteristics.LENS_POSE_REFERENCE)
                }.getOrNull()
                val intrinsicCalibration = runCatching {
                    characteristics.get(CameraCharacteristics.LENS_INTRINSIC_CALIBRATION)
                }.getOrNull()
                if (poseRotation != null) {
                    Log.i(
                        TAG,
                        "Camera $cameraId carries factory LENS_POSE_ROTATION " +
                            "(reference=${poseReference ?: "unreported"}) — the densifier " +
                            "prefers it over the coarse SENSOR_ORIENTATION rotation.",
                    )
                }
                ArCameraCharacteristicsProbe(
                    cameraId = cameraId,
                    timestampSourceIsRealtime = realtime,
                    timestampSourceKnown = source != null,
                    pixelArrayWidth = size?.width ?: 0,
                    pixelArrayHeight = size?.height ?: 0,
                    sensorOrientationDeg = orientation,
                    lensFacingFront = front,
                    lensPoseRotationXyzw = poseRotation
                        ?.takeIf { it.size == 4 }
                        ?.map { it.toDouble() }?.toDoubleArray(),
                    lensPoseTranslationM = poseTranslation
                        ?.takeIf { it.size == 3 }
                        ?.map { it.toDouble() }?.toDoubleArray(),
                    lensPoseReference = poseReference,
                    lensIntrinsicCalibration = intrinsicCalibration
                        ?.map { it.toDouble() }?.toDoubleArray(),
                )
            } catch (e: Exception) {
                Log.w(TAG, "camera characteristics unavailable for $cameraId: ${e.message}")
                null
            }
        }
    }
}
