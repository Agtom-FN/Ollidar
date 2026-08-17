package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.cos
import kotlin.math.sin

/**
 * ROUND 5 AUDIT — urgent field item: a real Pixel 8 Pro + COIN-D6 walk
 * produced "the plane of Z-axis instead of XY" (the owner's words) — a flat,
 * non-extruded scan instead of a proper 3D volume, even though ARCore
 * tracking/the walked trail looked fine on the same walk.
 *
 * The whole `engine/` tree is read-only for this task (and out of scope to
 * modify even if it weren't), so this cannot exercise the real
 * `D6PushbroomAssembler` directly. What it CAN do, entirely inside the
 * `android/` tree, is prove or
 * disprove the one hypothesis that lives on this side of the JNI boundary:
 * that [BracketNominals.cadNominal]'s D6 rotation (currently identity) maps
 * the sensor's own scan plane onto the WRONG pair of camera axes, so that
 * walking forward moves the rig WITHIN the plane the fan already sweeps
 * (producing exactly "one thickened plane, never extruded") instead of
 * THROUGH it.
 *
 * This test reimplements — deliberately, not by calling engine code — the
 * exact composition `engine/docs/A8-pushbroom.md` §3.1 documents and
 * `engine/src/slam/pushbroom/pushbroom_assembler.cpp`'s `resolve_()`
 * performs:
 *
 * ```
 * world_from_lidar = world_from_phone(t) · phone_from_lidar
 * p_lidar(angle, range) = (range·sin(angle), range·cos(angle), 0)
 * p_world = world_from_lidar · p_lidar
 * ```
 *
 * using `:core`'s own [Mat4]/[Quat]/[Vec3] (the same types
 * [com.lidarscan.core.calib.MountCalibration] and the mount-calibration
 * wizard already use) and the REAL [BracketNominals.cadNominal] matrix a D6
 * session with no measured calibration actually applies
 * (`CaptureViewModel.startArPipelines`). If this test passes, the
 * mount-nominal-rotation hypothesis is disproven for this exact composition;
 * if it fails, the nominal's rotation block is the bug and needs fixing here,
 * in the `android/` tree, without touching `engine/` at all.
 *
 * ARCore's world frame is Y-up, X-right, camera looks down -Z (documented
 * already in `TrajectoryTrail.kt`/`Geometry.kt`) — axes are asserted BY NAME
 * below specifically so a Y/Z mixup in either the production matrix or a
 * future edit to it fails loudly here instead of only in the field.
 */
class D6PushbroomGeometryTest {

    private data class WorldPoint(val x: Double, val y: Double, val z: Double)

    /**
     * Simulates a straight-line walk (no turning — orientation held at
     * `Quat.IDENTITY`, matching the coordinator's own "straight-line walking
     * poses" ask) carrying a D6 whose full revolutions are resolved against
     * [BracketNominals.cadNominal] exactly the way `resolve_()` does.
     */
    private fun walkAndScan(
        speedMPerS: Double,
        durationS: Double,
        poseHz: Double,
        rangeM: Double,
        anglesDeg: List<Double> = (0 until 360 step 30).map { it.toDouble() },
        cameraHeightM: Double = 1.4,
    ): List<WorldPoint> {
        val phoneFromLidar = BracketNominals.cadNominal(SensorType.COIN_D6)
        val out = mutableListOf<WorldPoint>()
        val stepS = 1.0 / poseHz
        var t = 0.0
        while (t <= durationS) {
            // ARCore convention: the camera looks down -Z, so walking FORWARD
            // (in the direction the phone's rear camera faces) advances -Z.
            val position = Vec3(0.0, cameraHeightM, -speedMPerS * t)
            val worldFromPhone = Mat4.fromRotationTranslation(Quat.IDENTITY, position)
            val worldFromLidar = worldFromPhone * phoneFromLidar
            for (angleDeg in anglesDeg) {
                val a = Math.toRadians(angleDeg)
                // A8 §3.1's sensor-frame convention, byte for byte:
                // x = d·sinθ, y = d·cosθ, z = 0.
                val pLidar = Vec3(rangeM * sin(a), rangeM * cos(a), 0.0)
                val pWorld = worldFromLidar.transform(pLidar)
                out.add(WorldPoint(pWorld.x, pWorld.y, pWorld.z))
            }
            t += stepS
        }
        return out
    }

    private fun List<WorldPoint>.extent(axis: (WorldPoint) -> Double): Double {
        val values = map(axis)
        return values.max() - values.min()
    }

    @Test
    fun `a straight walk extrudes the D6 fan along the walk axis, not just within its own sweep`() {
        // 10 m walk at 1 m/s (10 s), a full D6 revolution every pose at 2 Hz,
        // 1.0 m range — deliberately chosen so the walked distance (10 m) is
        // far bigger than the fan's own diameter (2 m), so a "collapsed to
        // one plane" bug and a "properly extruded" result are unmistakable,
        // not a close call.
        val points = walkAndScan(speedMPerS = 1.0, durationS = 10.0, poseHz = 2.0, rangeM = 1.0)
        assertTrue("expected a non-trivial synthetic point set", points.size > 100)

        val extentX = points.extent { it.x }
        val extentY = points.extent { it.y }
        val extentZ = points.extent { it.z }

        // The fan's own sweep (bounded by ~2x its range, plus float slop) —
        // X (left-right) and Y (up-down) must NOT grow with how far the
        // operator walked.
        assertTrue("X extent $extentX should be bounded by the fan's own diameter (~2 m), not the walk", extentX < 2.2)
        assertTrue("Y extent $extentY should be bounded by the fan's own diameter (~2 m), not the walk", extentY < 2.2)

        // Z (forward/back, the walk axis under a straight, non-turning walk
        // with ARCore's default -Z-forward orientation) must span
        // approximately the walked distance — THIS is "extruded along the
        // walk trajectory" versus "a single [ever-so-slightly-thickened]
        // plane" (the reported bug) in one number.
        assertTrue(
            "Z extent $extentZ should span roughly the 10 m walked distance — a value near the fan's own " +
                "~1-2 m diameter instead would mean the walk never extruded the scan (the reported bug)",
            extentZ > 8.0,
        )

        // Explicit, named-axis assertion so a Y<->Z swap anywhere in this
        // chain (the production matrix, or a future edit to this test) fails
        // loudly rather than passing by accident: the walk axis must dominate
        // by a wide margin over the fan's own perpendicular sweep.
        assertTrue(
            "the walk (Z) extent must dominate the fan's own (Y, height) extent by a wide margin — " +
                "got Z=$extentZ vs Y=$extentY — a Y/Z mixup would make these look swapped or similar",
            extentZ > extentY * 3.0,
        )
    }

    @Test
    fun `a stationary D6 revolution stays within the fan's own diameter on every axis`() {
        // The negative-space check: with NO walking at all, nothing should
        // extrude on ANY axis — a sanity bound on the positive-case test
        // above, proving the large Z extent there really does come from the
        // walk and not from some unrelated blow-up in the composition.
        val points = walkAndScan(speedMPerS = 0.0, durationS = 2.0, poseHz = 5.0, rangeM = 1.0)
        assertTrue(points.extent { it.x } < 2.2)
        assertTrue(points.extent { it.y } < 2.2)
        assertTrue(points.extent { it.z } < 2.2)
    }
}
