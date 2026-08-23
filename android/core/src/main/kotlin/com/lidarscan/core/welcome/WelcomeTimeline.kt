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

        /**
         * The puck's centre, and the line it stands on.
         *
         * ROUND 35 item 184: the centre is **no longer** the pivot for
         * anything. It is kept because it is a fact about the art — the
         * sprite's middle — and because [STORYBOARD_TO_MASTER] is derived from
         * it. Everything that used to spin, sweep or expand about it now uses
         * [EMIT_X] / [EMIT_Y].
         */
        const val PUCK_CENTER_X: Float = PUCK_LEFT + PUCK_WIDTH / 2f
        const val PUCK_CENTER_Y: Float = PUCK_TOP + PUCK_HEIGHT / 2f

        /** The puck's contact line — where it stands, and where a squash is felt. */
        const val PUCK_FOOT_Y: Float = PUCK_TOP + PUCK_HEIGHT

        /**
         * ROUND 35 item 184 — **THE EMIT POINT. There is exactly one.**
         *
         * > *"the lidar light, the spot and spin should align on the same
         * > point."* — the owner, on the round-34 footage.
         *
         * The lidar's optical centre: `layers.json`'s `fanOrigin` anchor, which
         * is where the LED has always been drawn and is also the fan bitmap's
         * own cone apex. Round 34 drew four things about **four** centres — the
         * LED here, the fan's sweep and the rings about [PUCK_CENTER_X] /
         * [PUCK_CENTER_Y] a hundred units away, and the landing squash about
         * the puck's foot — which is why the cone orbited instead of turning
         * and why the pulse left from a point the light was not at.
         *
         * The light does not move; the other three come to it. Every user of
         * this point is listed in `WelcomeOverlay.drawLidarFlip`, and the
         * concentricity is photographed at the flash rather than argued.
         */
        const val EMIT_X: Float = 670f
        const val EMIT_Y: Float = 248f

        /** `fan-dots.png`, and where its top-left corner lands on the master. */
        const val FAN_LEFT: Float = 678f
        const val FAN_TOP: Float = 129f
        const val FAN_WIDTH: Float = 293f
        const val FAN_HEIGHT: Float = 352f

        /**
         * The cone's apex — just outside the fan bitmap, at the puck's emitter.
         *
         * It **is** [EMIT_X] / [EMIT_Y], stated twice on purpose: this is the
         * fan bitmap's own geometry (the point its dots were drawn to radiate
         * from) and the emit point is the app's one pivot, and round 35 asserts
         * they are equal rather than assuming it. If the fan art is ever
         * re-cut, this pair moves and the emit point does not have to.
         */
        const val FAN_ORIGIN_X: Float = EMIT_X
        const val FAN_ORIGIN_Y: Float = EMIT_Y

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

    /**
     * ROUND 35 item 187(a) — **the overlay does not fade any more.**
     *
     * Round 32 ran this from 1 to 0 over the last tenth of the film, so A
     * dissolved into the app while the flash was still going off. The owner
     * asked for the film to finish and then be held; the dismissal is now the
     * caller's, after [WelcomeAnimation.HOLD_MS], and this track's only job is
     * to keep the page opaque until then.
     */
    private val aOverlay = Track(LINEAR, 0f to 1f, 1f to 1f)

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

    /**
     * ROUND 35 item 187(b): it lights at the landing and **stays lit** — the
     * resting pose the hold is held on is *the llama with the lidar seated and
     * the LED on*, and a light that fades out over the last tenth of the film
     * would leave a dead instrument on screen for the whole second.
     */
    private val aLedAlpha = Track(LINEAR, 0f to 0f, A_LIGHT_ON to 0f, 0.605f to 1f, 1f to 1f)

    // The sweep is a thing that HAPPENS, so it does end — one revolution and
    // out, finished a little before the film is, which is what leaves the
    // resting pose clean for the hold.
    private val aFanAlpha = Track(
        LINEAR,
        0f to 0f, A_LIGHT_ON to 0f, 0.64f to 1f, 0.80f to 1f, 0.94f to 0.6f, 0.98f to 0f, 1f to 0f,
    )
    private val aFanRotation = Track(
        LINEAR,
        0f to 0f, A_LIGHT_ON to 0f, 0.64f to 80f, 0.80f to 300f, 0.90f to 360f, 1f to 360f,
    )

    private val aRingScale = Track(
        EASE_OUT,
        0f to 0.2f, A_LIGHT_ON to 0.2f, 0.66f to 1f, 0.86f to 5.5f, 1f to A_RING_FULL_SCALE,
    )
    // ROUND 35 item 187(b): the rings are out and gone by 96 %, not at 100 %.
    // Ring 2 is this track read [A_RING2_DELAY] late, so a fade that only
    // reached zero at the very end left the second ring faintly orange on the
    // last frame — and that frame is now held for a whole second.
    private val aRingAlpha = Track(
        EASE_OUT,
        0f to 0f, A_LIGHT_ON to 0f, 0.66f to 0.9f, 0.86f to 0.45f, 0.96f to 0f, 1f to 0f,
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


    // ══ ANIMATION B — THE UNICORN EGG ═════════════════════════════════════
    //
    // ROUND 37 item 189 — **the approved storyboard, as arithmetic**, for the
    // second time in this file.
    //
    // > *"rainbow ribbons circle the llama in quick revolutions, THE LIDAR
    // > ITSELF grows into the horn, then the transformation completes and the
    // > unicorn gallops off-screen. 3.0 seconds."* — the owner, handing over
    // > `ollidar-unicorn-egg.html` v2 on 2026-08-23.
    //
    // Round 36's B — the reference video's two sections, the jaw grind and the
    // covered lens — is **gone entire** (item 188, superseded). It shipped
    // nowhere. This is not a correction of it; it is a different film.
    //
    // The storyboard's `@keyframes` blocks are the specification, exactly as
    // round 32's were for A, so the stops below read as its own percentages
    // divided by a hundred. Where the owner's covering note names a window that
    // the CSS does not quite land in, **the note wins and the CSS's shape is
    // kept** — every such case is marked below, and there are four of them.
    //
    // ## The mapping, beat by beat
    //
    // | storyboard | ours | file |
    // |---|---|---|
    // | `.orbit` — `orbitspin 0.65s linear 2` | 0.00 ‥ 1.30 s — two orbits each | [ribbonAt], [B_RIBBONS] |
    // | `.flash` 44 → 49 → 56 % | 1.32 ‥ 1.65 s, peak 1.47 s | [bFlash] |
    // | `.spark` 46 → 52 → 62 % ×3 | 1.38 ‥ 1.80 s, three of them | [sparkleAt] |
    // | `.puck` `puckgone` 45 → 46 % | 1.30 ‥ 1.43 s, stretching as it goes | [bPuckStretch] |
    // | `.hornwrap` `horngrow` 45 → 52 → 56 % | 1.30 ‥ 1.50 ‥ 1.60 s, overshoot and settle | [bHornGrow] |
    // | `bodycolor` `steps(1)` at 53 % | 1.37 ‥ 1.60 s, a front sweeping down | [bFleeceFlood] |
    // | `.mane` 54 → 60 % | 1.41 ‥ 1.60 s | [bManeAlpha] |
    // | `act` 60 → 66 → 72 % | 1.60 crouch, 1.80 rear, 2.00 down | [bBodyDy], [bBodyRotDeg] |
    // | `act` 72 → 100 % | 2.00 ‥ 2.79 s, and gone | [bGallopX], [gallopBob] |
    // | `.trail` 70 → 80 → 100 % | 2.07 ‥ 2.76 s | [bTrailAlpha] |
    // | `.dust` 74 → 82 → 100 % | 2.00 ‥ 2.70 s | [bDustAlpha] |
    //
    // Every number is fixed rather than random, for the reason every constant
    // in this file is fixed: the film has to be the same film every time.

    /** **The ribbons are done. 1.30 s.** */
    const val B_RIBBONS_END: Float = 0.4333f

    /**
     * How many times each ribbon goes round in that time. **Two**, the
     * storyboard's `animation: orbitspin 0.65s linear 2`, and a test counts
     * them rather than trusting the arithmetic that produces them.
     */
    const val B_ORBITS: Int = 2

    /**
     * **The transformation begins. 1.30 s** — the same instant the ribbons
     * finish, which is what makes them read as the cause of it.
     */
    const val B_TRANSFORM: Float = 0.4333f

    /** **…and the horn is grown, the fleece flooded, the mane in. 1.60 s.** */
    const val B_HORN_DONE: Float = 0.5333f

    /** **The rear-up is at its peak. 1.80 s.** */
    const val B_REAR_PEAK: Float = 0.60f

    /** **The front hooves come down and it goes. 2.00 s.** */
    const val B_LAUNCH: Float = 0.6667f

    /**
     * **Off the right edge. 2.85 s.**
     *
     * The storyboard's `.actor` only reaches `translateX(430px)` at 100 %, so
     * its stage is still emptying on the last frame. The owner's note is
     * explicit that it should not be — *"empty stage ~0.2 s, then dismiss"*.
     * Deviation 1 of 4.
     *
     * **The two numbers this beat has, and why they differ.** This constant is
     * when the animal's *travel* completes, and [exitTravelMasterUnits] measures
     * that to the master canvas's own left edge — a definition that cannot come
     * out wrong if the sprite is ever re-cut. The animal's leftmost *drawn*
     * pixel is about seventy units in from there, so on the recording the screen
     * is bare from roughly 2.74 s: 0.15 s by this constant, and about a quarter
     * of a second to the eye, which is the beat the owner asked for. The first
     * cut had it at 0.93 and photographed with more than a third of a second of
     * nothing at the end.
     */
    const val B_EXIT: Float = 0.95f

    /** Gallop bounces on the way out. The item asks for 2–3. */
    const val B_GALLOP_BOUNCES: Int = 3

    private val bOverlay = Track(LINEAR, 0f to 1f, B_EXIT to 1f, 1f to 0f)

    // ── THE RIBBONS ────────────────────────────────────────────────────────
    //
    // The storyboard could only draw a bar swinging round a pivot, because CSS
    // has one `rotate()` and no depth: its ribbon is always on top of the
    // llama, and the caption admits it — *"elliptical orbit passing IN FRONT of
    // and BEHIND the llama — layer order must swap at the orbit's front/back
    // crossings, which the storyboard's flat version could not show but the
    // build must do."*
    //
    // So a ribbon here is not a bar. It is an **arc of its own orbit** — a band
    // sampled along the ellipse, every sample carrying its own depth — and the
    // composable draws the samples behind the animal before it draws the
    // animal, and the rest after. The swap is therefore not an event that has
    // to be timed: it is what sorting by depth does, twice a revolution, for
    // free, and it cannot go out of step with the motion because it *is* the
    // motion.

    /**
     * One ribbon's orbit, in master units, plus where in the cycle it starts.
     *
     * The ellipse is the same trick a hand-drawn orbit is: wide in x, shallow
     * in y, and tilted a few degrees so it is a ring seen from above rather
     * than a rail. [b] is a quarter of [a] — flatter than that and the ribbon
     * slides sideways without ever going round anything.
     */
    class RibbonOrbit(
        /** The ellipse's centre. */
        val cx: Float,
        val cy: Float,
        /** Its half-width and half-height. */
        val a: Float,
        val b: Float,
        /** …and the roll of the whole ring, degrees. */
        val tiltDeg: Float,
        /** Where this ribbon starts, in revolutions. */
        val phase: Float,
        /** Its own start colour along the rainbow, 0‥1. */
        val hue: Float,
    )

    /**
     * **Two** ribbons, phase-offset.
     *
     * The storyboard offsets the second with `animation-delay: 0.18s`, which
     * would leave it still going round at 1.48 s — a fifth of a second into the
     * transformation, with the flash already up. The owner's note says both are
     * inside the first 1.3 s, so the delay is spent as a **phase** instead:
     * 0.18 s of a 0.65 s revolution is 0.277 of a turn, and that is what the
     * second one starts at. Same look, and both are gone when the flash lands.
     * Deviation 2 of 4.
     *
     * Their ellipses are deliberately not the same ellipse. Two identical rings
     * a third of a turn apart photograph as one ring with two beads on it.
     */
    val B_RIBBONS: List<RibbonOrbit> = listOf(
        RibbonOrbit(cx = 500f, cy = 566f, a = 436f, b = 134f, tiltDeg = 8f, phase = 0f, hue = 0f),
        RibbonOrbit(cx = 496f, cy = 508f, a = 458f, b = 108f, tiltDeg = -10f, phase = 0.277f, hue = 0.34f),
    )

    /**
     * How many samples a ribbon is drawn from.
     *
     * They are strokes rather than one filled path, and that is what makes the
     * depth sort possible at all: a single path has one z, and a band that
     * wraps round an animal has two. Twenty-eight is where the joints stop
     * being visible at a phone's pixel density.
     */
    const val B_RIBBON_SEGMENTS: Int = 28

    /** How much of the orbit the band spans, in revolutions. */
    const val B_RIBBON_ARC: Float = 0.46f

    /** Its half-width at the ellipse's own scale, master units. */
    const val B_RIBBON_HALF_WIDTH: Float = 27f

    /**
     * One sample of a ribbon, in master units.
     *
     * [depth] is the only field that is not a coordinate: positive is **in
     * front of** the llama and negative is **behind** it, and the composable
     * draws the two runs on either side of the animal. It is `sin` of the
     * orbit's angle, so it changes sign exactly twice a revolution — at the
     * ellipse's ends, which are the crossings the item names.
     */
    data class RibbonSample(
        val x: Float,
        val y: Float,
        val halfWidth: Float,
        val depth: Float,
        val alpha: Float,
        /** Where along the rainbow this sample is, 0‥1, wrapped. */
        val hue: Float,
    )

    /**
     * How far round ribbon [index] has gone at [t], **in revolutions**,
     * including its phase.
     *
     * Public because it is the thing item 189 counts: *two full orbits each*.
     * A test that counted peaks in a coordinate would be counting the ellipse,
     * not the orbit.
     */
    fun ribbonTurns(index: Int, t: Float): Float {
        val orbit = B_RIBBONS[index]
        val p = (t / B_RIBBONS_END).coerceIn(0f, 1f)
        return orbit.phase + p * B_ORBITS
    }

    /**
     * The whole ribbon [index] at [t] — or an empty list once they are gone.
     *
     * The band trails **behind** the head: sample 0 is the head, sample
     * `B_RIBBON_SEGMENTS − 1` is the tail, and the tail is thinner and fainter,
     * which is what gives a rigid ring the look of something with fabric in it.
     */
    fun ribbonAt(index: Int, t: Float): List<RibbonSample> {
        if (t > B_RIBBONS_END) return emptyList()
        val orbit = B_RIBBONS[index]
        val fade = ribbonFade(t)
        if (fade <= 0f) return emptyList()
        val head = ribbonTurns(index, t)
        val rad = orbit.tiltDeg * Math.PI.toFloat() / 180f
        val cs = kotlin.math.cos(rad)
        val sn = kotlin.math.sin(rad)
        return (0 until B_RIBBON_SEGMENTS).map { i ->
            val along = i.toFloat() / (B_RIBBON_SEGMENTS - 1)
            val turns = head - along * B_RIBBON_ARC
            val theta = turns * 2.0 * Math.PI
            val ex = (orbit.a * kotlin.math.cos(theta)).toFloat()
            val ey = (orbit.b * kotlin.math.sin(theta)).toFloat()
            // The depth is taken BEFORE the tilt, from the untilted ellipse:
            // rolling the ring a few degrees changes where a sample is drawn,
            // not which side of the animal it is on.
            val depth = kotlin.math.sin(theta).toFloat()
            // Perspective, as the one cue a flat drawing can afford: the near
            // half of the ring is wider than the far half.
            val width = B_RIBBON_HALF_WIDTH * (0.62f + 0.44f * depth)
            // The tail thins and fades. Squared, so most of the band is at full
            // weight and only the last few samples are ghosts.
            val taper = 1f - along * along
            RibbonSample(
                x = orbit.cx + ex * cs - ey * sn,
                y = orbit.cy + ex * sn + ey * cs,
                halfWidth = width * (0.45f + 0.55f * taper),
                depth = depth,
                alpha = fade * (0.30f + 0.70f * taper),
                hue = wrap01(orbit.hue + turns * 0.9f),
            )
        }
    }

    /**
     * The ribbons' own opacity: on quickly, off on the last stride into the
     * flash.
     *
     * The storyboard's `orbitspin` is `opacity:1` for 99 % of its two turns and
     * 0 at the very end, which in CSS is a ribbon that vanishes between two
     * frames. A tenth of a revolution of fade is what stops that reading as a
     * dropped frame, and it is finished before [B_TRANSFORM] either way.
     */
    private fun ribbonFade(t: Float): Float {
        if (t < 0f || t > B_RIBBONS_END) return 0f
        val inRamp = (t / (B_RIBBONS_END * 0.06f)).coerceIn(0f, 1f)
        val outRamp = ((B_RIBBONS_END - t) / (B_RIBBONS_END * 0.09f)).coerceIn(0f, 1f)
        return inRamp * outRamp
    }

    private fun wrap01(v: Float): Float = v - kotlin.math.floor(v)

    /**
     * ROUND 37 item 189 — **the llama watches.**
     *
     * The item makes the follow *"a plus if cheap"*, and it is cheap: the eye
     * is already a separate sprite that animation A slides about, so it costs a
     * translate. It looks at the **near** ribbon — the one in front — because
     * an animal tracking something behind its own head is a different and much
     * more alarming drawing.
     *
     * @return the eye's offset from its anchor, master units, x then y.
     */
    fun eyeFollow(t: Float): Pair<Float, Float> {
        if (t > B_RIBBONS_END) return 0f to 0f
        val fade = ribbonFade(t)
        if (fade <= 0f) return 0f to 0f
        // Whichever of the two is in front right now; ties go to the first.
        val heads = B_RIBBONS.indices.map { ribbonAt(it, t).firstOrNull() }
        val near = heads.filterNotNull().maxByOrNull { it.depth } ?: return 0f to 0f
        val dx = near.x - Art.EYE_CENTER_X
        val dy = near.y - Art.EYE_CENTER_Y
        val span = kotlin.math.hypot(dx.toDouble(), dy.toDouble()).toFloat().coerceAtLeast(1f)
        return (dx / span * EYE_FOLLOW_X * fade) to (dy / span * EYE_FOLLOW_Y * fade)
    }

    /**
     * How far the eye travels. Small: the face patch is about 250 units across
     * and the pupil is 56, so anything past twenty units is a llama with its
     * eye where its cheek should be.
     */
    private const val EYE_FOLLOW_X: Float = 15f
    private const val EYE_FOLLOW_Y: Float = 10f

    /**
     * …and the head goes with it, barely — the whole animal rolls about its own
     * base by this much, in phase with the near ribbon's x.
     *
     * Two degrees. It is under the threshold at which anybody could say what
     * changed, which is the correct size for a thing described as *subtle*.
     */
    const val B_WATCH_TILT_DEG: Float = 2f

    // ── THE TRANSFORMATION ─────────────────────────────────────────────────

    /**
     * ROUND 37 item 189 — **the puck stretches into the horn.**
     *
     * > *"THE LIDAR PUCK SPRITE STRETCHES INTO THE SPIRAL HORN — a morph
     * > anchored at the puck's seat."*
     *
     * A morph and not a swap, and the difference is one shared anchor: both
     * shapes are scaled about **the puck's own seat** — the middle of its
     * contact line, [Art.PUCK_CENTER_X] × [Art.PUCK_FOOT_Y] — so neither of
     * them ever leaves the llama's head while the other arrives. The puck grows
     * in y and narrows in x as it goes ([bPuckStretch], [bPuckNarrow]) so that
     * the shape it is fading out of is already horn-proportioned; the horn
     * grows on the same line from a sixth of its height.
     *
     * The storyboard's `puckgone` is `steps(1)` at 46 % — an instant swap,
     * which is what CSS can do with two `<svg>`s and no morph target. Its
     * caption is the part that matters and it says stretch. Deviation 3 of 4.
     */
    private val bPuckAlpha = Track(
        LINEAR,
        0f to 1f, B_TRANSFORM to 1f, 0.4750f to 0f, 1f to 0f,
    )

    private val bPuckStretch = Track(
        EASE_IN_OUT,
        0f to 1f, B_TRANSFORM to 1f, 0.4750f to 2.45f, 1f to 2.45f,
    )

    private val bPuckNarrow = Track(
        EASE_IN_OUT,
        0f to 1f, B_TRANSFORM to 1f, 0.4750f to 0.42f, 1f to 0.42f,
    )

    /** The storyboard's `.hornwrap` easing: `cubic-bezier(.3,1.4,.5,1)`. */
    private val HORN = cubicBezier(0.3f, 1.4f, 0.5f, 1f)

    private val bHornAlpha = Track(
        LINEAR,
        0f to 0f, B_TRANSFORM to 0f, 0.4900f to 1f, 1f to 1f,
    )

    /**
     * The horn's height, as a fraction of its own.
     *
     * The storyboard: `0%,45% { scaleY(0.15) } 52% { scaleY(1.12) } 56%,100%
     * { scale(1) }` — grow, overshoot by an eighth, settle. The overshoot is
     * the whole character of it and it is the reason this track exists rather
     * than a ramp: a horn that arrives at its length and stops has been placed
     * there, and a horn that goes past and comes back has **grown**.
     *
     * The owner's note puts the transformation in 1.3 ‥ 1.6 s where the CSS
     * settles at 56 % (1.68 s), so the settle is pulled to 1.60 s and the
     * overshoot with it. Deviation 4 of 4.
     */
    private val bHornGrow = Track(
        HORN,
        0f to 0.15f, B_TRANSFORM to 0.15f, 0.5000f to 1.12f, B_HORN_DONE to 1f, 1f to 1f,
    )

    /** …and its width, which the storyboard runs from 0.6 to 1 over the same span. */
    private val bHornWidth = Track(
        EASE_OUT,
        0f to 0.60f, B_TRANSFORM to 0.60f, 0.5000f to 1f, 1f to 1f,
    )

    /**
     * **The fleece floods rainbow**, 0 = the art's own cream, 1 = flooded to
     * the last hair.
     *
     * The storyboard is `steps(1)`: `#fff` until 53 % and `url(#rain)` after
     * it, which is a fill swap because a CSS fill cannot be half applied. The
     * owner's word is **floods**, and a flood has a front. It comes **down**
     * from the crown — the magic is arriving from the horn, so the colour has
     * to leave from there — and the composable spends this number as the
     * position of that front rather than as an opacity, so the llama is
     * two-tone for a fifth of a second on the way.
     */
    private val bFleeceFlood = Track(
        EASE_OUT,
        0f to 0f, 0.4550f to 0f, B_HORN_DONE to 1f, 1f to 1f,
    )

    /**
     * **The mane**, along the neck's back edge.
     *
     * The storyboard runs it 54 → 60 % (1.62 ‥ 1.80 s), which is *after* the
     * window its own caption puts the transformation in. The caption wins: it
     * arrives with the horn and the flood, and all three are done at 1.60 s
     * when the animal rears.
     */
    private val bManeAlpha = Track(
        EASE_OUT,
        0f to 0f, 0.4700f to 0f, B_HORN_DONE to 1f, 1f to 1f,
    )

    /** The storyboard's `.flash`: nothing, then everything, then nothing. */
    private val bFlash = Track(
        EASE_OUT,
        0f to 0f, 0.44f to 0f, 0.49f to 1f, 0.55f to 0f, 1f to 0f,
    )

    /**
     * One four-point sparkle: where it sits, master units, and how late it is.
     *
     * **Three**, which is what item 189 asks for, at the storyboard's own
     * offsets — `0`, `0.08s`, `0.16s` — converted to film time. They are placed
     * around the horn rather than around the animal: a sparkle by the llama's
     * feet is weather, and a sparkle by the horn is what just happened.
     */
    class Sparkle(val x: Float, val y: Float, val size: Float, val delay: Float)

    val B_SPARKLES: List<Sparkle> = listOf(
        Sparkle(x = 372f, y = 196f, size = 66f, delay = 0f),
        Sparkle(x = 762f, y = 300f, size = 58f, delay = 0.0267f),
        Sparkle(x = 596f, y = 40f, size = 74f, delay = 0.0533f),
    )

    /** A sparkle as drawn: its own scale, opacity and roll. */
    data class SparkleFrame(
        val x: Float,
        val y: Float,
        val radius: Float,
        val alpha: Float,
        val spinDeg: Float,
    )

    /**
     * Sparkle [index] at [t], or `null` before and after it.
     *
     * The storyboard's `.spark`: `46 % { 0.3, opacity 0 } 52 % { 1.2, opacity
     * 1, rotate 20° } 62 % { 0.4, opacity 0, rotate 45° }` — in, over-large,
     * out, turning the whole way.
     */
    fun sparkleAt(index: Int, t: Float): SparkleFrame? {
        val s = B_SPARKLES[index]
        val start = 0.46f + s.delay
        val peak = 0.52f + s.delay
        val end = 0.60f + s.delay
        if (t < start || t > end) return null
        val scale: Float
        val alpha: Float
        if (t <= peak) {
            val p = EASE_OUT((t - start) / (peak - start))
            scale = 0.30f + 0.90f * p
            alpha = p
        } else {
            val p = EASE_OUT((t - peak) / (end - peak))
            scale = 1.20f - 0.80f * p
            alpha = 1f - p
        }
        val spin = 45f * ((t - start) / (end - start))
        return SparkleFrame(s.x, s.y, s.size * scale, alpha, spin)
    }

    // ── THE REAR-UP, AND THE GALLOP ────────────────────────────────────────

    /**
     * How far the front comes up, degrees, **negative because the animal faces
     * right** and a lift of the right-hand side is a turn anticlockwise.
     *
     * Nine, which is the middle of item 189's *"~6-10°"* and a degree and a
     * half past the storyboard's `rotate(-6deg)` — the storyboard rears a stage
     * llama drawn upright, and ours is a bust whose pivot is further from its
     * muzzle, so the same six degrees moved it visibly less.
     */
    const val B_REAR_DEG: Float = -9f

    /**
     * The whole animal's vertical travel, master units, negative = up.
     *
     * The crouch first — the storyboard's `60% { translateY(2px) }`, which is
     * the weight going down before it goes up and is the entire reason the rear
     * does not look like a hinge opening — then the lift, then down again for
     * the launch. The gallop's own bounce is [gallopBob] and is added to this.
     */
    private val bBodyDy = Track(
        EASE_IN_OUT,
        0f to 0f, B_HORN_DONE to 0f,
        0.5600f to sb(3f), B_REAR_PEAK to sb(-13f), B_LAUNCH to 0f, 1f to 0f,
    )

    /**
     * …and the roll it carries, degrees, about the animal's own base.
     *
     * About the **base** and not about the head, for the reason round 36's lean
     * gave and which has not changed: a bust that pivots on itself nods, and a
     * bust that pivots on its base rears.
     */
    private val bBodyRotDeg = Track(
        EASE_IN_OUT,
        0f to 0f, B_HORN_DONE to 0f,
        0.5600f to 1.2f, B_REAR_PEAK to B_REAR_DEG, B_LAUNCH to 0f, 1f to 0f,
    )

    /**
     * The storyboard's `.actor` easing is `ease-in-out` across the whole three
     * seconds; the exit alone wants its acceleration weighted forward, because
     * a gallop that eases *out* is an animal slowing down as it leaves.
     *
     * Measured off the first recording rather than chosen: at
     * `cubic-bezier(.36,0,.78,1)` the animal's centroid moved 82 px in the first
     * third of a second of the run and 320 in the last tenth, which photographs
     * as a hesitation followed by a jump cut. These four numbers put half the
     * travel inside the first half of the run, so the bounce cycles happen
     * while the animal is still on the screen — which is the only reason to
     * have them.
     */
    private val GALLOP = cubicBezier(0.25f, 0.05f, 0.55f, 1f)

    /**
     * **How far along the exit the animal is, 0 = where it stood, 1 = entirely
     * off the right-hand edge of the screen.**
     *
     * A fraction and not a distance, for exactly the reason animation A's rings
     * are a fraction: the storyboard could write `translateX(430px)` because
     * its stage was a fixed 340 px box, and a phone is not. How many master
     * units 1 is worth is [exitTravelMasterUnits], measured off the screen the
     * film is actually playing on.
     */
    private val bGallopX = Track(
        GALLOP,
        0f to 0f, B_LAUNCH to 0f, B_EXIT to 1f, 1f to 1f,
    )

    /**
     * **How far the animal must travel to be entirely off the right edge**, in
     * master units.
     *
     * Pure, and here rather than in the composable, because *"gallops off the
     * RIGHT edge"* is a claim about the screen and a test has to be able to
     * check it on screens nobody has built yet. The art box's own left edge and
     * scale are what the composable knows; the width is the screen's.
     *
     * @param screenWidthPx the whole screen.
     * @param boxLeftPx the art box's left edge on it.
     * @param boxScale master units → pixels.
     */
    fun exitTravelMasterUnits(screenWidthPx: Float, boxLeftPx: Float, boxScale: Float): Float =
        // From the art box's left edge to past the screen's right edge, plus a
        // margin: the llama is drawn from the canvas's own left edge, so the
        // last thing to leave is the pixel at master x = 0.
        (screenWidthPx - boxLeftPx) / boxScale + EXIT_MARGIN

    /** …and how far past the edge is far enough to be certain. */
    private const val EXIT_MARGIN: Float = 40f

    /** How high a gallop bounce goes, master units. */
    const val B_GALLOP_BOB: Float = 46f

    /** …and how much it rolls with each stride, degrees. */
    const val B_GALLOP_TILT_DEG: Float = 3.6f

    /**
     * The bounce, as arithmetic rather than as a table of stops.
     *
     * [B_GALLOP_BOUNCES] cycles of a sine across the exit: it starts at nothing
     * and ends at nothing, which a hand-written table of seven stops would also
     * do and would do worse — the stops would have to be recomputed by hand
     * every time [B_LAUNCH] or [B_EXIT] moved, and the film would develop a limp
     * that nobody could see the cause of. A bounce is periodic; write it as one.
     */
    private fun gallopPhase(t: Float): Float {
        if (t <= B_LAUNCH) return 0f
        val p = ((t - B_LAUNCH) / (B_EXIT - B_LAUNCH)).coerceIn(0f, 1f)
        return p
    }

    fun gallopBob(t: Float): Float {
        val p = gallopPhase(t)
        if (p <= 0f || p >= 1f) return 0f
        return -B_GALLOP_BOB * kotlin.math.sin(2.0 * Math.PI * B_GALLOP_BOUNCES * p).toFloat()
    }

    /**
     * …and the roll that goes with it, a quarter-cycle out of phase so the
     * animal is most nose-up at the top of a stride rather than on the way to
     * it. In phase, the two cancel and the gallop reads as a lift.
     */
    fun gallopTilt(t: Float): Float {
        val p = gallopPhase(t)
        if (p <= 0f || p >= 1f) return 0f
        return B_GALLOP_TILT_DEG *
            kotlin.math.sin(2.0 * Math.PI * B_GALLOP_BOUNCES * p - Math.PI / 2.0).toFloat() +
            B_GALLOP_TILT_DEG
    }

    /**
     * **The rainbow trail**, and how far behind it is still visible.
     *
     * The storyboard's `.trail` starts at 70 %, peaks at 0.85 alpha at 80 % and
     * is gone at 100 %. Ours is out a little earlier at each end, because the
     * stage has to be empty at [B_EXIT] and a band still fading at 3.0 s is the
     * thing the owner's *"empty stage"* note is about.
     */
    private val bTrailAlpha = Track(
        EASE_OUT,
        0f to 0f, 0.69f to 0f, 0.78f to 0.82f, 0.92f to 0f, 1f to 0f,
    )

    /**
     * **The dust puff**, at the launch point and nowhere else.
     *
     * It fires **on** the launch, not a fifth of a second after it as the
     * storyboard's `.dust` does (74 %). Dust is thrown by the push-off; dust
     * that arrives later is an animal kicking at nothing.
     */
    private val bDustAlpha = Track(
        EASE_OUT,
        0f to 0f, B_LAUNCH to 0f, 0.71f to 0.85f, 0.90f to 0f, 1f to 0f,
    )

    /** How far the dust has spread and risen, 0‥1. */
    private val bDustSpread = Track(
        EASE_OUT,
        0f to 0f, B_LAUNCH to 0f, 0.90f to 1f, 1f to 1f,
    )

    /** One frame of animation B — the unicorn egg. */
    data class FrameB(
        val overlayAlpha: Float,
        /** The samples of each ribbon, in orbit order. Empty once they are gone. */
        val ribbons: List<List<RibbonSample>>,
        /** The eye's own offset from its anchor while it watches, master units. */
        val eyeLookX: Float,
        val eyeLookY: Float,
        /** The whole animal's travel and roll: crouch, rear, gallop bounce. */
        val bodyDy: Float,
        val bodyRotDeg: Float,
        /** …and how far along the exit it is, 0‥1 of [exitTravelMasterUnits]. */
        val gallopX: Float,
        /** The puck, stretching toward the horn as it goes. */
        val puckAlpha: Float,
        val puckStretch: Float,
        val puckNarrow: Float,
        /** …and the horn arriving on the same anchor. */
        val hornAlpha: Float,
        val hornGrow: Float,
        val hornWidth: Float,
        /** How far the rainbow has run down the fleece, 0 = cream, 1 = flooded. */
        val fleeceFlood: Float,
        val maneAlpha: Float,
        val flash: Float,
        val sparkles: List<SparkleFrame>,
        val trailAlpha: Float,
        val dustAlpha: Float,
        val dustSpread: Float,
    )

    /** @param t 0f‥1f — the fraction of [WelcomeAnimation.DURATION_MS] elapsed. */
    fun frameB(t: Float): FrameB {
        val c = t.coerceIn(0f, 1f)
        val (lookX, lookY) = eyeFollow(c)
        return FrameB(
            overlayAlpha = bOverlay.at(c),
            ribbons = B_RIBBONS.indices.map { ribbonAt(it, c) },
            eyeLookX = lookX,
            eyeLookY = lookY,
            bodyDy = bBodyDy.at(c) + gallopBob(c),
            // The watch-tilt rides x, so the animal leans a little after
            // whichever ribbon is in front of it. It is over by [B_TRANSFORM]
            // and cannot interfere with the rear.
            bodyRotDeg = bBodyRotDeg.at(c) + gallopTilt(c) +
                B_WATCH_TILT_DEG * (lookX / EYE_FOLLOW_X),
            gallopX = bGallopX.at(c),
            puckAlpha = bPuckAlpha.at(c),
            puckStretch = bPuckStretch.at(c),
            puckNarrow = bPuckNarrow.at(c),
            hornAlpha = bHornAlpha.at(c),
            hornGrow = bHornGrow.at(c),
            hornWidth = bHornWidth.at(c),
            fleeceFlood = bFleeceFlood.at(c),
            maneAlpha = bManeAlpha.at(c),
            flash = bFlash.at(c),
            sparkles = B_SPARKLES.indices.mapNotNull { sparkleAt(it, c) },
            trailAlpha = bTrailAlpha.at(c),
            dustAlpha = bDustAlpha.at(c),
            dustSpread = bDustSpread.at(c),
        )
    }
}
