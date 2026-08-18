package com.lidarscan.core.capture

/**
 * ROUND 12 — **a scan may not start before the tracker has settled.**
 *
 * ## The evidence
 *
 * From the owner's own session log (`lidarscan-capture-log-2026-08-18-1418.txt`)
 * and the containers exported with it:
 *
 * ```
 * 14:13:20.167 [session] start: project=scan-025-…
 * 14:13:27.079 [ar] SECTION BREAK #1 reason=IMPOSSIBLE_STEP jump=2.015m/0.61deg gapMs=33
 * 14:13:53.646 [ar] SECTION BREAK #2 reason=IMPOSSIBLE_STEP jump=0.608m/0.45deg gapMs=33
 * ```
 *
 * — **2.0 metres in 33 milliseconds, 6.9 seconds after Start**, and
 * `scan-028.lscan` carries the same shape 3.7 s in (0.30 m / 33 ms). Two of the
 * three early breaks in that session happened inside the first seven seconds of
 * a capture, and `scan-026` — started from a settled tracker after a 1.1 s mount
 * hold — had none in 61 seconds.
 *
 * A 2 m step in one frame is not a walk. It is ARCore re-anchoring its world
 * frame, which it does while VIO is still converging on a room it has only just
 * begun to see. Everything recorded before that jump is in a different world
 * frame from everything after it — so those first seconds are not merely
 * lower-quality geometry, they are geometry in the wrong place, and the
 * assembler has no way to know.
 *
 * ## Why the tracking flag alone is not the gate
 *
 * It has to be said plainly because it is the obvious thing to reach for and it
 * does not work: **ARCore reported `TrackingState.TRACKING` with pose quality
 * GOOD across every one of those jumps.** All three of the owner's containers
 * decode with `tracking_lost = 0` and a single pose-quality value throughout.
 * The tracker does not report re-anchoring as a failure; it just moves.
 *
 * So the gate is not "is it tracking" but "**has it been tracking, without
 * moving in a way a person cannot move, for long enough to believe it**" —
 * which is a property of a WINDOW of poses, and is what this class computes.
 *
 * ## Why it waits rather than refuses
 *
 * A refusal at Start is the ROUND 8 mount-gate mistake again: a verdict on a
 * moment the operator has already finished, with nothing to do but tap again.
 * This reports a state the caller can show while it resolves itself, and it
 * always resolves — either the tracker settles, or [MAX_WAIT_MILLIS] passes and
 * the capture starts anyway with the operator told why. Never starting is worse
 * than starting warm-ish; starting silently on a garbage frame is worse than
 * both.
 *
 * Pure `:core`: no ARCore types, so the decision is unit-testable on a bare JVM.
 */
class TrackingWarmup(
    /** How long the pose stream must be continuously clean before Start is allowed. */
    private val requiredStableMillis: Long = REQUIRED_STABLE_MILLIS,
    /** Reuses the section detector's own thresholds so the two can never disagree. */
    private val maxSpeedMps: Double = PoseSectionTracker.MAX_SPEED_MPS,
    private val maxTurnRateDegPerS: Double = PoseSectionTracker.MAX_TURN_RATE_DEG_PER_S,
) {
    enum class Blocker {
        /** Fewer than two poses, or none at all — ARCore has not delivered yet. */
        NO_POSES,

        /** ARCore says it is not tracking right now. */
        NOT_TRACKING,

        /** A step no walk could produce, inside the window. The scan-025 case. */
        IMPOSSIBLE_STEP,

        /** Clean, but not clean for long enough yet. */
        TOO_SHORT,
    }

    data class Verdict(
        val ready: Boolean,
        val stableMillis: Long,
        val blocker: Blocker?,
        /** The largest implied speed seen in the clean window, m/s — for the log. */
        val worstStepMps: Double,
    ) {
        /** 0..1, for the same progress treatment the mount hold uses. */
        val fraction: Float
            get() = if (ready) 1f else (stableMillis.toFloat() / REQUIRED_STABLE_MILLIS).coerceIn(0f, 1f)

        val logSuffix: String
            get() = "stableMs=%d ready=%b blocker=%s worstStep=%.2fm/s".format(
                stableMillis,
                ready,
                blocker?.name ?: "none",
                worstStepMps,
            )

        /** What the operator is shown while this is false. One short line. */
        val label: String
            get() = when {
                ready -> "Tracking steady"
                blocker == Blocker.NO_POSES -> "Waiting for tracking…"
                blocker == Blocker.NOT_TRACKING -> "Tracking lost — point at a textured surface"
                blocker == Blocker.IMPOSSIBLE_STEP -> "Tracking is still settling — hold still a moment"
                else -> "Letting tracking settle…"
            }
    }

    /**
     * `samples` is the controller's pose ring, oldest first. Walks BACKWARD
     * from the newest sample and stops at the first thing that disqualifies the
     * window, so the answer is "how long has it been good UNTIL NOW" and a
     * problem five seconds ago stops mattering five seconds after it ends.
     */
    fun evaluate(samples: List<PoseSample>): Verdict {
        if (samples.size < 2) {
            return Verdict(false, 0L, Blocker.NO_POSES, 0.0)
        }
        val newest = samples.last()
        if (!newest.tracking) {
            return Verdict(false, 0L, Blocker.NOT_TRACKING, 0.0)
        }

        var worst = 0.0
        var i = samples.size - 1
        var blocker: Blocker? = null
        while (i > 0) {
            val cur = samples[i]
            val prev = samples[i - 1]
            if (!prev.tracking) {
                blocker = Blocker.NOT_TRACKING
                break
            }
            val dtNs = cur.tMonoNs - prev.tMonoNs
            if (dtNs > 0L) {
                val dtSeconds = dtNs / 1e9
                // The same floor PoseSectionTracker uses: two poses a
                // millisecond apart imply 30 m/s from ordinary VIO jitter.
                if (dtSeconds >= PoseSectionTracker.MIN_DT_SECONDS) {
                    val speed = (cur.position - prev.position).norm / dtSeconds
                    val turn = Math.toDegrees(prev.orientation.angleTo(cur.orientation)) / dtSeconds
                    if (speed > maxSpeedMps || turn > maxTurnRateDegPerS) {
                        blocker = Blocker.IMPOSSIBLE_STEP
                        worst = maxOf(worst, speed)
                        break
                    }
                    worst = maxOf(worst, speed)
                }
            }
            --i
        }

        // The window is [i, newest], and `blocker` is what ENDED it going
        // backwards — not a disqualification of the window itself. That
        // distinction is the whole semantics: the question is "how long has the
        // tracker been good UNTIL NOW", so a 2 m jump four seconds ago bounds
        // the window at four seconds and does not veto it. A gate that vetoed
        // would never open again after one bad frame, which is a trap and would
        // arrive as ROUND 10 item 38's complaint by another road.
        val stableMillis = (newest.tMonoNs - samples[i].tMonoNs) / 1_000_000L
        val ready = stableMillis >= requiredStableMillis
        return Verdict(
            ready = ready,
            stableMillis = stableMillis,
            blocker = if (ready) null else (blocker ?: Blocker.TOO_SHORT),
            worstStepMps = worst,
        )
    }

    companion object {
        /**
         * Two seconds.
         *
         * Set from the failures rather than from taste: the owner's two early
         * breaks landed 3.7 s and 6.9 s after Start, so a gate long enough to
         * cover them entirely would make the operator wait seven seconds before
         * every scan — which is a worse product and would be turned off.
         *
         * What two seconds buys is the case the gate CAN win: sixty ARCore
         * frames with no re-anchor in them is strong evidence the tracker has a
         * map of this room, and it costs the operator about as long as reading
         * the button. A break at 6.9 s is caught by the section detector and the
         * cue, which is where a mid-capture relocalization belongs.
         */
        const val REQUIRED_STABLE_MILLIS = 2_000L

        /**
         * The longest Start will wait before going anyway.
         *
         * A cap is mandatory, not defensive: in a dark or featureless room
         * ARCore may never produce a clean two-second window, and an app that
         * silently refuses to start is the ROUND 10 item 38 complaint
         * ("i can't start a new capture unless i close and reopen the app")
         * arriving by a different road.
         */
        const val MAX_WAIT_MILLIS = 4_000L
    }
}
