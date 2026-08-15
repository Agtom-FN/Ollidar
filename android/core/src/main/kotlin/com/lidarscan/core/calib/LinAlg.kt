package com.lidarscan.core.calib

import kotlin.math.abs
import kotlin.math.sqrt

/**
 * The two numerical routines the calibration path needs and the JVM does not
 * ship: a symmetric eigen-decomposition (for null-space / dominant-direction
 * problems — the DLT homography and the plane fits) and a small dense linear
 * solve.
 *
 * Both are deliberately textbook and unoptimised. The largest matrix here is
 * 9x9, run a few times per captured pose (not per frame), so clarity beats
 * every other consideration; a hand-rolled "clever" version would be harder
 * to check and no faster at this size.
 */
object LinAlg {

    /**
     * Cyclic Jacobi eigen-decomposition of a symmetric `n x n` matrix (row-major).
     * Returns eigenvalues ascending and the matching eigenvectors as columns
     * of the returned `n x n` row-major matrix — so the null-space vector of
     * an `AᵀA` is column 0.
     *
     * Jacobi (rather than the usual "just take the last row of V from an
     * SVD") because it needs no external library, is unconditionally stable
     * for symmetric input, and its convergence criterion is a single
     * off-diagonal norm that a test can assert on.
     */
    fun symmetricEigen(matrix: DoubleArray, n: Int): Pair<DoubleArray, DoubleArray> {
        require(matrix.size == n * n) { "expected ${n * n} elements" }
        val a = matrix.copyOf()
        val v = DoubleArray(n * n)
        for (i in 0 until n) v[i * n + i] = 1.0

        repeat(100) {
            var off = 0.0
            for (p in 0 until n) {
                for (q in p + 1 until n) off += a[p * n + q] * a[p * n + q]
            }
            if (off < 1e-24) return@repeat

            for (p in 0 until n) {
                for (q in p + 1 until n) {
                    val apq = a[p * n + q]
                    if (abs(apq) < 1e-18) continue
                    val app = a[p * n + p]
                    val aqq = a[q * n + q]
                    val theta = (aqq - app) / (2.0 * apq)
                    val t = if (theta >= 0.0) {
                        1.0 / (theta + sqrt(1.0 + theta * theta))
                    } else {
                        -1.0 / (-theta + sqrt(1.0 + theta * theta))
                    }
                    val c = 1.0 / sqrt(1.0 + t * t)
                    val s = t * c

                    for (k in 0 until n) {
                        val akp = a[k * n + p]
                        val akq = a[k * n + q]
                        a[k * n + p] = c * akp - s * akq
                        a[k * n + q] = s * akp + c * akq
                    }
                    for (k in 0 until n) {
                        val apk = a[p * n + k]
                        val aqk = a[q * n + k]
                        a[p * n + k] = c * apk - s * aqk
                        a[q * n + k] = s * apk + c * aqk
                    }
                    for (k in 0 until n) {
                        val vkp = v[k * n + p]
                        val vkq = v[k * n + q]
                        v[k * n + p] = c * vkp - s * vkq
                        v[k * n + q] = s * vkp + c * vkq
                    }
                }
            }
        }

        val values = DoubleArray(n) { a[it * n + it] }
        val order = (0 until n).sortedBy { values[it] }
        val sortedValues = DoubleArray(n) { values[order[it]] }
        val sortedVectors = DoubleArray(n * n)
        for (col in 0 until n) {
            for (row in 0 until n) sortedVectors[row * n + col] = v[row * n + order[col]]
        }
        return sortedValues to sortedVectors
    }

    /**
     * Solves `A x = b` for a dense `n x n` A (row-major) by Gaussian
     * elimination with partial pivoting. Returns null when the matrix is
     * singular to within [tolerance] — a caller has to handle that (a
     * degenerate pose), so it is not an exception.
     */
    fun solve(a: DoubleArray, b: DoubleArray, n: Int, tolerance: Double = 1e-12): DoubleArray? {
        val m = a.copyOf()
        val x = b.copyOf()
        for (col in 0 until n) {
            var pivot = col
            for (row in col + 1 until n) {
                if (abs(m[row * n + col]) > abs(m[pivot * n + col])) pivot = row
            }
            if (abs(m[pivot * n + col]) < tolerance) return null
            if (pivot != col) {
                for (k in 0 until n) {
                    val t = m[col * n + k]; m[col * n + k] = m[pivot * n + k]; m[pivot * n + k] = t
                }
                val t = x[col]; x[col] = x[pivot]; x[pivot] = t
            }
            for (row in col + 1 until n) {
                val f = m[row * n + col] / m[col * n + col]
                if (f == 0.0) continue
                for (k in col until n) m[row * n + k] -= f * m[col * n + k]
                x[row] -= f * x[col]
            }
        }
        for (row in n - 1 downTo 0) {
            var s = x[row]
            for (k in row + 1 until n) s -= m[row * n + k] * x[k]
            x[row] = s / m[row * n + row]
        }
        return x
    }
}
