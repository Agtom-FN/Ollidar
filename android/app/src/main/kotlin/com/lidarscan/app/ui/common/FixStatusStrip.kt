package com.lidarscan.app.ui.common

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.lidarscan.core.gnss.CaptureGate
import com.lidarscan.core.gnss.CaptureGateVerdict
import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.gnss.GnssFixSnapshot
import com.lidarscan.core.gnss.NtripState
import com.lidarscan.core.gnss.NtripStatsSnapshot

/**
 * B9 — the fix-status strip (Tech Spec §3.13's "Capture (… RTK status strip)",
 * §2.3's "Fix states surfaced: RTK Fixed / Float / DGPS / Single / none").
 *
 * The colours are A14's own fix-quality palette
 * ([com.lidarscan.core.render.DisplayParams.DEFAULT_FIX_COLORS]) rather than a
 * second set invented here, so a point coloured by fix quality in the 3D view
 * and the badge in the strip agree on what "Float" looks like.
 */
@Composable
fun fixColor(fix: FixType): Color {
    val c = com.lidarscan.core.render.DisplayParams.DEFAULT_FIX_COLORS[fix.code]
    return Color(c.r / 255f, c.g / 255f, c.b / 255f, 1f)
}

@Composable
fun FixStatusStrip(
    fix: GnssFixSnapshot,
    ntrip: NtripStatsSnapshot,
    modifier: Modifier = Modifier,
    compact: Boolean = false,
) {
    Row(
        modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, RoundedCornerShape(10.dp))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.size(12.dp).background(fixColor(fix.fix), CircleShape))
        Column(Modifier.weight(1f)) {
            Text(
                if (fix.hasFix) "${fix.fix.label} · ${fix.satellites} sats" else "No rover data",
                style = MaterialTheme.typography.titleSmall,
            )
            if (!compact) {
                Text(
                    fix.accuracyText(),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (ntrip.state != NtripState.IDLE) {
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    // "Connected" and "receiving corrections" are different
                    // claims, and A10 §8 is explicit that a UI must not infer
                    // the second from the first.
                    if (ntrip.receiving) "corrections live" else ntrip.state.label.lowercase(),
                    style = MaterialTheme.typography.labelMedium,
                    color = if (ntrip.receiving) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                )
                if (!compact) {
                    Text(
                        ntrip.ageText(),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

/**
 * §3.4's capture gate, as a banner. Blocking and warning look different on
 * purpose — a warning that looks like a block trains an operator to ignore both.
 */
@Composable
fun CaptureGateBanner(gate: CaptureGate, modifier: Modifier = Modifier) {
    if (gate.verdict == CaptureGateVerdict.OK) return
    val bg = when (gate.verdict) {
        CaptureGateVerdict.BLOCK -> MaterialTheme.colorScheme.errorContainer
        else -> MaterialTheme.colorScheme.tertiaryContainer
    }
    val fg = when (gate.verdict) {
        CaptureGateVerdict.BLOCK -> MaterialTheme.colorScheme.onErrorContainer
        else -> MaterialTheme.colorScheme.onTertiaryContainer
    }
    Column(
        modifier
            .fillMaxWidth()
            .background(bg, RoundedCornerShape(10.dp))
            .padding(12.dp),
    ) {
        Text(
            (if (gate.verdict == CaptureGateVerdict.BLOCK) "Capture blocked — " else "Warning — ") + gate.headline,
            style = MaterialTheme.typography.titleSmall,
            color = fg,
        )
        Text(gate.detail, style = MaterialTheme.typography.bodySmall, color = fg)
    }
}
