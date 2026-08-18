package com.lidarscan.core.calib

import com.lidarscan.core.capture.PoseSample
import kotlin.math.sin
import kotlin.random.Random
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 11 item 45 — the guided hold-still re-zero.
 *
 * The claim being tested is not "the ring fills". It is the one item 45c rests
 * on: **holding longer makes the stored trim more accurate, and the app can
 * MEASURE that rather than assert it.** `MountTrimRefiner` measures it by
 * splitting the hold in half and comparing the two means — an empirical answer
 * to "if I took this trim twice, how far apart would the answers be" that needs
 * no noise model and survives ARCore's very-much-correlated frame-to-frame
 * drift.
 *
 * The fixtures are the ones `MountTrimGateTest` established for exactly this
 * reason: a jitter model rather than an ideal, because an ideal window is the
 * one input this control never receives.
 */
class MountTrimRefinerTest {

    private val hz = 30
    private fun deg(d: Double) = Math.toRadians(d)

    /** A small rotation of `angleDeg` about a pseudo-random axis. */
    private fun wobble(rng: Random, angleDeg: Double): Quat {
        val axis = Vec3(rng.nextDouble() - 0.5, rng.nextDouble() - 0.5, rng.nextDouble() - 0.5)
        val n = axis.norm
        val unit = if (n < 1e-9) Vec3(0.0, 1.0, 0.0) else Vec3(axis.x / n, axis.y / n, axis.z / n)
        return Quat.fromAxisAngle(unit, deg(angleDeg))
    }

    /**
     * `seconds` of a braced hand at 30 Hz: independent per-frame jitter of
     * about `wobbleDeg`, plus an optional slow sinusoidal drift — the thing that
     * makes a naive standard-error argument wrong, because it does NOT average
     * out over a second.
     */
    private fun hold(
        seconds: Double,
        // Sized from the owner's own capture rather than from taste: scan-020's
        // accepted trim reported spreadP90 = 2.40 deg and spreadMax = 2.65 deg,
        // which is what a braced hand on this rig actually delivers. A gentler
        // model would make a one-second hold look good enough and the whole of
        // item 45c unnecessary.
        wobbleDeg: Double = 1.2,
        // A slow WANDER, not white noise, and it is the reason this class
        // measures rather than models. ARCore's pose does not jitter
        // independently frame to frame; it wanders over a second or two, so the
        // mean of one second sits wherever the wander happened to be. A
        // standard-error argument (spread / sqrt(N)) is blind to that and would
        // report a one-second hold as already excellent. 1.3 deg over a 1.6 s
        // period keeps the gate's own p90 inside its 2.5 deg limit — this is a
        // hand the ROUND 8 gate accepts — while making the one-second MEAN
        // genuinely unreliable.
        driftDeg: Double = 1.3,
        driftPeriodS: Double = 1.6,
        seed: Int = 11,
        tracking: Boolean = true,
    ): List<PoseSample> {
        val rng = Random(seed)
        val n = (seconds * hz).toInt()
        val base = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(130.0))
        return (0 until n).map { i ->
            val t = i.toDouble() / hz
            val drift = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), deg(driftDeg * sin(2 * Math.PI * t / driftPeriodS)))
            val jitter = wobble(rng, Math.abs(rng.nextDouble() + rng.nextDouble() - 1.0) * 2.0 * wobbleDeg)
            PoseSample(
                tMonoNs = (t * 1e9).toLong(),
                position = Vec3.ZERO,
                orientation = (base * drift * jitter).normalized(),
                tracking = tracking,
            )
        }
    }

    @Test
    fun `holding longer measurably improves the trim, and the ring reports it`() {
        val refiner = MountTrimRefiner()
        val short = refiner.evaluate(hold(1.2), holdStartedAtMonoNs = 0L)
        val long = refiner.evaluate(hold(6.0), holdStartedAtMonoNs = 0L)

        // The gate's own number — the JITTER — does NOT improve, and that is the
        // point item 45c's wording nearly missed: a UI promising "spread falling
        // to 0.8 deg" would be promising something that does not happen.
        assertTrue("both holds pass the ROUND 8 gate", short.gatePasses && long.gatePasses)
        assertEquals(short.spreadP90Deg, long.spreadP90Deg, 0.5)

        // The ACCURACY OF THE MEAN — what is actually stored — does improve.
        assertTrue("short hold measurable: ${short.stabilityDeg}", short.stabilityDeg >= 0.0)
        assertTrue(
            "6 s (${long.stabilityDeg}) should beat 1.2 s (${short.stabilityDeg})",
            long.stabilityDeg < short.stabilityDeg,
        )
        assertTrue("6 s must reach the 0.8 deg target: ${long.stabilityDeg}", long.refined)
        assertFalse("1.2 s must not", short.refined)
        assertTrue(long.done)
    }

    @Test
    fun `the ring is honest about how far through the hold it is`() {
        val refiner = MountTrimRefiner()
        assertEquals(0f, refiner.evaluate(emptyList(), 0L).fraction, 0f)
        val half = refiner.evaluate(hold(4.0), 0L)
        assertEquals(4_000f / MountTrimRefiner.DEFAULT_MAX_HOLD_MS, half.fraction, 0.02f)
        // ...and it never runs past full, however long the operator holds.
        assertEquals(1f, refiner.evaluate(hold(20.0), 0L).fraction, 0.001f)
    }

    @Test
    fun `a rig that is being carried never completes the hold`() {
        val refiner = MountTrimRefiner()
        // 6 deg/s is a slow deliberate turn — well inside "carrying the rig",
        // and the same falsifiability control MountTrimGateTest uses.
        val moving = (0 until 180).map { i ->
            val t = i / 30.0
            PoseSample(
                tMonoNs = (t * 1e9).toLong(),
                position = Vec3.ZERO,
                orientation = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), deg(6.0 * t)).normalized(),
                tracking = true,
            )
        }
        val p = refiner.evaluate(moving, 0L)
        assertFalse("the ROUND 8 gate must still refuse a moving rig", p.gatePasses)
        assertFalse(p.done)
        assertTrue(p.label.contains("movement"))
        // ...and capture() refuses it too, through the same unchanged gate.
        val result = refiner.capture(moving, holdStartedAtMonoNs = 0L, nowMillis = 0L)
        assertTrue(result is MountTrimResult.Rejected)
        assertEquals(MountTrimRejection.MOVING, (result as MountTrimResult.Rejected).reason)
    }

    @Test
    fun `the captured trim is averaged over the WHOLE hold, not the last second`() {
        val refiner = MountTrimRefiner()
        val samples = hold(6.0)
        val oneSecond = MountTrimSampler.capture(samples, nowMillis = 0L)
        val wholeHold = refiner.capture(samples, holdStartedAtMonoNs = 0L, nowMillis = 0L)
        assertTrue(oneSecond is MountTrimResult.Captured)
        assertTrue(wholeHold is MountTrimResult.Captured)
        val short = (oneSecond as MountTrimResult.Captured).trim
        val long = (wholeHold as MountTrimResult.Captured).trim
        // 6 s at 30 Hz is 180 samples; the gate's window is ~31.
        assertTrue("${long.sampleCount} should be ~6x ${short.sampleCount}", long.sampleCount > 150)
        assertTrue(short.sampleCount < 40)
        // Both describe the same rig, so they must agree to well inside the
        // jitter — this is the sanity check that "averaging more" has not
        // quietly become "averaging something else".
        assertEquals(short.magnitudeDeg, long.magnitudeDeg, 1.0)
    }

    @Test
    fun `a hold that loses tracking is refused, and stability is unmeasurable`() {
        val refiner = MountTrimRefiner()
        val lost = hold(4.0, tracking = false)
        val p = refiner.evaluate(lost, 0L)
        assertFalse(p.gatePasses)
        assertFalse(p.done)
        assertTrue("no tracked samples means no two means to compare", p.stabilityDeg < 0.0)
    }

    @Test
    fun `too few samples to split means the ring says so rather than guessing`() {
        val refiner = MountTrimRefiner()
        // Under 2 x MIN_SAMPLES there is no honest way to form two means.
        assertTrue(refiner.splitHalfStabilityDeg(hold(0.4)) < 0.0)
        assertTrue(refiner.splitHalfStabilityDeg(hold(2.0)) >= 0.0)
    }

    @Test
    fun `a slow correlated drift is caught by split-half where a jitter count would not`() {
        val refiner = MountTrimRefiner()
        // Same per-frame jitter, but the hand also drifts 1.5 deg over a 6 s
        // period. A standard-error model (spread / sqrt(N)) would report this
        // hold as getting better and better; the two half-means genuinely
        // disagree, so split-half does not.
        val steady = refiner.evaluate(hold(6.0, driftDeg = 0.0), 0L)
        // A wander SLOWER than the hold: six seconds is not enough to average
        // over one period of it, so the two half-means land on opposite sides.
        val drifting = refiner.evaluate(hold(6.0, driftDeg = 2.0, driftPeriodS = 14.0), 0L)
        assertTrue(
            "drifting (${drifting.stabilityDeg}) must read worse than steady (${steady.stabilityDeg})",
            drifting.stabilityDeg > steady.stabilityDeg,
        )
    }

    @Test
    fun `the auto-refresh window is far shorter than the previous-run staleness rule`() {
        // Item 45b: a bracket picked up and put down between two scans in ONE
        // session has moved, and twelve hours (ROUND 8's `fromPreviousRun`
        // threshold) is not a useful answer to that.
        assertTrue(MountTrimRefiner.AUTO_REFRESH_AFTER_MS < MountTrimProvenances.STALE_AFTER_MILLIS)
        assertEquals(10 * 60_000L, MountTrimRefiner.AUTO_REFRESH_AFTER_MS)
    }
}
