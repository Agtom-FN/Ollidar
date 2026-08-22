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


    // ══ ANIMATION B — the reference video, as two sections ════════════════
    //
    // ROUND 36 item 188 — **the owner's reference, translated rather than
    // interpreted.**
    //
    // > *"turn the 2 sections of spit into animation style directly and use
    // > it."* — the owner, 2026-08-23, handing over The Pet Collective's
    // > *"Llama Spit! Expectations Vs. Reality"* (12.3 s).
    //
    // Round 35 built a spit that nobody filmed: a tell, two bursts and six
    // marks on the glass. It is **entirely replaced**. What the reference
    // actually contains, watched frame by frame, is two shots and one gag:
    //
    //  * **EXPECTATION** (0 ‥ 5.3 s) — a llama leans in and takes a treat out
    //    of a hand. Gentle, slow, no spit at all. The whole section is one
    //    endearing beat, and the joke does not work without it;
    //  * **REALITY** (5.3 ‥ 12.3 s) — a different animal faces the lens
    //    dead-on and **grinds its jaw side to side** for several seconds while
    //    staring down the camera. Then, *between two frames* at 11.05 s, the
    //    lens is gone: the whole screen is murky translucent splatter, held to
    //    the end, with the llama a blur somewhere behind it.
    //
    // The two things that make the reference funny are therefore the **jaw**
    // and the **cut to nothing**, and both are structural rather than
    // decorative. Round 35's choreography had neither: its spray flew *past*
    // the camera and politely left the view clear.
    //
    // ## The mapping, section by section
    //
    // | reference | ours |
    // |---|---|
    // | 0 ‥ 5.3 s — leans in, takes the treat | 0.00 ‥ 1.00 s — [bLean] + [bBlink] on the side pose |
    // | 5.3 s — cut to the other animal | 1.00 ‥ 1.20 s — [B_TURN], the pinch to the front pose |
    // | 5.3 ‥ 11.0 s — the jaw grinds, the stare | 1.20 ‥ 2.10 s — [bJawGrind], [bEyeNarrow], [bEarPin] |
    // | 11.05 s — two frames, and the lens is gone | 2.10 ‥ 2.20 s — [B_SPRAY] for one frame, then [B_LENS] |
    // | 11.1 ‥ 12.3 s — held, drips, a shape behind it | 2.20 ‥ 3.00 s — the hold, [LensSplat.drip], the grin |
    //
    // Six seconds of stare becomes nine tenths of one, and five of treat
    // becomes one: the ratio the reference keeps between its two sections is
    // roughly 5:7, and ours is 1:2, because a three-second film cannot spend
    // half of itself on the set-up and still land the gag.
    //
    // **No hand and no treat.** Item 188 makes it optional and the answer is
    // no: this art has an ink language for exactly one character, and a human
    // hand drawn in it is a second character with no vocabulary — while a
    // treat with no hand holding it is a pellet floating in the air. The lean
    // itself is what the reference's first section *is* (the animal comes to
    // you), and a blink is the sweetness the treat was carrying.
    //
    // Every number is fixed rather than random, for the reason every constant
    // in this file is fixed: the film has to be the same film every time.

    /**
     * **One frame**, in film time, at 30 fps.
     *
     * The reference's gag is a *cut*: at 11.02 s the animal is in focus and at
     * 11.09 s the lens is opaque, with a single smeared frame between them.
     * That is the beat, so it is a named constant rather than a number chosen
     * to look fast — the cone lives for one of these and the cover arrives over
     * two.
     */
    const val B_FRAME: Float = 1f / 90f

    /** The lean is at full reach — the head is down and in. **0.62 s.** */
    const val B_TAKE: Float = 0.207f

    /** The cut: the side pose starts to go. **1.02 s.** */
    const val B_CUT: Float = 0.34f

    /** The pinch — both poses flat in x, which is the turn's own middle. **1.07 s.** */
    const val B_TURN: Float = 0.3555f

    /** The front pose is up, and the jaw starts working. **1.20 s.** */
    const val B_FRONT: Float = 0.40f

    /**
     * **THE HIT. 2.10 s.**
     *
     * The last grind, the mouth wide, and the cone leaves — all on one instant,
     * because in the reference there is no wind-up at all. The animal is
     * chewing, and then the lens is gone.
     */
    const val B_HIT: Float = 0.70f

    /** …and it is completely gone by here, two frames later. **2.20 s.** */
    const val B_COVERED: Float = 0.7333f

    /** The grin, dimly, through the one patch that has run clear. **2.40 s.** */
    const val B_GRIN: Float = 0.80f

    /**
     * How far a particle at `reach = 1` travels toward the viewer, master
     * units. The cone only lives for [B_FRAME], so this is how far it gets in
     * that frame rather than a flight anyone can follow.
     */
    val B_SPRAY_TRAVEL: Float = sb(150f)

    /** …and how wide the cone opens, at `spread = ±1`. */
    val B_SPRAY_SPREAD: Float = sb(150f)

    /** A particle's radius at `size = 1`, before the flight grows it. */
    const val B_DROP_RADIUS: Float = 27f

    /**
     * Mist is soft and nearly not there — no outline, and this much alpha at
     * most.
     *
     * Not lower: water-blue at 0.22 over this app's dark page composites to a
     * dark teal, and three of them photographed as grey smudges — a hole in the
     * screen rather than a puff in the air. Semi-transparent has to be
     * semi-transparent *against the ground it is on*.
     */
    const val B_MIST_ALPHA: Float = 0.42f

    private val bOverlay = Track(LINEAR, 0f to 1f, 0.93f to 1f, 1f to 0f)

    // ── SECTION ONE — the expectation ──────────────────────────────────────

    /**
     * ROUND 36 item 188 — **the lean**, 0 = standing, 1 = head down and in.
     *
     * The reference's first section is five seconds of an animal *coming to
     * you*: it lowers its head, reaches, takes what is in the hand and lifts
     * away again chewing. Three of those four beats are in this one track —
     * reach (to [B_TAKE]), take (the hold), lift away (the release) — and the
     * release only goes back to a third rather than to nothing, because an
     * animal that has just been fed does not return to attention.
     *
     * The composable spends it as a rotation about the animal's own base plus a
     * little scale, so the muzzle arcs **forward and down** the way a neck
     * does. A translation would have slid the whole llama across the page.
     */
    private val bLean = Track(
        EASE_IN_OUT,
        0f to 0f, 0.03f to 0f, B_TAKE to 1f, 0.26f to 1f, B_CUT to 0.34f, 1f to 0.34f,
    )

    /**
     * …and **the blink**, 0 = open, 1 = shut. One, soft, just after the take.
     *
     * This is what the treat is carrying in the reference and it is the whole
     * reason the second section is funny: something has to be sweet first. A
     * blink rather than round 35's sparkle — a starburst is a *cartoon*
     * saying "isn't this nice", and the animal doing it itself is better.
     */
    private val bBlink = Track(
        EASE_IN_OUT,
        0f to 0f, 0.232f to 0f, 0.250f to 1f, 0.256f to 1f, 0.280f to 0f, 1f to 0f,
    )

    // ── THE CUT ────────────────────────────────────────────────────────────
    //
    // The reference cuts hard to a different animal on a different hillside.
    // We have one llama, so the cut is a turn — the two alphas cross while the
    // pair is pinched flat in x, which is what makes it read as a head coming
    // round rather than as one drawing replacing another. Two tenths of a
    // second, which is the item's window and is quick enough to be a cut.

    private val bSideAlpha = Track(LINEAR, 0f to 1f, B_CUT to 1f, 0.362f to 0f, 1f to 0f)
    private val bFrontAlpha = Track(LINEAR, 0f to 0f, 0.352f to 0f, 0.372f to 1f, 1f to 1f)
    private val bTurnScaleX = Track(
        EASE_IN_OUT,
        0f to 1f, 0.315f to 1f, B_TURN to 0.18f, B_FRONT to 1f, 1f to 1f,
    )

    // ── SECTION TWO — the reality: the tell ────────────────────────────────

    /**
     * ROUND 36 item 188 — **the jaw**, −1 = ground fully left, +1 = fully
     * right.
     *
     * This is the reference's tell and round 35 did not have it. Six seconds of
     * that video are an animal chewing *at* the camera: the muzzle slides
     * across the face, right, left, right, while the eyes never leave you. It
     * is menace and it is entirely lateral — a jaw that opens and shuts is an
     * animal eating, and a jaw that goes side to side is an animal deciding.
     *
     * **Two and a half cycles** in nine tenths of a second (item 188 asks for
     * 2–3), and the last one is short of the far side because the mouth is
     * already opening for the hit.
     */
    private val bJawGrind = Track(
        EASE_IN_OUT,
        0f to 0f, B_FRONT to 0f,
        0.445f to 1f, 0.490f to -1f, 0.535f to 1f, 0.580f to -1f, 0.625f to 1f,
        0.670f to -0.55f, B_HIT to 0f, 1f to 0f,
    )

    /**
     * The ears, 0 = upright, 1 = flat back against the head.
     *
     * They pin on the turn and they **stay** pinned: item 188 keeps round 35's
     * ear-pin and joins the jaw to it rather than replacing it, and there is no
     * beat in this cut where they would come up again — the film ends with the
     * lens covered, and an animal that has just done that does not relax while
     * you are still looking at it.
     */
    private val bEarPin = Track(
        EASE_IN_OUT,
        0f to 0f, 0.372f to 0f, 0.45f to 1f, 1f to 1f,
    )

    /** …and the eyes narrow onto the stare, 0 = round, 1 = lidded. */
    private val bEyeNarrow = Track(
        EASE_IN_OUT,
        0f to 0f, 0.372f to 0f, 0.47f to 1f, 1f to 1f,
    )

    /**
     * The head through the grind, master units, negative = up.
     *
     * Almost still, and that is the point — the reference's second animal holds
     * its head at one height for six seconds and lets the jaw do all of the
     * moving. What little there is here is the jaw's own weight (a few units
     * with each pass), then a small draw-back a tenth before the hit and the
     * snap through it.
     */
    private val bHeadRise = Track(
        EASE_IN_OUT,
        0f to 0f, B_FRONT to 0f, 0.47f to 7f, 0.535f to -5f, 0.60f to 6f, 0.66f to -4f,
        0.69f to -20f, B_HIT to -12f, 0.716f to 30f, 0.77f to 14f, 1f to 14f,
    )

    /** …and the roll it carries, degrees, about the base of the neck. */
    private val bHeadTilt = Track(
        EASE_IN_OUT,
        0f to 0f, B_FRONT to 0f, 0.47f to 2.4f, 0.535f to -2.4f, 0.60f to 2f, 0.66f to -1.8f,
        0.69f to -5f, B_HIT to -3f, 0.716f to 6.5f, 0.78f to 0f, 1f to 0f,
    )

    /**
     * The creep toward the viewer, as a scale.
     *
     * It grows through the stare — five per cent over nine tenths of a second,
     * which nobody watches happen and everybody feels — and is thrown at the
     * camera on the hit. That is the frame the cone comes out of, and it is
     * the last frame of the animal anyone sees.
     */
    private val bHeadLunge = Track(
        EASE_IN_OUT,
        0f to 1f, B_FRONT to 1f, 0.66f to 1.05f, 0.69f to 1.02f, B_HIT to 1.03f,
        0.716f to 1.24f, 0.78f to 1.09f, 1f to 1.09f,
    )

    /**
     * How open the mouth is: working through the grind, **wide** on the hit.
     *
     * It never shuts during the stare. A jaw grinding with the mouth closed is
     * a shape sliding about under the fleece; a crack of dark that opens and
     * narrows with each pass is a mouth.
     */
    private val bMouthOpen = Track(
        EASE_OUT,
        0f to 0f, B_FRONT to 0.12f,
        0.445f to 0.34f, 0.490f to 0.14f, 0.535f to 0.36f, 0.580f to 0.14f, 0.625f to 0.34f,
        0.670f to 0.16f, 0.694f to 0.30f, B_HIT to 1f, 0.716f to 0.82f, 0.78f to 0.30f,
        // …and it narrows again as the grin comes up behind the patch, so that
        // what shows through the thin place is a grin and not a mouth with a
        // line under it.
        0.86f to 0.15f, 1f to 0.15f,
    )

    private val bGrinAlpha = Track(EASE_OUT, 0f to 0f, B_GRIN to 0f, 0.86f to 1f, 1f to 1f)

    // ── THE CONE — one frame of it ─────────────────────────────────────────

    /**
     * One particle's whole life, as eight numbers.
     *
     * A **table** and not a particle system, and not a random draw, so the
     * burst is the same burst on every play. Round 35's version of this class
     * also carried the mark each drop left on the glass; item 188 takes that
     * away, because in the reference nothing lands in front of you — the lens
     * itself goes, all at once, and what covers it is [B_LENS].
     */
    class SprayShot(
        /** When it leaves the mouth, in film time. */
        val launch: Float,
        /** How long it is in the air, in film time. */
        val flight: Float,
        /** Its bearing across the cone, −1‥1 of [B_SPRAY_SPREAD]. */
        val spread: Float,
        /** How far toward the viewer it gets, as a fraction of [B_SPRAY_TRAVEL]. */
        val reach: Float,
        /** Its radius, as a fraction of [B_DROP_RADIUS] (or of a mist puff's own scale). */
        val size: Float,
        /** A few degrees of tumble on top of the bearing its own flight gives it. */
        val spin: Float,
        /** Soft, outline-free and semi-transparent, rather than a drop with an edge. */
        val mist: Boolean = false,
    ) {
        /** The instant it arrives at the glass. */
        val landing: Float get() = launch + flight
    }

    /**
     * ROUND 36 item 188 — **the single frame of spray cone.**
     *
     * Twelve drops and three mist puffs, all of them out of the mouth inside
     * four hundredths of a second and all of them gone within [B_FRAME] of
     * leaving. This is not round 35's burst re-timed: that one was a
     * quarter-second flight anyone could follow, and the reference contains no
     * such thing. What it contains is **one** smeared frame between a llama and
     * an opaque lens, and this table is that frame — wide, large, and over.
     *
     * It is deliberately more than covers the screen, because it is only ever
     * seen once and behind it the cover is already arriving.
     */
    val B_SPRAY: List<SprayShot> = listOf(
        SprayShot(0.7000f, 0.0130f, -0.92f, 1.42f, 0.52f, -8f),
        SprayShot(0.7000f, 0.0122f, -0.55f, 1.60f, 0.60f, 6f),
        SprayShot(0.7002f, 0.0140f, -0.24f, 1.30f, 0.44f, 12f),
        SprayShot(0.7002f, 0.0116f, 0.06f, 1.66f, 0.62f, -4f),
        SprayShot(0.7004f, 0.0134f, 0.36f, 1.24f, 0.46f, 9f),
        SprayShot(0.7004f, 0.0126f, 0.68f, 1.52f, 0.55f, -11f),
        SprayShot(0.7006f, 0.0144f, 0.98f, 1.18f, 0.40f, 5f),
        SprayShot(0.7008f, 0.0110f, -0.74f, 1.06f, 0.34f, -7f),
        SprayShot(0.7010f, 0.0118f, -0.06f, 0.96f, 0.30f, 10f),
        SprayShot(0.7012f, 0.0128f, 0.52f, 1.02f, 0.36f, -6f),
        SprayShot(0.7014f, 0.0112f, -0.40f, 1.34f, 0.38f, 8f),
        SprayShot(0.7016f, 0.0120f, 0.82f, 0.90f, 0.32f, -9f),
        // …and the mist inside it, out to the sides rather than on the chin: a
        // translucent puff centred on the muzzle photographs as a wet smudge on
        // the fleece, which is a different animal noise entirely.
        SprayShot(0.7000f, 0.0136f, -0.70f, 1.10f, 3.2f, 0f, mist = true),
        SprayShot(0.7004f, 0.0142f, 0.62f, 1.05f, 3.6f, 0f, mist = true),
        SprayShot(0.7008f, 0.0130f, 0.02f, 1.28f, 2.9f, 0f, mist = true),
    )

    /**
     * **Where** a particle is along its flight, and **how big** it is, are two
     * different curves, and that is deliberate.
     *
     * [burst] is the position: it leaves **explosively** and eases in, so the
     * cone is clear of the animal's own face within a fifth of the flight.
     * [approach] is the size: slow, then fast, which is what something coming
     * at a camera does. Together they read as "thrown hard, and arriving" —
     * and at this timing they are what stops the one frame being a ring of
     * identical dots pasted round the muzzle.
     */
    private fun burst(p: Float): Float =
        1f - Math.pow((1f - p).toDouble(), 1.4).toFloat()

    private fun approach(p: Float): Float = p * (0.42f + 0.58f * p)

    /**
     * One particle, this frame — or `null` if it has not left yet or has
     * arrived.
     *
     * Public, and indexed rather than handed a [SprayShot], so that a test can
     * walk one particle's whole flight rather than trying to pick it out of
     * [FrameB.spray], a list whose membership changes every frame.
     */
    fun sprayAt(index: Int, t: Float): SprayDrop? {
        val shot = B_SPRAY[index]
        if (t < shot.launch || t > shot.landing) return null
        // Bounded by the landing INSTANT rather than by the progress it
        // computes: `(landing − launch) / flight` is not exactly 1 in float for
        // most of this table.
        val p = ((t - shot.launch) / shot.flight).coerceIn(0f, 1f)
        val a = approach(p)
        val out = burst(p)
        val dx = shot.spread * B_SPRAY_SPREAD * out
        val dy = shot.reach * B_SPRAY_TRAVEL * out
        // The tail points back the way it came, which is a fact about the
        // flight rather than a number someone chose. `teardropPath` draws its
        // tail up, so the rotation that takes "up" onto "back toward the
        // mouth" is atan2(−dx, dy).
        val bearing = Math.toDegrees(kotlin.math.atan2(-dx.toDouble(), dy.toDouble())).toFloat()
        return if (shot.mist) {
            SprayDrop(
                dx = dx,
                dy = dy,
                radius = B_DROP_RADIUS * shot.size * (0.55f + 2.3f * p),
                // In fast, out slowly — a puff of mist has no edge and no
                // moment of arrival.
                alpha = B_MIST_ALPHA * (1f - p) * (1f - p) * (p * 7f).coerceAtMost(1f),
                stretch = 1f,
                tiltDeg = 0f,
                mist = true,
            )
        } else {
            SprayDrop(
                dx = dx,
                dy = dy,
                radius = B_DROP_RADIUS * shot.size * (0.30f + 3.4f * a),
                // It hands over to the cover rather than vanishing: the last
                // fifth of every flight fades, and the wash is already coming.
                alpha = ((1f - p) / 0.20f).coerceIn(0f, 1f),
                // Long and thin while it is fast, rounding up as it arrives.
                stretch = 1.85f - 0.70f * a,
                tiltDeg = bearing + shot.spin * (1f - p),
                mist = false,
            )
        }
    }

    /** A particle in flight, in master units measured from the mouth. */
    data class SprayDrop(
        val dx: Float,
        val dy: Float,
        val radius: Float,
        val alpha: Float,
        /** Length ÷ width: long while it is fast, rounding up as it slows. */
        val stretch: Float,
        /** Degrees, so its tail points back along the flight it actually flew. */
        val tiltDeg: Float,
        /** Soft, outline-free, low alpha. */
        val mist: Boolean,
    )

    // ── THE COVERED LENS ───────────────────────────────────────────────────
    //
    // ROUND 36 item 188 — the gag, and the one part of this film that is not
    // in master-art units.
    //
    // Everything else in both animations lives on the 1024 × 1024 icon canvas,
    // because everything else is a drawing of a llama. This is a drawing of
    // **the lens**, and the lens is the phone: it has to reach the corners of
    // whatever screen it is on, in the way the scan rings in animation A do
    // (see `WelcomeOverlay.ringReachInMasterUnits` for the same problem solved
    // the other way round). So the cover is stated in **screen fractions** —
    // x of the width, y of the height, radius of the width — and the
    // composable multiplies them out.

    /**
     * The murk over the whole screen, at full cover.
     *
     * **Just over half**, and the first cut of this had it at 0.76 with the
     * blobs at 0.62 on top. Photographed, that was not a covered lens: eighteen
     * translucent shapes at 0.62 stack, and where three of them overlapped the
     * result was 95 % opaque, so the whole screen went to one flat pale green
     * with a scribble of ink lines on it and the llama disappeared completely.
     * Item 188 asks for the llama to be **dimly visible through** the mess, and
     * a translucent thing that overlaps itself six times is not translucent.
     *
     * At these two numbers the densest place on the screen is about 79 % opaque
     * and the thinnest is 55 %, so the animal is a shape behind all of it and
     * the blobs are still individually legible.
     */
    const val B_WASH_ALPHA: Float = 0.55f

    /** …and how thin it goes over the one patch that runs clear. */
    const val B_PATCH_ALPHA: Float = 0.18f

    /**
     * The clear patch's radius, as a fraction of the screen's **width**.
     *
     * Its centre is not here: the composable puts it on the llama's own mouth,
     * measured off the art box, because the whole point of the patch is that
     * the **grin** is what shows through it. At the first cut's 0.24 the patch
     * was wider than the whole head and what came through it was the entire
     * animal, lit up, which is a window and not a thin place.
     */
    const val B_PATCH_RADIUS: Float = 0.17f

    /** A blob's own opacity on top of the wash — see [B_WASH_ALPHA] for why it is this low. */
    const val B_LENS_ALPHA: Float = 0.24f

    /** …and the weight of the ink round the larger ones, which does **not** follow the fill. */
    const val B_LENS_INK: Float = 0.30f

    /**
     * Blobs smaller than this, as a fraction of the screen's width, are not
     * outlined — item 188 asks for *"ink-outlined **larger** blobs"* and it is
     * right to. Outlining all eighteen put a hairline round every shape on the
     * screen and the cover photographed as a contour map.
     */
    const val B_LENS_INK_MIN_RADIUS: Float = 0.37f

    /** A drip is denser than the blob it came off — otherwise it is invisible against it. */
    const val B_DRIP_DENSITY: Float = 1.45f

    /**
     * The area of the screen the blobs alone must cover, before the wash —
     * item 188's *"high coverage ~90%"*, and a number the test measures rather
     * than trusts.
     */
    const val B_LENS_COVERAGE: Float = 0.90f

    // ── the shape of one splat ─────────────────────────────────────────────
    //
    // The outline lives HERE and not in the composable that strokes it, for one
    // reason: item 188's *"~90 % of the screen"* is a claim about the area
    // these shapes actually enclose, and a test that measures an ellipse
    // standing in for them measures the wrong thing by a fifth. The composable
    // and the test now build the same polygon out of the same two tables.

    /** How flat a splat is: taller than it is wide reads as a drip, not a hit. */
    const val SPLAT_SQUASH: Float = 0.82f

    /**
     * The splat's outline: eighteen radii, **lumpy rather than toothed**.
     *
     * Round 32 alternated two radii every point, which is not a splash — it is
     * a flower, and the first recording of round 34 showed it as one. These
     * wander over three or four points at a time, across ±28 %: at ±14 % a
     * mark photographs as a blue potato. A mark left by something that arrived
     * at speed has a couple of tongues on it.
     */
    val SPLAT_LOBES: FloatArray = floatArrayOf(
        1.00f, 1.14f, 0.94f, 0.76f, 0.88f, 1.09f, 1.26f, 1.02f, 0.83f,
        0.72f, 0.91f, 1.18f, 1.28f, 0.97f, 0.79f, 0.90f, 1.11f, 0.85f,
    )

    /** …and the lobes welded on off-centre, which stop the outline being a ring of teeth. */
    val SPLAT_BLOBS: List<Triple<Float, Float, Float>> = listOf(
        Triple(-0.92f, -0.42f, 0.30f),
        Triple(0.84f, -0.55f, 0.24f),
        Triple(0.62f, 0.58f, 0.22f),
    )

    /**
     * Vertex [i] of the outline of the splat with this [seed], as a multiple of
     * its radius and an angle in radians.
     *
     * @param seed which blob this is. It rolls both tables by a different
     *   amount for each one, so eighteen blobs on one screen are eighteen
     *   shapes rather than one shape eighteen times.
     */
    fun splatLobe(seed: Int, i: Int): Float =
        SPLAT_LOBES[Math.floorMod(i + seed * 5, SPLAT_LOBES.size)]

    fun splatAngle(seed: Int, i: Int): Double =
        2.0 * Math.PI * i / SPLAT_LOBES.size + 0.35 + seed * 0.41

    /** …and the [b]-th welded lobe of the same splat. */
    fun splatBlob(seed: Int, b: Int): Triple<Float, Float, Float> =
        SPLAT_BLOBS[Math.floorMod(b + seed, SPLAT_BLOBS.size)]

    /**
     * One blob of splatter on the lens.
     *
     * Fixed, and laid out rather than scattered: fifteen large blobs on a
     * jittered three-across grid, plus three long streaks, which between them
     * cover [B_LENS_COVERAGE] of a handset's screen — high, and deliberately
     * **not** total: at radii a third larger the same eighteen shapes close up
     * into one sheet and stop being blobs at all. The ragged holes between them
     * are where the wash alone shows, and they are what makes the cover read as
     * something thrown rather than as a colour filter.
     *
     * Their **centres** are all clear of the band where the llama's mouth sits,
     * on every screen this app runs on, so the patch that runs clear has no
     * blob core sitting on it — see [lensGapClearance].
     */
    class LensBlob(
        /** Its centre, as a fraction of the screen's width. */
        val x: Float,
        /** …and of its height. */
        val y: Float,
        /** Its radius, as a fraction of the screen's width. */
        val radius: Float,
        /** Length ÷ width. Above about 2 it stops being a blob and is a streak. */
        val stretch: Float,
        /** Which way that length lies, degrees. */
        val angleDeg: Float,
        /** How long after the hit it appears — a frame at most; this is a splat, not a fall. */
        val delay: Float,
        /** How far it runs before it dries, as a fraction of the screen's height. */
        val drip: Float = 0f,
    )

    /**
     * The cover, as laid out.
     *
     * Read as three columns and five rows with the middle of the fourth row
     * left out — that gap is the llama's mouth. The three [stretch] > 2
     * entries at the end are the streaks item 188 asks for alongside the
     * blobs: the same lumpy outline, pulled out and laid over at an angle, so
     * they are smears of the same stuff rather than a second material.
     *
     * **Two** of them run (the item asks for 1–2), and they are chosen high on
     * the screen so the run is visible for the whole hold rather than reaching
     * the bottom edge in a tenth of a second.
     */
    val B_LENS: List<LensBlob> = listOf(
        // row one — the top edge, over-covered because a blob centred on 0.04
        // puts only its lower half on the screen.
        LensBlob(0.14f, 0.04f, 0.345f, 1.15f, -18f, 0.000f),
        LensBlob(0.55f, 0.01f, 0.390f, 1.05f, 10f, 0.004f),
        LensBlob(0.93f, 0.07f, 0.338f, 1.25f, -30f, 0.002f),
        // row two
        LensBlob(0.04f, 0.23f, 0.330f, 1.10f, 25f, 0.006f, drip = 0.26f),
        LensBlob(0.48f, 0.19f, 0.405f, 1.00f, -8f, 0.000f),
        LensBlob(0.94f, 0.26f, 0.353f, 1.18f, 14f, 0.008f),
        // row three
        LensBlob(0.10f, 0.42f, 0.345f, 1.08f, -22f, 0.002f),
        LensBlob(0.90f, 0.37f, 0.360f, 1.12f, 8f, 0.004f, drip = 0.22f),
        // row four — the middle is missing on purpose. This is the mouth.
        LensBlob(-0.03f, 0.58f, 0.345f, 1.20f, 18f, 0.006f),
        LensBlob(1.03f, 0.56f, 0.353f, 1.14f, -16f, 0.000f),
        // row five — pushed to the edges and low, for the same reason as row
        // four: on a squarer screen the art box sits lower and the mouth comes
        // down with it, and these are the two that would arrive on top of it.
        LensBlob(0.03f, 0.79f, 0.368f, 1.06f, 12f, 0.008f),
        LensBlob(0.97f, 0.79f, 0.368f, 1.10f, -14f, 0.002f),
        // row six — the bottom edge, over-covered like the top
        LensBlob(0.10f, 0.97f, 0.375f, 1.16f, -10f, 0.004f),
        LensBlob(0.52f, 1.00f, 0.413f, 1.02f, 6f, 0.000f),
        LensBlob(0.93f, 0.99f, 0.360f, 1.22f, 20f, 0.006f),
        // the streaks
        LensBlob(0.28f, 0.30f, 0.150f, 3.4f, 62f, 0.010f),
        LensBlob(0.76f, 0.68f, 0.128f, 3.8f, -52f, 0.008f),
        LensBlob(0.42f, 0.88f, 0.128f, 3.2f, 78f, 0.010f),
    )

    /**
     * The wash arriving: nothing for one frame after the hit — which is the
     * frame the cone is seen in — then all of it over two more.
     */
    private val bLensWash = Track(
        EASE_OUT,
        0f to 0f, B_HIT + B_FRAME to 0f, B_COVERED to 1f, 1f to 1f,
    )

    /**
     * …and the patch running clear, 0 = the lens is uniformly gone, 1 = there
     * is a thin place over the mouth.
     *
     * It opens **after** the cover rather than with it, because the reference's
     * own hold does exactly that: the first covered frame is opaque and the
     * shape behind it only swims back into view as the stuff runs.
     */
    private val bLensPatch = Track(EASE_OUT, 0f to 0f, B_COVERED to 0f, 0.84f to 1f, 1f to 1f)

    /**
     * One blob on the lens, this frame — or `null` if it has not arrived.
     *
     * The bloom is deliberately tiny (a fifth of a frame's worth of growth over
     * one frame): a splat that *grows* is a drop landing in slow motion, and
     * the whole gag is that this did not happen in slow motion.
     */
    fun lensAt(index: Int, t: Float): LensSplat? {
        val blob = B_LENS[index]
        val start = B_HIT + B_FRAME + blob.delay
        // Every blob is finished at B_COVERED regardless of its own delay: the
        // ones that start late arrive faster, which is why the cover reads as
        // one event with texture rather than as a queue of arrivals.
        val ramp = ((t - start) / (B_COVERED - start)).coerceIn(0f, 1f)
        if (ramp <= 0f) return null
        // It dries: the film's last beat takes the gloss off the cover so the
        // grin behind the patch is the brightest thing left.
        val dry = ((t - 0.90f) / 0.10f).coerceIn(0f, 1f)
        val run = ((t - start - 0.02f) / 0.26f).coerceIn(0f, 1f)
        return LensSplat(
            x = blob.x,
            y = blob.y,
            radius = blob.radius * (0.84f + 0.16f * ramp),
            stretch = blob.stretch,
            angleDeg = blob.angleDeg,
            alpha = (B_LENS_ALPHA * ramp) - 0.06f * dry,
            ink = (B_LENS_INK * ramp) - 0.08f * dry,
            drip = blob.drip * EASE_OUT(run),
            seed = index,
        )
    }

    /** One blob on the lens, in screen fractions. */
    data class LensSplat(
        val x: Float,
        val y: Float,
        val radius: Float,
        val stretch: Float,
        val angleDeg: Float,
        val alpha: Float,
        /** The ink round it. Its own number, so a faint blob still has an edge. */
        val ink: Float,
        /** How far it has run, as a fraction of the screen's height. 0 = it does not run. */
        val drip: Float,
        /**
         * Which blob this is — the index into [B_LENS].
         *
         * The draw rolls the lumpy outline by it, so eighteen blobs are
         * eighteen shapes rather than one shape eighteen times.
         */
        val seed: Int,
    )

    /**
     * ROUND 36 item 188 — **how far a blob's centre is from the mouth**, in
     * screen widths, less its own radius.
     *
     * Positive for every blob is the property the layout above exists to have:
     * the patch that runs clear must not have an opaque core sitting on it, and
     * "I looked at it and it seemed fine" is not a thing that stays true when
     * somebody nudges a row.
     *
     * @param aspect the screen's width ÷ height. The mouth's own height on
     *   screen depends on it (`WelcomeOverlay.artBox` fits the art to the
     *   narrower of 80 % of the width and 46 % of the height), and
     *   [B_MOUTH_Y_MIN] ‥ [B_MOUTH_Y_MAX] are where it can land.
     */
    fun lensGapClearance(index: Int, aspect: Float, mouthY: Float): Float {
        val blob = B_LENS[index]
        val dx = blob.x - 0.5f
        // y is a fraction of the HEIGHT and x of the WIDTH, so one of them has
        // to be converted before they can be a distance. Widths, because that
        // is the unit radii are in.
        val dy = (blob.y - mouthY) / aspect
        return kotlin.math.hypot(dx.toDouble(), dy.toDouble()).toFloat() - blob.radius
    }

    /**
     * Where the llama's mouth can land, as a fraction of the screen's height.
     *
     * `WelcomeOverlay.artBox` fits the master square to the narrower of 80 % of
     * the width and 46 % of the height and pins its top at 30 %, and
     * `FrontPose.MOUTH_Y` is 70.1 % of the way down it. So on a 21:9 handset
     * the mouth is at 0.536 and on anything squarer than about 5:9 it stops at
     * 0.622 — this pair is that range, rounded out.
     */
    const val B_MOUTH_Y_MIN: Float = 0.53f

    /** …and the lowest it goes. */
    const val B_MOUTH_Y_MAX: Float = 0.63f

    /** One frame of animation B. */
    data class FrameB(
        val overlayAlpha: Float,
        val sideAlpha: Float,
        val frontAlpha: Float,
        /** Both poses pinch in x through the turn. */
        val turnScaleX: Float,
        /** ROUND 36 item 188 §1: 0 = standing, 1 = leaning in for the treat. */
        val lean: Float,
        /** …and 0 = eye open, 1 = shut. */
        val blink: Float,
        /** 0 = ears up, 1 = pinned flat back. */
        val earPin: Float,
        /** ROUND 36 item 188 §2: the jaw, −1 = ground left, +1 = ground right. */
        val jawGrind: Float,
        /** …and 0 = round-eyed, 1 = lidded onto the stare. */
        val eyeNarrow: Float,
        /** The chin's height, master units, negative = up. */
        val headRise: Float,
        /** …the roll it carries, degrees. */
        val headTiltDeg: Float,
        /** …and the creep at the viewer, as a scale. */
        val headLunge: Float,
        /** 0 = shut, 1 = wide. */
        val mouthOpen: Float,
        /** The one frame of cone. */
        val spray: List<SprayDrop>,
        /** ROUND 36 item 188: how much of the lens is gone, 0‥1. */
        val lensWash: Float,
        /** …and how far the one clear patch has run, 0‥1. */
        val lensPatch: Float,
        /** …and every blob on it. */
        val lens: List<LensSplat>,
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
            lean = bLean.at(c),
            blink = bBlink.at(c),
            earPin = bEarPin.at(c),
            jawGrind = bJawGrind.at(c),
            eyeNarrow = bEyeNarrow.at(c),
            headRise = bHeadRise.at(c),
            headTiltDeg = bHeadTilt.at(c),
            headLunge = bHeadLunge.at(c),
            mouthOpen = bMouthOpen.at(c),
            spray = B_SPRAY.indices.mapNotNull { sprayAt(it, c) },
            lensWash = bLensWash.at(c),
            lensPatch = bLensPatch.at(c),
            lens = B_LENS.indices.mapNotNull { lensAt(it, c) },
            grinAlpha = bGrinAlpha.at(c),
        )
    }
}
