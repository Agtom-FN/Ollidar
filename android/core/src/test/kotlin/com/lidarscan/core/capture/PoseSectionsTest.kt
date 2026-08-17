package com.lidarscan.core.capture

import com.lidarscan.core.calib.Quat
import com.lidarscan.core.calib.Vec3
import kotlin.math.PI
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 7, item 3 — the section-break detector.
 *
 * Every case here is a shape a real ARCore session produces: a clean walk, a
 * relocalization after a loss, a silent drift correction, the teleport-looking
 * artefacts of a paused-and-resumed session. The bar the tests hold is the one
 * the field cares about: **a clean walk must never be split** (a false seam
 * would make the post pipeline align two halves that were already aligned), and
 * **a frame jump must always be caught** (a missed seam is a bent room nobody
 * can explain).
 */
class PoseSectionsTest {

    private val nsPerFrame = 33_333_333L // ARCore at ~30 Hz

    private fun sample(
        index: Int,
        position: Vec3,
        yawRad: Double = 0.0,
        tracking: Boolean = true,
    ) = PoseSample(
        tMonoNs = index * nsPerFrame,
        position = position,
        orientation = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), yawRad),
        tracking = tracking,
    )

    // ── the normal case: nothing is a section break ────────────────────────

    @Test
    fun `a clean 1 m per s walk with gait jitter is never split`() {
        val t = PoseSectionTracker()
        var seed = 12345L
        fun jitter(): Double {
            // Deterministic pseudo-noise, +/- 5 mm: ordinary VIO wobble that
            // must not read as a frame jump.
            seed = seed * 6364136223846793005L + 1442695040888963407L
            return ((seed ushr 33).toDouble() / Int.MAX_VALUE - 0.5) * 0.010
        }
        for (i in 0 until 300) { // 10 s
            val tSec = i * nsPerFrame / 1e9
            t.add(
                sample(
                    i,
                    Vec3(jitter(), 1.35 + jitter(), -1.0 * tSec + jitter()),
                    yawRad = 0.05 * kotlin.math.sin(2 * PI * 2.0 * tSec),
                ),
            )
        }
        assertEquals("a clean walk must be one section", 1, t.sectionCount())
        assertTrue(t.breaks().isEmpty())
    }

    @Test
    fun `a brisk deliberate turn is not a section break`() {
        val t = PoseSectionTracker()
        // 150 deg/s for a second — fast for a walkthrough, well inside human.
        for (i in 0..30) {
            val tSec = i * nsPerFrame / 1e9
            t.add(sample(i, Vec3(0.0, 1.35, 0.0), yawRad = Math.toRadians(150.0 * tSec)))
        }
        assertEquals(1, t.sectionCount())
    }

    // ── the failures that produce "sections" ───────────────────────────────

    @Test
    fun `tracking lost then regained is a section break, measured across the gap`() {
        val t = PoseSectionTracker()
        t.add(sample(0, Vec3(0.0, 1.35, 0.0)))
        t.add(sample(1, Vec3(0.0, 1.35, -0.033)))
        // Two seconds of loss. The poses reported meanwhile are the tracker's
        // guesses and must not be what the jump is measured against.
        for (i in 2..60) t.add(sample(i, Vec3(9.0, 9.0, 9.0), tracking = false))
        // Re-acquired, 0.4 m away from where it was last genuinely tracking.
        val br = t.add(sample(61, Vec3(0.4, 1.35, -0.033)))

        assertNotNull("a relocalization must be a section break", br)
        assertEquals(PoseSectionBreak.Reason.TRACKING_REGAINED, br!!.reason)
        assertEquals(0.4, br.positionJumpM, 1e-6)
        assertTrue("gap should span the loss: ${br.gapMillis} ms", br.gapMillis > 1900)
        assertEquals(2, t.sectionCount())
        assertTrue(br.summary.contains("tracking re-acquired"))
    }

    @Test
    fun `a silent drift correction while tracking is caught as an impossible step`() {
        val t = PoseSectionTracker()
        t.add(sample(0, Vec3(0.0, 1.35, 0.0)))
        // 0.5 m in one 33 ms frame = 15 m/s. ARCore never reported a loss; the
        // world frame simply moved. This is the case that leaves a seam nobody
        // can account for afterwards.
        val br = t.add(sample(1, Vec3(0.5, 1.35, 0.0)))

        assertNotNull(br)
        assertEquals(PoseSectionBreak.Reason.IMPOSSIBLE_STEP, br!!.reason)
        assertEquals(0.5, br.positionJumpM, 1e-6)
        assertEquals(2, t.sectionCount())
    }

    @Test
    fun `an impossible rotation step is caught even with no translation`() {
        val t = PoseSectionTracker()
        t.add(sample(0, Vec3(0.0, 1.35, 0.0), yawRad = 0.0))
        // 30 deg in 33 ms = 900 deg/s.
        val br = t.add(sample(1, Vec3(0.0, 1.35, 0.0), yawRad = Math.toRadians(30.0)))
        assertNotNull(br)
        assertEquals(PoseSectionBreak.Reason.IMPOSSIBLE_STEP, br!!.reason)
        assertEquals(30.0, br.rotationJumpDeg, 1e-3)
    }

    // ── the guards ─────────────────────────────────────────────────────────

    @Test
    fun `two poses closer together than the dt floor are never a break`() {
        val t = PoseSectionTracker()
        t.add(PoseSample(0L, Vec3(0.0, 1.35, 0.0), Quat.IDENTITY, true))
        // 1 ms apart with 3 cm of jitter implies 30 m/s, which is dt
        // quantisation, not a relocalization.
        val br = t.add(PoseSample(1_000_000L, Vec3(0.03, 1.35, 0.0), Quat.IDENTITY, true))
        assertNull(br)
        assertEquals(1, t.sectionCount())
    }

    @Test
    fun `an out-of-order or repeated timestamp is ignored rather than counted`() {
        val t = PoseSectionTracker()
        t.add(sample(10, Vec3(0.0, 1.35, 0.0)))
        assertNull(t.add(PoseSample(10 * nsPerFrame, Vec3(5.0, 1.35, 0.0), Quat.IDENTITY, true)))
        assertNull(t.add(PoseSample(5 * nsPerFrame, Vec3(5.0, 1.35, 0.0), Quat.IDENTITY, true)))
        assertEquals(1, t.sectionCount())
    }

    @Test
    fun `the recorded break list is bounded`() {
        val t = PoseSectionTracker(maxBreaks = 4)
        for (i in 0 until 100) {
            // Every other frame teleports.
            t.add(sample(i, Vec3(if (i % 2 == 0) 0.0 else 5.0, 1.35, 0.0)))
        }
        assertEquals(4, t.breaks().size)
    }

    @Test
    fun `reset clears the sections so a second capture starts whole`() {
        val t = PoseSectionTracker()
        t.add(sample(0, Vec3(0.0, 1.35, 0.0)))
        t.add(sample(1, Vec3(0.5, 1.35, 0.0)))
        assertEquals(2, t.sectionCount())
        t.reset()
        assertEquals(1, t.sectionCount())
        assertTrue(t.breaks().isEmpty())
        // And the first sample after a reset cannot itself be a break — there
        // is nothing to compare it to.
        assertNull(t.add(sample(50, Vec3(99.0, 1.35, 0.0))))
    }

    @Test
    fun `the hint is silent for one section and specific above it`() {
        assertNull(sectionHint(0))
        assertNull(sectionHint(1))
        val hint = sectionHint(3)
        assertNotNull(hint)
        assertTrue(hint!!.contains("3 sections"))
    }
}
