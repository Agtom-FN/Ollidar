package com.lidarscan.app.ui.review

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.filled.GridView
import androidx.compose.material.icons.filled.IosShare
import androidx.compose.material.icons.filled.Share
import androidx.compose.ui.draw.clip
import androidx.compose.material3.LinearProgressIndicator
import com.lidarscan.app.ui.theme.SemBad
import androidx.compose.ui.platform.testTag
import com.lidarscan.app.ui.components.BackBar
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.app.ui.theme.ViewportGround
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Straighten
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.PointCloudView
import com.lidarscan.core.measure.MeasureUnit
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap
import com.lidarscan.core.projects.ProjectActionWording
import com.lidarscan.core.render.PointSizeMode
import kotlin.math.roundToInt

@Composable
fun ReviewRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
    /**
     * ROUND 22 item 97: **null hides the floor-plan door.** It is an Advanced
     * feature now, and a null callback hides the pill and makes the route
     * unreachable from here in one statement.
     */
    onOpenPlan: ((String) -> Unit)?,
    /**
     * Redesign: Export is a first-class button here. ROUND 22 item 96: null in
     * Simple mode, where Review carries its OWN export row instead of sending
     * the operator to a Processing screen Simple mode does not show.
     */
    onOpenExport: ((String) -> Unit)? = null,
    /** ROUND 22 item 97 — see [com.lidarscan.core.SimpleMode]. */
    advanced: Boolean = false,
) {
    val vm: ReviewViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ReviewViewModel(container, container.projectStore, container.settingsRepository, projectId)
            }
        },
    )
    val state by vm.uiState.collectAsStateWithLifecycle()
    // ── ROUND 22 item 96: Export on Review, VERBATIM ────────────────────────
    //
    // Simple mode removes the Processing screen from navigation, and the brief
    // is explicit that export must reuse its paths rather than grow a second
    // implementation: PLY / LAS / PCD / Bundle, the same job, the same
    // ROUND 7 Downloads delivery, the same ROUND 18 "wait for the auto-process
    // before zipping" rule. So the export row drives the REAL
    // `ProcessingViewModel` — the screen it used to live on is what changed,
    // not the pipeline. Built unconditionally (a ViewModel with no collector
    // costs one object) so the branch below is purely about what is drawn.
    val exportVm: com.lidarscan.app.ui.processing.ProcessingViewModel = viewModel(
        key = "review-export-$projectId",
        factory = viewModelFactory {
            initializer {
                com.lidarscan.app.ui.processing.ProcessingViewModel(
                    container, container.projectStore, container.settingsRepository, projectId,
                )
            }
        },
    )
    ReviewScreen(
        state = state,
        vm = vm,
        exportVm = exportVm,
        onBack = onBack,
        onOpenPlan = onOpenPlan?.let { open -> { open(projectId) } },
        onOpenExport = onOpenExport?.let { open -> { open(projectId) } },
        advanced = advanced,
    )
}

/**
 * The redesign's Review screen: a hero viewport with the colour chips, the
 * Floor plan / Export split, and the measure UX untouched.
 *
 * **What did not change.** The measure tool is the same: the same transparent
 * tap-catcher over the `SurfaceView` (so a pick never fights the orbit
 * gesture), the same "nearest sampled point" caveat in the read-out, the same
 * unit chips and Clear. B11's behaviour was not in scope for the redesign and
 * is not touched here — only the chrome around it moved.
 *
 * The full §3.9 display panel is still one tap away behind the Tune action;
 * the four colour chips on the body are the shortcut for the one setting
 * people change constantly, not a replacement for the panel.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ReviewScreen(
    state: ReviewUiState,
    vm: ReviewViewModel,
    onBack: () -> Unit,
    exportVm: com.lidarscan.app.ui.processing.ProcessingViewModel? = null,
    onOpenPlan: (() -> Unit)?,
    onOpenExport: (() -> Unit)? = null,
    advanced: Boolean = false,
) {
    // ROUND 22 item 96: the Downloads write needs a Context (ROUND 7's
    // MediaStore delivery). Read once here rather than at the click, so the
    // export path is identical to the Processing screen's.
    val exportContext = androidx.compose.ui.platform.LocalContext.current

    var showPanel by remember { mutableStateOf(false) }
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)
    val manifest = state.project?.manifest

    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        BackBar(
            title = manifest?.name ?: "Review",
            subtitle = listOfNotNull(
                "Review",
                manifest?.pointCountEstimate?.let { "%.1f M pts".format(it / 1_000_000.0) },
                manifest?.crsEpsg?.takeIf { it != 0 }?.let { "georef EPSG $it" } ?: "local frame",
            ).joinToString(" · "),
            onBack = onBack,
            actions = {
                IconButton(onClick = { vm.toggleMeasure() }) {
                    Icon(
                        Icons.Filled.Straighten,
                        contentDescription = "Measure",
                        tint = if (state.measureMode) Ember else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                IconButton(onClick = { showPanel = true }) {
                    Icon(
                        Icons.Filled.Tune,
                        contentDescription = "Display settings",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            },
        )

        // ── hero viewport ───────────────────────────────────────────────
        val shape = RoundedCornerShape(20.dp)
        Box(
            Modifier
                .fillMaxWidth()
                .weight(1f)
                .padding(horizontal = 14.dp)
                .background(ViewportGround, shape)
                .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
                .clip(shape)
                .testTag("reviewViewport"),
        ) {
            if (state.hasCloud) {
                PointCloudView(
                    source = vm.cloudSource,
                    colorMode = state.display.colorMode,
                    colormap = state.display.activeScalar.colormap,
                    pointSizePx = state.display.pointSize.fixedPx,
                    cameraMode = CameraMode.ORBIT,
                    displayParams = state.display,
                    onRendererReady = vm::onRendererReady,
                    // ── ROUND 25 item 117: the measure tap, and what it replaced ──
                    //
                    // This used to be a transparent `Box` with
                    // `detectTapGestures` laid OVER the SurfaceView, on the
                    // reasoning that "a pick never fights the orbit gesture".
                    // It did not fight it; it removed it. A pointer-input node
                    // above the view takes the tap AND the drag AND the pinch,
                    // so with measure mode on the viewer could not be moved at
                    // all — which is the state the owner would have found the
                    // moment he tried the new pan on a scan he was measuring.
                    //
                    // The tap now goes through the SAME gesture arbiter as the
                    // navigation, which is the only arrangement in which "a tap
                    // measures, a drag orbits, two fingers pan, a pinch zooms"
                    // is a set of rules rather than a set of hopes.
                    onTapPick = if (state.measureMode) {
                        { x, y -> vm.onTap(x, y) }
                    } else {
                        null
                    },
                    modifier = Modifier.fillMaxSize(),
                )
                if (state.measureMode) {
                    ScanChip(
                        text = if (state.measurement == null) "MEASURE ON · TAP A POINT" else "MEASURE ON",
                        color = Ember,
                        modifier = Modifier.align(Alignment.TopStart).padding(12.dp),
                    )
                }
                ScanChip(
                    text = "%,d pts".format(state.totalPoints),
                    modifier = Modifier.align(Alignment.BottomStart).padding(12.dp),
                )
            } else {
                // ROUND 8 (owner item 27c). This used to be one paragraph
                // saying "run Post-process first", which was wrong twice over
                // for the sensor the owner actually uses: post-processing
                // REFUSED a D6 project, and the reason it refused was that the
                // trajectory was never recorded. Opening a saved D6 scan now
                // shows the 3D map without being asked, and the only case that
                // still shows text is the one that genuinely cannot be fixed.
                Box(
                    Modifier.fillMaxSize().testTag("reviewLoadState"),
                    contentAlignment = Alignment.Center,
                ) {
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        modifier = Modifier.padding(28.dp),
                    ) {
                        val busy = state.load == ReviewLoad.PROBING ||
                            state.load == ReviewLoad.LOADING_RECORDED ||
                            state.load == ReviewLoad.RESOLVING
                        if (busy) {
                            CircularProgressIndicator(
                                color = ScanTeal,
                                strokeWidth = 2.dp,
                                modifier = Modifier.size(28.dp),
                            )
                            Spacer(Modifier.height(14.dp))
                        }
                        Text(
                            text = when (state.load) {
                                // Named rather than a generic "Loading…": the
                                // three paths have very different latencies
                                // (a file read vs. a full re-resolve) and an
                                // operator who knows which one is running does
                                // not think the app has hung.
                                ReviewLoad.PROBING -> "Opening this scan…"
                                ReviewLoad.LOADING_RECORDED -> "Loading the 3D map recorded with this scan…"
                                ReviewLoad.RESOLVING ->
                                    "Rebuilding the 3D map from this scan's returns and trajectory…"
                                else -> state.loadMessage ?: "No cloud in memory."
                            },
                            style = MaterialTheme.typography.bodyMedium,
                            color = if (state.load == ReviewLoad.NO_TRAJECTORY) {
                                SemWarn
                            } else {
                                MaterialTheme.colorScheme.onSurfaceVariant
                            },
                        )
                    }
                }
            }
        }

        // ── ROUND 13: "Process this scan" ───────────────────────────────
        //
        // Shown only when the capture is actually in pieces. A button that is
        // always there and usually does nothing teaches the operator to ignore
        // it, and this one is the answer to the complaint that opened the
        // round: a 5-section scan was worthless and there was nothing to press.
        if (state.sections > 1) {
            ProcessSectionsCard(state = state, onProcess = vm::processScan)
        }

        // ── colour chips ────────────────────────────────────────────────
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 14.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // ROUND 10 (owner item 39): RGB is paused with the camera. TIME
            // has always fallen back to the RGB pass (the hint below says so)
            // and there is nothing behind either on this rig, so the review
            // strip offers the two modes a D6 cloud actually has.
            buildList {
                add(ColorMode.HEIGHT to "HEIGHT")
                if (com.lidarscan.core.FeatureFlags.RGB_COLOR_MODE_ENABLED) {
                    add(ColorMode.RGB to "RGB")
                    add(ColorMode.TIME to "TIME")
                }
                add(ColorMode.INTENSITY to "INTENSITY")
            }.forEach { (mode, label) ->
                val selected = state.display.colorMode == mode
                ScanChip(
                    text = label,
                    color = if (selected) Ember else null,
                    onClick = { vm.updateDisplay { it.copy(colorMode = mode) } },
                    modifier = Modifier.height(38.dp).padding(top = 6.dp),
                )
            }
        }

        // A14's own rule, surfaced rather than silently applied: PointVertex
        // carries no per-point time, so TIME falls back to the RGB pass.
        if (state.display.colorMode == ColorMode.TIME) {
            Hint(
                "No per-point time in PointVertex — this falls back to RGB.",
                color = SemWarn,
                modifier = Modifier.padding(horizontal = 16.dp),
            )
            Spacer(Modifier.height(6.dp))
        }

        if (state.measureMode) {
            Box(Modifier.padding(horizontal = 14.dp)) { MeasureHud(state, vm) }
            Spacer(Modifier.height(10.dp))
        }

        // ── ROUND 22 items 96 + 97 / ROUND 23 item 104a ─────────────────────
        //
        // The floor plan is behind the Advanced switch, so on an ordinary walk
        // this row is Share + Export — which is the point of item 96: Export
        // used to mean "open the Details hub, find the Processing screen, pick
        // a format there". It is here now, on the screen the operator is
        // already looking at.
        //
        // ROUND 23 item 104a: **and so is Share, in BOTH modes.** Round 22 gave
        // Review a full-width Export and, with it, only the Downloads copy —
        // the hand-off to the system share sheet that the bundle path had
        // always carried was left on the Processing screen that Simple mode
        // removes. From where the owner stands that is export/share
        // "vanishing", and it is fixed by giving the sheet its own button
        // rather than by hiding it inside Export: Share is
        // `ProcessingViewModel.export(share = true)`, i.e. the same job and the
        // same ROUND 7 "Downloads FIRST, sheet second" order, one flag apart.
        //
        // Export is the primary and Share the secondary because only one of the
        // two guarantees the file survives the operator dismissing a chooser.
        // Share is drawn whenever there is a ViewModel behind it, so Advanced
        // mode — where Export navigates to the Processing screen — keeps it too.
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 14.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (onOpenPlan != null) {
                SecondaryPill(
                    text = "Floor plan",
                    icon = Icons.Filled.GridView,
                    onClick = onOpenPlan,
                    modifier = Modifier.weight(1f),
                )
            }
            if (exportVm != null) {
                SecondaryPill(
                    text = ProjectActionWording.SHARE_ACTION,
                    icon = Icons.Filled.Share,
                    onClick = { exportVm.export(exportContext, share = true) },
                    modifier = Modifier.weight(1f).testTag("reviewShareButton"),
                )
            }
            PrimaryPill(
                text = com.lidarscan.core.Wording.EXPORT_ACTION,
                icon = Icons.Filled.IosShare,
                onClick = {
                    if (onOpenExport != null) onOpenExport() else exportVm?.export(exportContext)
                },
                modifier = Modifier.weight(1f).testTag("reviewExportButton"),
            )
        }
        if (onOpenExport == null && exportVm != null) {
            // Item 96: the format row, on Review, reusing ProcessingViewModel's
            // export paths verbatim — same formats, same Downloads delivery.
            ExportFormatRow(exportVm)
        }

        Spacer(Modifier.height(ScanDims.TabBarClearance))
    }

    if (showPanel) {
        ModalBottomSheet(
            onDismissRequest = { showPanel = false },
            sheetState = sheetState,
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            // ROUND 16 item 61: "...and there are some tab and window show the
            // same too". This was the other one.
            shape = RoundedCornerShape(
                topStart = ScanDims.SheetRadius,
                topEnd = ScanDims.SheetRadius,
            ),
        ) {
            DisplayPanel(state, vm, advanced)
        }
    }
}

@Composable
private fun MeasureHud(state: ReviewUiState, vm: ReviewViewModel) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            val m = state.measurement
            if (m != null) {
                Text(vm.formatted(m.distanceM), style = MaterialTheme.typography.headlineSmall)
                Text(
                    "horizontal ${vm.formatted(m.horizontalM)} · Δz ${vm.formatted(m.deltaZM)}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    // ROUND 22 item 98: was 37 words explaining the sampling
                    // strategy. The honesty that matters — the pick can be a
                    // few centimetres out — survives; the explanation of WHY
                    // does not belong on a measuring tool.
                    com.lidarscan.core.Wording.MEASURE_DETAIL,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                Text(
                    state.pickMessage ?: com.lidarscan.core.Wording.MEASURE_HINT,
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                MeasureUnit.entries.forEach { u ->
                    FilterChip(
                        selected = state.measureUnit == u,
                        onClick = { vm.setMeasureUnit(u) },
                        label = { Text(u.abbreviation) },
                    )
                }
                TextButton(onClick = vm::clearMeasurement) { Text("Clear") }
            }
        }
    }
}

/**
 * B10 — §3.9's render-settings panel, bound to A14's model.
 *
 * EDL has a switch and no effect yet, and says so: `points.mat` has no
 * post-process pass and S3 never measured EDL's cost on this class of GPU, so
 * shipping a toggle that silently does nothing would be worse than one that
 * admits it.
 */
@Composable
private fun DisplayPanel(state: ReviewUiState, vm: ReviewViewModel, advanced: Boolean = false) {
    val d = state.display
    Column(
        Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp)
            .padding(bottom = 32.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("Display", style = MaterialTheme.typography.titleLarge)
        Text(
            "Saved with this project (§3.9). Profiles set the starting point.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        // ── ROUND 23 item 106d: the profile chips obey Simple mode ──────────
        //
        // Round 22 hid the Survey and Research profiles and the floor plan; this
        // panel went on enumerating DisplayProfile.entries, so all four chips
        // stayed one tap away and `SimpleMode.showsSurveyProfile(false)`
        // answered "no" with a chip labelled "Survey" on the screen. Applying a
        // profile is how the Survey GNSS capture gate and the Research point
        // budget get switched on, so this was the hidden feature reachable
        // anyway. The filter is `SimpleMode.displayProfiles`, in :core and under
        // test, rather than three inline conditions here.
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.testTag("reviewProfileChips")) {
            com.lidarscan.core.SimpleMode.displayProfiles(advanced).forEach { p ->
                FilterChip(selected = false, onClick = { vm.applyProfile(p) }, label = { Text(p.displayName) })
            }
        }

        HorizontalDivider()
        Text("Colour", style = MaterialTheme.typography.titleSmall)
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            // ROUND 10 (owner item 39): the Display panel enumerated every
            // enum value, so it surfaced RGB and FIX_QUALITY whatever the rest
            // of the app did. It now asks the same availability map the chips
            // above do, which is where the pause is expressed once.
            ColorMode.entries.filter {
                // ROUND 11 (item 42): coverage is a LIVE-view mode. It answers
                // "where have I not been yet", which is a question about a walk
                // in progress; on a sealed scan the answer is "nowhere else,
                // it is finished". Offering it in Review would also be a lie
                // about the container, which carries no density.
                it != ColorMode.COVERAGE &&
                    (it != ColorMode.RGB || com.lidarscan.core.FeatureFlags.RGB_COLOR_MODE_ENABLED)
            }.forEach { mode ->
                val reason = state.colorModeReasons[mode]
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FilterChip(
                        selected = d.colorMode == mode,
                        onClick = { vm.updateDisplay { it.copy(colorMode = mode) } },
                        enabled = reason == null,
                        label = { Text(mode.name.lowercase().replaceFirstChar { it.uppercase() }) },
                    )
                }
                if (reason != null && d.colorMode != mode) {
                    Text(
                        reason,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        if (d.colorMode == ColorMode.HEIGHT || d.colorMode == ColorMode.INTENSITY) {
            Text("Colormap", style = MaterialTheme.typography.titleSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Colormap.entries.forEach { cm ->
                    FilterChip(
                        selected = d.activeScalar.colormap == cm,
                        onClick = {
                            vm.updateDisplay { p ->
                                when (p.colorMode) {
                                    ColorMode.HEIGHT -> p.copy(height = p.height.copy(colormap = cm))
                                    ColorMode.INTENSITY -> p.copy(intensity = p.intensity.copy(colormap = cm))
                                    else -> p
                                }
                            }
                        },
                        label = { Text(cm.name.lowercase().replaceFirstChar { it.uppercase() }) },
                    )
                }
            }
            LabeledSlider(
                "Gamma",
                d.activeScalar.gamma,
                0.1f..4f,
                "%.2f".format(d.activeScalar.gamma),
            ) { v ->
                vm.updateDisplay { p ->
                    when (p.colorMode) {
                        ColorMode.HEIGHT -> p.copy(height = p.height.copy(gamma = v))
                        ColorMode.INTENSITY -> p.copy(intensity = p.intensity.copy(gamma = v))
                        else -> p
                    }
                }
            }
            LabeledSlider(
                "Brightness",
                d.activeScalar.brightness,
                0.1f..3f,
                "%.2f".format(d.activeScalar.brightness),
            ) { v ->
                vm.updateDisplay { p ->
                    when (p.colorMode) {
                        ColorMode.HEIGHT -> p.copy(height = p.height.copy(brightness = v))
                        ColorMode.INTENSITY -> p.copy(intensity = p.intensity.copy(brightness = v))
                        else -> p
                    }
                }
            }
            SwitchRow("Auto range", d.activeScalar.autoRange, "Track the cloud's real range each frame instead of a pinned one.") { on ->
                vm.updateDisplay { p ->
                    when (p.colorMode) {
                        ColorMode.HEIGHT -> p.copy(height = p.height.copy(autoRange = on))
                        ColorMode.INTENSITY -> p.copy(intensity = p.intensity.copy(autoRange = on))
                        else -> p
                    }
                }
            }
        }

        HorizontalDivider()
        Text("Point size", style = MaterialTheme.typography.titleSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            PointSizeMode.entries.forEach { m ->
                FilterChip(
                    selected = d.pointSize.mode == m,
                    onClick = { vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(mode = m)) } },
                    label = {
                        Text(
                            when (m) {
                                PointSizeMode.FIXED_PIXELS -> "Fixed px"
                                PointSizeMode.ADAPTIVE -> "Adaptive"
                                PointSizeMode.WORLD_SIZE -> "World size"
                            },
                        )
                    },
                )
            }
        }
        when (d.pointSize.mode) {
            PointSizeMode.FIXED_PIXELS -> LabeledSlider(
                "Size",
                d.pointSize.fixedPx,
                // ROUND 19 item 76: the same constants the capture sheet reads.
                com.lidarscan.core.render.DisplayLimits.POINT_SIZE_MIN_PX..
                    com.lidarscan.core.render.DisplayLimits.POINT_SIZE_MAX_PX,
                "%.1f px".format(d.pointSize.fixedPx),
            ) { v -> vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(fixedPx = v)) } }
            PointSizeMode.ADAPTIVE -> {
                LabeledSlider("Min", d.pointSize.adaptiveMinPx, 0.5f..12f, "%.1f px".format(d.pointSize.adaptiveMinPx)) { v ->
                    vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(adaptiveMinPx = v)) }
                }
                LabeledSlider("Max", d.pointSize.adaptiveMaxPx, 0.5f..24f, "%.1f px".format(d.pointSize.adaptiveMaxPx)) { v ->
                    vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(adaptiveMaxPx = v)) }
                }
            }
            PointSizeMode.WORLD_SIZE -> LabeledSlider(
                "Diameter",
                d.pointSize.worldSizeM,
                0.0005f..0.2f,
                "%.0f mm".format(d.pointSize.worldSizeM * 1000f),
            ) { v -> vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(worldSizeM = v)) } }
        }

        HorizontalDivider()
        Text("Detail budget", style = MaterialTheme.typography.titleSmall)
        LabeledSlider(
            "LOD",
            (d.lodPointBudget / 1_000_000f),
            com.lidarscan.core.render.DisplayLimits.LOD_MIN_M..
                com.lidarscan.core.render.DisplayLimits.LOD_MAX_M,
            "${(d.lodPointBudget / 1_000_000f).roundToInt()} M points",
        ) { v -> vm.updateDisplay { it.copy(lodPointBudget = (v * 1_000_000f).roundToInt()) } }
        // ROUND 22 item 98: was 37 words about page order and decimation.
        // ROUND 22 item 100: and when the device is what is limiting the
        // slider, say THAT instead — it is the only one of the two the
        // operator can act on (by using a different phone).
        Text(
            com.lidarscan.core.Wording.DETAIL_BUDGET_HINT,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        HorizontalDivider()
        Text("Clipping", style = MaterialTheme.typography.titleSmall)
        SwitchRow("Height clip", d.clipHeightEnabled, "Hide everything outside a height band — the same band a floor plan slices.") { on ->
            vm.updateDisplay { it.copy(clipHeightEnabled = on) }
        }
        if (d.clipHeightEnabled) {
            LabeledSlider("Floor", d.clipHeightMin, -5f..10f, "%.2f m".format(d.clipHeightMin)) { v ->
                vm.updateDisplay { it.copy(clipHeightMin = v) }
            }
            LabeledSlider("Ceiling", d.clipHeightMax, -5f..10f, "%.2f m".format(d.clipHeightMax)) { v ->
                vm.updateDisplay { it.copy(clipHeightMax = v) }
            }
        }
        SwitchRow("Box clip", d.clipBoxEnabled, "Restrict to an axis-aligned box in the session's local frame.") { on ->
            vm.updateDisplay { it.copy(clipBoxEnabled = on) }
        }

        HorizontalDivider()
        Text("Background", style = MaterialTheme.typography.titleSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BACKGROUNDS.forEach { (label, rgba) ->
                OutlinedButton(onClick = { vm.updateDisplay { it.copy(background = rgba) } }) {
                    Box(
                        Modifier
                            .height(14.dp)
                            .width(14.dp)
                            .background(Color(rgba.r / 255f, rgba.g / 255f, rgba.b / 255f, 1f)),
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(label)
                }
            }
        }

        HorizontalDivider()
        // ROUND 22 item 98: the EDL row is REMOVED.
        //
        // Its own detail text said it: "Persisted, but NOT rendered in this
        // build — points.mat has no post-process pass". A switch that renders
        // nothing on the only device this app runs on is worse than an absent
        // one: it costs the operator a decision and returns nothing for it. The
        // FIELD survives on `DisplayParams` (it travels with the project, so a
        // desktop that can draw EDL still gets the intent) and the day a
        // post-process pass exists the row comes straight back.
        SwitchRow(
            com.lidarscan.core.Wording.SHOW_MY_PATH,
            d.showTrajectory,
            // Was: "Draws your walk through the cloud — start teal, end amber,
            // red where tracking was lost." The colour legend is on screen.
            "Start green, end amber, red where tracking was lost.",
        ) { on ->
            vm.updateDisplay { it.copy(showTrajectory = on) }
        }
        // ROUND 17 item 65: and when there is no path, say so. A missing
        // trajectory used to remove the entity from the scene in silence,
        // which is indistinguishable from a broken renderer.
        val trajectoryNote by vm.trajectoryNote.collectAsStateWithLifecycle()
        trajectoryNote?.let { note ->
            Text(
                note,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.outline,
                modifier = Modifier
                    .padding(horizontal = 4.dp, vertical = 6.dp)
                    .testTag("trajectoryNote"),
            )
        }

        TextButton(onClick = vm::resetToProfileDefault) { Text("Reset to this project's profile defaults") }
    }
}

private val BACKGROUNDS = listOf(
    "Dark" to com.lidarscan.core.render.Rgba(18, 18, 22, 255),
    "Black" to com.lidarscan.core.render.Rgba(0, 0, 0, 255),
    "Grey" to com.lidarscan.core.render.Rgba(96, 96, 100, 255),
    "White" to com.lidarscan.core.render.Rgba(245, 245, 248, 255),
)

@Composable
private fun LabeledSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    readout: String,
    onChange: (Float) -> Unit,
) {
    Column {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            Text(readout, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Slider(value = value.coerceIn(range), valueRange = range, onValueChange = onChange)
    }
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, detail: String, onChange: (Boolean) -> Unit) {
    Column {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            Switch(checked = checked, onCheckedChange = onChange)
        }
        Text(detail, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}


// ── ROUND 13: the Process card ──────────────────────────────────────────────
//
// The numbers it shows are the ones that mean something to a person holding a
// phone: how many pieces, and the HEIGHT SPREAD before and after. The height
// spread is the headline because it is the only number in the report that does
// not come from the same measurement that produced the correction — the
// operator walks on a flat floor, so it has to shrink, and on the owner's
// scan-030 it goes 0.82 m -> 0.27 m.
//
// The start-to-end gap is stated but never sold as an improvement: before
// stitching it compares two points in different world frames and means
// nothing, and after, it is ARCore's own drift, which this does not fix.
@Composable
private fun ProcessSectionsCard(
    state: ReviewUiState,
    onProcess: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 14.dp)
            .padding(top = 12.dp)
            .background(SemWarn.copy(alpha = 0.10f), RoundedCornerShape(12.dp))
            .padding(14.dp)
            .testTag("reviewProcessCard"),
    ) {
        val stitch = state.stitch
        Text(
            when {
                stitch != null -> stitch.headline
                state.isStitched ->
                    "${state.sections} pieces, already aligned — you are looking at the " +
                        "corrected map."
                else ->
                    "This scan is in ${state.sections} pieces. The camera re-anchored " +
                        "${state.sections - 1} time" + (if (state.sections == 2) "" else "s") +
                        " while you walked, so the room is about a metre apart at each join. " +
                        "Processing puts it back into one frame."
            },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
        // ROUND 16 item 61: shared with the capture card. See
        // ui/components/ProcessResultLines.kt.
        com.lidarscan.app.ui.components.ProcessResultLines(
            stitch = stitch,
            detailTag = "reviewProcessDetail",
            selfCheckTag = "reviewSelfCheck",
            mountWarningTag = "reviewMountWarning",
        )
        if (state.processError != null) {
            Spacer(Modifier.height(6.dp))
            Text(
                state.processError,
                style = MaterialTheme.typography.bodySmall,
                color = SemBad,
            )
        }
        Spacer(Modifier.height(10.dp))
        if (state.processing) {
            // Determinate, because the pipeline reports a real fraction. A
            // spinner on a job that can take forty seconds is how an operator
            // decides the app has hung.
            LinearProgressIndicator(
                progress = { state.processProgress },
                modifier = Modifier
                    .fillMaxWidth()
                    .testTag("reviewProcessProgress"),
                color = ScanTeal,
            )
            Spacer(Modifier.height(6.dp))
            Text(
                "Aligning the pieces… ${(state.processProgress * 100).toInt()}%. " +
                    "This takes a few tens of seconds and only reads the scan's raw files.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            OutlinedButton(
                onClick = onProcess,
                modifier = Modifier.testTag("reviewProcessButton"),
            ) {
                Text(if (state.isStitched) "Process again" else "Process this scan")
            }
        }
    }
}


/**
 * ROUND 22 item 96 — **the export row, on the screen the operator is already
 * looking at.**
 *
 * Simple mode removes the "Details, jobs & export" hub and the Processing
 * screen from navigation, so exporting used to be three navigations away from
 * the scan being exported. This is the same four formats
 * ([com.lidarscan.core.model.ExportFormat]) driven through the same
 * [com.lidarscan.app.ui.processing.ProcessingViewModel] — `setExportFormat`
 * and `export` are the Processing screen's own calls, unchanged. Nothing about
 * the pipeline, the job, the gates or the ROUND 7 Downloads delivery is
 * reimplemented here; only where the buttons live moved.
 *
 * The format note the Processing screen shows (`exportFormatNote` — e.g. LAS
 * wanting a georeference) is carried through, because it is the one thing that
 * can make an export produce a file the operator did not expect.
 */
@Composable
private fun ExportFormatRow(vm: com.lidarscan.app.ui.processing.ProcessingViewModel) {
    val state by vm.uiState.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 8.dp)) {
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier.testTag("reviewExportFormats"),
        ) {
            com.lidarscan.core.model.ExportFormat.entries.forEach { format ->
                FilterChip(
                    selected = state.exportFormat == format,
                    onClick = { vm.setExportFormat(format) },
                    label = { Text(format.name.substringBefore('_')) },
                )
            }
        }
        Text(
            com.lidarscan.core.Wording.EXPORT_DETAIL,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 6.dp),
        )
        state.exportNote?.let { note ->
            Text(
                note,
                style = MaterialTheme.typography.labelSmall,
                color = SemWarn,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
        state.message?.let { message ->
            Text(
                message,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp).testTag("reviewExportMessage"),
            )
        }
    }
}
