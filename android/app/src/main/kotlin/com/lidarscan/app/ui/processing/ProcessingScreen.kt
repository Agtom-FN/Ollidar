package com.lidarscan.app.ui.processing

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloudUpload
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Inventory2
import androidx.compose.material.icons.filled.Palette
import androidx.compose.material.icons.filled.ViewInAr
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.HeroHeader
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanCard
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.SegmentedPill
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.app.ui.theme.SemBad
import com.lidarscan.app.ui.theme.SemGood
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.core.jobs.ActionGate
import com.lidarscan.core.jobs.JobKind
import com.lidarscan.core.jobs.JobState
import com.lidarscan.core.jobs.ProcessingJob
import com.lidarscan.core.jobs.ProcessingMode
import com.lidarscan.core.model.ExportFormat

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
 * The redesign's Jobs tab: a **"Processing"** hero, the This-phone / Cloud /
 * Bundle segmented pill, and icon-tile job cards with ember progress.
 *
 * Everything B6 could do here it can still do — the same three modes, the same
 * gates with the same refusal reasons, the same queue with cancel — but the
 * mode chooser stopped being three `FilterChip`s in a row and the queue
 * stopped being a stack of text. A job now reads as a tile: what kind it is
 * (icon), what it is working on (mono sub-line), and how far along (a state
 * chip, plus an ember bar while it is actually running).
 */
@Composable
fun ProcessingScreen(
    state: ProcessingUiState,
    vm: ProcessingViewModel,
    onBack: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val snackbar = remember { SnackbarHostState() }
    val context = LocalContext.current

    LaunchedEffect(state.message) {
        state.message?.let {
            snackbar.showSnackbar(it)
            vm.dismissMessage()
        }
    }

    Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        Column(
            Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .navigationBarsPadding()
                .verticalScroll(rememberScrollState()),
        ) {
            HeroHeader(
                title = "Processing",
                subtitle = state.project?.manifest?.name ?: "no project",
            )

            Column(Modifier.padding(horizontal = 16.dp)) {
                SegmentedPill(
                    options = listOf(
                        ProcessingMode.LOCAL to "This phone",
                        ProcessingMode.CLOUD to "Cloud",
                        ProcessingMode.EXTRACT_FOR_TRANSFER to "Bundle",
                    ),
                    selected = state.mode,
                    onSelect = vm::setMode,
                    height = 46.dp,
                    modifier = Modifier.testTag("processingModeRow"),
                )
                Spacer(Modifier.height(11.dp))
                Hint(state.mode.summary, color = InkFaint)
                Spacer(Modifier.height(14.dp))

                if (!state.engineAvailable) {
                    ScanCard {
                        Text(
                            "The native engine is not loaded in this build, so nothing can be processed. " +
                                "Check Settings → Engine (developer): the simulated engine has no processing " +
                                "pipeline behind it.",
                            style = MaterialTheme.typography.bodySmall,
                            color = SemWarn,
                        )
                    }
                    Spacer(Modifier.height(14.dp))
                }

                when (state.mode) {
                    ProcessingMode.LOCAL -> LocalActions(state, vm)
                    ProcessingMode.CLOUD -> CloudActions(state, vm, onOpenSettings)
                    ProcessingMode.EXTRACT_FOR_TRANSFER -> TransferActions(state) { share ->
                        vm.transferBundle(context, share)
                    }
                }

                Spacer(Modifier.height(20.dp))
                SectionTitle("Queue")
                Spacer(Modifier.height(8.dp))

                if (state.jobs.isEmpty()) {
                    Hint(
                        "Nothing queued. The queue runs one job at a time — that is A15's design, not a limit " +
                            "of this screen.",
                        color = InkFaint,
                    )
                } else {
                    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        state.jobs.reversed().forEach { job ->
                            JobTile(job) { vm.cancelJob(job.id) }
                        }
                    }
                }

                Spacer(Modifier.height(14.dp))
                Hint(
                    "Jobs continue in the background. Cloud needs the server configured in Settings.",
                    color = InkFaint,
                )
                Spacer(Modifier.height(ScanDims.TabBarClearance))
            }
        }

        SnackbarHost(
            snackbar,
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = ScanDims.TabBarClearance),
        ) { data -> Snackbar(snackbarData = data, containerColor = MaterialTheme.colorScheme.surfaceContainerHigh) }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        fontFamily = DisplayFontFamily,
        fontWeight = FontWeight.SemiBold,
        fontSize = 17.sp,
        color = MaterialTheme.colorScheme.onSurface,
    )
}

// ── the three modes' actions ────────────────────────────────────────────────

@Composable
private fun LocalActions(state: ProcessingUiState, vm: ProcessingViewModel) {
    val context = androidx.compose.ui.platform.LocalContext.current
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        GatedAction(
            icon = Icons.Filled.ViewInAr,
            title = "Post-process",
            detail = "Full-density LIO re-run, loop closure and pose-graph optimization from the recorded " +
                "streams. This is the expensive one — expect minutes, and the phone to get warm.",
            gate = state.postProcessGate,
            onRun = vm::postProcess,
        )
        // ROUND 10 (owner item 39): "pause, disable and hide the colorize
        // function and features". The job, its gate, its native binding and
        // `ProcessingViewModel.colorize()` all stay — only the door is closed.
        // `FeatureFlags.COLORIZE_ENABLED` reopens it.
        if (com.lidarscan.core.FeatureFlags.COLORIZE_ENABLED) {
            GatedAction(
                icon = Icons.Filled.Palette,
                title = "Colorize",
                detail = "Samples camera keyframes onto the processed cloud. " +
                    "${state.keyframeCount} keyframe(s) recorded · clock sync: ${state.syncQuality.label}.",
                gate = state.colorizeGate,
                onRun = vm::colorize,
            )
        }
        ScanCard {
            TileHead(Icons.Filled.Download, "Export")
            Spacer(Modifier.height(10.dp))
            SegmentedPill(
                options = ExportFormat.pointCloudFormats.map { it to it.displayName },
                selected = state.exportFormat,
                onSelect = vm::setExportFormat,
            )
            Spacer(Modifier.height(8.dp))
            Hint(state.exportFormat.description, color = InkFaint)
            state.exportNote?.let {
                Spacer(Modifier.height(6.dp))
                Hint(it, color = SemWarn)
            }
            Spacer(Modifier.height(10.dp))
            GateFooter(state.exportGate) { vm.export(context) }
        }
    }
}

@Composable
private fun CloudActions(state: ProcessingUiState, vm: ProcessingViewModel, onOpenSettings: () -> Unit) {
    val c = state.cloud
    ScanCard {
        TileHead(Icons.Filled.CloudUpload, "Cloud submit")
        Spacer(Modifier.height(8.dp))
        Hint(
            "The capture is packaged as a .lscan.zip (the same bundle Extract-for-transfer produces, minus " +
                "the processed results), uploaded in resumable chunks, and the worker's results land in this " +
                "project's processed/ directory.",
            color = InkFaint,
        )
        if (c.running || c.jobId != null) {
            Spacer(Modifier.height(12.dp))
            Text(c.phase, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurface)
            Spacer(Modifier.height(6.dp))
            EmberProgress(
                if (c.uploadFraction < 1f) c.uploadFraction * 0.5f else 0.5f + c.serverProgress * 0.5f,
            )
            c.jobId?.let {
                Spacer(Modifier.height(6.dp))
                Text("Server job $it · ${c.serverState ?: "…"}", style = MonoMeta, color = InkFaint)
            }
        }
        c.error?.let {
            Spacer(Modifier.height(8.dp))
            Hint(it, color = SemBad)
        }
        c.resultFile?.let {
            Spacer(Modifier.height(6.dp))
            Text("Result: ${it.name}", style = MonoMeta, color = InkFaint)
        }
        Spacer(Modifier.height(12.dp))
        if (!state.cloudGate.enabled) {
            Hint(state.cloudGate.reason.orEmpty(), color = SemBad)
            Spacer(Modifier.height(8.dp))
            SecondaryPill(text = "Open Settings", height = 46.dp, onClick = onOpenSettings)
        } else {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PrimaryPill(
                    text = "Upload and process",
                    height = 46.dp,
                    enabled = !c.running,
                    onClick = vm::cloudSubmit,
                )
                if (c.running) SecondaryPill(text = "Cancel", height = 46.dp, onClick = vm::cancelCloud)
            }
        }
    }
}

@Composable
private fun TransferActions(state: ProcessingUiState, onRun: (Boolean) -> Unit) {
    ScanCard {
        TileHead(Icons.Filled.Inventory2, "Save this scan to the phone")
        Spacer(Modifier.height(8.dp))
        Hint(
            "Packages the whole project — manifest, raw streams, camera frames and any processed results — as " +
                "one .lscan.zip and saves it to Downloads/LidarScan. No server, no account, nothing to " +
                "configure. That zip is what the desktop app imports.",
            color = InkFaint,
        )
        Spacer(Modifier.height(12.dp))
        if (!state.transferGate.enabled) {
            Hint(state.transferGate.reason.orEmpty(), color = SemBad)
        } else {
            // ROUND 7: saving is the primary action and the share sheet is the
            // extra, not the other way round. A share sheet is a hand-off with
            // no result callback, and making it the only route is how the
            // owner's export "went nowhere".
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PrimaryPill(
                    text = "Save to Downloads",
                    height = 46.dp,
                    onClick = { onRun(false) },
                    modifier = Modifier.testTag("saveBundleButton"),
                )
                SecondaryPill(text = "Save + share…", height = 46.dp, onClick = { onRun(true) })
            }
        }
    }
}

@Composable
private fun GatedAction(
    icon: ImageVector,
    title: String,
    detail: String,
    gate: ActionGate,
    onRun: () -> Unit,
) {
    ScanCard {
        TileHead(icon, title)
        Spacer(Modifier.height(8.dp))
        Hint(detail, color = InkFaint)
        Spacer(Modifier.height(10.dp))
        GateFooter(gate, onRun)
    }
}

/**
 * A refused action prints its reason instead of a button. That is B6's own
 * rule, kept verbatim: the reason *is* the affordance, and an always-present
 * disabled button with the reason hidden in a tooltip would be worse.
 */
@Composable
private fun GateFooter(gate: ActionGate, onRun: () -> Unit) {
    if (gate.enabled) {
        PrimaryPill(text = "Run", height = 46.dp, onClick = onRun)
    } else {
        Hint(gate.reason.orEmpty(), color = SemBad)
    }
}

/** The icon tile + title that heads every action card and every job row. */
@Composable
private fun TileHead(icon: ImageVector, title: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        JobIconTile(icon)
        Spacer(Modifier.width(12.dp))
        Text(
            title,
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 17.sp,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

@Composable
private fun JobIconTile(icon: ImageVector) {
    Box(
        Modifier
            .size(46.dp)
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, RoundedCornerShape(ScanDims.TileRadius))
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(ScanDims.TileRadius)),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

// ── the queue ───────────────────────────────────────────────────────────────

@Composable
private fun JobTile(job: ProcessingJob, onCancel: () -> Unit) {
    ScanCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            JobIconTile(jobIcon(job.kind))
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    job.kind.displayName,
                    fontFamily = DisplayFontFamily,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 16.sp,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    "#${job.id} · ${job.statusText}",
                    style = MonoMeta,
                    color = if (job.state == JobState.FAILED && !job.wasCancelled) SemBad else InkFaint,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.width(8.dp))
            val (label, color) = jobStateChip(job)
            ScanChip(text = label, color = color, showDot = true)
        }
        if (job.state.isActive) {
            Spacer(Modifier.height(10.dp))
            EmberProgress(job.progress)
            Spacer(Modifier.height(4.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(onClick = onCancel) { Text("Cancel", color = MaterialTheme.colorScheme.onSurfaceVariant) }
            }
        }
    }
}

/** The ember progress bar — the one place a job's motion is visible. */
@Composable
private fun EmberProgress(progress: Float) {
    LinearProgressIndicator(
        progress = { progress.coerceIn(0f, 1f) },
        modifier = Modifier.fillMaxWidth().height(6.dp),
        color = Ember,
        trackColor = MaterialTheme.colorScheme.surfaceContainerHigh,
        strokeCap = androidx.compose.ui.graphics.StrokeCap.Round,
        gapSize = 0.dp,
        drawStopIndicator = {},
    )
}

private fun jobIcon(kind: JobKind): ImageVector = when (kind) {
    JobKind.POST_PROCESS -> Icons.Filled.ViewInAr
    JobKind.COLORIZE -> Icons.Filled.Palette
    JobKind.EXPORT_POINTS -> Icons.Filled.Download
    JobKind.TRANSFER_EXPORT -> Icons.Filled.Inventory2
    JobKind.CLOUD_SUBMIT -> Icons.Filled.CloudUpload
}

private fun jobStateChip(job: ProcessingJob): Pair<String, Color?> = when {
    job.wasCancelled -> "CANCELLED" to null
    job.state == JobState.FAILED -> "FAILED" to SemBad
    job.state == JobState.DONE -> "DONE" to SemGood
    job.state == JobState.RUNNING -> "${(job.progress * 100).toInt()}%" to SemWarn
    job.state == JobState.CANCELLING -> "CANCELLING" to SemWarn
    else -> "QUEUED" to null
}
