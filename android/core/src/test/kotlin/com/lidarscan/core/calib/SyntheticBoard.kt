package com.lidarscan.core.calib

import kotlin.math.roundToInt
import kotlin.random.Random

/**
 * Renders a checkerboard the way a camera would see it, so the detector and
 * the plane estimator can be tested against a pose that is known exactly
 * rather than assumed.
 *
 * The renderer is a ray-caster, not a rasteriser: for each pixel it
 * back-projects through the pinhole, intersects the board plane and reads the
 * square colour at the hit point. That makes the ground truth the *pose*, not
 * a drawn quad — so a detector error and a projection error cannot cancel.
 *
 * 2x2 supersampling, then optional Gaussian blur and additive noise, because
 * a detector that only works on hard-edged synthetic pixels would tell us
 * nothing about a real camera image (it would still tell us nothing about a
 * *real* one either — see CheckerboardDetector's own doc comment on the
 * limits of this envelope).
 */
object SyntheticBoard {

    /**
     * The quiet zone around the pattern is WHITE, not grey — that is what an
     * A1 print on paper actually looks like, and it matters: with a grey
     * background every corner of the outer ring of squares is also a saddle,
     * so a detector tuned against a grey-bordered synthetic board would be
     * tuned against a target that does not exist.
     */
    const val BACKGROUND = 235
    const val DARK = 30
    const val LIGHT = 225

    fun render(
        spec: CheckerboardSpec,
        intrinsics: PinholeIntrinsics,
        cameraFromBoard: Mat4,
        blurSigma: Double = 0.8,
        noiseSigma: Double = 2.0,
        seed: Int = 7,
    ): LumaImage {
        val boardFromCamera = cameraFromBoard.inverseRigid()
        val r = cameraFromBoard.rotation()
        val normal = Vec3(r[2], r[5], r[8])          // board +z in camera frame
        val origin = cameraFromBoard.translation      // board origin in camera frame
        val planeD = normal dot origin

        val w = intrinsics.width
        val h = intrinsics.height
        val acc = DoubleArray(w * h)

        val s = spec.squareSizeM
        val minX = -s
        val maxX = spec.cols * s
        val minY = -s
        val maxY = spec.rows * s

        for (y in 0 until h) {
            for (x in 0 until w) {
                var sum = 0.0
                for (sy in 0 until 2) {
                    for (sx in 0 until 2) {
                        // Pixel (x, y)'s CENTRE is at the integer coordinate
                        // (x, y) — the same convention the pinhole projection
                        // and the detector's own integer peak positions use.
                        // Sampling at x+0.25/x+0.75 instead would offset every
                        // rendered corner by half a pixel and quietly put a
                        // 0.7 px floor under every accuracy number here.
                        val u = x - 0.25 + sx * 0.5
                        val v = y - 0.25 + sy * 0.5
                        val ray = Vec3(
                            (u - intrinsics.cx) / intrinsics.fx,
                            (v - intrinsics.cy) / intrinsics.fy,
                            1.0,
                        )
                        val denom = normal dot ray
                        if (kotlin.math.abs(denom) < 1e-12) {
                            sum += BACKGROUND
                            continue
                        }
                        val t = planeD / denom
                        if (t <= 0) {
                            sum += BACKGROUND
                            continue
                        }
                        val p = ray * t
                        val q = boardFromCamera.transform(p)
                        sum += if (q.x < minX || q.x > maxX || q.y < minY || q.y > maxY) {
                            BACKGROUND.toDouble()
                        } else {
                            val ci = Math.floorDiv((q.x / s * 1e6).roundToInt(), 1_000_000)
                            val cj = Math.floorDiv((q.y / s * 1e6).roundToInt(), 1_000_000)
                            if ((ci + cj) and 1 == 0) LIGHT.toDouble() else DARK.toDouble()
                        }
                    }
                }
                acc[y * w + x] = sum / 4.0
            }
        }

        val blurred = if (blurSigma > 0) blur(acc, w, h, blurSigma) else acc
        val rng = Random(seed)
        val bytes = ByteArray(w * h)
        for (i in blurred.indices) {
            val n = if (noiseSigma > 0) gaussian(rng) * noiseSigma else 0.0
            bytes[i] = (blurred[i] + n).coerceIn(0.0, 255.0).roundToInt().toByte()
        }
        return LumaImage(w, h, bytes)
    }

    /** Ground-truth image positions of the inner corners, same order as [CheckerboardSpec.objectPoints]. */
    fun projectCorners(
        spec: CheckerboardSpec,
        intrinsics: PinholeIntrinsics,
        cameraFromBoard: Mat4,
    ): List<Corner> = spec.objectPoints().map { o ->
        val p = cameraFromBoard.transform(o)
        Corner(
            intrinsics.fx * p.x / p.z + intrinsics.cx,
            intrinsics.fy * p.y / p.z + intrinsics.cy,
            1.0,
        )
    }

    /**
     * A camera pose looking at the board's centre from a given
     * azimuth/elevation/roll and distance — the wizard's own pose
     * parameterisation, so a test can ask for exactly the poses
     * [PosePlan] prescribes.
     */
    fun poseFor(
        spec: CheckerboardSpec,
        azimuthDeg: Double,
        elevationDeg: Double,
        rollDeg: Double,
        distanceM: Double,
    ): Mat4 {
        val centre = Vec3((spec.cols - 1) * spec.squareSizeM / 2.0, (spec.rows - 1) * spec.squareSizeM / 2.0, 0.0)
        val az = Math.toRadians(azimuthDeg)
        val el = Math.toRadians(elevationDeg)
        // Camera position in the board frame, on a sphere around the centre.
        // NOTE THE SIGN: the object-point frame is (+x along a row, +y down a
        // column), so its +z — being x cross y — points INTO the board, away
        // from the viewer. The camera therefore sits at NEGATIVE z. This is
        // the same convention OpenCV's chessboard object points imply, and
        // getting it backwards renders the board mirrored, which is a
        // wonderfully confusing way to "fail" (every corner is found, the
        // reprojection is perfect, and the recovered normal points through
        // the wall).
        val eye = centre + Vec3(
            distanceM * Math.cos(el) * Math.sin(az),
            -distanceM * Math.sin(el),
            -distanceM * Math.cos(el) * Math.cos(az),
        )
        // Camera axes in the board frame, OpenCV convention (+x right, +y
        // down, +z forward toward the board).
        val forward = (centre - eye).normalized()
        val worldDown = Vec3(0.0, 1.0, 0.0)
        var right = (worldDown cross forward).normalized()
        var down = (forward cross right).normalized()
        val roll = Math.toRadians(rollDeg)
        val cr = Math.cos(roll)
        val sr = Math.sin(roll)
        val rolledRight = (right * cr + down * sr).normalized()
        val rolledDown = (down * cr - right * sr).normalized()
        right = rolledRight
        down = rolledDown

        // boardFromCamera has the camera axes as its columns; the renderer
        // wants cameraFromBoard, so invert.
        val boardFromCamera = Mat4(
            doubleArrayOf(
                right.x, down.x, forward.x, eye.x,
                right.y, down.y, forward.y, eye.y,
                right.z, down.z, forward.z, eye.z,
                0.0, 0.0, 0.0, 1.0,
            ),
        )
        return boardFromCamera.inverseRigid()
    }

    private fun blur(src: DoubleArray, w: Int, h: Int, sigma: Double): DoubleArray {
        val radius = kotlin.math.max(1, (sigma * 3).roundToInt())
        val k = DoubleArray(2 * radius + 1)
        var sum = 0.0
        for (i in k.indices) {
            val d = (i - radius).toDouble()
            k[i] = kotlin.math.exp(-(d * d) / (2 * sigma * sigma))
            sum += k[i]
        }
        for (i in k.indices) k[i] /= sum
        val tmp = DoubleArray(w * h)
        for (y in 0 until h) {
            for (x in 0 until w) {
                var a = 0.0
                for (i in k.indices) a += k[i] * src[y * w + (x + i - radius).coerceIn(0, w - 1)]
                tmp[y * w + x] = a
            }
        }
        val out = DoubleArray(w * h)
        for (y in 0 until h) {
            for (x in 0 until w) {
                var a = 0.0
                for (i in k.indices) a += k[i] * tmp[(y + i - radius).coerceIn(0, h - 1) * w + x]
                out[y * w + x] = a
            }
        }
        return out
    }

    private fun gaussian(rng: Random): Double {
        val u1 = rng.nextDouble().coerceAtLeast(1e-12)
        val u2 = rng.nextDouble()
        return kotlin.math.sqrt(-2.0 * kotlin.math.ln(u1)) * kotlin.math.cos(2.0 * Math.PI * u2)
    }
}
