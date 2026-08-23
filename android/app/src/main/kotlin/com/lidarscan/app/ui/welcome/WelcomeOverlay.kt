package com.lidarscan.app.ui.welcome

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Paint
import androidx.compose.ui.graphics.PathOperation
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.imageResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import com.lidarscan.app.R
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.core.welcome.WelcomeAnimation
import com.lidarscan.core.welcome.WelcomeTimeline
import com.lidarscan.core.welcome.WelcomeTimeline.Art
import kotlin.math.hypot

/**
 * ROUND 32 item 177 — **the welcome animation, drawn.**
 *
 * Pure Compose, as the item requires: four bitmap layers cut from the launcher
 * icon's 1024 px master, plus one drawn front-facing pose, moved by
 * [WelcomeTimeline]. No video, no GIF, no Lottie — the whole thing costs 102 kB
 * of art and no new dependency.
 *
 * ## One coordinate system
 *
 * Every number below is in **master-art units**: the 1024 × 1024 canvas the
 * icon was drawn on. One `withTransform` at the top of each draw maps that
 * square onto the screen, and after it a stroke width of 21 is the icon's own
 * outline weight, the puck's anchor is literally `(474, 176)`, and the two
 * films' geometry is the same arithmetic the `:core` tests pin. Nothing in here
 * converts anything twice.
 *
 * The one place that reaches back out to the screen is the rings, which have to
 * reach the **screen's** corner rather than the art box's — see
 * [ringReachInMasterUnits].
 *
 * ## ROUND 37 item 189 — what is drawn and what is a sprite
 *
 * Both films are now **the side sprites and nothing else**. Round 32's
 * front-facing pose — a drawn silhouette of head, crown scallops and two ears —
 * existed for animation B alone, and B does not turn to face you any more; it
 * was deleted with the choreography that needed it. What this file draws by
 * hand is what the master art has no sprite for and never will: the horn, the
 * mane, the ribbons, the sparkles and the dust, all in the icon's own ink
 * weight and all rooted on anchors measured off the sprites they sit on.
 */

// ── the art's own palette ──────────────────────────────────────────────────
//
// Sampled from the master PNG, not from `ScanColors`, and that is deliberate:
// this is an ILLUSTRATION, and it is the same illustration in both themes for
// the same reason the launcher icon is. What follows the theme is the scrim
// behind it (the page the app is about to show) and nothing else.

/** The fleece. `#F4F2ED`. */
private val Fleece = Color(0xFFF4F2ED)

/** The outline. `#211C18`. */
private val Ink = Color(0xFF211C18)

/** Agtom orange — the same value as `ScanColors.primary`, stated here because the art is. */
private val Flame = Color(0xFFF26A1B)

/** The icon's outline weight, measured off the master art (median run 19, mean 23). */
private const val OUTLINE = 21f

/**
 * The dead lidar: desaturated to luminance, halved, and pushed slightly blue.
 *
 * The item asks for the puck to be DARK until it lands. A flat silhouette would
 * have done that and thrown away the drum, the brim and the fluff notch that
 * make it recognisable as the thing on the llama's head — which is the whole
 * point of the shot. This keeps every edge and takes the life out of it: fleece
 * `#F4F2ED` lands on a slate `#8C929C`, ink stays near black.
 */
private val DeadMetal = ColorFilter.colorMatrix(
    ColorMatrix(
        floatArrayOf(
            0.1063f, 0.3576f, 0.0361f, 0f, 18f,
            0.1063f, 0.3576f, 0.0361f, 0f, 24f,
            0.1063f, 0.3576f, 0.0361f, 0f, 34f,
            0f, 0f, 0f, 1f, 0f,
        ),
    ),
)

// ── where the art box sits on the screen ───────────────────────────────────

/** The master square's side, as a fraction of the screen. */
private const val ART_WIDTH_FRACTION = 0.80f
private const val ART_HEIGHT_FRACTION = 0.46f

/** Its top edge. Chosen so A's apex clears the icon's frame with room to spare. */
private const val ART_TOP_FRACTION = 0.30f

private class ArtBox(val left: Float, val top: Float, val side: Float) {
    /** Master units → screen pixels. */
    val scale: Float get() = side / Art.CANVAS

    fun x(master: Float): Float = left + master * scale
    fun y(master: Float): Float = top + master * scale
}

private fun DrawScope.artBox(): ArtBox {
    val side = minOf(size.width * ART_WIDTH_FRACTION, size.height * ART_HEIGHT_FRACTION)
    return ArtBox(
        left = (size.width - side) / 2f,
        top = size.height * ART_TOP_FRACTION,
        side = side,
    )
}

/**
 * How far, **in master units**, a ring must travel for the owner's *"rings
 * expand … TO THE SCREEN EDGES"* to be true — the distance from the puck to the
 * furthest corner of the actual screen.
 *
 * The storyboard could say `scale(8.5)` because its stage was a fixed 340 × 420
 * box. A phone is not, and 8.5 × the storyboard's radius stops less than half
 * way down a modern handset. So the timeline's scale is read as a **fraction of
 * the way to the corner** (`scale / A_RING_FULL_SCALE`), which is the same
 * animation on every screen and is right on all of them.
 */
private fun DrawScope.ringReachInMasterUnits(box: ArtBox): Float {
    // ROUND 35 item 184: measured from the EMIT POINT, because that is where
    // the rings now leave from. Measuring from the puck's centre while drawing
    // from the emitter is how a ring stops a hundred units short on one side.
    val cx = box.x(Art.EMIT_X)
    val cy = box.y(Art.EMIT_Y)
    val corners = listOf(
        hypot(cx, cy),
        hypot(size.width - cx, cy),
        hypot(cx, size.height - cy),
        hypot(size.width - cx, size.height - cy),
    )
    return corners.max() / box.scale
}

// ── the overlay ────────────────────────────────────────────────────────────

/**
 * ROUND 32 item 177 → **ROUND 34 item 182: the card is gone.**
 *
 * Round 32 drew the launcher icon — the orange frame and its cream paper —
 * behind both films, and the reason was a bug: the cut body layer is an
 * OUTLINE whose fluff interior is transparent (in the icon the fleece and the
 * paper are the same colour, so the artist never drew a fill), and on this
 * app's dark page it composited as a see-through scribble with a floating
 * white face. The card put the artwork back on the ground it was drawn for and
 * the symptom went away.
 *
 * The owner's order: *"remove the boundary and show it as the welcome icon,
 * same style."* So the workaround comes out and the actual defect is fixed —
 * an **opaque fleece fill is baked into the body sprite** (see
 * `scratchpad/anim-assets/fill.py`, whose method is the enclosed-region flood
 * the layer cut used, plus the seal that an open-bottomed silhouette needs).
 * The llama now stands free on the page, in both themes, with no box round it,
 * and the scan rings leave the puck across an open screen instead of bursting
 * out of a frame.
 */

/**
 * The full-screen welcome film.
 *
 * @param variant which of the two, from [WelcomeAnimation.variantFor].
 * @param onFinished called exactly once — when the three seconds are up, or the
 *   instant the screen is touched. The caller removes the overlay; nothing here
 *   keeps running afterwards.
 */
@Composable
fun WelcomeOverlay(
    variant: WelcomeAnimation.Variant,
    onFinished: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val finish by rememberUpdatedState(onFinished)
    var done by remember { mutableStateOf(false) }
    val progress = remember { Animatable(0f) }

    LaunchedEffect(variant) {
        progress.animateTo(
            targetValue = 1f,
            // Linear, because every curve in this film is already in the
            // timeline's own per-segment easings. A second easing out here
            // would re-time all of them at once.
            //
            // ROUND 35 item 187: the duration is the film PLUS the hold, and
            // `filmProgress` clamps the film's own clock at 1 — so the last
            // second is the last frame, held, rather than a second drawing that
            // has to be kept in step with the first.
            animationSpec = tween(
                durationMillis = WelcomeAnimation.totalMsFor(variant),
                easing = LinearEasing,
            ),
        )
        if (!done) {
            done = true
            finish()
        }
    }

    val page = ScanColors.page
    // The egg's description is what the connected suite watches for and what a
    // screen reader announces; it says "developer" and not "unicorn", because
    // naming the joke in the accessibility tree gives it away.
    val description = when (variant) {
        WelcomeAnimation.Variant.LIDAR_FLIP -> "Welcome animation"
        WelcomeAnimation.Variant.UNICORN -> "Welcome animation, developer"
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .testTag("welcomeOverlay")
            .semantics { contentDescription = description }
            // TAP ANYWHERE = SKIP. On the DOWN, not on the tap: waiting for the
            // up would mean the first thing the app does is make you finish a
            // gesture. The event is consumed so the same touch cannot also
            // press whatever is underneath it — the overlay is a lid, and a lid
            // that leaks is worse than no lid.
            .pointerInput(variant) {
                awaitPointerEventScope {
                    val event = awaitPointerEvent()
                    event.changes.forEach { it.consume() }
                    if (!done) {
                        done = true
                        finish()
                    }
                }
            },
    ) {
        val body = ImageBitmap.imageResource(R.drawable.welcome_llama_body)
        val puck = ImageBitmap.imageResource(R.drawable.welcome_lidar_puck)
        val fan = ImageBitmap.imageResource(R.drawable.welcome_fan_dots)
        val eye = ImageBitmap.imageResource(R.drawable.welcome_llama_eye)
        // The mane is the same shape every frame it is on screen, and building
        // it is four `Path.op` unions.
        val mane = remember { maneArt() }

        Canvas(Modifier.fillMaxSize()) {
            val film = WelcomeAnimation.filmProgress(variant, progress.value)
            when (variant) {
                WelcomeAnimation.Variant.LIDAR_FLIP ->
                    drawLidarFlip(WelcomeTimeline.frameA(film), page, body, puck, fan, eye)

                WelcomeAnimation.Variant.UNICORN ->
                    drawUnicornEgg(WelcomeTimeline.frameB(film), page, body, eye, puck, mane)
            }
        }
    }
}

// ── shared drawing helpers, all in master units ────────────────────────────

/**
 * Draws [image] so that it occupies the master-unit rectangle
 * ([left], [top], [width] × [height]).
 *
 * The bitmaps are the downscaled cuts (the body is 512 px standing for 1024
 * master units), so every layer needs its own source-to-master ratio and none
 * of them may assume 1:1.
 */
private fun DrawScope.drawLayer(
    image: ImageBitmap,
    left: Float,
    top: Float,
    width: Float,
    height: Float,
    alpha: Float = 1f,
    colorFilter: ColorFilter? = null,
) {
    if (alpha <= 0f) return
    drawImage(
        image = image,
        srcOffset = IntOffset.Zero,
        srcSize = IntSize(image.width, image.height),
        dstOffset = IntOffset(left.toInt(), top.toInt()),
        dstSize = IntSize(width.toInt(), height.toInt()),
        alpha = alpha.coerceIn(0f, 1f),
        colorFilter = colorFilter,
    )
}

/** The llama's body, filling the master canvas, bobbing by [bob]. */
private fun DrawScope.drawBody(body: ImageBitmap, bob: Float, alpha: Float = 1f) {
    withTransform({ translate(0f, bob) }) {
        drawLayer(body, 0f, 0f, Art.CANVAS, Art.CANVAS, alpha)
    }
}

/**
 * The eye, at its anchor, carried by the body's [bob] and its own [look].
 *
 * ROUND 37 item 189 — and by [lookX] / [lookY], which is the whole of *"llama
 * watches (subtle eye/head follow is a plus if cheap)"*. It is cheap because
 * the eye has been a separate sprite since round 32: the cost of an animal
 * tracking something round its own head is one translate.
 *
 * Round 36's blink parameter is **gone** with the film that used it. Animation
 * A never passed either, and passes neither now.
 */
private fun DrawScope.drawEye(
    eye: ImageBitmap,
    bob: Float,
    look: Float,
    alpha: Float = 1f,
    lookX: Float = 0f,
    lookY: Float = 0f,
) {
    withTransform({ translate(lookX, bob + look + lookY) }) {
        drawLayer(
            eye,
            Art.EYE_CENTER_X - Art.EYE_SPRITE_WIDTH / 2f,
            Art.EYE_CENTER_Y - Art.EYE_SPRITE_HEIGHT / 2f,
            Art.EYE_SPRITE_WIDTH,
            Art.EYE_SPRITE_HEIGHT,
            alpha,
        )
    }
}

/**
 * The lit emitter.
 *
 * ROUND 35 item 184 — it takes its point rather than knowing one. In A that
 * point is [EMIT], drawn **outside** the puck's own transform stack so that the
 * light, the fan's apex and both ring centres are literally the same pixel at
 * the flash and not four points a few units apart. In B the llama is wearing
 * the puck somewhere else, and the same emitter offset is carried with it.
 */
private fun DrawScope.drawLed(at: Offset, alpha: Float) {
    if (alpha <= 0f) return
    drawCircle(Flame, radius = 74f, center = at, alpha = 0.22f * alpha)
    drawCircle(Flame, radius = 42f, center = at, alpha = 0.45f * alpha)
    drawCircle(Flame, radius = 21f, center = at, alpha = alpha)
}

/**
 * ROUND 35 item 184 — **the one emit point**, in master units.
 *
 * The owner: *"the lidar light, the spot and spin should align on the same
 * point."* This is that point — the LED's own anchor from `layers.json`, which
 * is also the fan bitmap's cone apex — and everything below that used to pivot
 * on the puck's centre or its foot now pivots on it.
 */
private val EMIT = Offset(Art.EMIT_X, Art.EMIT_Y)

// ══ ANIMATION A ═══════════════════════════════════════════════════════════

private fun DrawScope.drawLidarFlip(
    f: WelcomeTimeline.FrameA,
    page: Color,
    body: ImageBitmap,
    puck: ImageBitmap,
    fan: ImageBitmap,
    eye: ImageBitmap,
) {
    if (f.overlayAlpha <= 0f) return
    drawRect(page, alpha = f.overlayAlpha)

    val box = artBox()
    val reach = ringReachInMasterUnits(box)

    withTransform({
        translate(box.left, box.top)
        scale(box.scale, box.scale, pivot = Offset.Zero)
    }) {
        drawBody(body, f.bodyBob, f.overlayAlpha)
        drawEye(eye, f.bodyBob, f.eyeBob, f.overlayAlpha)

        // ROUND 35 item 184 — the rings, the fan, the spin and the squash all
        // leave from HERE, and the LED is drawn on the same coordinate with
        // nothing between it and them.
        val centre = EMIT
        fun ring(alpha: Float, scale: Float, weight: Float) {
            if (alpha <= 0f) return
            val fraction = scale / WelcomeTimeline.A_RING_FULL_SCALE
            drawCircle(
                color = Flame,
                radius = reach * fraction,
                center = centre,
                alpha = (alpha * f.overlayAlpha).coerceIn(0f, 1f),
                // The stroke does NOT scale with the ring the way a CSS
                // `transform: scale()` would. On the storyboard's 340 px stage
                // that was invisible; across a phone's whole diagonal it turns
                // the last ring into a 100 px orange band. It grows a little,
                // which keeps the pulse feeling like it is coming at you.
                style = Stroke(width = (weight + weight * 1.4f * fraction) / box.scale),
            )
        }
        ring(f.ring2Alpha, f.ring2Scale, 2.2f)
        ring(f.ring1Alpha, f.ring1Scale, 3.4f)

        // The fan dots, sweeping one revolution about the puck.
        if (f.fanAlpha > 0f) {
            withTransform({ rotate(f.fanRotationDeg, pivot = centre) }) {
                drawLayer(
                    fan, Art.FAN_LEFT, Art.FAN_TOP, Art.FAN_WIDTH, Art.FAN_HEIGHT,
                    f.fanAlpha * f.overlayAlpha,
                )
            }
        }

        // The puck itself: airborne, **pinwheeling about its own emitter**, and
        // landing with a squash whose axis passes through it.
        //
        // ROUND 35 item 184(c)(d). Round 34 spun it about the sprite's centre
        // and squashed it about the sprite's foot, so the thing the light comes
        // out of described a circle of its own while everything else radiated
        // from a point it was never at. Pivoting on the emitter costs nothing
        // at either end of the flip — 0° and 360° are the same frame whatever
        // the pivot is — and in between the puck swings the way a tossed
        // instrument swings about the heavy end.
        withTransform({
            translate(0f, f.puckDy)
            rotate(f.puckRotationDeg, pivot = EMIT)
            scale(f.puckScaleX, f.puckScaleY, pivot = Offset(Art.EMIT_X, Art.PUCK_FOOT_Y))
        }) {
            drawLayer(
                puck, Art.PUCK_LEFT, Art.PUCK_TOP, Art.PUCK_WIDTH, Art.PUCK_HEIGHT,
                f.overlayAlpha,
            )
            if (f.puckDim > 0f) {
                drawLayer(
                    puck, Art.PUCK_LEFT, Art.PUCK_TOP, Art.PUCK_WIDTH, Art.PUCK_HEIGHT,
                    f.puckDim * f.overlayAlpha, DeadMetal,
                )
            }
        }
        // …and the light last of all, on the bare emit point. It is outside the
        // block above on purpose: inside it, the landing squash would drag the
        // LED 25 units down the frame it ignites on, and the flash is the one
        // frame this item is judged by.
        drawLed(EMIT, f.ledAlpha * f.overlayAlpha)
    }
}


// ══ ANIMATION B — THE UNICORN EGG ═════════════════════════════════════════

/**
 * ROUND 32 item 177 → ROUND 34 item 183 → ROUND 35 item 185 → ROUND 36 item
 * 188 → **ROUND 37 item 189: the unicorn, drawn.**
 *
 * The owner replaced the brief rather than correcting it, and approved a
 * storyboard for the replacement (`ollidar-unicorn-egg.html` v2). Round 36's
 * film — the lean, the cut, the jaw grind, the covered lens — is **gone**, and
 * with it the whole front-facing pose it was the only user of. What is here is
 * the storyboard's four beats, on the **side sprites**, which is the second
 * thing the item asks for by name: *built from the REAL icon sprites, as the
 * welcome film uses.*
 *
 *  * **the ribbons** — two rainbow bands orbiting on an ellipse that passes in
 *    front of and behind the animal ([drawRibbonRun]);
 *  * **the transformation** — the flash, three sparkles, the puck stretching
 *    into the horn ([drawHornMorph]), the fleece flooding ([drawFloodedBody])
 *    and the mane ([drawMane]);
 *  * **the rear-up** — one rotation about the base;
 *  * **the gallop** — off the right edge, with a trail and a dust puff.
 *
 * The draw order is the item's own sentence: **back ribbons, animal, front
 * ribbons.** Nothing times the swap, because nothing has to — the samples carry
 * their own depth and the sort is the swap.
 */
private fun DrawScope.drawUnicornEgg(
    f: WelcomeTimeline.FrameB,
    page: Color,
    body: ImageBitmap,
    eye: ImageBitmap,
    puck: ImageBitmap,
    mane: Path,
) {
    if (f.overlayAlpha <= 0f) return
    drawRect(page, alpha = f.overlayAlpha)

    val box = artBox()
    // ROUND 37 item 189 — how far "off the RIGHT edge" is, on THIS screen.
    // Measured the way animation A's rings measure their reach, and for the
    // same reason: the storyboard's `translateX(430px)` is true of a 340 px
    // stage and of nothing else.
    val exit = WelcomeTimeline.exitTravelMasterUnits(size.width, box.left, box.scale)

    withTransform({
        translate(box.left, box.top)
        scale(box.scale, box.scale, pivot = Offset.Zero)
    }) {
        val a = f.overlayAlpha

        // ── what the gallop leaves behind, under everything it passes.
        drawGallopTrail(f, exit, a)
        drawDustPuff(f, a)

        // ── the half of each ribbon that is BEHIND the animal.
        for (samples in f.ribbons) drawRibbonRun(samples, front = false, alpha = a)

        // ── the animal itself, carried by one transform: the exit, the
        // crouch-and-rear, the gallop's bounce and roll. All of it about the
        // BASE, so the rear is a rear and not a hinge.
        withTransform({
            translate(f.gallopX * exit, f.bodyDy)
            rotate(f.bodyRotDeg, pivot = BASE_PIVOT)
        }) {
            drawFloodedBody(body, f.fleeceFlood, a)
            drawMane(mane, f.maneAlpha * a)
            drawEye(eye, 0f, 0f, a, f.eyeLookX, f.eyeLookY)
            drawHornMorph(puck, f, a)
        }

        // ── …and the half of each ribbon that is IN FRONT of it.
        for (samples in f.ribbons) drawRibbonRun(samples, front = true, alpha = a)

        // ── the sparkles, over everything, because they are the event and not
        // an object in the scene.
        for (s in f.sparkles) drawSparkle(s, a)
    }

    // ── the flash is drawn in SCREEN space, like animation A's rings and for
    // the same reason: it is a thing that happens to the picture rather than a
    // thing in it, so it has to reach the corners of whatever it is on.
    drawTransformFlash(f, box)
}

// ── the animal's own base ──────────────────────────────────────────────────

/**
 * ROUND 36 item 188 → **ROUND 37 item 189: the standing llama's own base**, low
 * and a little forward of the fleece's centre of mass, measured off
 * `welcome_llama_body.webp`.
 *
 * Round 36 leaned about it; round 37 rears, rolls and bounces about it. It is
 * the same point for the same reason both times: a bust that pivots on its head
 * nods, and a bust that pivots on its base is an animal moving.
 */
private val BASE_PIVOT = Offset(430f, 960f)

// ── the rainbow ────────────────────────────────────────────────────────────

/**
 * ROUND 37 item 189 — **the storyboard's two rainbows, and they are two on
 * purpose.**
 *
 * [RAIN] is the storyboard's `<linearGradient id="rain">`, a pastel ramp, and
 * it is what anything made of *llama* turns: the fleece, the horn, the mane,
 * the trail. [RIBBON] is its `.ribbon` background, the saturated one, and it is
 * what the two orbiting bands are — objects in the air rather than the animal.
 *
 * Keeping them apart is what stops the transformation from being one flat
 * colour event: a saturated ribbon reads against a pastel llama, and a pastel
 * ribbon against a pastel llama is a smear.
 */
private val RAIN = listOf(
    Color(0xFFFF8A80), Color(0xFFFFD180), Color(0xFFFFFF8D),
    Color(0xFFB9F6CA), Color(0xFF82B1FF), Color(0xFFEA80FC),
)

private val RIBBON = listOf(
    Color(0xFFFF3B30), Color(0xFFFF9500), Color(0xFFFFCC00),
    Color(0xFF34C759), Color(0xFF007AFF), Color(0xFFAF52DE),
)

/**
 * A colour off a ramp at [p], **wrapping** — so a hue that runs off the violet
 * end comes back at the red one and a ribbon going round twice does not have a
 * seam in it.
 */
private fun rainbowAt(ramp: List<Color>, p: Float): Color {
    val n = ramp.size
    val x = (p - kotlin.math.floor(p)) * n
    val i = x.toInt().coerceIn(0, n - 1)
    val t = x - i
    val next = ramp[(i + 1) % n]
    val c = ramp[i]
    return Color(
        red = c.red + (next.red - c.red) * t,
        green = c.green + (next.green - c.green) * t,
        blue = c.blue + (next.blue - c.blue) * t,
    )
}

// ── the ribbons ────────────────────────────────────────────────────────────

/**
 * ROUND 37 item 189 — **one ribbon's front half, or its back half.**
 *
 * > *"layer order must swap at the orbit's front/back crossings, which the
 * > storyboard's flat version could not show but the build must do."*
 *
 * The swap is not timed and there is no state anywhere that says which side a
 * ribbon is on. Each sample carries its own `depth`, this function draws the
 * ones whose sign matches, and the caller calls it twice with the animal in
 * between. A ribbon crossing the ellipse's end therefore hands itself over
 * mid-band, sample by sample, which is what going behind something looks like.
 *
 * Each sample is a **round-capped stroke** from its neighbour, not a segment of
 * one filled path, and that is the mechanism rather than a shortcut: a path has
 * one z. Round caps weld the strokes into a continuous band, and the colour is
 * taken per sample off the ramp, which is how the gradient survives being
 * chopped into twenty-eight pieces.
 *
 * The **glow** the item asks for is a second pass underneath at three times the
 * width and a fifth of the alpha. Drawn per sample rather than as one blurred
 * layer because a `saveLayer` and a blur for this is a full-screen buffer per
 * frame for something a wide translucent stroke does honestly.
 */
private fun DrawScope.drawRibbonRun(
    samples: List<WelcomeTimeline.RibbonSample>,
    front: Boolean,
    alpha: Float,
) {
    if (samples.isEmpty() || alpha <= 0f) return
    for (i in 0 until samples.size - 1) {
        val s = samples[i]
        val next = samples[i + 1]
        // A joint belongs to the side its own midpoint is on; splitting on the
        // sample rather than the joint would leave a one-stroke gap at each
        // crossing.
        val mid = (s.depth + next.depth) / 2f
        if ((mid > 0f) != front) continue
        val paint = rainbowAt(RIBBON, s.hue)
        val a = (s.alpha * alpha).coerceIn(0f, 1f)
        if (a <= 0f) continue
        val from = Offset(s.x, s.y)
        val to = Offset(next.x, next.y)
        drawLine(
            color = paint,
            start = from,
            end = to,
            strokeWidth = s.halfWidth * 6f,
            cap = StrokeCap.Round,
            alpha = a * 0.16f,
        )
        drawLine(
            color = paint,
            start = from,
            end = to,
            strokeWidth = s.halfWidth * 2f,
            cap = StrokeCap.Round,
            alpha = a,
        )
    }
}

// ── the fleece, flooded ────────────────────────────────────────────────────

/**
 * ROUND 37 item 189 — **the fleece floods rainbow.**
 *
 * > *"tint the body sprite via a gradient overlay masked to the sprite's opaque
 * > fleece — the round-35 filled body layer makes this possible."*
 *
 * It does, and this is why it had to: until round 34 baked an opaque fleece
 * into the cut body layer, the sprite was an **outline** with a transparent
 * interior, and there was nothing to tint.
 *
 * The mask is [BlendMode.Modulate] inside a `saveLayer`, and the choice of
 * blend is the whole of the trick:
 *
 *  * `SrcIn` and `SrcAtop` would have replaced the sprite's colour, **ink and
 *    all**, and the icon's language is one continuous dark outline;
 *  * plain `Multiply` keeps the ink (dark × anything is dark) but Skia's
 *    Porter-Duff coverage lets the source through where the destination is
 *    empty, so a full-canvas rainbow would have filled the layer;
 *  * `Modulate` is multiply **including alpha** — `a = sa × da` — so it is
 *    multiply where the sprite is and nothing at all where it is not. The
 *    fleece goes rainbow, the outline stays black, the page is untouched.
 *
 * And the flood has a **front**: each stop is lerped from white — which under a
 * multiply is a no-op — toward its colour by how far the front has passed it.
 * At [flood] = 0 every stop is white and the whole pass is arithmetic that
 * changes nothing, which is worth more than a branch: there is no second code
 * path for the un-flooded llama, so there is nothing to keep in step.
 */
private fun DrawScope.drawFloodedBody(body: ImageBitmap, flood: Float, alpha: Float) {
    if (flood <= 0f) {
        drawLayer(body, 0f, 0f, Art.CANVAS, Art.CANVAS, alpha)
        return
    }
    // The front runs from the crown to below the feet, with a soft edge: the
    // colour arrives from the horn, so it comes DOWN.
    val front = -FLOOD_SOFTNESS + flood * (Art.CANVAS + 2f * FLOOD_SOFTNESS)
    val stops = RAIN.mapIndexed { i, colour ->
        val at = i.toFloat() / (RAIN.size - 1)
        val y = at * Art.CANVAS
        val wet = ((front - y) / FLOOD_SOFTNESS).coerceIn(0f, 1f)
        at to Color(
            red = 1f + (colour.red - 1f) * wet,
            green = 1f + (colour.green - 1f) * wet,
            blue = 1f + (colour.blue - 1f) * wet,
        )
    }.toTypedArray()

    drawIntoCanvas { canvas ->
        canvas.saveLayer(Rect(0f, 0f, Art.CANVAS, Art.CANVAS), Paint())
        drawLayer(body, 0f, 0f, Art.CANVAS, Art.CANVAS, alpha)
        drawRect(
            brush = Brush.verticalGradient(
                colorStops = stops,
                startY = 0f,
                endY = Art.CANVAS,
            ),
            topLeft = Offset.Zero,
            size = Size(Art.CANVAS, Art.CANVAS),
            blendMode = BlendMode.Modulate,
        )
        canvas.restore()
    }
}

/** How wide the flood's own edge is, master units. A hard line is a wipe. */
private const val FLOOD_SOFTNESS = 260f

// ── the mane ───────────────────────────────────────────────────────────────

/**
 * ROUND 37 item 189 — **a rainbow mane along the neck edge.**
 *
 * The storyboard draws it as a narrow crescent down the back of the neck,
 * rainbow-filled with an ink stroke on it (`.mane`, `fill="url(#rain)"
 * stroke="#1c2430"`). Ours follows the **real** silhouette's back edge, traced
 * off `welcome_llama_body.webp`: the nape at roughly (470, 250), out to the
 * fleece's left shoulder at (250, 470), and down the back to (215, 800).
 *
 * Built once and not per frame — it is the same shape every time it is on
 * screen — and given the icon's own fluff language with four scallops welded
 * onto its outer edge, because a smooth crescent on this animal is a saddle.
 *
 * It keeps its **ink outline** even after the fleece has flooded, and that is
 * the one thing holding it apart from the body it sits on: at 1.60 s the llama
 * and the mane are the same six colours, and without the outline the mane is a
 * darker patch of nothing.
 */
private fun maneArt(): Path {
    val band = Path().apply {
        // The outer edge, on the fleece's own back contour — traced off the
        // sprite's alpha at eight rows rather than eyeballed: x is 258 at
        // y = 420, 214 at 616 and 220 at the bottom, which is where the silhouette
        // actually is. A mane that follows a curve somebody liked sits on the
        // animal's shoulder and reads as a strap.
        moveTo(424f, 250f)
        cubicTo(352f, 272f, 292f, 336f, 258f, 420f)
        cubicTo(228f, 506f, 214f, 616f, 220f, 802f)
        // …and back up the inner edge, the same curve pulled toward the animal
        // by the band's own width.
        lineTo(300f, 790f)
        cubicTo(300f, 640f, 316f, 520f, 352f, 436f)
        cubicTo(388f, 356f, 430f, 316f, 474f, 306f)
        close()
    }
    var union = band
    for ((cx, cy, r) in MANE_TUFTS) {
        val bump = Path().apply { addOval(Rect(cx - r, cy - r, cx + r, cy + r)) }
        val merged = Path()
        merged.op(union, bump, PathOperation.Union)
        union = merged
    }
    return union
}

/**
 * The scallops on the mane's outer edge, in master units.
 *
 * They sit a little **proud** of the fleece rather than inside it, which is
 * deliberate: hair hangs past the animal it grows on, and a mane whose outer
 * edge is flush with the silhouette is a painted stripe.
 */
private val MANE_TUFTS = listOf(
    Triple(372f, 268f, 42f),
    Triple(300f, 336f, 46f),
    Triple(252f, 428f, 46f),
    Triple(226f, 540f, 46f),
    Triple(220f, 660f, 44f),
    Triple(228f, 762f, 40f),
)

private fun DrawScope.drawMane(mane: Path, alpha: Float) {
    if (alpha <= 0f) return
    drawPath(
        mane,
        brush = Brush.verticalGradient(
            colors = RAIN,
            startY = 250f,
            endY = 800f,
        ),
        alpha = alpha.coerceIn(0f, 1f),
    )
    drawPath(mane, Ink, alpha = alpha.coerceIn(0f, 1f), style = Stroke(width = OUTLINE))
}

// ── the puck becomes the horn ──────────────────────────────────────────────

/**
 * ROUND 37 item 189 — **the morph, anchored at the puck's seat.**
 *
 * > *"THE LIDAR PUCK SPRITE STRETCHES INTO THE SPIRAL HORN — a morph anchored
 * > at the puck's seat (scaleY growth from the puck's base with the puck's
 * > silhouette blending into the horn shape)."*
 *
 * Both shapes are scaled about **[SEAT]** — the middle of the puck's own
 * contact line, which is where it stands on the llama's crown — and that single
 * shared anchor is what makes this a morph rather than a crossfade between two
 * pictures. The puck grows in y and narrows in x as it fades, so by the time it
 * is half gone it is already horn-shaped; the horn grows out of the same point
 * from a sixth of its height, overshoots by an eighth and settles, which is the
 * storyboard's `cubic-bezier(.3,1.4,.5,1)`.
 *
 * Drawing the puck **first** and the horn over it is the other half: for the
 * two frames they overlap, the horn's ink outline is what the eye reads, so the
 * fading puck looks like the thing inside it rather than a ghost beside it.
 */
private fun DrawScope.drawHornMorph(puck: ImageBitmap, f: WelcomeTimeline.FrameB, alpha: Float) {
    if (f.puckAlpha > 0f) {
        withTransform({ scale(f.puckNarrow, f.puckStretch, pivot = SEAT) }) {
            drawLayer(
                puck, Art.PUCK_LEFT, Art.PUCK_TOP, Art.PUCK_WIDTH, Art.PUCK_HEIGHT,
                f.puckAlpha * alpha,
            )
        }
        // ROUND 35 item 184 — the LED, on the emit point, outside the stretch:
        // a light that stretches is a light with a shape, and this one has to
        // stay the same lamp until the instant it is not there any more.
        drawLed(EMIT, f.puckAlpha * alpha)
    }
    if (f.hornAlpha > 0f) drawHorn(f.hornGrow, f.hornWidth, f.hornAlpha * alpha)
}

/**
 * The seat: the middle of the puck's contact line.
 *
 * Not [EMIT] and not the puck's centre. A horn grows out of the skull it is
 * rooted in, so the anchor is where the object **meets the animal** — and the
 * two things scaled about it are therefore both pinned to the crown for the
 * whole of the transformation, which is the only way neither of them can be
 * seen to float.
 */
private val SEAT = Offset(Art.PUCK_CENTER_X, Art.PUCK_FOOT_Y)

/** The horn's height at full growth, master units. */
private const val HORN_HEIGHT = 300f

/** …and its half-width where it leaves the crown. */
private const val HORN_HALF_BASE = 56f

/**
 * The forward lean, degrees. The animal is in profile facing right, and a horn
 * drawn dead vertical on a profile head is an aerial. Six is the angle at which
 * it reads as growing out of the brow rather than out of the ear.
 */
private const val HORN_TILT_DEG = 6f

/**
 * ROUND 37 item 189 — **the spiral horn**, as the storyboard draws it: a
 * tapered shape with a rainbow gradient, an ink outline and **three** spiral
 * grooves.
 *
 * The grooves are generated off the horn's own outline rather than placed —
 * each one runs from the left edge to the right edge at its own height, sloping
 * up as it crosses. That is the difference between three lines that are on the
 * horn and three lines that are near it: at [grow] = 0.15 and at 1.12 they are
 * still exactly on the taper, because they are computed from it.
 *
 * The storyboard's horn is a triangle (`M6 46 L14 2 L22 46 Z`). Ours has a
 * slight belly on each side, which is the same shape the master art gives
 * everything else it draws — there is not a straight line anywhere on this
 * llama.
 */
private fun DrawScope.drawHorn(grow: Float, widthScale: Float, alpha: Float) {
    if (alpha <= 0f || grow <= 0f) return
    val h = HORN_HEIGHT * grow
    val w = HORN_HALF_BASE * widthScale
    val cx = SEAT.x
    val baseY = SEAT.y
    val tipY = baseY - h

    // Half-width at a height p (0 = the crown, 1 = the tip), with the belly.
    fun halfAt(p: Float): Float = w * (1f - p) * (1f + 0.34f * p)

    withTransform({ rotate(HORN_TILT_DEG, pivot = SEAT) }) {
        val horn = Path().apply {
            moveTo(cx - w, baseY)
            quadraticTo(cx - w * 0.86f, baseY - h * 0.52f, cx, tipY)
            quadraticTo(cx + w * 0.86f, baseY - h * 0.52f, cx + w, baseY)
            close()
        }
        drawPath(
            horn,
            brush = Brush.verticalGradient(colors = RAIN.reversed(), startY = tipY, endY = baseY),
            alpha = alpha,
        )
        drawPath(horn, Ink, alpha = alpha, style = Stroke(width = OUTLINE))
        // Three grooves, each a chord of the taper with a rise on it — which is
        // what one turn of a spiral looks like from the side.
        for (at in HORN_GROOVES) {
            val y = baseY - h * at
            val half = halfAt(at)
            val rise = h * 0.055f
            val groove = Path().apply {
                moveTo(cx - half * 0.92f, y + rise * 0.5f)
                quadraticTo(cx, y - rise * 0.7f, cx + half * 0.92f, y - rise * 0.5f)
            }
            drawPath(
                groove, Ink,
                alpha = (alpha * 0.62f).coerceIn(0f, 1f),
                style = Stroke(width = OUTLINE * 0.55f, cap = StrokeCap.Round),
            )
        }
    }
}

/** Where the three grooves sit, as fractions of the horn's height. */
private val HORN_GROOVES = floatArrayOf(0.24f, 0.50f, 0.74f)

// ── the flash and the sparkles ─────────────────────────────────────────────

/**
 * ROUND 37 item 189 — the storyboard's `.flash`: a white radial burst centred
 * on the horn.
 *
 * In **screen** space, like animation A's rings and for the same reason — it is
 * something that happens to the picture, so it has to reach the corners of
 * whatever screen the picture is on. Its centre is read off the art box so that
 * it leaves from the horn on every device rather than from a screen fraction
 * that happens to be near it on one.
 */
private fun DrawScope.drawTransformFlash(f: WelcomeTimeline.FrameB, box: ArtBox) {
    val flash = f.flash * f.overlayAlpha
    if (flash <= 0f) return
    val centre = Offset(box.x(Art.PUCK_CENTER_X), box.y(Art.PUCK_TOP - HORN_HEIGHT * 0.4f))
    val radius = hypot(size.width, size.height) * 0.62f
    // The first cut of this was 0.92 at the core over 0.72 of the diagonal, and
    // the recording said what was wrong with it: three tenths of a second of a
    // near-white screen, with the flood and the mane — the two things the flash
    // is there to *hide the arrival of* — happening behind a sheet where nobody
    // could see them arrive OR be surprised by them. A flash is a wince, not an
    // exposure. At these numbers the llama is a shape all the way through it and
    // the rainbow is still a reveal on the frame after.
    drawCircle(
        brush = Brush.radialGradient(
            0.00f to Color.White.copy(alpha = 0.66f * flash),
            0.30f to Color.White.copy(alpha = 0.38f * flash),
            1.00f to Color.Transparent,
            center = centre,
            radius = radius,
        ),
        radius = radius,
        center = centre,
    )
}

/**
 * One four-point sparkle — the storyboard's `&#10022;`, drawn rather than set
 * in a font.
 *
 * Four concave-sided points, which is the shape a glyph gives and a cross does
 * not: a plain cross reads as a plus sign at this size. The long axis is twice
 * the short one, and it turns as it goes, per `.spark`'s own rotate.
 */
private fun DrawScope.drawSparkle(s: WelcomeTimeline.SparkleFrame, alpha: Float) {
    val a = (s.alpha * alpha).coerceIn(0f, 1f)
    if (a <= 0f || s.radius <= 0f) return
    val long = s.radius
    val short = s.radius * 0.30f
    val waist = s.radius * 0.14f
    val star = Path().apply {
        moveTo(s.x, s.y - long)
        quadraticTo(s.x + waist, s.y - waist, s.x + short, s.y)
        quadraticTo(s.x + waist, s.y + waist, s.x, s.y + long)
        quadraticTo(s.x - waist, s.y + waist, s.x - short, s.y)
        quadraticTo(s.x - waist, s.y - waist, s.x, s.y - long)
        close()
    }
    withTransform({ rotate(s.spinDeg, pivot = Offset(s.x, s.y)) }) {
        drawPath(star, Color.White, alpha = a)
    }
}

// ── the gallop's leavings ──────────────────────────────────────────────────

/**
 * ROUND 37 item 189 — **a rainbow trail band stretching behind and fading.**
 *
 * A **wedge** rather than a rectangle, and the taper is the fade: it is a
 * hair's width where the animal launched and full height where the animal is
 * now, so the far end of it is thin as well as faint. The storyboard's `.trail`
 * fades a bar with `opacity`, which on a phone's black page leaves a grey bar
 * for the last third of its life.
 *
 * It is drawn **before** the animal and under the ribbons, so the unicorn is
 * always in front of its own wake.
 */
private fun DrawScope.drawGallopTrail(f: WelcomeTimeline.FrameB, exit: Float, alpha: Float) {
    val a = (f.trailAlpha * alpha).coerceIn(0f, 1f)
    if (a <= 0f || f.gallopX <= 0f) return
    val from = TRAIL_FROM_X
    val to = from + f.gallopX * exit
    val mid = TRAIL_Y
    val half = TRAIL_HALF_HEIGHT
    val wedge = Path().apply {
        moveTo(from, mid - half * 0.10f)
        lineTo(to, mid - half)
        lineTo(to, mid + half)
        lineTo(from, mid + half * 0.10f)
        close()
    }
    // Two gradients at right angles, which needs a layer: the rainbow runs down
    // the band and the FADE runs along it. The taper alone was not enough — a
    // wedge two units tall still carries the ramp's middle stops at full alpha,
    // so the first recording ended in a hard olive spike hanging in the dark.
    drawIntoCanvas { canvas ->
        canvas.saveLayer(Rect(from - 20f, mid - half - 20f, to + 20f, mid + half + 20f), Paint())
        drawPath(
            wedge,
            brush = Brush.verticalGradient(
                colors = RAIN,
                startY = mid - half,
                endY = mid + half,
            ),
            alpha = a,
        )
        drawRect(
            brush = Brush.horizontalGradient(
                0.00f to Color.Transparent,
                0.45f to Color.White.copy(alpha = 0.55f),
                1.00f to Color.White,
                startX = from,
                endX = to,
            ),
            topLeft = Offset(from, mid - half),
            size = Size(to - from, half * 2f),
            blendMode = BlendMode.DstIn,
        )
        canvas.restore()
    }
}

/** Where the trail leaves from, and the band it occupies, master units. */
private const val TRAIL_FROM_X = 300f
private const val TRAIL_Y = 640f
private const val TRAIL_HALF_HEIGHT = 130f

/**
 * ROUND 37 item 189 — **one dust puff at the launch point.**
 *
 * Three soft discs on a radial brush that reaches zero at its own edge, for the
 * reason round 35 wrote down the hard way: a flat translucent circle keeps a
 * perfectly legible rim as it fades, so a puff drawn from flat discs ends its
 * life as three grey rings hanging in the air.
 *
 * It stays where the hooves were. Dust that travels is smoke.
 */
private fun DrawScope.drawDustPuff(f: WelcomeTimeline.FrameB, alpha: Float) {
    val a = (f.dustAlpha * alpha).coerceIn(0f, 1f)
    if (a <= 0f) return
    for ((dx, dy, scale) in DUST_PUFFS) {
        val r = 78f * scale * (0.45f + 1.15f * f.dustSpread)
        val at = Offset(
            BASE_PIVOT.x + dx * (0.4f + 1.6f * f.dustSpread),
            BASE_PIVOT.y + dy * (0.4f + 1.6f * f.dustSpread),
        )
        drawCircle(
            brush = Brush.radialGradient(
                0.0f to Dust.copy(alpha = a),
                0.5f to Dust.copy(alpha = a * 0.62f),
                1.0f to Color.Transparent,
                center = at,
                radius = r,
            ),
            radius = r,
            center = at,
        )
    }
}

/** The puff's three lobes: offset from the base, and each one's size. */
private val DUST_PUFFS = listOf(
    Triple(-46f, -18f, 1.00f),
    Triple(38f, -54f, 0.78f),
    Triple(96f, -12f, 0.62f),
)

/**
 * The dust's colour — pale and warm, lifted well off the page for the reason
 * round 35's mist had to be: a mid grey at low alpha over a near-black page
 * composites to a hole, not to a puff.
 */
private val Dust = Color(0xFFD8D2C6)
