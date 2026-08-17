package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 9, owner item 34 — **the chirality test, Android side.**
 *
 * The owner's sentence is the whole specification: *"The output is left right
 * reversed."*
 *
 * ## Why this file exists at all
 *
 * The two D6 geometry tests that already sit in this directory are both
 * **MIRROR-INVARIANT**, and that is precisely why this bug survived eight
 * rounds of green builds:
 *
 *  * [D6PushbroomGeometryTest] asserts **axis extents** (`max - min` per axis)
 *    and nothing else. A mirrored room has byte-for-byte identical extents.
 *  * [D6WalkingGaitPlanarityTest] asserts **best-fit-plane RMS**. A mirrored
 *    wall is exactly as planar as the wall it is a mirror of.
 *
 * Both quantities are sign-blind. Neither can tell a room from its reflection.
 * Every engine-side test had the same blind spot (`se3::mat4_is_rigid` does not
 * fire either — see below). So the fan came out reflected for eight rounds and
 * the suite stayed green the entire time. This is the first Kotlin-side test
 * that can distinguish a scan from its mirror image, and it is the Kotlin
 * mirror of the engine's `tests/test_round9_chirality.cpp`.
 *
 * ## The frame under test
 *
 * `engine/include/scanengine/drivers/d6/d6_fan.h` is the authority; it derives
 * the frame in full. Right-handed, pinned to the physical unit:
 *
 * ```
 * +y  the 0-degree beam direction (the vendor's zero mark)
 * +z  the spin axis, out of the BASE of the unit (AWAY from the cap)
 * +x  = y x z
 *
 * p_lidar(theta, d) = (-d*sin theta,  d*cos theta,  0)
 * ```
 *
 * The negative `x` is the ROUND 9 fix. The vendor datasheet quotes its angle
 * convention in a **left-handed** system ("left-hand coordinate system ...
 * rotation angle increases clockwise", `docs/bench/BENCH_SETUP.md` §3.1); a
 * left-handed triple transcribed verbatim into a right-handed frame keeps `x`
 * and `y` and silently reverses the sense of rotation about the spin axis. The
 * pre-fix formula was `x = +d*sin theta`, a pure reflection across the y-z
 * plane.
 *
 * Restricted to the fan's own plane (`z == 0` for every return) that reflection
 * equals the *proper* rotation `diag(-1, +1, -1)`, so a mirrored fan is
 * indistinguishable from a mount extrinsic yawed 180 degrees — [Mat4.isRigid]
 * (and the engine's `se3::mat4_is_rigid`) sees a perfectly legal `det = +1`
 * matrix and stays silent. Only a *handedness* assertion catches it, which is
 * what the tests below are.
 *
 * ## What is deliberately NOT changed
 *
 * [BracketNominals.cadNominal] for [SensorType.COIN_D6] carries an **identity**
 * rotation, and it always has. Under the corrected frame the owner's mount —
 * D6 on the back of the phone, 0-degree beam UP, cap/top FORWARD along the walk
 * — maps `lidar +y -> camera +Y (up)`, `lidar +z (base) -> camera +Z
 * (backward, since ARCore looks along -Z, so the cap faces forward)`, `lidar +x
 * -> camera +X (right)`. The CAD nominal was never the wrong part; the fan
 * formula was. These tests compose the corrected formula through the real
 * production matrix to pin that.
 */
class D6ChiralityTest {

    private companion object {
        /** The owner's mount, as a walk: eye height, and how far along the walk we are. */
        const val EYE_HEIGHT_M = 1.35
        const val RANGE_M = 2.5

        /** Cosine bar for "this bearing IS that direction" — ~8 degrees of slop, far tighter than a sign flip. */
        const val ALIGNED = 0.99
    }

    /**
     * The corrected fan formula, `d6_fan.h` (1). [mirrored] flips the `x` sign
     * back to the pre-ROUND-9 convention (`x = +d*sin theta`) so the control
     * arms below can run the OLD convention through the very same production
     * code path and watch it land on the wrong side.
     */
    private fun fanPoint(thetaDeg: Double, rangeM: Double, mirrored: Boolean = false): Vec3 {
        val a = thetaDeg * PI / 180.0
        val xSign = if (mirrored) 1.0 else -1.0
        return Vec3(xSign * rangeM * sin(a), rangeM * cos(a), 0.0)
    }

    // --- the walk ----------------------------------------------------------
    //
    // ARCore's world frame is +Y up, +X right, camera looking along -Z. The
    // owner walks with the phone's BACK facing forward, so the camera looks
    // along the walk direction and the phone's orientation is IDENTITY: no
    // rotation is applied to get from "phone at session start" to "phone
    // walking forward". Walk direction is therefore world -Z.

    private val up = Vec3(0.0, 1.0, 0.0)
    private val forward = Vec3(0.0, 0.0, -1.0)

    /**
     * The operator's LEFT, **computed, never hard-coded**: in a right-handed
     * world frame, `up x forward` is the left hand. (Sanity: with up = +Y and
     * forward = -Z this is -X, i.e. the opposite of ARCore's camera +X "right"
     * — which is the point.) Deriving it here means a future edit to `forward`
     * or `up` cannot quietly turn this test into a tautology.
     */
    private val left = (up cross forward).normalized()

    private val phoneFromLidar = BracketNominals.cadNominal(SensorType.COIN_D6)

    /**
     * Unit bearing, in the ARCore world frame, from the LIDAR's own origin to
     * the return at [thetaDeg] — resolved exactly the way
     * `D6PushbroomAssembler::resolve_()` does it:
     *
     * ```
     * world_from_lidar = world_from_phone(t) * phone_from_lidar
     * p_world          = world_from_lidar * p_lidar(theta, d)
     * ```
     *
     * The bearing is measured from the lidar origin rather than the phone
     * origin so that the nominal's lever arm (6 cm up, 3.5 cm back — still a
     * CAD placeholder) cannot tilt the answer.
     */
    private fun bearing(thetaDeg: Double, atMetres: Double = 0.0, mirrored: Boolean = false): Vec3 {
        val position = Vec3(0.0, EYE_HEIGHT_M, -atMetres)
        val worldFromPhone = Mat4.fromRotationTranslation(Quat.IDENTITY, position)
        val worldFromLidar = worldFromPhone * phoneFromLidar
        val origin = worldFromLidar.transform(Vec3.ZERO)
        val pWorld = worldFromLidar.transform(fanPoint(thetaDeg, RANGE_M, mirrored))
        return (pWorld - origin).normalized()
    }

    private fun assertPointsAt(thetaDeg: Double, wanted: Vec3, name: String, mirrored: Boolean = false) {
        val b = bearing(thetaDeg, mirrored = mirrored)
        val c = b dot wanted.normalized()
        assertTrue(
            "theta=$thetaDeg should land $name (bearing $b vs $wanted, cos=$c)" +
                if (mirrored) " under the PRE-FIX formula" else "",
            c > ALIGNED,
        )
    }

    // --- the four cardinal returns -----------------------------------------

    @Test
    fun `theta 90 lands on the operator's LEFT`() {
        assertPointsAt(90.0, left, "on the operator's LEFT")
    }

    @Test
    fun `theta 270 lands on the operator's RIGHT`() {
        assertPointsAt(270.0, left * -1.0, "on the operator's RIGHT")
    }

    @Test
    fun `theta 0 lands UP, along the vendor's zero mark`() {
        // The 0-degree beam is the physical zero mark on the housing, and the
        // owner's mount points it at the ceiling.
        assertPointsAt(0.0, up, "UP")
    }

    @Test
    fun `theta 180 lands DOWN`() {
        assertPointsAt(180.0, up * -1.0, "DOWN")
    }

    // --- the falsifiable control -------------------------------------------

    @Test
    fun `the PRE-FIX formula puts theta 90 on the RIGHT, which is the bug`() {
        // The whole reason this file exists. If the old convention did NOT
        // reverse left and right, then nothing above proves anything and the
        // ROUND 9 fix was a no-op. Assert the failure explicitly.
        val mirroredNinety = bearing(90.0, mirrored = true)
        assertTrue(
            "the pre-ROUND-9 formula (x = +d*sin theta) was supposed to put theta=90 on the operator's " +
                "RIGHT — if it does not, this test proves nothing about the fix. Got $mirroredNinety",
            (mirroredNinety dot (left * -1.0)) > ALIGNED,
        )
        // ... and therefore it must FAIL the assertion the corrected formula passes.
        assertTrue(
            "the pre-fix bearing must NOT satisfy the corrected left-handed assertion",
            (mirroredNinety dot left) < -ALIGNED,
        )
    }

    @Test
    fun `the PRE-FIX formula is a pure reflection - it leaves UP and DOWN alone`() {
        // A reflection across the fan's y-z plane, not a rotation: the two
        // returns that lie ON the mirror plane (theta = 0 and 180) are
        // untouched, which is exactly why no extent-based or planarity-based
        // test could ever see it. Only the off-plane returns move.
        assertPointsAt(0.0, up, "UP", mirrored = true)
        assertPointsAt(180.0, up * -1.0, "DOWN", mirrored = true)

        val fixed = bearing(90.0)
        val old = bearing(90.0, mirrored = true)
        assertEquals("a reflection is exactly antipodal here", -1.0, fixed dot old, 1e-9)
    }

    @Test
    fun `the mirrored fan is sign-blind to every metric the older D6 tests measure`() {
        // Belt and braces on the paragraph in the KDoc: run a whole revolution
        // through both conventions and show the axis extents — the quantity
        // D6PushbroomGeometryTest asserts on — are IDENTICAL. This is the
        // documented reason the bug survived, asserted rather than asserted-by-
        // comment.
        val thetas = (0 until 360 step 5).map { it.toDouble() }
        fun extents(mirrored: Boolean): Triple<Double, Double, Double> {
            val pts = thetas.map { bearing(it, atMetres = 1.0, mirrored = mirrored) * RANGE_M }
            return Triple(
                pts.maxOf { it.x } - pts.minOf { it.x },
                pts.maxOf { it.y } - pts.minOf { it.y },
                pts.maxOf { it.z } - pts.minOf { it.z },
            )
        }
        val a = extents(false)
        val b = extents(true)
        assertEquals(a.first, b.first, 1e-9)
        assertEquals(a.second, b.second, 1e-9)
        assertEquals(a.third, b.third, 1e-9)
    }

    // --- the production matrix itself --------------------------------------

    @Test
    fun `the D6 CAD nominal carries an identity rotation and stays rigid`() {
        // Deliberately pinned. Under the corrected fan frame the owner's mount
        // makes lidar and camera axes agree one for one, so identity is the
        // DERIVED answer, not a placeholder. Anyone "fixing" this matrix to
        // chase a mirrored cloud is fixing the wrong thing — see d6_fan.h §4.
        assertTrue("the CAD nominal must be a proper rigid transform", phoneFromLidar.isRigid())
        val r = phoneFromLidar.rotation()
        val identity = doubleArrayOf(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
        for (i in r.indices) {
            assertTrue(
                "phone_from_lidar rotation[$i] should be ${identity[i]}, was ${r[i]}",
                abs(r[i] - identity[i]) < 1e-12,
            )
        }
        // A left-handed / mirrored extrinsic would have det = -1. It does not,
        // and that is the trap: `diag(-1, +1, -1)` — the exact transform that
        // turns the corrected fan into the buggy one — has det = +1 and sails
        // through this check. isRigid() is necessary and NOT sufficient.
        val mirroredNominal = Mat4(
            phoneFromLidar.m.copyOf().also { m ->
                for (row in 0 until 4) {
                    m[row * 4 + 0] = -m[row * 4 + 0]
                    m[row * 4 + 2] = -m[row * 4 + 2]
                }
            },
        )
        assertTrue(
            "the legacy-fan extrinsic diag(-1,+1,-1) is ALSO rigid — which is why no rigidity check " +
                "anywhere could have caught this bug",
            mirroredNominal.isRigid(),
        )
    }
}
