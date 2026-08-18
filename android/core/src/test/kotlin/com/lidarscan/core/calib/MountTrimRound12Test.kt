package com.lidarscan.core.calib

import com.lidarscan.core.capture.PoseSample

import com.lidarscan.core.calib.Vec3
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 12 — the scan-028 counterexample, reproduced and then closed.
 *
 * ## What came back from the field
 *
 * `captures/scan-028.lscan/project.json`, recorded on 0.7.0 with the round-11
 * guided hold shipping:
 *
 * ```json
 * "mountTrim": { "sampleCount": 244, "spreadDeg": 3.58, "spreadP90Deg": 2.40 }
 * ```
 *
 * 244 samples at 30 Hz is 8.1 s — the `maxHoldMillis` TIMEOUT path, so the hold
 * ran to the end of its budget without ever converging — and it stored a
 * `spreadP90Deg` of 2.40 deg, which is numerically identical to what
 * `scan-020` recorded on 0.6.0 before item 45 existed. Read at face value the
 * refinement had achieved nothing.
 *
 * ## Two separate defects, and the interesting one is the second
 *
 * 1. **The whole-hold mean was stored without its own spread being gated.**
 *    `MountTrimSampler.capture` judges the trailing 1 s; the refiner then threw
 *    that trim away and stored a mean over the whole 8 s whose dispersion was
 *    compared against nothing. Covered by the first two tests here.
 *
 * 2. **`spreadP90Deg` is not an accuracy figure and was being read as one.**
 *    `scan-026` reads 0.44 deg over 34 samples and `scan-028` reads 2.40 deg
 *    over 244 — a 5x difference in the only quality number on disk. The two
 *    stored trims are **1.33 deg apart**, and re-resolving each capture through
 *    the production pipeline with the OTHER one's mount extrinsic changes the
 *    map's self-consistency by 5 % and its occupied-voxel count by 0.15 %
 *    (`engine_cli --d6-selfcheck --mount-from`, numbers in NOTES ROUND 12).
 *    A one-second dispersion and an eight-second dispersion were never
 *    comparable quantities. The fix is to store the split-half accuracy, which
 *    IS comparable, and to compare on it.
 */
class MountTrimRound12Test {

    private fun rotY(deg: Double): Quat {
        val h = Math.toRadians(deg) / 2.0
        return Quat(0.0, sin(h), 0.0, cos(h)).normalized()
    }

    /** 30 Hz of poses; `wobbleDeg` is a sinusoid at `wobbleHz`. */
    private fun hold(
        millis: Long,
        wobbleDeg: Double,
        wobbleHz: Double = 0.25,
        startNs: Long = 0L,
        tracking: Boolean = true,
    ): List<PoseSample> {
        val out = ArrayList<PoseSample>()
        val stepNs = 33_333_333L
        var t = startNs
        while (t < startNs + millis * 1_000_000L) {
            val s = (t - startNs) / 1e9
            out.add(
                PoseSample(
                    tMonoNs = t,
                    position = Vec3(0.0, 0.0, 0.0),
                    orientation = rotY(wobbleDeg * sin(2 * PI * wobbleHz * s)),
                    tracking = tracking,
                ),
            )
            t += stepNs
        }
        return out
    }

    @Test
    fun `a hold that wandered early is not stored as if it had not`() {
        // Seven seconds of 3 deg wander, then one still second. This is
        // scan-028's shape: the gate sees only the still second and says yes.
        val wandered = hold(millis = 7_000, wobbleDeg = 3.0, wobbleHz = 0.5, startNs = 0L)
        val still = hold(
            millis = 1_200,
            wobbleDeg = 0.05,
            startNs = wandered.last().tMonoNs + 33_333_333L,
        )
        val all = wandered + still

        // The gate, over the trailing second only, passes — that is the
        // premise, and it is what 0.7.0 relied on.
        val gateOnly = MountTrimSampler.capture(all, nowMillis = 1_000L)
        assertTrue("the still second must pass the gate", gateOnly is MountTrimResult.Captured)

        val refiner = MountTrimRefiner()
        val result = refiner.capture(all, holdStartedAtMonoNs = 0L, nowMillis = 1_000L)
        assertTrue(result is MountTrimResult.Captured)
        val trim = (result as MountTrimResult.Captured).trim

        // The stored trim must not carry the wander's dispersion. Before ROUND
        // 12 this asserted 244-sample / 2.4 deg behaviour: sampleCount would be
        // the whole ~250-sample hold and spreadP90Deg would be several degrees.
        assertTrue(
            "stored spreadP90 ${trim.spreadP90Deg} must clear the same gate the trim passed",
            trim.spreadP90Deg <= MountTrim.MAX_SPREAD_P90_DEG,
        )
        assertTrue(
            "stored spread ${trim.spreadDeg} must clear the outlier ceiling",
            trim.spreadDeg <= MountTrim.MAX_SPREAD_OUTLIER_DEG,
        )
        // And the accuracy figure IS recorded, so the next field report has the
        // number that means something.
        assertNotNull("the split-half accuracy must reach the container", trim.accuracyDeg)
    }

    @Test
    fun `a genuinely still long hold still averages over the whole hold`() {
        // The feature round 11 shipped must survive the fix: six still seconds
        // must still be averaged over six seconds, not over one.
        val samples = hold(millis = 6_000, wobbleDeg = 0.2, wobbleHz = 0.3)
        val refiner = MountTrimRefiner()
        val result = refiner.capture(samples, holdStartedAtMonoNs = 0L, nowMillis = 1_000L)
        assertTrue(result is MountTrimResult.Captured)
        val trim = (result as MountTrimResult.Captured).trim
        assertTrue("a 6 s hold at 30 Hz is ~180 samples, got ${trim.sampleCount}", trim.sampleCount > 100)
        assertTrue(trim.spreadP90Deg <= MountTrim.MAX_SPREAD_P90_DEG)
        assertNotNull(trim.accuracyDeg)
    }

    @Test
    fun `the ring does not report a passing gate before a hold exists`() {
        // The owner's log has `mount hold released early: holdMs=0 samples=1
        // ... gate=true` twice. A gate verdict about a hold that has not
        // happened is not a verdict.
        val samples = hold(millis = 2_000, wobbleDeg = 0.1)
        val refiner = MountTrimRefiner()
        val firstTick = refiner.evaluate(samples, holdStartedAtMonoNs = samples.last().tMonoNs)
        assertFalse("no hold has accumulated yet", firstTick.gatePasses)
        assertFalse(firstTick.done)

        // ... and once a real hold exists, it does pass.
        val later = refiner.evaluate(samples, holdStartedAtMonoNs = 0L)
        assertTrue(later.gatePasses)
    }

    @Test
    fun `untracked frames are excluded from the hold the ring reports`() {
        val tracked = hold(millis = 1_500, wobbleDeg = 0.1, startNs = 0L)
        val lost = hold(
            millis = 1_000,
            wobbleDeg = 0.1,
            startNs = tracked.last().tMonoNs + 33_333_333L,
            tracking = false,
        )
        val refiner = MountTrimRefiner()
        val p = refiner.evaluate(tracked + lost, holdStartedAtMonoNs = 0L)
        // `samples` counts the hold, and the hold is the tracked frames only —
        // the same set capture() averages. Before ROUND 12 the two disagreed.
        assertEquals(tracked.size, p.samples)
    }

    @Test
    fun `qualityRank prefers a measured trim over an unmeasured one whatever the p90 says`() {
        // scan-026 vs scan-028 in miniature: the p90s say one thing, and the
        // p90s are not comparable.
        val measuredButMiddling = MountTrim(
            qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0,
            spreadP90Deg = 2.40, sampleCount = 244, stabilityDeg = 0.35,
        )
        val unmeasuredButTightP90 = MountTrim(
            qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0,
            spreadP90Deg = 0.44, sampleCount = 34, stabilityDeg = -1.0,
        )
        assertTrue(
            "a measured 0.35 deg accuracy must beat an unmeasured 0.44 deg dispersion",
            measuredButMiddling.qualityRank < unmeasuredButTightP90.qualityRank,
        )
        assertFalse(measuredButMiddling.accuracyIsPoor)

        val poor = measuredButMiddling.copy(stabilityDeg = 1.6)
        assertTrue("1.6 deg is past the one-degree floor", poor.accuracyIsPoor)
        assertTrue(poor.qualityRank > measuredButMiddling.qualityRank)
    }

    @Test
    fun `an unmeasured trim is unknown rather than poor`() {
        val legacy = MountTrim(qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0, spreadP90Deg = 2.4)
        assertEquals(null, legacy.accuracyDeg)
        assertFalse("unknown is not a warning the operator can act on", legacy.accuracyIsPoor)
    }
}
