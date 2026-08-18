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
import com.lidarscan.core.capture.SectionBreakBracket
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

    /**
     * ROUND 16 item 58 — **the gate is a check, and a check is not a lock.**
     *
     * This is the whole of the scan-039 regression, and it is a bug ROUND 14
     * created by fixing a different one.
     *
     * [ArSessionGate.mayDrive] is read at the TOP of [onFrame] and then the GL
     * thread proceeds into `Session.update()`. Nothing stopped the main thread
     * from calling `Session.close()` in the microseconds AFTER that check
     * passed and BEFORE (or during) the `update()` it authorised. ROUND 6 did
     * not need to stop it: the only lifecycle call on the hot path was
     * `pause()`, and `mayDrive` shutting first is enough to keep a LATER
     * `update()` off a paused session — which was the crash it was written for.
     * [close] existed and, in ROUND 6's own words, "had zero callers anywhere
     * in src/main".
     *
     * ROUND 14 gave it a caller on the hottest path there is: every Start now
     * runs [resetWorldFrame], which closes the session while the pose pump's
     * `RENDERMODE_CONTINUOUSLY` thread is driving it at 60 Hz. A
     * `Session.close()` concurrent with a `Session.update()` is undefined by
     * ARCore's contract, and what it does in practice on the owner's Pixel is
     * worse than a crash: `createSession()` and `resume()` both SUCCEED on the
     * replacement, the gate reports the session running, and the camera never
     * delivers a single frame to it. The evidence is in the owner's own log —
     * the reset is logged, the start gate then times out with `blocker=NO_POSES`,
     * and 51 seconds of `cue: tracking_degraded` follow with not one pose
     * pushed. scan-039 has `lidar.bin` and 899 KB of 400 Hz `imu_phone.bin` and
     * NO `poses_ar.bin` and NO `map.bin` at all.
     *
     * It is intermittent because it is a race: of the five resets in that
     * session, scan-036 and scan-038 won it and scan-037 and scan-039 lost it.
     *
     * So ownership of the session becomes a real mutual exclusion:
     *
     *  * [onFrame] takes it with `tryLock` and gives up the frame if it cannot
     *    have it. The GL thread must NEVER block on the main thread — a few
     *    dropped frames during a rebuild is exactly the right price, and a
     *    render thread waiting on a lock is how an ANR is written.
     *  * [resetWorldFrame] takes it and HOLDS it across close / create / bind /
     *    resume, so there is no instant at which a half-built session is
     *    reachable from a render thread.
     *
     * Deadlock-free by construction: the only thing that blocks is the main
     * thread waiting for at most one in-flight frame, and nothing inside
     * [onFrame] ever waits on the main thread.
     */
    private val driveLock = java.util.concurrent.locks.ReentrantLock()

    private inline fun <T> java.util.concurrent.locks.ReentrantLock.withLockReentrant(
        body: () -> T,
    ): T {
        lock()
        try {
            return body()
        } finally {
            unlock()
        }
    }

    /**
     * ROUND 16 item 58 — frames dropped because a rebuild held [driveLock].
     * Exposed so the Diagnostics sheet can show that the mechanism ran rather
     * than leaving it as an invisible claim; a healthy reset costs single
     * digits.
     */
    private val framesYielded = java.util.concurrent.atomic.AtomicLong(0)

    val framesYieldedToReset: Long get() = framesYielded.get()

    /**
     * ROUND 14: the last camera texture a renderer bound, and who bound it.
     * Exists only so [resetWorldFrame] can re-bind it to a new session; see
     * [setCameraTextureName].
     */
    @Volatile
    private var boundTextureId: Int = -1

    @Volatile
    private var boundTextureOwner: RendererOwner? = null

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
                failureReason == TrackingFailureReason.INSUFFICIENT_LIGHT -> "Too dark to track — add light"
                failureReason == TrackingFailureReason.EXCESSIVE_MOTION -> "Moving too fast — slow down"
                failureReason == TrackingFailureReason.INSUFFICIENT_FEATURES -> "Not enough texture in view — point at something with detail"
                failureReason == TrackingFailureReason.CAMERA_UNAVAILABLE -> "Camera unavailable — another app may be using it"
                failureReason == TrackingFailureReason.BAD_STATE -> "Tracking is recovering…"
                else -> "Tracking is initialising…"
            }
    }

    private val _status = MutableStateFlow(ArStatus())
    val status: StateFlow<ArStatus> = _status.asStateFlow()

    /**
     * The pose stream, also consumed by the wizard's hold-still check and B8's
     * motion gate.
     *
     * ROUND 11 (owner item 45): the ring is 320 samples rather than the default
     * 64. `MountTrimRefiner` averages over the WHOLE hold-still gesture — up to
     * 8 s — and 64 samples is 2 s at ARCore's measured 30 Hz, so the refinement
     * would have silently stopped improving after two seconds while the ring
     * showed it still filling. `estimateAt` filters by its own 100 ms window and
     * is unaffected by the capacity; the cost is ~256 extra PoseSamples.
     */
    val motion = RigMotionTracker(capacity = 320)

    /**
     * ROUND 7, item 3: the section-break detector, fed from the same pose stream
     * as [motion].
     *
     * Lives on the controller rather than in the ViewModel because this is the
     * one place every ARCore pose passes through, and a break missed here is a
     * seam nobody can find afterwards. Reset by the ViewModel at each Start —
     * sections belong to a capture, not to the app's lifetime.
     */
    val sections = com.lidarscan.core.capture.PoseSectionTracker()

    /**
     * Set by the ViewModel: called on the AR thread for each detected
     * discontinuity.
     *
     * ROUND 15: the flag is whether the break was HEALED. A healed break is
     * bookkeeping — it is still recorded, and the offline stitch still runs on
     * it — and the operator has nothing to do about it, so it must not buzz.
     * An unhealed one is the only kind worth interrupting a walk for.
     */
    @Volatile
    var onSectionBreak: ((com.lidarscan.core.capture.PoseSectionBreak, Boolean) -> Unit)? = null

    /** ROUND 15 item 54. Reset by [resetSections]; read for the seal summary. */
    private val healedBreaks = java.util.concurrent.atomic.AtomicInteger(0)
    private val unhealedBreaks = java.util.concurrent.atomic.AtomicInteger(0)

    val healedBreakCount: Int get() = healedBreaks.get()
    val unhealedBreakCount: Int get() = unhealedBreaks.get()

    /**
     * Clears the section tracker AND the ROUND 15 heal counters. One call, so
     * a Start cannot reset half of the pair — the counters and the tracker
     * describe the same capture.
     */
    fun resetSections() {
        sections.reset()
        healedBreaks.set(0)
        unhealedBreaks.set(0)
    }

    /**
     * ROUND 17 item 63 — one sentence about the last re-anchor decision, for
     * the capture debug log. Null when nothing has been decided this session or
     * the engine is not attached.
     */
    fun lastReanchorSummary(): String? {
        val handle = engineHandle
        if (handle == 0L) return null
        val v = runCatching { ScanEngineNative.nativeLastReanchor(handle) }.getOrNull() ?: return null
        if (v.size < 10 || v[0] == 0.0) return null
        return ("verdict=%s gapS=%.3f reported=%.3fm/%.2fdeg gyro=%.2fdeg " +
            "residual=%.3fm/%.2fdeg walkBound=%.2fm gyroUsed=%s").format(
            ScanEngineNative.GapVerdict.name(v[1].toInt()),
            v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9] != 0.0,
        )
    }

    /**
     * Returns true when the engine accepted the correction. False means the
     * bracket could not define a rigid transform — a pose the tracker
     * disowned, a degenerate rotation, two poses with the same stamp — and
     * that is exactly the case the operator cue exists for.
     */
    private fun healLiveFrame(bracket: SectionBreakBracket): Boolean {
        val handle = engineHandle
        if (handle == 0L) return false
        return runCatching {
            ScanEngineNative.nativeHealLiveFrame(
                handle = handle,
                beforeNs = bracket.before.tMonoNs,
                bpx = bracket.before.position.x,
                bpy = bracket.before.position.y,
                bpz = bracket.before.position.z,
                bqx = bracket.before.orientation.x,
                bqy = bracket.before.orientation.y,
                bqz = bracket.before.orientation.z,
                bqw = bracket.before.orientation.w,
                beforeLost = !bracket.before.tracking,
                afterNs = bracket.after.tMonoNs,
                apx = bracket.after.position.x,
                apy = bracket.after.position.y,
                apz = bracket.after.position.z,
                aqx = bracket.after.orientation.x,
                aqy = bracket.after.orientation.y,
                aqz = bracket.after.orientation.z,
                aqw = bracket.after.orientation.w,
                afterLost = !bracket.after.tracking,
            ) == 0
        }.getOrDefault(false)
    }

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
            reportArFailure("could not start the tracking camera", e)
            _status.value = _status.value.copy(sessionRunning = false)
            Result.failure(e)
        }
    }

    fun pause() = driveLock.withLockReentrant {
        // ROUND 6 (owner item 19, crash cause 3): the gate is shut FIRST, so a
        // GL thread that is mid-frame cannot get past `mayDrive` into a
        // `Session.update()` on a session this line is about to pause.
        //
        // ROUND 16 item 58: ...and the lock is what makes "cannot" true rather
        // than likely. `pause()` on a session another thread is inside is the
        // same undefined call `close()` is, and both reach here from the same
        // main-thread lifecycle (`DisposableEffect(needsArSession)`) while the
        // pose pump is running. Reentrant, so `resetWorldFrame` — which already
        // holds it — passes straight through.
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

    fun close() = driveLock.withLockReentrant {
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
     * ROUND 14 (owner question: *"does the origin and IMU data offset zero
     * every time when the capture starts?"*). Until this round the answer was
     * **no**, and it was the single largest defect in the round-13 build.
     *
     * One `Session` was created per PROCESS and only ever paused between
     * captures ([close] existed but had no caller anywhere in `src/main`), so
     * capture N+1 inherited capture N's world origin, capture N's feature map
     * and capture N's relocalisation database. The owner's own containers prove
     * it: scan-035's first tracked pose sits **7 cm** from scan-034's last one,
     * 49 s and a Stop/Start apart, and scan-034's sits 0.42 m from scan-033's
     * last. Three scans of one small flat, one accumulating map — and 22.5 s
     * into scan-035 the pose stream jumped **1.631 m / 162.57° in 33 ms** while
     * the recorded 400 Hz gyro integrated **1.56°** over the same 33 ms. The
     * phone did not move. ARCore relocalised onto a place it had mapped during
     * an EARLIER capture, with the wrong orientation hypothesis. That is only
     * possible while the earlier capture's map is still loaded.
     *
     * So every capture now begins by throwing the world away: close the
     * session, build a new one, hand it back the renderer's existing camera
     * texture, resume. The [gate] is what makes this safe from the GL thread —
     * `sessionCreated`/`resumed` both go false for the duration, so an
     * in-flight [onFrame] returns null instead of calling `update()` on a
     * closing session. That is exactly the state machine ROUND 6 built.
     *
     * The cost is honest and it is the cost the app ALREADY paid: a
     * pause/resume was re-acquiring tracking in ~0.5 s anyway (scan-034 and
     * scan-035 both open with 14-15 recorded poses at exactly the origin,
     * `tracking_lost = 1`). A fresh session pays the same beat and gets a clean
     * frame for it. Returns false when there is nothing to reset or the rebuild
     * failed — the caller must never let this block a capture.
     */
    fun resetWorldFrame(): Boolean = resetWorldFrame(attempts = RESET_ATTEMPTS).ok

    /**
     * ROUND 16 item 58 — what a reset actually did, for the log and for the
     * caller that has to decide whether to arm a recorder on it.
     *
     * [ok] means the session was rebuilt and resumed. It does NOT mean the
     * camera is delivering — that is what [CaptureViewModel]'s start gate is
     * for, and this type exists so the two can be reported separately instead
     * of both hiding behind one Boolean.
     */
    data class ResetResult(val ok: Boolean, val attempts: Int, val yieldedFrames: Long)

    /**
     * ROUND 16 item 58: [resetWorldFrame], holding [driveLock] across the whole
     * rebuild, and retried.
     *
     * The lock is the fix for the race. The RETRY is the belt to its braces and
     * it costs nothing when nothing is wrong: `close()` releases a camera
     * device, `Session(context)` acquires one, and on some OEM builds the
     * release is not complete by the time the acquire runs — which surfaces as
     * a `CameraNotAvailableException` out of `resume()`, or as a session that
     * resumes and then never produces a frame. One rebuild is cheap (the owner's
     * successful resets cleared the start gate in ~2.0 s); paying for a second
     * one on the rare bad draw is much cheaper than a 2D scan.
     */
    fun resetWorldFrame(attempts: Int): ResetResult {
        if (session == null) return ResetResult(ok = false, attempts = 0, yieldedFrames = 0L)
        val before = framesYielded.get()
        driveLock.lock()
        try {
            val texture = boundTextureId
            val textureOwner = boundTextureOwner
            var tries = 0
            while (tries < attempts.coerceAtLeast(1)) {
                ++tries
                close()
                val created = createSession()
                if (created.isFailure) {
                    Log.w(TAG, "world-frame reset could not rebuild the ARCore session (try $tries)")
                    continue
                }
                // The renderer never learned that its session was replaced — it
                // is still holding the same GL texture and the same claim — so
                // hand the texture straight back rather than waiting for a
                // surface event that is not coming.
                if (texture >= 0 && textureOwner != null) {
                    setCameraTextureName(texture, textureOwner)
                }
                geometryDirty = true
                if (resume().isSuccess) {
                    return ResetResult(true, tries, framesYielded.get() - before)
                }
                Log.w(TAG, "world-frame reset rebuilt the session but could not resume it (try $tries)")
            }
            return ResetResult(false, tries, framesYielded.get() - before)
        } finally {
            driveLock.unlock()
        }
    }

    /**
     * ROUND 16 item 58 — how many poses this controller has ACCEPTED since the
     * counter was last armed, and when the most recent one landed.
     *
     * The pose watchdog and the start gate both need "is the tracker alive
     * RIGHT NOW", and neither could ask it before: [ArStatus.posesPushed] only
     * counts poses that reached an engine handle (zero before a recording
     * starts, by design), and [motion]'s ring answers a question about a
     * window rather than about the last instant. This counts every pose that
     * got past [publishPose]'s timestamp filter, engine or no engine.
     */
    private val posesAccepted = java.util.concurrent.atomic.AtomicLong(0)

    @Volatile
    private var lastPoseElapsedMillis: Long = 0L

    val acceptedPoseCount: Long get() = posesAccepted.get()

    /** `android.os.SystemClock.elapsedRealtime()` of the last accepted pose; 0 when none. */
    val lastAcceptedPoseAtMillis: Long get() = lastPoseElapsedMillis

    /** Arms the two counters above for a new capture (or a new reset). */
    fun resetPoseCounters() {
        posesAccepted.set(0)
        lastPoseElapsedMillis = 0L
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
            .onSuccess {
                // ROUND 14: remembered so [resetWorldFrame] can hand the SAME
                // texture to a freshly created session. The GL context and the
                // texture outlive the ARCore session — it is only the session's
                // binding to them that dies with it — so re-binding is a
                // one-line hand-off rather than a renderer teardown.
                boundTextureId = textureId
                boundTextureOwner = owner
            }
            .onFailure { reportArFailure("could not bind the tracking camera texture", it) }
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
        // ROUND 16 item 58: the lock FIRST, and never blocking. If a
        // [resetWorldFrame] is in flight this frame is simply skipped — the
        // session it would have driven is being replaced, so there is nothing
        // useful it could have done, and blocking a render thread on the main
        // thread is how an ANR is written.
        if (!driveLock.tryLock()) {
            framesYielded.incrementAndGet()
            return null
        }
        try {
            return onFrameLocked(owner)
        } finally {
            driveLock.unlock()
        }
    }

    private fun onFrameLocked(owner: RendererOwner): Frame? {
        // ROUND 6 (owner item 19): ONE gate, checked before anything touches
        // the session. `NOT_RESUMED` is the case that used to be a crash —
        // `Session.update()` on a paused session throws `SessionPausedException`
        // (unchecked), and on a GL thread that is a dead process, not a caught
        // error. See ArSessionGate's header for all three routes into it.
        //
        // ROUND 16: still checked, and now checked INSIDE the lock, which is
        // what makes it a decision rather than a guess about the next
        // microsecond.
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
            reportArFailure("the tracking camera stopped", e)
            return null
        }
        latestFrame = frame
        // A listener is app code (B8's keyframe recorder, the trail, the
        // calibration detector). One of them throwing must degrade AR, not kill
        // the app — same rule, one layer up.
        runCatching { publishPose(frame) }
            .onFailure { reportArFailure("pose publishing failed", it) }
        for (listener in frameListeners) {
            runCatching { listener(frame) }
                .onFailure { reportArFailure("a frame consumer failed", it) }
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
        val sample = PoseSample(timestamp, position, orientation, tracking)
        // ROUND 16 item 58: counted HERE — after the ordering filter and before
        // anything that can be switched off — so "the tracker is alive" is one
        // fact with one definition, shared by the start gate, the pose watchdog
        // and the seal.
        posesAccepted.incrementAndGet()
        lastPoseElapsedMillis = android.os.SystemClock.elapsedRealtime()
        motion.add(sample)
        // ROUND 7: a relocalization moves ARCore's world frame under the
        // pushbroom. Record it here, where every pose passes exactly once.
        // ROUND 15 item 54: HEAL FIRST, THEN PUSH.
        //
        // The correction is applied before `nativePushPose` below, so the very
        // pose that announced the re-anchor is already in the healed frame and
        // the live map never shows a single frame of the shattered one. The
        // engine records the RAW pose regardless — healing is a live-view
        // transform and the container is unaffected (see
        // `Engine::heal_live_frame`).
        sections.addBracketed(sample)?.let { bracket ->
            val healed = healLiveFrame(bracket)
            if (healed) healedBreaks.incrementAndGet() else unhealedBreaks.incrementAndGet()
            runCatching { onSectionBreak?.invoke(bracket.breakInfo, healed) }
        }

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

    companion object {
        private const val TAG = "CaptureArController"

        /**
         * ROUND 16 item 58. Two, not one and not five: one retry covers the
         * camera-handoff draw that loses the race, and a loop that keeps
         * rebuilding would turn a broken camera into a Start button that never
         * returns — which is ROUND 10 item 38's complaint arriving by a third
         * road.
         *
         * Public because [com.lidarscan.app.ui.capture.CaptureViewModel] asks
         * for it at two call sites — Start, and the start gate's NO_POSES
         * rebuild — and two call sites picking their own retry count is how
         * they end up disagreeing.
         */
        const val RESET_ATTEMPTS = 2

        /**
         * ARCore reports no per-pose covariance, so these are **stated
         * assumptions, not measurements** — carried into `scan_pose` so the
         * pushbroom's confidence gate has something monotone to work with,
         * and deliberately pessimistic while tracking is degraded. S6's own
         * budget attributes 11.4 px at 3 m to ARCore relative pose error
         * (WIZARD.md §4), which is the scale these are chosen around.
         */
        private fun positionSigmaFor(state: TrackingState): Float = when (state) {
            TrackingState.TRACKING -> 0.02f
            TrackingState.PAUSED -> 0.20f
            TrackingState.STOPPED -> 1.0f
        }

        private fun orientationSigmaFor(state: TrackingState): Float = when (state) {
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
        private fun poseQualityOf(state: TrackingState, reason: TrackingFailureReason): Int = when (state) {
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
