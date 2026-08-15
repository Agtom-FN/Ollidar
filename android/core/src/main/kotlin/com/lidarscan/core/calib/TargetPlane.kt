package com.lidarscan.core.calib

import kotlin.math.abs
import kotlin.math.sqrt

/**
 * Pinhole intrinsics as ARCore hands them out (`CameraIntrinsics
 * .getFocalLength()/.getPrincipalPoint()/.getImageDimensions()`), plus the two
 * fields ARCore does *not* supply and B8 fills in from `ImageMetadata` — see
 * `ArCameraIntrinsics.kt` on the app side.
 *
 * ARCore's intrinsics describe an image it has already rectified, so
 * [distortion] is all-zero unless a caller pulled
 * `ImageMetadata.LENS_RADIAL_DISTORTION` — which is the *raw sensor's*
 * distortion and therefore must NOT be attached to ARCore's rectified
 * intrinsics. That is why the wizard's plane estimate below uses the pinhole
 * model alone.
 */
data class PinholeIntrinsics(
    val fx: Double,
    val fy: Double,
    val cx: Double,
    val cy: Double,
    val width: Int,
    val height: Int,
    val distortion: DoubleArray = DoubleArray(5),
) {
    /** Back-projects a pixel to a unit ray in the camera frame (+x right, +y down, +z forward — the OpenCV/PnP convention this file works in). */
    fun rayThrough(u: Double, v: Double): Vec3 =
        Vec3((u - cx) / fx, (v - cy) / fy, 1.0).normalized()

    override fun equals(other: Any?): Boolean =
        other is PinholeIntrinsics && fx == other.fx && fy == other.fy && cx == other.cx &&
            cy == other.cy && width == other.width && height == other.height &&
            distortion.contentEquals(other.distortion)

    override fun hashCode(): Int =
        (((fx.hashCode() * 31 + fy.hashCode()) * 31 + cx.hashCode()) * 31 + cy.hashCode()) * 31 +
            distortion.contentHashCode()
}

/**
 * One wizard pose's camera-side measurement: the target plane **as the camera
 * measured it, in the camera frame** — which is literally all
 * `scan_mount_calib_add_observation` takes from the camera (A8 §4.1: "The
 * camera side is a plane rather than a pose deliberately: it is all the
 * residual needs").
 *
 * `normal` is unit and `d` is positive, both required by the C ABI. The full
 * board pose is kept alongside because the live checks need it (incidence
 * angle, distance, and the roll the wizard prompts for).
 */
data class TargetPlaneObservation(
    val normal: Vec3,
    val d: Double,
    /** `camera_from_board` — board origin and axes expressed in the camera frame. */
    val cameraFromBoard: Mat4,
    val reprojectionRmsPx: Double,
    /** Angle between the board normal and the camera's view direction to the board centre, degrees. */
    val incidenceDeg: Double,
    /** Distance from the camera centre to the board centre, metres. */
    val distanceM: Double,
) {
    val boardCentreCamera: Vec3 get() = cameraFromBoard.translation
}

/**
 * Recovers the target plane from a checkerboard detection.
 *
 * **Homography, not full PnP.** The target is planar, so `H = K·[r1 r2 t]`
 * carries everything: `r3 = r1 × r2` is the board normal in the camera frame
 * and `d = r3 · t` its distance. Going through a general PnP would add an
 * iterative solver and a pile of failure modes for an answer this problem
 * gives in closed form. The residual A8's solver minimises is
 * `(n·(R·p + t) − d)/σ`, so the *plane*, not the pose, is the quantity whose
 * accuracy matters.
 *
 * Pipeline: Hartley normalisation -> DLT (null vector of `AᵀA`, via
 * [LinAlg.symmetricEigen]) -> denormalise -> decompose against K ->
 * re-orthonormalise the rotation -> reprojection RMS as a quality read.
 */
object TargetPlaneEstimator {

    /** Returns null when the correspondences are degenerate (collinear, or too few). */
    fun estimate(
        detection: CheckerboardDetection,
        intrinsics: PinholeIntrinsics,
    ): TargetPlaneObservation? {
        val objectPoints = detection.spec.objectPoints()
        val imagePoints = detection.corners
        if (objectPoints.size != imagePoints.size || objectPoints.size < 4) return null

        val h = homography(objectPoints, imagePoints) ?: return null

        // K^-1 · H, column by column. K is upper-triangular with no skew, so
        // its inverse is written out rather than solved for.
        val invFx = 1.0 / intrinsics.fx
        val invFy = 1.0 / intrinsics.fy
        fun unproject(c0: Double, c1: Double, c2: Double) =
            Vec3((c0 - intrinsics.cx * c2) * invFx, (c1 - intrinsics.cy * c2) * invFy, c2)

        val h1 = unproject(h[0], h[3], h[6])
        val h2 = unproject(h[1], h[4], h[7])
        val h3 = unproject(h[2], h[5], h[8])

        val n1 = h1.norm
        val n2 = h2.norm
        if (n1 <= 1e-12 || n2 <= 1e-12) return null
        // One scale for both columns (their true norms are equal; averaging
        // the two estimates is the standard, and slightly better-conditioned,
        // choice).
        var lambda = 2.0 / (n1 + n2)

        var r1 = h1 * lambda
        var r2 = h2 * lambda
        var t = h3 * lambda
        // The board must be IN FRONT of the camera. A homography is only
        // defined up to sign, and the wrong sign puts the board behind the
        // lens with an inverted normal — which would still "solve", silently.
        if (t.z < 0.0) {
            r1 = r1 * -1.0
            r2 = r2 * -1.0
            t = t * -1.0
            lambda = -lambda
        }

        // Re-orthonormalise: DLT noise leaves r1, r2 slightly non-orthogonal.
        // Symmetric Gram-Schmidt (rotate both toward each other by half the
        // error) rather than the asymmetric version, so neither column is
        // privileged.
        val err = r1 dot r2
        val a = (r1 - r2 * (err / 2.0)).normalized()
        val b = (r2 - r1 * (err / 2.0)).normalized()
        val r3 = (a cross b).normalized()

        val cameraFromBoard = Mat4(
            doubleArrayOf(
                a.x, b.x, r3.x, t.x,
                a.y, b.y, r3.y, t.y,
                a.z, b.z, r3.z, t.z,
                0.0, 0.0, 0.0, 1.0,
            ),
        )

        // Plane {p : n·p = d} through the board origin with the board normal.
        // Flipping n flips d, so requiring d > 0 (the C ABI's contract) is one
        // conditional, not a special case.
        var normal = r3
        var d = normal dot t
        if (d < 0.0) {
            normal = normal * -1.0
            d = -d
        }
        if (d <= 0.0 || !d.isFinite()) return null

        val rms = reprojectionRms(objectPoints, imagePoints, cameraFromBoard, intrinsics)

        val spec = detection.spec
        val centreBoard = Vec3(
            (spec.cols - 1) * spec.squareSizeM / 2.0,
            (spec.rows - 1) * spec.squareSizeM / 2.0,
            0.0,
        )
        val centreCamera = cameraFromBoard.transform(centreBoard)
        val viewDir = centreCamera.normalized()
        // Incidence: 0° means looking straight down the normal. The board's
        // normal may point either way, so the acute angle is what is meant.
        val incidence = Math.toDegrees(angleBetween(viewDir, normal)).let { if (it > 90.0) 180.0 - it else it }

        return TargetPlaneObservation(
            normal = normal,
            d = d,
            cameraFromBoard = cameraFromBoard,
            reprojectionRmsPx = rms,
            incidenceDeg = incidence,
            distanceM = centreCamera.norm,
        )
    }

    /** Normalised DLT. Returns a row-major 3x3, scaled so `h[8] == 1` when possible. */
    internal fun homography(objectPoints: List<Vec3>, imagePoints: List<Corner>): DoubleArray? {
        val n = objectPoints.size
        // Degenerate input check, BEFORE the fit rather than after. Collinear
        // image points admit a whole family of homographies that reproject
        // them perfectly, so no residual-based test can catch this — only the
        // spread of the inputs can. The ratio is the smaller-to-larger
        // eigenvalue of the point scatter; even a 62°-incidence view of an
        // 8x6 board stays four orders of magnitude above this floor.
        if (spreadRatio(imagePoints.map { it.x to it.y }) < 1e-4) return null
        if (spreadRatio(objectPoints.map { it.x to it.y }) < 1e-4) return null
        val (objT, objN) = normalize(objectPoints.map { it.x to it.y })
        val (imgT, imgN) = normalize(imagePoints.map { it.x to it.y })

        // A is 2n x 9; accumulate AᵀA directly (9x9) rather than materialising A.
        val ata = DoubleArray(81)
        val row = DoubleArray(9)
        fun accumulate() {
            for (i in 0 until 9) {
                for (j in 0 until 9) ata[i * 9 + j] += row[i] * row[j]
            }
        }
        for (k in 0 until n) {
            val (x, y) = objN[k]
            val (u, v) = imgN[k]
            row[0] = x; row[1] = y; row[2] = 1.0
            row[3] = 0.0; row[4] = 0.0; row[5] = 0.0
            row[6] = -u * x; row[7] = -u * y; row[8] = -u
            accumulate()
            row[0] = 0.0; row[1] = 0.0; row[2] = 0.0
            row[3] = x; row[4] = y; row[5] = 1.0
            row[6] = -v * x; row[7] = -v * y; row[8] = -v
            accumulate()
        }

        val (values, vectors) = LinAlg.symmetricEigen(ata, 9)
        // Degenerate configurations show up as a second near-zero eigenvalue
        // (the null space is not one-dimensional) — reject rather than return
        // an arbitrary vector from a two-dimensional null space.
        if (values[1] <= 1e-12 * (values[8] + 1e-30)) return null
        val hn = DoubleArray(9) { vectors[it * 9] }

        // Denormalise: H = T_img^-1 · Hn · T_obj
        val h = mul3(mul3(invertSimilarity(imgT), hn), objT)
        val scale = h[8]
        if (abs(scale) > 1e-12) {
            for (i in 0 until 9) h[i] /= scale
        }
        return if (h.all { it.isFinite() }) h else null
    }

    /** Smaller/larger eigenvalue of a 2-D point set's scatter matrix: 0 for a perfectly collinear set, 1 for an isotropic one. */
    private fun spreadRatio(points: List<Pair<Double, Double>>): Double {
        if (points.size < 3) return 0.0
        val mx = points.sumOf { it.first } / points.size
        val my = points.sumOf { it.second } / points.size
        var sxx = 0.0
        var syy = 0.0
        var sxy = 0.0
        for ((x, y) in points) {
            val ex = x - mx
            val ey = y - my
            sxx += ex * ex; syy += ey * ey; sxy += ex * ey
        }
        val (values, _) = LinAlg.symmetricEigen(doubleArrayOf(sxx, sxy, sxy, syy), 2)
        val large = values[1]
        return if (large <= 0.0) 0.0 else values[0] / large
    }

    /** Hartley normalisation: centroid to the origin, mean distance to sqrt(2). Returns the 3x3 transform and the transformed points. */
    private fun normalize(points: List<Pair<Double, Double>>): Pair<DoubleArray, List<Pair<Double, Double>>> {
        val n = points.size
        val mx = points.sumOf { it.first } / n
        val my = points.sumOf { it.second } / n
        val meanDist = points.sumOf { sqrt((it.first - mx) * (it.first - mx) + (it.second - my) * (it.second - my)) } / n
        val s = if (meanDist > 1e-12) sqrt(2.0) / meanDist else 1.0
        val t = doubleArrayOf(s, 0.0, -s * mx, 0.0, s, -s * my, 0.0, 0.0, 1.0)
        val out = points.map { (x, y) -> (s * (x - mx)) to (s * (y - my)) }
        return t to out
    }

    /** Inverse of a `[[s,0,tx],[0,s,ty],[0,0,1]]` normalisation transform. */
    private fun invertSimilarity(t: DoubleArray): DoubleArray {
        val s = t[0]
        val tx = t[2]
        val ty = t[5]
        val inv = 1.0 / s
        return doubleArrayOf(inv, 0.0, -tx * inv, 0.0, inv, -ty * inv, 0.0, 0.0, 1.0)
    }

    private fun mul3(a: DoubleArray, b: DoubleArray): DoubleArray {
        val r = DoubleArray(9)
        for (i in 0 until 3) {
            for (j in 0 until 3) {
                var s = 0.0
                for (k in 0 until 3) s += a[i * 3 + k] * b[k * 3 + j]
                r[i * 3 + j] = s
            }
        }
        return r
    }

    private fun reprojectionRms(
        objectPoints: List<Vec3>,
        imagePoints: List<Corner>,
        cameraFromBoard: Mat4,
        intrinsics: PinholeIntrinsics,
    ): Double {
        var sum = 0.0
        for (i in objectPoints.indices) {
            val p = cameraFromBoard.transform(objectPoints[i])
            if (p.z <= 1e-9) return Double.POSITIVE_INFINITY
            val u = intrinsics.fx * p.x / p.z + intrinsics.cx
            val v = intrinsics.fy * p.y / p.z + intrinsics.cy
            val du = u - imagePoints[i].x
            val dv = v - imagePoints[i].y
            sum += du * du + dv * dv
        }
        return sqrt(sum / objectPoints.size)
    }
}
