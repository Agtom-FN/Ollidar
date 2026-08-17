package com.lidarscan.core.calib

import com.lidarscan.core.capture.PoseSample
import com.lidarscan.core.model.SensorType
import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 6, owner item 23 — the composition math for the one-tap mount re-zero.
 *
 * No ARCore device exists in this environment, so this arithmetic is the only
 * part of item 23 that CAN be proven here, and it is the part that decides
 * whether every resolved point is where it belongs. The cases below are the ones
 * the owner's rig can actually be in: nominal, tilted, rolled 90°, upside down,
 * and a compound of all three.
 */
class MountTrimTest {

    private fun deg(d: Double) = Math.toRadians(d)

    private fun aboutEqual(a: Double, b: Double, tol: Double = 1e-9) =
        abs(a - b) <= tol

    private fun assertMat4Equals(expected: Mat4, actual: Mat4, tol: Double = 1e-9) {
        for (i in 0 until 16) {
            assertTrue(
                "element $i: expected ${expected.m[i]} got ${actual.m[i]}",
                aboutEqual(expected.m[i], actual.m[i], tol),
            )
        }
    }

    // ── the composition itself ─────────────────────────────────────────────

    @Test
    fun `a rig held exactly at the reference produces an identity trim and leaves the nominal untouched`() {
        val trim = MountTrim.fromHoldOrientation(MountTrim.REFERENCE_HOLD)
        assertTrue("identity hold must give a ~0 deg trim", trim.magnitudeDeg < 1e-9)

        val nominal = BracketNominals.cadNominal(SensorType.COIN_D6)
        assertMat4Equals(nominal, trim.composedWith(nominal))
    }

    @Test
    fun `a 90 degree roll is cancelled exactly - the trim is the inverse of the hold`() {
        // The rig is held rolled 90° about the phone's own forward axis (z).
        val hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(90.0))
        val trim = MountTrim.fromHoldOrientation(hold)

        // trim == hold⁻¹, so hold ∘ trim is the identity: the lidar ends up in
        // exactly the world attitude the nominal assumed.
        val roundTrip = (hold * trim.rotation).normalized()
        assertTrue(
            "hold ∘ trim must be identity, got ${Math.toDegrees(Quat.IDENTITY.angleTo(roundTrip))} deg",
            Quat.IDENTITY.angleTo(roundTrip) < 1e-9,
        )
        assertEquals(90.0, trim.magnitudeDeg, 1e-6)
    }

    @Test
    fun `upside down is handled without the quaternion double-cover flipping the sign`() {
        val hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(180.0))
        val trim = MountTrim.fromHoldOrientation(hold)
        assertEquals("a 180 deg hold is a 180 deg trim", 180.0, trim.magnitudeDeg, 1e-6)

        // The composed extrinsic must still be a legal rigid transform — the
        // engine rejects anything else with SCAN_ERR_INVALID_ARGUMENT rather
        // than producing a mirrored cloud.
        val composed = trim.composedWith(BracketNominals.cadNominal(SensorType.COIN_D6))
        assertTrue("composed extrinsic must stay rigid", composed.isRigid(1e-9))

        // And the same rotation with the opposite quaternion sign must give the
        // same trim: ARCore hands out either.
        val negated = Quat(-hold.x, -hold.y, -hold.z, -hold.w)
        val trimNegated = MountTrim.fromHoldOrientation(negated)
        assertEquals(trim.magnitudeDeg, trimNegated.magnitudeDeg, 1e-9)
    }

    @Test
    fun `a compound tilt composes onto the nominal and stays rigid, and the translation is the nominal's`() {
        val hold = (
            Quat.fromAxisAngle(Vec3(1.0, 0.0, 0.0), deg(12.0)) *
                Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), deg(-37.0)) *
                Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(94.0))
            ).normalized()
        val trim = MountTrim.fromHoldOrientation(hold)
        val nominal = BracketNominals.cadNominal(SensorType.COIN_D6)
        val composed = trim.composedWith(nominal)

        assertTrue("composed extrinsic must stay rigid", composed.isRigid(1e-9))

        // A re-zero measures attitude, never lever arm: the translation must be
        // the nominal's verbatim, not the nominal's rotated by the trim.
        assertEquals(nominal.translation.x, composed.translation.x, 1e-12)
        assertEquals(nominal.translation.y, composed.translation.y, 1e-12)
        assertEquals(nominal.translation.z, composed.translation.z, 1e-12)
    }

    @Test
    fun `the composed rotation is trim x nominal, in that order`() {
        val hold = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), deg(30.0))
        val trim = MountTrim.fromHoldOrientation(hold)
        val nominal = BracketNominals.cadNominal(SensorType.COIN_D6)

        val expectedRotation = Mat4.fromRotationTranslation(trim.rotation, Vec3.ZERO) * nominal
        val composed = trim.composedWith(nominal)
        // Rotation block only — the translation is deliberately the nominal's.
        val e = expectedRotation.rotation()
        val a = composed.rotation()
        for (i in e.indices) assertEquals("rotation element $i", e[i], a[i], 1e-12)
    }

    @Test
    fun `a Mid360 trim is composed onto the Mid360 nominal, not the D6 one`() {
        val hold = Quat.fromAxisAngle(Vec3(1.0, 0.0, 0.0), deg(20.0))
        val trim = MountTrim.fromHoldOrientation(hold, sensor = SensorType.MID360)
        val nominal = BracketNominals.cadNominal(SensorType.MID360)
        val composed = trim.composedWith(nominal)
        assertEquals(SensorType.MID360, trim.sensor)
        assertEquals(nominal.translation.z, composed.translation.z, 1e-12)
    }

    // ── the sampler's rejection gates ──────────────────────────────────────

    private fun stillWindow(
        orientation: Quat,
        count: Int = 30,
        spanMs: Long = 1_000L,
        tracking: Boolean = true,
        jitterDeg: Double = 0.0,
    ): List<PoseSample> = (0 until count).map { i ->
        val t = i * (spanMs * 1_000_000L / (count - 1).coerceAtLeast(1))
        val jitter = if (jitterDeg == 0.0) {
            Quat.IDENTITY
        } else {
            // Deterministic wobble, alternating sign, well inside the gate.
            Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), deg(jitterDeg * if (i % 2 == 0) 1.0 else -1.0))
        }
        PoseSample(
            tMonoNs = t,
            position = Vec3.ZERO,
            orientation = (orientation * jitter).normalized(),
            tracking = tracking,
        )
    }

    @Test
    fun `a still one-second hold is accepted and recovers the held attitude`() {
        val hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(45.0))
        val result = MountTrimSampler.capture(stillWindow(hold), nowMillis = 1_700_000_000_000L)
        val captured = result as? MountTrimResult.Captured
            ?: error("expected a captured trim, got $result")
        assertEquals("the trim must invert the held attitude", 45.0, captured.trim.magnitudeDeg, 1e-6)
        // ROUND 8: the single `MAX_SPREAD_DEG` became two gates. `spreadDeg`
        // keeps its ROUND 6 meaning (the WORST deviation), so it is checked
        // against the outlier ceiling; the p90 is checked against the steadiness
        // limit. See MountTrim's companion for the field log behind the split.
        assertTrue("spread must be reported", captured.trim.spreadDeg < MountTrim.MAX_SPREAD_OUTLIER_DEG)
        assertTrue("p90 spread must be reported", captured.trim.spreadP90Deg <= MountTrim.MAX_SPREAD_P90_DEG)
        assertEquals(1_700_000_000_000L, captured.trim.capturedAtEpochMillis)
    }

    @Test
    fun `sub-degree jitter is tolerated - a hand-held rig is never perfectly still`() {
        val hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(20.0))
        val result = MountTrimSampler.capture(stillWindow(hold, jitterDeg = 0.3), nowMillis = 0L)
        assertTrue("0.3 deg of wobble must not be rejected, got $result", result is MountTrimResult.Captured)
    }

    /**
     * ROUND 8: the refusals are compared on their REASON, not on the whole
     * `Rejected` value.
     *
     * `Rejected` now carries a [MountTrimMeasurement] — the numbers the owner's
     * eight identical `mount re-zero refused: MOVING` log lines did not have —
     * so a whole-value `assertEquals` would be asserting the exact p90 of a
     * synthetic wobble, which is a test of the fixture rather than of the gate.
     */
    private fun reasonOf(result: MountTrimResult): MountTrimRejection? =
        (result as? MountTrimResult.Rejected)?.reason

    @Test
    fun `a rig that moved is refused`() {
        val hold = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(20.0))
        val result = MountTrimSampler.capture(stillWindow(hold, jitterDeg = 6.0), nowMillis = 0L)
        assertEquals(MountTrimRejection.MOVING, reasonOf(result))
    }

    @Test
    fun `a window with a tracking dropout is refused`() {
        val hold = Quat.IDENTITY
        val samples = stillWindow(hold).toMutableList()
        samples[5] = samples[5].copy(tracking = false)
        assertEquals(
            MountTrimRejection.NOT_TRACKING,
            reasonOf(MountTrimSampler.capture(samples, nowMillis = 0L)),
        )
    }

    @Test
    fun `too few samples, and too short a hold, are both refused`() {
        assertEquals(
            MountTrimRejection.NO_POSES,
            reasonOf(MountTrimSampler.capture(emptyList(), nowMillis = 0L)),
        )
        assertEquals(
            "four frames is not a one-second hold",
            MountTrimRejection.NOT_ENOUGH_SAMPLES,
            reasonOf(MountTrimSampler.capture(stillWindow(Quat.IDENTITY, count = 4), nowMillis = 0L)),
        )
        assertEquals(
            "30 frames crammed into 200 ms is not a one-second hold either",
            MountTrimRejection.NOT_ENOUGH_SAMPLES,
            reasonOf(MountTrimSampler.capture(stillWindow(Quat.IDENTITY, spanMs = 200L), nowMillis = 0L)),
        )
    }

    @Test
    fun `only the most recent window is averaged - an older, different attitude is ignored`() {
        val old = stillWindow(Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(80.0)), count = 20, spanMs = 1_000)
        val recent = stillWindow(Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), deg(10.0)), count = 30, spanMs = 1_000)
            .map { it.copy(tMonoNs = it.tMonoNs + 5_000_000_000L) }
        val result = MountTrimSampler.capture(old + recent, nowMillis = 0L, windowMs = 1_200L)
        val captured = result as? MountTrimResult.Captured ?: error("expected a capture, got $result")
        assertEquals("only the last ~1.2 s counts", 10.0, captured.trim.magnitudeDeg, 1e-6)
    }

    @Test
    fun `the mean of sign-flipped quaternions does not cancel to zero`() {
        val q = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), deg(50.0))
        val flipped = Quat(-q.x, -q.y, -q.z, -q.w)
        val mean = MountTrimSampler.meanOrientation(listOf(q, flipped, q, flipped))
        assertTrue(
            "q and -q are the same rotation; their mean must be that rotation, not zero",
            mean.angleTo(q) < 1e-9,
        )
    }

    // ── age labelling, because it drives the "re-zero" affordance ──────────

    @Test
    fun `age reads in human units and never goes negative`() {
        val trim = MountTrim.fromHoldOrientation(Quat.IDENTITY, capturedAtEpochMillis = 1_000_000L)
        assertEquals("just now", trim.ageLabel(1_005_000L))
        assertEquals("45s ago", trim.ageLabel(1_045_000L))
        assertEquals("3 min ago", trim.ageLabel(1_000_000L + 200_000L))
        assertEquals("2 h ago", trim.ageLabel(1_000_000L + 7_400_000L))
        assertFalse("a clock that went backwards must not print a negative age", trim.ageMillis(0L) < 0L)
    }
}
