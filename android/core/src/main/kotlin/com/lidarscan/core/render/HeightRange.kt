package com.lidarscan.core.render

/**
 * ROUND 28 item 154: the height axis, and the arithmetic that turns a pair of
 * cloud bounds into the `valueMin`/`valueMax` the point shader normalises
 * against.
 *
 * **What it was.** `points.mat` normalised `world.z` and clipped on `world.z`,
 * and `PointCloudRenderer` fed it `combinedBounds*[2]` to match. All three
 * agreed with each other and all three were wrong: the runtime frame is
 * ARCore's, which is **Y-up** — the derivation is already written out in
 * [FollowCamera]'s config site in `PointCloudRenderer`, which committed to
 * [UpAxis.Y_UP] and then explicitly flagged this auto-range as the one place
 * that had not followed.
 *
 * **What broke.** Colour was being auto-ranged over a *horizontal* axis. A
 * handheld scan walked at one height has a large x/z footprint and a small y
 * span, so the review shot the owner sent back was a flat dark-indigo cloud:
 * z spanned metres, the Turbo ramp spread itself over the room's depth, and
 * with the camera framing the room the visible points all landed in the same
 * few texels near `t = 0`. The failure mode is silent — nothing divides by
 * zero, nothing throws, the cloud simply comes out one colour.
 *
 * **Why this is the answer.** The axis is stated once, here, as a named
 * constant every site has to spell ([AXIS]), rather than as a bare `[2]`
 * repeated in three files that can each be fixed independently and wrongly.
 * The normalisation and its degenerate guard come with it, because "which
 * component is height" and "what happens when that component has no span" are
 * the same question asked twice.
 */
object HeightRange {

    /**
     * The index of the world-up component in an `(x, y, z)` bounds triple.
     *
     * **1, not 2.** See the class header, and `PointCloudRenderer`'s
     * `followCamera` field for the full derivation of why the renderer's frame
     * is Y-up: `CaptureArController.publishPose` pushes ARCore's `camera.pose`
     * verbatim into the engine, so every `PointVertex` is in ARCore's Y-up
     * world frame. The engine's own +z-up convention lives only in
     * `engine/tests/test_pushbroom.cpp`'s synthetic fixture and is not a frame
     * any device produces.
     */
    const val AXIS = 1

    /**
     * The smallest height span worth dividing by, in metres.
     *
     * 1 mm, the same floor `CloudThumbnail.normalise` uses for its own z span
     * — deliberately the same number, because a cloud that is degenerate for
     * the thumbnail and not for the viewport (or the reverse) would be a
     * project card and a Review screen disagreeing about whether a scan is
     * flat.
     */
    const val EPSILON_M = 1e-3f

    /**
     * Half the range handed back when the real span is degenerate.
     *
     * A collapsed cloud gets a range *centred on itself* rather than a range
     * starting at itself: every point then normalises to exactly `t = 0.5` and
     * the cloud draws in one honest mid-ramp colour. Clamping the span to
     * [EPSILON_M] instead — the obvious fix, and what `CloudThumbnail` does
     * because a thumbnail has nowhere better to go — would divide a millimetre
     * of sensor noise across the whole ramp and paint a flat wall in confetti.
     */
    const val DEGENERATE_HALF_SPAN_M = 0.5f

    /** A resolved `[min, max]` pair, ready to hand to the shader. */
    data class Range(val min: Float, val max: Float) {
        val span: Float get() = max - min
    }

    /**
     * Resolves the auto-range for a cloud whose height bounds are [min]..[max],
     * falling back to [fallback] when the bounds are not usable at all.
     *
     * Three cases, in the order they are checked:
     *  1. **Not finite.** Either bound NaN or infinite — a page that has not
     *     had its bounds computed, or a pose that went NaN mid-walk. There is
     *     no data to range over, so the caller's own range (the manual/profile
     *     default) is returned unchanged. Never divided by.
     *  2. **Degenerate.** A real, finite, but sub-[EPSILON_M] span: a
     *     single-plane cloud, or the very first page of a capture. Answered
     *     with a [DEGENERATE_HALF_SPAN_M] window centred on the data — mid
     *     ramp, one colour, no division by ~0.
     *  3. **Normal.** Returned as-is, with the pair ordered.
     */
    fun resolve(min: Float, max: Float, fallback: Range): Range {
        if (!min.isFinite() || !max.isFinite()) return fallback
        val lo = minOf(min, max)
        val hi = maxOf(min, max)
        if (hi - lo < EPSILON_M) {
            val centre = (lo + hi) * 0.5f
            return Range(centre - DEGENERATE_HALF_SPAN_M, centre + DEGENERATE_HALF_SPAN_M)
        }
        return Range(lo, hi)
    }

    /**
     * The JVM twin of `points.mat`'s normalisation line
     * (`clamp((value - valueMin) / span, 0, 1)`), carrying the same guards.
     *
     * It exists so the ramp assertion in `HeightRangeTest` can run on every
     * build without a GPU: the shader's arithmetic is four operations and
     * re-stating them here costs nothing next to the alternative, which is
     * having no build-time evidence at all that a 2 m cloud produces more than
     * one colour. The GLSL side keeps its own `max(span, 1e-6)` — that is a
     * last-ditch divide guard, not this policy.
     */
    fun normalise(value: Float, min: Float, max: Float): Float {
        if (!value.isFinite() || !min.isFinite() || !max.isFinite()) return 0.5f
        val span = max - min
        if (span < EPSILON_M) return 0.5f
        return ((value - min) / span).coerceIn(0f, 1f)
    }

    /**
     * How far the live bounds must drift before the shader's range is worth
     * re-uploading — as a fraction of the range already applied.
     *
     * 2 %: below that the colour shift is under one texel of a 256-entry LUT,
     * so re-applying would be work with no pixel to show for it.
     */
    const val REAPPLY_FRACTION = 0.02f

    /**
     * True when [applied] no longer describes [current] closely enough to keep.
     *
     * **Why the trigger is the bounds and not the point count.** A live cloud
     * grows, and a height range computed once when the first page landed is
     * wrong by the second frame — that was the other half of item 154. The
     * obvious cheap threshold is "re-apply when the point count grows past a
     * fraction", but point count is a *proxy* for the thing that actually
     * matters and a bad one in both directions: ten thousand points added
     * along a corridor at one height change no colour at all, while a single
     * page from a staircase changes every colour in the cloud. Comparing the
     * two floats the bounds accumulator already maintains is cheaper than
     * reading a count *and* exactly right, so there is no reason to take the
     * proxy.
     *
     * Scale-relative rather than absolute, because a 5 cm drift is nothing in
     * a stairwell and is the whole ramp on a desktop-sized scan. The applied
     * span is the scale reference; [EPSILON_M] floors it so a degenerate
     * applied range cannot make the tolerance zero and re-apply every frame.
     */
    fun needsReapply(applied: Range, current: Range): Boolean {
        if (!current.min.isFinite() || !current.max.isFinite()) return false
        val tolerance = maxOf(applied.span, EPSILON_M) * REAPPLY_FRACTION
        return kotlin.math.abs(current.min - applied.min) > tolerance ||
            kotlin.math.abs(current.max - applied.max) > tolerance
    }
}
