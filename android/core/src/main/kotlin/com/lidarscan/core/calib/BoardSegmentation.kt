package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import kotlin.math.abs
import kotlin.math.sqrt
import kotlin.random.Random

/** A plane `{p : normal·p = d}` expressed in the lidar's own sensor frame. */
data class LidarPlane(val normal: Vec3, val d: Double) {
    fun signedDistance(p: Vec3): Double = (normal dot p) - d
}

/**
 * Which lidar returns landed on the calibration board.
 *
 * `scan_mount_calib_add_observation` takes "the lidar returns segmented onto
 * the board, in the SENSOR frame" and weights them by `sigma_m`. Everything
 * in this file exists to produce that list without a human drawing a box
 * around the board.
 *
 * **The bootstrap, stated openly.** Segmentation needs to know roughly where
 * the board is in the *lidar's* frame, and the only thing that knows that
 * before the calibration exists is the calibration itself. So the wizard uses
 * the bracket's **CAD nominal** extrinsic to transform the camera-measured
 * plane into the lidar frame, gates returns to a generous band around that
 * prediction (default ±20 cm — several times the few-centimetre error the CAD
 * nominal is assumed to carry), and then re-fits the plane/line to the gated
 * returns by RANSAC so the *fit* owes nothing to the prediction beyond which
 * points it looked at. That is the standard bootstrap for this problem and it
 * is why S6's own procedure requires the board to stand "at least 0.5 m clear
 * of anything behind it" — the gate is what that clearance is for.
 *
 * It also means a badly wrong CAD nominal produces an empty or partial
 * segmentation, which surfaces as the wizard's "The lidar can't find the
 * board" check failing, not as a silently wrong calibration.
 */
data class BoardSegmentation(
    val points: List<Vec3>,
    /** RMS distance of the kept returns from the re-fitted model, metres. */
    val residualRmsM: Double,
    /** Returns considered before the fit (after the range + plane-band gate). */
    val gatedCount: Int,
) {
    val count: Int get() = points.size

    /** Flat `x,y,z` for the JNI call. */
    fun toFloatArray(): FloatArray {
        val out = FloatArray(points.size * 3)
        points.forEachIndexed { i, p ->
            out[3 * i] = p.x.toFloat()
            out[3 * i + 1] = p.y.toFloat()
            out[3 * i + 2] = p.z.toFloat()
        }
        return out
    }
}

data class SegmentationConfig(
    val rangeMinM: Double = 0.5,
    val rangeMaxM: Double = 3.5,
    /** Half-width of the gate around the CAD-predicted board plane, metres. */
    val planeBandM: Double = 0.20,
    /** RANSAC inlier threshold, metres. Set from the sensor's own range noise. */
    val inlierM: Double = 0.03,
    val iterations: Int = 240,
    val minInliers: Int = 20,
)

/**
 * Per-sensor defaults, straight from S6's tables.
 *
 * `sigmaM` is the 1-sigma range noise the solver whitens each residual with.
 * The D6 number is the **datasheet bound (30 mm)**, not a measurement:
 * S6 §5 open question 1 says "the whole D6 verdict pivots on it: at 30 mm the
 * wizard cannot close the budget, at 10 mm it can", and S1 has not yet
 * reported a measured sigma. Using the pessimistic bound means the solver
 * down-weights D6 observations correctly relative to a Mid-360's; it does not
 * make the D6 wizard sufficient, which is exactly S6's finding.
 *
 * `minReturnsPerPose` is WIZARD.md screen 2's "Lidar sees it" chip: >= 20
 * (D6) / >= 150 (Mid-360).
 */
enum class LidarProfile(
    val sigmaM: Double,
    val minReturnsPerPose: Int,
    val scanIs2d: Boolean,
    val recommendedPoses: Int,
) {
    /** COIN-D6: a 2-D scanner. Its plane returns a LINE, not a patch — see [BoardSegmenter]. */
    D6(sigmaM = 0.030, minReturnsPerPose = 20, scanIs2d = true, recommendedPoses = 12),

    /** Livox Mid-360: a 3-D scanner; 8 poses are enough (S6 §1). */
    MID360(sigmaM = 0.020, minReturnsPerPose = 150, scanIs2d = false, recommendedPoses = 8),
    ;

    companion object {
        fun of(sensor: SensorType): LidarProfile = when (sensor) {
            SensorType.COIN_D6 -> D6
            SensorType.MID360 -> MID360
        }
    }
}

object BoardSegmenter {

    /**
     * Transforms a camera-frame target plane into the lidar frame using
     * `phone_from_lidar` (== `camera_from_lidar`; A8 §3.1: the phone frame
     * and the camera frame are one frame).
     *
     * `n_c·(R·p_l + t) = d_c`  =>  `(Rᵀ·n_c)·p_l = d_c − n_c·t`
     */
    fun predictInLidarFrame(camera: TargetPlaneObservation, phoneFromLidar: Mat4): LidarPlane {
        val r = phoneFromLidar.rotation()
        val n = camera.normal
        val nl = Vec3(
            r[0] * n.x + r[3] * n.y + r[6] * n.z,
            r[1] * n.x + r[4] * n.y + r[7] * n.z,
            r[2] * n.x + r[5] * n.y + r[8] * n.z,
        )
        val t = phoneFromLidar.translation
        return LidarPlane(nl, camera.d - (n dot t))
    }

    /**
     * Gates and re-fits. [profile] decides which model is fitted:
     *
     *  * **Mid-360 (3-D)** — a plane by RANSAC over point triples.
     *  * **D6 (2-D)** — a LINE by RANSAC over point pairs, in the scanner's
     *    own z = 0 scan plane. Fitting a plane to a 2-D scanner's returns is
     *    rank-deficient by construction (every return has z = 0, so the normal
     *    is unconstrained out of plane) and would "succeed" while meaning
     *    nothing. This is the same geometric fact behind S6's verdict that a
     *    2-D scanner supplies 2 constraints per pose instead of 3.
     */
    fun segment(
        sensorFramePoints: List<Vec3>,
        predicted: LidarPlane,
        profile: LidarProfile,
        config: SegmentationConfig = SegmentationConfig(inlierM = profile.sigmaM),
        random: Random = Random(0x5EED),
    ): BoardSegmentation {
        val gated = sensorFramePoints.filter { p ->
            val r = p.norm
            r in config.rangeMinM..config.rangeMaxM &&
                abs(predicted.signedDistance(p)) <= config.planeBandM
        }
        if (gated.size < config.minInliers) {
            return BoardSegmentation(emptyList(), Double.NaN, gated.size)
        }
        return if (profile.scanIs2d) {
            fitLine(gated, config, random)
        } else {
            fitPlane(gated, config, random)
        }
    }

    private fun fitPlane(points: List<Vec3>, config: SegmentationConfig, random: Random): BoardSegmentation {
        var best: List<Vec3> = emptyList()
        repeat(config.iterations) {
            val a = points[random.nextInt(points.size)]
            val b = points[random.nextInt(points.size)]
            val c = points[random.nextInt(points.size)]
            val n = ((b - a) cross (c - a))
            if (n.norm < 1e-9) return@repeat
            val nn = n.normalized()
            val d = nn dot a
            val inliers = points.filter { abs((nn dot it) - d) <= config.inlierM }
            if (inliers.size > best.size) best = inliers
        }
        if (best.size < config.minInliers) return BoardSegmentation(emptyList(), Double.NaN, points.size)

        // Least-squares refit over the inliers: the eigenvector of the
        // scatter matrix with the smallest eigenvalue is the plane normal.
        val centroid = best.fold(Vec3.ZERO) { acc, p -> acc + p } * (1.0 / best.size)
        val m = DoubleArray(9)
        for (p in best) {
            val q = p - centroid
            val v = doubleArrayOf(q.x, q.y, q.z)
            for (i in 0 until 3) {
                for (j in 0 until 3) m[i * 3 + j] += v[i] * v[j]
            }
        }
        val (_, vectors) = LinAlg.symmetricEigen(m, 3)
        val n = Vec3(vectors[0], vectors[3], vectors[6]).normalized()
        val d = n dot centroid
        val rms = sqrt(best.sumOf { val e = (n dot it) - d; e * e } / best.size)
        return BoardSegmentation(best, rms, points.size)
    }

    private fun fitLine(points: List<Vec3>, config: SegmentationConfig, random: Random): BoardSegmentation {
        var best: List<Vec3> = emptyList()
        repeat(config.iterations) {
            val a = points[random.nextInt(points.size)]
            val b = points[random.nextInt(points.size)]
            val dx = b.x - a.x
            val dy = b.y - a.y
            val len = sqrt(dx * dx + dy * dy)
            if (len < 1e-6) return@repeat
            // Line normal in the scan plane.
            val nx = -dy / len
            val ny = dx / len
            val d = nx * a.x + ny * a.y
            val inliers = points.filter { abs(nx * it.x + ny * it.y - d) <= config.inlierM }
            if (inliers.size > best.size) best = inliers
        }
        if (best.size < config.minInliers) return BoardSegmentation(emptyList(), Double.NaN, points.size)

        // Total-least-squares refit of the 2-D line over the inliers.
        val mx = best.sumOf { it.x } / best.size
        val my = best.sumOf { it.y } / best.size
        var sxx = 0.0
        var syy = 0.0
        var sxy = 0.0
        for (p in best) {
            val ex = p.x - mx
            val ey = p.y - my
            sxx += ex * ex; syy += ey * ey; sxy += ex * ey
        }
        val (_, vectors) = LinAlg.symmetricEigen(doubleArrayOf(sxx, sxy, sxy, syy), 2)
        val nx = vectors[0]
        val ny = vectors[2]
        val d = nx * mx + ny * my
        val rms = sqrt(best.sumOf { val e = nx * it.x + ny * it.y - d; e * e } / best.size)
        return BoardSegmentation(best, rms, points.size)
    }
}
