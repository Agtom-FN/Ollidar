package com.lidarscan.core.calib

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 20 (item 79) — the gravity-referenced trim, pinned on the OWNER'S OWN
 * quaternions, not synthetic ones.
 *
 * Trim A is the stored trim scans 054/055 ran on; trim B is scan-056's —
 * captured 3.5 minutes apart on the same physical rig, from the owner's
 * 2026-08-19 session, read verbatim out of the bundles' project.json. The
 * round-20 adjudication measured them 23.19 deg apart, swapped them between
 * the scans with engine_cli --mount-from, and found B better on ALL THREE
 * scans (selfCheck 1.97→1.22 / 5.79→4.48 / 2.75→1.45 cm) — proof that the
 * difference was mostly junk, not mostly signal. The decomposition must
 * remove exactly the junk: the about-gravity yaw (51.6 deg of A, 21.1 deg of
 * B, measured against each scan's own dead session origin), leaving the
 * genuine hold-tilt difference (12.48 deg) for item 80's backstop to catch.
 */
class MountTrimRound20Test {

    // The owner's stored trims, (x, y, z, w), verbatim.
    private val trimA = Quat(
        -0.14713701533870038,
        0.30202905772616806,
        0.7044345907133807,
        0.6252208045264762,
    ).normalized()
    private val trimB = Quat(
        0.06554833001012035,
        -0.12817674402676563,
        -0.7117615245781093,
        -0.687509760551679,
    ).normalized()

    private fun degBetween(a: Quat, b: Quat) = Math.toDegrees(a.angleTo(b))

    @Test
    fun `the owner's two raw trims disagree by the measured 23 degrees`() {
        assertEquals(23.19, degBetween(trimA, trimB), 0.01)
    }

    @Test
    fun `yaw-normalised, the same two trims keep only their genuine tilt difference`() {
        val a = MountTrim(trimA.x, trimA.y, trimA.z, trimA.w).yawNormalized()
        val b = MountTrim(trimB.x, trimB.y, trimB.z, trimB.w).yawNormalized()
        // 12.48 deg of REAL hold-tilt difference remains (A was held 14.1 deg
        // off vertical, B 5.4 deg — different holds). The 10.7 deg that
        // vanished was yaw about gravity, which a static hold cannot observe.
        assertEquals(12.48, degBetween(a.rotation, b.rotation), 0.05)
        // Both keep the Rz(90)-class working magnitude every healthy trim has.
        assertEquals(92.05, a.magnitudeDeg, 0.05)
        assertEquals(91.25, b.magnitudeDeg, 0.05)
    }

    @Test
    fun `a gravity-referenced trim's quaternion has no about-gravity component`() {
        // The swing of a Y-axis swing-twist decomposition has zero y — the
        // invariant that says "no yaw was invented".
        val a = MountTrim(trimA.x, trimA.y, trimA.z, trimA.w).yawNormalized()
        val b = MountTrim(trimB.x, trimB.y, trimB.z, trimB.w).yawNormalized()
        assertEquals(0.0, a.qy, 1e-12)
        assertEquals(0.0, b.qy, 1e-12)
        val fresh = MountTrim.fromHoldOrientation(
            (
                Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), Math.toRadians(37.0)) *
                    Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), Math.toRadians(-86.0)) *
                    Quat.fromAxisAngle(Vec3(1.0, 0.0, 0.0), Math.toRadians(9.0))
                ).normalized(),
        )
        assertEquals(0.0, fresh.qy, 1e-12)
        assertTrue(fresh.gravityReferenced)
    }

    @Test
    fun `yaw normalisation is idempotent`() {
        val once = MountTrim(trimA.x, trimA.y, trimA.z, trimA.w).yawNormalized()
        val twice = once.yawNormalized()
        assertEquals(0.0, degBetween(once.rotation, twice.rotation), 1e-9)
        assertTrue(once.gravityReferenced)
    }

    @Test
    fun `an upright hold facing ANY direction produces the same trim`() {
        // The whole point: the operator's facing direction (and the session's
        // yaw origin) must not enter phone_from_lidar. An upright portrait
        // hold is Rz(-90) in the sensor frame; yaw it anywhere and the trim
        // must not move.
        val portrait = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), Math.toRadians(-90.0))
        val trims = listOf(0.0, 37.1, 134.0, -51.6, 180.0).map { yawDeg ->
            val hold = (Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), Math.toRadians(yawDeg)) * portrait)
                .normalized()
            MountTrim.fromHoldOrientation(hold)
        }
        for (t in trims.drop(1)) {
            assertEquals(0.0, degBetween(trims.first().rotation, t.rotation), 1e-9)
        }
        // And that shared answer is the Rz(+90) working rotation.
        assertEquals(90.0, trims.first().magnitudeDeg, 1e-9)
    }

    @Test
    fun `a legacy persisted trim decodes as not gravity-referenced`() {
        val legacy = kotlinx.serialization.json.Json.decodeFromString<MountTrim>(
            """{"qx":-0.147137,"qy":0.302029,"qz":0.704434,"qw":0.625220}""",
        )
        assertTrue(!legacy.gravityReferenced)
        assertTrue(legacy.yawNormalized().gravityReferenced)
    }

    @Test
    fun `hold tilt still enters the trim - item 80 is the backstop, not this`() {
        // A hold tilted off vertical keeps that tilt in the trim (a static
        // hold cannot distinguish hand tilt from mount tilt). Honesty check:
        // the decomposition removes the about-gravity twist ONLY.
        val portrait = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), Math.toRadians(-90.0))
        val upright = MountTrim.fromHoldOrientation(portrait)
        // Roll about the horizontal Z (the view axis of this hold) is pure
        // swing — it survives exactly.
        val rolled = (Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), Math.toRadians(5.0)) * portrait)
            .normalized()
        assertEquals(
            5.0,
            degBetween(upright.rotation, MountTrim.fromHoldOrientation(rolled).rotation),
            1e-6,
        )
        // Pitch about the horizontal X axis also survives (7.07 deg of trim
        // change for a 5 deg world tilt — the decomposition is about Y, so a
        // horizontal tilt maps through it, never to zero).
        val pitched = (Quat.fromAxisAngle(Vec3(1.0, 0.0, 0.0), Math.toRadians(5.0)) * portrait)
            .normalized()
        assertEquals(
            7.07,
            degBetween(upright.rotation, MountTrim.fromHoldOrientation(pitched).rotation),
            0.01,
        )
    }
}
