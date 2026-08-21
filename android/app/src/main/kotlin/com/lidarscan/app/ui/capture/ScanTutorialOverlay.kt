package com.lidarscan.app.ui.capture

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.core.capture.ScanTutorial
import com.lidarscan.core.capture.TutorialAnchor
import com.lidarscan.core.capture.TutorialState

/**
 * ROUND 24 item 110(b) — **the spotlight registry.**
 *
 * A control says where it is by hanging [tutorialAnchor] on its modifier; the
 * overlay reads the map. That indirection is what keeps the tour from becoming
 * a second layout: nothing is moved, wrapped or re-parented to be highlighted,
 * and a control that is not currently composed simply has no entry — the
 * overlay then centres its card and rings nothing, which is exactly what has to
 * happen on the disconnected ready screen a first-run operator is looking at.
 *
 * A `CompositionLocal` rather than a parameter threaded through eight
 * composables: the anchors are on controls four and five levels down
 * (`TransportRow`'s scan button, `CaptureChipRow`'s row) and passing a
 * registry through every intermediate signature would put tutorial plumbing in
 * the parameter list of every part of the Scan screen.
 */
internal val LocalTutorialAnchors = compositionLocalOf<MutableMap<TutorialAnchor, Rect>?> { null }

/**
 * The modifier a control hangs on itself to say where it is: a plain
 * `onGloballyPositioned`, and **only** when a registry is present, so the
 * ordinary path — nobody taking the tour, which is every walk after the first
 * — measures nothing and allocates nothing.
 */
@Composable
internal fun rememberTutorialAnchor(anchor: TutorialAnchor): Modifier {
    val registry = LocalTutorialAnchors.current ?: return Modifier
    return Modifier.onGloballyPositioned { coordinates ->
        registry[anchor] = coordinates.boundsInRoot()
    }
}

/**
 * Provides the registry for [content] while [enabled]; otherwise a straight
 * pass-through, so nothing measures anything when nobody is looking.
 */
@Composable
internal fun TutorialAnchorScope(enabled: Boolean, content: @Composable () -> Unit) {
    if (!enabled) {
        content()
        return
    }
    val registry = remember { mutableStateMapOf<TutorialAnchor, Rect>() }
    CompositionLocalProvider(LocalTutorialAnchors provides registry) { content() }
}

/**
 * ROUND 24 item 110(b) — **the tour.**
 *
 * Six steps, one control at a time: the screen dims, the control keeps its
 * brightness inside a ring, and a card underneath says in twelve words or fewer
 * what it does. Next and Skip, and the last step's button says Done.
 *
 * ## The cut-out, and why it is drawn rather than layered
 *
 * The dim is one `Canvas` with `CompositingStrategy.Offscreen`: a full-screen
 * black rectangle, then the highlight rectangle punched through it with
 * `BlendMode.Clear`. Offscreen compositing is the part that matters — without
 * its own layer, `Clear` would erase the app's own pixels underneath instead of
 * the scrim's, which is a spectacular-looking bug. The alternative (four
 * rectangles around a hole) is more code, and gets the corner radius wrong.
 *
 * ## It consumes touches, and that is the difference from item 112
 *
 * The tracking-loss popup deliberately lets the STOP button through, because
 * abandoning a bad scan must always be possible. A tutorial is the opposite: it
 * is a mode the operator entered on purpose, its own buttons are how you leave,
 * and a stray tap landing on the real SCAN button behind the explanation of the
 * SCAN button would start a recording nobody asked for.
 */
@Composable
internal fun ScanTutorialOverlay(
    state: TutorialState,
    anchors: Map<TutorialAnchor, Rect>,
    onNext: () -> Unit,
    onSkip: () -> Unit,
) {
    val step = state.step ?: return
    val spot: Rect? = anchors[step.anchor]

    androidx.compose.foundation.layout.BoxWithConstraints(
        Modifier.fillMaxSize().testTag("tutorialOverlay"),
    ) {
        // The barrier is on the CANVAS, not on this Box.
        //
        // `clickable` sets `mergeDescendants`, so putting it on the root would
        // fold the card's title, body, progress and both buttons into one
        // semantics node — the overlay would still work and would become
        // untestable and unreadable to a screen reader in the same stroke.
        // Caught on the emulator: `tutorialCard` was "not displayed" because it
        // had been merged away.
        androidx.compose.foundation.Canvas(
            Modifier
                .fillMaxSize()
                // A tutorial is a mode: it eats every touch that is not one of
                // its own buttons. See the header for why this differs from
                // item 112, where STOP must stay reachable through the scrim.
                .androidClickableNoRipple()
                // Without its own layer, BlendMode.Clear punches through the
                // APP, not through the scrim.
                .graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen),
        ) {
            drawRect(Color.Black.copy(alpha = 0.78f))
            if (spot != null) {
                val pad = 8.dp.toPx()
                val topLeft = Offset(spot.left - pad, spot.top - pad)
                val size = Size(spot.width + pad * 2, spot.height + pad * 2)
                drawRoundRect(
                    color = Color.Transparent,
                    topLeft = topLeft,
                    size = size,
                    cornerRadius = androidx.compose.ui.geometry.CornerRadius(20.dp.toPx()),
                    blendMode = BlendMode.Clear,
                )
                drawRoundRect(
                    color = Ember,
                    topLeft = topLeft,
                    size = size,
                    cornerRadius = androidx.compose.ui.geometry.CornerRadius(20.dp.toPx()),
                    style = Stroke(width = 2.dp.toPx()),
                )
            }
        }

        // ROUND 27 item 133(a): the card goes to whichever half of the screen
        // the spotlight is NOT in, and the rule is now
        // `TutorialCardPlacement.cardHalf` in `:core` — a pure function of the
        // spotlight's two edges and THIS box's height, tested on a JVM.
        //
        // The height comes from `BoxWithConstraints` rather than from
        // `Configuration.screenHeightDp`, because the spotlight's bounds are in
        // this box's pixels and the configuration height is a different number
        // (24 dp shorter here). Comparing the two is how a rule about halves
        // ends up deciding on an inset.
        val windowHeightPx = with(androidx.compose.ui.platform.LocalDensity.current) {
            maxHeight.toPx()
        }
        val alignment = when (
            com.lidarscan.core.capture.TutorialCardPlacement.cardHalf(
                spotTop = spot?.top,
                spotBottom = spot?.bottom,
                screenHeight = windowHeightPx,
            )
        ) {
            com.lidarscan.core.capture.TutorialCardPlacement.Half.TOP -> Alignment.TopCenter
            com.lidarscan.core.capture.TutorialCardPlacement.Half.BOTTOM -> Alignment.BottomCenter
            com.lidarscan.core.capture.TutorialCardPlacement.Half.CENTER -> Alignment.Center
        }
        Column(
            Modifier
                .align(alignment)
                .fillMaxWidth()
                .padding(horizontal = 20.dp, vertical = 34.dp)
                .background(
                    MaterialTheme.colorScheme.surfaceContainer,
                    RoundedCornerShape(ScanDims.CardRadius),
                )
                .border(1.dp, Ember, RoundedCornerShape(ScanDims.CardRadius))
                .padding(horizontal = 18.dp, vertical = 16.dp)
                .testTag("tutorialCard"),
        ) {
            Text(
                ScanTutorial.progressLabel(state),
                style = MonoLabel,
                color = Ember,
                modifier = Modifier.testTag("tutorialProgress"),
            )
            Spacer(Modifier.height(6.dp))
            Text(
                step.title,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.Bold,
                fontSize = 20.sp,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.testTag("tutorialTitle"),
            )
            Spacer(Modifier.height(6.dp))
            Text(
                step.body,
                style = MaterialTheme.typography.bodyMedium,
                color = InkFaint,
                modifier = Modifier.testTag("tutorialBody"),
            )
            Spacer(Modifier.height(14.dp))
            Row(
                Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                TextButton(onClick = onSkip, modifier = Modifier.testTag("tutorialSkip")) {
                    Text(ScanTutorial.SKIP, color = InkFaint)
                }
                Spacer(Modifier.weight(1f))
                PrimaryPill(
                    text = ScanTutorial.advanceLabel(state),
                    height = 46.dp,
                    onClick = onNext,
                    modifier = Modifier.testTag("tutorialNext"),
                )
            }
        }
    }
}

/**
 * ROUND 24 item 110(b) — **the one-time offer.**
 *
 * A card, not a dialog. The Scan screen's own rule since round 5 is that a
 * modal is the worst possible interruption on this tab, and that applies most
 * of all to the very first time someone opens it. It is dismissible, it never
 * comes back (both flags are persisted — see
 * [com.lidarscan.core.capture.ScanTutorial.shouldOffer]), and it sits at the
 * top of the screen where the loud band already lives.
 */
@Composable
internal fun TutorialOffer(onAccept: () -> Unit, onDismiss: () -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 14.dp, vertical = 6.dp)
            .background(
                MaterialTheme.colorScheme.surfaceContainer,
                RoundedCornerShape(ScanDims.TileRadius),
            )
            .border(1.dp, Ember, RoundedCornerShape(ScanDims.TileRadius))
            .padding(horizontal = 14.dp, vertical = 12.dp)
            .testTag("tutorialOffer"),
    ) {
        Text(
            ScanTutorial.OFFER_TITLE,
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 16.sp,
            color = MaterialTheme.colorScheme.onSurface,
            textAlign = TextAlign.Start,
        )
        Spacer(Modifier.height(8.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onDismiss, modifier = Modifier.testTag("tutorialOfferDismiss")) {
                Text(ScanTutorial.OFFER_DISMISS, color = InkFaint)
            }
            Spacer(Modifier.weight(1f))
            PrimaryPill(
                text = ScanTutorial.OFFER_ACCEPT,
                height = 42.dp,
                onClick = onAccept,
                modifier = Modifier.testTag("tutorialOfferAccept"),
            )
        }
    }
}

/**
 * A click that swallows the gesture and draws nothing.
 *
 * `clickable` with no indication and no role: the overlay must not look
 * tappable (it is not a button, it is a barrier) and must not ripple when a
 * stray touch lands on it.
 */
@Composable
private fun Modifier.androidClickableNoRipple(): Modifier {
    val interaction = remember { androidx.compose.foundation.interaction.MutableInteractionSource() }
    return this.then(
        Modifier.clickable(
            interactionSource = interaction,
            indication = null,
            onClick = {},
        ),
    )
}
