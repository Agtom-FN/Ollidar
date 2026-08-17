package com.lidarscan.app.ar

import java.util.concurrent.atomic.AtomicReference

/**
 * ROUND 6, owner item 19 — **"the AR overlay crush the app when enable".**
 *
 * The ownership half of this already existed in [CaptureArController] (ROUND 5
 * AUDIT §5, the black-camera fix). What did not exist, and is what actually
 * killed the process, is the **lifecycle** half. This class is both, extracted
 * into one plain-Kotlin state machine with no ARCore, no GL and no Android types
 * — so the thing that has to be correct is the thing that can be unit-tested on
 * a bare JVM, which matters here because no ARCore device exists in this
 * environment and the failure only ever appeared on one.
 *
 * ## The crash, precisely
 *
 * `Session.update()` throws `SessionPausedException` — an unchecked
 * `RuntimeException` — when the session has not been resumed. It is called from
 * the `GLSurfaceView` render thread ([ArCameraBackgroundRenderer.onDrawFrame]),
 * where an uncaught exception is an **uncaught exception on a non-UI thread**:
 * the default handler takes the process down. `CaptureArController.onFrame()`
 * caught exactly one exception type, `CameraNotAvailableException`, so every
 * other ARCore exception went straight to the killer.
 *
 * There were three concrete ways to get there when the operator enabled the AR
 * overlay, and the app hit all three:
 *
 *  1. **Grant-the-permission path.** `CaptureScreen`'s permission callback
 *     called `createSession()` and *not* `resume()`. Switching to AR on a phone
 *     that had never granted the camera permission (which, since ROUND 5 moved
 *     the request off screen entry, is the normal first time) therefore produced
 *     a live, un-resumed `Session` while the overlay's GL thread was already
 *     spinning at `RENDERMODE_CONTINUOUSLY`. First `onDrawFrame` → throw → dead.
 *  2. **Create/resume race.** Even with permission already granted,
 *     `createSession()` publishes `session` and *then* runs a Camera2
 *     characteristics probe before `LaunchedEffect` gets to `resume()`. The GL
 *     thread only needs to win that millisecond-scale window once.
 *  3. **Pause during teardown.** `DisposableEffect(needsArSession)`'s
 *     `onDispose` pauses the controller on the main thread while the GL thread
 *     is still running — leaving/backgrounding the screen with AR on.
 *
 * [mayDrive] is the fix: a renderer thread may only touch the session when it
 * **owns** it AND the session is created AND resumed AND nothing has already
 * failed. Everything else no-ops with a reason the UI can print, which is the
 * other half of item 19 ("failure shows an inline error state instead of
 * dying").
 *
 * Thread-safety: every field is atomic or volatile and every transition is a
 * single write. Claims/releases/lifecycle come from the main thread, [mayDrive]
 * is read from a GL thread, and there is no compound state to tear.
 */
class ArSessionGate {

    /** Which of the (at most one) renderers is allowed to drive the session. */
    enum class Owner { POSE_PUMP, OVERLAY }

    /** Why a renderer may or may not call into the session right now. */
    enum class Decision {
        /** Own it, session up and resumed — go ahead. */
        PROCEED,

        /** Another renderer owns the session; this one is stale or superseded. */
        NOT_OWNER,

        /** No session has been created yet (permission still pending, ARCore not installed…). */
        NO_SESSION,

        /** Session exists but is paused — calling `update()` here is the crash. */
        NOT_RESUMED,

        /** Something already failed; the AR path is degraded until it is cleared. */
        FAILED,
        ;

        val mayProceed: Boolean get() = this == PROCEED
    }

    private val owner = AtomicReference<Owner?>(null)

    @Volatile
    private var sessionCreated = false

    @Volatile
    private var resumed = false

    @Volatile
    private var failureReason: String? = null

    /** The current owner, for diagnostics and tests. */
    val currentOwner: Owner? get() = owner.get()

    /** Non-null once the AR path has degraded; the inline "AR unavailable (…)" line reads this. */
    val failure: String? get() = failureReason

    val isSessionRunning: Boolean get() = sessionCreated && resumed && failureReason == null

    /**
     * Declares [candidate] the only renderer allowed to drive the session from
     * now on. A later claim always supersedes an earlier one — the caller is
     * switching *to* that renderer, so there is never a reason to refuse it.
     *
     * Claiming also clears a previous failure: the operator toggling back into
     * AR is an explicit request to try again, and a permanently poisoned gate
     * would make one bad frame disable AR for the rest of the session.
     */
    fun claim(candidate: Owner) {
        failureReason = null
        owner.set(candidate)
    }

    /**
     * Relinquishes ownership, but only if [candidate] still holds it. An
     * out-of-order release — the old renderer's `AndroidView` disposing *after*
     * a newer claim already landed, which is exactly the race this exists to
     * survive — must never undo the newer owner's claim.
     */
    fun release(candidate: Owner) {
        owner.compareAndSet(candidate, null)
    }

    /**
     * The surface backing [candidate] went away (window detach, activity
     * backgrounded, configuration change). Identical to [release] in effect and
     * separate in name because it is a different event with a different call
     * site, and conflating them is how "surface destroyed mid-claim" turns into
     * a renderer that still believes it owns a session it can no longer draw.
     */
    fun surfaceDestroyed(candidate: Owner) = release(candidate)

    fun onSessionCreated() {
        sessionCreated = true
    }

    fun onResumed() {
        resumed = true
    }

    fun onPaused() {
        resumed = false
    }

    fun onSessionClosed() {
        sessionCreated = false
        resumed = false
    }

    /**
     * Records a failure. Sticky until the next [claim] or [clearFailure], so a
     * renderer that throws on every frame reports once instead of flooding.
     * Returns true when this was the *first* failure, i.e. the one worth
     * logging and showing.
     */
    fun fail(reason: String): Boolean {
        if (failureReason != null) return false
        failureReason = reason
        return true
    }

    fun clearFailure() {
        failureReason = null
    }

    /** May [candidate] call into the ARCore session right now, and if not, why not. */
    fun mayDrive(candidate: Owner): Decision = when {
        failureReason != null -> Decision.FAILED
        owner.get() !== candidate -> Decision.NOT_OWNER
        !sessionCreated -> Decision.NO_SESSION
        !resumed -> Decision.NOT_RESUMED
        else -> Decision.PROCEED
    }
}
