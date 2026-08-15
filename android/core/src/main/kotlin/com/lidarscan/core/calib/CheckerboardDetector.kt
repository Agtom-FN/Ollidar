package com.lidarscan.core.calib

import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * Finds the inner corners of a printed checkerboard in a camera luma image.
 *
 * **Why this exists at all.** S6's verdict (WIZARD.md §0, finding 1) is that
 * the camera side of the mount calibration *must* be a checkerboard — ARCore
 * plane detection is 3–6x over the whole colorization budget on its own — and
 * A8 §4.1 deliberately keeps the detection on the app side, because "it keeps
 * checkerboard detection and PnP on the app side where the image already is".
 * So there is no engine call to delegate to; this is the app's job.
 *
 * **What is actually implemented** (read this before trusting a number that
 * comes out of it):
 *
 *  1. Optional box-downsample to a working resolution (default: longest side
 *     ~720 px), then a separable Gaussian blur.
 *  2. A **saddle response** `Ixy² − Ixx·Iyy` from the image Hessian. A
 *     checkerboard inner corner is a saddle of intensity, so this response is
 *     positive there and *negative* at blobs and flat regions — which is the
 *     property that keeps it from firing on every high-contrast blob a Harris
 *     detector would return. Corners of the board's outer frame, text, and
 *     window mullions still fire; step 4 is what removes them.
 *  3. Non-maximum suppression at a radius, keeping the top-K candidates.
 *  4. **Lattice growth**: from a seed candidate, estimate two local lattice
 *     vectors from its neighbours, then grow a grid by predicting the next
 *     corner at `p + e` and snapping to the nearest candidate within a
 *     tolerance, updating the local basis at every step so perspective
 *     foreshortening is tracked as the grid walks across the board. The
 *     largest fully-populated rectangular block is then matched against the
 *     expected `cols x rows` (either orientation — a non-square board
 *     disambiguates itself).
 *  5. Subpixel refinement by fitting a 2-D quadratic to the saddle response
 *     in a 3x3 neighbourhood at FULL resolution.
 *
 * **What it is NOT, stated plainly.** This is not OpenCV's
 * `findChessboardCornersSB`. Specifically it does not: verify alternating
 * corner polarity, run OpenCV's iterative gradient-orthogonality subpixel
 * step, handle a partially occluded board, adapt its blur to the observed
 * corner spacing, or reject a mis-scaled print. Its verified envelope is the
 * synthetic one in `CheckerboardDetectorTest` — a rendered board under
 * perspective, roll up to ±60°, Gaussian blur and additive noise — and
 * **nothing in this repository has yet run it on a real camera image**,
 * because no ARCore device was available (see android/NOTES.md). The wizard
 * treats a detection as a *proposal*: every pose still has to pass the live
 * checks in [PoseChecks], and the session still has to pass the split-half
 * gate, which is precisely the S6-mandated safety net for a capture whose
 * inputs are worse than they look.
 */
interface CheckerboardDetector {
    fun detect(image: LumaImage, spec: CheckerboardSpec): CheckerboardDetection?
}

/** Tunables, exposed so a test (or a later bench pass) can move them without editing the detector. */
data class DetectorConfig(
    /** Longest side of the working image; the input is box-downsampled to fit. */
    val workingLongSide: Int = 720,
    val blurSigma: Double = 1.2,
    /** NMS radius in working pixels. */
    val suppressionRadius: Int = 6,
    /** Candidates kept after NMS. 4x an 8x6 board leaves room for clutter without exploding the grid search. */
    val maxCandidates: Int = 220,
    /** A candidate must reach this fraction of the strongest response to be kept. */
    val relativeResponseFloor: Double = 0.02,
    /** How far a predicted lattice position may be from a real candidate, as a fraction of the step length. */
    val snapTolerance: Double = 0.35,
    /** Seeds tried before giving up (strongest candidates first). */
    val maxSeeds: Int = 24,
)

class SaddleCheckerboardDetector(private val config: DetectorConfig = DetectorConfig()) :
    CheckerboardDetector {

    override fun detect(image: LumaImage, spec: CheckerboardSpec): CheckerboardDetection? {
        val scale = downsampleFactor(image, config.workingLongSide)
        val work = image.downsample(scale)
        if (work.width < 32 || work.height < 32) return null

        val response = saddleResponse(work, config.blurSigma)
        val candidates = suppressAndRank(response, work.width, work.height)
        if (candidates.size < spec.cornerCount) return null

        val grid = fitGrid(candidates, spec, work) ?: return null

        // Back to full resolution, then refine each corner against the
        // full-res response surface (the working-resolution position is only
        // accurate to ~1 working pixel = `scale` full pixels, which at
        // scale=3 would be a 3 px error — far too coarse for a calibration
        // whose whole budget is ~4 px at 3 m).
        val fullResponse = saddleResponse(image, config.blurSigma * scale)
        val refined = grid.map { p ->
            refine(fullResponse, image.width, image.height, p.x * scale, p.y * scale)
        }
        val mean = refined.sumOf { abs(it.response) } / refined.size
        return CheckerboardDetection(spec, refined, mean)
    }

    // --- 1/2. response ------------------------------------------------------

    private fun downsampleFactor(image: LumaImage, longSide: Int): Int {
        val longest = max(image.width, image.height)
        if (longest <= longSide) return 1
        return max(1, longest / longSide)
    }

    /**
     * `Ixy² − Ixx·Iyy` over a Gaussian-blurred image: positive at a saddle,
     * negative at a peak/valley, ~0 on flat ground and on a straight edge.
     */
    internal fun saddleResponse(image: LumaImage, sigma: Double): DoubleArray {
        val w = image.width
        val h = image.height
        val blurred = gaussianBlur(image, sigma)
        val out = DoubleArray(w * h)
        for (y in 1 until h - 1) {
            for (x in 1 until w - 1) {
                val i = y * w + x
                val ixx = blurred[i - 1] - 2 * blurred[i] + blurred[i + 1]
                val iyy = blurred[i - w] - 2 * blurred[i] + blurred[i + w]
                val ixy = (blurred[i - w - 1] + blurred[i + w + 1] -
                    blurred[i - w + 1] - blurred[i + w - 1]) / 4.0
                out[i] = ixy * ixy - ixx * iyy
            }
        }
        return out
    }

    private fun gaussianBlur(image: LumaImage, sigma: Double): DoubleArray {
        val w = image.width
        val h = image.height
        val radius = max(1, (sigma * 3.0).roundToInt())
        val kernel = DoubleArray(2 * radius + 1)
        var sum = 0.0
        for (i in kernel.indices) {
            val d = (i - radius).toDouble()
            kernel[i] = exp(-(d * d) / (2 * sigma * sigma))
            sum += kernel[i]
        }
        for (i in kernel.indices) kernel[i] /= sum

        val tmp = DoubleArray(w * h)
        for (y in 0 until h) {
            for (x in 0 until w) {
                var acc = 0.0
                for (k in kernel.indices) {
                    val sx = (x + k - radius).coerceIn(0, w - 1)
                    acc += kernel[k] * image.at(sx, y)
                }
                tmp[y * w + x] = acc
            }
        }
        val out = DoubleArray(w * h)
        for (y in 0 until h) {
            for (x in 0 until w) {
                var acc = 0.0
                for (k in kernel.indices) {
                    val sy = (y + k - radius).coerceIn(0, h - 1)
                    acc += kernel[k] * tmp[sy * w + x]
                }
                out[y * w + x] = acc
            }
        }
        return out
    }

    // --- 3. NMS -------------------------------------------------------------

    private fun suppressAndRank(response: DoubleArray, w: Int, h: Int): List<Corner> {
        val r = config.suppressionRadius
        var maxResponse = 0.0
        for (v in response) if (v > maxResponse) maxResponse = v
        if (maxResponse <= 0.0) return emptyList()
        val floor = maxResponse * config.relativeResponseFloor

        val peaks = ArrayList<Corner>()
        for (y in r until h - r) {
            for (x in r until w - r) {
                val v = response[y * w + x]
                if (v < floor) continue
                var isMax = true
                loop@ for (dy in -r..r) {
                    for (dx in -r..r) {
                        if (dx == 0 && dy == 0) continue
                        if (response[(y + dy) * w + (x + dx)] > v) {
                            isMax = false
                            break@loop
                        }
                    }
                }
                if (isMax) peaks.add(Corner(x.toDouble(), y.toDouble(), v))
            }
        }
        return peaks.sortedByDescending { it.response }.take(config.maxCandidates)
    }

    // --- 4. lattice growth --------------------------------------------------

    private data class GridKey(val i: Int, val j: Int)

    private companion object {
        /**
         * How many candidate lattice bases to try per seed before moving on.
         * More than one is needed because the X-junction test verifies that a
         * basis points at four alternating quadrants but says nothing about
         * SCALE: an offset to a spurious peak 9 px from a true corner still
         * lands its quadrant samples inside the four correct squares when the
         * real step is 30 px. So the shortest verified basis is not reliably
         * the lattice, and the thing that actually distinguishes them is
         * whether the grid GROWS — which is what trying several does.
         */
        const val MAX_BASES_PER_SEED = 8
    }

    /**
     * Grows a lattice from each seed in turn and returns the first grid whose
     * fully-populated rectangular block matches [spec] (in either
     * orientation), re-ordered row-major to match
     * [CheckerboardSpec.objectPoints].
     */
    private fun fitGrid(
        candidates: List<Corner>,
        spec: CheckerboardSpec,
        image: LumaImage,
    ): List<Corner>? {
        val seeds = candidates.take(min(config.maxSeeds, candidates.size))
        for (seed in seeds) {
            for (basis in estimateBases(seed, candidates, image)) {
            val (grid, bases) = grow(seed, basis.first, basis.second, candidates, image)
            // Prune everything that is not a true X-junction BEFORE looking
            // for a block: the outer ring of a printed board's squares meets
            // the white quiet zone at points that are still local maxima of
            // the saddle response, so without this step a `cols x rows` board
            // presents as a `(cols+2) x (rows+2)` lattice and the size match
            // below rejects a perfectly good detection.
            val verified = grid.filter { (key, corner) ->
                val b = bases[key] ?: return@filter false
                isXJunction(image, corner, b.first, b.second)
            }
            val block = largestFullBlock(verified) ?: continue
            if (DEBUG) println("seed=$seed basis=$basis grid=${grid.size} verified=${verified.size} block=$block")
            val ordered = orderBlock(verified, block, spec) ?: continue
            if (!isProjectivelyRegular(ordered, spec)) continue
            return ordered
            }
        }
        return null
    }

    /**
     * The whole-grid consistency check: a planar board's inner corners are a
     * regular grid, so their image must be a PROJECTIVE image of one — a
     * single homography has to explain all `cols x rows` of them.
     *
     * This is what catches the failure the per-node X-junction test cannot: a
     * grid in which 47 corners are right and one snapped to a spurious peak
     * ~9 px away. The grid still looks complete and every node is a genuine
     * corner-like feature; only the global regularity is violated. Left in,
     * that one corner would drag the plane estimate — and, downstream, the
     * extrinsic — by far more than S6's whole 0.16° budget.
     *
     * Rejecting (rather than repairing) is deliberate: this runs inside the
     * seed/basis loop, so a rejection just means the next basis is tried, and
     * if none survives, the wizard simply does not capture that pose. A pose
     * not captured costs the user a second; a silently bent one costs the
     * calibration.
     */
    private fun isProjectivelyRegular(ordered: List<Corner>, spec: CheckerboardSpec): Boolean {
        val unitGrid = buildList {
            for (r in 0 until spec.rows) {
                for (c in 0 until spec.cols) add(Vec3(c.toDouble(), r.toDouble(), 0.0))
            }
        }
        val h = TargetPlaneEstimator.homography(unitGrid, ordered) ?: return false

        // Tolerance scales with the observed corner spacing: 12% of a step,
        // floored at 1.5 px so a distant (small-step) board is not held to
        // sub-pixel perfection the response map cannot deliver.
        val step = hypot(ordered[1].x - ordered[0].x, ordered[1].y - ordered[0].y)
        val tolerance = max(1.5, 0.12 * step)

        for (i in ordered.indices) {
            val g = unitGrid[i]
            val w = h[6] * g.x + h[7] * g.y + h[8]
            if (abs(w) < 1e-12) return false
            val u = (h[0] * g.x + h[1] * g.y + h[2]) / w
            val v = (h[3] * g.x + h[4] * g.y + h[5]) / w
            if (hypot(u - ordered[i].x, v - ordered[i].y) > tolerance) return false
        }
        return true
    }

    /**
     * Confirms a lattice node is a genuine checkerboard corner by sampling the
     * four quadrants it separates: at an X-junction the two DIAGONAL quadrants
     * match and the two adjacent ones differ strongly. A corner where the
     * pattern meets the paper's quiet zone has three quadrants the same colour
     * and fails this immediately.
     *
     * This is the one place the detector looks at the image again after the
     * response stage, and it is what makes the exact `cols x rows` size match
     * safe to insist on.
     */
    private fun isXJunction(image: LumaImage, corner: Corner, e1: Vec2, e2: Vec2): Boolean {
        // Quadrant centres sit half a lattice step away along each axis.
        fun s(a: Double, b: Double): Double = image.sample(
            corner.x + (a * e1.x + b * e2.x) / 2.0,
            corner.y + (a * e1.y + b * e2.y) / 2.0,
        )
        val pp = s(1.0, 1.0)
        val pm = s(1.0, -1.0)
        val mp = s(-1.0, 1.0)
        val mm = s(-1.0, -1.0)

        val diagonalA = (pp + mm) / 2.0
        val diagonalB = (pm + mp) / 2.0
        val contrast = abs(diagonalA - diagonalB)
        if (contrast < 25.0) return false  // no black/white step here at all
        // Each diagonal pair must agree to well inside the step between them.
        return abs(pp - mm) < 0.5 * contrast && abs(pm - mp) < 0.5 * contrast
    }

    /**
     * Two independent lattice vectors from [seed]'s neighbourhood.
     *
     * The obvious version — "take the two shortest non-collinear neighbour
     * offsets" — does not survive contact with a real response map. NMS
     * suppresses within [DetectorConfig.suppressionRadius], which is
     * deliberately *smaller* than the snap tolerance, so a spurious peak can
     * and does sit 7–15 px from a true corner while the real lattice step is
     * 30 px. Seeded with that offset, the growth walks one cell and dies.
     *
     * So every candidate pair is instead VERIFIED as a lattice basis by
     * checking that the seed is an X-junction *at that scale*: with a
     * too-short basis the four quadrant samples all land inside one square,
     * the contrast collapses, and the pair is rejected. The shortest pair
     * that survives is the lattice.
     */
    private fun estimateBases(
        seed: Corner,
        candidates: List<Corner>,
        image: LumaImage,
    ): List<Pair<Vec2, Vec2>> {
        val offsets = candidates.asSequence()
            .filter { it !== seed }
            .map { Vec2(it.x - seed.x, it.y - seed.y) }
            .filter { it.length > 2.0 }
            .sortedBy { it.length }
            .take(16)
            .toList()
        if (offsets.size < 2) return emptyList()

        val bases = ArrayList<Pair<Pair<Vec2, Vec2>, Double>>()
        for (i in offsets.indices) {
            for (j in i + 1 until offsets.size) {
                val e1 = offsets[i]
                val e2 = offsets[j]
                if (abs(cosBetween(e1, e2)) >= 0.87) continue
                if (!isXJunction(image, seed, e1, e2)) continue
                bases.add((e1 to e2) to max(e1.length, e2.length))
            }
        }
        return bases.sortedBy { it.second }.take(MAX_BASES_PER_SEED).map { it.first }
    }

    private fun grow(
        seed: Corner,
        e1: Vec2,
        e2: Vec2,
        candidates: List<Corner>,
        image: LumaImage,
    ): Pair<Map<GridKey, Corner>, Map<GridKey, Pair<Vec2, Vec2>>> {
        val grid = HashMap<GridKey, Corner>()
        // Per-cell lattice basis: perspective makes the step change as the
        // grid walks across the board, so each placed corner carries the
        // basis that produced it and hands a corrected copy to its children.
        val bases = HashMap<GridKey, Pair<Vec2, Vec2>>()
        val used = HashSet<Corner>()

        grid[GridKey(0, 0)] = seed
        bases[GridKey(0, 0)] = e1 to e2
        used.add(seed)

        val queue = ArrayDeque<GridKey>()
        queue.add(GridKey(0, 0))
        var placements = 0
        val limit = 400  // a hard bound; an 8x6 board needs 48

        while (queue.isNotEmpty() && placements < limit) {
            val key = queue.removeFirst()
            val here = grid[key] ?: continue
            val (b1, b2) = bases[key] ?: continue

            for ((di, dj, step) in listOf(
                Triple(1, 0, b1),
                Triple(-1, 0, Vec2(-b1.x, -b1.y)),
                Triple(0, 1, b2),
                Triple(0, -1, Vec2(-b2.x, -b2.y)),
            )) {
                val next = GridKey(key.i + di, key.j + dj)
                if (grid.containsKey(next)) continue
                val predX = here.x + step.x
                val predY = here.y + step.y
                val tol = step.length * config.snapTolerance
                // Nearest candidate that is ALSO a real X-junction. Testing
                // the junction here rather than only after the walk matters:
                // NMS can leave a spurious peak within the snap tolerance of
                // a true corner (its radius is smaller than the tolerance by
                // design, so a corner is never suppressed by its neighbour),
                // and snapping to that one puts a ~10 px error into a single
                // cell of an otherwise perfect grid — the worst kind, because
                // the grid still looks complete.
                val match = candidates
                    .asSequence()
                    .filter { it !in used }
                    .filter { hypot(it.x - predX, it.y - predY) <= tol }
                    .sortedBy { hypot(it.x - predX, it.y - predY) }
                    .firstOrNull { isXJunction(image, it, b1, b2) }
                    ?: continue

                grid[next] = match
                used.add(match)
                placements++
                // Correct the basis from what was actually found: the step in
                // the direction just walked becomes the observed offset, the
                // other is inherited.
                val observed = Vec2(match.x - here.x, match.y - here.y)
                bases[next] = if (di != 0) {
                    (if (di > 0) observed else Vec2(-observed.x, -observed.y)) to b2
                } else {
                    b1 to (if (dj > 0) observed else Vec2(-observed.x, -observed.y))
                }
                queue.add(next)
            }
        }
        return grid to bases
    }

    private data class Block(val i0: Int, val j0: Int, val width: Int, val height: Int)

    /** The largest axis-aligned block of grid indices that is completely filled. */
    private fun largestFullBlock(grid: Map<GridKey, Corner>): Block? {
        if (grid.isEmpty()) return null
        val minI = grid.keys.minOf { it.i }
        val maxI = grid.keys.maxOf { it.i }
        val minJ = grid.keys.minOf { it.j }
        val maxJ = grid.keys.maxOf { it.j }

        var best: Block? = null
        var bestArea = 0
        for (i0 in minI..maxI) {
            for (j0 in minJ..maxJ) {
                if (!grid.containsKey(GridKey(i0, j0))) continue
                var maxW = maxI - i0 + 1
                var h = 0
                while (j0 + h <= maxJ) {
                    var w = 0
                    while (w < maxW && grid.containsKey(GridKey(i0 + w, j0 + h))) w++
                    if (w == 0) break
                    maxW = min(maxW, w)
                    h++
                    val area = maxW * h
                    if (area > bestArea) {
                        bestArea = area
                        best = Block(i0, j0, maxW, h)
                    }
                }
            }
        }
        return best
    }

    /**
     * Emits the block row-major in the order [CheckerboardSpec.objectPoints]
     * expects. A board seen "sideways" (the grid's i axis running down the
     * board's columns) is transposed here rather than rejected — the wizard
     * asks for roll variation up to ±60°, so a 90°-ish presentation is a
     * normal pose, not an error.
     */
    private fun orderBlock(
        grid: Map<GridKey, Corner>,
        block: Block,
        spec: CheckerboardSpec,
    ): List<Corner>? {
        val transpose = when {
            block.width == spec.cols && block.height == spec.rows -> false
            block.width == spec.rows && block.height == spec.cols -> true
            else -> return null
        }
        val out = ArrayList<Corner>(spec.cornerCount)
        for (r in 0 until spec.rows) {
            for (c in 0 until spec.cols) {
                val key = if (transpose) {
                    GridKey(block.i0 + r, block.j0 + c)
                } else {
                    GridKey(block.i0 + c, block.j0 + r)
                }
                out.add(grid[key] ?: return null)
            }
        }

        // HANDEDNESS. The lattice's own axes come out of `grow()` in whatever
        // order the seed's neighbours happened to be found, so half the time
        // the (col, row) labelling above is a MIRROR of the board rather than
        // a rotation of it. A mirrored correspondence is not merely
        // mislabelled — it is geometrically impossible, and the homography
        // fitted to it decomposes into a left-handed "rotation", i.e. a plane
        // normal pointing through the board. The board's own 180° rotational
        // symmetry means the remaining ambiguity after this fix is harmless
        // (the plane is identical either way), which is exactly why this is
        // the only symmetry worth resolving.
        //
        // A board seen from its printed side projects with the column
        // direction x row direction cross product POSITIVE in image
        // coordinates (image y runs down). If it is negative, reversing the
        // column order turns the mirror back into a rotation.
        val u = Vec2(out[1].x - out[0].x, out[1].y - out[0].y)
        val v = Vec2(out[spec.cols].x - out[0].x, out[spec.cols].y - out[0].y)
        if (u.x * v.y - u.y * v.x < 0.0) {
            val mirrored = ArrayList<Corner>(spec.cornerCount)
            for (r in 0 until spec.rows) {
                for (c in 0 until spec.cols) mirrored.add(out[r * spec.cols + (spec.cols - 1 - c)])
            }
            return mirrored
        }
        return out
    }

    // --- 5. subpixel --------------------------------------------------------

    /**
     * Fits a separable quadratic to the response in a 3x3 neighbourhood and
     * takes its extremum. A displacement larger than one pixel means the fit
     * is not describing a local peak (a ridge, or a neighbouring corner
     * dominating), so it is discarded and the integer position kept.
     */
    internal fun refine(response: DoubleArray, w: Int, h: Int, x0: Double, y0: Double): Corner {
        val xi = x0.roundToInt().coerceIn(1, w - 2)
        val yi = y0.roundToInt().coerceIn(1, h - 2)
        val i = yi * w + xi
        val c = response[i]
        val dxx = response[i + 1] - 2 * c + response[i - 1]
        val dyy = response[i + w] - 2 * c + response[i - w]
        val dx = (response[i + 1] - response[i - 1]) / 2.0
        val dy = (response[i + w] - response[i - w]) / 2.0
        var ox = if (abs(dxx) > 1e-9) -dx / dxx else 0.0
        var oy = if (abs(dyy) > 1e-9) -dy / dyy else 0.0
        if (abs(ox) > 1.0) ox = 0.0
        if (abs(oy) > 1.0) oy = 0.0
        return Corner(xi + ox, yi + oy, c)
    }
}

/** A 2-D offset in image pixels. Package-internal — the public geometry type is [Vec3]. */
internal data class Vec2(val x: Double, val y: Double) {
    val length: Double get() = sqrt(x * x + y * y)
}

internal val DEBUG: Boolean get() = System.getProperty("lidarscan.calib.debug") != null

internal fun cosBetween(a: Vec2, b: Vec2): Double {
    val la = a.length
    val lb = b.length
    if (la <= 0.0 || lb <= 0.0) return 0.0
    return ((a.x * b.x + a.y * b.y) / (la * lb)).coerceIn(-1.0, 1.0)
}
