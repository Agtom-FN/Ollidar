package com.lidarscan.app.ui.review

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.IosShare
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.RadioButtonChecked
import androidx.compose.material.icons.filled.RadioButtonUnchecked
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.PointCloudView
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanCard
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.ScanEmptyState
import com.lidarscan.app.ui.components.ScanIconButton
import com.lidarscan.app.ui.components.ScanIcons
import com.lidarscan.app.ui.components.ScanRow
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.SectionLabel
import com.lidarscan.app.ui.components.SegmentedPill
import com.lidarscan.app.ui.components.SheetSlider
import com.lidarscan.app.ui.theme.HeightRamp
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.app.ui.theme.ScanMeta
import com.lidarscan.app.ui.theme.ScanMetaCaps
import com.lidarscan.app.ui.theme.ScanTitle
import com.lidarscan.core.measure.MeasureUnit
import com.lidarscan.core.model.ExportFormat
import com.lidarscan.core.projects.ProjectActionWording
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap
import com.lidarscan.core.render.PointCountFormat
import com.lidarscan.core.render.PointSizeMode
import com.lidarscan.core.render.ViewerChrome
import kotlin.math.roundToInt

@Composable
fun ReviewRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
    /**
     * ROUND 22 item 97: **null hides the floor-plan door.** It is an Advanced
     * feature now, and a null callback hides the control and makes the route
     * unreachable from here in one statement.
     */
    onOpenPlan: ((String) -> Unit)?,
    /**
     * ROUND 28 item 161: this used to be the Export button's destination in
     * Advanced mode. Export is the **sheet** now, on every path — §D.4 —
     * because a screen whose primary action means two different things
     * depending on a switch is a screen the operator cannot learn. The callback
     * survives as the `⋯` menu's door to this scan's job queue, which is what
     * the Processing route actually is once §D.6 empties it of launchers.
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
    // before zipping" rule. So the export sheet drives the REAL
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
 * ROUND 28 item 161 — **Review, rebuilt: the cloud gets the screen.**
 *
 * §D.4, and the counts are the specification: **0 chips, 2 buttons, 2 icon
 * buttons**, against the 7 chips in 3 treatments, 2 pills, 1 caption and 5
 * stacked rows this screen shipped with in v0.9.12. What was removed and where
 * it went:
 *
 * | Was | Now |
 * |---|---|
 * | floating `52,041 pts` chip | deleted — it duplicated the header and contradicted it (R2/R6) |
 * | `HEIGHT` / `INTENSITY` chip row | the display sheet's `Colour` row (R10) |
 * | `PLY/LAS14/PCD/DXF/PDF` chips | the **Export sheet**, chosen once (R5) |
 * | "Saves to your Downloads folder." | the Export sheet's confirm button (R5) |
 * | ~300 px of dead ground | the cloud, via `weight(1f)` (R7) |
 *
 * ## The layout is a column again, and that is the fix for R8
 *
 * Round 26 item 126 made this screen a full-bleed viewport with floating
 * overlays. It bought the cloud a lot of pixels and it cost the app its theme:
 * `ScanColors.viewport` is dark in **both** themes (a point cloud is drawn in
 * light and needs a dark room), so a viewport that reaches every edge makes
 * Review a dark screen — and the tab bar, which the app draws over the route
 * and paints from the *live* theme, stayed white. Two themes on one screen,
 * which is R8.
 *
 * So the viewport is bounded again: header above, action row below, and the
 * bottom `TabBarClearance` painted `ScanColors.page`, which is the ground the
 * tab bar actually sits on. The cloud does not lose the space — it takes the
 * slack with `weight(1f)` and lands at ~72 % against the old ~44 %. Round 26's
 * *other* half survives intact: a tap on empty cloud still takes the chrome
 * away, and with the header and the action row gone the column has nothing but
 * the viewport in it, so the full-bleed frame is one tap away rather than
 * permanent.
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

    var showDisplaySheet by remember { mutableStateOf(false) }
    var showExportSheet by remember { mutableStateOf(false) }
    val displaySheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)
    val exportSheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val manifest = state.project?.manifest

    // ── ROUND 26 item 126: the controls hide on a tap ──────────────────────
    //
    // `rememberSaveable`, so a rotation does not un-hide them: item 125 makes
    // Review a two-orientation screen, and an operator who hid the chrome to
    // look at a corridor and then turned the phone sideways to see more of it
    // would otherwise get the toolbar back in the middle of the gesture.
    //
    // The arbitration itself is [ViewerChrome] in `:core` — a truth table with
    // a test, not three `if`s in a composable.
    var controlsShown by rememberSaveable { mutableStateOf(true) }
    val controlsVisible = ViewerChrome.controlsVisible(controlsShown, state.measureMode)

    Box(
        Modifier
            .fillMaxSize()
            // ROUND 28 item 161 (R8): the screen's ground is the PAGE, not the
            // viewport. This is the pixel the tab bar is drawn on top of, and
            // painting it from the live theme is what stops Review being a dark
            // screen with light chrome bolted to the bottom of it.
            .background(ScanColors.page)
            .testTag("reviewScreen"),
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .navigationBarsPadding()
                // The tab bar floats over every route (`LidarScanApp`), so the
                // column owes it clearance. Round 26 reserved the same value
                // inside a floating strip; it is owed by the LAYOUT now, which
                // is why the cloud can end above it instead of behind it.
                .padding(bottom = ScanDims.TabBarClearance),
        ) {
            AnimatedVisibility(visible = controlsVisible, enter = fadeIn(), exit = fadeOut()) {
                ReviewHeader(
                    title = manifest?.name ?: "Review",
                    // ── ROUND 28 item 150: ONE point number, from ONE source ──
                    //
                    // This line read `"%.1f M pts".format(it / 1_000_000.0)`
                    // against `manifest.pointCountEstimate`, which printed the
                    // owner's 46,500-point scan as **`0.0 M pts`** — an
                    // unconditional divide by a million — while a floating chip
                    // six lines below printed 52,041 from the loaded cloud. A
                    // wrong number and a contradicting number, on the headline
                    // screen.
                    //
                    // The shared adaptive formatter now does the formatting
                    // (`PointCountFormat`, in `:core`, with its own tests), and
                    // the number comes from `state.totalPoints` — the cloud that
                    // is actually on screen. Where an estimate and a measurement
                    // disagree the measurement wins; the operator cannot tell
                    // which one he is being shown, so he must always be shown
                    // the true one.
                    meta = listOfNotNull(
                        state.totalPoints.takeIf { it > 0 }?.let { PointCountFormat.compactPts(it) },
                        manifest?.crsEpsg?.takeIf { it != 0 }?.let { "EPSG $it" } ?: "Local frame",
                    ).joinToString(" · "),
                    measureOn = state.measureMode,
                    canProcessAgain = state.canProcessAgain,
                    onBack = onBack,
                    onToggleMeasure = vm::toggleMeasure,
                    onOpenDisplay = { showDisplaySheet = true },
                    onProcessAgain = vm::processScan,
                    onOpenPlan = onOpenPlan,
                    onOpenJobs = onOpenExport,
                )
            }

            // ── ROUND 26 item 126 / ROUND 28 item 161 (R7): the cloud takes
            // the slack ────────────────────────────────────────────────────
            //
            // `weight(1f)`. The v0.9.12 screen had a fixed stack of controls
            // under the viewport AND ~300 px of dead dark ground under THAT —
            // it squeezed the cloud and wasted the bottom at the same time.
            ReviewViewport(
                state = state,
                vm = vm,
                modifier = Modifier.weight(1f),
                onEmptyTap = { controlsShown = ViewerChrome.onViewportTap(controlsShown, state.measureMode) },
            )

            AnimatedVisibility(visible = controlsVisible, enter = fadeIn(), exit = fadeOut()) {
                ReviewActions(
                    state = state,
                    vm = vm,
                    exportVm = exportVm,
                    exportContext = exportContext,
                    onOpenExportSheet = { showExportSheet = true },
                )
            }
        }
    }

    if (showDisplaySheet) {
        ModalBottomSheet(
            onDismissRequest = { showDisplaySheet = false },
            sheetState = displaySheetState,
            containerColor = ScanColors.card,
            // ROUND 16 item 61: "...and there are some tab and window show the
            // same too". This was the other one.
            shape = RoundedCornerShape(topStart = ScanDims.SheetRadius, topEnd = ScanDims.SheetRadius),
        ) {
            DisplaySheet(state, vm, advanced)
        }
    }

    if (showExportSheet && exportVm != null) {
        ModalBottomSheet(
            onDismissRequest = { showExportSheet = false },
            sheetState = exportSheetState,
            containerColor = ScanColors.card,
            shape = RoundedCornerShape(topStart = ScanDims.SheetRadius, topEnd = ScanDims.SheetRadius),
        ) {
            ExportSheet(
                vm = exportVm,
                context = exportContext,
                onDone = { showExportSheet = false },
            )
        }
    }
}

// ── header ──────────────────────────────────────────────────────────────────

/**
 * ROUND 28 item 161 — the header, hand-built rather than [com.lidarscan.app.ui.components.BackBar].
 *
 * Two reasons, and the first is R9. `BackBar` ellipsises at the **end**, and
 * every name on this screen is `Scan-085-2026-08-21-1803`: the informative tail
 * is the date and the time, and end-ellipsis is precisely the cut that throws
 * them away and leaves four identical rows reading `Scan-08…`. This title
 * middle-ellipsises, so the operator keeps both ends of the one string that
 * tells two scans apart.
 *
 * The second is the icon set. §D.4 puts [ScanIcons.Measure] and
 * [ScanIcons.Display] here as [ScanIconButton]s — full-ink, 48 dp, 24 dp glyph
 * (item 168) — and the `Tune` glyph this bar used to carry is now *scan-local
 * settings on the Scan screen*, so it must not appear on Review at all. The
 * title is `weight(1f)` against three fixed 48 dp targets, which is what keeps
 * it off them at fontScale 1.3 instead of colliding.
 */
@Composable
private fun ReviewHeader(
    title: String,
    meta: String,
    measureOn: Boolean,
    canProcessAgain: Boolean,
    onBack: () -> Unit,
    onToggleMeasure: () -> Unit,
    onOpenDisplay: () -> Unit,
    onProcessAgain: () -> Unit,
    onOpenPlan: (() -> Unit)?,
    onOpenJobs: (() -> Unit)?,
) {
    var menuOpen by remember { mutableStateOf(false) }
    Column(
        Modifier
            .fillMaxWidth()
            .background(ScanColors.page)
            .statusBarsPadding()
            // Kept from round 26: the whole chrome set shares one visibility, so
            // the presence of this tag still means "the controls are on screen".
            .testTag("reviewControls"),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                // `heightIn`, not `height`: at fontScale 1.3 the title and the
                // meta line together are taller than 56 dp, and a fixed height
                // would clip the number rather than the chrome.
                .defaultMinSize(minHeight = ScanDims.Row)
                .padding(horizontal = ScanDims.S1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ScanIconButton(Icons.AutoMirrored.Filled.ArrowBack, "Back", onBack)
            Spacer(Modifier.width(ScanDims.S1))
            Column(Modifier.weight(1f)) {
                Text(
                    text = title,
                    style = ScanTitle,
                    color = ScanColors.ink,
                    maxLines = 1,
                    // R9. See the header.
                    overflow = TextOverflow.MiddleEllipsis,
                )
                Text(
                    text = meta,
                    style = ScanMeta,
                    color = ScanColors.inkMute,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            ScanIconButton(
                icon = ScanIcons.Measure,
                contentDescription = "Measure",
                onClick = onToggleMeasure,
                toggledOn = measureOn,
                modifier = Modifier.testTag("reviewMeasureButton"),
            )
            ScanIconButton(
                icon = ScanIcons.Display,
                contentDescription = "Display",
                onClick = onOpenDisplay,
                modifier = Modifier.testTag("reviewDisplayButton"),
            )
            Box {
                ScanIconButton(
                    icon = Icons.Filled.MoreVert,
                    contentDescription = "More",
                    onClick = { menuOpen = true },
                    modifier = Modifier.testTag("reviewMenuButton"),
                )
                DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                    // ── ROUND 28 item 161 / §D.6: Process again lives here ───
                    //
                    // It was a card with a 24-word paragraph on the Jobs tab,
                    // one tab away from the scan it acts on. Disabled it carries
                    // its reason in ink-mute — see
                    // `ReviewUiState.canProcessAgain` for why that reason is not
                    // red, and for what replaced the 62-word paragraph.
                    DropdownMenuItem(
                        enabled = canProcessAgain,
                        text = {
                            Column {
                                Text(
                                    ReviewProcessWording.PROCESS_AGAIN,
                                    style = ScanBody,
                                    color = if (canProcessAgain) ScanColors.ink else ScanColors.inkFaint,
                                )
                                if (!canProcessAgain) {
                                    Text(
                                        ReviewProcessWording.ALREADY_FINAL,
                                        style = ScanMeta,
                                        color = ScanColors.inkMute,
                                        modifier = Modifier.testTag("reviewProcessAgainDetail"),
                                    )
                                }
                            }
                        },
                        onClick = {
                            menuOpen = false
                            onProcessAgain()
                        },
                        modifier = Modifier.testTag("reviewProcessAgain"),
                    )
                    // ROUND 22 item 97: the floor plan is Advanced-only, and a
                    // null callback is how that is expressed. It is a menu row
                    // rather than a third button because §D.4's action bar is
                    // exactly Share + Export in every mode.
                    if (onOpenPlan != null) {
                        DropdownMenuItem(
                            text = { Text("Floor plan", style = ScanBody, color = ScanColors.ink) },
                            onClick = {
                                menuOpen = false
                                onOpenPlan()
                            },
                        )
                    }
                    // The Advanced door to this scan's queue. §D.6 leaves Jobs
                    // as a read-only queue, so this is the only thing left on
                    // that screen worth navigating to.
                    if (onOpenJobs != null) {
                        DropdownMenuItem(
                            text = { Text("Jobs for this scan", style = ScanBody, color = ScanColors.ink) },
                            onClick = {
                                menuOpen = false
                                onOpenJobs()
                            },
                        )
                    }
                }
            }
        }
    }
}

// ── the cloud ───────────────────────────────────────────────────────────────

/**
 * ROUND 26 item 126 — the point cloud and the one tap path.
 *
 * The tap goes through `PointCloudView`'s round-25 gesture arbiter in BOTH
 * modes, rather than being attached only when measure mode is on. That is what
 * makes "a measure tap is not empty space" a single decision at a single site
 * ([ViewerChrome.onViewportTap]) instead of two listeners that could both be
 * live.
 *
 * ROUND 28 item 161: it is `weight(1f)` in a column now rather than the whole
 * window, so the empty state no longer needs the round-26 magic insets
 * (`top = 110.dp`, `bottom = 200/280.dp`) that kept its paragraph out from
 * under the floating chrome. There is no floating chrome to dodge — the band
 * this box occupies IS the band that is free.
 */
@Composable
private fun ReviewViewport(
    state: ReviewUiState,
    vm: ReviewViewModel,
    modifier: Modifier = Modifier,
    onEmptyTap: () -> Unit,
) {
    Box(
        modifier
            .fillMaxWidth()
            .background(ScanColors.viewport)
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
                // ── ROUND 25 item 117 / ROUND 26 item 126: ONE tap path ──────
                //
                // Round 25's note, kept because it is the reason this is a
                // callback and not an overlay: the measure tap used to be a
                // transparent `Box` with `detectTapGestures` laid OVER the
                // SurfaceView, on the reasoning that "a pick never fights the
                // orbit gesture". It did not fight it; it removed it — a
                // pointer-input node above the view takes the tap AND the drag
                // AND the pinch, so with measure mode on the viewer could not
                // be moved at all.
                //
                // Round 26 adds the second claimant. The tap is now always
                // wired and the DECISION is made in one pure function, so
                // hiding the chrome cannot cost a measurement and measuring
                // cannot cost the chrome. Navigation is untouched in both:
                // a drag orbits, two fingers pan, a pinch zooms and a double
                // tap reframes, whether the controls are on screen or not.
                onTapPick = { x, y ->
                    if (ViewerChrome.tapIsMeasurement(state.measureMode)) vm.onTap(x, y) else onEmptyTap()
                },
                modifier = Modifier.fillMaxSize(),
            )
            // ── ROUND 13 / ROUND 26 item 126: "Process this scan" ────────────
            //
            // Shown only when the capture is actually in pieces. A button that
            // is always there and usually does nothing teaches the operator to
            // ignore it, and this one is the answer to the complaint that
            // opened round 13: a 5-section scan was worthless and there was
            // nothing to press. §C.5 lets a warning banner float; this is one.
            if (state.sections > 1) {
                Box(
                    Modifier
                        .align(Alignment.TopCenter)
                        .widthIn(max = SECTIONS_CARD_MAX_WIDTH)
                        .padding(ScanDims.ScreenMargin),
                ) {
                    ProcessSectionsCard(state = state, onProcess = vm::processScan)
                }
            }
        } else {
            ReviewLoadState(state, vm)
        }
    }
}

/**
 * ROUND 8 (owner item 27c) / ROUND 28 item 161 — what the viewport says when
 * there is nothing to draw, in §C.6's shape.
 *
 * Round 8's note, still the reason this exists: this used to be one paragraph
 * saying "run Post-process first", which was wrong twice over for the sensor
 * the owner actually uses — post-processing REFUSED a D6 project, and the
 * reason it refused was that the trajectory was never recorded. Opening a saved
 * D6 scan now shows the 3D map without being asked, and the only case that
 * still shows text is the one that genuinely cannot be fixed.
 *
 * What item 161 changes is the *shape*: `icon → ≤6-word title → body → one
 * Primary naming the fix`. The bodies are round 8's paragraphs, unedited — the
 * pre-0.5.0 explanation in particular is long because it has to be, and
 * summarising it away would leave "no 3D map" looking like a bug rather than
 * like a fact about a capture made by an older build.
 */
@Composable
private fun ReviewLoadState(state: ReviewUiState, vm: ReviewViewModel) {
    val busy = state.load == ReviewLoad.PROBING ||
        state.load == ReviewLoad.LOADING_RECORDED ||
        state.load == ReviewLoad.RESOLVING
    Box(
        Modifier
            .fillMaxSize()
            .padding(ScanDims.ScreenMargin)
            .verticalScroll(rememberScrollState())
            .testTag("reviewLoadState"),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.widthIn(max = SECTIONS_CARD_MAX_WIDTH),
        ) {
            if (busy) {
                // §C.6 forbids a bare spinner *where a percentage exists*. None
                // does here: probing is a file header read and the re-resolve
                // publishes its first page long before it can report a
                // fraction, which is what flips this state to READY.
                CircularProgressIndicator(
                    color = ScanColors.primaryInk,
                    strokeWidth = ScanDims.S1 / 2,
                    modifier = Modifier.size(ScanDims.S8),
                )
                Spacer(Modifier.height(ScanDims.S4))
                Text(
                    text = when (state.load) {
                        // Named rather than a generic "Loading…": the three
                        // paths have very different latencies (a file read vs. a
                        // full re-resolve) and an operator who knows which one
                        // is running does not think the app has hung.
                        ReviewLoad.PROBING -> "Opening this scan…"
                        ReviewLoad.LOADING_RECORDED -> "Loading the 3D map recorded with this scan…"
                        else -> "Rebuilding the 3D map from this scan's returns and trajectory…"
                    },
                    style = ScanBody,
                    // Same reason as the empty state below: the viewport is
                    // dark in both themes, so this takes the dark ink.
                    color = com.lidarscan.app.ui.theme.InkMute,
                    textAlign = TextAlign.Center,
                )
            } else {
                ScanEmptyState(
                    icon = Icons.Filled.Layers,
                    // ROUND 28: this state is drawn INSIDE the viewport, which
                    // is dark in both themes. See `ScanEmptyState.onViewport`.
                    onViewport = true,
                    title = state.loadHeadline ?: "Nothing to show yet",
                    body = state.loadMessage.orEmpty(),
                    action = if (state.canProcess) {
                        {
                            PrimaryPill(
                                text = com.lidarscan.core.Wording.REVIEW_PROCESS,
                                onClick = vm::processScan,
                                modifier = Modifier.testTag("reviewEmptyProcessButton"),
                            )
                        }
                    } else {
                        null
                    },
                )
            }
            if (state.processing) {
                Spacer(Modifier.height(ScanDims.S4))
                LinearProgressIndicator(
                    progress = { state.processProgress },
                    modifier = Modifier
                        .fillMaxWidth()
                        .testTag("reviewEmptyProcessProgress"),
                    color = ScanColors.primaryInk,
                )
                Spacer(Modifier.height(ScanDims.S2))
                Text(
                    // The stage name, when the job reports one. A determinate
                    // bar that sits at 0 % for the first half-minute of a
                    // Mid-360 resolve is a bar an operator reads as a hang.
                    state.processStage ?: com.lidarscan.core.Wording.REVIEW_PROCESSING,
                    style = ScanMeta,
                    color = ScanColors.inkMute,
                    modifier = Modifier.testTag("reviewEmptyProcessStage"),
                )
            }
            if (state.processError != null) {
                // ROUND 27 item 134(b): the REASON, on screen. It already
                // existed — the native engine's `lastError()`, the job's own
                // message — and nothing read it, so a failed run cleared its
                // chip and returned the operator to this same paragraph with no
                // evidence anything had happened. `bad` is correct here and
                // nowhere else on this screen: an operation ran and failed.
                Spacer(Modifier.height(ScanDims.S3))
                Text(
                    state.processError,
                    style = ScanBody,
                    color = ScanColors.bad,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.testTag("reviewProcessError"),
                )
            }
        }
    }
}

// ── the action row ──────────────────────────────────────────────────────────

/**
 * ROUND 28 item 161 — §D.4's action bar: **Share and Export, and nothing
 * else.**
 *
 * ROUND 22 items 96 + 97 / ROUND 23 item 104a, still the reason for the split:
 * Export is the primary and Share the secondary because only one of the two
 * guarantees the file survives the operator dismissing a chooser. Share is
 * `ProcessingViewModel.export(share = true)` — the same job and the same
 * ROUND 7 "Downloads FIRST, sheet second" order, one flag apart.
 *
 * What is new is what is *not* here: the five permanent format chips and the
 * "Saves to your Downloads folder." caption. Format is chosen once per export,
 * not browsed (R5), so it moved into the sheet Export opens and the caption
 * became that sheet's confirm button.
 *
 * The message line above the row is §C.6's success/failure state: **an inline
 * row at the point of action, never a floating toast over the thing it
 * describes.**
 */
@Composable
private fun ReviewActions(
    state: ReviewUiState,
    vm: ReviewViewModel,
    exportVm: com.lidarscan.app.ui.processing.ProcessingViewModel?,
    exportContext: android.content.Context,
    onOpenExportSheet: () -> Unit,
) {
    Column(Modifier.fillMaxWidth().background(ScanColors.page)) {
        if (state.measureMode) {
            MeasureCard(state, vm)
        }
        val exportState = exportVm?.uiState?.collectAsStateWithLifecycle()?.value
        exportState?.message?.let { message ->
            HorizontalDivider(thickness = ScanDims.Hair, color = ScanColors.line)
            ScanRow(
                title = message,
                titleColor = ScanColors.inkMute,
                modifier = Modifier.testTag("reviewExportMessage"),
            )
        }
        Row(
            Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = ScanDims.RowWithThumb)
                .padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S3),
            horizontalArrangement = Arrangement.spacedBy(ScanDims.S3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SecondaryPill(
                text = ProjectActionWording.SHARE_ACTION,
                icon = Icons.Filled.Share,
                enabled = exportVm != null && state.hasCloud,
                onClick = { exportVm?.export(exportContext, share = true) },
                modifier = Modifier.weight(1f).testTag("reviewShareButton"),
            )
            PrimaryPill(
                text = com.lidarscan.core.Wording.EXPORT_ACTION,
                icon = Icons.Filled.IosShare,
                enabled = exportVm != null && state.hasCloud,
                onClick = onOpenExportSheet,
                modifier = Modifier.weight(1f).testTag("reviewExportButton"),
            )
        }
    }
}

// ── measure ─────────────────────────────────────────────────────────────────

/**
 * B11's measure read-out, restyled onto the round-28 scale.
 *
 * **What did not change**: the same "nearest sampled point" caveat, the same
 * units, the same Clear. B11's behaviour was not in scope for the redesign.
 *
 * ROUND 28 item 161: the `TAP A POINT` / `MEASURING` chip that used to sit
 * under the header is gone. A chip is a *state that deviates from the norm of
 * a visible set* (§C.4) and this one was a mode indicator in a sixth chip
 * treatment, duplicating a card that already says the same thing in words.
 */
@Composable
private fun MeasureCard(state: ReviewUiState, vm: ReviewViewModel) {
    ScanCard(
        modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S2),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(ScanDims.CardPadding),
    ) {
        val m = state.measurement
        if (m != null) {
            Text(vm.formatted(m.distanceM), style = ScanTitle, color = ScanColors.ink)
            Text(
                "horizontal ${vm.formatted(m.horizontalM)} · Δz ${vm.formatted(m.deltaZM)}",
                style = ScanMeta,
                color = ScanColors.inkMute,
            )
            Text(
                // ROUND 22 item 98: was 37 words explaining the sampling
                // strategy. The honesty that matters — the pick can be a few
                // centimetres out — survives; the explanation of WHY does not
                // belong on a measuring tool.
                com.lidarscan.core.Wording.MEASURE_DETAIL,
                style = ScanMeta,
                color = ScanColors.inkMute,
            )
        } else {
            Text(
                state.pickMessage ?: com.lidarscan.core.Wording.MEASURE_HINT,
                style = ScanBody,
                color = ScanColors.ink,
            )
        }
        Spacer(Modifier.height(ScanDims.S2))
        Row(
            horizontalArrangement = Arrangement.spacedBy(ScanDims.S2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.weight(1f)) {
                SegmentedPill(
                    options = MeasureUnit.entries.map { it to it.abbreviation },
                    selected = state.measureUnit,
                    onSelect = vm::setMeasureUnit,
                )
            }
            SecondaryPill(text = "Clear", onClick = vm::clearMeasurement)
        }
    }
}

// ── the export sheet ────────────────────────────────────────────────────────

/**
 * ROUND 28 item 161 / §D.4 — **the Export sheet: format chosen once, not
 * browsed.**
 *
 * Five export formats lived on the body of this screen as permanent chips
 * (R5), one of them in an off-palette lavender (R4), and two of them —
 * `DXF` and `PDF` — are **floor-plan vector formats the point-cloud export job
 * cannot write at all** (`ExportFormat.isPointCloud`). That is not a styling
 * defect: the screen offered the operator two choices that could only fail. The
 * list here is `ExportFormat.pointCloudFormats`, which is the exporter's own
 * answer to "what can this write", plus the bundle — so the sheet cannot drift
 * from the pipeline.
 *
 * ## Destination
 *
 * §D.6 moves the Jobs tab's `This phone / Cloud / Bundle` segmented control
 * here. It arrives as two rows rather than one, because that control was one
 * enum doing two orthogonal jobs: **Bundle is a format** (the mockup lists it
 * with the others — a `.lscan.zip` of the whole capture, written by
 * `transferBundle`), and **This phone / Cloud is a destination**. Splitting
 * them is what lets the sheet state, honestly, that a Cloud submit ignores the
 * chosen format — it uploads the capture and a Linux worker runs the same
 * engine CLI on it. That is why the format rows disable themselves when the
 * destination is Cloud, with the reason in ink-mute per §C.6 rather than in
 * red: nothing has failed.
 *
 * Nothing about the pipeline, the job, the gates or the ROUND 7 Downloads
 * delivery is reimplemented here — `setExportFormat`, `export`,
 * `transferBundle` and `cloudSubmit` are the Processing screen's own calls.
 */
@Composable
private fun ExportSheet(
    vm: com.lidarscan.app.ui.processing.ProcessingViewModel,
    context: android.content.Context,
    onDone: () -> Unit,
) {
    val state by vm.uiState.collectAsStateWithLifecycle()
    var bundle by rememberSaveable { mutableStateOf(false) }
    var toCloud by rememberSaveable { mutableStateOf(false) }
    var destinationOpen by remember { mutableStateOf(false) }

    Column(
        Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(bottom = ScanDims.S8),
    ) {
        Text(
            com.lidarscan.core.Wording.EXPORT_ACTION,
            style = ScanTitle,
            color = ScanColors.ink,
            modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S2),
        )
        SheetRows(
            // Kept from the permanent chip row §D.4 deletes: the emulator suite
            // knows this screen's format control by this name, and it is still
            // the format control — it is a list of rows in a sheet now.
            modifier = Modifier.testTag("reviewExportFormats"),
            rows = buildList<@Composable () -> Unit> {
                ExportFormat.pointCloudFormats.forEach { format ->
                    add {
                        ExportOptionRow(
                            code = exportCode(format),
                            description = exportDescription(format),
                            selected = !bundle && state.exportFormat == format,
                            enabled = !toCloud,
                            onSelect = {
                                bundle = false
                                vm.setExportFormat(format)
                            },
                        )
                    }
                }
                add {
                    ExportOptionRow(
                        code = "BUNDLE",
                        // ROUND 18 item 71 is what makes this honest: the bundle
                        // waits for the auto-process before it zips, and says so
                        // inside itself when it could not.
                        description = "Everything, for desktop",
                        selected = bundle,
                        enabled = !toCloud,
                        onSelect = { bundle = true },
                    )
                }
                add {
                    Box {
                        ScanRow(
                            title = "Destination",
                            detail = when {
                                toCloud && !state.cloudGate.enabled -> state.cloudGate.reason
                                toCloud -> "Cloud takes the whole capture."
                                else -> null
                            },
                            meta = if (toCloud) "Cloud" else "This phone",
                            onClick = { destinationOpen = true },
                            trailing = {
                                Icon(
                                    Icons.Filled.ArrowDropDown,
                                    contentDescription = null,
                                    tint = ScanColors.inkFaint,
                                )
                            },
                            modifier = Modifier.testTag("reviewExportDestination"),
                        )
                        DropdownMenu(
                            expanded = destinationOpen,
                            onDismissRequest = { destinationOpen = false },
                        ) {
                            DropdownMenuItem(
                                text = { Text("This phone", style = ScanBody, color = ScanColors.ink) },
                                onClick = {
                                    toCloud = false
                                    destinationOpen = false
                                },
                            )
                            DropdownMenuItem(
                                enabled = state.cloudGate.enabled,
                                text = {
                                    Text(
                                        "Cloud",
                                        style = ScanBody,
                                        color = if (state.cloudGate.enabled) ScanColors.ink else ScanColors.inkFaint,
                                    )
                                },
                                onClick = {
                                    toCloud = true
                                    destinationOpen = false
                                },
                            )
                        }
                    }
                }
            },
        )

        // The one thing that can make an export produce a file the operator did
        // not expect — LAS wanting a georeference, most of the time. Carried
        // through from the Processing screen verbatim.
        state.exportNote?.takeIf { !toCloud && !bundle }?.let { note ->
            Text(
                note,
                style = ScanMeta,
                color = ScanColors.warn,
                modifier = Modifier.padding(
                    horizontal = ScanDims.ScreenMargin,
                    vertical = ScanDims.S2,
                ),
            )
        }

        val gate = when {
            toCloud -> state.cloudGate
            bundle -> state.transferGate
            else -> state.exportGate
        }
        PrimaryPill(
            // §D.4: "Saves to your Downloads folder." was a permanent caption
            // under the body. It is the button's own detail now — it describes
            // what pressing THIS does, at the moment of pressing it.
            text = if (toCloud) "Export · sends to your server" else "Export · saves to Downloads",
            enabled = gate.enabled,
            onClick = {
                when {
                    toCloud -> vm.cloudSubmit()
                    bundle -> vm.transferBundle(context, share = false)
                    else -> vm.export(context)
                }
                onDone()
            },
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S3)
                .testTag("reviewExportConfirm"),
        )
        gate.reason?.takeIf { !gate.enabled }?.let { reason ->
            Text(
                reason,
                style = ScanMeta,
                color = ScanColors.inkMute,
                modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin),
            )
        }
    }
}

/** §D.4's mockup names the formats the way an operator says them, not the way the enum does. */
private fun exportCode(format: ExportFormat): String = when (format) {
    ExportFormat.PLY_BINARY -> "PLY"
    ExportFormat.LAS14 -> "LAS 1.4"
    ExportFormat.PCD -> "PCD"
    // Unreachable — `pointCloudFormats` is the only caller — but a `when` over
    // an enum that can grow should not compile into a silent blank.
    ExportFormat.DXF, ExportFormat.PDF -> format.displayName
}

/**
 * ≤12 words, and about **what the operator will do with the file**, not about
 * the file. `ExportFormat.description` says "Binary PLY with RGB. Opens
 * everywhere; no CRS." — the first half is for whoever writes the exporter.
 */
private fun exportDescription(format: ExportFormat): String = when (format) {
    ExportFormat.PLY_BINARY -> "Opens everywhere"
    ExportFormat.LAS14 -> "Survey tools"
    ExportFormat.PCD -> "Robotics tools"
    ExportFormat.DXF, ExportFormat.PDF -> "Floor plan"
}

/**
 * ROUND 28 item 161 — a [ScanRow] whose title is a **code**.
 *
 * §C.4 makes `ScanRow` the default and §D.4 asks for these rows verbatim; the
 * one thing `ScanRow` cannot express is item 167's other half — *a code a
 * person spells out (`PLY`, `LAS 1.4`) is [ScanMetaCaps], not [ScanBody]*. Its
 * `title` is `ScanBody` by construction, and `ScanUi.kt` is not this task's
 * file. So the geometry is `ScanRow`'s, exactly (`ScanDims.Row`,
 * `CardPadding`, `IconGap`), and only the title's style differs. A
 * `titleStyle` parameter on `ScanRow` would delete this composable.
 */
@Composable
private fun ExportOptionRow(
    code: String,
    description: String,
    selected: Boolean,
    enabled: Boolean,
    onSelect: () -> Unit,
) {
    val ink = if (enabled) ScanColors.ink else ScanColors.inkFaint
    Row(
        Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = ScanDims.Row)
            .then(if (enabled) Modifier.clickable(onClick = onSelect) else Modifier)
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S2),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
    ) {
        Text(code, style = ScanMetaCaps, color = ink, modifier = Modifier.weight(1f))
        Text(
            description,
            style = ScanMeta,
            color = ScanColors.inkMute,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Icon(
            imageVector = if (selected) Icons.Filled.RadioButtonChecked else Icons.Filled.RadioButtonUnchecked,
            contentDescription = null,
            tint = if (selected && enabled) ScanColors.primaryInk else ScanColors.inkFaint,
            modifier = Modifier.size(ScanDims.S6),
        )
    }
}

// ── the display sheet ───────────────────────────────────────────────────────

/**
 * §3.9's render settings — **and, since ROUND 28 item 161, the home of the
 * colour control.**
 *
 * `HEIGHT` / `INTENSITY` were two chips on the body of the Review screen, given
 * the same visual weight as `Export` (R10): a display preference, drawn as
 * loudly as the once-a-session action that writes a file. They are the first
 * row of this sheet now, which is where a preference belongs and which is what
 * the display icon in the header has always opened.
 *
 * The simple sheet is §D.4's mockup, row for row. Everything §3.9 also carries
 * — the ramp choice, gamma, brightness, auto-range, the point-size numbers, the
 * detail budget, clipping, profiles — is real and persisted and lives under
 * Advanced, because none of it is a thing an operator touches while looking at
 * a finished scan and all of it was competing with the two controls that are.
 */
@Composable
private fun DisplaySheet(state: ReviewUiState, vm: ReviewViewModel, advanced: Boolean) {
    val d = state.display
    val trajectoryNote by vm.trajectoryNote.collectAsStateWithLifecycle()
    var pointSizeOpen by remember { mutableStateOf(false) }
    var backgroundOpen by remember { mutableStateOf(false) }

    Column(
        Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(bottom = ScanDims.S8),
    ) {
        Text(
            "Display",
            style = ScanTitle,
            color = ScanColors.ink,
            modifier = Modifier.padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S2),
        )
        SheetRows(
            listOf(
                {
                    // ROUND 10 (owner item 39) / ROUND 11 (item 42), unchanged
                    // reasoning: the picker offers what `colorModeAvailability`
                    // says is available rather than enumerating the enum — that
                    // map is where the pause on RGB, the missing per-point
                    // timestamps and the "needs a rover" fix tag are each
                    // expressed once. Coverage is excluded here and only here:
                    // it answers "where have I not been yet", which is a
                    // question about a walk in progress, and on a sealed scan
                    // the answer is "nowhere else, it is finished".
                    val offered = ColorMode.entries.filter {
                        it != ColorMode.COVERAGE && state.colorModeReasons[it] == null
                    }
                    ChoiceRow(
                        label = "Colour",
                        // If a project was saved in a mode this build cannot
                        // offer, say why rather than silently re-selecting.
                        detail = state.colorModeReasons[d.colorMode],
                        options = offered.map { it to it.name.lowercase().replaceFirstChar { c -> c.uppercase() } },
                        selected = d.colorMode,
                        onSelect = { mode -> vm.updateDisplay { it.copy(colorMode = mode) } },
                        testTag = "reviewColourRow",
                    )
                },
                {
                    // ROUND 26 item 127 / ROUND 27 item 141: height opens on
                    // Turbo, and a persisted grayscale is migrated to it once.
                    // The swatch is that decision made visible — the operator
                    // can see that low is dark blue and high is dark red, which
                    // is the whole reason Turbo beat Spectrum on a dark
                    // viewport. `HeightRamp` samples the same `ColormapLut` the
                    // shader samples, so this strip and the cloud cannot drift.
                    ScanRow(
                        title = "Height ramp",
                        trailing = {
                            Box(
                                Modifier
                                    .width(RAMP_SWATCH_WIDTH)
                                    .height(ScanDims.S3)
                                    .background(
                                        Brush.horizontalGradient(HeightRamp.Turbo.stops()),
                                        RoundedCornerShape(ScanDims.S1),
                                    )
                                    .testTag("reviewHeightRamp"),
                            )
                        },
                    )
                },
                {
                    // ── ROUND 28 item 161, owner-approved: the intensity ramp
                    // is a CHOICE ──────────────────────────────────────────
                    //
                    // Round 10 item 39 made intensity grayscale deliberately: a
                    // D6 return has no colour of its own, so INTENSITY is the
                    // driver's own intensity triple, and a hue ramp over it
                    // invents structure that is not in the data. That reasoning
                    // holds, which is why GREY is still the default and why
                    // this row exists at all rather than the ramp simply being
                    // switched. What it does not justify is *removing the
                    // operator's ability to pick*: on a low-contrast scan a
                    // colour ramp separates returns the grey ramp compresses.
                    ChoiceRow(
                        label = "Intensity ramp",
                        options = listOf(
                            Colormap.GRAYSCALE to "Grey",
                            Colormap.TURBO to "Colour",
                        ),
                        selected = if (d.intensity.colormap == Colormap.GRAYSCALE) {
                            Colormap.GRAYSCALE
                        } else {
                            Colormap.TURBO
                        },
                        onSelect = { cm ->
                            vm.updateDisplay { it.copy(intensity = it.intensity.copy(colormap = cm)) }
                        },
                        testTag = "reviewIntensityRamp",
                    )
                },
                {
                    Box {
                        ScanRow(
                            title = "Point size",
                            meta = pointSizeName(d.pointSize.mode),
                            onClick = { pointSizeOpen = true },
                            trailing = {
                                Icon(
                                    Icons.Filled.ArrowDropDown,
                                    contentDescription = null,
                                    tint = ScanColors.inkFaint,
                                )
                            },
                        )
                        DropdownMenu(expanded = pointSizeOpen, onDismissRequest = { pointSizeOpen = false }) {
                            PointSizeMode.entries.forEach { m ->
                                DropdownMenuItem(
                                    text = { Text(pointSizeName(m), style = ScanBody, color = ScanColors.ink) },
                                    onClick = {
                                        vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(mode = m)) }
                                        pointSizeOpen = false
                                    },
                                )
                            }
                        }
                    }
                },
                {
                    // ROUND 16 items 59 + 61: the toggle is
                    // `DisplayParams.showTrajectory`, which has been persisted
                    // per project since the desktop viewer. Adding a second
                    // Boolean beside it would have been exactly the duplicated
                    // surface item 61 is about.
                    //
                    // ROUND 17 item 65: and when there is no path, say so. A
                    // missing trajectory used to remove the entity from the
                    // scene in silence, which is indistinguishable from a broken
                    // renderer. It is the row's own `detail` now — the fact
                    // belongs to the control it is about.
                    ScanRow(
                        title = com.lidarscan.core.Wording.SHOW_MY_PATH,
                        detail = trajectoryNote ?: "Start green, end amber, red where tracking was lost.",
                        modifier = Modifier.testTag("trajectoryNote"),
                        trailing = {
                            Switch(
                                checked = d.showTrajectory,
                                onCheckedChange = { on -> vm.updateDisplay { it.copy(showTrajectory = on) } },
                            )
                        },
                    )
                },
                {
                    Box {
                        val current = BACKGROUNDS.firstOrNull { it.second == d.background }
                        ScanRow(
                            title = "Background",
                            meta = current?.first ?: "Custom",
                            onClick = { backgroundOpen = true },
                            leading = {
                                Box(
                                    Modifier
                                        .size(ScanDims.S6)
                                        .background(d.background.toColor(), CircleShape),
                                )
                            },
                            trailing = {
                                Icon(
                                    Icons.Filled.ArrowDropDown,
                                    contentDescription = null,
                                    tint = ScanColors.inkFaint,
                                )
                            },
                        )
                        DropdownMenu(expanded = backgroundOpen, onDismissRequest = { backgroundOpen = false }) {
                            BACKGROUNDS.forEach { (label, rgba) ->
                                DropdownMenuItem(
                                    text = { Text(label, style = ScanBody, color = ScanColors.ink) },
                                    onClick = {
                                        vm.updateDisplay { it.copy(background = rgba) }
                                        backgroundOpen = false
                                    },
                                )
                            }
                        }
                    }
                },
            ),
        )

        if (advanced) {
            DisplayAdvanced(state, vm)
        }
    }
}

/**
 * §3.9's remaining parameters, behind the Advanced switch.
 *
 * ROUND 23 item 106d, unchanged and still the reason the profile list is
 * filtered rather than enumerated: applying a profile is how the Survey GNSS
 * capture gate and the Research point budget get switched on, so a Simple-mode
 * screen that still listed all four had the hidden features one tap away. The
 * filter is `SimpleMode.displayProfiles`, in `:core` and under test.
 *
 * ROUND 22 item 98's EDL removal stands: the field survives on `DisplayParams`
 * (it travels with the project, so a desktop that can draw EDL still gets the
 * intent) but there is no row, because `points.mat` has no post-process pass
 * and a switch that renders nothing costs a decision and returns nothing.
 */
@Composable
private fun DisplayAdvanced(state: ReviewUiState, vm: ReviewViewModel) {
    val d = state.display
    var profileOpen by remember { mutableStateOf(false) }
    var rampOpen by remember { mutableStateOf(false) }
    val scalar = d.activeScalar
    val scalarMode = d.colorMode

    SectionLabel("ADVANCED")
    SheetRows(
        buildList<@Composable () -> Unit> {
            add {
                Box {
                    ScanRow(
                        title = "Profile",
                        detail = "Sets every value below at once.",
                        onClick = { profileOpen = true },
                        trailing = {
                            Icon(
                                Icons.Filled.ArrowDropDown,
                                contentDescription = null,
                                tint = ScanColors.inkFaint,
                            )
                        },
                        // Kept from the FilterChip row this replaces.
                        modifier = Modifier.testTag("reviewProfileChips"),
                    )
                    DropdownMenu(expanded = profileOpen, onDismissRequest = { profileOpen = false }) {
                        com.lidarscan.core.SimpleMode.displayProfiles(true).forEach { p ->
                            DropdownMenuItem(
                                text = { Text(p.displayName, style = ScanBody, color = ScanColors.ink) },
                                onClick = {
                                    vm.applyProfile(p)
                                    profileOpen = false
                                },
                            )
                        }
                    }
                }
            }
            if (scalarMode == ColorMode.HEIGHT || scalarMode == ColorMode.INTENSITY) {
                add {
                    Box {
                        ScanRow(
                            title = "Ramp",
                            meta = scalar.colormap.name,
                            onClick = { rampOpen = true },
                            trailing = {
                                Icon(
                                    Icons.Filled.ArrowDropDown,
                                    contentDescription = null,
                                    tint = ScanColors.inkFaint,
                                )
                            },
                        )
                        DropdownMenu(expanded = rampOpen, onDismissRequest = { rampOpen = false }) {
                            Colormap.entries.forEach { cm ->
                                DropdownMenuItem(
                                    text = { Text(cm.name, style = ScanMetaCaps, color = ScanColors.ink) },
                                    onClick = {
                                        vm.updateScalar(scalarMode) { it.copy(colormap = cm) }
                                        rampOpen = false
                                    },
                                )
                            }
                        }
                    }
                }
                add {
                    SliderRow("Gamma", "%.2f".format(scalar.gamma), scalar.gamma, 0.1f..4f) { v ->
                        vm.updateScalar(scalarMode) { it.copy(gamma = v) }
                    }
                }
                add {
                    SliderRow("Brightness", "%.2f".format(scalar.brightness), scalar.brightness, 0.1f..3f) { v ->
                        vm.updateScalar(scalarMode) { it.copy(brightness = v) }
                    }
                }
                add {
                    ScanRow(
                        title = "Auto range",
                        detail = "Track the cloud's real range each frame.",
                        trailing = {
                            Switch(
                                checked = scalar.autoRange,
                                onCheckedChange = { on -> vm.updateScalar(scalarMode) { it.copy(autoRange = on) } },
                            )
                        },
                    )
                }
            }
            when (d.pointSize.mode) {
                PointSizeMode.FIXED_PIXELS -> add {
                    SliderRow(
                        "Size",
                        "%.1f px".format(d.pointSize.fixedPx),
                        d.pointSize.fixedPx,
                        // ROUND 19 item 76: the same constants the capture sheet reads.
                        com.lidarscan.core.render.DisplayLimits.POINT_SIZE_MIN_PX..
                            com.lidarscan.core.render.DisplayLimits.POINT_SIZE_MAX_PX,
                    ) { v -> vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(fixedPx = v)) } }
                }
                PointSizeMode.ADAPTIVE -> {
                    add {
                        SliderRow(
                            "Smallest",
                            "%.1f px".format(d.pointSize.adaptiveMinPx),
                            d.pointSize.adaptiveMinPx,
                            0.5f..12f,
                        ) { v -> vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(adaptiveMinPx = v)) } }
                    }
                    add {
                        SliderRow(
                            "Largest",
                            "%.1f px".format(d.pointSize.adaptiveMaxPx),
                            d.pointSize.adaptiveMaxPx,
                            0.5f..24f,
                        ) { v -> vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(adaptiveMaxPx = v)) } }
                    }
                }
                PointSizeMode.WORLD_SIZE -> add {
                    SliderRow(
                        "Diameter",
                        "%.0f mm".format(d.pointSize.worldSizeM * 1000f),
                        d.pointSize.worldSizeM,
                        0.0005f..0.2f,
                    ) { v -> vm.updateDisplay { it.copy(pointSize = it.pointSize.copy(worldSizeM = v)) } }
                }
            }
            add {
                // ROUND 22 item 98: was 37 words about page order and
                // decimation. ROUND 22 item 100: and when the device is what is
                // limiting the slider, say THAT instead — it is the only one of
                // the two the operator can act on (by using a different phone).
                SliderRow(
                    "Detail budget",
                    "${(d.lodPointBudget / 1_000_000f).roundToInt()} M points",
                    d.lodPointBudget / 1_000_000f,
                    com.lidarscan.core.render.DisplayLimits.LOD_MIN_M..
                        com.lidarscan.core.render.DisplayLimits.LOD_MAX_M,
                    detail = com.lidarscan.core.Wording.DETAIL_BUDGET_HINT,
                ) { v -> vm.updateDisplay { it.copy(lodPointBudget = (v * 1_000_000f).roundToInt()) } }
            }
            add {
                ScanRow(
                    title = "Height clip",
                    detail = "Hide everything outside a height band.",
                    trailing = {
                        Switch(
                            checked = d.clipHeightEnabled,
                            onCheckedChange = { on -> vm.updateDisplay { it.copy(clipHeightEnabled = on) } },
                        )
                    },
                )
            }
            if (d.clipHeightEnabled) {
                add {
                    SliderRow("Floor", "%.2f m".format(d.clipHeightMin), d.clipHeightMin, -5f..10f) { v ->
                        vm.updateDisplay { it.copy(clipHeightMin = v) }
                    }
                }
                add {
                    SliderRow("Ceiling", "%.2f m".format(d.clipHeightMax), d.clipHeightMax, -5f..10f) { v ->
                        vm.updateDisplay { it.copy(clipHeightMax = v) }
                    }
                }
            }
            add {
                ScanRow(
                    title = "Box clip",
                    detail = "Restrict to a box in the scan's own frame.",
                    trailing = {
                        Switch(
                            checked = d.clipBoxEnabled,
                            onCheckedChange = { on -> vm.updateDisplay { it.copy(clipBoxEnabled = on) } },
                        )
                    },
                )
            }
            add {
                ScanRow(
                    title = "Reset to profile defaults",
                    titleColor = ScanColors.primaryInk,
                    onClick = vm::resetToProfileDefault,
                )
            }
        },
    )
}

// ── shared sheet parts ──────────────────────────────────────────────────────

/**
 * Rows with hairlines between them and **no card around them**.
 *
 * `ScanRowCard` is the right thing on a page; inside a sheet it is a card drawn
 * on the same `ScanColors.card` the sheet already is, which renders as a border
 * around nothing. §D.4's sheet mockups are bare rows, and this is that.
 */
@Composable
private fun SheetRows(rows: List<@Composable () -> Unit>, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth()) {
        rows.forEachIndexed { index, row ->
            if (index > 0) {
                HorizontalDivider(thickness = ScanDims.Hair, color = ScanColors.line)
            }
            row()
        }
    }
}

/**
 * A [ScanRow]-shaped row whose control is a [SegmentedPill].
 *
 * `ScanRow`'s `trailing` slot cannot hold one: `SegmentedPill` calls
 * `fillMaxWidth()` on itself, and an unweighted `Row` child that fills the
 * width takes the whole row and starves the weighted title of it. So the label
 * and the control are both weighted here, which is the only difference from
 * `ScanRow` — the padding, the minimum height and the type are its.
 */
@Composable
private fun <T> ChoiceRow(
    label: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    detail: String? = null,
    testTag: String? = null,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = ScanDims.Row)
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S2)
            .then(if (testTag != null) Modifier.testTag(testTag) else Modifier),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = ScanBody, color = ScanColors.ink, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (detail != null) {
                Text(detail, style = ScanMeta, color = ScanColors.inkMute, maxLines = 2)
            }
        }
        Box(Modifier.weight(1.3f)) {
            SegmentedPill(options = options, selected = selected, onSelect = onSelect)
        }
    }
}

/** A slider under a `ScanRow`-shaped label line: name on the left, value on the right. */
@Composable
private fun SliderRow(
    label: String,
    readout: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    detail: String? = null,
    onChange: (Float) -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S2),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = ScanBody, color = ScanColors.ink, modifier = Modifier.weight(1f))
            Text(readout, style = ScanMeta, color = ScanColors.inkMute)
        }
        SheetSlider(
            value = value,
            range = range,
            onValueChange = onChange,
            contentDescription = label,
        )
        if (detail != null) {
            Text(detail, style = ScanMeta, color = ScanColors.inkMute)
        }
    }
}

// ── the sections banner ─────────────────────────────────────────────────────

/**
 * ROUND 13 — the Process card.
 *
 * The numbers it shows are the ones that mean something to a person holding a
 * phone: how many pieces, and the HEIGHT SPREAD before and after. The height
 * spread is the headline because it is the only number in the report that does
 * not come from the same measurement that produced the correction — the
 * operator walks on a flat floor, so it has to shrink, and on the owner's
 * scan-030 it goes 0.82 m -> 0.27 m.
 *
 * The start-to-end gap is stated but never sold as an improvement: before
 * stitching it compares two points in different world frames and means nothing,
 * and after, it is ARCore's own drift, which this does not fix.
 *
 * ROUND 28 item 161: it is a `ScanCard(floating = true)` on the warn container
 * now rather than a hand-rolled `Column` with its own background and radius —
 * §C.5 permits exactly three floating things and a warning banner is one of
 * them. Its button is a Secondary, because §C.4 allows one primary per screen
 * and that is Export.
 */
@Composable
private fun ProcessSectionsCard(
    state: ReviewUiState,
    onProcess: () -> Unit,
) {
    ScanCard(
        modifier = Modifier.testTag("reviewProcessCard"),
        container = ScanColors.warnContainer,
        borderColor = ScanColors.warn,
        floating = true,
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
            style = ScanBody,
            color = ScanColors.ink,
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
            Spacer(Modifier.height(ScanDims.S2))
            Text(state.processError, style = ScanMeta, color = ScanColors.bad)
        }
        Spacer(Modifier.height(ScanDims.S3))
        if (state.processing) {
            // Determinate, because the pipeline reports a real fraction. A
            // spinner on a job that can take forty seconds is how an operator
            // decides the app has hung.
            LinearProgressIndicator(
                progress = { state.processProgress },
                modifier = Modifier
                    .fillMaxWidth()
                    .testTag("reviewProcessProgress"),
                color = ScanColors.primaryInk,
            )
            Spacer(Modifier.height(ScanDims.S2))
            Text(
                "Aligning the pieces… ${(state.processProgress * 100).toInt()}%.",
                style = ScanMeta,
                color = ScanColors.inkMute,
            )
        } else {
            SecondaryPill(
                text = if (state.isStitched) ReviewProcessWording.PROCESS_AGAIN else "Process this scan",
                onClick = onProcess,
                modifier = Modifier.fillMaxWidth().testTag("reviewProcessButton"),
            )
        }
    }
}

// ── local constants and helpers ─────────────────────────────────────────────

/**
 * The banner and the empty state stop growing here. Both are prose over a
 * viewport, and a paragraph that runs the full width of a tablet is a paragraph
 * nobody finishes. Off the 4 dp grid's *spacing* values on purpose — this is a
 * measure, not a gap.
 */
private val SECTIONS_CARD_MAX_WIDTH = 400.dp

/** §D.4's height-ramp swatch: four grid cells wide, `ScanDims.S3` tall. */
private val RAMP_SWATCH_WIDTH = ScanDims.S6 * 4

private val BACKGROUNDS = listOf(
    "Dark" to com.lidarscan.core.render.Rgba(18, 18, 22, 255),
    "Black" to com.lidarscan.core.render.Rgba(0, 0, 0, 255),
    "Grey" to com.lidarscan.core.render.Rgba(96, 96, 100, 255),
    "White" to com.lidarscan.core.render.Rgba(245, 245, 248, 255),
)

private fun com.lidarscan.core.render.Rgba.toColor(): Color =
    Color(red = r / 255f, green = g / 255f, blue = b / 255f, alpha = 1f)

private fun pointSizeName(mode: PointSizeMode): String = when (mode) {
    PointSizeMode.FIXED_PIXELS -> "Fixed px"
    PointSizeMode.ADAPTIVE -> "Adaptive"
    PointSizeMode.WORLD_SIZE -> "World size"
}
