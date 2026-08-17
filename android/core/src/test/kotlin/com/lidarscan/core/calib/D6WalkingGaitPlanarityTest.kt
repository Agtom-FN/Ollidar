package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 7 — **the walking-gait wall test, Android side.**
 *
 * The owner's sentence is the specification: *"when i walk through the room its
 * not given a stable scan with straight walls."*
 *
 * [D6PushbroomGeometryTest] (ROUND 5 AUDIT) already proved the *axes* of this
 * chain are right — that a straight walk extrudes along the walk and the fan
 * stays bounded. It could not say anything about straightness, because it used
 * one pose per revolution and a perfectly smooth trajectory, which is the one
 * combination in which per-point pairing does not matter.
 *
 * This test asks the field question instead. A person walking is not a dolly:
 * they sway sideways and bob at about 2 Hz and the phone yaws a few degrees with
 * every step. So: walk 4 s at 1 m/s past a flat wall 2 m to the right, with that
 * gait, sampling ARCore poses at 30 Hz and the D6 at 10 Hz with a real angular
 * sweep, resolve every return through **the production
 * `BracketNominals.cadNominal(COIN_D6)` matrix** and A8's documented formula,
 * and fit a plane to what comes back.
 *
 * Two arms, one stimulus:
 *
 *  * **per-point pose pairing** — each return resolved at its own interpolated
 *    pose. This is what the D6 path produces as of ROUND 7 (the driver now
 *    back-dates every point from its byte position at a known baud, so the
 *    per-point times A8 always interpolated against are finally real).
 *  * **one pose per revolution** — the falsifiable control. Same wall, same
 *    ranges, same mount, same poses; only the timestamps collapsed. It must
 *    fail the same bar, or this test proves nothing.
 *
 * The interpolation here is written from A8's spec (position LERP, orientation
 * shortest-arc SLERP) rather than called into `engine/`, deliberately: this is
 * an independent check of the Android nominal against the documented contract,
 * the same posture `D6PushbroomGeometryTest` takes.
 */
class D6WalkingGaitPlanarityTest {

    // --- the walk ----------------------------------------------------------
    //
    // ARCore's world frame: +Y is up (gravity-aligned), and the camera looks
    // along -Z at session start. Walking forward is therefore -Z, and the
    // owner's phone-back D6 sweeps the phone's own XY plane — which, with the
    // CAD nominal's identity rotation, is the vertical plane spanned by "right"
    // and "up". That is the pushbroom.

    private companion object {
        const val SPEED_MPS = 1.0
        const val STEP_HZ = 2.0
        const val LATERAL_AMP_M = 0.020 // +/- 2 cm of side-to-side sway
        const val VERTICAL_AMP_M = 0.030 // +/- 3 cm of bob
        const val YAW_AMP_RAD = 0.052 // +/- 3.0 deg of per-step yaw
        const val ROLL_AMP_RAD = 0.030 // +/- 1.7 deg of roll
        const val EYE_HEIGHT_M = 1.35

        const val WALL_X_M = 2.0
        const val WALK_SECONDS = 4.0
        const val POSE_HZ = 30.0 // ARCore
        const val REVOLUTION_HZ = 10.0 // COIN-D6
        const val RETURNS_PER_REVOLUTION = 360

        /** The owner's tolerance for "a straight wall". */
        const val PLANARITY_BAR_M = 0.02
    }

    private fun positionAt(t: Double): Vec3 {
        val w = 2.0 * PI * STEP_HZ
        return Vec3(
            x = LATERAL_AMP_M * sin(w * t),
            y = EYE_HEIGHT_M + VERTICAL_AMP_M * sin(w * t + 0.9),
            z = -SPEED_MPS * t,
        )
    }

    private fun orientationAt(t: Double): Quat {
        val w = 2.0 * PI * STEP_HZ
        val yaw = Quat.fromAxisAngle(Vec3(0.0, 1.0, 0.0), YAW_AMP_RAD * sin(w * t + 0.4))
        val roll = Quat.fromAxisAngle(Vec3(0.0, 0.0, 1.0), ROLL_AMP_RAD * sin(w * t + 2.1))
        return (yaw * roll).normalized()
    }

    private data class PoseSampleAt(val t: Double, val position: Vec3, val orientation: Quat)

    /** A8 §3.4: position LERP, orientation shortest-arc SLERP, between the bracketing poses. */
    private fun interpolate(poses: List<PoseSampleAt>, t: Double): Pair<Vec3, Quat> {
        var hi = poses.indexOfFirst { it.t >= t }
        if (hi <= 0) hi = 1
        val a = poses[hi - 1]
        val b = poses[hi]
        val u = ((t - a.t) / (b.t - a.t)).coerceIn(0.0, 1.0)
        val pos = a.position + (b.position - a.position) * u
        return pos to slerp(a.orientation, b.orientation, u)
    }

    private fun slerp(from: Quat, to: Quat, u: Double): Quat {
        val a = from.normalized()
        var b = to.normalized()
        var dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w
        // Shortest arc: q and -q are the same rotation (the double cover A8 §3.4
        // and MountTrimSampler both warn about).
        if (dot < 0.0) {
            b = Quat(-b.x, -b.y, -b.z, -b.w)
            dot = -dot
        }
        if (dot > 0.9995) {
            return Quat(
                a.x + (b.x - a.x) * u,
                a.y + (b.y - a.y) * u,
                a.z + (b.z - a.z) * u,
                a.w + (b.w - a.w) * u,
            ).normalized()
        }
        val theta = acos(dot.coerceIn(-1.0, 1.0))
        val s = sin(theta)
        val wa = sin((1.0 - u) * theta) / s
        val wb = sin(u * theta) / s
        return Quat(
            a.x * wa + b.x * wb,
            a.y * wa + b.y * wb,
            a.z * wa + b.z * wb,
            a.w * wa + b.w * wb,
        ).normalized()
    }

    /**
     * The true range from the rig's true pose at [t] to the wall at
     * `x = WALL_X_M`, for a fan return at [angleDeg], or null when that ray
     * misses the wall (or hits it outside a real room's floor/ceiling).
     */
    private fun rangeToWall(t: Double, angleDeg: Double, phoneFromLidar: Mat4): Double? {
        val a = angleDeg * PI / 180.0
        // A8 §3.1's sensor-frame convention, verbatim.
        val dirLidar = Vec3(sin(a), cos(a), 0.0)
        val dirPhone = phoneFromLidar.rotationQuat().rotate(dirLidar)
        val dirWorld = orientationAt(t).rotate(dirPhone)
        if (abs(dirWorld.x) < 1e-6) return null
        val pos = positionAt(t)
        val lever = phoneFromLidar.translation
        val origin = pos + orientationAt(t).rotate(lever)
        val d = (WALL_X_M - origin.x) / dirWorld.x
        if (d < 0.4 || d > 8.0) return null
        val y = origin.y + d * dirWorld.y
        if (y < 0.05 || y > 2.9) return null // floor / ceiling, not the wall
        return d
    }

    /**
     * RMS distance of the resolved points to their own best-fit plane
     * `x = a + b*y + c*z`.
     *
     * A best-fit plane, not the known wall: a constant offset is a *time
     * offset* symptom (the sensor-latency setting's job), not a bending one, and
     * it must not be allowed to masquerade as "the wall is bent". Tilt and bow
     * both survive the fit and land in the residual, which is what the eye
     * calls a crooked wall.
     */
    private fun planeFitRmsMetres(points: List<Vec3>): Double {
        val s = Array(3) { DoubleArray(4) }
        for (p in points) {
            val basis = doubleArrayOf(1.0, p.y, p.z)
            for (i in 0..2) {
                for (j in 0..2) s[i][j] += basis[i] * basis[j]
                s[i][3] += basis[i] * p.x
            }
        }
        for (c in 0..2) {
            var piv = c
            for (r in c + 1..2) if (abs(s[r][c]) > abs(s[piv][c])) piv = r
            val tmp = s[c]; s[c] = s[piv]; s[piv] = tmp
            val d = s[c][c]
            check(abs(d) > 1e-12) { "degenerate plane fit — the stimulus is wrong, not the code under test" }
            for (j in c..3) s[c][j] /= d
            for (r in 0..2) {
                if (r == c) continue
                val f = s[r][c]
                for (j in c..3) s[r][j] -= f * s[c][j]
            }
        }
        val (a, b, cc) = Triple(s[0][3], s[1][3], s[2][3])
        var sum = 0.0
        for (p in points) {
            val resid = p.x - (a + b * p.y + cc * p.z)
            sum += resid * resid
        }
        // x = a + b y + c z has normal (1, -b, -c); dividing turns the
        // axis-aligned residual into a perpendicular distance.
        return sqrt(sum / points.size) / sqrt(1.0 + b * b + cc * cc)
    }

    private fun walkPastAWall(perPointTime: Boolean): List<Vec3> {
        val phoneFromLidar = BracketNominals.cadNominal(SensorType.COIN_D6)

        // ARCore's pose stream, one sample every 33.3 ms, covering the walk
        // with a margin so nothing extrapolates.
        val poseDt = 1.0 / POSE_HZ
        val poses = buildList {
            var t = -2 * poseDt
            while (t <= WALK_SECONDS + 2 * poseDt) {
                add(PoseSampleAt(t, positionAt(t), orientationAt(t)))
                t += poseDt
            }
        }

        val revolutions = (WALK_SECONDS * REVOLUTION_HZ).toInt()
        val revSeconds = 1.0 / REVOLUTION_HZ
        val out = ArrayList<Vec3>(revolutions * 120)
        for (rev in 0 until revolutions) {
            val tRev = rev * revSeconds
            for (i in 0 until RETURNS_PER_REVOLUTION) {
                val tPoint = tRev + revSeconds * i / RETURNS_PER_REVOLUTION
                val angle = 360.0 * i / RETURNS_PER_REVOLUTION
                // The RANGE is always measured at the return's true time — that
                // is physics, and it is identical in both arms. Only the time
                // the resolver is told differs.
                val d = rangeToWall(tPoint, angle, phoneFromLidar) ?: continue

                val tResolve = if (perPointTime) tPoint else tRev
                val (pos, orient) = interpolate(poses, tResolve)
                // A8 §3.1: world_from_lidar = world_from_phone(t) · phone_from_lidar
                val worldFromPhone = Mat4.fromRotationTranslation(orient, pos)
                val worldFromLidar = worldFromPhone * phoneFromLidar
                val a = angle * PI / 180.0
                out.add(worldFromLidar.transform(Vec3(d * sin(a), d * cos(a), 0.0)))
            }
        }
        return out
    }

    @Test
    fun `a wall walked past at 1 m per s with gait sway resolves planar within 2 cm`() {
        val points = walkPastAWall(perPointTime = true)
        assertTrue("stimulus too thin: ${points.size} returns hit the wall", points.size > 2000)

        val rms = planeFitRmsMetres(points)
        println("per-point pairing: plane-fit RMS = ${"%.4f".format(rms * 100.0)} cm over ${points.size} pts")
        assertTrue("wall is not planar: RMS ${rms * 100} cm", rms < PLANARITY_BAR_M)
        // Far under the bar, and pinned there: the residual left is the
        // interpolator's own error against a 2 Hz sinusoid sampled at 30 Hz,
        // which is sub-millimetre. A creep to 1 cm is a regression, not noise.
        assertTrue("planarity regressed: RMS ${rms * 100} cm", rms < 0.004)
    }

    @Test
    fun `the same wall with one pose per revolution is measurably bent`() {
        val perPoint = planeFitRmsMetres(walkPastAWall(perPointTime = true))
        val perRevolution = planeFitRmsMetres(walkPastAWall(perPointTime = false))
        println(
            "one-pose-per-revolution: plane-fit RMS = ${"%.4f".format(perRevolution * 100.0)} cm " +
                "(per-point: ${"%.4f".format(perPoint * 100.0)} cm)",
        )
        // The falsifiable half. If collapsing 100 ms of returns onto one pose
        // ever stops mattering, the test above stops proving anything and this
        // one says so.
        assertTrue(
            "per-revolution pairing was supposed to be measurably worse: " +
                "${perRevolution * 100} cm vs ${perPoint * 100} cm",
            perRevolution > 4.0 * perPoint,
        )
    }
}
