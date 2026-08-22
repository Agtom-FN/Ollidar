package com.lidarscan.core.capture

/**
 * ROUND 28 item 155 — **the start gate must be allowed to give up.**
 *
 * The owner's log, 21:01:22 → 21:01:42, twenty and a bit seconds:
 *
 * ```
 * start gate: waiting for tracking — blocker=NO_POSES
 * start gate: timed out                                     (4 s)
 * start gate: NO_POSES … rebuilding it once more
 * start gate (after rebuild): still blocked                 (4 s)
 * start hold: waiting for a steady hold
 * start hold: TIMED OUT after 10000 ms                      (10 s)
 * [session] start
 * cue: tracking_degraded                                    (+4 ms)
 * banner up: stop walking, hold still
 * ```
 *
 * Underneath all twenty seconds, about sixty times a second:
 * `gate refused FAILED … the tracking camera stopped (FatalException)`.
 *
 * Three things are wrong there and they are all one thing: **the gate has no
 * concept of a terminal failure.** Round 12 wrote the rule that the gate never
 * refuses — *"an app that will not start is worse than a warned one"* — and
 * that rule is right about a gate that is *waiting*. It is wrong about a gate
 * that is *dead*, and the app could not tell the two apart, so it ran both
 * timers to completion and then recorded into a state it had already diagnosed.
 *
 * ### What this object decides
 *
 * Given what the gate knows at the end of its wait, one of three things:
 *
 *  * [PROCEED] — tracking is ready, or blocked for a reason that waiting and
 *    walking genuinely fix. Round 12's rule, untouched.
 *  * [ABORT] — the tracking camera has stopped and stayed stopped. Do not run
 *    the hold timer; do not start. Show the error card. **This alone recovers
 *    about twenty seconds per failed attempt**, and on the OPPO — where the
 *    fault is permanent — it is the difference between an app that says what is
 *    wrong and an app that produces six unusable scans in silence.
 *  * [OFFER_START_ANYWAY] — the gate ended `NO_POSES` after a rebuild. ARCore
 *    is alive but has delivered nothing; a scan started here will be 2-D. Round
 *    16 already diagnosed this exactly and then started anyway with a warning
 *    note the operator was walking away from. It becomes a **choice**: the same
 *    card, with `Start anyway` as an explicit secondary. A degraded scan is a
 *    decision, never a surprise.
 *
 * ### Why it is in `:core`
 *
 * The one device that reproduces the fault is not on this desk and ARCore does
 * not run on the emulator at all, so every claim about this path is a unit test
 * or it is nothing. [ArTrouble] made the same call one round ago for the same
 * reason, and this reuses its clock rule rather than inventing a second one:
 * a FatalException that clears in 200 ms is a blink, one that persists for
 * [ArTrouble.FATAL_PERSIST_MILLIS] is a dead camera.
 */
enum class StartGateOutcome {
    /** Start. Round 12's rule: a gate that is merely unsettled never refuses. */
    PROCEED,

    /** The camera is dead. Abort now, with the error card. */
    ABORT,

    /** ARCore is alive and delivering nothing. Ask, do not assume. */
    OFFER_START_ANYWAY,
}

object StartGateDecision {

    // ── the wording, under the law (instruction ≤6 words, detail ≤12) ───────

    /** 3 words. The hard-failure card's headline, per the mockup. */
    const val ABORT_TITLE = "Camera won't start."

    /** 6 words. */
    const val ABORT_DETAIL = "Close other camera apps, then retry."

    /** 5 words. */
    const val NO_POSES_TITLE = "No position tracking yet."

    /**
     * 11 words.
     *
     * It says what the operator LOSES rather than what the subsystem did, which
     * is round 18's diet applied to this line: "NO_POSES after two attempts" is
     * true and tells him nothing he can act on.
     */
    const val NO_POSES_DETAIL = "This scan will be flat. Move the phone slowly first."

    const val RETRY = "Retry"
    const val CANCEL = "Cancel"
    const val START_ANYWAY = "Start anyway"

    /**
     * @param ready the tracking warmup's final verdict.
     * @param blocker its blocker when not ready.
     * @param fatalSinceMillis when the AR gate first reported a persistent
     *   `FAILED`, or null if it is not failing. Same source [ArTrouble] reads.
     * @param nowMillis the clock.
     * @param rebuilt true once the round-16 NO_POSES rebuild has been spent —
     *   before that, `NO_POSES` still has a fix left to try and is not yet a
     *   question for the operator.
     */
    fun outcome(
        ready: Boolean,
        blocker: TrackingWarmup.Blocker?,
        fatalSinceMillis: Long?,
        nowMillis: Long,
        rebuilt: Boolean,
    ): StartGateOutcome {
        // The camera being dead outranks everything, including "ready": a
        // verdict computed from a pose window that stopped being refilled three
        // seconds ago is a verdict about the past. This ordering is the same
        // judgement `ArTrouble.kindFor` makes about a missing APK — the cause
        // that explains the others is reported instead of them.
        if (fatalSinceMillis != null &&
            nowMillis - fatalSinceMillis >= ArTrouble.FATAL_PERSIST_MILLIS
        ) {
            return StartGateOutcome.ABORT
        }
        if (ready) return StartGateOutcome.PROCEED
        // Round 16 item 58: NO_POSES means ARCore delivered NOTHING, which is
        // not "not settled yet". It gets its one rebuild first; after that it
        // is a question, not a warning printed at somebody's back.
        if (rebuilt && blocker == TrackingWarmup.Blocker.NO_POSES) {
            return StartGateOutcome.OFFER_START_ANYWAY
        }
        // Every other blocker means ARCore is running and the pose stream is
        // not good enough YET, which is exactly what waiting is for, and what
        // round 12 decided must never refuse a start.
        return StartGateOutcome.PROCEED
    }

    /** Every operator-facing string this item adds, for the wording guard. */
    val INSTRUCTIONS: List<String> = listOf(ABORT_TITLE, NO_POSES_TITLE)
    val DETAILS: List<String> = listOf(ABORT_DETAIL, NO_POSES_DETAIL)
}
