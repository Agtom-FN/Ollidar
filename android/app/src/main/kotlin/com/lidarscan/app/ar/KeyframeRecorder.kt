package com.lidarscan.app.ar

import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.media.Image
import android.util.Log
import com.google.ar.core.Frame
import com.google.ar.core.TrackingState
import com.google.ar.core.exceptions.NotYetAvailableException
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.capture.KeyframeSelector
import com.lidarscan.core.capture.RigMotionTracker
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * **B8**: the camera-keyframe pipeline. ARCore's own camera frames at a 2–5 fps
 * motion-gated cadence, JPEG-encoded into `streams/frames/`, indexed in
 * `frames.idx`.
 *
 * ### No CameraX, no second camera client
 *
 * Tech Spec §3.5 says "CameraX shared with ARCore session". In practice that
 * means one of two things on ARCore: a *shared camera* session
 * (`Session.createForSharedCamera`, where the app opens the `CameraDevice`
 * itself and hands ARCore a surface), or ARCore's own
 * `Frame.acquireCameraImage()`. This uses the latter, for three reasons:
 *
 *  1. **It is the same image ARCore tracked with**, so its timestamp, pose and
 *     intrinsics are the ones ARCore actually computed — no correlation
 *     between two streams, no second clock to reconcile. Given that S6's
 *     budget is 83% time-sync dominated (WIZARD.md §4), adding a second
 *     camera pipeline to save nothing would be an odd trade.
 *  2. **The metadata B8 needs is already there.** `Frame.getImageMetadata()`
 *     re-exposes the `CaptureResult` tags, *including*
 *     `SENSOR_ROLLING_SHUTTER_SKEW` — see [ArCameraMetadata] for the full
 *     ARCore-vs-Camera2 breakdown. Shared camera would buy access to the same
 *     tags at the cost of owning a `CameraCaptureSession`.
 *  3. Shared camera mode is materially more device-specific, and **no ARCore
 *     device was available to validate it here.**
 *
 * The cost is honest: `acquireCameraImage()` returns the CPU image at the
 * session's *image* resolution (`CameraConfig.getImageSize()`, typically
 * 640x480 or 1280x720), not a full-resolution still. For colorization —
 * sampling a colour per point — that is adequate and is what the 2–5 fps
 * cadence assumes; a higher-resolution keyframe path is a shared-camera
 * follow-up, and it is a resolution decision, not an architecture one.
 *
 * ### The cadence and the gate
 *
 * [KeyframeSelector] (in `:core`, and unit-tested there) owns both: a 3 fps
 * default inside §3.5's 2–5 fps, and S6's motion gate — refuse anything over
 * 15 °/s, and within each slot take the *slowest* frame rather than the
 * first. Angular rate and linear speed come from [RigMotionTracker]'s finite
 * differences over the ARCore pose stream, and are recorded per keyframe so
 * the colorizer can re-apply the gate offline from the `.lscan` alone
 * (A11 §3.3 item 4).
 *
 * ### Threading
 *
 * [onFrame] runs on the ARCore/GL thread and does the cheap part: gate, then
 * copy the Y/U/V planes out of the `Image` and close it immediately —
 * ARCore's image reader has a small pool and holding one stalls tracking. The
 * expensive part (JPEG encode + two file writes + the JNI index append) runs
 * on a single-threaded encoder executor, which also gives the native
 * `KeyframeWriter` the "one thread at a time" contract it documents.
 */
class KeyframeRecorder(
    private val motion: RigMotionTracker,
    private val selector: KeyframeSelector = KeyframeSelector(targetFps = DEFAULT_FPS),
    private val jpegQuality: Int = 85,
) {

    data class Stats(
        val recording: Boolean = false,
        val keyframesWritten: Int = 0,
        val framesConsidered: Long = 0,
        val skippedMotion: Long = 0,
        val skippedNoImage: Long = 0,
        val rejectedByFormat: Int = 0,
        val bytesWritten: Long = 0,
        val lastError: String? = null,
        /** True once a device has reported a rolling-shutter skew; false means row_time was written as 0. */
        val rollingShutterKnown: Boolean = false,
    )

    private val _stats = MutableStateFlow(Stats())
    val stats: StateFlow<Stats> = _stats.asStateFlow()

    private val encoder = Executors.newSingleThreadExecutor { r ->
        Thread(r, "lidarscan-keyframe-encoder").apply { priority = Thread.NORM_PRIORITY - 1 }
    }
    private val running = AtomicBoolean(false)
    private val nextIndex = AtomicInteger(0)

    /**
     * Redesign: the Capture-settings sheet's camera-keyframes switch. Read on
     * the ARCore/GL thread in [onFrame] and written from the UI thread, hence
     * atomic.
     */
    private val keyframesEnabled = AtomicBoolean(true)

    /** Turns the keyframe pipeline on/off mid-session without stopping the recorder. */
    fun setEnabled(enabled: Boolean) {
        val was = keyframesEnabled.getAndSet(enabled)
        if (!was && enabled) selector.reset()
    }

    /** §3.5's 2–5 fps cadence, changeable from the sheet while recording. */
    fun setTargetFps(fps: Double) = selector.setTargetFps(fps)

    @Volatile private var writerHandle: Long = 0L
    @Volatile private var framesDir: File? = null

    /**
     * Opens `streams/frames/` in [lscanDir] and the index writer. The index
     * FILE is created lazily on the first record (A11 §3.1), so a session that
     * never produces a keyframe leaves none behind and a reader's `kNotFound`
     * genuinely means "this session had no camera".
     */
    fun start(lscanDir: File): Result<Unit> {
        if (running.get()) return Result.success(Unit)
        if (!ScanEngineNative.isAvailable) {
            return Result.failure(IllegalStateException("scanengine_jni not loaded — keyframes need the native writer"))
        }
        val handle = ScanEngineNative.nativeKeyframeWriterOpen(lscanDir.absolutePath)
        if (handle == 0L) {
            return Result.failure(IllegalStateException("could not open frames.idx in $lscanDir"))
        }
        writerHandle = handle
        framesDir = File(File(lscanDir, "streams"), "frames").apply { mkdirs() }
        nextIndex.set(0)
        selector.reset()
        running.set(true)
        _stats.value = Stats(recording = true)
        return Result.success(Unit)
    }

    /**
     * Flushes and closes the index. Does NOT block: the flush/close is queued
     * behind whatever is still encoding, on the same single-threaded executor,
     * so the last keyframes are written before the handle is destroyed — but
     * the caller returns immediately rather than stalling the UI thread on a
     * JPEG encode. A crash in that window loses at most the queued frames,
     * and `frames.idx` is a chunked stream whose truncated tail is a normal,
     * handled outcome (A5's truncated-tail rule) rather than corruption.
     */
    fun stop() {
        if (!running.getAndSet(false)) return
        val handle = writerHandle
        writerHandle = 0L
        // Drain on the encoder thread itself, so anything already queued is
        // written before the handle is destroyed.
        encoder.execute {
            if (handle != 0L) {
                ScanEngineNative.nativeKeyframeWriterFlush(handle)
                ScanEngineNative.nativeKeyframeWriterClose(handle)
            }
        }
        _stats.value = _stats.value.copy(recording = false)
    }

    fun shutdown() {
        stop()
        encoder.shutdown()
    }

    /**
     * One ARCore frame, from the GL thread. Cheap path only — see the class
     * doc's threading note.
     */
    fun onFrame(frame: Frame) {
        if (!running.get()) return
        // Redesign: the Capture-settings sheet's "Camera keyframes" switch.
        // Gating here rather than detaching the listener is deliberate — the
        // ARCore frame callback is shared with the pose pump, and the count
        // must FREEZE where it stood rather than be rewritten or reset when
        // the switch goes off (round 3's resolution, verbatim). Coming back on
        // resets the selector's slot so the first frame after is not judged
        // against a deadline from before the gap.
        if (!keyframesEnabled.get()) return
        val handle = writerHandle
        if (handle == 0L) return

        val camera = frame.camera
        val tracking = camera.trackingState == TrackingState.TRACKING
        val timestamp = frame.timestamp
        val estimate = motion.estimateAt(timestamp)

        _stats.value = _stats.value.copy(framesConsidered = _stats.value.framesConsidered + 1)

        val candidate = selector.offer(
            tMonoNs = timestamp,
            tracking = tracking,
            angularRateRadPerS = estimate.angularRateRadPerS.toDouble(),
            linearSpeedMPerS = estimate.linearSpeedMPerS.toDouble(),
            motionValid = estimate.valid,
        )
        if (candidate == null) {
            if (estimate.valid && !selector.withinMotionGate(estimate.angularRateRadPerS.toDouble())) {
                _stats.value = _stats.value.copy(skippedMotion = _stats.value.skippedMotion + 1)
            }
            return
        }
        // The selector may hand back a candidate from EARLIER in the slot,
        // whose Image is long gone (ARCore only ever holds the current one).
        // Recording the current frame's image against that candidate's
        // timestamp would be a lie about when the picture was taken — so the
        // motion figures travel with the candidate and the image, pose,
        // intrinsics and time all come from THIS frame.
        val image: Image = try {
            frame.acquireCameraImage()
        } catch (e: NotYetAvailableException) {
            _stats.value = _stats.value.copy(skippedNoImage = _stats.value.skippedNoImage + 1)
            return
        } catch (e: Exception) {
            _stats.value = _stats.value.copy(lastError = "acquireCameraImage: ${e.message}")
            return
        }

        val payload: Nv21Frame? = try {
            if (image.format != ImageFormat.YUV_420_888) {
                _stats.value = _stats.value.copy(
                    rejectedByFormat = _stats.value.rejectedByFormat + 1,
                    lastError = "unexpected camera image format ${image.format}",
                )
                null
            } else {
                Nv21Frame.from(image)
            }
        } finally {
            // Immediately: ARCore's reader pool is small and a held image
            // stalls tracking, which is the one thing worse than a missed
            // keyframe.
            image.close()
        }
        if (payload == null) return

        val intrinsics = camera.imageIntrinsics
        val focal = intrinsics.focalLength
        val principal = intrinsics.principalPoint
        val dimensions = intrinsics.imageDimensions
        val metadata = ArCameraMetadata.of(frame, dimensions[1])
        val pose = camera.pose

        var flags = 0
        if (estimate.valid) flags = flags or ScanEngineNative.KeyframeFlags.MOTION_VALID
        if (metadata.exposureKnown) flags = flags or ScanEngineNative.KeyframeFlags.EXPOSURE_VALID
        if (!tracking) flags = flags or ScanEngineNative.KeyframeFlags.TRACKING_LOST
        if (metadata.autoExposureLocked) flags = flags or ScanEngineNative.KeyframeFlags.AUTO_EXPOSURE_LOCKED

        val record = PendingKeyframe(
            index = nextIndex.getAndIncrement(),
            nv21 = payload,
            // `candidate` chose WHEN to sample; this frame supplies WHAT was
            // sampled. They are the same frame in the common case (the slot's
            // slowest frame is usually its last), and when they differ the
            // timestamp recorded is this frame's, never the candidate's.
            tEngineNs = timestamp,
            exposureNs = metadata.exposureNs,
            iso = metadata.iso,
            rowTimeNs = metadata.rowTimeNs,
            rowTimeKnown = metadata.rowTimeKnown,
            px = pose.tx().toDouble(), py = pose.ty().toDouble(), pz = pose.tz().toDouble(),
            qx = pose.qx().toDouble(), qy = pose.qy().toDouble(),
            qz = pose.qz().toDouble(), qw = pose.qw().toDouble(),
            fx = focal[0], fy = focal[1], cx = principal[0], cy = principal[1],
            width = dimensions[0], height = dimensions[1],
            poseQuality = if (tracking) {
                ScanEngineNative.PoseQuality.GOOD
            } else {
                ScanEngineNative.PoseQuality.POOR
            },
            trackingLost = !tracking,
            flags = flags,
            angularRateRadPerS = estimate.angularRateRadPerS,
            linearSpeedMPerS = estimate.linearSpeedMPerS,
        )
        encoder.execute { write(handle, record) }
    }

    private fun write(handle: Long, record: PendingKeyframe) {
        val dir = framesDir ?: return
        val name = "kf_%06d.jpg".format(record.index)
        val file = File(dir, name)

        val jpeg = ByteArrayOutputStream(64 * 1024)
        val yuv = YuvImage(record.nv21.bytes, ImageFormat.NV21, record.nv21.width, record.nv21.height, null)
        if (!yuv.compressToJpeg(Rect(0, 0, record.nv21.width, record.nv21.height), jpegQuality, jpeg)) {
            _stats.value = _stats.value.copy(lastError = "JPEG encode failed for $name")
            return
        }
        val bytes = jpeg.toByteArray()
        try {
            file.writeBytes(bytes)
        } catch (e: Exception) {
            _stats.value = _stats.value.copy(lastError = "write $name: ${e.message}")
            return
        }

        val error = ScanEngineNative.nativeKeyframeWriterAdd(
            handle = handle,
            tEngineNs = record.tEngineNs,
            exposureNs = record.exposureNs,
            px = record.px, py = record.py, pz = record.pz,
            qx = record.qx, qy = record.qy, qz = record.qz, qw = record.qw,
            fx = record.fx, fy = record.fy, cx = record.cx, cy = record.cy,
            // ARCore's intrinsics describe an ALREADY-RECTIFIED image, so the
            // correct coefficients here are zero. See ArCameraMetadata's
            // "What nobody exposes" note — this is a deliberate zero.
            distortion = FloatArray(5),
            width = record.width,
            height = record.height,
            // 0 means "global shutter" to the format. A phone never is one, so
            // 0 here is strictly "this device did not report a skew", and the
            // stats flag surfaces that rather than letting it look like a
            // corrected frame.
            rowTimeNs = if (record.rowTimeKnown) record.rowTimeNs else 0f,
            positionSigmaM = if (record.trackingLost) 0.20f else 0.02f,
            orientationSigmaDeg = if (record.trackingLost) 5.0f else 0.5f,
            poseQuality = record.poseQuality,
            trackingLost = record.trackingLost,
            poseSource = ScanEngineNative.StreamId.POSE_AR,
            flags = record.flags,
            iso = record.iso,
            angularRateRadPerS = record.angularRateRadPerS,
            linearSpeedMPerS = record.linearSpeedMPerS,
            imageBytes = bytes.size,
            imageName = name,
        )
        if (error != null) {
            // The record was refused by validate_keyframe() — delete the
            // orphan JPEG rather than leave an image no index points at.
            file.delete()
            Log.w(TAG, "keyframe rejected: $error")
            _stats.value = _stats.value.copy(lastError = error)
            return
        }

        _stats.value = _stats.value.copy(
            keyframesWritten = _stats.value.keyframesWritten + 1,
            bytesWritten = _stats.value.bytesWritten + bytes.size,
            rollingShutterKnown = _stats.value.rollingShutterKnown || record.rowTimeKnown,
            lastError = null,
        )
    }

    private class PendingKeyframe(
        val index: Int,
        val nv21: Nv21Frame,
        val tEngineNs: Long,
        val exposureNs: Long,
        val iso: Float,
        val rowTimeNs: Float,
        val rowTimeKnown: Boolean,
        val px: Double, val py: Double, val pz: Double,
        val qx: Double, val qy: Double, val qz: Double, val qw: Double,
        val fx: Float, val fy: Float, val cx: Float, val cy: Float,
        val width: Int, val height: Int,
        val poseQuality: Int,
        val trackingLost: Boolean,
        val flags: Int,
        val angularRateRadPerS: Float,
        val linearSpeedMPerS: Float,
    )

    companion object {
        private const val TAG = "KeyframeRecorder"

        /** Inside Tech Spec §3.5's 2–5 fps. 3 leaves headroom for the motion gate to discard a slot without starving the index. */
        const val DEFAULT_FPS = 3.0
    }
}

/**
 * A YUV_420_888 camera image copied into the interleaved NV21 layout
 * [YuvImage] wants.
 *
 * `YuvImage` is used rather than a `ImageReader` configured for JPEG because
 * ARCore owns the camera and hands out YUV — there is no JPEG-format reader to
 * attach without shared-camera mode. `YuvImage.compressToJpeg` is a platform
 * encoder (libjpeg under the hood), so this is not a software fallback so much
 * as the normal path for "I have YUV planes and want a JPEG".
 *
 * The plane copy respects both `rowStride` and `pixelStride`: a semi-planar
 * device hands out interleaved VU with `pixelStride == 2` and copying it as if
 * it were planar produces the classic green-and-magenta image.
 */
class Nv21Frame private constructor(val bytes: ByteArray, val width: Int, val height: Int) {
    companion object {
        fun from(image: Image): Nv21Frame? {
            val width = image.width
            val height = image.height
            if (width <= 0 || height <= 0) return null
            val out = ByteArray(width * height * 3 / 2)

            val y = image.planes[0]
            copyPlane(y.buffer, y.rowStride, y.pixelStride, width, height, out, 0, 1)

            // NV21 is Y then interleaved V,U.
            val u = image.planes[1]
            val v = image.planes[2]
            val chromaWidth = width / 2
            val chromaHeight = height / 2
            copyPlane(v.buffer, v.rowStride, v.pixelStride, chromaWidth, chromaHeight, out, width * height, 2)
            copyPlane(u.buffer, u.rowStride, u.pixelStride, chromaWidth, chromaHeight, out, width * height + 1, 2)

            return Nv21Frame(out, width, height)
        }

        private fun copyPlane(
            buffer: java.nio.ByteBuffer,
            rowStride: Int,
            pixelStride: Int,
            width: Int,
            height: Int,
            out: ByteArray,
            outOffset: Int,
            outPixelStride: Int,
        ) {
            val row = ByteArray(rowStride)
            var outIndex = outOffset
            buffer.rewind()
            for (r in 0 until height) {
                val remaining = buffer.remaining()
                if (remaining <= 0) return
                val toRead = minOf(rowStride, remaining)
                buffer.get(row, 0, toRead)
                if (pixelStride == 1 && outPixelStride == 1) {
                    System.arraycopy(row, 0, out, outIndex, minOf(width, toRead))
                    outIndex += width
                } else {
                    var i = 0
                    var c = 0
                    while (c < width && i < toRead) {
                        out[outIndex] = row[i]
                        outIndex += outPixelStride
                        i += pixelStride
                        c++
                    }
                }
            }
        }
    }
}

/** The luma plane of an ARCore camera image as a `:core` [com.lidarscan.core.calib.LumaImage], for checkerboard detection. */
fun Image.toLumaImage(): com.lidarscan.core.calib.LumaImage? {
    if (format != ImageFormat.YUV_420_888) return null
    val plane = planes[0]
    val buffer = plane.buffer
    val out = ByteArray(width * height)
    val row = ByteArray(plane.rowStride)
    buffer.rewind()
    var outIndex = 0
    for (r in 0 until height) {
        if (buffer.remaining() <= 0) break
        val toRead = minOf(plane.rowStride, buffer.remaining())
        buffer.get(row, 0, toRead)
        if (plane.pixelStride == 1) {
            System.arraycopy(row, 0, out, outIndex, minOf(width, toRead))
        } else {
            var i = 0
            var c = 0
            while (c < width && i < toRead) {
                out[outIndex + c] = row[i]
                i += plane.pixelStride
                c++
            }
        }
        outIndex += width
    }
    return com.lidarscan.core.calib.LumaImage(width, height, out)
}
