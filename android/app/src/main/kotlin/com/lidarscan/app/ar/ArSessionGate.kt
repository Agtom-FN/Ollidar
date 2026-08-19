package com.lidarscan.app.ar

import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
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
 * ---
 *
 * ## ROUND 22 item 89 — ownership is an INSTANCE, not a role
 *
 * The above was right and it was not enough, and the gap cost the owner days of
 * "tracking lost until I restart the app".
 *
 * Until this round, [claim] and [release] both spoke in terms of the [Owner]
 * **enum value**, and `release(candidate)` compare-and-set on it. That works
 * when the two renderers are genuinely different roles. But
 * `CaptureScreen.AR_OVERLAY_ARCHIVED` has been `true` since ROUND 7 — the AR
 * overlay is retired — so **every claim and every release in the shipping app
 * is `POSE_PUMP`**. The enum carries no identity at all, and the "out-of-order
 * release cannot undo a newer claim" guarantee this class was built to provide
 * silently evaporated: `POSE_PUMP.compareAndSet(POSE_PUMP, null)` always
 * succeeds, whichever *instance* asked.
 *
 * That is not a theoretical window. Navigation Compose keeps the **outgoing**
 * destination composed through the transition, so on every trip back into the
 * Scan tab (which, before item 88, was every seal) the new
 * [ArPosePumpView]'s `factory` claims at `ArPosePumpView.kt:68` and then the
 * OLD view's `onRelease` runs at `:93` and nulls the claim that the live
 * renderer is about to drive with. From that instant [mayDrive] returns
 * [Decision.NOT_OWNER] forever, `CaptureArController.onFrameLocked` returns
 * `null` on every frame, no pose is ever published again, and nothing anywhere
 * says so. Only killing the process fixed it.
 *
 * So a claim is now an **opaque per-claim token** ([Claim]) with its own
 * serial. [release] and [surfaceDestroyed] compare-and-set on the token, so a
 * stale view can only ever release ITS OWN claim — the enum's role is reduced
 * to what it always should have been, a name for logs and diagnostics.
 *
 * ## And the silence was the other half of the bug
 *
 * A bare `return null` on a render thread is a fault with no symptom but its
 * consequence. Every non-[Decision.PROCEED] answer is now reported through
 * [decisionSink], rate-limited to at most one line per [LOG_INTERVAL_MS] (a
 * refusal at 60 Hz must not be a way to fill the log), carrying the decision,
 * the current owner, the asking claim and how many refusals were suppressed
 * since the last line. A suppressed count is what distinguishes "one frame
 * lost during a hand-off" from "sixty a second for four minutes", and it is
 * exactly the distinction nobody could make from the owner's log.
 *
 * Thread-safety: every field is atomic or volatile and every transition is a
 * single write. Claims/releases/lifecycle come from the main thread, [mayDrive]
 * is read from a GL thread, and there is no compound state to tear.
 */
class ArSessionGate {

    /** Which of the (at most one) renderers is allowed to drive the session. */
    enum class Owner { POSE_PUMP, OVERLAY }

    /**
     * ROUND 22 item 89 — **one renderer's claim on the session.**
     *
     * Opaque by construction: the only thing a caller can do with it is hand it
     * back to [release], [surfaceDestroyed] or [mayDrive]. Identity is the
     * object's own — [equals] is not overridden and must not be, because two
     * claims by the same [owner] are exactly the case this type exists to tell
     * apart. [serial] is carried for the log line only; it is what makes
     * "POSE_PUMP#7 asked while POSE_PUMP#8 owns it" a readable sentence instead
     * of two identical words.
     */
    class Claim internal constructor(val owner: Owner, val serial: Long) {
        override fun toString(): String = "${owner.name}#$serial"
    }

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

    private val claimHolder = AtomicReference<Claim?>(null)
    private val claimSerial = AtomicLong(0L)

    @Volatile
    private var sessionCreated = false

    @Volatile
    private var resumed = false

    @Volatile
    private var failureReason: String? = null

    /** The current claim, for diagnostics, for tests, and for `resetWorldFrame`'s re-verify. */
    val currentClaim: Claim? get() = claimHolder.get()

    /** The current owner, for diagnostics and tests. */
    val currentOwner: Owner? get() = claimHolder.get()?.owner

    /** Non-null once the AR path has degraded; the inline "AR unavailable (…)" line reads this. */
    val failure: String? get() = failureReason

    val isSessionRunning: Boolean get() = sessionCreated && resumed && failureReason == null

    // ── ROUND 22 item 89: the refusal log ───────────────────────────────────

    /**
     * Where a refused [mayDrive] is reported. Set by [CaptureArController] to
     * the app's `CaptureLog`; left null in unit tests that do not care, and set
     * to a collector in the ones that do.
     *
     * Deliberately a plain lambda and not a logger interface: this class has no
     * Android dependencies and is going to keep it that way.
     */
    @Volatile
    var decisionSink: ((String) -> Unit)? = null

    /** Injectable clock, so the rate limiter can be tested without sleeping. */
    @Volatile
    var clockMillis: () -> Long = { System.currentTimeMillis() }

    private val lastDecisionLogAtMs = AtomicLong(Long.MIN_VALUE)
    private val suppressedDecisions = AtomicInteger(0)

    /**
     * Declares a NEW claim by [candidate] and returns its token. A later claim
     * always supersedes an earlier one — the caller is switching *to* that
     * renderer, so there is never a reason to refuse it.
     *
     * Claiming also clears a previous failure: the operator toggling back into
     * AR is an explicit request to try again, and a permanently poisoned gate
     * would make one bad frame disable AR for the rest of the session.
     *
     * **The return value is not optional.** A caller that drops it can never
     * release, and a caller that releases something else can never revoke a
     * live claim — which, since ROUND 22 item 89, is the point.
     */
    fun claim(candidate: Owner): Claim {
        failureReason = null
        val token = Claim(candidate, claimSerial.incrementAndGet())
        claimHolder.set(token)
        // A fresh claim is a fresh story: let the next refusal (if any) speak
        // immediately rather than waiting out the previous claim's window.
        lastDecisionLogAtMs.set(Long.MIN_VALUE)
        return token
    }

    /**
     * Relinquishes ownership, but only if [claim] is still THE claim. An
     * out-of-order release — the old renderer's `AndroidView` disposing *after*
     * a newer claim already landed, which is exactly the race this exists to
     * survive — must never undo the newer owner's claim.
     *
     * ROUND 22 item 89: this compares the token, not the owner enum. With the
     * AR overlay archived, every claim in the shipping app is `POSE_PUMP`, so
     * comparing the enum made this method a guaranteed success for whoever
     * asked — the guard was a no-op wearing a guard's comment.
     *
     * Null-tolerant: a view that never got a claim (its factory never ran) can
     * still call this on the way out without a branch at the call site.
     */
    fun release(claim: Claim?) {
        if (claim == null) return
        claimHolder.compareAndSet(claim, null)
    }

    /**
     * The surface backing [claim] went away (window detach, activity
     * backgrounded, configuration change). Identical to [release] in effect and
     * separate in name because it is a different event with a different call
     * site, and conflating them is how "surface destroyed mid-claim" turns into
     * a renderer that still believes it owns a session it can no longer draw.
     */
    fun surfaceDestroyed(claim: Claim?) = release(claim)

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

    /**
     * May the holder of [claim] call into the ARCore session right now, and if
     * not, why not.
     *
     * ROUND 22 item 89: a refused answer is also REPORTED (rate-limited), which
     * it never was. `CaptureArController.onFrameLocked`'s `return null` on a
     * refusal was silent, so a permanently stuck `NOT_OWNER` looked from the
     * outside exactly like a phone that had stopped tracking — the owner's
     * words were "tracking lost until app restart", and the app agreed with him
     * because it had nothing to say.
     */
    fun mayDrive(claim: Claim?): Decision {
        val decision = decide(claim)
        if (decision != Decision.PROCEED) noteRefusal(decision, claim)
        return decision
    }

    /**
     * The decision without the reporting — for callers that are asking a
     * question rather than driving a frame (the diagnostics strip, the tests
     * that assert the state machine and would otherwise flood their own sink).
     */
    fun peekDrive(claim: Claim?): Decision = decide(claim)

    private fun decide(claim: Claim?): Decision = when {
        failureReason != null -> Decision.FAILED
        claim == null || claimHolder.get() !== claim -> Decision.NOT_OWNER
        !sessionCreated -> Decision.NO_SESSION
        !resumed -> Decision.NOT_RESUMED
        else -> Decision.PROCEED
    }

    private fun noteRefusal(decision: Decision, claim: Claim?) {
        val sink = decisionSink ?: return
        val now = clockMillis()
        val last = lastDecisionLogAtMs.get()
        if (last != Long.MIN_VALUE && now - last < LOG_INTERVAL_MS) {
            suppressedDecisions.incrementAndGet()
            return
        }
        // One winner per window. A loser counts itself as suppressed rather
        // than logging, so the count stays honest under contention.
        if (!lastDecisionLogAtMs.compareAndSet(last, now)) {
            suppressedDecisions.incrementAndGet()
            return
        }
        val suppressed = suppressedDecisions.getAndSet(0)
        val line = buildString {
            append("gate refused ").append(decision.name)
            append(" asked=").append(claim?.toString() ?: "none")
            append(" owner=").append(claimHolder.get()?.toString() ?: "none")
            append(" created=").append(sessionCreated)
            append(" resumed=").append(resumed)
            failureReason?.let { append(" failure=").append(it) }
            if (suppressed > 0) append(" (+").append(suppressed).append(" more since the last line)")
        }
        runCatching { sink(line) }
    }

    companion object {
        /**
         * ROUND 22 item 89: at most one refusal line per second. A `NOT_OWNER`
         * at `RENDERMODE_CONTINUOUSLY` is 60 lines a second and would rotate
         * the 512 KB `capture.log` out of existence inside a couple of minutes
         * — which would destroy the very evidence this line exists to leave.
         * The suppressed count carries the rate that the lines no longer do.
         */
        const val LOG_INTERVAL_MS = 1_000L
    }
}
