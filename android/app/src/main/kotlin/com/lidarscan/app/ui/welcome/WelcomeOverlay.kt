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
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathOperation
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
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

/** The launcher icon's own frame orange, measured off the master. Deeper than [Flame]. */
private val IconFrame = Color(0xFFE55B2A)

/** …and the paper it stands on. */
private val IconPaper = Color(0xFFF1F2ED)

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

/**
 * The launcher icon's squircle, approximated as a rounded rectangle, and the
 * paper inside it — both measured off the 1024 px master
 * (frame band 51 units, outer corner ≈ 200, inner ≈ 150).
 */
private const val ICON_CORNER = 200f
private const val ICON_BAND = 51f
private const val ICON_INNER_CORNER = 150f

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
    val cx = box.x(Art.PUCK_CENTER_X)
    val cy = box.y(Art.PUCK_CENTER_Y)
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
 * ROUND 32 item 177 — **the ground the illustration stands on.**
 *
 * The cut body layer is an OUTLINE: its fluff interior is transparent, because
 * in the launcher icon the fleece and the icon's paper are the same colour and
 * the artist never drew a fill. Composited straight onto this app's page — dark
 * by default — the llama came out as a see-through scribble with a floating
 * white face. That was found by watching the recording, which is the entire
 * reason the item asks for one.
 *
 * The fix is not to invent a fill. It is to give the film the ground the
 * artwork was drawn for: **the app icon itself**, at size, with the paper and
 * the orange frame reproduced from the master's own measurements. The
 * storyboard had already said so and it was missed on the first read — its
 * stage is `background:#F2F1EC; border:3px solid; border-radius:24px`, a light
 * panel, not a bare page.
 *
 * And it earns its place beyond the bug: the thing the operator tapped to get
 * here is this icon, so the film now starts inside it, throws the lidar clear
 * of it, and bursts the scan rings out past its edge to the corners of the
 * screen.
 */
private fun DrawScope.drawIconGround(alpha: Float) {
    if (alpha <= 0f) return
    drawRoundRect(
        color = IconFrame,
        topLeft = Offset.Zero,
        size = Size(Art.CANVAS, Art.CANVAS),
        cornerRadius = CornerRadius(ICON_CORNER, ICON_CORNER),
        alpha = alpha,
    )
    drawRoundRect(
        color = IconPaper,
        topLeft = Offset(ICON_BAND, ICON_BAND),
        size = Size(Art.CANVAS - 2 * ICON_BAND, Art.CANVAS - 2 * ICON_BAND),
        cornerRadius = CornerRadius(ICON_INNER_CORNER, ICON_INNER_CORNER),
        alpha = alpha,
    )
}

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
            animationSpec = tween(durationMillis = WelcomeAnimation.DURATION_MS, easing = LinearEasing),
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
        val front = remember { frontPosePath() }

        Canvas(Modifier.fillMaxSize()) {
            when (variant) {
                WelcomeAnimation.Variant.LIDAR_FLIP ->
                    drawLidarFlip(WelcomeTimeline.frameA(progress.value), page, body, puck, fan, eye)

                WelcomeAnimation.Variant.LLAMA_SPIT ->
                    drawLlamaSpit(WelcomeTimeline.frameB(progress.value), page, body, puck, eye, front)
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

/** The eye, at its anchor, carried by the body's [bob] and its own [look]. */
private fun DrawScope.drawEye(eye: ImageBitmap, bob: Float, look: Float, alpha: Float = 1f) {
    withTransform({ translate(0f, bob + look) }) {
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

/** The lit emitter, at the fan's own apex so the light and the beam share an origin. */
private fun DrawScope.drawLed(alpha: Float) {
    if (alpha <= 0f) return
    val at = Offset(Art.FAN_ORIGIN_X, Art.FAN_ORIGIN_Y)
    drawCircle(Flame, radius = 74f, center = at, alpha = 0.22f * alpha)
    drawCircle(Flame, radius = 42f, center = at, alpha = 0.45f * alpha)
    drawCircle(Flame, radius = 21f, center = at, alpha = alpha)
}

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
        drawIconGround(f.overlayAlpha)
        drawBody(body, f.bodyBob, f.overlayAlpha)
        drawEye(eye, f.bodyBob, f.eyeBob, f.overlayAlpha)

        // The rings: from the puck, out past every edge of the screen.
        val centre = Offset(Art.PUCK_CENTER_X, Art.PUCK_CENTER_Y)
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

        // The puck itself: airborne, turning once, landing with a squash about
        // its own foot (which is what makes a squash read as weight rather than
        // as a wobble).
        withTransform({
            translate(0f, f.puckDy)
            rotate(f.puckRotationDeg, pivot = centre)
            scale(
                f.puckScaleX,
                f.puckScaleY,
                pivot = Offset(Art.PUCK_CENTER_X, Art.PUCK_TOP + Art.PUCK_HEIGHT),
            )
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
            drawLed(f.ledAlpha * f.overlayAlpha)
        }
    }
}

// ══ ANIMATION B ═══════════════════════════════════════════════════════════

private fun DrawScope.drawLlamaSpit(
    f: WelcomeTimeline.FrameB,
    page: Color,
    body: ImageBitmap,
    puck: ImageBitmap,
    eye: ImageBitmap,
    front: Path,
) {
    if (f.overlayAlpha <= 0f) return
    drawRect(page, alpha = f.overlayAlpha)

    val box = artBox()
    withTransform({
        translate(box.left, box.top)
        scale(box.scale, box.scale, pivot = Offset.Zero)
    }) {
        val turnPivot = Offset(Art.CANVAS / 2f, FrontPose.CENTER_Y)

        // The icon stands still and the llama turns inside it — the frame is
        // the app's, not the animal's.
        drawIconGround(f.overlayAlpha)

        // ── act one: the side llama, and the spark in its eye ──────────────
        if (f.sideAlpha > 0f) {
            withTransform({ scale(f.turnScaleX, 1f, pivot = turnPivot) }) {
                val a = f.sideAlpha * f.overlayAlpha
                drawBody(body, 0f, a)
                drawEye(eye, 0f, 0f, a)
                drawLayer(puck, Art.PUCK_LEFT, Art.PUCK_TOP, Art.PUCK_WIDTH, Art.PUCK_HEIGHT, a)
                drawLed(a)
                if (f.twinkleAlpha > 0f) {
                    drawStar(
                        centre = Offset(Art.EYE_CENTER_X, Art.EYE_CENTER_Y),
                        outer = 62f * f.twinkleScale,
                        inner = 15f * f.twinkleScale,
                        rotationDeg = f.twinkleRotationDeg,
                        alpha = f.twinkleAlpha * a,
                    )
                }
            }
        }

        // ── act two: face the viewer, puff, and spit ───────────────────────
        if (f.frontAlpha > 0f) {
            val a = f.frontAlpha * f.overlayAlpha
            withTransform({ scale(f.turnScaleX, 1f, pivot = turnPivot) }) {
                withTransform({ scale(f.cheekScaleX, f.cheekScaleY, pivot = turnPivot) }) {
                    drawPath(front, Fleece, alpha = a)
                    drawPath(front, Ink, alpha = a, style = Stroke(width = OUTLINE))
                    // The hat rides the crown, clear of the fluff so the brim
                    // is readable — the same object the side pose wears, not a
                    // drawn stand-in for it.
                    drawLayer(
                        puck,
                        Art.CANVAS / 2f - Art.PUCK_WIDTH / 2f,
                        FrontPose.CENTER_Y - FrontPose.RADIUS_Y - 132f,
                        Art.PUCK_WIDTH, Art.PUCK_HEIGHT, a,
                    )
                    drawLed(a)
                    drawFrontFace(eye, a, f.grinAlpha)
                }

                // The droplet: down the screen and four times bigger, which is
                // what "toward the viewer" looks like on a flat surface.
                if (f.dropletAlpha > 0f) {
                    withTransform({
                        translate(0f, f.dropletDy)
                        scale(
                            f.dropletScale,
                            f.dropletScale,
                            pivot = Offset(Art.CANVAS / 2f, FrontPose.MOUTH_Y),
                        )
                    }) {
                        val drop = Rect(
                            Art.CANVAS / 2f - 27f, FrontPose.MOUTH_Y - 35f,
                            Art.CANVAS / 2f + 27f, FrontPose.MOUTH_Y + 35f,
                        )
                        drawOval(Water, drop.topLeft, drop.size, alpha = f.dropletAlpha * a)
                        drawOval(
                            Ink, drop.topLeft, drop.size,
                            alpha = f.dropletAlpha * a, style = Stroke(width = 8f),
                        )
                    }
                }
            }

            // ── act three: on the glass. Outside the turn, because it is on
            // the screen rather than on the llama.
            if (f.splatAlpha > 0f) {
                val splat = splatPath(
                    Offset(Art.CANVAS / 2f, FrontPose.MOUTH_Y + WelcomeTimeline.B_DROPLET_TRAVEL),
                    150f * f.splatScale,
                )
                drawPath(splat, Water, alpha = f.splatAlpha * a)
                drawPath(splat, Ink, alpha = f.splatAlpha * a, style = Stroke(width = 12f))
            }
        }
    }
}

// ── the drawn front pose ───────────────────────────────────────────────────

/**
 * The one element of this round that is drawn rather than cut, and the numbers
 * behind it.
 *
 * Everything is derived from the approved storyboard's front face by the same
 * [WelcomeTimeline.STORYBOARD_TO_MASTER] conversion the rest of the film uses,
 * with two departures made on purpose:
 *
 *  * the crown carries **fluff scallops**, because a bare oval is not this
 *    llama — the whole silhouette in the master art is made of them;
 *  * the hat is the **real puck sprite** rather than the storyboard's stand-in
 *    rectangle, so the front pose wears the same object the side pose does.
 */
private object FrontPose {
    const val CENTER_X = 512f
    const val CENTER_Y = 520f
    const val RADIUS_X = 258f
    const val RADIUS_Y = 322f

    /** Storyboard px → master units, about the face's own centre. */
    fun fx(storyboardX: Float): Float =
        CENTER_X + (storyboardX - 170f) * WelcomeTimeline.STORYBOARD_TO_MASTER

    fun fy(storyboardY: Float): Float =
        CENTER_Y + (storyboardY - 200f) * WelcomeTimeline.STORYBOARD_TO_MASTER

    /** Where the droplet leaves, and therefore where the splat is measured from. */
    val MOUTH_Y: Float = fy(222f)
}

/**
 * Head ∪ nine crown scallops ∪ two ears, as **one** path.
 *
 * Unioned rather than stacked: a stack of filled-and-stroked circles leaves
 * every interior arc showing, and the icon's language is a single unbroken
 * outline round the whole animal. `Path.op` does it once, at composition, and
 * the draw is then two calls.
 */
private fun frontPosePath(): Path {
    val head = Path().apply {
        addOval(
            Rect(
                FrontPose.CENTER_X - FrontPose.RADIUS_X,
                FrontPose.CENTER_Y - FrontPose.RADIUS_Y,
                FrontPose.CENTER_X + FrontPose.RADIUS_X,
                FrontPose.CENTER_Y + FrontPose.RADIUS_Y,
            ),
        )
    }

    val pieces = mutableListOf<Path>()

    // The crown fluff: bumps riding the top arc, alternating big and small so
    // the silhouette is irregular the way the master art's is.
    //
    // The radii are what they are because of the recording. At 52/43 the union
    // came out as a smooth egg with a faint wobble — a sheep's head, not this
    // llama's — because a bump has to stand PROUD of the ellipse to read as
    // fleece, and one that sits 20 units above a 272-unit curve does not.
    for (i in 0 until 9) {
        val angle = PI * (1.10 + i * 0.10)
        val cx = FrontPose.CENTER_X + (cos(angle) * FrontPose.RADIUS_X * 0.97).toFloat()
        val cy = FrontPose.CENTER_Y + (sin(angle) * FrontPose.RADIUS_Y * 0.97).toFloat()
        val r = if (i % 2 == 0) 76f else 58f
        pieces += Path().apply { addOval(Rect(cx - r, cy - r, cx + r, cy + r)) }
    }

    // The ears. Two rounds of recording went into these three numbers. The
    // storyboard's short quadratics came out as horns; lengthening them made
    // them longer horns that also punched through the icon's frame. The master
    // art's ears are LEAVES — wide at the base, tapered, canted out — and the
    // tip has to stay inside the paper, because the frame is the app's icon and
    // nothing may hang off it.
    for (side in intArrayOf(-1, 1)) {
        val s = side.toFloat()
        val px = FrontPose.CENTER_X + s * 138f
        val py = FrontPose.CENTER_Y - FrontPose.RADIUS_Y + 96f
        pieces += Path().apply {
            moveTo(px, py)
            // the outer edge, out and up to a tip that clears the crown but
            // stays well inside the icon's paper (which begins at 51).
            quadraticTo(px + s * 6f, py - 130f, px + s * 44f, py - 196f)
            // …and the inner edge, back down to a wide base on the fleece.
            quadraticTo(px + s * 78f, py - 120f, px + s * 104f, py - 4f)
            close()
        }
    }

    var union = head
    for (piece in pieces) {
        val next = Path()
        next.op(union, piece, PathOperation.Union)
        union = next
    }
    return union
}

/** Eyes, muzzle and — last of all — the grin. */
private fun DrawScope.drawFrontFace(eye: ImageBitmap, alpha: Float, grinAlpha: Float) {
    // The open eye is the real sprite; the wink is a stroke, as it is in the
    // storyboard, because a closed eye has no artwork to cut.
    drawLayer(
        eye,
        FrontPose.fx(146f) - Art.EYE_SPRITE_WIDTH / 2f,
        FrontPose.fy(184f) - Art.EYE_SPRITE_HEIGHT / 2f,
        Art.EYE_SPRITE_WIDTH,
        Art.EYE_SPRITE_HEIGHT,
        alpha,
    )
    val wink = Path().apply {
        moveTo(FrontPose.fx(184f), FrontPose.fy(182f))
        quadraticTo(
            FrontPose.fx(194f), FrontPose.fy(191f),
            FrontPose.fx(204f), FrontPose.fy(182f),
        )
    }
    drawPath(wink, Ink, alpha = alpha, style = Stroke(width = OUTLINE_FINE, cap = StrokeCap.Round))

    // The pursed mouth the droplet comes out of.
    drawOval(
        Ink,
        topLeft = Offset(FrontPose.fx(164f), FrontPose.fy(216f)),
        size = Size(FrontPose.fx(176f) - FrontPose.fx(164f), FrontPose.fy(226f) - FrontPose.fy(216f)),
        alpha = alpha,
    )

    if (grinAlpha > 0f) {
        val grin = Path().apply {
            moveTo(FrontPose.fx(150f), FrontPose.fy(218f))
            quadraticTo(
                FrontPose.fx(170f), FrontPose.fy(236f),
                FrontPose.fx(190f), FrontPose.fy(218f),
            )
        }
        drawPath(
            grin, Ink,
            alpha = grinAlpha * alpha,
            style = Stroke(width = OUTLINE_FINE, cap = StrokeCap.Round),
        )
    }
}

/** A four-point sparkle, for B's opening beat. */
private fun DrawScope.drawStar(
    centre: Offset,
    outer: Float,
    inner: Float,
    rotationDeg: Float,
    alpha: Float,
) {
    if (outer <= 0f || alpha <= 0f) return
    val path = Path()
    for (i in 0 until 8) {
        val r = if (i % 2 == 0) outer else inner
        val a = PI * (i / 4.0) + rotationDeg * PI / 180.0
        val x = centre.x + (cos(a) * r).toFloat()
        val y = centre.y + (sin(a) * r).toFloat()
        if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
    path.close()
    drawPath(path, Flame, alpha = alpha.coerceIn(0f, 1f))
}

/**
 * The splat: eleven alternating lobes about [centre], which is a wet impact
 * without being a starburst, plus three satellites so it reads as having
 * arrived rather than as having been placed.
 */
private fun splatPath(centre: Offset, radius: Float): Path {
    val path = Path()
    val lobes = 11
    for (i in 0 until lobes * 2) {
        val r = if (i % 2 == 0) radius else radius * 0.68f
        val a = PI * i / lobes + 0.35
        val x = centre.x + (cos(a) * r).toFloat()
        val y = centre.y + (sin(a) * r * 0.82f).toFloat()
        if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
    path.close()
    var splat = path
    for ((dx, dy, dr) in listOf(
        Triple(-1.28f, -0.55f, 0.17f),
        Triple(1.18f, -0.72f, 0.13f),
        Triple(0.95f, 0.68f, 0.10f),
    )) {
        val cx = centre.x + dx * radius
        val cy = centre.y + dy * radius * 0.82f
        val r = dr * radius
        val blob = Path().apply { addOval(Rect(cx - r, cy - r, cx + r, cy + r)) }
        val merged = Path()
        merged.op(splat, blob, PathOperation.Union)
        splat = merged
    }
    return splat
}
