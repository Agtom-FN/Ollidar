package com.lidarscan.app.ui.components

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.background
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.core.calib.HoldOrientation
import com.lidarscan.core.capture.AttitudeIndicator as Attitude
import kotlinx.coroutines.flow.StateFlow

/**
 * ROUND 28 item 168 — **the mini attitude indicator.**
 *
 * The owner's addition to the redesign, and the only genuinely new component in
 * it. While walking, the operator cannot see whether he is holding the rig
 * square; the seal grade finds out afterwards, when nothing can be done. This
 * is that fact, live, in a 40 dp circle:
 *
 * ```
 *      ╭───────╮        neutral ring, ink-mute
 *   ───┤   ●   ├───     side ticks, ink-mute
 *      ╰───────╯        orange horizon needle + centre dot, rotating with roll
 * ```
 *
 * Placement, per the owner: **right of the STOP button while recording**, so
 * the bottom row reads pause · STOP · attitude with the two 52 dp circles
 * mirrored either side of the FAB; and **inside the hold-still start card**,
 * above the countdown, where "hold still" is exactly the instruction the
 * instrument helps obey.
 *
 * Everything below the drawing is [Attitude] in `:core` — the angle is unit
 * tested there, and this file contains no arithmetic beyond turning degrees
 * into a rotation. The needle goes amber past
 * [Attitude.AMBER_DEG]; a phone too flat to have an attitude draws
 * the ring and no needle, because an instrument that admits it cannot read is
 * trusted and one that shows a plausible wrong number once is not.
 *
 * Pure Compose `Canvas`. No image asset, no ARCore call, no recomposition
 * beyond the roll value itself.
 */
@Composable
fun AttitudeIndicator(
    /** `HoldOrientation.screenUpAngleDeg`; null when there is no attitude. */
    rollDeg: Double?,
    modifier: Modifier = Modifier,
    /** `HoldOrientation.confident` — false when the phone is too flat to say. */
    confident: Boolean = true,
    size: Dp = 40.dp,
) {
    val reading = Attitude.reading(rollDeg, confident)

    // The needle is animated rather than snapped: gait noise is a few degrees
    // per step and an un-damped needle at frame rate reads as a fault light,
    // which is the opposite of what an instrument beside a STOP button should
    // do. 120 ms is under the threshold at which a control feels laggy and well
    // over the period of a footfall.
    val needle by animateFloatAsState(
        targetValue = reading.needleDeg.toFloat(),
        label = "attitudeNeedle",
    )

    val ringInk = if (reading.beyondThreshold) ScanColors.warn else ScanColors.inkMute
    val needleInk = if (reading.beyondThreshold) ScanColors.warn else ScanColors.primary
    val description = when {
        !reading.known -> "Attitude unavailable"
        reading.beyondThreshold -> "Rig ${reading.offSquareDeg.toInt()} degrees off square"
        else -> "Rig level"
    }

    Box(
        modifier
            .size(size)
            .testTag("attitudeIndicator")
            .semantics { contentDescription = description },
    ) {
        Canvas(Modifier.size(size)) {
            val r = this.size.minDimension / 2f
            val cx = this.size.width / 2f
            val cy = this.size.height / 2f
            val strokePx = 1.4.dp.toPx()

            // The ring.
            drawCircle(
                color = ringInk,
                radius = r - strokePx,
                center = Offset(cx, cy),
                style = Stroke(width = strokePx),
            )

            // The two side ticks — the fixed horizon the needle is read
            // against. Without them a rotating line in a circle has nothing to
            // be rotated relative to.
            val tickOuter = r * 0.80f
            val tickInner = r * 0.44f
            listOf(-1f, 1f).forEach { side ->
                drawLine(
                    color = ringInk,
                    start = Offset(cx + side * tickInner, cy),
                    end = Offset(cx + side * tickOuter, cy),
                    strokeWidth = strokePx,
                )
            }

            if (!reading.known) return@Canvas

            // The horizon needle and its centre dot, rotated together about the
            // instrument's centre. Positive tips the right-hand end down, which
            // is the sense a real attitude indicator turns.
            rotate(degrees = needle, pivot = Offset(cx, cy)) {
                val half = r * 0.58f
                drawLine(
                    color = needleInk,
                    start = Offset(cx - half, cy),
                    end = Offset(cx + half, cy),
                    strokeWidth = 1.8.dp.toPx(),
                    cap = androidx.compose.ui.graphics.StrokeCap.Round,
                )
                drawCircle(color = needleInk, radius = 1.6.dp.toPx(), center = Offset(cx, cy))
            }
        }
    }
}

/**
 * ROUND 30 item 175 — **the same instrument, wired to a source that moves.**
 *
 * Round 28's indicator took a `Double?` and the Scan screen handed it round 26
 * item 125(b)'s START orientation: a value written once when the mount hold
 * settles and, by its own KDoc, "left alone for the rest of the capture". So on
 * the owner's Pixel the needle was null through the hold-still card and frozen
 * for the whole walk — an instrument fed a constant, which is exactly what "it
 * does not move" describes.
 *
 * The flow is collected **here**, at the leaf, and not in `CaptureRoute`: the
 * source publishes at 20 Hz, and a read at the route would recompose the entire
 * Scan screen twenty times a second to rotate a 34 dp line. Reading it inside
 * the instrument scopes the invalidation to the instrument.
 *
 * `null` from the source means no reading at all — before the first sample, or
 * after the feed is released — and draws the ring with no needle, which is the
 * same honesty the `confident` flag buys.
 */
@Composable
fun AttitudeIndicator(
    attitude: StateFlow<HoldOrientation?>,
    modifier: Modifier = Modifier,
    size: Dp = 40.dp,
) {
    val hold by attitude.collectAsStateWithLifecycle()
    AttitudeIndicator(
        rollDeg = hold?.screenUpAngleDeg,
        modifier = modifier,
        confident = hold?.confident ?: false,
        size = size,
    )
}

/**
 * The recording row's housing for [AttitudeIndicator] — the same 52 dp circular
 * trough the pause button sits in, so the two mirror each other either side of
 * the STOP button exactly as the mockup draws them.
 */
@Composable
fun AttitudeButtonSlot(
    attitude: StateFlow<HoldOrientation?>,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier
            .size(52.dp)
            .background(ScanColors.trough, CircleShape),
        contentAlignment = androidx.compose.ui.Alignment.Center,
    ) {
        AttitudeIndicator(attitude = attitude, size = 34.dp)
    }
}
