package com.lidarscan.core.calib

import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * The small linear-algebra vocabulary B7/B8 need, in `:core` so it is
 * plain-JVM testable. Deliberately minimal — there is no matrix library
 * dependency here and none is wanted: everything below is a 3-vector, a unit
 * quaternion or a 4x4 rigid transform, and the engine's own C ABI takes
 * exactly those.
 *
 * **Row-major, everywhere.** `scan_engine_set_mount_extrinsics` and
 * `scan_mount_calib_solve` both take a ROW-major `double[16]` and the engine
 * *rejects* a column-major one (`SCAN_ERR_INVALID_ARGUMENT`) rather than
 * produce a mirrored cloud — see `scanengine_c.h`'s note on the D6 pushbroom.
 * Android's own `android.opengl.Matrix` and ARCore's `Pose.toMatrix()` are
 * COLUMN-major, so [Mat4.fromColumnMajor] is the one conversion point and it
 * is named for what it does.
 */
data class Vec3(val x: Double, val y: Double, val z: Double) {
    operator fun plus(o: Vec3) = Vec3(x + o.x, y + o.y, z + o.z)
    operator fun minus(o: Vec3) = Vec3(x - o.x, y - o.y, z - o.z)
    operator fun times(s: Double) = Vec3(x * s, y * s, z * s)
    infix fun dot(o: Vec3): Double = x * o.x + y * o.y + z * o.z
    infix fun cross(o: Vec3) = Vec3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x)
    val norm: Double get() = sqrt(x * x + y * y + z * z)
    fun normalized(): Vec3 {
        val n = norm
        return if (n <= 0.0) this else Vec3(x / n, y / n, z / n)
    }

    companion object {
        val ZERO = Vec3(0.0, 0.0, 0.0)
    }
}

/** Unit quaternion in ARCore's / the engine's shared `(x, y, z, w)` order. */
data class Quat(val x: Double, val y: Double, val z: Double, val w: Double) {

    val norm: Double get() = sqrt(x * x + y * y + z * z + w * w)

    fun normalized(): Quat {
        val n = norm
        return if (n <= 0.0) IDENTITY else Quat(x / n, y / n, z / n, w / n)
    }

    fun conjugate() = Quat(-x, -y, -z, w)

    operator fun times(o: Quat) = Quat(
        w * o.x + x * o.w + y * o.z - z * o.y,
        w * o.y - x * o.z + y * o.w + z * o.x,
        w * o.z + x * o.y - y * o.x + z * o.w,
        w * o.w - x * o.x - y * o.y - z * o.z,
    )

    /** Rotates [v] by this quaternion (`q v q*`). */
    fun rotate(v: Vec3): Vec3 {
        val u = Vec3(x, y, z)
        val s = w
        return u * (2.0 * (u dot v)) + v * (s * s - (u dot u)) + (u cross v) * (2.0 * s)
    }

    /**
     * Smallest angle (radians) between two orientations. Uses `|w|` of the
     * relative quaternion so a quaternion and its negation — which are the
     * same rotation, and which ARCore hands out with either sign, exactly the
     * hazard A8 §3.4 documents for SLERP — read as zero rotation, not as a
     * half-turn.
     */
    fun angleTo(o: Quat): Double {
        val r = (o * this.conjugate()).normalized()
        return 2.0 * acos(min(1.0, abs(r.w)))
    }

    /** Roll about the camera's own optical (+z) axis, radians, in [-pi, pi]. */
    fun rollRadians(): Double {
        // ARCore's camera frame is +x right, +y up, -z into the scene. "Roll"
        // as the wizard means it (WIZARD.md screen 2, "tilt the phone") is
        // rotation about that optical axis, i.e. how far the image's own
        // horizontal axis has turned in the world.
        val right = rotate(Vec3(1.0, 0.0, 0.0))
        val up = rotate(Vec3(0.0, 1.0, 0.0))
        // Project the world-up onto the camera's right/up plane and read its
        // angle; that is the visible tilt of the horizon in the image.
        val worldUp = Vec3(0.0, 1.0, 0.0)
        return atan2(worldUp dot right, worldUp dot up)
    }

    /**
     * ROUND 20 (item 79) — the **swing–twist decomposition** about [axis]
     * (unit): `this = twist ∘ swing`, where `twist` is a rotation purely about
     * [axis] and `swing` has **zero component along [axis]** (its quaternion's
     * projection onto the axis is exactly zero — the standard invariant of the
     * decomposition, and what the gravity-referenced trim asserts on).
     *
     * Degenerate case: when the projection `(w, q·axis)` has ~zero norm the
     * rotation is a half-turn about an axis perpendicular to [axis] and the
     * twist is genuinely undefined; identity is returned for it (the whole
     * rotation is then swing), which is the conservative answer for the one
     * caller this exists for — a scanning hold is never a half-turn from
     * upright.
     */
    fun twistAbout(axis: Vec3): Quat {
        val a = axis.normalized()
        val proj = x * a.x + y * a.y + z * a.z
        val t = Quat(a.x * proj, a.y * proj, a.z * proj, w)
        return if (t.norm < 1e-9) IDENTITY else t.normalized()
    }

    /** The swing half of [twistAbout]'s decomposition: `twist⁻¹ ∘ this`. */
    fun swingAbout(axis: Vec3): Quat = (twistAbout(axis).conjugate() * this).normalized()

    companion object {
        val IDENTITY = Quat(0.0, 0.0, 0.0, 1.0)

        fun fromAxisAngle(axis: Vec3, radians: Double): Quat {
            val a = axis.normalized()
            val h = radians / 2.0
            val s = sin(h)
            return Quat(a.x * s, a.y * s, a.z * s, cos(h))
        }

        /** From a row-major 3x3 rotation block (Shepperd's method — numerically stable in all four cases). */
        fun fromRotationMatrix(r: DoubleArray): Quat {
            require(r.size == 9) { "expected a 3x3 row-major rotation" }
            val trace = r[0] + r[4] + r[8]
            return if (trace > 0.0) {
                val s = sqrt(trace + 1.0) * 2.0
                Quat((r[7] - r[5]) / s, (r[2] - r[6]) / s, (r[3] - r[1]) / s, 0.25 * s).normalized()
            } else if (r[0] > r[4] && r[0] > r[8]) {
                val s = sqrt(1.0 + r[0] - r[4] - r[8]) * 2.0
                Quat(0.25 * s, (r[1] + r[3]) / s, (r[2] + r[6]) / s, (r[7] - r[5]) / s).normalized()
            } else if (r[4] > r[8]) {
                val s = sqrt(1.0 + r[4] - r[0] - r[8]) * 2.0
                Quat((r[1] + r[3]) / s, 0.25 * s, (r[5] + r[7]) / s, (r[2] - r[6]) / s).normalized()
            } else {
                val s = sqrt(1.0 + r[8] - r[0] - r[4]) * 2.0
                Quat((r[2] + r[6]) / s, (r[5] + r[7]) / s, 0.25 * s, (r[3] - r[1]) / s).normalized()
            }
        }
    }
}

/** A rigid 4x4 transform, **row-major**, in the layout the engine's C ABI takes. */
@JvmInline
value class Mat4(val m: DoubleArray) {

    init {
        require(m.size == 16) { "Mat4 needs 16 elements" }
    }

    val translation: Vec3 get() = Vec3(m[3], m[7], m[11])

    /** The rotation block, row-major 3x3. */
    fun rotation(): DoubleArray = doubleArrayOf(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10])

    fun rotationQuat(): Quat = Quat.fromRotationMatrix(rotation())

    fun transform(v: Vec3) = Vec3(
        m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
        m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
        m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11],
    )

    operator fun times(o: Mat4): Mat4 {
        val r = DoubleArray(16)
        for (i in 0 until 4) {
            for (j in 0 until 4) {
                var s = 0.0
                for (k in 0 until 4) s += m[i * 4 + k] * o.m[k * 4 + j]
                r[i * 4 + j] = s
            }
        }
        return Mat4(r)
    }

    /** Inverse of a RIGID transform (`R^T`, `-R^T t`) — not a general inverse. */
    fun inverseRigid(): Mat4 {
        val r = DoubleArray(16)
        r[0] = m[0]; r[1] = m[4]; r[2] = m[8]
        r[4] = m[1]; r[5] = m[5]; r[6] = m[9]
        r[8] = m[2]; r[9] = m[6]; r[10] = m[10]
        val t = translation
        r[3] = -(r[0] * t.x + r[1] * t.y + r[2] * t.z)
        r[7] = -(r[4] * t.x + r[5] * t.y + r[6] * t.z)
        r[11] = -(r[8] * t.x + r[9] * t.y + r[10] * t.z)
        r[15] = 1.0
        return Mat4(r)
    }

    /**
     * True when the rotation block is orthonormal and right-handed to
     * [tolerance]. The engine runs the same check (`se3::mat4_is_rigid`) on
     * every `double[16]` crossing its API and rejects failures; checking here
     * too means the wizard can say *which* matrix was bad instead of relaying
     * a bare `SCAN_ERR_INVALID_ARGUMENT`.
     */
    fun isRigid(tolerance: Double = 1e-6): Boolean {
        val r = rotation()
        // Columns orthonormal.
        for (i in 0 until 3) {
            for (j in 0 until 3) {
                var s = 0.0
                for (k in 0 until 3) s += r[k * 3 + i] * r[k * 3 + j]
                val want = if (i == j) 1.0 else 0.0
                if (abs(s - want) > tolerance) return false
            }
        }
        // Right-handed: det == +1, not -1 (a mirrored frame passes the
        // orthonormality test and is exactly the failure mode a column-major
        // matrix produces).
        val det = r[0] * (r[4] * r[8] - r[5] * r[7]) -
            r[1] * (r[3] * r[8] - r[5] * r[6]) +
            r[2] * (r[3] * r[7] - r[4] * r[6])
        if (abs(det - 1.0) > tolerance) return false
        return abs(m[12]) < tolerance && abs(m[13]) < tolerance && abs(m[14]) < tolerance &&
            abs(m[15] - 1.0) < tolerance
    }

    companion object {
        fun identity() = Mat4(
            doubleArrayOf(
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 1.0,
            ),
        )

        fun fromRotationTranslation(q: Quat, t: Vec3): Mat4 {
            val n = q.normalized()
            val xx = n.x * n.x; val yy = n.y * n.y; val zz = n.z * n.z
            val xy = n.x * n.y; val xz = n.x * n.z; val yz = n.y * n.z
            val wx = n.w * n.x; val wy = n.w * n.y; val wz = n.w * n.z
            return Mat4(
                doubleArrayOf(
                    1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), t.x,
                    2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx), t.y,
                    2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy), t.z,
                    0.0, 0.0, 0.0, 1.0,
                ),
            )
        }

        /**
         * The one conversion point for Android/ARCore matrices, which are
         * COLUMN-major (`Pose.toMatrix()`, `android.opengl.Matrix`). Handing
         * one of those straight to the engine is the documented field failure
         * this function exists to prevent.
         */
        fun fromColumnMajor(cm: FloatArray): Mat4 {
            require(cm.size == 16) { "expected 16 elements" }
            val r = DoubleArray(16)
            for (row in 0 until 4) {
                for (col in 0 until 4) r[row * 4 + col] = cm[col * 4 + row].toDouble()
            }
            return Mat4(r)
        }
    }
}

/** Signed shortest difference between two angles, radians, in [-pi, pi]. */
fun angleDifference(a: Double, b: Double): Double {
    var d = a - b
    while (d > Math.PI) d -= 2.0 * Math.PI
    while (d < -Math.PI) d += 2.0 * Math.PI
    return d
}

/** Angle between two unit vectors, radians, in [0, pi]. */
fun angleBetween(a: Vec3, b: Vec3): Double =
    acos((a.normalized() dot b.normalized()).coerceIn(-1.0, 1.0))

/** `asin` clamped to its domain — the arguments here come from dot products that can land at 1+1e-16. */
fun safeAsin(v: Double): Double = asin(v.coerceIn(-1.0, 1.0))
