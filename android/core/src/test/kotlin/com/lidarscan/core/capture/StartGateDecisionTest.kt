package com.lidarscan.core.capture

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * ROUND 28 item 155 — the twenty seconds, as assertions.
 *
 * ARCore does not run on the emulator and the device that reproduces the fault
 * is not on this bench, so this path is a unit test or it is a claim.
 */
class StartGateDecisionTest {

    private val persist = ArTrouble.FATAL_PERSIST_MILLIS

    // ── round 12's rule survives ────────────────────────────────────────────

    @Test
    fun `a ready gate starts`() {
        assertEquals(
            StartGateOutcome.PROCEED,
            StartGateDecision.outcome(
                ready = true,
                blocker = null,
                fatalSinceMillis = null,
                nowMillis = 10_000L,
                rebuilt = false,
            ),
        )
    }

    /**
     * The three blockers that mean "ARCore is running and the stream is not
     * good enough yet" are exactly what waiting is for. Round 12: an app that
     * will not start is worse than a warned one.
     */
    @Test
    fun `an unsettled gate still never refuses`() {
        listOf(
            TrackingWarmup.Blocker.NOT_TRACKING,
            TrackingWarmup.Blocker.IMPOSSIBLE_STEP,
            TrackingWarmup.Blocker.TOO_SHORT,
        ).forEach { blocker ->
            assertEquals(
                "blocker $blocker must not refuse",
                StartGateOutcome.PROCEED,
                StartGateDecision.outcome(
                    ready = false,
                    blocker = blocker,
                    fatalSinceMillis = null,
                    nowMillis = 10_000L,
                    rebuilt = true,
                ),
            )
        }
    }

    // ── the OPPO ────────────────────────────────────────────────────────────

    /**
     * The regression this item exists for. A camera that has been dead for
     * three seconds aborts the start instead of running a 4 s gate, a 4 s
     * rebuild and a 10 s hold and then recording anyway.
     */
    @Test
    fun `a persistently dead camera aborts`() {
        assertEquals(
            StartGateOutcome.ABORT,
            StartGateDecision.outcome(
                ready = false,
                blocker = TrackingWarmup.Blocker.NO_POSES,
                fatalSinceMillis = 1_000L,
                nowMillis = 1_000L + persist,
                rebuilt = true,
            ),
        )
    }

    /**
     * A blink is not a fault. ARCore legitimately drops a frame or two through
     * a hard turn, and a gate that aborted on that would be a gate nobody could
     * start a scan through.
     */
    @Test
    fun `a momentary failure is not a fault`() {
        assertEquals(
            StartGateOutcome.PROCEED,
            StartGateDecision.outcome(
                ready = true,
                blocker = null,
                fatalSinceMillis = 1_000L,
                nowMillis = 1_000L + persist - 1,
                rebuilt = false,
            ),
        )
    }

    /**
     * A verdict computed from a pose window that stopped being refilled three
     * seconds ago is a verdict about the past. The dead camera outranks it —
     * the same ordering `ArTrouble.kindFor` applies to a missing APK.
     */
    @Test
    fun `a dead camera outranks a ready verdict`() {
        assertEquals(
            StartGateOutcome.ABORT,
            StartGateDecision.outcome(
                ready = true,
                blocker = null,
                fatalSinceMillis = 0L,
                nowMillis = persist,
                rebuilt = false,
            ),
        )
    }

    // ── NO_POSES becomes a question ─────────────────────────────────────────

    /**
     * Before the round-16 rebuild is spent, NO_POSES still has a fix left to
     * try and is not yet a question for the operator.
     */
    @Test
    fun `NO_POSES before the rebuild is not yet a question`() {
        assertEquals(
            StartGateOutcome.PROCEED,
            StartGateDecision.outcome(
                ready = false,
                blocker = TrackingWarmup.Blocker.NO_POSES,
                fatalSinceMillis = null,
                nowMillis = 10_000L,
                rebuilt = false,
            ),
        )
    }

    /**
     * After it, the app knows the scan will be flat. Round 16 printed a warning
     * note and started anyway; item 155 makes it a choice, because a degraded
     * scan the operator did not choose is the one that gets thrown away.
     */
    @Test
    fun `NO_POSES after the rebuild offers the choice`() {
        assertEquals(
            StartGateOutcome.OFFER_START_ANYWAY,
            StartGateDecision.outcome(
                ready = false,
                blocker = TrackingWarmup.Blocker.NO_POSES,
                fatalSinceMillis = null,
                nowMillis = 10_000L,
                rebuilt = true,
            ),
        )
    }

    // ── the wording law ─────────────────────────────────────────────────────

    @Test
    fun `every instruction is six words or fewer`() {
        StartGateDecision.INSTRUCTIONS.forEach {
            val words = it.trim().split(Regex("\\s+")).size
            org.junit.Assert.assertTrue("\"$it\" is $words words", words <= 6)
        }
    }

    @Test
    fun `every detail is twelve words or fewer`() {
        StartGateDecision.DETAILS.forEach {
            val words = it.trim().split(Regex("\\s+")).size
            org.junit.Assert.assertTrue("\"$it\" is $words words", words <= 12)
        }
    }

    /**
     * The one thing the copy must never do: name a subsystem. "NO_POSES after
     * two attempts" is true and unactionable; the operator is told what he
     * loses and what to try.
     */
    @Test
    fun `no line names a subsystem`() {
        (StartGateDecision.INSTRUCTIONS + StartGateDecision.DETAILS).forEach { line ->
            listOf("ARCore", "NO_POSES", "FatalException", "VIO", "session").forEach { jargon ->
                org.junit.Assert.assertFalse(
                    "\"$line\" names $jargon",
                    line.contains(jargon, ignoreCase = true),
                )
            }
        }
    }
}
