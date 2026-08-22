package com.lidarscan.app.ui.components

import androidx.compose.animation.core.AnimationSpec
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.app.ui.welcome.WelcomeReducedMotion
import com.lidarscan.core.calib.HoldOrientation
import com.lidarscan.core.capture.PostureIndicator as Posture
import kotlinx.coroutines.flow.StateFlow

/*
 * ROUND 33 item 179 — **the posture indicator, in its two approved forms.**
 *
 * The owner approved a prototype on 2026-08-22 whose CSS keyframes are the
 * specification, and the split of placements is his: the **3D phone ghost** in
 * the hold-still card, where the posture is learned before GO and there is room
 * for a literal picture of the phone and a word of correction; the **bubble**
 * beside STOP, where it is glanced at from arm's length mid-walk and only a dot
 * moving in a circle survives. Each replaces round 28's single-axis dial in its
 * own placement.
 *
 * Everything with a number in it is [Posture] in `:core` — the tolerance (which
 * is round 28's `AMBER_DEG`, read and not copied), the roll's snap to the
 * nearest square hold, the radial combination of the two axes, the dominant-axis
 * hint and the bubble's offset and clamp. This file turns those into shapes.
 *
 * ### Reduced motion, and why these still move
 *
 * Item 179 is explicit: an indicator is **information**, not decoration, and a
 * posture indicator that holds still is a broken instrument rather than a
 * considerate one. So [WelcomeReducedMotion] does not stop the ghost tilting or
 * the bubble travelling; it removes the **glow** and turns the springs into
 * instant writes. The reading is identical either way.
 */

/** The prototype's measurements, in one place, so both placements are one design. */
private object PostureDims {
    /** The card's housing. Item 179(b) asks for "~96 dp". */
    val CardHousing: Dp = 96.dp

    /** The strip's, which is the pause button's trough — item 179(c). */
    val StripHousing: Dp = 52.dp

    // The prototype's proportions, as fractions of its 150 px housing, so both
    // sizes are one design rather than two sets of dp that drifted.
    const val GHOST_WIDTH = 56f / 150f
    const val GHOST_HEIGHT = 104f / 150f
    const val TARGET_INSET = 24f / 150f
    const val TARGET_CORNER = 14f / 150f
    const val BUBBLE_DIAMETER = 26f / 150f
    const val TOLERANCE_RING = 31f / 150f

    /**
     * `graphicsLayer.cameraDistance` for the ghost, **measured rather than
     * derived**.
     *
     * The prototype is `perspective: 400px` over a 104 px phone — 3.85 phone
     * heights, and that ratio is the target. What Compose's `cameraDistance`
     * is denominated in is the problem: it is neither documented in dp nor in
     * px, the platform scales it on the way to the render node, and both
     * readings of it were tried on the AVD and photographed.
     *
     *  * `ghostHeight.toPx() * 3.85` (≈ 700) is **orthographic** — a 20° lean
     *    photographed as a 6 % vertical squash with the top and bottom edges
     *    the same width to the pixel;
     *  * this value taper the ghost by about 6 % across its height at 20°,
     *    which measures back to roughly four phone heights — the prototype's
     *    look, on the renderer the shots were taken with.
     *
     * So it is a number with a photograph behind it, and the photographs are in
     * `uishots7/`. It is not scaled by the housing size because the ghost has
     * exactly one placement: the card.
     */
    const val CAMERA_DISTANCE = 9f
}

/**
 * What the instrument says about itself.
 *
 * Deliberately **word for word round 28's**, including "off square", even though
 * the number behind it is now two axes rather than one: it is the string three
 * emulator suites already assert on, an operator hears the same sentence he
 * heard in 0.9.15, and the reading it describes is a strict superset of the one
 * that produced it. `offPostureDeg` for a roll-only tilt IS the old number.
 */
private fun postureDescription(reading: Posture.Reading): String = when {
    !reading.known -> "Attitude unavailable"
    reading.beyondTolerance -> "Rig ${reading.offPostureDeg.toInt()} degrees off square"
    else -> "Rig level"
}

/**
 * ROUND 33 item 179(b) — **Option 1, the hold-still card's 3D phone ghost.**
 *
 * A miniature phone inside the enlarged circular housing, tilted by the live
 * posture: `rotationX` is the pitch — positive leans the top edge away, exactly
 * as [HoldOrientation.screenPitchDeg] is signed — and `rotationY` the roll,
 * which swings the LOW edge towards the viewer so the ghost falls the way the
 * hand does. Behind it, the dashed target frame the phone is supposed to sit
 * in; under it, one word of correction when it does not.
 *
 * The flow is collected **here, at the leaf**, for round 30's reason: the source
 * publishes at 20 Hz and a read any higher up the tree would recompose the Scan
 * screen twenty times a second in order to tilt a 36 dp rectangle.
 *
 * [onPostureLost] fires **once** on each good→bad edge — see
 * `CueKind.POSTURE_OFF`, which is what the Scan screen passes down here.
 */
@Composable
fun PostureGhostIndicator(
    attitude: StateFlow<HoldOrientation?>,
    modifier: Modifier = Modifier,
    size: Dp = PostureDims.CardHousing,
    onPostureLost: () -> Unit = {},
) {
    val hold by attitude.collectAsStateWithLifecycle()
    PostureGhostIndicator(
        reading = Posture.reading(hold),
        modifier = modifier,
        size = size,
        onPostureLost = onPostureLost,
    )
}

/**
 * The same instrument from a literal reading — the form the Compose tests drive,
 * and the form that keeps every decision in this file testable without a sensor.
 */
@Composable
fun PostureGhostIndicator(
    reading: Posture.Reading,
    modifier: Modifier = Modifier,
    size: Dp = PostureDims.CardHousing,
    onPostureLost: () -> Unit = {},
) {
    val context = LocalContext.current
    val reducedMotion = remember(context) { WelcomeReducedMotion.isOn(context) }

    // ONE tick on the good→bad transition, item 179(b). The edge is held in
    // composition state rather than derived per frame: a value that is 10.1°
    // for four consecutive publications is one crossing, not four buzzes.
    val wasBeyond = remember { mutableStateOf(false) }
    LaunchedEffect(reading.beyondTolerance, reading.known) {
        val bad = reading.known && reading.beyondTolerance
        if (bad && !wasBeyond.value) onPostureLost()
        // Coming back inside re-arms it; so does losing the reading entirely,
        // because a phone that went flat and came back tilted is a new crossing.
        wasBeyond.value = bad
    }

    val target = if (reading.beyondTolerance) ScanColors.warn else ScanColors.good
    val frame = ScanColors.line

    val pitch by animateFloatAsState(
        targetValue = reading.drawnPitchDeg.toFloat(),
        animationSpec = postureSpec(reducedMotion),
        label = "posturePitch",
    )
    val roll by animateFloatAsState(
        targetValue = reading.drawnRollDeg.toFloat(),
        animationSpec = postureSpec(reducedMotion),
        label = "postureRoll",
    )

    Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = modifier) {
        Box(
            Modifier
                .size(size)
                .background(ScanColors.trough, CircleShape)
                .testTag("attitudeIndicator")
                .semantics { contentDescription = postureDescription(reading) },
            contentAlignment = Alignment.Center,
        ) {
            // The dashed target frame: where the phone is supposed to be, drawn
            // behind it and never moving. Without it a tilted rectangle in a
            // circle has nothing to be tilted relative to — the same argument
            // round 28's fixed side ticks were drawn for.
            Canvas(Modifier.size(size)) {
                val inset = this.size.minDimension * PostureDims.TARGET_INSET
                val corner = this.size.minDimension * PostureDims.TARGET_CORNER
                drawRoundRect(
                    color = frame,
                    topLeft = Offset(inset, inset),
                    size = Size(this.size.width - inset * 2, this.size.height - inset * 2),
                    cornerRadius = CornerRadius(corner, corner),
                    style = Stroke(
                        width = 1.5.dp.toPx(),
                        pathEffect = PathEffect.dashPathEffect(
                            floatArrayOf(4.dp.toPx(), 4.dp.toPx()),
                        ),
                    ),
                )
            }

            if (reading.known) {
                val ghostW = size * PostureDims.GHOST_WIDTH
                val ghostH = size * PostureDims.GHOST_HEIGHT
                Canvas(
                    Modifier
                        .size(ghostW, ghostH)
                        .testTag("postureGhost")
                        .graphicsLayer {
                            rotationX = pitch
                            rotationY = roll
                            // See PostureDims.CAMERA_DISTANCE: this number
                            // came off the AVD's own screenshots, because what
                            // Compose scales this by is not documented.
                            cameraDistance = PostureDims.CAMERA_DISTANCE
                        },
                ) {
                    val corner = CornerRadius(this.size.minDimension * 0.18f)
                    // Fill first, border over it, at the prototype's alpha.
                    drawRoundRect(color = target.copy(alpha = 0.12f), cornerRadius = corner)
                    drawRoundRect(
                        color = target,
                        cornerRadius = corner,
                        style = Stroke(width = 2.dp.toPx()),
                    )
                    // The camera notch, which is the whole of why this reads as
                    // a phone and not as a rectangle: it is what tells the
                    // operator which end is up while the thing is tilting.
                    val notchW = this.size.width * 0.25f
                    val notchH = this.size.height * 0.038f
                    drawRoundRect(
                        color = target,
                        topLeft = Offset((this.size.width - notchW) / 2f, this.size.height * 0.055f),
                        size = Size(notchW, notchH),
                        cornerRadius = CornerRadius(notchH / 2f),
                    )
                }
            }
        }

        // The correction, under the housing, and only when there is one. An
        // empty line is NOT reserved for it: the card holds a fixed minimum
        // height already (`StartModalCard`), and a permanently blank row under
        // a green instrument reads as a thing that has failed to load.
        val hint = reading.hint
        if (hint != null) {
            Spacer(Modifier.height(8.dp))
            Text(
                hint,
                style = ScanBody,
                color = ScanColors.warn,
                textAlign = TextAlign.Center,
                modifier = Modifier.testTag("postureHint"),
            )
        }
    }
}

/**
 * ROUND 33 item 179(c) — **Option 2, the recording strip's two-axis bubble.**
 *
 * The same 52 dp trough the pause button sits in, so the walking row still reads
 * pause · STOP · instrument with the two circles mirrored either side of the
 * FAB. A cross-hair and a tolerance ring at the 10° radius; a bubble that
 * carries both axes at once; green inside, amber outside, and a soft glow that
 * reduced motion takes away.
 *
 * No text and no haptics, deliberately. The walk already has four cue patterns
 * the operator is learning to tell apart through a pocket, and a fifth that
 * fired on a hand wobble would devalue the four that mean something.
 */
@Composable
fun PostureBubbleSlot(
    attitude: StateFlow<HoldOrientation?>,
    modifier: Modifier = Modifier,
    size: Dp = PostureDims.StripHousing,
) {
    val hold by attitude.collectAsStateWithLifecycle()
    PostureBubbleSlot(reading = Posture.reading(hold), modifier = modifier, size = size)
}

/** The bubble from a literal reading — the Compose tests' entry point. */
@Composable
fun PostureBubbleSlot(
    reading: Posture.Reading,
    modifier: Modifier = Modifier,
    size: Dp = PostureDims.StripHousing,
) {
    val context = LocalContext.current
    val reducedMotion = remember(context) { WelcomeReducedMotion.isOn(context) }
    val bubbleInk = if (reading.beyondTolerance) ScanColors.warn else ScanColors.good
    val chrome = ScanColors.line

    // The offset is computed in `:core` from the housing's own pixels, so the
    // arithmetic — the 10° scale, the radial clamp — is unit tested against the
    // numbers this Canvas actually draws with rather than against a copy.
    val density = LocalDensity.current
    val housingPx = with(density) { size.toPx() }
    val bubbleRadiusPx = housingPx * PostureDims.BUBBLE_DIAMETER / 2f
    val offset = Posture.bubbleOffset(
        reading = reading,
        toleranceRadiusPx = housingPx * PostureDims.TOLERANCE_RING,
        maxRadiusPx = housingPx / 2f - bubbleRadiusPx - with(density) { 2.dp.toPx() },
    )
    val dx by animateFloatAsState(offset.dx, postureSpec(reducedMotion), label = "bubbleX")
    val dy by animateFloatAsState(offset.dy, postureSpec(reducedMotion), label = "bubbleY")

    Box(
        modifier
            .size(size)
            .background(ScanColors.trough, CircleShape)
            .testTag("attitudeIndicator")
            .semantics { contentDescription = postureDescription(reading) },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(size)) {
            val cx = this.size.width / 2f
            val cy = this.size.height / 2f
            val hair = 1.4.dp.toPx()
            val ringR = this.size.minDimension * PostureDims.TOLERANCE_RING

            // The cross-hair — the fixed frame the bubble is read against.
            val armInner = ringR * 1.25f
            val armOuter = this.size.minDimension / 2f - hair * 2f
            listOf(-1f to 0f, 1f to 0f, 0f to -1f, 0f to 1f).forEach { (ux, uy) ->
                drawLine(
                    color = chrome,
                    start = Offset(cx + ux * armInner, cy + uy * armInner),
                    end = Offset(cx + ux * armOuter, cy + uy * armOuter),
                    strokeWidth = hair,
                )
            }

            // The tolerance ring, AT the 10° radius the offset maps 10° onto —
            // item 179(d)'s one constant, drawn and tested against each other.
            drawCircle(color = chrome, radius = ringR, center = Offset(cx, cy), style = Stroke(hair))

            if (!reading.known) return@Canvas

            if (!reducedMotion) {
                drawCircle(
                    color = bubbleInk.copy(alpha = 0.28f),
                    radius = bubbleRadiusPx * 1.75f,
                    center = Offset(cx + dx, cy + dy),
                )
            }
            drawCircle(
                color = bubbleInk,
                radius = bubbleRadiusPx,
                center = Offset(cx + dx, cy + dy),
            )
        }
    }
}

/**
 * How both indicators travel.
 *
 * A spring normally: the reading is already low-passed by `LiveAttitude`, and
 * what this adds is the weight that makes a 20 Hz stream read as a physical
 * object rather than as a value being written. Under reduced motion it is a
 * zero-length tween — an instant write, which still MOVES (the position is the
 * information) and simply stops overshooting.
 */
private fun postureSpec(reducedMotion: Boolean): AnimationSpec<Float> =
    if (reducedMotion) {
        tween<Float>(durationMillis = 0)
    } else {
        spring<Float>(dampingRatio = 0.85f, stiffness = 260f)
    }
