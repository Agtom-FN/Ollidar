package com.lidarscan.app.ar

import android.content.Context
import android.util.Log
import android.view.Display
import com.google.ar.core.Config
import com.google.ar.core.Frame
import com.google.ar.core.Session
import com.google.ar.core.TrackingFailureReason
import com.google.ar.core.TrackingState
import com.google.ar.core.exceptions.CameraNotAvailableException
import com.google.ar.core.exceptions.UnavailableApkTooOldException
import com.google.ar.core.exceptions.UnavailableArcoreNotInstalledException
import com.google.ar.core.exceptions.UnavailableDeviceNotCompatibleException
import com.google.ar.core.exceptions.UnavailableSdkTooOldException
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import com.lidarscan.core.capture.PoseSample
import com.lidarscan.core.capture.RigMotionTracker
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Owns the ARCore session for a capture (or a calibration) and streams its
 * poses into the engine.
 *
 * ### Threading, which is the whole design
 *
 * ARCore's `Session.update()` must be called on the thread that owns the GL
 * context the camera texture lives in — for us, the `GLSurfaceView`'s render
 * thread ([ArCameraBackgroundRenderer]). So this controller does **not** own
 * a thread: it is *driven* from the render thread via [onFrame], and
 * everything it publishes is a `StateFlow` the UI collects. The engine calls
 * it makes (`scan_engine_push_pose`) are explicitly safe from that thread —
 * `scanengine_c.h`: "Safe from the AR thread while points are decoded on
 * another".
 *
 * ### Clock domain
 *
 * `Frame.getTimestamp()` is nanoseconds in the same base as
 * `SystemClock.elapsedRealtimeNanos()` (CLOCK_BOOTTIME) — which is already
 * the engine's own domain, so A4 installs a **passthrough** estimator for
 * `StreamId::kPoseAr` (A8 §3.5) and nothing here converts anything. The one
 * assumption that could break this is the camera's own timestamp source, and
 * [ArCameraCharacteristicsProbe] checks it once per session and logs loudly
 * on a mismatch rather than letting it pass silently.
 *
 * ### Tracking state -> pose quality
 *
 * Mapped in [poseQualityOf], to match the gating the engine already does
 * (`SCAN_POSE_GATE_TRACKING_LOST` / `_LOW_CONFIDENCE`, A8 §3.3): a pose
 * pushed while ARCore is not tracking carries `tracking_lost = 1`, so the
 * pushbroom assembler flags and (by default) excludes the points that
 * interpolate against it — Tech Spec §3.3's "points during ARCore tracking
 * loss are flagged and excluded by default", implemented by *telling the
 * truth about the pose* rather than by dropping it here.
 */
class CaptureArController(
    private val context: Context,
    private val installer: ArInstaller = ArInstaller(),
) {

    data class ArStatus(
        val availability: ArAvailability = ArAvailability.CHECKING,
        val sessionRunning: Boolean = false,
        val tracking: Boolean = false,
        val failureReason: TrackingFailureReason = TrackingFailureReason.NONE,
        val posesPushed: Long = 0,
        val posesRejected: Long = 0,
        val trackingLossEpisodes: Int = 0,
        val message: String? = null,
        /** Null until the first session has been configured. */
        val cameraProbe: ArCameraCharacteristicsProbe? = null,
    ) {
        val trackingHint: String?
            get() = when {
                tracking -> null
                failureReason == TrackingFailureReason.INSUFFICIENT_LIGHT -> "Too dark for AR tracking — add light"
                failureReason == TrackingFailureReason.EXCESSIVE_MOTION -> "Moving too fast — slow down"
                failureReason == TrackingFailureReason.INSUFFICIENT_FEATURES -> "Not enough texture in view — point at something with detail"
                failureReason == TrackingFailureReason.CAMERA_UNAVAILABLE -> "Camera unavailable — another app may be using it"
                failureReason == TrackingFailureReason.BAD_STATE -> "AR tracking is recovering…"
                else -> "AR is initialising…"
            }
    }

    private val _status = MutableStateFlow(ArStatus())
    val status: StateFlow<ArStatus> = _status.asStateFlow()

    /** The pose stream, also consumed by the wizard's hold-still check and B8's motion gate. */
    val motion = RigMotionTracker()

    @Volatile
    var session: Session? = null
        private set

    /** Set while a capture session is recording; poses are pushed into this engine handle. */
    @Volatile
    var engineHandle: Long = 0L

    /** The most recent frame, for consumers that need the image (B8) or the camera matrices (the overlay). */
    @Volatile
    var latestFrame: Frame? = null
        private set

    private val lastPoseNs = AtomicLong(Long.MIN_VALUE)
    private var wasTracking = false
    private var displayRotation = 0
    private var viewWidth = 1
    private var viewHeight = 1
    private var geometryDirty = true

    /**
     * Consumers of each frame, after its pose has been pushed: B8's keyframe
     * recorder and the calibration wizard's detector pump.
     *
     * A *list*, not a single slot, because both the Capture screen and the
     * wizard can be alive at once in the navigation back stack — with one
     * slot, whichever was created second would silently unsubscribe the
     * other, and worse, whichever was *cleared* second would null out a
     * listener it never installed. Copy-on-write: the list is written from
     * the UI thread and read from the GL thread on every frame.
     */
    private val frameListeners = java.util.concurrent.CopyOnWriteArrayList<(Frame) -> Unit>()

    fun addFrameListener(listener: (Frame) -> Unit) {
        frameListeners.addIfAbsent(listener)
    }

    fun removeFrameListener(listener: (Frame) -> Unit) {
        frameListeners.remove(listener)
    }

    fun refreshAvailability() {
        _status.value = _status.value.copy(availability = installer.availability(context))
    }

    /**
     * Creates the session if needed. Must be called before [resume]; safe to
     * call repeatedly. The caller is responsible for having camera permission
     * and for the [ArInstaller.requestInstall] dance — see [ArAvailability].
     */
    fun createSession(): Result<Session> {
        session?.let { return Result.success(it) }
        return try {
            val s = Session(context)
            val config = Config(s).apply {
                // LATEST_CAMERA_IMAGE, not BLOCKING: update() must not block
                // the render thread waiting for a frame, because the same
                // thread also drives the overlay's composition. A dropped
                // camera frame costs one overlay frame; a blocked one costs
                // the whole UI.
                updateMode = Config.UpdateMode.LATEST_CAMERA_IMAGE
                // AUTO focus: keyframes are for colour sampling, and a
                // fixed-focus frame of a near wall is unusable for that.
                // (The cost is that intrinsics drift slightly with focus,
                // which is why the intrinsics are re-read PER KEYFRAME rather
                // than cached once — see KeyframeRecorder.)
                focusMode = Config.FocusMode.AUTO
                // Planes are not used for the mount calibration (S6 finding 1:
                // ARCore plane detection is 3-6x over the whole colorization
                // budget on its own, which is why the target is a
                // checkerboard). Disabled to save the CPU it costs.
                planeFindingMode = Config.PlaneFindingMode.DISABLED
                lightEstimationMode = Config.LightEstimationMode.DISABLED
            }
            s.configure(config)
            session = s
            _status.value = _status.value.copy(
                availability = ArAvailability.READY,
                cameraProbe = ArCameraCharacteristicsProbe.probe(context, s),
                message = null,
            )
            Result.success(s)
        } catch (e: UnavailableArcoreNotInstalledException) {
            failWith(ArAvailability.NEEDS_INSTALL, e)
        } catch (e: UnavailableApkTooOldException) {
            failWith(ArAvailability.NEEDS_INSTALL, e)
        } catch (e: UnavailableSdkTooOldException) {
            failWith(ArAvailability.NEEDS_INSTALL, e)
        } catch (e: UnavailableDeviceNotCompatibleException) {
            failWith(ArAvailability.UNSUPPORTED, e)
        } catch (e: Exception) {
            failWith(ArAvailability.UNKNOWN, e)
        }
    }

    private fun failWith(availability: ArAvailability, e: Exception): Result<Session> {
        Log.w(TAG, "ARCore session creation failed", e)
        _status.value = _status.value.copy(
            availability = availability,
            sessionRunning = false,
            message = availability.message(),
        )
        return Result.failure(e)
    }

    fun resume(): Result<Unit> {
        val s = session ?: return Result.failure(IllegalStateException("no ARCore session"))
        return try {
            s.resume()
            geometryDirty = true
            _status.value = _status.value.copy(sessionRunning = true, message = null)
            Result.success(Unit)
        } catch (e: CameraNotAvailableException) {
            _status.value = _status.value.copy(
                sessionRunning = false,
                message = "Camera unavailable — close other camera apps and try again",
            )
            Result.failure(e)
        }
    }

    fun pause() {
        session?.pause()
        _status.value = _status.value.copy(sessionRunning = false, tracking = false)
    }

    fun close() {
        pause()
        session?.close()
        session = null
        latestFrame = null
        motion.clear()
        lastPoseNs.set(Long.MIN_VALUE)
        _status.value = ArStatus(availability = _status.value.availability)
    }

    /** Display geometry, from the `GLSurfaceView`'s `onSurfaceChanged` and the activity's display rotation. */
    fun setDisplayGeometry(display: Display?, width: Int, height: Int) {
        displayRotation = display?.rotation ?: displayRotation
        viewWidth = width.coerceAtLeast(1)
        viewHeight = height.coerceAtLeast(1)
        geometryDirty = true
    }

    /** The camera texture ARCore renders the background into — created by [ArCameraBackgroundRenderer]. */
    fun setCameraTextureName(textureId: Int) {
        session?.setCameraTextureName(textureId)
    }

    /**
     * One ARCore frame. Called from the GL render thread. Returns the frame so
     * the renderer can draw the background from the same object (ARCore
     * requires drawing the background with the frame that produced it — using
     * a stale one tears the image against the display transform).
     */
    fun onFrame(): Frame? {
        val s = session ?: return null
        if (geometryDirty) {
            s.setDisplayGeometry(displayRotation, viewWidth, viewHeight)
            geometryDirty = false
        }
        val frame = try {
            s.update()
        } catch (e: CameraNotAvailableException) {
            _status.value = _status.value.copy(sessionRunning = false, message = "Camera lost")
            return null
        }
        latestFrame = frame
        publishPose(frame)
        for (listener in frameListeners) listener(frame)
        return frame
    }

    private fun publishPose(frame: Frame) {
        val camera = frame.camera
        val trackingState = camera.trackingState
        val tracking = trackingState == TrackingState.TRACKING
        val timestamp = frame.timestamp
        if (timestamp <= 0L) return

        // The engine REJECTS an out-of-order pose (SCAN_ERR_INVALID_ARGUMENT,
        // "rather than silently corrupting every interpolation that follows"),
        // so a repeated frame timestamp — which LATEST_CAMERA_IMAGE can hand
        // out when the render thread outruns the camera — is filtered here
        // rather than sent and counted as a rejection.
        val previous = lastPoseNs.get()
        if (previous != Long.MIN_VALUE && timestamp <= previous) return
        lastPoseNs.set(timestamp)

        // getPose(), not getDisplayOrientedPose(): the display-oriented pose
        // is rotated to match the screen and is for rendering only. The
        // trajectory the engine records must be the physical camera's.
        val pose = camera.pose
        val position = Vec3(pose.tx().toDouble(), pose.ty().toDouble(), pose.tz().toDouble())
        val orientation = Quat(
            pose.qx().toDouble(), pose.qy().toDouble(), pose.qz().toDouble(), pose.qw().toDouble(),
        )
        motion.add(PoseSample(timestamp, position, orientation, tracking))

        val previousTracking = wasTracking
        wasTracking = tracking

        val handle = engineHandle
        var pushed = _status.value.posesPushed
        var rejected = _status.value.posesRejected
        if (handle != 0L) {
            val err = ScanEngineNative.nativePushPose(
                handle = handle,
                tMonoNs = timestamp,
                px = position.x, py = position.y, pz = position.z,
                qx = orientation.x, qy = orientation.y, qz = orientation.z, qw = orientation.w,
                positionSigmaM = positionSigmaFor(trackingState),
                orientationSigmaDeg = orientationSigmaFor(trackingState),
                quality = poseQualityOf(trackingState, camera.trackingFailureReason),
                trackingLost = !tracking,
                // Negative: let the engine derive confidence from
                // quality/tracking_lost. ARCore has no scalar confidence to
                // report, and manufacturing one here would be a number with no
                // measurement behind it.
                confidence = -1f,
            )
            if (err == ScanEngineNative.ErrorCode.OK) pushed++ else rejected++
        }

        _status.value = _status.value.copy(
            tracking = tracking,
            failureReason = camera.trackingFailureReason,
            posesPushed = pushed,
            posesRejected = rejected,
            trackingLossEpisodes = _status.value.trackingLossEpisodes +
                if (previousTracking && !tracking) 1 else 0,
        )
    }

    private companion object {
        const val TAG = "CaptureArController"

        /**
         * ARCore reports no per-pose covariance, so these are **stated
         * assumptions, not measurements** — carried into `scan_pose` so the
         * pushbroom's confidence gate has something monotone to work with,
         * and deliberately pessimistic while tracking is degraded. S6's own
         * budget attributes 11.4 px at 3 m to ARCore relative pose error
         * (WIZARD.md §4), which is the scale these are chosen around.
         */
        fun positionSigmaFor(state: TrackingState): Float = when (state) {
            TrackingState.TRACKING -> 0.02f
            TrackingState.PAUSED -> 0.20f
            TrackingState.STOPPED -> 1.0f
        }

        fun orientationSigmaFor(state: TrackingState): Float = when (state) {
            TrackingState.TRACKING -> 0.5f
            TrackingState.PAUSED -> 5.0f
            TrackingState.STOPPED -> 30.0f
        }

        /**
         * `TrackingState` + `TrackingFailureReason` -> `SCAN_POSE_QUALITY_*`.
         *
         * PAUSED with a *recoverable* reason maps to POOR rather than INVALID
         * because ARCore's last pose is still geometrically meaningful for a
         * moment (it is dead-reckoning), and the engine's own gate — plus the
         * `tracking_lost` flag pushed alongside — is what decides whether to
         * keep those points. STOPPED means the session is over; there is no
         * pose to trust at all.
         */
        fun poseQualityOf(state: TrackingState, reason: TrackingFailureReason): Int = when (state) {
            TrackingState.TRACKING -> ScanEngineNative.PoseQuality.GOOD
            TrackingState.PAUSED -> when (reason) {
                TrackingFailureReason.EXCESSIVE_MOTION,
                TrackingFailureReason.INSUFFICIENT_FEATURES,
                TrackingFailureReason.INSUFFICIENT_LIGHT,
                -> ScanEngineNative.PoseQuality.POOR
                else -> ScanEngineNative.PoseQuality.INVALID
            }
            TrackingState.STOPPED -> ScanEngineNative.PoseQuality.INVALID
        }
    }
}
