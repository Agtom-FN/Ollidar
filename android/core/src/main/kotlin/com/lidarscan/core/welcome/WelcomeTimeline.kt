package com.lidarscan.core.welcome

/**
 * ROUND 32 item 177 — **the approved storyboard, as arithmetic.**
 *
 * The owner approved an artifact (`ollidar-welcome-animation.html`) whose CSS
 * `@keyframes` blocks are the specification rather than an illustration of one.
 * This file is those blocks, transcribed: same stop positions (percentages of
 * the three seconds), same values, same per-segment easing, which is what CSS
 * does with `animation-timing-function` and therefore what a faithful port has
 * to do too.
 *
 * It is here, in `:core`, and not inside the composable, for one reason: a
 * clock-driven test can then pin the waypoints the owner actually asked for —
 * *exactly one 360° rotation*, *the light only after landing*, *the rings reach
 * the screen edges* — against the same numbers the screen draws, with no
 * emulator and no frame timing in the way. A Compose animation that is checked
 * only by looking at it is checked once.
 *
 * ## Units
 *
 * Positions and sizes are **master-art units**: the 1024 × 1024 canvas the
 * launcher icon was drawn on, which is the coordinate system the four cut
 * layers ([Art]) are anchored in. The composable maps that square onto the
 * screen once and everything else follows. The storyboard drew its stand-in
 * llama in a 340 × 420 stage at a different scale; [STORYBOARD_TO_MASTER] is
 * the conversion, measured between the two landmarks that exist in both
 * drawings — the puck's centre and the eye.
 *
 * Time is a fraction of [WelcomeAnimation.DURATION_MS], 0f‥1f, so every stop
 * below reads as the storyboard's own percentage divided by a hundred.
 */
object WelcomeTimeline {

    // ── the master art, and where its pieces sit ───────────────────────────

    /**
     * The four layers cut from the 1024 px master, with the anchors that
     * reassemble it (verified at 99.8 % against the original before any of
     * this was written).
     *
     * The body has its head **reconstructed** where the lidar sits and carries
     * **no** eye, so the puck and the eye can move independently of it. That
     * is the entire reason the art was cut at all.
     */
    object Art {
        /** The master canvas, both sides. */
        const val CANVAS: Float = 1024f

        /** `llama-body.png` — drawn to fill the whole canvas. */
        const val PUCK_LEFT: Float = 474f
        const val PUCK_TOP: Float = 176f
        const val PUCK_WIDTH: Float = 200f
        const val PUCK_HEIGHT: Float = 198f

        /** The puck's centre — the pivot for its flip, the fan's sweep and the rings. */
        const val PUCK_CENTER_X: Float = PUCK_LEFT + PUCK_WIDTH / 2f
        const val PUCK_CENTER_Y: Float = PUCK_TOP + PUCK_HEIGHT / 2f

        /** `fan-dots.png`, and where its top-left corner lands on the master. */
        const val FAN_LEFT: Float = 678f
        const val FAN_TOP: Float = 129f
        const val FAN_WIDTH: Float = 293f
        const val FAN_HEIGHT: Float = 352f

        /**
         * The cone's apex — just outside the fan bitmap, at the puck's emitter.
         * This is where the LED lights, so the light and the beam come from the
         * same point rather than from two guesses.
         */
        const val FAN_ORIGIN_X: Float = 670f
        const val FAN_ORIGIN_Y: Float = 248f

        /** `llama-eye.png`, by centre and radius. The sprite is 63 × 64. */
        const val EYE_CENTER_X: Float = 512f
        const val EYE_CENTER_Y: Float = 472.5f
        const val EYE_RADIUS: Float = 28.2f
        const val EYE_SPRITE_WIDTH: Float = 63f
        const val EYE_SPRITE_HEIGHT: Float = 64f
    }

    /**
     * Storyboard pixels → master units.
     *
     * Measured rather than guessed, between the only two landmarks both
     * drawings share: storyboard puck centre (206, 114) to eye (236, 158) is
     * 53.25 px; master puck centre (574, 275) to eye (512, 472.5) is 207.0
     * units. The storyboard's stand-in llama has different proportions from the
     * real art — its puck is drawn small — so anchoring on the puck's *size*
     * would have stretched every distance by a third.
     */
    const val STORYBOARD_TO_MASTER: Float = 3.887f

    private fun sb(storyboardPx: Float): Float = storyboardPx * STORYBOARD_TO_MASTER

    // ── easings ────────────────────────────────────────────────────────────

    /** `linear`. */
    private val LINEAR: (Float) -> Float = { it }

    /**
     * `steps(1)`, which in CSS means `steps(1, end)`: the value holds at the
     * segment's start and jumps at its end. Not a rounding of a ramp — the
     * storyboard uses it where an instant is wanted (the LED, the eye), and an
     * instant is what the owner approved.
     */
    private val HOLD: (Float) -> Float = { 0f }

    private fun cubicBezier(x1: Float, y1: Float, x2: Float, y2: Float): (Float) -> Float {
        // The standard CSS solve: invert x(t) by bisection, then evaluate y(t).
        // 24 halvings puts the residual well under a display frame's worth of
        // progress at any duration this app will ever use.
        fun axis(a: Float, b: Float, t: Float): Float {
            val u = 1f - t
            return 3f * u * u * t * a + 3f * u * t * t * b + t * t * t
        }
        return { x ->
            when {
                x <= 0f -> 0f
                x >= 1f -> 1f
                else -> {
                    var lo = 0f
                    var hi = 1f
                    var t = x
                    repeat(24) {
                        if (axis(x1, x2, t) < x) lo = t else hi = t
                        t = (lo + hi) * 0.5f
                    }
                    axis(y1, y2, t)
                }
            }
        }
    }

    private val EASE_OUT = cubicBezier(0f, 0f, 0.58f, 1f)
    private val EASE_IN_OUT = cubicBezier(0.42f, 0f, 0.58f, 1f)

    /** The storyboard's `.puck` easing: `cubic-bezier(.4,.1,.5,.9)`. */
    private val FLIP = cubicBezier(0.4f, 0.1f, 0.5f, 0.9f)

    /** The storyboard's `.spit` easing: `cubic-bezier(.2,.6,.4,1)`. */
    private val SPIT = cubicBezier(0.2f, 0.6f, 0.4f, 1f)

    // ── keyframe tracks ────────────────────────────────────────────────────

    /**
     * One CSS `@keyframes` property: stops in ascending position, interpolated
     * with [easing] applied to each **segment's** own progress.
     *
     * Per-segment is what CSS does and it is not a detail: the puck's flip has
     * four segments and a single easing stretched across all of them would put
     * the apex in the wrong place and the landing at the wrong speed.
     */
    private class Track(
        private val easing: (Float) -> Float,
        private vararg val stops: Pair<Float, Float>,
    ) {
        init {
            require(stops.size >= 2) { "a track needs at least two stops" }
        }

        fun at(t: Float): Float {
            if (t <= stops.first().first) return stops.first().second
            if (t >= stops.last().first) return stops.last().second
            for (i in 0 until stops.size - 1) {
                val (p0, v0) = stops[i]
                val (p1, v1) = stops[i + 1]
                if (t <= p1) {
                    if (p1 <= p0) return v1
                    return v0 + (v1 - v0) * easing((t - p0) / (p1 - p0))
                }
            }
            return stops.last().second
        }
    }

    // ══ ANIMATION A — the lidar flip ══════════════════════════════════════

    /**
     * The instant the light is allowed to exist — the storyboard's `a-led`
     * step, and the same instant the puck touches back down.
     *
     * Named because it is the one waypoint the owner stated twice: *the lidar
     * puck (DARK … till landing)* and *LED/light ignites* **after** the squash.
     * A test asserts nothing orange exists before it.
     */
    const val A_LIGHT_ON: Float = 0.58f

    /** The puck leaves the head here; before this it is simply sitting on it. */
    const val A_LAUNCH: Float = 0.12f

    /** The single revolution, in degrees. Exactly one — not `>=`, not 720. */
    const val A_FULL_TURN_DEG: Float = 360f

    /**
     * The storyboard's ring scale at t = 1. The composable divides by this and
     * multiplies by the distance from the puck to the screen's far corner, so
     * "the rings reach the window edges" is true on every screen instead of on
     * the one the storyboard was drawn at.
     */
    const val A_RING_FULL_SCALE: Float = 8.5f

    /** `.ring2`'s `animation-delay: 0.12s`, as a fraction of the three seconds. */
    const val A_RING2_DELAY: Float = 0.04f

    private val aOverlay = Track(LINEAR, 0f to 1f, 0.90f to 1f, 1f to 0f)

    private val aBodyBob = Track(
        EASE_IN_OUT,
        0f to 0f, 0.08f to 0f,
        0.12f to sb(5f), 0.16f to sb(-3f), 0.20f to 0f,
        0.56f to 0f, 0.60f to sb(3f), 0.66f to 0f, 1f to 0f,
    )

    private val aPuckDy = Track(
        FLIP,
        0f to 0f, A_LAUNCH to 0f,
        0.36f to sb(-78f), 0.52f to sb(-30f), A_LIGHT_ON to sb(3f),
        0.64f to 0f, 1f to 0f,
    )

    private val aPuckRotation = Track(
        FLIP,
        0f to 0f, A_LAUNCH to 0f,
        0.36f to 200f, 0.52f to 340f, A_LIGHT_ON to A_FULL_TURN_DEG,
        0.64f to A_FULL_TURN_DEG, 1f to A_FULL_TURN_DEG,
    )

    // The squash the item asks for and the storyboard could only hint at with
    // a 3 px overshoot: a compress on contact, a small rebound, settled by 66 %.
    // Volume-preserving-ish (x widens as y flattens), about the puck's FOOT.
    private val aSquashY = Track(
        EASE_OUT,
        0f to 1f, 0.57f to 1f, 0.595f to 0.80f, 0.625f to 1.09f, 0.66f to 1f, 1f to 1f,
    )
    private val aSquashX = Track(
        EASE_OUT,
        0f to 1f, 0.57f to 1f, 0.595f to 1.18f, 0.625f to 0.95f, 0.66f to 1f, 1f to 1f,
    )

    /**
     * How dark the puck is drawn while it is airborne. 1 = the dead grey the
     * storyboard's `.led` starts on, 0 = the art's own colour.
     *
     * The item's words: *DARK: desaturate/dim the sprite or overlay till
     * landing*. Ramped over 25 ms rather than stepped, so the ignition reads as
     * a light coming on rather than as a sprite swap — but it is still exactly
     * zero for every frame before [A_LIGHT_ON].
     */
    private val aPuckDim = Track(LINEAR, 0f to 1f, A_LIGHT_ON to 1f, 0.605f to 0f, 1f to 0f)

    private val aLedAlpha = Track(LINEAR, 0f to 0f, A_LIGHT_ON to 0f, 0.605f to 1f, 0.90f to 1f, 1f to 0f)

    private val aFanAlpha = Track(
        LINEAR,
        0f to 0f, A_LIGHT_ON to 0f, 0.64f to 1f, 0.80f to 1f, 0.90f to 0.8f, 1f to 0f,
    )
    private val aFanRotation = Track(
        LINEAR,
        0f to 0f, A_LIGHT_ON to 0f, 0.64f to 80f, 0.80f to 300f, 0.90f to 360f, 1f to 360f,
    )

    private val aRingScale = Track(
        EASE_OUT,
        0f to 0.2f, A_LIGHT_ON to 0.2f, 0.66f to 1f, 0.86f to 5.5f, 1f to A_RING_FULL_SCALE,
    )
    private val aRingAlpha = Track(
        EASE_OUT,
        0f to 0f, A_LIGHT_ON to 0f, 0.66f to 0.9f, 0.86f to 0.45f, 1f to 0f,
    )

    private val aEyeBob = Track(
        HOLD,
        0f to 0f, 0.14f to 0f, 0.16f to sb(-6f), 0.50f to sb(-6f), 0.52f to 0f, 1f to 0f,
    )

    /**
     * One frame of animation A. Every field is in master units or degrees; the
     * composable adds no timing of its own.
     */
    data class FrameA(
        /** The whole overlay, scrim included. Fades on the flash's tail. */
        val overlayAlpha: Float,
        /** The llama's crouch-and-toss, master units, positive = down. */
        val bodyBob: Float,
        val puckDy: Float,
        val puckRotationDeg: Float,
        val puckScaleX: Float,
        val puckScaleY: Float,
        /** 1 = the puck is drawn dead/dark, 0 = its own colour. */
        val puckDim: Float,
        val ledAlpha: Float,
        /** True once anything is allowed to be lit. See [A_LIGHT_ON]. */
        val lightOn: Boolean,
        val fanAlpha: Float,
        val fanRotationDeg: Float,
        val ring1Alpha: Float,
        /** Storyboard scale; divide by [A_RING_FULL_SCALE] for "fraction of the way to the corner". */
        val ring1Scale: Float,
        val ring2Alpha: Float,
        val ring2Scale: Float,
        /** The eye following the puck up and back, master units. */
        val eyeBob: Float,
    )

    /** @param t 0f‥1f — the fraction of [WelcomeAnimation.DURATION_MS] elapsed. */
    fun frameA(t: Float): FrameA {
        val c = t.coerceIn(0f, 1f)
        val ring2T = (c - A_RING2_DELAY).coerceIn(0f, 1f)
        return FrameA(
            overlayAlpha = aOverlay.at(c),
            bodyBob = aBodyBob.at(c),
            puckDy = aPuckDy.at(c),
            puckRotationDeg = aPuckRotation.at(c),
            puckScaleX = aSquashX.at(c),
            puckScaleY = aSquashY.at(c),
            puckDim = aPuckDim.at(c),
            ledAlpha = aLedAlpha.at(c),
            lightOn = c >= A_LIGHT_ON,
            fanAlpha = aFanAlpha.at(c),
            fanRotationDeg = aFanRotation.at(c),
            ring1Alpha = aRingAlpha.at(c),
            ring1Scale = aRingScale.at(c),
            // Ring 2 is ring 1 read 0.12 s late — one track, two clocks, so the
            // two rings can never drift apart in shape.
            ring2Alpha = if (c < A_RING2_DELAY) 0f else aRingAlpha.at(ring2T),
            ring2Scale = aRingScale.at(ring2T),
            eyeBob = aEyeBob.at(c),
        )
    }

    // ══ ANIMATION B — the spit ════════════════════════════════════════════

    /** The turn: the side pose is gone and the front pose is up by here. */
    const val B_TURN: Float = 0.31f

    /** The droplet leaves the mouth. */
    const val B_SPIT: Float = 0.57f

    /** It hits the glass. */
    const val B_SPLAT: Float = 0.88f

    /**
     * How far the droplet travels toward the viewer, master units, and
     * therefore where the splat sits: the mouth plus this. One number, two
     * users, so the drop can never land somewhere the splat is not.
     */
    val B_DROPLET_TRAVEL: Float = sb(130f)

    private val bOverlay = Track(LINEAR, 0f to 1f, 0.93f to 1f, 1f to 0f)

    // The storyboard swapped the poses with steps(1). The item asks for a
    // "crossfade/turn", so the two alphas cross over ~90 ms while the pair is
    // pinched flat in x — which is what makes it read as a head turning rather
    // than as one drawing replacing another.
    private val bSideAlpha = Track(LINEAR, 0f to 1f, 0.29f to 1f, 0.32f to 0f, 1f to 0f)
    private val bFrontAlpha = Track(LINEAR, 0f to 0f, 0.30f to 0f, 0.33f to 1f, 1f to 1f)
    private val bTurnScaleX = Track(
        EASE_IN_OUT,
        0f to 1f, 0.26f to 1f, B_TURN to 0.20f, 0.37f to 1f, 1f to 1f,
    )

    private val bTwinkleAlpha = Track(
        EASE_IN_OUT,
        0f to 0f, 0.04f to 0f, 0.10f to 1f, 0.16f to 0.7f, 0.22f to 1f, 0.29f to 0f, 1f to 0f,
    )
    private val bTwinkleScale = Track(
        EASE_IN_OUT,
        0f to 0.2f, 0.04f to 0.2f, 0.10f to 1.15f, 0.16f to 0.8f, 0.22f to 1.1f, 0.29f to 0.3f, 1f to 0.3f,
    )
    private val bTwinkleRotation = Track(
        EASE_IN_OUT,
        0f to 0f, 0.04f to 0f, 0.10f to 20f, 0.16f to 40f, 0.22f to 60f, 0.29f to 80f, 1f to 80f,
    )

    private val bCheekScaleX = Track(
        EASE_IN_OUT,
        0f to 1f, 0.38f to 1f, 0.46f to 1.06f, 0.52f to 1.07f, 0.56f to 0.99f, 0.60f to 1f, 1f to 1f,
    )
    private val bCheekScaleY = Track(
        EASE_IN_OUT,
        0f to 1f, 0.38f to 1f, 0.46f to 1.10f, 0.52f to 1.12f, 0.56f to 0.98f, 0.60f to 1f, 1f to 1f,
    )

    private val bDropletAlpha = Track(
        SPIT,
        0f to 0f, 0.54f to 0f, B_SPIT to 1f, 0.82f to 1f, B_SPLAT to 0f, 1f to 0f,
    )
    private val bDropletDy = Track(
        SPIT,
        0f to 0f, 0.54f to 0f,
        B_SPIT to sb(6f), 0.70f to sb(60f), 0.82f to sb(110f), B_SPLAT to B_DROPLET_TRAVEL,
        1f to B_DROPLET_TRAVEL,
    )
    private val bDropletScale = Track(
        SPIT,
        0f to 0.15f, 0.54f to 0.15f,
        B_SPIT to 0.3f, 0.70f to 1.6f, 0.82f to 3.4f, B_SPLAT to 4.2f, 1f to 4.2f,
    )

    private val bSplatAlpha = Track(EASE_OUT, 0f to 0f, 0.82f to 0f, B_SPLAT to 0.85f, 1f to 0.7f)
    private val bSplatScale = Track(EASE_OUT, 0f to 0.2f, 0.82f to 0.2f, B_SPLAT to 1.15f, 1f to 1.3f)

    private val bGrinAlpha = Track(HOLD, 0f to 0f, 0.86f to 0f, 0.87f to 1f, 1f to 1f)

    /** One frame of animation B. */
    data class FrameB(
        val overlayAlpha: Float,
        val sideAlpha: Float,
        val frontAlpha: Float,
        /** Both poses pinch in x through the turn. */
        val turnScaleX: Float,
        val twinkleAlpha: Float,
        val twinkleScale: Float,
        val twinkleRotationDeg: Float,
        val cheekScaleX: Float,
        val cheekScaleY: Float,
        val dropletAlpha: Float,
        /** Master units down from the mouth — the "toward the viewer" half of it. */
        val dropletDy: Float,
        /** …and the scale-up half, ~4× by the time it arrives. */
        val dropletScale: Float,
        val splatAlpha: Float,
        val splatScale: Float,
        val grinAlpha: Float,
    )

    /** @param t 0f‥1f — the fraction of [WelcomeAnimation.DURATION_MS] elapsed. */
    fun frameB(t: Float): FrameB {
        val c = t.coerceIn(0f, 1f)
        return FrameB(
            overlayAlpha = bOverlay.at(c),
            sideAlpha = bSideAlpha.at(c),
            frontAlpha = bFrontAlpha.at(c),
            turnScaleX = bTurnScaleX.at(c),
            twinkleAlpha = bTwinkleAlpha.at(c),
            twinkleScale = bTwinkleScale.at(c),
            twinkleRotationDeg = bTwinkleRotation.at(c),
            cheekScaleX = bCheekScaleX.at(c),
            cheekScaleY = bCheekScaleY.at(c),
            dropletAlpha = bDropletAlpha.at(c),
            dropletDy = bDropletDy.at(c),
            dropletScale = bDropletScale.at(c),
            splatAlpha = bSplatAlpha.at(c),
            splatScale = bSplatScale.at(c),
            grinAlpha = bGrinAlpha.at(c),
        )
    }
}
