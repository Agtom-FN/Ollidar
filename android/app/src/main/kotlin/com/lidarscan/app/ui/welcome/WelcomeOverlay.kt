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
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin

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
 * ## Why the front pose is drawn and not a bitmap
 *
 * A front-facing llama does not exist in the master art and could not be cut
 * from it. It is therefore the one drawn element in this round, built from the
 * same fleece, the same ink and the same outline weight, exactly as the
 * approved storyboard's front face was — and built as a single **unioned**
 * silhouette (head ∪ nine crown scallops ∪ two ears) so it carries one
 * continuous outline instead of a pile of overlapping circles.
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

/** The storyboard's water-blue droplet. */
private val Water = Color(0xFF7FD4E8)

/**
 * ROUND 35 item 185(b) — the mist's own colour: [Water] lifted most of the way
 * to white.
 *
 * Not [Water] at a low alpha. Mist is drawn over this app's page, which is
 * nearly black on the dark theme, and a translucent mid-blue over black is a
 * dark blue — the first two cuts photographed as grey smudges hanging beside
 * the llama's chin, which is the opposite of "airy". A pale tint at the same
 * alpha composites UP off the page and reads as spray in the air.
 */
private val Mist = Color(0xFFCDEAF4)

/**
 * ROUND 36 item 188 — **the covered lens.** A murky pale blue-green.
 *
 * Not [Water] and not [Mist]. What is on the reference's lens for its last
 * second and a bit is not water and is not clean: it is a translucent
 * green-grey film with the hillside behind it, and the two things that make it
 * read are that it is DESATURATED and that it is LIGHTER than everything it
 * covers. A cyan at this coverage would have been a colour wash on the film;
 * this is a substance on the glass.
 */
private val Murk = Color(0xFFA9CEC2)

/** The icon's outline weight, measured off the master art (median run 19, mean 23). */
private const val OUTLINE = 21f

/** The lighter weight the ears and facial strokes carry. */
private const val OUTLINE_FINE = 18f

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
    val description = when (variant) {
        WelcomeAnimation.Variant.LIDAR_FLIP -> "Welcome animation"
        WelcomeAnimation.Variant.LLAMA_SPIT -> "Welcome animation, developer"
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
        val front = remember { frontPoseArt() }

        Canvas(Modifier.fillMaxSize()) {
            val film = WelcomeAnimation.filmProgress(variant, progress.value)
            when (variant) {
                WelcomeAnimation.Variant.LIDAR_FLIP ->
                    drawLidarFlip(WelcomeTimeline.frameA(film), page, body, puck, fan, eye)

                WelcomeAnimation.Variant.LLAMA_SPIT ->
                    drawLlamaSpit(WelcomeTimeline.frameB(film), page, body, puck, eye, front)
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
 * ROUND 36 item 188 — and shut by [blink], which defaults to open so that
 * animation A, which has no blink in it, is the film it always was.
 *
 * A blink is a **squash and a lid**, not a swap to a second drawing: the sprite
 * is scaled toward a line about its own centre, and past halfway an ink stroke
 * of the icon's own weight is laid across it. The sprite alone would have been
 * a black eye becoming a black slit, which at this size is a llama looking down
 * rather than a llama blinking.
 */
private fun DrawScope.drawEye(
    eye: ImageBitmap,
    bob: Float,
    look: Float,
    alpha: Float = 1f,
    blink: Float = 0f,
) {
    withTransform({ translate(0f, bob + look) }) {
        val open = 1f - 0.94f * blink.coerceIn(0f, 1f)
        val centre = Offset(Art.EYE_CENTER_X, Art.EYE_CENTER_Y)
        withTransform({ scale(1f, open, pivot = centre) }) {
            drawLayer(
                eye,
                Art.EYE_CENTER_X - Art.EYE_SPRITE_WIDTH / 2f,
                Art.EYE_CENTER_Y - Art.EYE_SPRITE_HEIGHT / 2f,
                Art.EYE_SPRITE_WIDTH,
                Art.EYE_SPRITE_HEIGHT,
                alpha,
            )
        }
        if (blink > 0.45f) {
            val lid = ((blink - 0.45f) / 0.55f).coerceIn(0f, 1f)
            val half = Art.EYE_RADIUS * (1.05f + 0.25f * lid)
            drawLine(
                color = Ink,
                start = Offset(centre.x - half, centre.y),
                end = Offset(centre.x + half, centre.y),
                strokeWidth = OUTLINE,
                cap = StrokeCap.Round,
                alpha = (alpha * lid).coerceIn(0f, 1f),
            )
        }
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

/** The emitter's offset inside the puck sprite, for wherever else the puck is worn. */
private val EMIT_IN_PUCK = Offset(Art.EMIT_X - Art.PUCK_LEFT, Art.EMIT_Y - Art.PUCK_TOP)

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

// ══ ANIMATION B ═══════════════════════════════════════════════════════════

/**
 * ROUND 32 item 177 → ROUND 34 item 183 → ROUND 35 item 185 → **ROUND 36 item
 * 188: the reference video, drawn.**
 *
 * The owner handed over the short his note had been about and asked for its
 * *"2 sections of spit"* turned into this art style directly, so round 35's
 * choreography is gone entire. What is here is the reference's own structure:
 *
 *  * **the expectation** — the side llama leans in and blinks
 *    ([WelcomeTimeline.FrameB.lean], [WelcomeTimeline.FrameB.blink]);
 *  * **the cut** — the pinch to the front pose;
 *  * **the reality** — the jaw grinds side to side under a lidded stare
 *    ([WelcomeTimeline.FrameB.jawGrind], [WelcomeTimeline.FrameB.eyeNarrow]);
 *  * **the lens** — one frame of cone, and then the screen is gone
 *    ([drawLensCover]).
 *
 * The last of those is the only thing in either film that is drawn in **screen
 * space** rather than in master-art units, and it has to be: the joke is that
 * the *lens* is covered, and the lens is the whole phone. It is therefore
 * outside the art box's transform, at the very end, over everything.
 */
private fun DrawScope.drawLlamaSpit(
    f: WelcomeTimeline.FrameB,
    page: Color,
    body: ImageBitmap,
    puck: ImageBitmap,
    eye: ImageBitmap,
    front: FrontPoseArt,
) {
    if (f.overlayAlpha <= 0f) return
    drawRect(page, alpha = f.overlayAlpha)

    val box = artBox()
    withTransform({
        translate(box.left, box.top)
        scale(box.scale, box.scale, pivot = Offset.Zero)
    }) {
        val turnPivot = Offset(Art.CANVAS / 2f, FrontPose.CENTER_Y)

        // ── SECTION ONE: THE EXPECTATION ───────────────────────────────────
        //
        // The reference spends five seconds on an animal lowering its head into
        // a hand. Ours has one, and spends it the same way: a rotation about
        // the llama's own base, so the muzzle swings FORWARD and DOWN on the
        // arc a neck actually describes, plus seven per cent of scale, which is
        // what "toward you" is when there is no perspective to work with.
        //
        // Rotating about the base and not about the head: a head that pivots on
        // itself is a nod, and this is a reach.
        if (f.sideAlpha > 0f) {
            val a = f.sideAlpha * f.overlayAlpha
            withTransform({ scale(f.turnScaleX, 1f, pivot = turnPivot) }) {
                withTransform({
                    rotate(LEAN_DEG * f.lean, pivot = LEAN_PIVOT)
                    scale(1f + LEAN_SCALE * f.lean, 1f + LEAN_SCALE * f.lean, pivot = LEAN_PIVOT)
                }) {
                    drawBody(body, 0f, a)
                    drawEye(eye, 0f, 0f, a, f.blink)
                    drawLayer(puck, Art.PUCK_LEFT, Art.PUCK_TOP, Art.PUCK_WIDTH, Art.PUCK_HEIGHT, a)
                    drawLed(EMIT, a)
                }
            }
        }

        // ── SECTION TWO: THE REALITY ───────────────────────────────────────
        if (f.frontAlpha > 0f) {
            val a = f.frontAlpha * f.overlayAlpha
            withTransform({ scale(f.turnScaleX, 1f, pivot = turnPivot) }) {
                // The head barely moves through the stare — the jaw does all of
                // it, inside [drawFrontFace] — and then snaps forward and at the
                // viewer on the hit. One transform, three tracks.
                withTransform({
                    translate(0f, f.headRise)
                    rotate(f.headTiltDeg, pivot = FrontPose.NECK)
                    scale(f.headLunge, f.headLunge, pivot = FrontPose.NECK)
                }) {
                    drawFrontLlama(front, puck, eye, f, a)
                }
            }

            // The cone, for its one frame. Outside the head's transform: what
            // has left the mouth is in the room, and does not lunge or pinch
            // with the animal that threw it.
            val mouth = Offset(Art.CANVAS / 2f, FrontPose.MOUTH_Y + f.headRise)
            for (drop in f.spray) {
                val centre = Offset(mouth.x + drop.dx, mouth.y + drop.dy)
                val alpha = (drop.alpha * a).coerceIn(0f, 1f)
                if (alpha <= 0f || drop.radius <= 0f) continue
                if (drop.mist) {
                    // Soft, outline-free, low alpha — and drawn with a RADIAL
                    // GRADIENT rather than flat discs, which is the whole
                    // difference between mist and a smudge. Flat circles, at any
                    // alpha, keep a perfectly legible rim as they fade, so the
                    // burst ends with grey rings hanging in the air. A brush
                    // that reaches zero at its own edge has no rim to leave.
                    for ((ox, oy, or) in MIST_PUFF) {
                        val r = drop.radius * or
                        val at = Offset(centre.x + drop.radius * ox, centre.y + drop.radius * oy)
                        drawCircle(
                            brush = Brush.radialGradient(
                                0.0f to Mist.copy(alpha = alpha),
                                0.42f to Mist.copy(alpha = alpha * 0.72f),
                                1.0f to Color.Transparent,
                                center = at,
                                radius = r,
                            ),
                            radius = r,
                            center = at,
                        )
                    }
                } else {
                    val path = teardropPath(centre, drop.radius, drop.stretch)
                    withTransform({ rotate(drop.tiltDeg, pivot = centre) }) {
                        drawPath(path, Water, alpha = alpha)
                        drawPath(
                            path, Ink,
                            alpha = alpha,
                            style = Stroke(width = (drop.radius * 0.24f).coerceIn(4f, 10f)),
                        )
                    }
                }
            }
        }
    }

    // ── AND THEN THE LENS IS GONE ─────────────────────────────────────────
    drawLensCover(f, box)
}

/**
 * ROUND 36 item 188 §1 — **where the lean pivots, and how far it goes.**
 *
 * The pivot is the standing llama's own base, low and a little forward of the
 * fleece's centre of mass, measured off `welcome_llama_body.webp`. Seven
 * degrees about it carries the muzzle — which is up at roughly (760, 540) on
 * the master canvas, since this animal faces right — about eighty units
 * forward and forty down, which is a llama leaning in and is not a llama
 * falling over. Twelve degrees was tried and reads as a stumble.
 */
private val LEAN_PIVOT = Offset(430f, 960f)
private const val LEAN_DEG = 7f

/** …plus this much scale, which is "toward you" in a drawing with no perspective. */
private const val LEAN_SCALE = 0.07f

/**
 * ROUND 36 item 188 — **the covered lens, in screen space.**
 *
 * Three layers, in the order the reference has them:
 *
 *  1. **the wash** — the whole screen, at [WelcomeTimeline.B_WASH_ALPHA],
 *     which is what makes the llama a dim shape rather than a hidden one;
 *  2. **the blobs and streaks** — [WelcomeTimeline.B_LENS], each with the
 *     lumpy ink outline the icon's language gives anything with an edge;
 *  3. **the drips** — two of them, running for the whole hold.
 *
 * The **clear patch** is taken *out* of the finished cover rather than painted
 * over the top of it, and that is the one structural thing in this function:
 * all three layers go into one `saveLayer`, and a radial `DstOut` brush then
 * removes about two thirds of the alpha over the llama's mouth. Painting a pale
 * disc on top would have put a pale disc on top — the grin has to be seen
 * *through* the goo, not beside it. And the brush is radial rather than a flat
 * disc because a flat one is a porthole, and stuff running off glass does not
 * leave a circle.
 *
 * Two thirds and not all of it: [WelcomeTimeline.B_PATCH_ALPHA] over
 * [WelcomeTimeline.B_WASH_ALPHA] is exactly the ratio, so the patch is as thin
 * as the timeline says and no thinner. The blob table is separately laid out
 * with its centres clear of that spot ([WelcomeTimeline.lensGapClearance]), so
 * there is never an opaque core sitting where the hole is.
 */
private fun DrawScope.drawLensCover(f: WelcomeTimeline.FrameB, box: ArtBox) {
    val cover = f.lensWash * f.overlayAlpha
    if (cover <= 0f) return
    val w = size.width
    val h = size.height

    // The thin place goes on the llama's OWN mouth, read off the art box, not
    // on a screen fraction that happens to be near it: the box is fitted to the
    // narrower of 80 % of the width and 46 % of the height, so on a squarer
    // screen the mouth is most of a tenth of the screen lower than it is on a
    // tall handset.
    val patch = Offset(box.x(Art.CANVAS / 2f), box.y(FrontPose.MOUTH_Y))
    val patchR = w * WelcomeTimeline.B_PATCH_RADIUS
    fun murk(alpha: Float) = Murk.copy(alpha = (alpha * cover).coerceIn(0f, 1f))

    drawIntoCanvas { canvas ->
        canvas.saveLayer(Rect(0f, 0f, w, h), Paint())

        // 1. the wash — the whole screen, which is what makes the llama a dim
        // shape behind it rather than a hidden one.
        drawRect(color = murk(WelcomeTimeline.B_WASH_ALPHA))

        // 2. the blobs, the streaks and what ran off them.
        for (blob in f.lens) {
            val alpha = (blob.alpha * f.overlayAlpha).coerceIn(0f, 1f)
            if (alpha <= 0f) continue
            val centre = Offset(w * blob.x, h * blob.y)
            val r = w * blob.radius
            val edge = (r * 0.06f).coerceIn(3f, 13f)
            // The ink does NOT follow the fill. At these alphas the fill is a
            // tint and an outline drawn as a fraction of it would vanish; and
            // the streaks get no outline at all, because a smear is what a blob
            // looks like when it has been dragged and a smear has no rim.
            val ink = if (blob.radius >= WelcomeTimeline.B_LENS_INK_MIN_RADIUS) {
                (blob.ink * f.overlayAlpha).coerceIn(0f, 1f)
            } else {
                0f
            }
            // Rotate FIRST and stretch second, so the stretch lies along the
            // angle rather than along the screen: that is the difference
            // between a streak thrown across the glass and a squashed blob.
            withTransform({
                rotate(blob.angleDeg, pivot = centre)
                scale(blob.stretch, 1f, pivot = centre)
            }) {
                val path = splatPath(centre, r, blob.seed)
                drawPath(path, Murk, alpha = alpha)
                if (ink > 0f) drawPath(path, Ink, alpha = ink, style = Stroke(width = edge))
            }

            // …and the couple of specks that carried on past each one. Separate
            // shapes and not lobes of the blob, because a droplet that has left
            // the puddle has air round it.
            for (k in 0 until 2) {
                val (bearing, distance, scale) =
                    SPLAT_SATELLITES[(k * 3 + blob.seed) % SPLAT_SATELLITES.size]
                val sr = r * scale * 0.55f
                val at = Offset(
                    centre.x + (cos(bearing.toDouble()) * r * distance).toFloat(),
                    centre.y + (sin(bearing.toDouble()) * r * distance * 0.86f).toFloat(),
                )
                val topLeft = Offset(at.x - sr, at.y - sr * 0.9f)
                val ovalSize = Size(sr * 2f, sr * 1.8f)
                drawOval(Murk, topLeft, ovalSize, alpha = alpha)
                drawOval(
                    Ink, topLeft, ovalSize,
                    alpha = ink * 0.8f,
                    style = Stroke(width = edge * 0.6f),
                )
            }

            // 3. …and the two that run, for the whole of the hold.
            if (blob.drip > 0f) {
                // It leaves from INSIDE the blob, not from its lower edge: a
                // tongue that starts where the mass ends is a shape hanging in
                // the air, and the first cut of this photographed as two pale
                // skittles on the glass.
                val from = Offset(centre.x + r * 0.10f, centre.y + r * 0.45f)
                val len = h * blob.drip
                val neck = (r * 0.34f).coerceIn(20f, 110f)
                val trail = Path().apply {
                    moveTo(from.x - neck, from.y)
                    quadraticTo(from.x - neck * 0.85f, from.y + len * 0.7f, from.x, from.y + len)
                    quadraticTo(from.x + neck * 0.85f, from.y + len * 0.7f, from.x + neck, from.y)
                    close()
                }
                // Denser than the blob it came off. A drip drawn at the same
                // alpha over a screen that is already this colour is not there.
                drawPath(
                    trail, Murk,
                    alpha = (alpha * WelcomeTimeline.B_DRIP_DENSITY).coerceIn(0f, 1f),
                )
                // …and no outline on it. Ink round a drip draws the eye to the
                // one thing on this screen that is supposed to be a smear.
            }
        }

        // …and then the bite, out of all three at once.
        if (f.lensPatch > 0f) {
            val bite =
                (1f - WelcomeTimeline.B_PATCH_ALPHA / WelcomeTimeline.B_WASH_ALPHA) * f.lensPatch
            drawCircle(
                brush = Brush.radialGradient(
                    0.00f to Color.Black.copy(alpha = bite),
                    0.55f to Color.Black.copy(alpha = bite * 0.74f),
                    1.00f to Color.Transparent,
                    center = patch,
                    radius = patchR,
                ),
                radius = patchR,
                center = patch,
                blendMode = BlendMode.DstOut,
            )
        }

        canvas.restore()
    }
}

/**
 * The front-facing llama, in the order the side art is built in: the fleece as
 * one silhouette, the inner-ear folds inside it, the smooth face patch over the
 * top of it, then the hat, the light and the face.
 *
 * ROUND 35 item 185(e): the pose is round 34's, unchanged, with **one**
 * addition — the ears are no longer baked into the silhouette. They are unioned
 * into it every frame at their current pin, which is two `Path.op` calls and
 * keeps the single unbroken outline that the whole pose is built around. Ears
 * drawn as separate shapes behind the head would have been free and would have
 * put a seam where the icon's language has none.
 */
private fun DrawScope.drawFrontLlama(
    front: FrontPoseArt,
    puck: ImageBitmap,
    eye: ImageBitmap,
    f: WelcomeTimeline.FrameB,
    a: Float,
) {
    val silhouette = Path().apply {
        val withLeft = Path()
        withLeft.op(front.head, earPath(-1f, f.earPin), PathOperation.Union)
        op(withLeft, earPath(1f, f.earPin), PathOperation.Union)
    }
    drawPath(silhouette, Fleece, alpha = a)
    drawPath(silhouette, Ink, alpha = a, style = Stroke(width = OUTLINE))
    for (side in floatArrayOf(-1f, 1f)) {
        drawPath(
            innerEarPath(side, f.earPin), Ink,
            alpha = a,
            style = Stroke(width = INNER_EAR_WEIGHT, cap = StrokeCap.Round),
        )
    }
    drawPath(front.facePatch, Fleece, alpha = a)
    drawPath(front.facePatch, Ink, alpha = a, style = Stroke(width = OUTLINE))

    // The hat rides the crown, clear of the fluff so the brim is readable — the
    // same object the side pose wears, not a drawn stand-in for it.
    val hatLeft = Art.CANVAS / 2f - Art.PUCK_WIDTH / 2f
    drawLayer(puck, hatLeft, FrontPose.HAT_TOP, Art.PUCK_WIDTH, Art.PUCK_HEIGHT, a)
    // ROUND 35 item 184 — the light goes where the emitter actually is on the
    // sprite the llama is wearing. Round 34 drew it at the master anchor, which
    // put an orange glow in the air to the right of the front pose's hat.
    drawLed(
        Offset(hatLeft + EMIT_IN_PUCK.x, FrontPose.HAT_TOP + EMIT_IN_PUCK.y),
        a,
    )
    drawFrontFace(eye, a, f.mouthOpen, f.jawGrind, f.eyeNarrow, f.grinAlpha)
}

// ── the drawn front pose ───────────────────────────────────────────────────

/**
 * ROUND 32 item 177 → ROUND 34 item 183(b) → **ROUND 35 item 185(a)(e).**
 *
 * The owner, on the 0.9.17 footage: *"the front view of the llama is not the
 * normal llama look."* He was right, and the reason was structural. The side
 * sprite is a fluffy cloud silhouette with a **smooth face patch** inside it,
 * each carrying one continuous ink outline, and the fleece of the cloud
 * overlaps the top of the patch in scallops. Round 32's front pose had fluff on
 * the CROWN only, no face patch at all, two tapered ears and one eye — a sheep
 * in a hat.
 *
 * Round 34 rebuilt it the way the side art is built and the owner accepted it;
 * **round 35 changes nothing about the drawing** except what item 185(e) asks
 * for: the ears come out of the baked silhouette so they can lay back, and the
 * mouth gains an open and a curled state. Every other number here was set
 * against a large side-by-side of this pose and the side sprite, which is the
 * only way to answer "does it match the art".
 */
private object FrontPose {
    const val CENTER_X = 512f
    const val CENTER_Y = 556f
    const val RADIUS_X = 268f
    const val RADIUS_Y = 300f

    /** The smooth face patch inside the fleece. */
    const val FACE_RADIUS_X = 168f
    const val FACE_RADIUS_Y = 210f
    const val FACE_CENTER_Y = CENTER_Y + 40f

    /** The ears: base offset, base line, height, half-width, and the tip's half-width. */
    const val EAR_DX = 132f
    const val EAR_BASE_Y = CENTER_Y - RADIUS_Y + 118f
    const val EAR_HEIGHT = 268f
    const val EAR_HALF_WIDTH = 98f
    const val EAR_TIP_HALF = 36f

    const val MUZZLE_CENTER_Y = FACE_CENTER_Y + 98f
    const val MUZZLE_RADIUS_X = 80f
    const val MUZZLE_RADIUS_Y = 60f

    /** Where the spray leaves, and therefore where every mark is measured from. */
    const val MOUTH_Y: Float = MUZZLE_CENTER_Y + 24f

    /** The crown, where the hat sits. */
    const val HAT_TOP: Float = CENTER_Y - RADIUS_Y - 88f

    /**
     * ROUND 35 item 185(a) — the base of the neck: the pivot the head cocks
     * back on and lunges from. Below the silhouette, so the chin travels and
     * the neck does not.
     */
    val NECK: Offset = Offset(CENTER_X, CENTER_Y + RADIUS_Y)
}

/**
 * The static half of the front pose, built once per composition.
 *
 * The **ears are not in it** any more (item 185(e)): they change every frame,
 * so they are unioned in at draw time. Everything that does not move is still
 * built once, because `Path.op` on seventeen scallops is not a per-frame cost
 * anybody needs to pay.
 */
private class FrontPoseArt(
    val head: Path,
    val facePatch: Path,
)

private fun frontPoseArt(): FrontPoseArt = FrontPoseArt(
    head = frontHeadPath(),
    facePatch = frontFacePatchPath(),
)

/**
 * Head ∪ seventeen scallops, as **one** path.
 *
 * Unioned rather than stacked: a stack of filled-and-stroked shapes leaves
 * every interior arc showing, and the icon's language is a single unbroken
 * outline round the whole animal.
 */
private fun frontHeadPath(): Path {
    var union = Path().apply {
        addOval(
            Rect(
                FrontPose.CENTER_X - FrontPose.RADIUS_X,
                FrontPose.CENTER_Y - FrontPose.RADIUS_Y,
                FrontPose.CENTER_X + FrontPose.RADIUS_X,
                FrontPose.CENTER_Y + FrontPose.RADIUS_Y,
            ),
        )
    }
    // The fleece, all the way round. Alternating radii, because a ring of
    // equal bumps reads as a gear and the master art's fluff is irregular.
    for (i in 0 until FRONT_SCALLOPS) {
        val angle = PI * (0.80 + 2.0 * i / FRONT_SCALLOPS)
        val cx = FrontPose.CENTER_X + (cos(angle) * FrontPose.RADIUS_X * 0.96).toFloat()
        val cy = FrontPose.CENTER_Y + (sin(angle) * FrontPose.RADIUS_Y * 0.96).toFloat()
        val r = if (i % 2 == 0) 74f else 56f
        val bump = Path().apply { addOval(Rect(cx - r, cy - r, cx + r, cy + r)) }
        val next = Path()
        next.op(union, bump, PathOperation.Union)
        union = next
    }
    return union
}

private const val FRONT_SCALLOPS = 17

/**
 * ROUND 35 item 185(a) — **how far back a pinned ear goes.**
 *
 * Seventy-eight degrees, which from the front puts the tip out sideways and
 * very slightly down: the silhouette loses its two uprights and gains two
 * horizontal blades against the skull, which is the shape the tell is. Ninety
 * would have laid them dead flat and read as a shrug; sixty still reads as an
 * ear that is merely tilted.
 */
private const val EAR_PIN_DEG = 78f

/**
 * The ear's own frame: rotate about the base by the pin, and **foreshorten**
 * along its length as it goes.
 *
 * The foreshortening is the half that makes it three-dimensional. An ear that
 * rotates at full length is a blade sweeping round a hub; a real ear laid back
 * is also pointing away from the camera, so it gets shorter, and a third of its
 * length is what that looks like at this angle.
 */
private class EarFrame(side: Float, pin: Float) {
    private val bx = FrontPose.CENTER_X + side * FrontPose.EAR_DX
    private val by = FrontPose.EAR_BASE_Y
    private val rad = side * EAR_PIN_DEG * pin * PI.toFloat() / 180f
    private val cs = cos(rad.toDouble()).toFloat()
    private val sn = sin(rad.toDouble()).toFloat()
    private val squash = 1f - 0.34f * pin

    fun x(ax: Float, ay: Float): Float = bx + (ax - bx) * cs - (ay - by) * squash * sn
    fun y(ax: Float, ay: Float): Float = by + (ax - bx) * sn + (ay - by) * squash * cs
}

/**
 * One ear: wide at the base, canted out, **round at the tip**, laid back by
 * [pin].
 *
 * Three curves rather than two, and the third is the whole point — it is the
 * tip's own arc. Round 32's ears met at a single point and came out as horns;
 * the master art's ears are leaves with a rounded end, and a rounded end needs
 * two points and a curve between them.
 */
private fun earPath(side: Float, pin: Float): Path {
    val bx = FrontPose.CENTER_X + side * FrontPose.EAR_DX
    val by = FrontPose.EAR_BASE_Y
    val h = FrontPose.EAR_HEIGHT
    val w = FrontPose.EAR_HALF_WIDTH
    val tip = FrontPose.EAR_TIP_HALF
    val e = EarFrame(side, pin)
    fun move(path: Path, x: Float, y: Float) = path.moveTo(e.x(x, y), e.y(x, y))
    fun quad(path: Path, cx: Float, cy: Float, x: Float, y: Float) =
        path.quadraticTo(e.x(cx, cy), e.y(cx, cy), e.x(x, y), e.y(x, y))
    return Path().apply {
        move(this, bx - side * w, by)
        quad(this, bx - side * (w + 30f), by - h * 0.55f, bx + side * (18f - tip), by - h)
        quad(this, bx + side * 22f, by - h - 26f, bx + side * (18f + tip), by - h + 10f)
        quad(this, bx + side * (w + 52f), by - h * 0.45f, bx + side * w, by)
        close()
    }
}

/**
 * The inner ear — a stroke, the way the side sprite draws its own.
 *
 * Kept well inside the ear and drawn at the LIGHTEST weight in the file. The
 * first cut used the same fine weight as the facial strokes and ran the shape
 * three quarters of the way up: photographed beside the side sprite it filled
 * the ear and the pair read as two dark blades rather than as two ears with a
 * fold in each. This is the same drawing, thinner and shorter, and it rides the
 * same [EarFrame] as the ear it is inside.
 */
private const val INNER_EAR_WEIGHT = 12f

private fun innerEarPath(side: Float, pin: Float): Path {
    val bx = FrontPose.CENTER_X + side * FrontPose.EAR_DX
    val by = FrontPose.EAR_BASE_Y - 40f
    val h = FrontPose.EAR_HEIGHT
    val e = EarFrame(side, pin)
    fun p(x: Float, y: Float) = Offset(e.x(x, y), e.y(x, y))
    val a = p(bx - side * 24f, by)
    val c1 = p(bx - side * 30f, by - h * 0.34f)
    val t = p(bx + side * 14f, by - h * 0.62f)
    val c2 = p(bx + side * 42f, by - h * 0.30f)
    val z = p(bx + side * 26f, by)
    return Path().apply {
        moveTo(a.x, a.y)
        quadraticTo(c1.x, c1.y, t.x, t.y)
        quadraticTo(c2.x, c2.y, z.x, z.y)
    }
}

/** The smooth face, with the fleece scalloping over its forehead. */
private fun frontFacePatchPath(): Path {
    var union = Path().apply {
        addOval(
            Rect(
                FrontPose.CENTER_X - FrontPose.FACE_RADIUS_X,
                FrontPose.FACE_CENTER_Y - FrontPose.FACE_RADIUS_Y,
                FrontPose.CENTER_X + FrontPose.FACE_RADIUS_X,
                FrontPose.FACE_CENTER_Y + FrontPose.FACE_RADIUS_Y,
            ),
        )
    }
    for (i in 0 until 7) {
        val angle = PI * (1.08 + 0.84 * i / 6.0)
        val cx = FrontPose.CENTER_X + (cos(angle) * FrontPose.FACE_RADIUS_X * 1.02).toFloat()
        val cy = FrontPose.FACE_CENTER_Y + (sin(angle) * FrontPose.FACE_RADIUS_Y * 1.02).toFloat()
        val r = if (i % 2 == 0) 50f else 40f
        val bump = Path().apply { addOval(Rect(cx - r, cy - r, cx + r, cy + r)) }
        val next = Path()
        next.op(union, bump, PathOperation.Union)
        union = next
    }
    return union
}

/**
 * ROUND 36 item 188 §2 — **where the jaw pivots, and how far it grinds.**
 *
 * The pivot is up under the eyes rather than on the muzzle itself, because a
 * jaw is hinged behind the face: rotating about the muzzle's own centre spins
 * the nose and leaves the chin where it was, which is a face made of rubber.
 * Twenty-six units of slide with four and a half degrees of roll on top is what
 * the reference's animal does — the slide is most of it, and the roll is the
 * part that stops the muzzle looking like a sticker being dragged about.
 */
private val JAW_PIVOT =
    Offset(FrontPose.CENTER_X, FrontPose.MUZZLE_CENTER_Y - FrontPose.MUZZLE_RADIUS_Y - 40f)
private const val JAW_SHIFT = 26f
private const val JAW_ROLL_DEG = 4.5f

/** Eyes narrowed onto the stare, the jaw grinding under them, and the grin. */
private fun DrawScope.drawFrontFace(
    eye: ImageBitmap,
    alpha: Float,
    mouthOpen: Float,
    jawGrind: Float,
    eyeNarrow: Float,
    grinAlpha: Float,
) {
    // TWO eyes, and they are the real sprite, because the side pose's eye is a
    // real sprite and a drawn circle beside it would not be the same black.
    //
    // ROUND 36 item 188 — they LID as the stare sets in: the sprite squashes
    // toward a line about its own centre and a brow comes down over it, sloping
    // toward the nose. The squash alone is an eye half shut; the brow is the
    // half that says the animal has decided something.
    for (side in intArrayOf(-1, 1)) {
        val s = side.toFloat()
        val cx = FrontPose.CENTER_X + s * 80f
        val cy = FrontPose.FACE_CENTER_Y - 62f
        val open = 1f - 0.44f * eyeNarrow
        withTransform({ scale(1f, open, pivot = Offset(cx, cy)) }) {
            drawLayer(
                eye,
                cx - Art.EYE_SPRITE_WIDTH / 2f,
                cy - Art.EYE_SPRITE_HEIGHT / 2f,
                Art.EYE_SPRITE_WIDTH,
                Art.EYE_SPRITE_HEIGHT,
                alpha,
            )
        }
        if (eyeNarrow > 0f) {
            val browY = cy - Art.EYE_SPRITE_HEIGHT / 2f * open - 14f
            val brow = Path().apply {
                moveTo(cx + s * 46f, browY - 10f * eyeNarrow)
                quadraticTo(cx, browY + 2f * eyeNarrow, cx - s * 44f, browY + 22f * eyeNarrow)
            }
            drawPath(
                brow, Ink,
                alpha = (alpha * eyeNarrow).coerceIn(0f, 1f),
                style = Stroke(width = INNER_EAR_WEIGHT, cap = StrokeCap.Round),
            )
        }
    }

    // ROUND 36 item 188 §2 — **THE JAW.** Everything below the eyes rides it:
    // the muzzle, both nostrils, the mouth and the lip go across the face
    // together, because a muzzle that slides while its nostrils stay put is two
    // drawings rather than one animal.
    withTransform({
        translate(jawGrind * JAW_SHIFT, 0f)
        rotate(jawGrind * JAW_ROLL_DEG, pivot = JAW_PIVOT)
    }) {
        // The muzzle: small, rounded, low on the face — filled fleece and
        // outlined at the finer weight the facial strokes carry.
        val muzzle = Rect(
            FrontPose.CENTER_X - FrontPose.MUZZLE_RADIUS_X,
            FrontPose.MUZZLE_CENTER_Y - FrontPose.MUZZLE_RADIUS_Y,
            FrontPose.CENTER_X + FrontPose.MUZZLE_RADIUS_X,
            FrontPose.MUZZLE_CENTER_Y + FrontPose.MUZZLE_RADIUS_Y,
        )
        drawOval(Fleece, muzzle.topLeft, muzzle.size, alpha = alpha)
        drawOval(
            Ink, muzzle.topLeft, muzzle.size,
            alpha = alpha,
            style = Stroke(width = OUTLINE_FINE),
        )
        // The nostrils are CURVES, not dots. Three dots inside an oval — which
        // is what the first cut drew — reads as a snout; the master art's
        // nostril is a hook, and two hooks over a mouth is a llama's face.
        for (side in intArrayOf(-1, 1)) {
            val s = side.toFloat()
            val cx = FrontPose.CENTER_X + s * 33f
            val cy = FrontPose.MUZZLE_CENTER_Y - 26f
            val hook = Path().apply {
                moveTo(cx - s * 15f, cy - 9f)
                quadraticTo(cx - s * 4f, cy + 9f, cx + s * 14f, cy + 1f)
            }
            drawPath(
                hook, Ink,
                alpha = alpha,
                style = Stroke(width = INNER_EAR_WEIGHT, cap = StrokeCap.Round),
            )
        }

        // The mouth: a crack that works with each grind and goes WIDE on the
        // hit. One dark oval growing in both axes rather than three drawings,
        // so there is no frame where one shape swaps for another.
        val mw = 28f + 30f * mouthOpen
        val mh = 18f + 58f * mouthOpen
        drawOval(
            Ink,
            Offset(FrontPose.CENTER_X - mw / 2f, FrontPose.MOUTH_Y - 9f),
            Size(mw, mh),
            alpha = alpha,
        )

        // The lip, flat and level. Round 35 curled it as a smug beat between
        // the mess and the grin; item 188 has no such beat — the mess IS the
        // last beat — so it is a line again and the grin does the work.
        val lip = Path().apply {
            moveTo(FrontPose.CENTER_X - 30f, FrontPose.MOUTH_Y + 20f)
            quadraticTo(
                FrontPose.CENTER_X, FrontPose.MOUTH_Y + 30f,
                FrontPose.CENTER_X + 30f, FrontPose.MOUTH_Y + 20f,
            )
        }
        drawPath(
            lip, Ink,
            alpha = alpha,
            style = Stroke(width = INNER_EAR_WEIGHT, cap = StrokeCap.Round),
        )

        if (grinAlpha > 0f) {
            val grin = Path().apply {
                moveTo(FrontPose.CENTER_X - 44f, FrontPose.MOUTH_Y + 6f)
                quadraticTo(
                    FrontPose.CENTER_X, FrontPose.MOUTH_Y + 34f,
                    FrontPose.CENTER_X + 44f, FrontPose.MOUTH_Y + 6f,
                )
            }
            drawPath(
                grin, Ink,
                alpha = (grinAlpha * alpha).coerceIn(0f, 1f),
                style = Stroke(width = OUTLINE_FINE, cap = StrokeCap.Round),
            )
        }
    }
}

/**
 * ROUND 34 item 183(c) → ROUND 35 item 185(b) — **a drop of water.**
 *
 * A bulb with a tail, drawn about [centre] with the tail pointing **up**, which
 * the caller then rotates so it points back along the flight the particle
 * actually flew. [stretch] is length ÷ width and comes from the timeline, so
 * the shape carries the speed: fast is long and thin, slow rounds up again.
 * That is the single thing that most makes a moving drop read as liquid rather
 * than as a shape being translated — and with fourteen of them at fourteen
 * different speeds it is what stops the burst from looking like confetti.
 */
private fun teardropPath(centre: Offset, halfWidth: Float, stretch: Float): Path {
    val w = halfWidth
    val h = w * 2.0f * stretch
    val tipY = centre.y - h
    return Path().apply {
        moveTo(centre.x, tipY)
        cubicTo(
            centre.x + w * 0.62f, tipY + h * 0.42f,
            centre.x + w, centre.y - w * 0.55f,
            centre.x + w, centre.y,
        )
        cubicTo(
            centre.x + w, centre.y + w * 1.28f,
            centre.x - w, centre.y + w * 1.28f,
            centre.x - w, centre.y,
        )
        cubicTo(
            centre.x - w, centre.y - w * 0.55f,
            centre.x - w * 0.62f, tipY + h * 0.42f,
            centre.x, tipY,
        )
        close()
    }
}

/**
 * ROUND 35 item 185(b) — one mist puff's own shape: three soft blobs, as
 * offsets and radii in fractions of the puff's radius.
 *
 * Fixed, like everything else in this file, and asymmetric on purpose: a
 * rosette that is symmetric about either axis reads as a flower again.
 */
private val MIST_PUFF = listOf(
    Triple(0.00f, 0.00f, 1.00f),
    Triple(0.52f, -0.30f, 0.74f),
    Triple(-0.58f, 0.24f, 0.68f),
)

/**
 * ROUND 35 item 185(c) — the few droplets that carry on past each impact:
 * bearing (radians), distance as a fraction of the mark's radius, and size as
 * another.
 *
 * Separate paths and not lobes of the mark: a droplet that has left the puddle
 * has air round it, and this is the difference between a mark that was **hit**
 * and a blob that was placed. Phased by the mark's own seed, so no two of the
 * six carry the same pattern.
 */
private val SPLAT_SATELLITES = listOf(
    Triple(-2.35f, 1.44f, 0.19f),
    Triple(-0.55f, 1.32f, 0.15f),
    Triple(0.72f, 1.58f, 0.12f),
    Triple(2.34f, 1.28f, 0.17f),
    Triple(1.55f, 1.62f, 0.11f),
)

/**
 * One splat's outline, built from [WelcomeTimeline]'s own two tables.
 *
 * ROUND 36 item 188: the tables moved to `:core`, because the item's *"~90 %
 * of the screen"* is a claim about the area this path encloses and a unit test
 * has to be able to build the same polygon. Nothing about the shape changed.
 *
 * @param seed which blob this is. It rolls both tables by a different amount
 *   for each one, so that eighteen blobs on one screen are eighteen shapes
 *   rather than one shape eighteen times.
 */
private fun splatPath(centre: Offset, radius: Float, seed: Int): Path {
    val path = Path()
    val lobes = WelcomeTimeline.SPLAT_LOBES.size
    for (i in 0 until lobes) {
        val r = radius * WelcomeTimeline.splatLobe(seed, i)
        val a = WelcomeTimeline.splatAngle(seed, i)
        val x = centre.x + (cos(a) * r).toFloat()
        val y = centre.y + (sin(a) * r * WelcomeTimeline.SPLAT_SQUASH).toFloat()
        if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
    path.close()
    var splat = path
    for (b in WelcomeTimeline.SPLAT_BLOBS.indices) {
        val (dx, dy, dr) = WelcomeTimeline.splatBlob(seed, b)
        val cx = centre.x + dx * radius
        val cy = centre.y + dy * radius * WelcomeTimeline.SPLAT_SQUASH
        val r = dr * radius
        val blob = Path().apply { addOval(Rect(cx - r, cy - r, cx + r, cy + r)) }
        val merged = Path()
        merged.op(splat, blob, PathOperation.Union)
        splat = merged
    }
    return splat
}
