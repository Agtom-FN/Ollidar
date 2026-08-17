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

    /**
     * ROUND 5 AUDIT bugfix (field report: "the AR camera not show up").
     *
     * There are two possible drivers of this session — [ArPosePumpView]'s 2 dp
     * `GLSurfaceView` (mounted in every other view mode) and [ArOverlayView]'s
     * own, full-screen `GLSurfaceView` (mounted in [CameraMode.AR]) — and the
     * class doc above has always said "never composed at the same time...
     * the Capture screen picks exactly one". That is true of *what Compose
     * decides to compose*, but it was never true of *the underlying GL
     * threads' lifecycle*: `AndroidView`'s `factory`/`onRelease` pair runs on
     * the Compose/main thread, but each `GLSurfaceView`'s `onSurfaceCreated`
     * fires asynchronously, on ITS OWN GL thread, whenever the platform
     * actually hands it a `Surface` — there was no guarantee the OLD
     * renderer's thread had genuinely stopped calling `Session.update()`/
     * `setCameraTextureName()` before the NEW one's thread started calling
     * them, right across a [CameraMode] switch. `Session.update()` from two
     * threads is undefined by ARCore's own contract; concretely here, the
     * pump's `RENDERMODE_CONTINUOUSLY` thread re-binds ITS OWN texture id on
     * every single frame (`ArCameraBackgroundRenderer.onDrawFrame`), so if it
     * won even one more race after the overlay claimed the session, ARCore
     * would keep writing the camera image into the (now off-screen, 2 dp)
     * pump's texture forever — the overlay's own texture is simply never
     * written, which reads as "camera not show up", not a crash or a glitch.
     *
     * [claimRenderer]/[releaseRenderer] make ownership explicit instead of
     * implicit-by-Compose-branch: whichever renderer is granted [activeOwner]
     * is the only one whose [onFrame]/[setCameraTextureName] calls actually
     * touch [session] — a stale renderer's calls are honoured right up until
     * a newer claim lands, and are silently ignored afterwards, so the
     * hand-off is safe regardless of which GL thread's `onSurfaceCreated`
     * happens to fire first or last. Ownership is granted from the SAME
     * `AndroidView` `factory`/`onRelease` lifecycle each view already had —
     * see [ArPosePumpView]/[ArOverlayView] — which runs on the main thread
     * and is exactly as ordered as Compose's own mount/unmount already is;
     * only the ENFORCEMENT of "one at a time" moved from an assumption to a
     * runtime check.
     */
    /**
     * ROUND 6 (owner item 19): ownership AND lifecycle now live in one
     * plain-Kotlin state machine, [ArSessionGate] — see its header for the
     * three concrete ways enabling the AR overlay used to take the process
     * down, and why the state machine is a separate, JVM-testable class rather
     * than more `@Volatile` fields in here.
     *
     * This typealias keeps every existing call site (`ArPosePumpView`,
     * `ArOverlayView`, `ArCameraBackgroundRenderer`, `MountCalibrationScreen`)
     * reading the same way it did in ROUND 5.
     */
    enum class RendererOwner { POSE_PUMP, OVERLAY }

    private val gate = ArSessionGate()

    private fun ArSessionGate.Owner.legacy(): RendererOwner =
        if (this == ArSessionGate.Owner.OVERLAY) RendererOwner.OVERLAY else RendererOwner.POSE_PUMP

    private fun RendererOwner.gated(): ArSessionGate.Owner =
        if (this == RendererOwner.OVERLAY) ArSessionGate.Owner.OVERLAY else ArSessionGate.Owner.POSE_PUMP

    /**
     * Declares [owner] the only renderer allowed to drive this session from
     * now on. Idempotent, safe from any thread. A later claim always
     * supersedes an earlier one — the caller is switching TO that renderer,
     * so there is never a reason to refuse it.
     *
     * ROUND 6: a claim also clears any previous AR failure, because the
     * operator toggling into the AR overlay is an explicit "try again".
     */
    fun claimRenderer(owner: RendererOwner) {
        gate.claim(owner.gated())
        publishArError(null)
    }

    /**
     * Relinquishes ownership, but only if [owner] still holds it — an
     * out-of-order release (the old renderer's `AndroidView` disposing after
     * a NEWER claim already landed, which is exactly the race this exists
     * to survive) must never undo the newer owner's claim.
     */
    fun releaseRenderer(owner: RendererOwner) {
        gate.release(owner.gated())
    }

    /**
     * ROUND 6: the renderer's surface went away (window detach, background,
     * configuration change) while it may still have held the claim. Distinct
     * call site from [releaseRenderer] on purpose — see [ArSessionGate.surfaceDestroyed].
     */
    fun onRendererSurfaceDestroyed(owner: RendererOwner) {
        gate.surfaceDestroyed(owner.gated())
    }

    /**
     * ROUND 6 (owner item 19): records that the AR path failed, degrading it to
     * an inline message instead of an exception on a GL thread. Safe to call
     * from any thread and from any depth of the render loop; the first failure
     * of a session wins and the rest are swallowed so a per-frame throw cannot
     * flood the log or the UI.
     */
    fun reportArFailure(context: String, error: Throwable?) {
        val detail = when {
            error == null -> context
            error.message.isNullOrBlank() -> "$context (${error.javaClass.simpleName})"
            else -> "$context (${error.javaClass.simpleName}: ${error.message})"
        }
        if (gate.fail(detail)) {
            Log.w(TAG, "AR path degraded: $detail", error)
            publishArError(detail)
        }
    }

    private fun publishArError(message: String?) {
        _status.value = _status.value.copy(arError = message)
    }

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
        /**
         * ROUND 6 (owner item 19): non-null once anything on the AR enable path
         * has failed. The Capture screen renders it as an inline
         * "AR unavailable — <reason>" state and falls back to the 3D-orbit
         * view; nothing on this path is ever allowed to reach the default
         * uncaught-exception handler again.
         */
        val arError: String? = null,
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
            // ROUND 6 (owner item 19, crash cause 2): the probe below is a
            // Camera2 characteristics query and takes milliseconds. Publishing
            // `session` before it used to open a window in which a
            // RENDERMODE_CONTINUOUSLY GL thread could see a non-null,
            // NOT-YET-RESUMED session and call `update()` on it —
            // `SessionPausedException`, uncaught, on a non-UI thread, process
            // gone. The gate is what closes that window for good (`mayDrive`
            // requires `resumed`), and the probe now runs BEFORE publication so
            // the window is not even opened.
            val probe = runCatching { ArCameraCharacteristicsProbe.probe(context, s) }.getOrNull()
            session = s
            gate.onSessionCreated()
            _status.value = _status.value.copy(
                availability = ArAvailability.READY,
                cameraProbe = probe,
                message = null,
                arError = null,
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
            // ROUND 6: only NOW may a renderer thread call `Session.update()`.
            gate.onResumed()
            _status.value = _status.value.copy(sessionRunning = true, message = null, arError = null)
            Result.success(Unit)
        } catch (e: CameraNotAvailableException) {
            gate.onPaused()
            _status.value = _status.value.copy(
                sessionRunning = false,
                message = "Camera unavailable — close other camera apps and try again",
            )
            Result.failure(e)
        } catch (e: Throwable) {
            // ROUND 6 (owner item 19): `resume()` can also throw
            // `SecurityException` (permission revoked between the check and the
            // call), `UnavailableException` subclasses and, on some OEM builds,
            // a bare `IllegalStateException`. Any of them used to propagate out
            // of a Compose LaunchedEffect and kill the app; now the AR path
            // degrades to an inline message and the 3D-orbit view keeps working.
            gate.onPaused()
            reportArFailure("could not start the AR camera", e)
            _status.value = _status.value.copy(sessionRunning = false)
            Result.failure(e)
        }
    }

    fun pause() {
        // ROUND 6 (owner item 19, crash cause 3): the gate is shut FIRST, so a
        // GL thread that is mid-frame cannot get past `mayDrive` into a
        // `Session.update()` on a session this line is about to pause.
        gate.onPaused()
        runCatching { session?.pause() }
            .onFailure { Log.w(TAG, "ARCore pause threw; treating the session as paused", it) }
        // ROUND 5 AUDIT bugfix: `failureReason` used to be left exactly as it
        // was before the pause. `needsArSession` (CaptureScreen.kt) toggles
        // this controller through pause()/resume() any time it goes momentarily
        // false — most notably right as a SECOND recording starts in the same
        // connect session (Stop drops `needsArSession` for a Mid-360 session,
        // the next Start brings it back) — and until the next `onFrame()`
        // actually runs, `updateMotionHint()`'s poll reads whatever
        // `failureReason` happened to be at the moment of the LAST pose before
        // the pause. If that was `EXCESSIVE_MOTION` (very plausible right at
        // the end of an active walkthrough), the "Moving too fast" hint fired
        // immediately on the new, perfectly stationary session — a stale
        // reading, not a stale reading `close()` has: `close()` already resets
        // to a fresh `ArStatus()` (`failureReason = NONE` by default), so
        // `pause()` doing anything less than the same NONE reset for this one
        // field was the actual gap, not a deliberate distinction.
        _status.value = _status.value.copy(
            sessionRunning = false,
            tracking = false,
            failureReason = TrackingFailureReason.NONE,
        )
    }

    fun close() {
        pause()
        gate.onSessionClosed()
        runCatching { session?.close() }
            .onFailure { Log.w(TAG, "ARCore close threw; dropping the session anyway", it) }
        session = null
        latestFrame = null
        motion.clear()
        lastPoseNs.set(Long.MIN_VALUE)
        _status.value = ArStatus(availability = _status.value.availability)
    }

    /**
     * ROUND 6 (owner item 23): the last window of pose samples, for the one-tap
     * mount re-zero. Snapshotted from [motion] (the same tracker B8's gate uses)
     * so the re-zero costs no extra ARCore work at all.
     */
    fun poseWindow(): List<com.lidarscan.core.capture.PoseSample> = motion.snapshot()

    /** Display geometry, from the `GLSurfaceView`'s `onSurfaceChanged` and the activity's display rotation. */
    fun setDisplayGeometry(display: Display?, width: Int, height: Int) {
        displayRotation = display?.rotation ?: displayRotation
        viewWidth = width.coerceAtLeast(1)
        viewHeight = height.coerceAtLeast(1)
        geometryDirty = true
    }

    /**
     * The camera texture ARCore renders the background into — created by
     * [ArCameraBackgroundRenderer]. No-ops unless [owner] currently holds
     * [claimRenderer]'s ownership — see [RendererOwner]'s doc for why: this is
     * what stops a not-yet-torn-down pump/overlay GL thread from re-binding
     * the session's camera texture out from under the one Compose actually
     * wants on screen.
     */
    fun setCameraTextureName(textureId: Int, owner: RendererOwner) {
        // ROUND 6: `setCameraTextureName` on a session that is merely created
        // (not resumed) is legal, so this deliberately does NOT require
        // `PROCEED` — binding the texture before resume is exactly what a
        // renderer should be doing. What it does require is ownership and a
        // session, and it must never throw onto the GL thread.
        if (gate.currentOwner !== owner.gated()) return
        val s = session ?: return
        runCatching { s.setCameraTextureName(textureId) }
            .onFailure { reportArFailure("could not bind the AR camera texture", it) }
    }

    /**
     * One ARCore frame. Called from the GL render thread. Returns the frame so
     * the renderer can draw the background from the same object (ARCore
     * requires drawing the background with the frame that produced it — using
     * a stale one tears the image against the display transform).
     *
     * No-ops (returns `null`, calls nothing on [session]) unless [owner]
     * currently holds [claimRenderer]'s ownership — see [RendererOwner]'s doc.
     * This is the half of the fix that matters most: it is what actually
     * prevents `Session.update()` from ever being called by two GL threads
     * at once, which ARCore's own contract leaves undefined.
     */
    fun onFrame(owner: RendererOwner): Frame? {
        // ROUND 6 (owner item 19): ONE gate, checked before anything touches
        // the session. `NOT_RESUMED` is the case that used to be a crash —
        // `Session.update()` on a paused session throws `SessionPausedException`
        // (unchecked), and on a GL thread that is a dead process, not a caught
        // error. See ArSessionGate's header for all three routes into it.
        if (!gate.mayDrive(owner.gated()).mayProceed) return null
        val s = session ?: return null
        val frame = try {
            if (geometryDirty) {
                s.setDisplayGeometry(displayRotation, viewWidth, viewHeight)
                geometryDirty = false
            }
            s.update()
        } catch (e: CameraNotAvailableException) {
            // Recoverable and expected (another app grabbed the camera): not a
            // gate failure, so the AR path stays available and simply reports.
            _status.value = _status.value.copy(sessionRunning = false, message = "Camera lost")
            return null
        } catch (e: Throwable) {
            // Everything else — SessionPausedException, MissingGlContextException,
            // DeadlineExceededException, a driver's own IllegalStateException.
            // The contract of this method is now that it NEVER throws onto the
            // render thread, whatever ARCore does.
            reportArFailure("the AR camera stopped", e)
            return null
        }
        latestFrame = frame
        // A listener is app code (B8's keyframe recorder, the trail, the
        // calibration detector). One of them throwing must degrade AR, not kill
        // the app — same rule, one layer up.
        runCatching { publishPose(frame) }
            .onFailure { reportArFailure("AR pose publishing failed", it) }
        for (listener in frameListeners) {
            runCatching { listener(frame) }
                .onFailure { reportArFailure("an AR frame consumer failed", it) }
        }
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
