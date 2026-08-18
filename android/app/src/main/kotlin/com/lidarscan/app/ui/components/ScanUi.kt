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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.app.ui.theme.MonoValue
import com.lidarscan.app.ui.theme.SheetSectionLabel

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
    /** Bottom-sheet segmented row that is the sheet's largest target (the View row). */
    val SegmentTall = 48.dp

    /** Ordinary sheet segmented row (colour mode, keyframe rate). */
    val SegmentChip = 40.dp

    /** Switch row minimum height. */
    val SwitchRow = 44.dp

    /** Slider thumb. */
    val SliderThumb = 28.dp

    /** Minimum touch target anywhere in the app. */
    val Touch = 44.dp

    /** The floating capsule tab bar. */
    val TabBar = 58.dp
    val TabBarSideInset = 16.dp
    val TabBarBottomInset = 12.dp

    /** Room scrollable content must leave so it clears the floating tab bar. */
    val TabBarClearance = 86.dp

    val CardRadius = 20.dp
    val TileRadius = 14.dp

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
                    style = MonoMeta,
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
                    style = MonoMeta,
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

/** The redesign's panel card: 20 dp radius, panel ground, soft border. */
@Composable
fun ScanCard(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    contentPadding: androidx.compose.foundation.layout.PaddingValues =
        androidx.compose.foundation.layout.PaddingValues(14.dp),
    content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit,
) {
    val shape = RoundedCornerShape(ScanDims.CardRadius)
    val base = modifier
        .fillMaxWidth()
        .background(MaterialTheme.colorScheme.surfaceContainer, shape)
        .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
    Column(
        modifier = (if (onClick != null) base.clickable(onClick = onClick) else base).padding(contentPadding),
        content = content,
    )
}

// ── chips ───────────────────────────────────────────────────────────────────

/**
 * The mockup's `.chip`: a hairline pill with an optional leading dot in the
 * chip's own colour. Used for sensor identity, profile, GEOREF ✓, RTK state
 * and every on-viewport readout.
 */
@Composable
fun ScanChip(
    text: String,
    modifier: Modifier = Modifier,
    color: Color? = null,
    showDot: Boolean = false,
    onClick: (() -> Unit)? = null,
    contentDescription: String? = null,
) {
    val fg = color ?: MaterialTheme.colorScheme.onSurfaceVariant
    val shape = RoundedCornerShape(percent = 50)
    val base = modifier
        .background(if (color != null) fg.copy(alpha = 0.13f) else Color.Transparent, shape)
        .border(1.dp, if (color != null) fg.copy(alpha = 0.42f) else MaterialTheme.colorScheme.outline, shape)
    Row(
        modifier = (
            if (onClick != null) {
                base.clickable(role = Role.Button, onClick = onClick)
            } else {
                base
            }
            )
            .padding(horizontal = 9.dp, vertical = 4.dp)
            .then(
                if (contentDescription != null) {
                    Modifier.describedAs(contentDescription)
                } else {
                    Modifier
                },
            ),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        if (showDot) {
            Box(Modifier.size(6.dp).background(fg, CircleShape))
        }
        Text(
            text = text,
            style = MonoLabel,
            color = fg,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

private fun Modifier.describedAs(description: String): Modifier =
    this.semantics { contentDescription = description }

// ── buttons ─────────────────────────────────────────────────────────────────

/** The ember pill — the one primary action per screen. */
@Composable
fun PrimaryPill(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    height: androidx.compose.ui.unit.Dp = 54.dp,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(height),
        shape = RoundedCornerShape(percent = 50),
        colors = ButtonDefaults.buttonColors(
            containerColor = MaterialTheme.colorScheme.primary,
            contentColor = MaterialTheme.colorScheme.onPrimary,
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

/** The quiet pill — the "other" half of a split action row. */
@Composable
fun SecondaryPill(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    height: androidx.compose.ui.unit.Dp = 54.dp,
) {
    OutlinedButton(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(height),
        shape = RoundedCornerShape(percent = 50),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
        colors = ButtonDefaults.outlinedButtonColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            contentColor = MaterialTheme.colorScheme.onSurface,
        ),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 18.dp),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(19.dp))
            Spacer(Modifier.width(9.dp))
        }
        Text(text, fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
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
 */
@Composable
fun <T> SegmentedPill(
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
    height: androidx.compose.ui.unit.Dp = ScanDims.SegmentChip,
    enabled: Boolean = true,
) {
    val shape = RoundedCornerShape(percent = 50)
    Row(
        modifier = modifier
            .fillMaxWidth()
            .alpha(if (enabled) 1f else 0.5f)
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, shape)
            .padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        options.forEach { (value, label) ->
            val isSelected = value == selected
            Box(
                modifier = Modifier
                    .weight(1f)
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
                    style = MonoValue.copy(fontSize = 17.sp),
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    modifier = if (stat.testTag != null) Modifier.testTag(stat.testTag) else Modifier,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    text = stat.label.uppercase(),
                    style = MonoLabel.copy(fontSize = 9.sp),
                    color = InkFaint,
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
    Column(modifier.fillMaxWidth().padding(top = 14.dp, bottom = 2.dp)) {
        Text(text.uppercase(), style = SheetSectionLabel, color = Ember)
        Spacer(Modifier.height(6.dp))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    }
}

/**
 * A sheet row's label line: mono uppercase caption, an optional lighter range
 * hint, and the live read-out pushed to the right — the mockup's
 * `.sheet-row > label` exactly, including "the range lives in the label, not
 * in a second row under the track".
 */
@Composable
fun SheetRowLabel(
    label: String,
    readout: String,
    modifier: Modifier = Modifier,
    hint: String? = null,
    readoutColor: Color = MaterialTheme.colorScheme.onSurface,
) {
    Row(
        modifier = modifier.fillMaxWidth().padding(bottom = 6.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        Text(label.uppercase(), style = MonoLabel, color = InkFaint)
        if (hint != null) {
            Spacer(Modifier.width(7.dp))
            Text(
                hint,
                style = MonoLabel.copy(fontSize = 9.5.sp, letterSpacing = 0.06.em),
                color = InkFaint.copy(alpha = 0.85f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f, fill = false),
            )
        }
        Spacer(Modifier.weight(1f))
        Text(readout, style = MonoValue, color = readoutColor, maxLines = 1)
    }
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
            Text(subtitle, style = MonoLabel.copy(fontSize = 9.5.sp, letterSpacing = 0.06.em), color = InkFaint)
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
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primary,
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
                    activeTrackColor = MaterialTheme.colorScheme.primary,
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
            Text(
                label.uppercase(),
                style = MonoLabel,
                color = InkFaint,
                modifier = Modifier.weight(1f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.width(10.dp))
            Text(
                value,
                style = MonoValue,
                color = valueColor,
                maxLines = 1,
                modifier = if (testTag != null) Modifier.testTag(testTag) else Modifier,
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
