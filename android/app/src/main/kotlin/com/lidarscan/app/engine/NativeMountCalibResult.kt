package com.lidarscan.app.engine

/**
 * Mirrors `scan_mount_calib_result` (`scanengine_c.h`) field for field.
 * Constructed from JNI via a cached constructor (`([DZZIIJJDDDDIDDD)V` —
 * `arcore_jni.cpp`'s `nativeMountCalibSolve`).
 *
 * **Gate on [splitHalfPx] and [gate]; never on [sigmaRotDeg] /
 * [sigmaTransMm] / [conditionNumber].** The header says so in as many words,
 * and A8 §4.3 measured why: over 21 sessions differing only in their noise
 * realisation, the true rotation error's coefficient of variation was 1.83
 * against the reported sigma's 0.13 — the covariance is nearly constant while
 * the thing it describes varies by a factor of two, so it cannot rank
 * sessions, which is the entire job of a gate. The three fields are carried
 * anyway because a bench pass will want to look at them.
 */
data class NativeMountCalibResult(
    /** `camera_from_lidar` == `phone_from_lidar`, **row-major** 4x4. */
    val cameraFromLidar: DoubleArray,
    val converged: Boolean,
    /** Too few observations, or a rank-deficient solve. Forced to a REJECT gate by the engine. */
    val degenerate: Boolean,
    val iterationsL2: Int,
    val iterationsRobust: Int,
    val observations: Long,
    val residuals: Long,
    val rmsResidualM: Double,
    val finalCost: Double,
    /** THE GATE: pixels of disagreement at [gateRangeM] between two half-solves. -1 when not computed. */
    val splitHalfPx: Double,
    val gateRangeM: Double,
    /** `SCAN_CALIB_GATE_*`. */
    val gate: Int,
    val sigmaRotDeg: Double,
    val sigmaTransMm: Double,
    val conditionNumber: Double,
) {
    override fun equals(other: Any?): Boolean =
        other is NativeMountCalibResult && cameraFromLidar.contentEquals(other.cameraFromLidar) &&
            splitHalfPx == other.splitHalfPx && gate == other.gate

    override fun hashCode(): Int = 31 * cameraFromLidar.contentHashCode() + gate
}
