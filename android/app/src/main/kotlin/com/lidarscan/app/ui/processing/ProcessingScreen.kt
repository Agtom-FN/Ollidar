package com.lidarscan.app.ui.processing

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.outlined.Work
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.HeroHeader
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.ScanEmptyState
import com.lidarscan.app.ui.components.ScanIconButton
import com.lidarscan.app.ui.components.ScanRow
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.StatusDot
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.app.ui.theme.ScanMeta
import com.lidarscan.core.jobs.JobState
import com.lidarscan.core.jobs.ProcessingJob

@Composable
fun ProcessingRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val vm: ProcessingViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ProcessingViewModel(container, container.projectStore, container.settingsRepository, projectId)
            }
        },
    )
    val state by vm.uiState.collectAsStateWithLifecycle()
    ProcessingScreen(state, vm, onBack, onOpenSettings)
}

/**
 * ROUND 28 item 163 (design review §D.6) — **Jobs is a queue, and only a queue.**
 *
 * The review's verdict on the old screen was that it "looks like a different
 * app", and the mechanism was precise: it was the only screen in Ollidar built
 * as a **document** — hero, prose, cards with rounded-square icon tiles, four
 * explainer paragraphs — rather than as an **instrument**. Underneath that it
 * was the wrong screen entirely: a *task launcher* wearing a *status* tab's
 * name, offering `Post-process`, `Colorize`, `Export`, `Save to Downloads` and
 * `Upload and process` — every one of which Review or the Projects `⋯` menu
 * already offers, at the moment the operator is actually looking at the scan.
 *
 * So the launcher is gone and the status is what is left. **One row type, three
 * states, no cards, no chips, no segmented controls, no paragraphs.**
 *
 * What left this file and where it went:
 *
 *  * `This phone / Cloud / Bundle` + its 38-word explainer → Review's Export
 *    sheet, as a **Destination** row (`ProcessingMode.summary` is now written to
 *    a row's ≤12-word budget for it).
 *  * `Post-process` card + its 24-word paragraph → Review's `⋯` menu, as
 *    **Process again**; its refused state is `ProcessingPolicy.postProcess`'s
 *    new five-word reason, in `inkMute` (see [ProcessingViewModel] and item 163
 *    in `ProcessingJob.kt` for why that sentence is no longer red).
 *  * `Export` card + format segment + `Run` → Review's Export sheet, which
 *    already had the full-width Export button the `Run` pill was a small
 *    left-aligned imitation of (J10).
 *  * The footer *"Jobs continue in the background…"* → deleted. It described the
 *    tab's own existence to someone standing in the tab.
 *  * The floating snackbar (J3/J4/J5) → **the rows**. A success that floats over
 *    the list describing the same success is one message too many, and this one
 *    was a filesystem path with a hash in it, drawn at 1.05:1. §C.6: the success
 *    *is* the row. See [ProcessingViewModel.outcomes].
 *
 * `onBack` and `onOpenSettings` survive because `LidarScanApp` owns the call
 * site; a read-only queue has no use for either.
 */
@Composable
fun ProcessingScreen(
    state: ProcessingUiState,
    vm: ProcessingViewModel,
    onBack: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val context = LocalContext.current
    // Newest first: a queue is read top-down and the thing that just happened is
    // what the operator came to check.
    val jobs = state.jobs.asReversed()
    val scanName = state.project?.manifest?.name.orEmpty()

    Column(
        Modifier
            .fillMaxSize()
            .background(ScanColors.page)
            .statusBarsPadding()
            .navigationBarsPadding()
            .testTag("jobsQueue"),
    ) {
        HeroHeader(title = "Jobs", subtitle = queueSummary(jobs))

        if (jobs.isEmpty()) {
            ScanEmptyState(
                icon = Icons.Outlined.Work,
                title = "No jobs yet",
                body = "Exports and reprocessing appear here.",
            )
            return@Column
        }

        LazyColumn(
            modifier = Modifier.fillMaxWidth(),
            contentPadding = PaddingValues(bottom = ScanDims.TabBarClearance),
        ) {
            items(jobs, key = { it.id }) { job ->
                HorizontalDivider(thickness = ScanDims.Hair, color = ScanColors.line)
                JobRow(
                    job = job,
                    scanName = scanName,
                    outcome = state.outcomes[job.id],
                    onCancel = { vm.cancelJob(job.id) },
                    onRetry = { vm.retry(context, job) },
                )
            }
        }
    }
}

/**
 * The one Meta line under the title. Counts, in the operator's words, and
 * nothing else — the old subtitle was the project name, which the operator had
 * just tapped and therefore already knew.
 *
 * `failed` appears only when it is non-zero: a steady state of "2 done" should
 * not carry a permanent "0 failed" for the one day in fifty it matters.
 */
private fun queueSummary(jobs: List<ProcessingJob>): String? {
    if (jobs.isEmpty()) return null
    val running = jobs.count { it.state.isActive }
    val done = jobs.count { it.state == JobState.DONE }
    val failed = jobs.count { it.state == JobState.FAILED && !it.wasCancelled }
    return buildList {
        if (running > 0) add("$running running")
        if (done > 0) add("$done done")
        if (failed > 0) add("$failed failed")
    }.joinToString(" · ").ifEmpty { "${jobs.size} queued" }
}

/**
 * THE ROW, in its three states — running, done, failed.
 *
 * ```
 * ● Export · Scan-085                          ✕     running: 72 dp, 4 dp bar + %
 * ● Export · Scan-085                          ✓     done:    Done · 813 KB · Downloads
 * ● Export · Scan-079                    [Retry]     failed:  Failed · Storage full
 * ```
 *
 * The dot carries the state, which is why J8's `JobIconTile` — a 46 dp
 * rounded-square used nowhere else in the app, a sixth icon shape for the sake
 * of one screen — is deleted rather than restyled. Five per-kind icons were
 * decorating a fact the row already stated in words.
 *
 * The running state is hand-built rather than a [ScanRow] call because
 * `ScanRow`'s `detail` is a `String`, and this row's second line is a 4 dp
 * progress bar (§C.6: "never a bare spinner where a percentage exists"). The
 * geometry below is `ScanRow`'s, token for token, so the three states are one
 * row and not two-plus-one.
 */
@Composable
private fun JobRow(
    job: ProcessingJob,
    scanName: String,
    outcome: String?,
    onCancel: () -> Unit,
    onRetry: () -> Unit,
) {
    val title = if (scanName.isBlank()) job.kind.displayName else "${job.kind.displayName} · $scanName"
    val failed = job.state == JobState.FAILED && !job.wasCancelled

    if (job.state.isActive) {
        Row(
            Modifier
                .fillMaxWidth()
                .height(ScanDims.RowWithThumb)
                .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S2),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
        ) {
            StatusDot(ScanColors.primary)
            Column(Modifier.weight(1f)) {
                Text(title, style = ScanBody, color = ScanColors.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Spacer(Modifier.height(ScanDims.S1))
                JobProgress(job.progress)
            }
            Text("${(job.progress * 100).toInt()}%", style = ScanMeta, color = ScanColors.inkMute, maxLines = 1)
            ScanIconButton(
                icon = Icons.Filled.Close,
                contentDescription = "Cancel this job",
                onClick = onCancel,
            )
        }
        return
    }

    ScanRow(
        title = title,
        detail = outcome ?: terminalDetail(job),
        minHeight = ScanDims.Row,
        leading = { StatusDot(terminalDot(job)) },
        trailing = {
            when {
                // §C.6's error state: a Secondary button performing the fix, at
                // the point of failure. Not a chevron into a screen that would
                // then ask the same question.
                failed -> SecondaryPill(text = "Retry", onClick = onRetry)
                job.state == JobState.DONE -> Icon(
                    Icons.Filled.Check,
                    contentDescription = null,
                    modifier = Modifier.size(24.dp),
                    tint = ScanColors.good,
                )
                else -> Unit
            }
        },
    )
}

/** The queue's only moving part: §C.6's determinate 4 dp inline bar. */
@Composable
private fun JobProgress(progress: Float) {
    LinearProgressIndicator(
        progress = { progress.coerceIn(0f, 1f) },
        modifier = Modifier.fillMaxWidth().height(ScanDims.S1),
        color = ScanColors.primary,
        trackColor = ScanColors.trough,
        strokeCap = StrokeCap.Round,
        gapSize = 0.dp,
        drawStopIndicator = {},
    )
}

/**
 * The ≤6-word cause under a finished row, when the ViewModel has nothing better.
 *
 * `Done · 813 KB · Downloads` comes from [ProcessingUiState.outcomes] and is
 * preferred; this is the fallback for a job that finished in a previous app
 * session, or one the engine failed on its own. **No path and no hash** — the
 * snackbar's `…-e035f2-20260821-212507.ply` is exactly what §D.6 deleted.
 */
private fun terminalDetail(job: ProcessingJob): String = when {
    job.wasCancelled -> "Cancelled"
    job.state == JobState.FAILED -> "Failed · ${job.statusText}"
    else -> "Done"
}

@Composable
private fun terminalDot(job: ProcessingJob): Color = when {
    job.wasCancelled -> ScanColors.inkFaint
    job.state == JobState.FAILED -> ScanColors.bad
    job.state == JobState.DONE -> ScanColors.good
    else -> ScanColors.inkFaint
}
