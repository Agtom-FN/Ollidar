package com.lidarscan.core.calib

import com.lidarscan.core.capture.PoseSample
import kotlin.math.cos
import kotlin.math.sin
import kotlin.random.Random
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 8, owner item 30a — **the mount re-zero gate, against a hand.**
 *
 * The field evidence this exists to answer, from the owner's Pixel 8 Pro +
 * COIN-D6 log on 0.4.0:
 *
 * ```
 * 00:50:43.157 [ar] mount re-zero refused: MOVING
 * 00:51:05.346 [ar] mount re-zero refused: MOVING
 * 00:51:16.648 [ar] mount re-zero refused: MOVING
 * 00:51:23.378 [ar] mount re-zero refused: MOVING
 * 00:51:24.533 [ar] mount re-zero refused: MOVING
 * 00:51:25.133 [ar] mount re-zero refused: MOVING
 * 00:51:26.967 [ar] mount re-zero refused: MOVING
 * 00:51:28.428 [pushbroom] extrinsic applied: … trim=none
 * ```
 *
 * Seven refusals in 44 seconds, zero successes, and then a capture on the bare
 * CAD nominal. `MountTrimTest` already covers the arithmetic (the composition,
 * the chordal mean, the double cover) and a *perfectly* still window; what it
 * never had was a window that looks like a real hand, which is the only input
 * this control ever actually receives.
 *
 * So every fixture below is built from a jitter model rather than from an ideal:
 * small continuous wobble plus the occasional bad frame, at the ~31 fps the
 * owner's own log shows (37 samples in a 1200 ms window). And the moving arms
 * are matched controls — if a change makes the steady case pass by making the
 * gate unfalsifiable, [aRigBeingCarriedIsStillRefused] and
 * [aSlowSteadyDriftIsRefusedByThePercentileEvenThoughNoFrameIsAnOutlier] fail.
 */
class MountTrimGateTest {

    private fun deg(d: Double) = Math.toRadians(d)

    /** A rotation of [angleDeg] about a unit axis given in spherical terms — deterministic per call. */
    private fun wobble(rng: Random, angleDeg: Double): Quat {
        // A uniformly-distributed axis: ARCore's attitude error is not confined
        // to one axis, and a single-axis fixture would let a gate that only
        // watched yaw look correct.
        val z = rng.nextDouble(-1.0, 1.0)
        val phi = rng.nextDouble(0.0, 2 * Math.PI)
        val r = Math.sqrt((1.0 - z * z).coerceAtLeast(0.0))
        return Quat.fromAxisAngle(Vec3(r * cos(phi), r * sin(phi), z), deg(angleDeg))
    }

    /**
     * A hand-held hold: [count] frames over [spanMs], each offset from the
     * intended attitude by a wobble drawn from a half-normal with scale
     * [wobbleScaleDeg], plus [glitches] frames pushed out to [glitchDeg].
     *
     * The glitches are the point. A single ARCore frame with a bad feature
     * match, one footstep transmitted up an arm, or the small correction that
     * follows a relocalisation puts one sample several degrees out — and under
     * the pre-ROUND-8 gate (`max deviation <= 1.5°`) **one** such sample vetoed
     * the entire hold.
     */
    private fun handHeldWindow(
        hold: Quat,
        seed: Int,
        count: Int = 31,
        spanMs: Long = 1_000L,
        wobbleScaleDeg: Double = 0.55,
        glitches: Int = 2,
        glitchDeg: Double = 4.0,
        tracking: Boolean = true,
    ): List<PoseSample> {
        val rng = Random(seed)
        val glitchAt = (0 until glitches).map { 3 + it * 11 }.toSet()
        return (0 until count).map { i ->
            val angle = if (i in glitchAt) {
                glitchDeg
            } else {
                // |N(0, scale)| — most frames well under a degree, a tail that
                // reaches ~2°, which is what a braced hand actually produces.
                Math.abs(rng.nextDouble() + rng.nextDouble() + rng.nextDouble() - 1.5) * 2.0 * wobbleScaleDeg
            }
            PoseSample(
                tMonoNs = i * (spanMs * 1_000_000L / (count - 1)),
                position = Vec3.ZERO,
                orientation = (hold * wobble(rng, angle)).normalized(),
                tracking = tracking,
            )
        }
    }

    /** A rig actually turning, at [ratePerSecondDeg], about a fixed axis. */
    private fun turningWindow(
        hold: Quat,
        ratePerSecondDeg: Double,
        count: Int = 31,
        spanMs: Long = 1_000L,
    ): List<PoseSample> = (0 until count).map { i ->
        val tSeconds = i * (spanMs / 1000.0) / (count - 1)
        PoseSample(
            tMonoNs = i * (spanMs * 1_000_000L / (count - 1)),
            position = Vec3.ZERO,
            orientation = (hold * Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), deg(ratePerSecondDeg * tSeconds)))
                .normalized(),
            tracking = true,
        )
    }

    private fun spreadOf(samples: List<PoseSample>): Pair<Double, Double> {
        val mean = MountTrimSampler.meanOrientation(samples.map { it.orientation })
        val devs = samples.map { Math.toDegrees(mean.angleTo(it.orientation)) }
        return MountTrimSampler.percentile(devs, MountTrim.SPREAD_PERCENTILE) to devs.max()
    }

    // ── the case the owner could not get through ───────────────────────────

    /**
     * The whole of item 30a: a normal steady hand, ten different ways, must be
     * accepted — and would have been refused by the old gate.
     *
     * Ten seeds rather than one because the failure being fixed is
     * *probabilistic*: the 0.3.0 log shows the same rig passing at
     * `spread=0.47deg … 0.82deg` and being refused four times in a row either
     * side of that, which is a threshold sitting on the noise floor. A gate that
     * only has to pass one lucky fixture would reproduce exactly that.
     */
    @Test
    fun aSteadyHandPassesEveryTime() {
        val hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(132.8))
        repeat(10) { seed ->
            val window = handHeldWindow(hold, seed = seed)
            val result = MountTrimSampler.capture(window, nowMillis = 1_700_000_000_000L)
            val captured = result as? MountTrimResult.Captured
                ?: error("seed $seed: a braced hand must be accepted, got $result")
            assertEquals(
                "seed $seed: the trim must still recover the held attitude",
                132.8,
                captured.trim.magnitudeDeg,
                // ~1.5°: this is a MEASUREMENT of an attitude that is jittering
                // by that much, so agreeing to better than the jitter would mean
                // the fixture was not jittering.
                1.5,
            )
            assertTrue(
                "seed $seed: p90 must be inside the steadiness limit",
                captured.trim.spreadP90Deg <= MountTrim.MAX_SPREAD_P90_DEG,
            )

            // The falsifiable half of the regression: this same window has a
            // worst-frame deviation ABOVE the 1.5° the pre-ROUND-8 gate compared
            // its max against, so it is a window the old code refused. If a
            // future change makes the fixture too clean, this fails and says so
            // rather than silently testing nothing.
            assertTrue(
                "seed $seed: the fixture must still be one the old max<=1.5deg gate refused " +
                    "(worst frame ${captured.trim.spreadDeg}deg)",
                captured.trim.spreadDeg > 1.5,
            )
        }
    }

    /**
     * The hold completes in ~1.5 s including the tap.
     *
     * `WINDOW_MS` 1000 and `MIN_SAMPLE_SPAN_MS` 700 mean the operator's own
     * experience is press-hold-done: 700 ms of tracked stillness is enough, and
     * the extra 300 ms of window is there to average over, not to wait for.
     */
    @Test
    fun sevenHundredMillisecondsOfHoldIsEnough() {
        val hold = Quat.IDENTITY
        assertTrue("the gate must not ask for more than 1 s", MountTrim.MIN_SAMPLE_SPAN_MS <= 700L)
        assertTrue("the averaged window must not exceed 1 s", MountTrim.WINDOW_MS <= 1_000L)

        // 22 frames in 710 ms — 31 fps, the rate the owner's own log shows.
        val short = handHeldWindow(hold, seed = 99, count = 22, spanMs = 710L)
        assertTrue(
            "a 710 ms braced hold must be accepted, got ${MountTrimSampler.capture(short, 0L)}",
            MountTrimSampler.capture(short, nowMillis = 0L) is MountTrimResult.Captured,
        )

        // 600 ms is not, and says so with its numbers rather than only a name.
        val tooShort = handHeldWindow(hold, seed = 99, count = 19, spanMs = 600L)
        val rejected = MountTrimSampler.capture(tooShort, nowMillis = 0L) as? MountTrimResult.Rejected
            ?: error("600 ms must not be accepted")
        assertEquals(MountTrimRejection.NOT_ENOUGH_SAMPLES, rejected.reason)
        // 599, not 600: the fixture's frame interval is an integer number of
        // nanoseconds, so the last sample lands one millisecond short. What
        // matters is that the number REACHED the refusal at all.
        assertTrue("span must be reported", rejected.measurement!!.spanMs in 590L..600L)
        assertEquals(19, rejected.measurement!!.samples)
    }

    // ── the controls: the gate must still refuse a rig that is moving ──────

    /**
     * REQUIRED CONTROL. A rig being carried cannot be re-zeroed, and no amount
     * of relaxing the steadiness limit may make it so — a trim taken mid-walk is
     * worse than no trim, because it silently rotates every resolved point.
     */
    @Test
    fun aRigBeingCarriedIsStillRefused() {
        val hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(132.8))
        // 15, 30 and 60 deg/s: a slow deliberate pan, a normal look-around, and
        // a turn. None of them is a hold.
        for (rate in listOf(15.0, 30.0, 60.0)) {
            val result = MountTrimSampler.capture(turningWindow(hold, rate), nowMillis = 0L)
            val rejected = result as? MountTrimResult.Rejected
                ?: error("${rate}deg/s is not a hold and must be refused, got $result")
            assertEquals("${rate}deg/s", MountTrimRejection.MOVING, rejected.reason)
            assertNotNull("the refusal must carry its numbers", rejected.measurement)
            val m = rejected.measurement!!
            assertTrue(
                "${rate}deg/s must break the outlier ceiling, not merely the percentile " +
                    "(max=${m.spreadMaxDeg})",
                m.spreadMaxDeg > MountTrim.MAX_SPREAD_OUTLIER_DEG,
            )
        }
    }

    /**
     * REQUIRED CONTROL, the subtle one: a drift slow enough that **no single
     * frame** is an outlier is still refused, by the percentile.
     *
     * At 6 deg/s over a 1 s window every sample sits within 3° of the mean —
     * comfortably inside [MountTrim.MAX_SPREAD_OUTLIER_DEG]. An outlier ceiling
     * on its own would accept it and average a rig that moved 6°. This is why
     * the gate is two numbers: the percentile is what notices that the
     * deviation is a *trend* rather than a tail.
     */
    @Test
    fun aSlowSteadyDriftIsRefusedByThePercentileEvenThoughNoFrameIsAnOutlier() {
        val samples = turningWindow(Quat.IDENTITY, ratePerSecondDeg = 6.0)
        val (p90, max) = spreadOf(samples)
        assertTrue("the fixture must have NO outlier frame (max=$max)", max < MountTrim.MAX_SPREAD_OUTLIER_DEG)
        assertTrue("but its p90 must be over the steadiness limit (p90=$p90)", p90 > MountTrim.MAX_SPREAD_P90_DEG)
        assertEquals(
            MountTrimRejection.MOVING,
            (MountTrimSampler.capture(samples, nowMillis = 0L) as? MountTrimResult.Rejected)?.reason,
        )
    }

    /**
     * REQUIRED CONTROL: a hold with one genuinely wild frame is refused by the
     * ceiling even though its p90 is fine — the tail is allowed to be a tail,
     * not a hand-off.
     */
    @Test
    fun oneWildFrameIsRefusedByTheOutlierCeiling() {
        val samples = handHeldWindow(Quat.IDENTITY, seed = 7, glitches = 1, glitchDeg = 9.0)
        val (p90, max) = spreadOf(samples)
        assertTrue("p90 must be inside the steadiness limit (p90=$p90)", p90 <= MountTrim.MAX_SPREAD_P90_DEG)
        assertTrue("max must be outside the ceiling (max=$max)", max > MountTrim.MAX_SPREAD_OUTLIER_DEG)
        assertEquals(
            MountTrimRejection.MOVING,
            (MountTrimSampler.capture(samples, nowMillis = 0L) as? MountTrimResult.Rejected)?.reason,
        )
    }

    /** NOT_TRACKING is untouched: no relaxation anywhere may let an untracked frame into the average. */
    @Test
    fun trackingLossIsStillAbsolute() {
        val samples = handHeldWindow(Quat.IDENTITY, seed = 3).toMutableList()
        samples[17] = samples[17].copy(tracking = false)
        assertEquals(
            MountTrimRejection.NOT_TRACKING,
            (MountTrimSampler.capture(samples, nowMillis = 0L) as? MountTrimResult.Rejected)?.reason,
        )
    }

    // ── item 30b: the refusal reports what it measured ─────────────────────

    /**
     * The log suffix, exactly as `CaptureViewModel` writes it. The owner's eight
     * refusals said `MOUNT re-zero refused: MOVING` and nothing else, which
     * could not distinguish "0.1° over" from "the gate is broken" from "no
     * samples are arriving" — all three were live hypotheses at the start of
     * this round.
     */
    @Test
    fun aRefusalCarriesItsMeasurementIntoTheLog() {
        val rejected = MountTrimSampler.capture(turningWindow(Quat.IDENTITY, 30.0), nowMillis = 0L)
            as? MountTrimResult.Rejected ?: error("expected a refusal")
        val suffix = rejected.measurement!!.logSuffix
        for (key in listOf("p90=", "max=", "limit=", "outlierLimit=", "samples=", "spanMs=")) {
            assertTrue("the log suffix must carry $key — got \"$suffix\"", suffix.contains(key))
        }
        assertTrue(suffix, suffix.contains("samples=31"))
        assertTrue(suffix, Regex("spanMs=99[0-9]|spanMs=1000").containsMatchIn(suffix))

        // And the panel sentence says what to DO, which is the half a refusal
        // reading only "the rig moved" never gave an operator who believed they
        // were holding still.
        assertTrue(rejected.sentence, rejected.sentence.contains("hold still ~1 s"))
        assertTrue(rejected.sentence, rejected.sentence.contains("2.5"))
    }

    /** No samples means no measurement — zeros here would read as "perfectly still", which is a lie. */
    @Test
    fun noPosesCarriesNoMeasurement() {
        val rejected = MountTrimSampler.capture(emptyList(), nowMillis = 0L) as MountTrimResult.Rejected
        assertEquals(MountTrimRejection.NO_POSES, rejected.reason)
        assertEquals(null, rejected.measurement)
        assertEquals(MountTrimRejection.NO_POSES.message, rejected.sentence)
    }

    // ── the percentile itself ──────────────────────────────────────────────

    @Test
    fun percentileIsNearestRankAndAlwaysAValueThatOccurred() {
        val values = (1..10).map { it.toDouble() }
        assertEquals(9.0, MountTrimSampler.percentile(values, 0.90), 1e-9)
        assertEquals(1.0, MountTrimSampler.percentile(values, 0.0), 1e-9)
        assertEquals(10.0, MountTrimSampler.percentile(values, 1.0), 1e-9)
        assertEquals(5.0, MountTrimSampler.percentile(listOf(5.0), 0.9), 1e-9)
        // Order must not matter.
        assertEquals(
            MountTrimSampler.percentile(values, 0.9),
            MountTrimSampler.percentile(values.reversed(), 0.9),
            1e-9,
        )
    }

    // ── item 30c: the state the panel shows without opening anything ───────

    @Test
    fun theProvenanceChipNamesTheStateInWords() {
        val now = 1_700_000_000_000L
        val trim = MountTrim.fromHoldOrientation(
            hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(132.8)),
            capturedAtEpochMillis = now - 2 * 60_000L,
            sampleCount = 31,
            spreadDeg = 0.82,
            spreadP90Deg = 0.41,
        )
        val set = MountTrimProvenances.describe(StoredMountTrim(trim, "run-1"), "run-1", now)
        assertEquals("MOUNT SET · 132.8° · 2 min ago", set.chipLabel)

        // And the absence is stated just as plainly. "Set mount reference" — a
        // button — was the ONLY mount affordance before, and it looks identical
        // whether the pushbroom is on a measured re-zero or on a CAD nominal
        // that is 132 degrees wrong.
        val none = MountTrimProvenances.describe(null, "run-1", now)
        assertEquals("NO MOUNT REF · CAD NOMINAL", none.chipLabel)

        // The age has to re-tick, so it must be derived from `now` rather than
        // frozen at capture time.
        val later = MountTrimProvenances.describe(StoredMountTrim(trim, "run-1"), "run-1", now + 3_600_000L)
        assertEquals("MOUNT SET · 132.8° · 1 h ago", later.chipLabel)
    }
}
