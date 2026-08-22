package com.lidarscan.app.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanMeta
import com.lidarscan.app.ui.theme.ScanMetaCaps
import com.lidarscan.app.ui.theme.ScanTitle
import com.lidarscan.app.ui.theme.ScanColors

/**
 * The redesign's shared chrome — one file so the mockup's measurements live in
 * one place instead of being re-typed per screen.
 *
 * Sizes here are the ones the owner review rounds actually argued about (48 dp
 * View halves, 44 dp switch rows, 40 dp chips, 28 dp slider thumbs, the 44 dp
 * hit target on a chip-sized health readout) — they are requirements, not
 * taste, so they are named constants rather than literals sprinkled through
 * five screens.
 */
object ScanDims {
    /**
     * ROUND 28 item 167's sibling — **the 4 dp grid, as seven legal values.**
     *
     * The audit counted `6.dp` 78 times, `10.dp` 68, `14.dp` 69, plus
     * 1, 2, 3, 5, 7, 9, 11, 19, 22, 23, 26, 30, 34, 38, 42, 46, 54, 55, 58, 86
     * and 110 — effectively a 1 dp grid, which is the same as no grid, which is
     * why the Scan screen had four different left margins on one screen and
     * nothing shared an edge with anything.
     *
     * `Hair` (1 dp) survives as a border width only; it is not a spacing value.
     */
    val Hair = 1.dp
    val S1 = 4.dp
    val S2 = 8.dp
    val S3 = 12.dp
    val S4 = 16.dp
    val S6 = 24.dp
    val S8 = 32.dp
    val S12 = 48.dp

    /** The screen margin. **Every** screen, one value. */
    val ScreenMargin = S4

    /** Card and row internal padding. */
    val CardPadding = S4

    /** Between list items in a stack that is not hairline-separated. */
    val ItemGap = S2

    /** Between sections. */
    val SectionGap = S6

    /** Between a label and its value. */
    val LabelGap = S1

    /** Icon to text. */
    val IconGap = S2

    /** THE ROW's height (§C.4). 72 dp when it carries a thumbnail. */
    val Row = 56.dp
    val RowWithThumb = 72.dp

    /** The Projects row's leading thumbnail. */
    val Thumb = 56.dp

    /** THE CHIP (§C.4): a state, never a control. */
    val Chip = 24.dp
    val ChipRadius = 12.dp

    /** THE BUTTON (§C.4): one height for primary, secondary and icon. */
    val Button = 48.dp

    /** The record FAB — the one deliberate exception to the button set. */
    val Fab = 88.dp

    /** Bottom-sheet segmented row that is the sheet's largest target (the View row). */
    val SegmentTall = 48.dp

    /** Ordinary sheet segmented row (colour mode, keyframe rate). */
    val SegmentChip = 40.dp

    /** Switch row minimum height. */
    val SwitchRow = 44.dp

    /** Slider thumb. */
    val SliderThumb = 28.dp

    /**
     * Minimum touch target anywhere in the app.
     *
     * ROUND 28: 44 → **48**, onto the grid. The old value was declared and then
     * violated by `ScanChip`, which accepted an `onClick` at ~24 dp tall (item
     * 152); that is resolved structurally rather than by padding, because a
     * chip is a state and not a control.
     */
    val Touch = 48.dp

    /**
     * ROUND 28 item 147 — **the tab bar is a bar again.**
     *
     * It was a floating capsule: inset 16 dp from each side, 12 dp from the
     * bottom, radius half its height, translucent, with a 12 dp shadow. That
     * made it the app's third floating layer, in direct violation of the
     * owner's own rule that only warnings and the scan FAB may float. It also
     * guillotined the last Projects row (nothing to fade against, no scrim),
     * and it cost every screen 32 dp of horizontal width for the insets.
     *
     * Level 0, anchored to the bottom edge, opaque, with a top hairline. The
     * side and bottom insets are gone rather than zeroed — a token that is
     * always 0 is a token that grows back.
     */
    val TabBar = 64.dp

    /** Room scrollable content must leave so it clears the tab bar. 86 → 80, on the grid. */
    val TabBarClearance = 80.dp

    /**
     * ROUND 26 item 124 — the height of [BackBar], as a token.
     *
     * The fullscreen Scan screen positions its floating chrome band from the
     * top of the window, so it has to know whether a back bar is in front of
     * it. It was a literal 56 in `CaptureLayout.APP_BAR_DP` and a `height(56.dp)`
     * in the bar itself; two copies of one number that has to agree, and the
     * consequence of them disagreeing is a chip row printed through the status
     * pill.
     */
    val BackBar = 56.dp

    /** ROUND 28 §C.4: THE CARD is 16 dp. Was 20. */
    val CardRadius = 16.dp
    val TileRadius = 12.dp

    /**
     * ROUND 16 item 61 — **the bottom-sheet and dialog radius, as a token.**
     *
     * Owner, on 0.9.0: *"for the merge process button the pop up window the
     * upper corner radius too larger and there are some tab and window show the
     * same too."* He is describing a real inconsistency and its cause is one
     * line in `Theme.kt`: `LidarScanShapes.extraLarge` is
     * `RoundedCornerShape(percent = 50)`, deliberately a PILL so that
     * un-restyled `Button`s, `FilterChip`s and `SegmentedButton`s round like
     * the hand-built ones. Material 3 then hands that same `extraLarge` to
     * `ModalBottomSheet` (`BottomSheetDefaults.ExpandedShape`) and to
     * `AlertDialog` (`AlertDialogDefaults.shape`) — and 50 % of a full-width
     * sheet's short side is an enormous curve.
     *
     * Three of the app's five sheets were already passing
     * `RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp)` by hand and two
     * were not, which is exactly why some windows looked right and some did
     * not. The fix is a token rather than three more literals: the pill stays
     * where it belongs (on controls), and every sheet and dialog reads from
     * here.
     */
    val SheetRadius = 20.dp
    val DialogRadius = 20.dp
}

// ── headers ─────────────────────────────────────────────────────────────────

/**
 * The hero header: Space Grotesk 28, a mono aggregate line under it, and an
 * optional trailing control (the Projects avatar).
 */
@Composable
fun HeroHeader(
    title: String,
    subtitle: String? = null,
    modifier: Modifier = Modifier,
    trailing: @Composable (() -> Unit)? = null,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(start = 20.dp, end = 16.dp, top = 14.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.Bold,
                fontSize = 28.sp,
                letterSpacing = (-0.03).em,
                color = MaterialTheme.colorScheme.onBackground,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (subtitle != null) {
                Spacer(Modifier.height(3.dp))
                Text(
                    text = subtitle,
                    style = ScanMeta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (trailing != null) {
            Spacer(Modifier.width(12.dp))
            trailing()
        }
    }
}

/** The circular avatar/settings affordance on the Projects hero. */
@Composable
fun AvatarButton(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier.size(46.dp),
        shape = CircleShape,
        color = MaterialTheme.colorScheme.surfaceContainer,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
    ) {
        IconButton(onClick = onClick) {
            Icon(icon, contentDescription = contentDescription, tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/**
 * A secondary screen's top bar: back arrow, display-face title, optional mono
 * sub-line, optional trailing actions. Deliberately hand-built rather than a
 * `TopAppBar` so the two-line title lands on the redesign's rhythm.
 */
@Composable
fun BackBar(
    title: String,
    subtitle: String? = null,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
    actions: @Composable (() -> Unit)? = null,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(start = 4.dp, end = 8.dp, top = 6.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconButton(onClick = onBack, modifier = Modifier.size(ScanDims.Touch)) {
            Icon(
                Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = "Back",
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.width(6.dp))
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.SemiBold,
                fontSize = 21.sp,
                letterSpacing = (-0.02).em,
                color = MaterialTheme.colorScheme.onBackground,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    style = ScanMeta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        actions?.invoke()
    }
}

// ── surfaces ────────────────────────────────────────────────────────────────

/**
 * The redesign's panel card: 20 dp radius, panel ground, soft border.
 *
 * **ROUND 25 item 116** gave it three optional parameters, all defaulting to
 * exactly what it drew before. The tracking-lost popup needed a card with a
 * semantic ground and a shadow, and it had two ways to get one: hand-roll a
 * `Column` with its own background, border and radius — which is what round 24
 * did, and is why the owner's note is *"align the style"* — or teach the app's
 * one card component the two things it could not yet express. A component that
 * cannot say "this card carries a warning" is a component every warning has to
 * work around, and every work-around is a card that drifts.
 *
 * ## ROUND 28 item 147 — the shadow is not a parameter any more
 *
 * `elevation: Dp` let any caller float any card, and by v0.9.12 the app had
 * four floating layers. Worse, the shadows were not decoration: with card-vs-
 * page at 1.08:1 (item 146) a card was literally invisible without one, so the
 * shadow was load-bearing and could not be removed a screen at a time.
 *
 * Item 146 fixes the ladder, which makes the shadow removable, and this
 * replaces the free parameter with the rule: exactly three things may float —
 * the record FAB, a warning or error banner, and a modal sheet with its scrim.
 * A caller says [floating] `= true` only if it is one of those, and there is no
 * longer a way to spell "6 dp because it looked flat".
 *
 * @param container the card's ground. Defaults to the neutral `surfaceContainer`.
 * @param borderColor the hairline. Defaults to `outlineVariant`.
 * @param floating level 1 (§C.5). Permitted for a modal over a scrim and a
 *   warning banner; forbidden everywhere else.
 */
@Composable
fun ScanCard(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    contentPadding: androidx.compose.foundation.layout.PaddingValues =
        androidx.compose.foundation.layout.PaddingValues(14.dp),
    container: Color? = null,
    borderColor: Color? = null,
    floating: Boolean = false,
    content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit,
) {
    val shape = RoundedCornerShape(ScanDims.CardRadius)
    val base = modifier
        .fillMaxWidth()
        // The shadow goes on FIRST and carries the same shape, so the card's
        // corners and its shadow's corners are one radius rather than two.
        .then(if (floating) Modifier.shadow(ScanElevation.Level1, shape) else Modifier)
        .background(container ?: MaterialTheme.colorScheme.surfaceContainer, shape)
        .border(1.dp, borderColor ?: MaterialTheme.colorScheme.outlineVariant, shape)
    Column(
        modifier = (if (onClick != null) base.clickable(onClick = onClick) else base).padding(contentPadding),
        content = content,
    )
}

// ── chips ───────────────────────────────────────────────────────────────────

/**
 * ROUND 28 items 149 / 152 — **THE CHIP: a state that differs from normal.
 * Read-only. Never a control.**
 *
 * Two changes, and the second is the one that matters.
 *
 * **It is not tappable.** The `onClick` parameter is gone. It rendered at ~24 dp
 * against a declared 44 dp minimum (item 152), and the fix is not more padding
 * — a chip that acts is a button wearing a chip's clothes, and the app already
 * has three button variants. Removing the parameter also removes the tap-target
 * violation by construction rather than by vigilance.
 *
 * **It is drawn only on deviation.** See [chipLaw]. In the owner's 66-project
 * fleet every scan is a D6, every scan is Quick scan and 65 of 66 are
 * georeferenced, so the three chips on every card carried exactly zero bits and
 * were the loudest element on the screen. 198 chips become about four, and the
 * two genuinely unusual scans become visible for the first time.
 *
 * Geometry per §C.4: 24 dp tall, 12 dp radius, the semantic at 12 % as fill,
 * **no border** (the fill is the shape; a border on a 12 % wash is what made
 * these read as pale-mint outlines on white), [ScanMetaCaps] text in the
 * semantic's theme-correct value.
 */
@Composable
fun ScanChip(
    text: String,
    modifier: Modifier = Modifier,
    color: Color? = null,
    showDot: Boolean = false,
    contentDescription: String? = null,
) {
    val fg = color ?: MaterialTheme.colorScheme.onSurfaceVariant
    val shape = RoundedCornerShape(ScanDims.ChipRadius)
    Row(
        modifier = modifier
            .defaultMinSize(minHeight = ScanDims.Chip)
            .background(fg.copy(alpha = 0.12f), shape)
            .padding(horizontal = ScanDims.S2, vertical = ScanDims.S1)
            .then(
                if (contentDescription != null) {
                    Modifier.describedAs(contentDescription)
                } else {
                    Modifier
                },
            ),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        if (showDot) {
            Box(Modifier.size(6.dp).background(fg, CircleShape))
        }
        Text(
            text = text,
            style = ScanMetaCaps,
            color = fg,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

private fun Modifier.describedAs(description: String): Modifier =
    this.semantics { contentDescription = description }

// ── buttons ─────────────────────────────────────────────────────────────────

/**
 * THE BUTTON, primary variant (§C.4). Filled Agtom orange, `onPrimary` label,
 * 48 dp, pill. **One per screen.**
 *
 * ROUND 28: the default height drops 54 → 48, onto the grid and onto the same
 * value as [SecondaryPill] and the icon button, so a Share/Export row is one
 * height rather than two that happen to match.
 *
 * **Disabled is [ScanDims.Button] of trough with ink-faint text, at full
 * opacity** — never a desaturated brand colour. The disconnected Scan screen
 * used to draw a muddy brown-orange FAB at full size with its glow intact: the
 * loudest object on the screen was the one thing that could not be pressed, and
 * a desaturated accent reads as *broken* rather than as *not yet*.
 */
@Composable
fun PrimaryPill(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    height: androidx.compose.ui.unit.Dp = ScanDims.Button,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(height),
        shape = RoundedCornerShape(percent = 50),
        colors = ButtonDefaults.buttonColors(
            containerColor = MaterialTheme.colorScheme.primary,
            contentColor = MaterialTheme.colorScheme.onPrimary,
            // §C.4's one disabled state, everywhere.
            disabledContainerColor = ScanColors.trough,
            disabledContentColor = ScanColors.inkFaint,
        ),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 22.dp),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(19.dp))
            Spacer(Modifier.width(9.dp))
        }
        Text(text, fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
    }
}

/**
 * THE BUTTON, secondary variant (§C.4): transparent, 1 dp line border, ink
 * label, 48 dp, pill.
 *
 * ROUND 28 item S13 folds the app's fifth button family into this one — the
 * bare orange text links (`Retry`, `Hide manual entry`) on the disconnected
 * Scan screen. They were styled identically to each other, so the *disclosure
 * toggle* out-shouted the *recovery action*, and being wider it out-shouted it
 * literally.
 */
@Composable
fun SecondaryPill(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    height: androidx.compose.ui.unit.Dp = ScanDims.Button,
) {
    OutlinedButton(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(height),
        shape = RoundedCornerShape(percent = 50),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
        colors = ButtonDefaults.outlinedButtonColors(
            containerColor = Color.Transparent,
            contentColor = MaterialTheme.colorScheme.onSurface,
            disabledContainerColor = ScanColors.trough,
            disabledContentColor = ScanColors.inkFaint,
        ),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 18.dp),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(19.dp))
            Spacer(Modifier.width(9.dp))
        }
        Text(text, style = ScanTitle)
    }
}

// ── segmented pill ──────────────────────────────────────────────────────────

/**
 * The redesign's segmented control: a pill trough with a pill-shaped ember
 * selection riding inside it. One implementation for every use — the Jobs
 * This-phone/Cloud/Bundle switcher, the sheet's 3D-orbit/AR-overlay row, the
 * 2/3/5 fps rates, colour mode — because they only differ in height.
 *
 * [enabled] dims the whole row to 50 % and refuses taps but keeps every label
 * legible, which is round 3's explicit resolution for the rate row with
 * keyframes off ("dimmed, not blank").
 *
 * ROUND 28: [fillWidth] exists because this composable used to call
 * `fillMaxWidth()` on itself, which a caller cannot undo. §D.7 and §D.4 both
 * want a segmented control as a ROW's trailing control — `Units [Meters|Feet]`
 * — and an unweighted child that fills starves the weighted title beside it, so
 * the row's name collapsed to nothing. Every existing caller wants the full
 * width and gets it by default; a trailing control passes false.
 *
 * ## ROUND 29 item 170(a) — **one option is not a control**
 *
 * With `FeatureFlags.FOLLOW_CAMERA_ENABLED` off, the Advanced sheet's View row
 * offered a single `3D orbit` segment — which renders as a **full-width filled
 * orange button** that does nothing when pressed, directly under a row already
 * reading out `3D orbit` on its right. The owner counted it among six oranges
 * on one sheet.
 *
 * A segmented control with nothing to choose between is a read-out wearing a
 * button's clothes. It draws **nothing**, and the value row above it — which
 * every call site already has, because that is the pattern — is what states the
 * value. The rule is here rather than at the one call site because
 * `DetailLevels.selectableOn` and the refresh notches can both collapse to one
 * option on some device, and a rule that lives in the component cannot be
 * forgotten by the next caller.
 */
@Composable
fun <T> SegmentedPill(
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
    height: androidx.compose.ui.unit.Dp = ScanDims.SegmentChip,
    enabled: Boolean = true,
    fillWidth: Boolean = true,
) {
    if (options.size < 2) return
    val shape = RoundedCornerShape(percent = 50)
    Row(
        modifier = modifier
            .then(if (fillWidth) Modifier.fillMaxWidth() else Modifier)
            .alpha(if (enabled) 1f else 0.5f)
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, shape)
            .padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        options.forEach { (value, label) ->
            val isSelected = value == selected
            Box(
                modifier = Modifier
                    .then(if (fillWidth) Modifier.weight(1f) else Modifier)
                    .height(height)
                    .background(if (isSelected) MaterialTheme.colorScheme.primary else Color.Transparent, shape)
                    .clickable(enabled = enabled, role = Role.RadioButton) { onSelect(value) },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = label,
                    fontFamily = DisplayFontFamily,
                    fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Medium,
                    fontSize = 14.sp,
                    color = if (isSelected) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

// ── stat panel ──────────────────────────────────────────────────────────────

data class Stat(val value: String, val label: String, val testTag: String? = null)

/**
 * The capture screen's four-number mono panel: value in JetBrains Mono over a
 * wide-tracked uppercase mono label, four equal columns, on the panel ground.
 */
@Composable
fun StatPanel(stats: List<Stat>, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainer, RoundedCornerShape(ScanDims.TileRadius))
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(ScanDims.TileRadius))
            .padding(vertical = 11.dp),
    ) {
        stats.forEach { stat ->
            Column(
                modifier = Modifier.weight(1f),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    text = stat.value,
                    style = ScanMeta,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    modifier = if (stat.testTag != null) Modifier.testTag(stat.testTag) else Modifier,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    text = stat.label.uppercase(),
                    style = ScanMetaCaps,
                    color = ScanColors.inkFaint,
                    maxLines = 1,
                )
            }
        }
    }
}

// ── bottom-sheet parts ──────────────────────────────────────────────────────

/** Mono, uppercase, ember — the sheet's own section rule (`AR & CAMERA`, `DISPLAY`, `DEVICE`). */
@Composable
fun SheetSection(text: String, modifier: Modifier = Modifier) {
    // ROUND 28 item 164's accent law, applied to every sheet in the app at
    // once: a section header is a PASSIVE LABEL. The Agtom orange is allowed
    // twice per screen — the primary action and the active tab — and this was
    // spending it on `TRACKING & CAMERA`, `DISPLAY`, `PERFORMANCE`, `SCAN`,
    // `CONNECTION`, `RECORDING` and `DEVICE`, seven headers in one sheet that
    // do nothing. When orange marks nine things it stops meaning "the action".
    Column(modifier.fillMaxWidth().padding(top = ScanDims.S4, bottom = ScanDims.S1)) {
        Text(text.uppercase(), style = ScanMetaCaps, color = ScanColors.inkMute)
        Spacer(Modifier.height(ScanDims.S2))
        HorizontalDivider(thickness = ScanDims.Hair, color = MaterialTheme.colorScheme.outlineVariant)
    }
}

/**
 * A sheet row's label line: mono uppercase caption, an optional lighter range
 * hint, and the live read-out pushed to the right — the mockup's
 * `.sheet-row > label` exactly, including "the range lives in the label, not
 * in a second row under the track".
 *
 * ## ROUND 27 item 130 — **an informational value never ends in an ellipsis**
 *
 * The hint carried `maxLines = 1` and `TextOverflow.Ellipsis` inside a
 * `weight(1f, fill = false)`, so on a 411 dp phone the Advanced sheet read
 * `SCANNER TRACKING phone-tracked ·…` and `POINT SIZE 0.1 – 12 px · 0.1 st…`.
 * Both are the same defect and it is a specific one: an ellipsis is a promise
 * that the rest is reachable — by a tap, by a scroll, by a long-press — and
 * here there is nothing to reach it with. The operator is simply told that
 * something was said and not what.
 *
 * So the hint WRAPS. Not "gets a bigger cap": no cap at all, because a cap is
 * how this comes back the first time somebody writes a longer range. The row
 * grows a line instead, which costs 12 dp in a sheet that already scrolls, and
 * the label and read-out stay bottom-aligned with it so the row still reads as
 * one line of caption with a number at the end.
 *
 * The read-out keeps `maxLines = 1`, and that is a different decision rather
 * than an oversight: it is a formatted quantity (`1.0 px`, `LIMITED`, `1.00`)
 * whose width is bounded by its own format string, and wrapping a number is
 * how `1.0 px` becomes `1.0` over `px`.
 */
@Composable
fun SheetRowLabel(
    label: String,
    readout: String,
    modifier: Modifier = Modifier,
    hint: String? = null,
    /** ROUND 27 item 130 — so a geometry test can name the row whose hint must WRAP. */
    hintTestTag: String? = null,
    readoutColor: Color = MaterialTheme.colorScheme.onSurface,
) {
    // ── ROUND 28 items 158 + 167: this row printed through itself ───────────
    //
    // It was a `Row` with `Alignment.Bottom`, an unweighted label, a hint at
    // `weight(1f, fill = false)` and then a SECOND weighted `Spacer`. Two
    // weighted children competed for the same space, and a hint long enough to wrap —
    // round 27 item 130 made hints wrap on purpose — drew its first line ABOVE
    // the label it was supposed to sit beside. On the AVD the Advanced sheet
    // rendered `how the cloud is framed` printed straight through `VIEW`, and
    // `phone-tracked · read-only` through `SCANNER TRACKING`. Text over text,
    // in the app's most-used sheet, invisible to every semantics test.
    //
    // It is THE ROW now: a weighted `Column` for the label and its detail, the
    // readout right-aligned beside it, top-aligned. A column cannot overlap
    // itself. The label is also sentence case — `Colour mode` is a phrase a
    // person says out loud, and §C.2 reserves Meta Caps for codes.
    Row(
        modifier = modifier.fillMaxWidth().padding(vertical = ScanDims.S1),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.S2),
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                label,
                style = ScanBody,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.testTag(hintTestTag?.let { "$it-label" } ?: "sheetRowLabel"),
            )
            if (hint != null) {
                Text(
                    hint,
                    style = ScanMeta,
                    color = ScanColors.inkMute,
                    modifier = Modifier.testTag(hintTestTag ?: "sheetRowHint"),
                )
            }
        }
        Text(readout, style = ScanMeta, color = readoutColor, maxLines = 1)
    }
}

/**
 * ROUND 17 item 67 — a plain statement of fact inside a sheet, with no control
 * beside it. Not a hint (which suggests an action) and not a warning (which
 * suggests a problem): the app is telling the operator what it does with their
 * camera, and that sentence needs no adornment.
 */
@Composable
fun SheetNote(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        style = MaterialTheme.typography.bodySmall,
        color = ScanColors.inkFaint,
        modifier = modifier.fillMaxWidth().padding(vertical = 8.dp),
    )
}

/** A 44 dp switch row: title + sub-label on the left, a big toggle on the right. */
@Composable
fun SheetSwitchRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = ScanDims.SwitchRow)
            .padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                title,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.SemiBold,
                fontSize = 14.sp,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Spacer(Modifier.height(2.dp))
            Text(subtitle, style = ScanMetaCaps, color = ScanColors.inkFaint)
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = MaterialTheme.colorScheme.primary,
                checkedBorderColor = MaterialTheme.colorScheme.primary,
                uncheckedThumbColor = MaterialTheme.colorScheme.onSurfaceVariant,
                uncheckedTrackColor = MaterialTheme.colorScheme.surfaceContainerHigh,
                uncheckedBorderColor = MaterialTheme.colorScheme.outline,
                // A disabled switch must still say which way it is set — M3's
                // defaults grey both states to nearly the same thing, which
                // reads as "off" for a row that is on and merely unavailable.
                disabledCheckedThumbColor = Color.White.copy(alpha = 0.6f),
                disabledCheckedTrackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.4f),
                disabledCheckedBorderColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.4f),
            ),
        )
    }
}

/** A slider row with the redesign's 28 dp thumb. */
@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
fun SheetSlider(
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    steps: Int = 0,
    contentDescription: String? = null,
) {
    Slider(
        value = value.coerceIn(range),
        valueRange = range,
        steps = steps,
        onValueChange = onValueChange,
        modifier = modifier
            .fillMaxWidth()
            .height(ScanDims.Touch)
            .then(if (contentDescription != null) Modifier.describedAs(contentDescription) else Modifier),
        // ── ROUND 29 item 170(a): the accent is the KNOB, not the bar ──────
        //
        // Two sliders with a full orange active track put four orange objects
        // on the Advanced sheet before the Done button — the owner counted
        // six in one screenshot. The track is a scale (how far along am I),
        // the thumb is the control (the thing under my finger), and §C's accent
        // law spends the brand colour on controls. Neutral bar, ember knob:
        // the position still reads at arm's length because the knob is 28 dp.
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.outline,
            inactiveTrackColor = MaterialTheme.colorScheme.surfaceContainerHigh,
        ),
        thumb = {
            Box(
                Modifier
                    .size(ScanDims.SliderThumb)
                    .background(MaterialTheme.colorScheme.primary, CircleShape)
                    .border(3.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.35f), CircleShape),
            )
        },
        // M3's default track draws a gap around the thumb and a stop dot at the
        // far end — both sized for M3's own 4 dp-wide thumb, and both read as
        // artefacts under a 28 dp one (a stub of active track to the left of
        // the knob, a stray dot to the right). Seen on a booted emulator, not
        // reasoned about: gap and stop indicator off.
        track = { sliderState ->
            SliderDefaults.Track(
                sliderState = sliderState,
                colors = SliderDefaults.colors(
                    activeTrackColor = MaterialTheme.colorScheme.outline,
                    inactiveTrackColor = MaterialTheme.colorScheme.surfaceContainerHigh,
                ),
                drawStopIndicator = null,
                thumbTrackGapSize = 0.dp,
                modifier = Modifier.height(6.dp),
            )
        },
    )
}

/**
 * One read-only Diagnostics row: a sentence-case label on the left, a mono
 * value on the right, hairline under it. Round 4's resolution is explicit that
 * nothing in this sheet is a target, so this composable is deliberately not
 * clickable and takes no `onClick`.
 */
@Composable
fun DiagRow(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    valueColor: Color = MaterialTheme.colorScheme.onSurface,
    testTag: String? = null,
) {
    Column(modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            // ROUND 27 item 130, the same sweep one sheet over: the LABEL is
            // a short constant and may ellipsize; the VALUE is what the
            // operator opened Diagnostics to read and must never be cut. The
            // weight moved from the label to the value, so a long verdict
            // ("phone-tracked · limited", an ARCore error string) wraps
            // right-aligned instead of being trimmed to fit a label that had
            // already taken the row.
            Text(
                label.uppercase(),
                style = ScanMetaCaps,
                color = ScanColors.inkFaint,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.width(10.dp))
            Text(
                value,
                style = ScanMeta,
                color = valueColor,
                textAlign = androidx.compose.ui.text.style.TextAlign.End,
                modifier = (if (testTag != null) Modifier.testTag(testTag) else Modifier)
                    .weight(1f),
            )
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    }
}

/** A muted explanatory line — the mockup's `.hint`. */
@Composable
fun Hint(text: String, modifier: Modifier = Modifier, color: Color = MaterialTheme.colorScheme.onSurfaceVariant) {
    Text(
        text,
        modifier = modifier,
        style = MaterialTheme.typography.bodySmall,
        color = color,
    )
}

// ── ROUND 28: the design system's own primitives ─────────────────────────────

/**
 * §C.5 — **two elevation levels, and that is all.**
 *
 * Level 0 is everything: cards, rows, chips, buttons, the viewport, headers,
 * list items, and — new this round — the tab bar. Level 1 is three things: the
 * record FAB, a warning or error banner, and a modal sheet with its scrim.
 *
 * There is no level 2, and there is deliberately no `Level0` constant: "no
 * shadow" is the absence of a modifier, not a value to pass.
 */
object ScanElevation {
    /** `y = 4, blur = 16, 20 % black` — §C.5's one floating shadow. */
    val Level1 = 4.dp
}

/**
 * ROUND 28 item 149 — **the chip law, as a predicate.**
 *
 * > A chip may only be drawn when its value differs from the norm for the set
 * > being displayed.
 *
 * The norm is the **mode** of the visible collection, not a hard-coded default.
 * That distinction is the whole design: in a fleet of 66 D6 scans the `D6` chip
 * says nothing and must vanish, but in a mixed fleet it says something and must
 * appear — and neither case needs a rule written about it, because the mode
 * moves with the data.
 *
 * Ties go to "everything is normal" (no chip). A two-value set split 33/33
 * genuinely has no norm, and drawing 66 chips because the modal count tied
 * would be the loudest possible answer to the quietest possible question.
 *
 * `null` values are ignored when computing the mode but are never themselves a
 * deviation: "unknown" is not a state the operator can act on.
 */
fun <T : Any> modalValue(values: List<T?>): T? {
    val present = values.filterNotNull()
    if (present.isEmpty()) return null
    val counts = present.groupingBy { it }.eachCount()
    val top = counts.maxOf { it.value }
    val leaders = counts.filterValues { it == top }.keys
    // A tie is not a norm. See the header.
    return if (leaders.size == 1) leaders.first() else null
}

/**
 * True when [value] deviates from [norm] and therefore earns a chip.
 *
 * Written as its own function rather than inlined as `value != norm` so that
 * the two cases where the answer is "no chip" for a reason other than equality
 * — an unknown value, and a set with no norm at all — are stated once and
 * cannot drift apart between the four screens that draw chips.
 */
fun <T : Any> deviates(value: T?, norm: T?): Boolean =
    value != null && norm != null && value != norm

/**
 * ROUND 28 §C.4 — **THE ROW. The default. Use this unless there is a reason
 * not to.**
 *
 * ```
 * 56 dp (72 with a thumbnail) · 16 dp horizontal · hairline between siblings
 * [dot | icon | thumbnail] [Title, ellipsised] ····· [Meta] [⋯ or chevron — never both]
 * ```
 *
 * The model is already in the codebase and the review names it the best-built
 * pattern in the app: Profile's "This phone" spec table (finding F6). Meta-caps
 * label left, mono value right, one row height. Everything below is that table,
 * generalised exactly as far as the eleven things it replaces need.
 *
 * It replaces `ProjectCard`, `DiagRow`, `DetailRow`, `FactRow`, `ManifestRow`,
 * `SwitchRow`, `ToggleRow`, `PickerRow`, `SliderRow`, `JobTile`,
 * `PreconditionRow`, `MountStateRow`, `NavCard` and fourteen of the
 * twenty-seven card variants.
 *
 * **A row states its own state and carries its own fix.** That is why [detail]
 * and [trailing] exist together: the readiness rows on the Scan screen are
 * `● Sensor · Not found` + *"Plug it in, then retry."* + a Secondary `Retry`,
 * and expressing that as a row rather than as a card with a loose control row
 * underneath is what let §D.1 delete five of the six controls on that screen.
 *
 * @param titleStyle **[ScanTitle]/[ScanBody] for a word, [ScanMetaCaps] for a
 *   code.** §C.2's rule reaches the row: an export sheet's `PLY` / `LAS 1.4` /
 *   `PCD` are codes a person spells out, and rendering them in body sentence
 *   case is the same category error as rendering `Quick scan` in
 *   instrument-panel caps. The default is [ScanBody] because the overwhelming
 *   majority of rows are titled with words.
 * @param leading a status dot, a 24 dp icon or a 56 dp thumbnail — the three
 *   things that may sit at a row's leading edge.
 * @param detail the ≤12-word second line. Present only when the row is not
 *   nominal; a row in its normal state is one line.
 * @param detailSlot a second line that is not a string — the Jobs queue's
 *   inline 4 dp progress bar (§C.6's determinate-loading pattern) is the case
 *   this exists for. Mutually exclusive with [detail]: a row has one second
 *   line or none, and offering both would immediately produce rows with two.
 * @param meta the right-aligned value, in [ScanMeta].
 * @param trailing the row's own control, or a chevron. **Never both a chevron
 *   and a `⋯`** (finding P1e — two affordances for one row, both stealing width
 *   from the title, which is why titles sat at the ellipsis threshold).
 */
@Composable
fun ScanRow(
    title: String,
    modifier: Modifier = Modifier,
    detail: String? = null,
    detailSlot: (@Composable () -> Unit)? = null,
    meta: String? = null,
    metaColor: Color? = null,
    titleColor: Color? = null,
    titleStyle: androidx.compose.ui.text.TextStyle = ScanBody,
    onClick: (() -> Unit)? = null,
    minHeight: androidx.compose.ui.unit.Dp = ScanDims.Row,
    leading: @Composable (() -> Unit)? = null,
    trailing: @Composable (() -> Unit)? = null,
) {
    val base = modifier.fillMaxWidth().defaultMinSize(minHeight = minHeight)
    Row(
        modifier = (if (onClick != null) base.clickable(onClick = onClick) else base)
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S2),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
    ) {
        leading?.invoke()
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                style = titleStyle,
                color = titleColor ?: MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (detail != null) {
                Text(
                    text = detail,
                    style = ScanMeta,
                    color = ScanColors.inkMute,
                    // §C.4's ROW is one line of title over one line of detail.
                    // A wrapping detail is what turned the Projects row's
                    // `120.3 K pts · Aug 21, 2026` into two lines and pushed the
                    // grade off the baseline it shares with the title — and a
                    // detail that needs two lines is a detail over its
                    // twelve-word budget.
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            } else if (detailSlot != null) {
                Spacer(Modifier.height(ScanDims.S1))
                detailSlot()
            }
        }
        if (meta != null) {
            Text(
                text = meta,
                style = ScanMeta,
                color = metaColor ?: ScanColors.inkMute,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        trailing?.invoke()
    }
}

/** The 8 dp semantic dot a row or a grade leads with. */
@Composable
fun StatusDot(color: Color, modifier: Modifier = Modifier) {
    Box(modifier.size(8.dp).background(color, CircleShape))
}

/**
 * A group of [ScanRow]s under one heading — §C.4's THE CARD, in its commonest
 * form. Hairlines between siblings, nothing around the outside but the card's
 * own 1 dp border.
 *
 * `contentPadding` is zero because the rows carry their own 16 dp: a card that
 * padded its rows *and* let the rows pad themselves is how a 56 dp row became
 * 84 dp, and is most of why Settings was five floating slabs with big gaps.
 */
@Composable
fun ScanRowCard(
    modifier: Modifier = Modifier,
    rows: List<@Composable () -> Unit>,
) {
    if (rows.isEmpty()) return
    ScanCard(modifier = modifier, contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
        rows.forEachIndexed { index, row ->
            if (index > 0) {
                HorizontalDivider(
                    thickness = ScanDims.Hair,
                    color = MaterialTheme.colorScheme.outlineVariant,
                )
            }
            row()
        }
    }
}

/**
 * A section label above a card: [ScanMetaCaps] in `ink-mute`.
 *
 * ROUND 28 item 164 — **not orange.** Settings spent the one brand accent on
 * `DISPLAY`, `ADVANCED FEATURES` and `ABOUT`, three passive labels that do
 * nothing, which is the mechanism of accent inflation: when orange marks nine
 * things on a screen it stops meaning "the action" and becomes decoration. The
 * accent law allows it twice per screen — the primary action and the active tab.
 */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text.uppercase(),
        style = ScanMetaCaps,
        color = ScanColors.inkMute,
        modifier = modifier.padding(
            start = ScanDims.ScreenMargin,
            end = ScanDims.ScreenMargin,
            top = ScanDims.S3,
            bottom = ScanDims.S1,
        ),
    )
}

/**
 * THE BUTTON, icon variant (§C.4) — and the fix for the owner's
 * "icons invisible in dark mode".
 *
 * ROUND 28 item 168, his exact call: **full ink colour, not muted**, ~2 dp
 * stroke weight, and a **subtle 6 % circular wash** behind the glyph, in both
 * themes. The wash is what gives a bare glyph an edge to be seen against on
 * either ground without adding a border, and the full-ink tint is what stops an
 * icon button reading as disabled — the previous `onSurfaceVariant` tint made
 * every icon in the app look like it was greyed out.
 *
 * 48 dp target around a 24 dp glyph, per §C.1's minimum.
 */
@Composable
fun ScanIconButton(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    toggledOn: Boolean = false,
    tint: Color? = null,
) {
    val ink = when {
        !enabled -> ScanColors.inkFaint
        toggledOn -> ScanColors.primaryInk
        else -> tint ?: ScanColors.ink
    }
    val wash = if (toggledOn) ScanColors.primaryInk.copy(alpha = 0.12f) else ScanColors.ink.copy(alpha = 0.06f)
    Box(
        modifier = modifier
            .size(ScanDims.Touch)
            .background(wash, CircleShape)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .describedAs(contentDescription),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(24.dp), tint = ink)
    }
}

/**
 * §C.6's empty state, one way everywhere.
 *
 * `32 dp ink-faint icon → 16 → Title (≤6 words) → 8 → Body (≤12 words) → 24 →
 * one Primary button naming the fix`. **No card**: an empty state wrapped in a
 * card is a card containing nothing, which is the one thing a card must never
 * be.
 *
 * @param onViewport true when this state is drawn **inside the 3-D viewport**
 *   rather than on the page. The viewport is dark in BOTH themes by design
 *   (`ScanColors.viewport` — a point cloud needs a dark room), so a state drawn
 *   there must take the dark theme's ink. The light sweep caught the
 *   alternative: Review's `Not processed yet` rendered in near-black
 *   `onSurface` on a `#1B222A` slab, an unreadable headline above a perfectly
 *   readable body. It is a parameter rather than a guess from the background
 *   because the composable cannot see what it is drawn on.
 */
@Composable
fun ScanEmptyState(
    icon: ImageVector,
    title: String,
    body: String,
    modifier: Modifier = Modifier,
    onViewport: Boolean = false,
    action: (@Composable () -> Unit)? = null,
) {
    val ink = if (onViewport) com.lidarscan.app.ui.theme.Ink else MaterialTheme.colorScheme.onSurface
    val mute = if (onViewport) com.lidarscan.app.ui.theme.InkMute else ScanColors.inkMute
    val faint = if (onViewport) com.lidarscan.app.ui.theme.InkFaint else ScanColors.inkFaint
    Column(
        modifier = modifier.fillMaxWidth().padding(ScanDims.S8),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(ScanDims.S8), tint = faint)
        Spacer(Modifier.height(ScanDims.S4))
        Text(title, style = ScanTitle, color = ink)
        Spacer(Modifier.height(ScanDims.S2))
        Text(
            body,
            style = ScanBody,
            color = mute,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
        )
        if (action != null) {
            Spacer(Modifier.height(ScanDims.S6))
            action()
        }
    }
}
