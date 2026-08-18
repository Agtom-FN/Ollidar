package com.lidarscan.app.ui.components

import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.lidarscan.app.ui.theme.SemBad
import com.lidarscan.core.capture.StitchResult

/**
 * ROUND 16 item 61 — **one implementation of "what processing found".**
 *
 * Owner, on 0.9.0: *"there are some tab and window show the same too"*.
 *
 * This is the clearest instance of it in the app. A finished `StitchResult`
 * has three sentences worth showing — what moved (`detail`), how well the map
 * agrees with itself (`selfCheckLine`, ROUND 15 item 57), and whether the
 * mount watchdog fired (`mountWarning`) — and until this round they were laid
 * out TWICE, in two files, by two nearly-identical blocks of Compose:
 *
 *  * `CaptureScreen.AutoProcessPanel` — `autoProcessDetail`,
 *    `autoProcessSelfCheck`, `autoProcessMountWarning`
 *  * `ReviewScreen.ProcessSectionsCard` — `reviewProcessDetail`,
 *    `reviewSelfCheck`, `reviewMountWarning`
 *
 * The old ReviewScreen comment even said so out loud: *"Same sentence on the
 * capture card and here, from the same `:core` property, so the two can never
 * drift apart."* The SENTENCES could not drift, because they come from `:core`.
 * Everything around them could and did: two different type scales, two
 * different spacings, and — the reason this matters rather than being tidiness
 * — two places to remember when a fourth line is added. Item 60's loop-end
 * result is exactly such a line, and it would have been added to one of them.
 *
 * The test tags are kept DISTINCT per surface, passed in by the caller, so the
 * existing instrumented tests keep asserting what they assert and a failure
 * still says which screen it was on.
 *
 * Deliberately NOT a `ColumnScope` extension: both callers already own a
 * `Column` and the only layout this does is `Spacer` + `Text`, neither of which
 * needs the receiver. A receiver would have made it un-callable by its fully
 * qualified name, which is how a shared composable ends up imported into one
 * file and re-typed into the other.
 */
@Composable
fun ProcessResultLines(
    stitch: StitchResult?,
    detailTag: String,
    selfCheckTag: String,
    mountWarningTag: String,
) {
    if (stitch == null) return
    stitch.detail?.let { detail ->
        Spacer(Modifier.height(6.dp))
        Text(
            detail,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.testTag(detailTag),
        )
    }
    // ROUND 15 item 57. Shown for a skipped fast-path run too — a one-piece
    // scan is exactly the one whose owner will believe the number.
    stitch.selfCheckLine?.let { line ->
        Spacer(Modifier.height(6.dp))
        Text(
            line,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.testTag(selfCheckTag),
        )
    }
    stitch.mountWarning?.let { warning ->
        Spacer(Modifier.height(6.dp))
        Text(
            warning,
            style = MaterialTheme.typography.bodySmall,
            color = SemBad,
            modifier = Modifier.testTag(mountWarningTag),
        )
    }
}
