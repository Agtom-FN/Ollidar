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
) {
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
                ArCameraCharacteristicsProbe(
                    cameraId = cameraId,
                    timestampSourceIsRealtime = realtime,
                    timestampSourceKnown = source != null,
                    pixelArrayWidth = size?.width ?: 0,
                    pixelArrayHeight = size?.height ?: 0,
                )
            } catch (e: Exception) {
                Log.w(TAG, "camera characteristics unavailable for $cameraId: ${e.message}")
                null
            }
        }
    }
}
