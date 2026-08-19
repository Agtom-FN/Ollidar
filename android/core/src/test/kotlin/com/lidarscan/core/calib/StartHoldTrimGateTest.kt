package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 22 item 92 — the start-hold trim gate, against **the owner's four real
 * numbers** from the 2026-08-20 session.
 *
 * The candidate his phone captured at Start:
 *
 * | statistic | value |
 * |---|---|
 * | magnitude | 0.24° |
 * | `spreadP90Deg` (dispersion about the window mean) | **0.20°** |
 * | `stabilityDeg` (split-half repeatability) | **3.18°** |
 *
 * The trim already persisted, measured over a proper hold: **0.29°** split-half.
 *
 * Round 20 applied the 3.18° one, silently, over the 0.29° one, at the head of
 * a scan. Two independent things are wrong with that and this suite pins both:
 * it is eleven times worse than what it replaced (and the auto-refresh path has
 * refused exactly this since ROUND 12), and its own two statistics disagree in
 * a way that says the **pose** moved rather than the phone.
 */
class StartHoldTrimGateTest {

    /**
     * A trim with the given statistics. The rotation itself is irrelevant to
     * every decision here — the gate judges the *quality* of a measurement, not
     * its value — so it is fixed at a small, plausible tilt.
     */
    private fun trim(
        spreadP90Deg: Double,
        stabilityDeg: Double,
        sampleCount: Int = 60,
    ): MountTrim = MountTrim(
        qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0,
        sensor = SensorType.COIN_D6,
        sampleCount = sampleCount,
        spreadDeg = spreadP90Deg * 2.0,
        spreadP90Deg = spreadP90Deg,
        stabilityDeg = stabilityDeg,
    )

    /** The candidate the owner's phone captured: 0.20° dispersion, 3.18° split-half. */
    private val ownersCandidate = trim(spreadP90Deg = 0.20, stabilityDeg = 3.18, sampleCount = 63)

    /** The trim it silently replaced: measured at 0.29°. */
    private val ownersIncumbent = trim(spreadP90Deg = 0.24, stabilityDeg = 0.29, sampleCount = 244)

    // ── the arithmetic the round-20 path never did ─────────────────────────

    @Test
    fun `the owner's candidate is refused, and refused as DRIFT rather than merely worse`() {
        val verdict = StartHoldTrimGate.judge(ownersCandidate, ownersIncumbent)
        assertEquals(
            "3.18 deg replacing 0.29 deg is the defect; drift is the SPECIFIC reason",
            StartHoldVerdict.REFUSE_DRIFT,
            verdict,
        )
    }

    @Test
    fun `the owner's numbers ARE the drift signature - 3_18 is 15_9x above 0_20`() {
        assertTrue(StartHoldTrimGate.trackingIsDrifting(spreadP90Deg = 0.20, stabilityDeg = 3.18))
        // The margin over the threshold, stated so a later retune is a decision
        // and not a surprise: 3.18 / 0.20 = 15.9, against a threshold of 4.
        assertEquals(15.9, 3.18 / 0.20, 0.05)
        assertTrue(15.9 > StartHoldTrimGate.DRIFT_RATIO)
    }

    @Test
    fun `a genuinely noisy hold is NOT drift - dispersion and split-half agree in magnitude`() {
        // A shaky hand: both statistics are large, and they are the same order.
        assertFalse(StartHoldTrimGate.trackingIsDrifting(spreadP90Deg = 1.60, stabilityDeg = 2.10))
        assertFalse(StartHoldTrimGate.trackingIsDrifting(spreadP90Deg = 0.90, stabilityDeg = 3.00))
    }

    @Test
    fun `an excellent hold is never called drift however clean the ratio`() {
        // 0.05 / 0.01 = 5x, over the ratio — and 0.05 deg is a superb trim.
        // The stability floor is what stops the ratio from condemning it.
        assertFalse(StartHoldTrimGate.trackingIsDrifting(spreadP90Deg = 0.01, stabilityDeg = 0.05))
        assertEquals(StartHoldVerdict.ACCEPT, StartHoldTrimGate.judge(trim(0.01, 0.05), ownersIncumbent))
    }

    @Test
    fun `an unmeasured stability can never be drift - there is nothing to disagree with`() {
        // stabilityDeg < 0 means "not measured" (MountTrim.accuracyDeg is null).
        assertFalse(StartHoldTrimGate.trackingIsDrifting(spreadP90Deg = 0.20, stabilityDeg = -1.0))
        assertNull(trim(0.20, -1.0).accuracyDeg)
    }

    @Test
    fun `a zero dispersion is not an infinite ratio`() {
        // A degenerate window (every sample identical) must not divide by zero
        // into a drift verdict.
        assertFalse(StartHoldTrimGate.trackingIsDrifting(spreadP90Deg = 0.0, stabilityDeg = 3.18))
    }

    // ── the ROUND 12 comparison, now on the start path too ─────────────────

    @Test
    fun `a materially worse but non-drifting candidate is refused as WORSE`() {
        // Same 11x degradation, but with a dispersion that matches it — a
        // genuinely bad hold rather than a drifting tracker.
        val worse = trim(spreadP90Deg = 1.50, stabilityDeg = 3.10)
        assertFalse(StartHoldTrimGate.trackingIsDrifting(worse))
        assertEquals(StartHoldVerdict.REFUSE_WORSE, StartHoldTrimGate.judge(worse, ownersIncumbent))
    }

    @Test
    fun `a better candidate always replaces the incumbent`() {
        assertEquals(
            StartHoldVerdict.ACCEPT,
            StartHoldTrimGate.judge(trim(0.10, 0.12), ownersIncumbent),
        )
    }

    @Test
    fun `a candidate inside the margin is accepted - the fresher frame wins a tie`() {
        // 0.31 vs 0.29 is 0.02 deg apart: the same measurement twice. The start
        // hold's reading is taken in THIS scan's own frame, which is the whole
        // reason round 20 put it there, so a tie must not be resolved in favour
        // of the older one.
        assertEquals(
            StartHoldVerdict.ACCEPT,
            StartHoldTrimGate.judge(trim(0.24, 0.31), ownersIncumbent),
        )
        // ...and just outside it is refused.
        assertEquals(
            StartHoldVerdict.REFUSE_WORSE,
            StartHoldTrimGate.judge(trim(0.24, 0.45), ownersIncumbent),
        )
    }

    @Test
    fun `with no incumbent anything non-drifting is accepted`() {
        assertEquals(StartHoldVerdict.ACCEPT, StartHoldTrimGate.judge(trim(1.50, 3.10), null))
        // ...but a drifting reading is still refused, because the problem is
        // not the comparison, it is the measurement.
        assertEquals(StartHoldVerdict.REFUSE_DRIFT, StartHoldTrimGate.judge(ownersCandidate, null))
    }

    @Test
    fun `a MEASURED candidate beats an UNMEASURED incumbent - round 18's rank, unchanged`() {
        // The incumbent's rank is UNMEASURED_RANK_BASE + p90 = 100.55; a
        // measured 0.80 deg candidate must win. This is the ROUND 18 line that
        // read "kept rank=0.78, candidate rank=100.55" the other way round.
        val unmeasuredIncumbent = trim(spreadP90Deg = 0.55, stabilityDeg = -1.0)
        assertEquals(100.55, unmeasuredIncumbent.qualityRank, 1e-9)
        assertEquals(
            StartHoldVerdict.ACCEPT,
            StartHoldTrimGate.judge(trim(0.30, 0.80), unmeasuredIncumbent),
        )
    }

    @Test
    fun `an UNMEASURED candidate loses to a MEASURED incumbent`() {
        val unmeasuredCandidate = trim(spreadP90Deg = 0.55, stabilityDeg = -1.0)
        assertEquals(
            StartHoldVerdict.REFUSE_WORSE,
            StartHoldTrimGate.judge(unmeasuredCandidate, ownersIncumbent),
        )
    }

    // ── what the operator and the log are told ─────────────────────────────

    @Test
    fun `the drift status is an instruction, not a diagnosis`() {
        val status = StartHoldTrimGate.refusalStatus(StartHoldVerdict.REFUSE_DRIFT)!!
        assertEquals("Tracking is drifting — hold on.", status)
        // Item 98's wording law: at most six words of instruction.
        assertTrue(status, status.trim().split(Regex("\\s+")).size <= 6)
    }

    @Test
    fun `the worse status is also within the wording law`() {
        val status = StartHoldTrimGate.refusalStatus(StartHoldVerdict.REFUSE_WORSE)!!
        assertTrue(status, status.trim().split(Regex("\\s+")).size <= 6)
        assertNull(StartHoldTrimGate.refusalStatus(StartHoldVerdict.ACCEPT))
    }

    @Test
    fun `the drift log line carries every number that produced the verdict`() {
        val line = StartHoldTrimGate.refusalLogLine(
            StartHoldVerdict.REFUSE_DRIFT, ownersCandidate, ownersIncumbent,
        )
        assertTrue(line, line.contains("0.20deg"))   // the dispersion
        assertTrue(line, line.contains("3.18deg"))   // the split-half
        assertTrue(line, line.contains("15.9x"))     // the ratio that decided it
        assertTrue(line, line.contains("0.29deg"))   // what it would have replaced
        assertTrue(line, line.contains("rank="))
    }

    @Test
    fun `the worse log line names the margin it was judged against`() {
        val line = StartHoldTrimGate.refusalLogLine(
            StartHoldVerdict.REFUSE_WORSE, trim(1.50, 3.10), ownersIncumbent,
        )
        assertTrue(line, line.contains("materially worse"))
        assertTrue(line, line.contains("margin 0.10deg"))
    }

    @Test
    fun `a log line for an absent incumbent says so rather than printing zeroes`() {
        val line = StartHoldTrimGate.refusalLogLine(StartHoldVerdict.REFUSE_DRIFT, ownersCandidate, null)
        assertTrue(line, line.contains("no incumbent"))
    }
}
