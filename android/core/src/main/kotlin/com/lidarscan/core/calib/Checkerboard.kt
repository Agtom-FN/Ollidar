package com.lidarscan.core.calib

/**
 * The printed target, as WIZARD.md screen 1 specifies it: an **A1
 * checkerboard, 8x6 inner corners at 100 mm**, on rigid backing.
 *
 * [cols]/[rows] count INNER corners (the saddle points where four squares
 * meet), not squares — that is the quantity a detector can actually find and
 * the quantity the object-point grid is built from. An 8x6 inner-corner board
 * is 9x7 squares, i.e. 0.90 x 0.70 m at 100 mm squares, which fits A1
 * (0.841 x 0.594 m) with the margin the print needs... which is why the
 * shipped default below is **0.080 m squares** on 8x6 inner corners: 0.72 x
 * 0.56 m of pattern inside an A1 sheet with a ~6 cm quiet border. The
 * `squareSizeM` field is user-editable in the wizard precisely because a
 * printer that silently scales the PDF is the single most common way a
 * calibration goes quietly wrong (WIZARD.md screen 1: "a 'measure your
 * square size' field as an escape hatch").
 */
data class CheckerboardSpec(
    val cols: Int = 8,
    val rows: Int = 6,
    val squareSizeM: Double = 0.080,
) {
    val cornerCount: Int get() = cols * rows

    /** Board width/height across the OUTER edge of the printed pattern, metres. */
    val patternWidthM: Double get() = (cols + 1) * squareSizeM
    val patternHeightM: Double get() = (rows + 1) * squareSizeM

    /**
     * Object points for the inner corners, in the board frame (z = 0, origin
     * at the top-left inner corner, +x along a row, +y down a column), in
     * row-major order matching [CheckerboardDetection.corners].
     */
    fun objectPoints(): List<Vec3> = buildList {
        for (r in 0 until rows) {
            for (c in 0 until cols) add(Vec3(c * squareSizeM, r * squareSizeM, 0.0))
        }
    }

    init {
        require(cols >= 3 && rows >= 3) { "a checkerboard needs at least 3x3 inner corners" }
        require(squareSizeM > 0.0) { "square size must be positive" }
    }
}

/**
 * An 8-bit luma image. Deliberately the *only* image type `:core` knows
 * about: `android.media.Image`'s Y plane is already 8-bit luma with a row
 * stride, so the app side extracts a [LumaImage] and everything downstream
 * (detector, tests, synthetic fixtures) is plain JVM with no Android
 * dependency and no bitmap decode.
 */
class LumaImage(val width: Int, val height: Int, val pixels: ByteArray) {
    init {
        require(width > 0 && height > 0) { "empty image" }
        require(pixels.size >= width * height) { "pixel buffer too small for ${width}x$height" }
    }

    /** 0..255. */
    fun at(x: Int, y: Int): Int = pixels[y * width + x].toInt() and 0xFF

    /** Bilinear sample; out-of-range coordinates clamp to the border. */
    fun sample(x: Double, y: Double): Double {
        val cx = x.coerceIn(0.0, (width - 1).toDouble())
        val cy = y.coerceIn(0.0, (height - 1).toDouble())
        val x0 = cx.toInt().coerceAtMost(width - 2)
        val y0 = cy.toInt().coerceAtMost(height - 2)
        val fx = cx - x0
        val fy = cy - y0
        val v00 = at(x0, y0).toDouble()
        val v10 = at(x0 + 1, y0).toDouble()
        val v01 = at(x0, y0 + 1).toDouble()
        val v11 = at(x0 + 1, y0 + 1).toDouble()
        return (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy
    }

    /** Box-downsamples by an integer [factor] (>= 1). Used to run detection at a fixed working resolution. */
    fun downsample(factor: Int): LumaImage {
        require(factor >= 1)
        if (factor == 1) return this
        val w = width / factor
        val h = height / factor
        val out = ByteArray(w * h)
        for (y in 0 until h) {
            for (x in 0 until w) {
                var sum = 0
                for (dy in 0 until factor) {
                    val sy = y * factor + dy
                    for (dx in 0 until factor) sum += at(x * factor + dx, sy)
                }
                out[y * w + x] = (sum / (factor * factor)).toByte()
            }
        }
        return LumaImage(w, h, out)
    }
}

/** A detected corner, in FULL-resolution image pixels, subpixel-refined. */
data class Corner(val x: Double, val y: Double, val response: Double)

/**
 * A complete, ordered detection: [corners] holds `spec.rows * spec.cols`
 * corners in row-major order, consistent with [CheckerboardSpec.objectPoints].
 * "Consistent" is all that is required — the board's own orientation is
 * unobservable from the pattern alone (a checkerboard is 180°-symmetric), and
 * nothing downstream needs it: the plane `(n, d)` the mount solver wants is
 * invariant to which corner is called the origin.
 */
data class CheckerboardDetection(
    val spec: CheckerboardSpec,
    val corners: List<Corner>,
    /** Mean absolute saddle response over the accepted corners — a rough sharpness/contrast score. */
    val meanResponse: Double,
) {
    init {
        require(corners.size == spec.cornerCount) {
            "expected ${spec.cornerCount} corners, got ${corners.size}"
        }
    }

    fun corner(row: Int, col: Int): Corner = corners[row * spec.cols + col]

    /** Smallest distance from any corner to the image border, in pixels. */
    fun marginPx(imageWidth: Int, imageHeight: Int): Double = corners.minOf { c ->
        minOf(c.x, c.y, imageWidth - 1 - c.x, imageHeight - 1 - c.y)
    }

    /** Fraction of the image area the detected corner grid's bounding box covers (0..1). */
    fun frameCoverage(imageWidth: Int, imageHeight: Int): Double {
        val minX = corners.minOf { it.x }
        val maxX = corners.maxOf { it.x }
        val minY = corners.minOf { it.y }
        val maxY = corners.maxOf { it.y }
        return ((maxX - minX) * (maxY - minY)) / (imageWidth.toDouble() * imageHeight.toDouble())
    }
}
