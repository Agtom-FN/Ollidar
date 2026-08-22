package com.lidarscan.app.ui.projects

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ViewList
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.GridView
import androidx.compose.material.icons.filled.IosShare
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Sort
import androidx.compose.material.icons.outlined.Layers
import androidx.compose.material.icons.outlined.RadioButtonUnchecked
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.AvatarButton
import com.lidarscan.app.ui.components.HeroHeader
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanCard
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.ScanEmptyState
import com.lidarscan.app.ui.components.ScanIconButton
import com.lidarscan.app.ui.components.ScanRow
import com.lidarscan.app.ui.components.ScanRowCard
import com.lidarscan.app.ui.components.StatusDot
import com.lidarscan.app.ui.components.deviates
import com.lidarscan.app.ui.components.modalValue
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.app.ui.theme.ScanMeta
import com.lidarscan.app.ui.theme.ScanMetaCaps
import com.lidarscan.app.ui.theme.ScanTitle
import com.lidarscan.core.Wording
import com.lidarscan.core.model.ProjectManifest
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.projects.BatchAction
import com.lidarscan.core.projects.ProjectActionWording
import com.lidarscan.core.projects.ProjectSelection
import com.lidarscan.core.projects.ProjectSort
import com.lidarscan.core.projects.ProjectsLayout
import com.lidarscan.core.projects.ProjectsView
import com.lidarscan.core.render.PointCountFormat
import com.lidarscan.core.store.Project
import kotlinx.coroutines.launch

@Composable
fun ProjectsListRoute(
    container: AppContainer,
    /**
     * ROUND 8 (owner item 31): the scan Capture has just sealed, so that
     * Stop lands here with the new scan already selected and its 3D preview
     * open rather than on an anonymous list the operator has to search.
     *
     * A one-shot: it seeds [ProjectsListScreen]'s own selection the first time
     * a given id arrives and never fights the user's taps afterwards. Null on
     * every other route into this tab.
     */
    initialSelectedId: String? = null,
    onSelectProject: (String) -> Unit,
    onOpenProject: (String) -> Unit,
    onOpenReview: (String) -> Unit,
    onNewScan: () -> Unit,
    onSettings: () -> Unit,
    /**
     * ROUND 22 item 96 — the row's ⋯ › Export. Export lives on the Review
     * screen now (the "Details, jobs & export" hub is gone from Simple mode),
     * so this opens Review with its export row already unfolded rather than
     * duplicating the export UI on a row.
     */
    onExport: (String) -> Unit = onOpenReview,
    /**
     * ROUND 23 item 104b — the row's ⋯ › Share. Same destination as Export:
     * Review is where the Share button and the format row both live.
     */
    onShare: (String) -> Unit = onExport,
    /**
     * ROUND 22 item 97: with Advanced ON, the ⋯ menu gains a fourth item that
     * opens the "Details, jobs & export" hub — the screen Simple mode removes
     * is reachable again, exactly as it is today, the moment the switch is on.
     */
    advanced: Boolean = false,
) {
    val viewModel: ProjectsListViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ProjectsListViewModel(
                    projectStore = container.projectStore,
                    // ROUND 28 item 162: no `keepEmptyScans` any more. The list
                    // shows every scan, empties included — see the ViewModel's
                    // header and finding P1j.
                    //
                    // ROUND 22 item 96: "Process again" reuses the SAME
                    // handle-less reprocess the seal's auto-process runs.
                    reprocess = { dir, onProgress ->
                        // The StitchResult's NUMBERS are still deliberately
                        // discarded: the row re-reads the project from disk,
                        // so they come from the manifest the reprocess just
                        // wrote rather than from a value held in memory by a
                        // screen the operator may have left.
                        //
                        // ROUND 27 item 134(b): its VERDICT is not. A null
                        // result or `ran = false` is a failed run, and until
                        // this round that fact went nowhere — the chip cleared
                        // and the operator was told nothing.
                        val result = container.processingRepository.reprocessD6(
                            lscanDir = dir,
                            refineSeams = true,
                            onProgress = onProgress,
                        )
                        if (result != null && result.ran) {
                            null
                        } else {
                            Wording.processFailed(
                                container.processingRepository.lastError()
                                    .ifBlank { "the scan's recorded data may be incomplete" },
                            )
                        }
                    },
                    // ROUND 22 item 90: a job the operator started must not die
                    // because they left the tab.
                    jobScope = container.containerScope,
                )
            }
        },
    )
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    // ── ROUND 23 item 104c: group export / share / delete ───────────────────
    //
    // A second ViewModel rather than more fields on ProjectsListViewModel: that
    // one also backs the project PICKER, and it deliberately knows nothing
    // about AppContainer, a Context or the export pipeline. This one owns the
    // selection, the per-row job progress and the batch run; both are read by
    // the same screen. Nothing in ui/nav changes — the route already has the
    // container.
    val batchViewModel: ProjectBatchViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ProjectBatchViewModel(
                    exporter = com.lidarscan.app.share.ProjectExporter(
                        repo = container.processingRepository,
                        store = container.projectStore,
                    ),
                    store = container.projectStore,
                    onProjectsChanged = viewModel::refresh,
                    log = { line ->
                        container.captureLog.log(com.lidarscan.app.debug.CaptureLog.TAG_EXPORT, line)
                    },
                    // ROUND 22 item 90: a batch the operator started must not
                    // die because they left the tab. Same scope, same reason as
                    // ProjectsListViewModel's reprocess.
                    jobScope = container.containerScope,
                )
            }
        },
    )
    val batchState by batchViewModel.uiState.collectAsStateWithLifecycle()

    // ── ROUND 24 item 108: the layout and the order, persisted ─────────────
    //
    // Read from the SAME settings flow every other surface reads (there is one
    // repository and one store), and written through it, so the preference is
    // remembered across a cold start rather than only across a rotation. A
    // preference that survives rotation but not a restart reads as the app
    // forgetting, which is worse than not offering it.
    val settings by container.settingsRepository.settings
        .collectAsStateWithLifecycle(initialValue = com.lidarscan.app.data.AppSettings())
    val prefsScope = androidx.compose.runtime.rememberCoroutineScope()

    // ROUND 5: the tab is a list + a preview, so the list has to be fresh when
    // the Capture tab has just created a project behind it.
    LaunchedEffect(Unit) { viewModel.refresh() }

    // A group delete removes the scans the selection still names. Pruning here
    // (rather than trusting the batch to have exited) keeps the two in step
    // when the list changes for any other reason too.
    LaunchedEffect(uiState.projects) {
        batchViewModel.pruneSelection(uiState.projects.map { it.id })
    }

    val batchContext = androidx.compose.ui.platform.LocalContext.current

    // ── ROUND 25 item 115: "Scan saved.", for a scan nobody pressed Stop on ──
    //
    // Leaving the Scan tab now seals whatever was recording, and that seal
    // deliberately does not navigate. So the report lands here, the first time
    // Projects is looked at afterwards, and is spent on the way in: an event,
    // not a state — leaving it set would put a stale "Scan saved." over the
    // list for the rest of the session. The refresh is deliberate too: the
    // sealed scan may have landed after this screen's own `refresh()` ran.
    var savedNotice by androidx.compose.runtime.remember {
        androidx.compose.runtime.mutableStateOf<String?>(null)
    }
    LaunchedEffect(Unit) {
        container.scanSavedNotice.collect { sealedId ->
            if (sealedId == null) return@collect
            container.scanSavedNotice.value = null
            savedNotice = sealedId
            viewModel.refresh()
        }
    }

    ProjectsListScreen(
        uiState = uiState,
        initialSelectedId = initialSelectedId,
        onSelectProject = onSelectProject,
        onOpenProject = onOpenProject,
        onOpenReview = onOpenReview,
        onNewScan = onNewScan,
        onSettings = onSettings,
        onDeleteProject = viewModel::delete,
        onExportProject = onExport,
        // ROUND 23 item 104b: ⋯ › Share opens the scan, where the Share button
        // now lives (item 104a) — the same door ⋯ › Export uses, for the same
        // reason: the format row and the sheet are both on Review.
        onShareProject = onShare,
        onReprocessProject = viewModel::reprocessProject,
        // ROUND 27 item 134(b): the failure line is dismissible.
        onDismissProcessFailure = viewModel::dismissProcessFailure,
        onOpenDetails = if (com.lidarscan.core.SimpleMode.showsProjectDetailHub(advanced)) onOpenProject else null,
        selection = batchState.selection,
        batchRunning = batchState.running,
        batchBusy = batchState.busy,
        batchMessage = batchState.message,
        onEnterSelection = batchViewModel::enterSelection,
        onToggleSelection = batchViewModel::toggle,
        onExitSelection = batchViewModel::exitSelection,
        onBatchAction = { action ->
            batchViewModel.runOnSelection(
                context = batchContext,
                action = action,
                // ROUND 24 item 108: the batch runs in the order the operator
                // is LOOKING at, not the store's. Sorting the list and then
                // exporting it in a different order is the kind of detail that
                // makes a group export impossible to check.
                listOrder = ProjectsView.sorted(uiState.projects, settings.projectsSort).map { it.id },
            )
        },
        layout = settings.projectsLayout,
        sort = settings.projectsSort,
        onLayoutChange = { next -> prefsScope.launch { container.settingsRepository.setProjectsLayout(next) } },
        onSortChange = { next -> prefsScope.launch { container.settingsRepository.setProjectsSort(next) } },
        savedNotice = savedNotice?.let { id ->
            uiState.projects.firstOrNull { it.id == id }?.manifest?.name
                // The id is a fallback, not a display name: if the scan is not
                // in the list yet, saying "Scan saved." with no name is still
                // true, and inventing one would not be.
                ?.let { name -> "${Wording.SCAN_SAVED} $name" }
                ?: Wording.SCAN_SAVED
        },
    )
}

/**
 * ROUND 28 item 162 (§D.5) — **66 rows that differ from each other.**
 *
 * The owner's screenshot of this tab is the review's worst finding by count:
 * 66 cards, ~140 dp each, carrying **198 identical chips** (`D6`, `QUICK SCAN`,
 * `GEOREF ✓` on every one), no thumbnail, a decorative chevron next to a `⋯`,
 * and a header line that truncated to `2 empt…`. Every card was the loudest
 * possible drawing of the facts all 66 scans share, and none of the facts that
 * separate them. Four changes, and they are all the same change:
 *
 *  1. **The thumbnail is back** (P1c), 56 dp at the row's leading edge.
 *     Reversing round 25 item 114 — the tile is the strongest differentiator
 *     the app has and `CloudThumbnail` never stopped rendering it.
 *  2. **The chip law** (§C.4): a chip is drawn only where a value deviates
 *     from the mode of the visible set. 198 chips become one or two.
 *  3. **A quality mark, right-aligned** (P1b) — see [ProjectRowGrade], which
 *     also documents the one thing that is honestly not knowable from disk.
 *  4. **Empty scans are shown** (P1j), not hidden behind a footnote that
 *     always truncated. Seven of the owner's seventy-four are empty.
 *
 * Plus §C.4's shape: a 72 dp [ScanRow] inside one [ScanRowCard] instead of a
 * 140 dp card, which is roughly twice as many scans per screen, and the
 * decorative chevron is gone (P1e — the row is tappable; the `⋯` stays; never
 * both).
 *
 * What did NOT change: a tap opens the scan (round 22 item 96), a long-press
 * enters selection (round 23 item 104c), and the ⋯ menu is still the four
 * things Simple mode's removed hub was used for.
 */
@Composable
fun ProjectsListScreen(
    uiState: ProjectsUiState,
    initialSelectedId: String? = null,
    onSelectProject: (String) -> Unit,
    onOpenProject: (String) -> Unit,
    onOpenReview: (String) -> Unit,
    onNewScan: () -> Unit,
    onSettings: () -> Unit,
    onDeleteProject: (String) -> Unit,
    onExportProject: (String) -> Unit = {},
    /** ROUND 23 item 104b — the ⋯ menu's new Share item. */
    onShareProject: (String) -> Unit = {},
    onReprocessProject: (String) -> Unit = {},
    onDismissProcessFailure: (String) -> Unit = {},
    onOpenDetails: ((String) -> Unit)? = null,
    // ── ROUND 23 item 104c: selection mode ──────────────────────────────────
    /** The state machine's current value — see [ProjectSelection] in :core. */
    selection: ProjectSelection = ProjectSelection.EMPTY,
    /** project id → 0..1 for a BATCH job running against that row. */
    batchRunning: Map<String, Float> = emptyMap(),
    /** True from the first job of a batch to the last. */
    batchBusy: Boolean = false,
    /** [com.lidarscan.core.projects.BatchReport.summary] once a run finishes. */
    batchMessage: String? = null,
    /**
     * ROUND 25 item 115 — a scan that sealed because the operator left the
     * Scan tab. Null the rest of the time, which is nearly always.
     */
    savedNotice: String? = null,
    onEnterSelection: (String) -> Unit = {},
    onToggleSelection: (String) -> Unit = {},
    onExitSelection: () -> Unit = {},
    onBatchAction: (BatchAction) -> Unit = {},
    // ── ROUND 24 item 108: gallery/list + sort ──────────────────────────────
    /** Which layout to draw. Persisted — see [ProjectsLayout]. */
    layout: ProjectsLayout = ProjectsLayout.DEFAULT,
    /** Which order to draw it in. Persisted — see [ProjectSort]. */
    sort: ProjectSort = ProjectSort.DEFAULT,
    onLayoutChange: (ProjectsLayout) -> Unit = {},
    onSortChange: (ProjectSort) -> Unit = {},
) {
    var selectedId by rememberSaveable { mutableStateOf<String?>(null) }
    // The group Delete confirm. ROUND 23 item 104c is explicit that moving
    // delete out of the long-press must not become an accidental-delete
    // regression: the dialog is the same one the row's ⋯ › Delete opens, it
    // just names a count instead of a scan.
    var showBatchDeleteConfirm by remember { mutableStateOf(false) }
    // ROUND 8 (item 31): adopt the just-sealed scan ONCE, keyed on the id.
    // Keyed rather than run on every composition because the user must stay in
    // charge afterwards: collapsing the row and having it spring back open on
    // the next recomposition would be worse than not highlighting it at all.
    // `LaunchedEffect(id)` also means returning to this tab later does not
    // re-select a scan the operator has since dismissed.
    LaunchedEffect(initialSelectedId) {
        if (initialSelectedId != null) selectedId = initialSelectedId
    }
    // ROUND 24 item 108: one sort, memoised on the two things it depends on.
    // Sorting inside the `items {}` lambda would re-sort on every scroll frame.
    val shown = remember(uiState.projects, sort) { ProjectsView.sorted(uiState.projects, sort) }
    // ROUND 28 item 149 — the norms of the set being displayed, computed once
    // per list rather than per row. This is the whole of the chip law's cost.
    val norms = remember(shown) { RowNorms.of(shown) }

    Column(
        Modifier
            .fillMaxSize()
            .background(ScanColors.page)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        HeroHeader(
            title = Wording.APP_NAME,
            subtitle = aggregateLine(uiState.projects),
            trailing = {
                AvatarButton(
                    icon = Icons.Filled.Person,
                    // ROUND 24 items 107 + 109: this used to say "Settings",
                    // which was a person-shaped button opening a tab that was
                    // already in the bar below it — and which collided the
                    // moment item 107 gave the Settings TAB its name back as a
                    // content description. It opens the Profile page now, so
                    // it is called Profile: one name, one node, one door.
                    contentDescription = "Profile",
                    onClick = onSettings,
                    modifier = Modifier.testTag("projectsAvatar"),
                )
            },
        )

        // ROUND 23 item 104c: the selection bar. It appears only in selection
        // mode, carries the count and the three group actions, and its X is the
        // way out — a mode with no visible exit is the reason long-press modes
        // get a bad name.
        if (selection.isActive) {
            SelectionBar(
                selection = selection,
                busy = batchBusy,
                onClose = onExitSelection,
                onExport = { onBatchAction(BatchAction.EXPORT) },
                onShare = { onBatchAction(BatchAction.SHARE) },
                onDelete = { showBatchDeleteConfirm = true },
            )
        }
        // ROUND 25 item 115. Above the batch's line and below the hero: this
        // is the app finishing a sentence the operator started on another tab,
        // so it belongs where they will look first. Drawn in the semantic green
        // rather than the neutral hint colour — "your walk was saved" is good
        // news and the only good news on this screen.
        savedNotice?.let { notice ->
            Hint(
                notice,
                color = ScanColors.good,
                modifier = Modifier
                    .padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S1)
                    .testTag("scanSavedNotice"),
            )
        }

        // The batch's own last word. ROUND 7's rule over a set: a run of three
        // that produced two files says so, out loud, and says what to tap.
        batchMessage?.let { message ->
            Hint(
                message,
                color = ScanColors.inkMute,
                modifier = Modifier
                    .padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S1)
                    .testTag("batchMessage"),
            )
        }

        // ── ROUND 24 item 108: one compact control row ─────────────────────
        //
        // Above the list, below the hero, and only when there is a list to
        // control: a sort menu over an empty tab is a control for nothing, and
        // the empty state has one job (point at the Scan tab).
        if (!uiState.loading && uiState.projects.isNotEmpty()) {
            ProjectsControlRow(
                layout = layout,
                sort = sort,
                onLayoutChange = onLayoutChange,
                onSortChange = onSortChange,
            )
        }

        // Weighted so the list gets the height LEFT OVER under the hero. The
        // tab bar is level 0 and opaque now (§C.5), so it reserves its own
        // space and the list's only bottom inset is `TabBarClearance` —
        // finding P1h's guillotined last card was the floating bar overlapping
        // a list that had asked for the full screen height.
        Box(Modifier.fillMaxWidth().weight(1f)) {
            when {
                // §C.6: a skeleton, never a centred spinner. The list has a
                // known shape before it has content, so it can be drawn.
                uiState.loading -> ProjectsSkeleton()

                uiState.projects.isEmpty() -> EmptyProjectsState(onNewScan)

                layout == ProjectsLayout.GALLERY -> LazyVerticalGrid(
                    columns = GridCells.Fixed(ProjectsView.columns(layout)),
                    modifier = Modifier.fillMaxSize().testTag("projectsGallery"),
                    contentPadding = PaddingValues(
                        start = ScanDims.ScreenMargin,
                        end = ScanDims.ScreenMargin,
                        top = ScanDims.S1,
                        bottom = ScanDims.TabBarClearance,
                    ),
                    verticalArrangement = Arrangement.spacedBy(ScanDims.ItemGap),
                    horizontalArrangement = Arrangement.spacedBy(ScanDims.ItemGap),
                ) {
                    items(shown, key = { it.id }) { project ->
                        ProjectGalleryCard(
                            project = project,
                            deviation = norms.deviation(project.manifest),
                            selected = selectedId == project.id,
                            selecting = selection.isActive,
                            checked = selection.contains(project.id),
                            onClick = {
                                if (selection.isActive) {
                                    onToggleSelection(project.id)
                                } else {
                                    onSelectProject(project.id)
                                    onOpenReview(project.id)
                                }
                            },
                            onLongClick = { onEnterSelection(project.id) },
                        )
                    }
                    item(span = { GridItemSpan(maxLineSpan) }) { ListHint() }
                }

                // ── §C.4: THE ROW, and one card holding all of them ─────────
                //
                // One `ScanRowCard` in one lazy item rather than a card per
                // scan: hairlines between siblings is what a list of the same
                // kind of thing looks like, and 66 separately bordered slabs
                // with 14 dp between them is finding P1f (and most of P1g —
                // white cards at 1.08:1 on the page, held together by a
                // hairline they each drew for themselves).
                else -> LazyColumn(
                    modifier = Modifier.fillMaxSize().testTag("projectsList"),
                    contentPadding = PaddingValues(
                        start = ScanDims.ScreenMargin,
                        end = ScanDims.ScreenMargin,
                        top = ScanDims.S1,
                        bottom = ScanDims.TabBarClearance,
                    ),
                ) {
                    item(key = "rows") {
                        ScanRowCard(
                            rows = shown.map { project ->
                                {
                                    ProjectScanRow(
                                        project = project,
                                        deviation = norms.deviation(project.manifest),
                                        selected = selectedId == project.id,
                                        // ROUND 23 item 104c: a reprocess chip
                                        // and an export chip are the same chip.
                                        // Whichever job is running against this
                                        // row owns it; they cannot both run,
                                        // because the batch refuses to start a
                                        // second run and the reprocess refuses
                                        // a second run per id.
                                        progress = uiState.running[project.id]
                                            ?: batchRunning[project.id],
                                        progressLabel = if (uiState.running.containsKey(project.id)) {
                                            { percent -> Wording.fixingProgress(percent) }
                                        } else {
                                            { percent -> ProjectActionWording.exportingProgress(percent) }
                                        },
                                        failureNote = uiState.processFailures[project.id],
                                        onDismissFailure = { onDismissProcessFailure(project.id) },
                                        selecting = selection.isActive,
                                        checked = selection.contains(project.id),
                                        // ── ROUND 22 item 96: a tap OPENS ────
                                        //
                                        // ROUND 5 made a tap "select and preview
                                        // in place", which meant looking at a
                                        // scan properly took a tap, a read of
                                        // two quiet text buttons, and a second
                                        // tap on the right one. The row is the
                                        // scan, so tapping it opens it.
                                        //
                                        // ROUND 23 item 104c: **while selecting,
                                        // a tap picks.** That is the one mode in
                                        // which the row is not a door.
                                        onClick = {
                                            if (selection.isActive) {
                                                onToggleSelection(project.id)
                                            } else {
                                                onSelectProject(project.id)
                                                onOpenReview(project.id)
                                            }
                                        },
                                        // ROUND 23 item 104c: long-press ENTERS
                                        // selection mode and picks this row. It
                                        // used to open the delete confirm;
                                        // delete is in the ⋯ menu and in the
                                        // selection bar, so nothing lost a
                                        // route — a destructive action simply
                                        // stopped being the only thing a
                                        // long-press could mean.
                                        onLongClick = { onEnterSelection(project.id) },
                                        onExport = { onExportProject(project.id) },
                                        onShare = { onShareProject(project.id) },
                                        onReprocess = { onReprocessProject(project.id) },
                                        onOpenDetails = onOpenDetails?.let { open ->
                                            { open(project.id) }
                                        },
                                        onDelete = { onDeleteProject(project.id) },
                                    )
                                }
                            },
                        )
                    }
                    item(key = "hint") { ListHint() }
                }
            }
        }
    }

    if (showBatchDeleteConfirm) {
        AlertDialog(
            shape = RoundedCornerShape(ScanDims.DialogRadius),
            onDismissRequest = { showBatchDeleteConfirm = false },
            // The SAME dialog the single-row Delete opens — same title, same
            // body, same confirm word. Only the count in front of it is new.
            title = { Text("${Wording.DELETE_TITLE} (${selection.count})") },
            text = { Text(Wording.DELETE_BODY) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showBatchDeleteConfirm = false
                        onBatchAction(BatchAction.DELETE)
                    },
                    modifier = Modifier.testTag("batchDeleteConfirm"),
                ) { Text(ProjectActionWording.DELETE_ACTION) }
            },
            dismissButton = {
                TextButton(onClick = { showBatchDeleteConfirm = false }) { Text("Cancel") }
            },
        )
    }
}

// ── the chip law, as this screen applies it ─────────────────────────────────

/** True when this scan carries a coordinate reference system worth naming. */
internal val ProjectManifest.isGeoreferenced: Boolean
    get() = crsEpsg != null && crsEpsg != 0

/**
 * ROUND 28 item 162 / finding P1a — **the one thing a row's chip may say.**
 *
 * Four candidate deviations, in the order the operator would want to be told
 * about them, and **at most one is drawn**. That cap is deliberate: three chips
 * per row is the defect this item exists to remove, and a row that deviated in
 * two ways would quietly grow the chip row back. The rest of a scan's metadata
 * is one tap away on Review, which is where a scan gets read rather than
 * scanned past.
 */
internal enum class RowDeviation { RECOVERED, SENSOR, PROFILE, GEOREF_PRESENT, GEOREF_MISSING }

/**
 * The modal sensor / profile / georeferencing of the **loaded, visible** list.
 *
 * The mode, not a hard-coded default: in a fleet of 66 D6 scans the `D6` chip
 * says nothing and must vanish, and in a mixed fleet it says something and must
 * appear. Neither case needs a rule written about it. See
 * [com.lidarscan.app.ui.components.modalValue], which also decides that a tie
 * is not a norm.
 */
internal data class RowNorms(
    val sensor: SensorType?,
    val profile: WorkflowProfile?,
    val georeferenced: Boolean?,
) {
    fun deviation(manifest: ProjectManifest): RowDeviation? = when {
        // ROUND 6 (owner item 20): a capture whose app-side metadata was
        // destroyed by the pre-0.3.0 `manifest.json` collision with the
        // engine's own container manifest, and which `FileProjectStore` rebuilt
        // so it is listable again. Its POINTS are intact; its name/sensor/
        // profile are a reconstruction, and saying so is the difference between
        // honest recovery and a quiet lie. It outranks the others because it is
        // the one that says "do not trust the rest of this row".
        manifest.recovered -> RowDeviation.RECOVERED
        deviates(manifest.sensor, sensor) -> RowDeviation.SENSOR
        deviates(manifest.profile, profile) -> RowDeviation.PROFILE
        deviates(manifest.isGeoreferenced, georeferenced) ->
            if (manifest.isGeoreferenced) RowDeviation.GEOREF_PRESENT else RowDeviation.GEOREF_MISSING
        else -> null
    }

    companion object {
        fun of(projects: List<Project>): RowNorms = ofManifests(projects.map { it.manifest })

        /**
         * The manifests are the whole input, so this is where the law is
         * actually written — and it is what the JVM test drives, because a
         * fleet of 66 scans should be assertable without 66 directories on a
         * disk.
         */
        fun ofManifests(manifests: List<ProjectManifest>): RowNorms = RowNorms(
            sensor = modalValue(manifests.map { it.sensor }),
            profile = modalValue(manifests.map { it.profile }),
            georeferenced = modalValue(manifests.map { it.isGeoreferenced }),
        )
    }
}

/** The deviation chip, in the semantic §C.3 gives it. Read-only, never a control. */
@Composable
private fun DeviationChip(manifest: ProjectManifest, deviation: RowDeviation) {
    when (deviation) {
        RowDeviation.RECOVERED ->
            ScanChip(text = "RECOVERED", color = ScanColors.warn, showDot = true)
        // ROUND 25 item 119: an exhaustive lookup, not an `else`. The `else`
        // painted the new STL-27L in the D6's teal.
        RowDeviation.SENSOR -> ScanChip(
            text = manifest.sensor.badgeLabel.uppercase(),
            color = ScanColors.current.sensor(manifest.sensor),
            showDot = true,
        )
        // A workflow profile is a fact, not a semantic state, so it is ink-mute
        // — the accent law allows orange twice per screen and both are spent.
        RowDeviation.PROFILE -> ScanChip(
            text = manifest.profile.displayName.uppercase(),
            color = ScanColors.inkMute,
            showDot = true,
        )
        RowDeviation.GEOREF_PRESENT ->
            ScanChip(text = "GEOREF", color = ScanColors.good, showDot = true)
        // Amber, not red: nothing failed, but in a library where everything
        // else carries a CRS this one will not line up with the others, and
        // that is worth noticing before an export rather than after.
        RowDeviation.GEOREF_MISSING ->
            ScanChip(text = "NO GEOREF", color = ScanColors.warn, showDot = true)
    }
}

/**
 * ROUND 28 item 162 / finding P1b — the quality mark: a semantic dot and a
 * Meta Caps code, right-aligned.
 *
 * `FAIR` resolves to ink-mute rather than amber on purpose (see
 * `ScanColorScheme.grade`): FAIR is the norm in this fleet, and a wall of amber
 * is the same defect as a wall of chips.
 */
@Composable
private fun GradeMark(code: String) {
    val tint = ScanColors.current.grade(code)
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.S1),
        modifier = Modifier.testTag("projectRowGrade"),
    ) {
        StatusDot(tint)
        Text(text = code, style = ScanMetaCaps, color = tint, maxLines = 1)
    }
}

// ── the row ─────────────────────────────────────────────────────────────────

/**
 * §C.4's ROW, carrying one scan.
 *
 * ```
 * ┌──┐ Scan-085-2026-08-21-1803              ⋯
 * │▩ │ 46.5 K pts · Aug 21            ● FAIR
 * └──┘
 * ```
 *
 * 72 dp instead of the ~140 dp `ProjectCard` it replaces (P1f), a 56 dp cloud
 * tile at the leading edge (P1c), no chevron (P1e — the row is tappable and the
 * `⋯` is its only affordance), and no chip unless this scan actually differs
 * from the set (P1a).
 *
 * The progress and failure lines hang **below** the row inside the same slot
 * rather than inside it: a 72 dp row has no third line, and a job's progress is
 * a temporary statement about the row, not part of what the scan is.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ProjectScanRow(
    project: Project,
    deviation: RowDeviation?,
    selected: Boolean,
    /** ROUND 22 item 96: 0..1 while a job started from this row runs; null otherwise. */
    progress: Float? = null,
    /** ROUND 23 item 104c: what the progress line says — a reprocess and an export differ. */
    progressLabel: (Int) -> String = { percent -> Wording.fixingProgress(percent) },
    /**
     * ROUND 27 item 134(b) — why this row's last "Process again" failed, or
     * null.
     *
     * A run that fails must leave something behind. Until that round the chip
     * simply disappeared and the operator was returned to an unchanged card,
     * which reads as "nothing happened" and is indistinguishable from a tap
     * that never registered.
     */
    failureNote: String? = null,
    onDismissFailure: () -> Unit = {},
    /** ROUND 23 item 104c: the list is in selection mode, so a tap picks. */
    selecting: Boolean = false,
    /** ROUND 23 item 104c: this row is in the selection. */
    checked: Boolean = false,
    onClick: () -> Unit,
    onLongClick: () -> Unit = {},
    onExport: () -> Unit,
    onShare: () -> Unit = {},
    onReprocess: () -> Unit,
    /** ROUND 22 item 97: non-null only with Advanced on. */
    onOpenDetails: (() -> Unit)? = null,
    onDelete: () -> Unit,
) {
    var showDeleteConfirm by remember { mutableStateOf(false) }
    val manifest = project.manifest
    val grade = ProjectRowGrade.of(manifest)

    Column {
        ScanRow(
            modifier = Modifier
                // A picked row is a 12 % primary wash rather than a 2 dp
                // border: rows share their edges with their neighbours, so a
                // border on one is a border between two, and "which of these am
                // I about to export" still has to be answerable at arm's
                // length.
                .background(
                    when {
                        checked -> ScanColors.primary.copy(alpha = 0.12f)
                        selected -> ScanColors.primary.copy(alpha = 0.06f)
                        else -> androidx.compose.ui.graphics.Color.Transparent
                    },
                )
                .combinedClickable(
                    onClick = onClick,
                    onLongClick = onLongClick,
                    onLongClickLabel = ProjectActionWording.SELECT_LABEL,
                )
                .testTag(
                    when {
                        checked -> "projectCardChecked"
                        selected -> "projectCardSelected"
                        else -> "projectCard"
                    },
                ),
            minHeight = ScanDims.RowWithThumb,
            title = manifest.name,
            // ROUND 28 item 162 (P1j): an empty scan is still a scan the
            // operator has to find and delete, so it is in the list — but its
            // name is ink-mute, because there is nothing in it to open.
            titleColor = if (manifest.isEmptyScan) ScanColors.inkMute else null,
            detail = metaLine(project),
            leading = {
                if (selecting) {
                    Icon(
                        if (checked) Icons.Filled.CheckCircle else Icons.Outlined.RadioButtonUnchecked,
                        contentDescription = null,
                        tint = if (checked) ScanColors.primaryInk else ScanColors.inkFaint,
                        modifier = Modifier
                            .size(ScanDims.S6)
                            .testTag(if (checked) "projectCardTick" else "projectCardUntick"),
                    )
                }
                ProjectThumbnail(
                    project = project,
                    cornerRadius = ScanDims.S2,
                    // Tagged so the emulator can assert item 162's reversal of
                    // item 114: the list must now draw one of these per row.
                    modifier = Modifier.size(ScanDims.Thumb).testTag("projectPreview"),
                )
            },
            trailing = {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(ScanDims.S2),
                ) {
                    deviation?.let { DeviationChip(manifest, it) }
                    grade?.let { GradeMark(it) }
                    RowMenu(
                        onExport = onExport,
                        onShare = onShare,
                        onReprocess = onReprocess,
                        onOpenDetails = onOpenDetails,
                        onDelete = { showDeleteConfirm = true },
                    )
                }
            },
        )

        // ROUND 22 item 96: a running job is visible on the row, because Simple
        // mode has no Jobs screen to go and look at. §C.6: a determinate load is
        // an inline 4 dp bar under a Meta label naming the stage, never a
        // spinner where a percentage exists.
        if (progress != null) {
            Column(
                Modifier.fillMaxWidth().padding(
                    start = ScanDims.CardPadding,
                    end = ScanDims.CardPadding,
                    bottom = ScanDims.S2,
                ),
            ) {
                Text(
                    progressLabel((progress * 100).toInt()),
                    style = ScanMeta,
                    color = ScanColors.inkMute,
                    modifier = Modifier.testTag("projectCardJobChip"),
                )
                Spacer(Modifier.height(ScanDims.S1))
                LinearProgressIndicator(
                    progress = { progress },
                    modifier = Modifier.fillMaxWidth().height(ScanDims.S1),
                    color = ScanColors.primary,
                    trackColor = ScanColors.trough,
                )
            }
        }

        // ROUND 27 item 134(b): and a job that FAILED is a line on the row, for
        // exactly the same reason. Tappable away, because it is an answer to
        // something the operator did and not a permanent state of the scan.
        if (failureNote != null) {
            Text(
                failureNote,
                style = ScanBody,
                color = ScanColors.bad,
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onDismissFailure)
                    .padding(
                        start = ScanDims.CardPadding,
                        end = ScanDims.CardPadding,
                        bottom = ScanDims.S2,
                    )
                    .testTag("projectCardProcessFailure"),
            )
        }
    }

    if (showDeleteConfirm) {
        DeleteScanDialog(
            onDismiss = { showDeleteConfirm = false },
            onConfirm = {
                showDeleteConfirm = false
                ProjectPreviewCache.invalidate(project.id)
                onDelete()
            },
        )
    }
}

/**
 * ── ROUND 22 item 96: the per-row ⋯ menu ────────────────────────────────────
 *
 * Export, Share, Process again and Delete — the things Simple mode's removed
 * "Details, jobs & export" hub was actually used for, on the row they are
 * about. Delete was a long-press from ROUND 5, which is a gesture the list hint
 * had to TEACH; a menu is found.
 */
@Composable
private fun RowMenu(
    onExport: () -> Unit,
    onShare: () -> Unit,
    onReprocess: () -> Unit,
    onOpenDetails: (() -> Unit)?,
    onDelete: () -> Unit,
) {
    var menuOpen by remember { mutableStateOf(false) }
    Box {
        ScanIconButton(
            icon = Icons.Filled.MoreVert,
            contentDescription = "More actions",
            onClick = { menuOpen = true },
            modifier = Modifier.testTag("projectCardMenu"),
        )
        DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
            DropdownMenuItem(
                text = { Text(Wording.CARD_MENU_EXPORT) },
                onClick = { menuOpen = false; onExport() },
                modifier = Modifier.testTag("cardMenuExport"),
            )
            // ROUND 23 item 104b: Share, next to Export, because the owner's
            // report is that BOTH vanished — and a menu that offers only "save
            // it where you cannot browse to it" is half an answer.
            DropdownMenuItem(
                text = { Text(ProjectActionWording.SHARE_ACTION) },
                onClick = { menuOpen = false; onShare() },
                modifier = Modifier.testTag("cardMenuShare"),
            )
            DropdownMenuItem(
                text = { Text(Wording.CARD_MENU_REPROCESS) },
                onClick = { menuOpen = false; onReprocess() },
                modifier = Modifier.testTag("cardMenuReprocess"),
            )
            onOpenDetails?.let { open ->
                DropdownMenuItem(
                    text = { Text("Details") },
                    onClick = { menuOpen = false; open() },
                    modifier = Modifier.testTag("cardMenuDetails"),
                )
            }
            DropdownMenuItem(
                text = { Text(Wording.CARD_MENU_DELETE) },
                onClick = { menuOpen = false; onDelete() },
                modifier = Modifier.testTag("cardMenuDelete"),
            )
        }
    }
}

/**
 * ROUND 22 item 98. Was: 'Delete "<name>"?' over "This permanently deletes the
 * .lscan project directory, including any captured streams. This can't be
 * undone." — the on-disk format and the word "streams" are facts about the
 * implementation, not about what the operator is about to lose.
 *
 * ROUND 16 item 61: dialogs inherited the theme's pill radius too.
 */
@Composable
private fun DeleteScanDialog(onDismiss: () -> Unit, onConfirm: () -> Unit) {
    AlertDialog(
        shape = RoundedCornerShape(ScanDims.DialogRadius),
        onDismissRequest = onDismiss,
        title = { Text(Wording.DELETE_TITLE) },
        text = { Text(Wording.DELETE_BODY) },
        confirmButton = {
            TextButton(onClick = onConfirm) { Text(ProjectActionWording.DELETE_ACTION) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// ── the gallery ─────────────────────────────────────────────────────────────

/**
 * ROUND 24 item 108's second layout, kept as **a card with a picture** — the
 * one thing §C.4 still allows a card to be for.
 *
 * ROUND 28 item 162 rebuilds it on the new tokens (`ScanCard`, the type scale,
 * `ScanDims`) and applies the same chip law and the same grade mark the row
 * uses, so the two layouts say the same things about a scan and differ only in
 * how much of the picture they show. The thumbnail is sized by aspect ratio
 * rather than a fixed height: a 2-column card is half as wide, and a height in
 * dp made it a letterbox on a wide phone and a square on a narrow one.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ProjectGalleryCard(
    project: Project,
    deviation: RowDeviation?,
    selected: Boolean,
    selecting: Boolean,
    checked: Boolean,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
) {
    val manifest = project.manifest
    val grade = ProjectRowGrade.of(manifest)
    ScanCard(
        modifier = Modifier
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongClick,
                onLongClickLabel = ProjectActionWording.SELECT_LABEL,
            )
            .testTag(
                when {
                    checked -> "projectCardChecked"
                    selected -> "projectCardSelected"
                    else -> "projectCard"
                },
            ),
        borderColor = if (checked || selected) ScanColors.primaryInk else null,
        contentPadding = PaddingValues(ScanDims.S3),
    ) {
        Box {
            ProjectThumbnail(
                project = project,
                modifier = Modifier
                    .fillMaxWidth()
                    // ROUND 5 (item 8): the preview IS the selection — a
                    // selected card gives its cloud more height, enough to read
                    // the shape of a scan, instead of opening another screen.
                    .aspectRatio(if (selected) 1f else 1.5f)
                    .testTag("projectPreview"),
            )
            if (selecting) {
                Icon(
                    if (checked) Icons.Filled.CheckCircle else Icons.Outlined.RadioButtonUnchecked,
                    contentDescription = null,
                    tint = if (checked) ScanColors.primaryInk else ScanColors.inkFaint,
                    modifier = Modifier
                        .padding(ScanDims.S2)
                        .size(ScanDims.S6)
                        .testTag(if (checked) "projectCardTick" else "projectCardUntick"),
                )
            }
        }
        Spacer(Modifier.height(ScanDims.S2))
        Text(
            text = manifest.name,
            style = ScanTitle,
            color = if (manifest.isEmptyScan) ScanColors.inkMute else ScanColors.ink,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            text = metaLine(project),
            style = ScanMeta,
            color = ScanColors.inkMute,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        if (deviation != null || grade != null) {
            Spacer(Modifier.height(ScanDims.S1))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(ScanDims.S2),
            ) {
                deviation?.let { DeviationChip(manifest, it) }
                grade?.let { GradeMark(it) }
            }
        }
    }
}

// ── chrome ──────────────────────────────────────────────────────────────────

/**
 * ROUND 23 item 104c — **the selection bar.**
 *
 * Count on the left, the three group actions on the right, and an X that is the
 * only way out of the mode. Delete is last and carries the `bad` tint: it is
 * the one action here that cannot be undone, and it is the one that used to be
 * what a long-press did by itself.
 *
 * The actions are disabled while a batch runs rather than hidden — a bar whose
 * buttons vanish mid-run reads as the app having lost the selection.
 */
@Composable
private fun SelectionBar(
    selection: ProjectSelection,
    busy: Boolean,
    onClose: () -> Unit,
    onExport: () -> Unit,
    onShare: () -> Unit,
    onDelete: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S1)
            .background(ScanColors.card, RoundedCornerShape(ScanDims.CardRadius))
            .padding(horizontal = ScanDims.S1)
            .testTag("selectionBar"),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.S1),
    ) {
        ScanIconButton(
            icon = Icons.Filled.Close,
            contentDescription = ProjectActionWording.SELECTION_CLOSE,
            onClick = onClose,
            modifier = Modifier.testTag("selectionClose"),
        )
        Text(
            text = selection.title(),
            style = ScanTitle,
            color = ScanColors.ink,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).testTag("selectionCount"),
        )
        ScanIconButton(
            icon = Icons.Filled.IosShare,
            contentDescription = Wording.CARD_MENU_EXPORT,
            onClick = onExport,
            enabled = !busy,
            modifier = Modifier.testTag("selectionExport"),
        )
        ScanIconButton(
            icon = Icons.Filled.Share,
            contentDescription = ProjectActionWording.SHARE_ACTION,
            onClick = onShare,
            enabled = !busy,
            modifier = Modifier.testTag("selectionShare"),
        )
        ScanIconButton(
            icon = Icons.Filled.Delete,
            contentDescription = ProjectActionWording.DELETE_ACTION,
            onClick = onDelete,
            enabled = !busy,
            tint = ScanColors.bad,
            modifier = Modifier.testTag("selectionDelete"),
        )
    }
}

/**
 * ROUND 24 item 108 — **one row, two controls, no menu bar.**
 *
 * ROUND 28 finding P1i: the two controls were not the same kind of thing. A
 * bare unlabelled grid glyph sat on the left in a 48 dp `IconButton`, an
 * icon+label sat on the right in an 8 dp-padded `Row`, and the two texts did
 * not share a baseline. They are now **one composable used twice** — icon,
 * gap, label, one height, one alignment — which is the only way two controls
 * end up on one baseline and stay there.
 *
 * The two decisions round 24 argued and this round keeps:
 *
 *  * **The layout is a toggle, not a menu.** There are two layouts, and the
 *    label names the layout you would GET rather than the one you are in —
 *    which is what a toggle means everywhere else on a phone.
 *  * **The sort shows its current value.** "why is this scan at the top" must
 *    be answerable without opening anything.
 */
@Composable
private fun ProjectsControlRow(
    layout: ProjectsLayout,
    sort: ProjectSort,
    onLayoutChange: (ProjectsLayout) -> Unit,
    onSortChange: (ProjectSort) -> Unit,
) {
    var sortMenuOpen by remember { mutableStateOf(false) }
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = ScanDims.S2)
            .testTag("projectsControlRow"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ControlButton(
            icon = if (layout == ProjectsLayout.GALLERY) {
                Icons.AutoMirrored.Filled.ViewList
            } else {
                Icons.Filled.GridView
            },
            label = ProjectsView.layoutActionLabel(layout),
            onClick = { onLayoutChange(ProjectsView.toggled(layout)) },
            modifier = Modifier.testTag("projectsLayoutToggle"),
        )
        Spacer(Modifier.weight(1f))
        Box {
            ControlButton(
                icon = Icons.Filled.Sort,
                label = sort.label,
                contentDescription = ProjectsView.SORT_LABEL,
                onClick = { sortMenuOpen = true },
                labelTestTag = "projectsSortValue",
                modifier = Modifier.testTag("projectsSortButton"),
            )
            DropdownMenu(expanded = sortMenuOpen, onDismissRequest = { sortMenuOpen = false }) {
                ProjectSort.entries.forEach { option ->
                    DropdownMenuItem(
                        text = { Text(option.label) },
                        onClick = { sortMenuOpen = false; onSortChange(option) },
                        modifier = Modifier.testTag("projectsSort_${option.name}"),
                    )
                }
            }
        }
    }
}

/** One control on the layout/sort row: icon, gap, label, 48 dp, one baseline. */
@Composable
private fun ControlButton(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    label: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    contentDescription: String? = null,
    labelTestTag: String? = null,
) {
    Row(
        modifier
            .defaultMinSize(minHeight = ScanDims.Touch)
            .clickable(onClick = onClick)
            .padding(horizontal = ScanDims.S2),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
    ) {
        Icon(
            icon,
            contentDescription = contentDescription ?: label,
            tint = ScanColors.inkMute,
            modifier = Modifier.size(ScanDims.S6),
        )
        Text(
            label,
            style = ScanBody,
            color = ScanColors.inkMute,
            maxLines = 1,
            modifier = if (labelTestTag != null) Modifier.testTag(labelTestTag) else Modifier,
        )
    }
}

/**
 * §C.6's indeterminate load: skeleton rows at 12 % ink, never a centred
 * spinner. The list's shape is known before its content is, so it is drawn —
 * and the screen does not jump when the projects arrive.
 */
@Composable
private fun ProjectsSkeleton() {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = ScanDims.ScreenMargin, vertical = ScanDims.S1)
            .testTag("projectsSkeleton"),
        // `Hair` is a border width, never a spacing value (§C.1), so the
        // skeleton separates its rows on the 4 dp grid like everything else.
        verticalArrangement = Arrangement.spacedBy(ScanDims.S1),
    ) {
        repeat(6) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(ScanDims.RowWithThumb)
                    .background(
                        ScanColors.ink.copy(alpha = 0.12f),
                        RoundedCornerShape(ScanDims.S2),
                    ),
            )
        }
    }
}

/**
 * ROUND 22 item 98 + ROUND 23 item 104c: what a tap does, and the one mode a
 * gesture alone cannot advertise.
 *
 * ROUND 28 item 162 drops the third line ("Some empty scans are hidden.") —
 * nothing is hidden any more, so there is nothing to footnote.
 */
@Composable
private fun ListHint() {
    Hint(
        Wording.PROJECTS_LIST_HINT + "\n" + ProjectActionWording.SELECTION_HINT,
        color = ScanColors.inkMute,
        modifier = Modifier
            .padding(horizontal = ScanDims.S1, vertical = ScanDims.S3)
            .testTag("projectsListHint"),
    )
}

/**
 * ROUND 28 item 151 — **ONE clause.**
 *
 * This built four — "66 projects · 65 georeferenced · 8.1 M points · 2 empty
 * hidden" — and `HeroHeader` renders its subtitle at `maxLines = 1, overflow =
 * Ellipsis`. That is not "might truncate on a small phone"; it is guaranteed
 * truncation on every device that exists, and what the owner saw was
 * `2 empt…` (finding P1d).
 *
 * The other three clauses are not deleted, they are *relocated*: item 165 moves
 * the breakdown to Profile's "This phone" table, which already had SCANS and
 * STORAGE rows waiting for exactly this. A table has room for four facts; a
 * one-line subtitle has room for one.
 *
 * The fourth clause is gone outright — item 162 stops hiding empty scans, so
 * there is nothing left to footnote.
 */
private fun aggregateLine(projects: List<Project>): String {
    if (projects.isEmpty()) return Wording.PROJECTS_EMPTY_TITLE
    return "${projects.size} scan${if (projects.size == 1) "" else "s"}"
}

/**
 * The row's second line: points, then the date.
 *
 * ROUND 28 item 150: this was the CORRECT adaptive formatter, and the Review
 * header twelve files away had a broken one. It moved to `PointCountFormat` in
 * `:core` so there is one of it, and item 162 gives an empty scan its own words
 * (`Empty — no points`) rather than printing `0 pts`.
 *
 * Item 162 also drops the `EPSG 32650` clause that used to end this line. It
 * was the same value on 65 of the owner's 66 rows — the chip law's definition
 * of noise — and the one scan that lacks it now says so with a chip, which is
 * the only place that fact is worth a pixel.
 */
private fun metaLine(project: Project): String {
    val m = project.manifest
    val points = PointCountFormat.rowClause(m.pointCountEstimate)
    val created = com.lidarscan.app.ui.common.formatCreatedDate(m.createdAtEpochMillis)
    return "$points · $created"
}

/**
 * §C.6's empty state, one way everywhere: a 32 dp ink-faint icon, a ≤6-word
 * title, a ≤12-word body and one Primary naming the fix. **No card.**
 *
 * ROUND 28 item 162 drops the "Some empty scans are hidden." note this used to
 * carry: with the filter gone, "No scans yet" cannot be a lie on a phone that
 * holds nothing but strays — there is no such phone any more, because the
 * strays are in the list.
 */
@Composable
private fun EmptyProjectsState(onNewScan: () -> Unit) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        ScanEmptyState(
            icon = Icons.Outlined.Layers,
            title = Wording.PROJECTS_EMPTY_TITLE,
            // ROUND 22 item 98. Was 26 words naming two products and a tab.
            body = Wording.PROJECTS_EMPTY_HINT,
            action = {
                // The one exception to "Projects never creates a scan": with no
                // projects at all, a tab that only says "go somewhere else" is
                // a dead end, so this is a shortcut TO the Scan tab, not a
                // second way to create a project.
                PrimaryPill(
                    text = Wording.PROJECTS_EMPTY_ACTION,
                    icon = Icons.Filled.Add,
                    onClick = onNewScan,
                    modifier = Modifier.testTag("newScanButton"),
                )
            },
        )
    }
}
