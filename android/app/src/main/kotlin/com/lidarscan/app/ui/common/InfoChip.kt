package com.lidarscan.app.ui.common

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile

/**
 * ROUND 28 item 162 — **one chip in the app, and this is no longer a second
 * one.**
 *
 * §C.4 lists `InfoChip`, `SensorBadge` and `ProfileChip` among the eleven chip
 * variants that collapse into `ScanChip`, and the Projects list — this file's
 * original reason to exist — stopped calling any of them this round. What kept
 * the file alive is that four Mid-360 connect readouts and the Advanced-only
 * project detail screen still call it, and those belong to other owners.
 *
 * So the shape is deleted without the call sites moving: these are now thin
 * forwarders onto [ScanChip], which means the geometry (24 dp, radius 12, the
 * semantic at 12 % with **no border**, Meta Caps) is defined once and those
 * screens inherit §C.4 without being edited. `Surface`, the 50 % radius, the
 * 16 % fill and `labelMedium` are all gone — the pale-mint outlined pill the
 * review measured is not reachable from anywhere any more.
 */
@Deprecated(
    "ROUND 28 §C.4: use ScanChip, and only when the value deviates from the set's norm.",
    ReplaceWith("ScanChip(text, modifier, color)", "com.lidarscan.app.ui.components.ScanChip"),
)
@Composable
fun InfoChip(
    text: String,
    color: Color,
    modifier: Modifier = Modifier,
) {
    ScanChip(text = text, modifier = modifier, color = color)
}

@Composable
fun SensorBadge(sensor: SensorType, modifier: Modifier = Modifier) {
    // ROUND 25 item 119: every sensor names its own badge colour explicitly.
    // No `else` — a fourth sensor must break this build rather than quietly
    // inherit whichever tint happened to be last in the list.
    // ROUND 28 item 144: the exhaustive `when` moved to `ScanColorScheme.sensor`
    // so the colour is the current theme's. The no-`else` property is kept
    // there — it is `SensorType.badgeTint`'s job, and it still breaks the build
    // in `:core` if a fourth sensor arrives.
    // ROUND 28 item 162: `D6` is a code a person spells out, so it is upper
    // case in Meta Caps rather than title case in a label style.
    ScanChip(
        text = sensor.badgeLabel.uppercase(),
        modifier = modifier,
        color = ScanColors.current.sensor(sensor),
    )
}

@Composable
fun ProfileChip(profile: WorkflowProfile, modifier: Modifier = Modifier) {
    // ROUND 28 item 162: was `colorScheme.tertiary` — a brand-ish accent spent
    // on a passive descriptor, which is the accent inflation §C.3 names. A
    // workflow profile is not a semantic state; it is a fact, so it is ink-mute.
    ScanChip(
        text = profile.displayName.uppercase(),
        modifier = modifier,
        color = ScanColors.inkMute,
    )
}
