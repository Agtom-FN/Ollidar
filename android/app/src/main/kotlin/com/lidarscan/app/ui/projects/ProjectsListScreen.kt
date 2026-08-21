package com.lidarscan.app.ui.projects

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.automirrored.filled.ViewList
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.IosShare
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.GridView
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Sort
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.outlined.RadioButtonUnchecked
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.IconButton
import androidx.compose.material3.Icon
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.AvatarButton
import com.lidarscan.app.ui.components.HeroHeader
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.theme.sensorBadgeColor
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.app.ui.theme.PoseBlue
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.app.ui.theme.SemGood
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.core.Wording
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.projects.BatchAction
import com.lidarscan.core.projects.ProjectActionWording
import com.lidarscan.core.projects.ProjectSelection
import com.lidarscan.core.projects.ProjectSort
import com.lidarscan.core.projects.ProjectsLayout
import com.lidarscan.core.projects.ProjectsView
import com.lidarscan.core.store.Project
import kotlinx.coroutines.flow.first
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
     * ROUND 22 item 96 — the card's ⋯ › Export. Export lives on the Review
     * screen now (the "Details, jobs & export" hub is gone from Simple mode),
     * so this opens Review with its export row already unfolded rather than
     * duplicating the export UI on a card.
     */
    onExport: (String) -> Unit = onOpenReview,
    /**
     * ROUND 23 item 104b — the card's ⋯ › Share. Same destination as Export:
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
                    // ROUND 9 (owner item 33): the scan library hides 0-point
                    // strays by default. Settings › Scans has the switch and a
                    // one-tap cleanup that deletes them for good.
                    keepEmptyScans = {
                        container.settingsRepository.settings.first().keepEmptyScans
                    },
                    // ROUND 22 item 96: "Process again" reuses the SAME
                    // handle-less reprocess the seal's auto-process runs.
                    reprocess = { dir, onProgress ->
                        // The StitchResult is deliberately discarded: the card
                        // shows progress and then re-reads the project from
                        // disk, so the numbers come from the manifest the
                        // reprocess just wrote rather than from a value held in
                        // memory by a screen the operator may have left.
                        container.processingRepository.reprocessD6(
                            lscanDir = dir,
                            refineSeams = true,
                            onProgress = onProgress,
                        )
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
    // selection, the per-card job progress and the batch run; both are read by
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
                ?.let { name -> "${'$'}{Wording.SCAN_SAVED} ${'$'}name" }
                ?: Wording.SCAN_SAVED
        },
    )
}

/**
 * ROUND 5 (item 8): the Projects tab is **the list of projects plus a preview of
 * the selected scan**, and nothing else.
 *
 * What that changed: tapping a card no longer navigates away — it *selects*, and
 * the card expands into an inline preview (a bigger cloud, the capture's own
 * numbers) right where it sits. The "New scan" pill is gone: creating scans is
 * the Capture tab's only job now (item 8), so the empty state points there rather
 * than duplicating it.
 *
 * Two quiet doors survive inside the selected card, and they are deliberate: the
 * **viewer** (Review) is the full-fidelity version of the preview this tab is for,
 * and **details** is where processing, export, calibration and merge live. Both
 * are about a scan that already exists; neither creates one.
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
    onOpenDetails: ((String) -> Unit)? = null,
    // ── ROUND 23 item 104c: selection mode ──────────────────────────────────
    /** The state machine's current value — see [ProjectSelection] in :core. */
    selection: ProjectSelection = ProjectSelection.EMPTY,
    /** project id → 0..1 for a BATCH job running against that card. */
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
    // regression: the dialog is the same one the card's ⋯ › Delete opens, it
    // just names a count instead of a scan.
    var showBatchDeleteConfirm by remember { mutableStateOf(false) }
    // ROUND 8 (item 31): adopt the just-sealed scan ONCE, keyed on the id.
    // Keyed rather than run on every composition because the user must stay in
    // charge afterwards: collapsing the card and having it spring back open on
    // the next recomposition would be worse than not highlighting it at all.
    // `LaunchedEffect(id)` also means returning to this tab later does not
    // re-select a scan the operator has since dismissed.
    LaunchedEffect(initialSelectedId) {
        if (initialSelectedId != null) selectedId = initialSelectedId
    }
    // ROUND 24 item 108: one sort, memoised on the two things it depends on.
    // Sorting inside the `items {}` lambda would re-sort on every scroll frame.
    val shown = remember(uiState.projects, sort) { ProjectsView.sorted(uiState.projects, sort) }
    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        HeroHeader(
            title = Wording.APP_NAME,
            subtitle = aggregateLine(uiState.projects, uiState.hiddenEmptyCount),
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
                color = SemGood,
                modifier = Modifier
                    .padding(horizontal = 20.dp, vertical = 4.dp)
                    .testTag("scanSavedNotice"),
            )
        }

        // The batch's own last word. ROUND 7's rule over a set: a run of three
        // that produced two files says so, out loud, and says what to tap.
        batchMessage?.let { message ->
            Hint(
                message,
                color = InkFaint,
                modifier = Modifier
                    .padding(horizontal = 20.dp, vertical = 4.dp)
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

        // Weighted so the list gets the height LEFT OVER under the hero,
        // not the full screen height (which would run the last card under the
        // floating tab bar).
        Box(Modifier.fillMaxWidth().weight(1f)) {
            when {
                uiState.loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator(color = MaterialTheme.colorScheme.primary)
                }

                uiState.projects.isEmpty() -> EmptyProjectsState(uiState.hiddenEmptyCount, onNewScan)

                // ── ROUND 24 item 108: the SAME card, in one or two columns ──
                //
                // `LazyVerticalGrid` with `GridCells.Fixed(1)` is a LazyColumn
                // with extra words, so the two layouts are one call site and
                // one card: everything round 23 built — selection, the ⋯ menu,
                // the progress chip, tap-opens-viewer — works in the gallery
                // because it is not a second implementation of a card, it is
                // the same one with a shorter thumbnail.
                else -> LazyVerticalGrid(
                    columns = GridCells.Fixed(ProjectsView.columns(layout)),
                    modifier = Modifier.fillMaxSize().testTag(
                        if (layout == ProjectsLayout.GALLERY) "projectsGallery" else "projectsList",
                    ),
                    contentPadding = PaddingValues(
                        start = 16.dp,
                        end = 16.dp,
                        top = 2.dp,
                        bottom = ScanDims.TabBarClearance,
                    ),
                    verticalArrangement = Arrangement.spacedBy(14.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    items(shown, key = { it.id }) { project ->
                        ProjectCard(
                            gallery = layout == ProjectsLayout.GALLERY,
                            project = project,
                            selected = selectedId == project.id,
                            // ROUND 23 item 104c: a reprocess chip and an export
                            // chip are the same chip. Whichever job is running
                            // against this card owns it; they cannot both run,
                            // because the batch refuses to start a second run
                            // and the reprocess refuses a second run per id.
                            progress = uiState.running[project.id] ?: batchRunning[project.id],
                            progressLabel = if (uiState.running.containsKey(project.id)) {
                                { percent -> Wording.fixingProgress(percent) }
                            } else {
                                { percent -> ProjectActionWording.exportingProgress(percent) }
                            },
                            selecting = selection.isActive,
                            checked = selection.contains(project.id),
                            // ── ROUND 22 item 96: a tap OPENS THE SCAN ───────
                            //
                            // ROUND 5 made a tap "select and preview in place",
                            // which meant looking at a scan properly took a tap,
                            // a read of two quiet text buttons, and a second tap
                            // on the right one. The owner's simplification asks
                            // for the obvious thing: the card is the scan, so
                            // tapping it opens it. Selection still happens (the
                            // seal→Projects handoff and the Jobs tab both read
                            // it), it just no longer competes with opening.
                            //
                            // ROUND 23 item 104c: **while selecting, a tap
                            // picks.** That is the one mode in which the card is
                            // not a door, and it is the reason ProjectSelection
                            // refuses to start a selection from a tap.
                            onClick = {
                                if (selection.isActive) {
                                    onToggleSelection(project.id)
                                } else {
                                    onSelectProject(project.id)
                                    onOpenReview(project.id)
                                }
                            },
                            // ROUND 23 item 104c: long-press ENTERS selection
                            // mode and picks this card. It used to open the
                            // delete confirm; delete is now in the ⋯ menu (where
                            // it has been since round 22) AND in the selection
                            // bar, so nothing lost a route — a destructive
                            // action simply stopped being the only thing a
                            // long-press could mean.
                            onLongClick = { onEnterSelection(project.id) },
                            onExport = { onExportProject(project.id) },
                            onShare = { onShareProject(project.id) },
                            onReprocess = { onReprocessProject(project.id) },
                            onOpenDetails = onOpenDetails?.let { open -> { open(project.id) } },
                            onDelete = { onDeleteProject(project.id) },
                        )
                    }
                    item(span = { androidx.compose.foundation.lazy.grid.GridItemSpan(maxLineSpan) }) {
                        Spacer(Modifier.height(2.dp))
                        Hint(
                            // ROUND 22 item 98. Was: "Tap a scan to preview it ·
                            // long-press to delete · new scans start in the
                            // Capture tab." — three instructions in one line,
                            // two of which describe behaviour item 96 changed
                            // (a tap opens now; delete is in the ⋯ menu, where
                            // it is discovered rather than explained).
                            // ROUND 23 item 104c: and one line for the mode
                            // that a gesture alone cannot advertise.
                            Wording.PROJECTS_LIST_HINT + "\n" +
                                ProjectActionWording.SELECTION_HINT +
                                if (uiState.hiddenEmptyCount > 0) {
                                    "\n" + Wording.PROJECTS_EMPTY_HIDDEN
                                } else {
                                    ""
                                },
                            color = InkFaint,
                            modifier = Modifier.padding(horizontal = 4.dp).testTag("projectsListHint"),
                        )
                    }
                }
            }
        }
    }

    if (showBatchDeleteConfirm) {
        AlertDialog(
            shape = RoundedCornerShape(ScanDims.DialogRadius),
            onDismissRequest = { showBatchDeleteConfirm = false },
            // The SAME dialog the single-card Delete opens — same title, same
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

/**
 * ROUND 23 item 104c — **the selection bar.**
 *
 * Count on the left, the three group actions on the right, and an X that is the
 * only way out of the mode. Delete is last and carries the theme's error tint:
 * it is the one action here that cannot be undone, and it is the one that used
 * to be what a long-press did by itself.
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
            .padding(horizontal = 12.dp, vertical = 4.dp)
            .background(MaterialTheme.colorScheme.surfaceContainer, RoundedCornerShape(ScanDims.CardRadius))
            .padding(horizontal = 6.dp, vertical = 4.dp)
            .testTag("selectionBar"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconButton(onClick = onClose, modifier = Modifier.testTag("selectionClose")) {
            Icon(
                Icons.Filled.Close,
                contentDescription = ProjectActionWording.SELECTION_CLOSE,
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }
        Text(
            text = selection.title(),
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 15.sp,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.weight(1f).testTag("selectionCount"),
        )
        IconButton(onClick = onExport, enabled = !busy, modifier = Modifier.testTag("selectionExport")) {
            Icon(
                Icons.Filled.IosShare,
                contentDescription = Wording.CARD_MENU_EXPORT,
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }
        IconButton(onClick = onShare, enabled = !busy, modifier = Modifier.testTag("selectionShare")) {
            Icon(
                Icons.Filled.Share,
                contentDescription = ProjectActionWording.SHARE_ACTION,
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }
        IconButton(onClick = onDelete, enabled = !busy, modifier = Modifier.testTag("selectionDelete")) {
            Icon(
                Icons.Filled.Delete,
                contentDescription = ProjectActionWording.DELETE_ACTION,
                tint = MaterialTheme.colorScheme.error,
            )
        }
    }
}

/**
 * ROUND 24 item 108 — **one row, two controls, no menu bar.**
 *
 * A layout toggle on the left and a sort menu on the right, in 40 dp, above a
 * list the operator came here to read. Two deliberate choices:
 *
 *  * **The layout is a toggle, not a menu.** There are two layouts. A menu for
 *    a binary is a tap and a read where a tap would do, and the icon shows the
 *    layout you would GET rather than the one you are in — which is what a
 *    toggle's icon means everywhere else on a phone.
 *  * **The sort is a menu showing its current value.** Three options with real
 *    names ("Newest", "A–Z", "Z–A"), and the current one is on the row, because
 *    "why is this scan at the top" must be answerable without opening anything.
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
            .padding(horizontal = 18.dp, vertical = 2.dp)
            .testTag("projectsControlRow"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconButton(
            onClick = { onLayoutChange(ProjectsView.toggled(layout)) },
            modifier = Modifier.testTag("projectsLayoutToggle"),
        ) {
            Icon(
                // The icon of the layout a tap would GIVE you, which is what a
                // toggle means: in the list, offer the grid.
                if (layout == ProjectsLayout.GALLERY) {
                    Icons.AutoMirrored.Filled.ViewList
                } else {
                    Icons.Filled.GridView
                },
                contentDescription = ProjectsView.layoutActionLabel(layout),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.weight(1f))
        Box {
            Row(
                Modifier
                    .clickable(onClick = { sortMenuOpen = true })
                    .padding(horizontal = 8.dp, vertical = 8.dp)
                    .testTag("projectsSortButton"),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    Icons.Filled.Sort,
                    contentDescription = ProjectsView.SORT_LABEL,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.height(18.dp),
                )
                Spacer(Modifier.padding(horizontal = 3.dp))
                Text(
                    sort.label,
                    style = MonoMeta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.testTag("projectsSortValue"),
                )
            }
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

private fun aggregateLine(projects: List<Project>, hiddenEmptyCount: Int = 0): String {
    // ROUND 9 (item 33): the hidden strays are named here rather than left to be
    // discovered — "2 projects" on a phone with five directories on it would be
    // the app quietly disagreeing with the file manager.
    val hidden = if (hiddenEmptyCount > 0) " · $hiddenEmptyCount empty hidden" else ""
    // ROUND 23: the operator's word is "scan" everywhere else on this screen
    // (item 98 renamed the empty state and the card menu in round 22 and left
    // this line saying "projects"). One vocabulary.
    if (projects.isEmpty()) return "no scans yet$hidden"
    val georeferenced = projects.count { it.manifest.crsEpsg != null && it.manifest.crsEpsg != 0 }
    val totalPoints = projects.sumOf { it.manifest.pointCountEstimate ?: 0L }
    val pts = when {
        totalPoints >= 1_000_000 -> "%.1f M points".format(totalPoints / 1_000_000.0)
        totalPoints > 0 -> "%,d points".format(totalPoints)
        else -> "no points yet"
    }
    return "${projects.size} project${if (projects.size == 1) "" else "s"} · " +
        "$georeferenced georeferenced · $pts$hidden"
}

/**
 * One project card: thumbnail, title with a chevron, the chip row, and a mono
 * meta line.
 *
 * Delete moved from a trash `IconButton` in the title row to a long-press. The
 * icon was a permanently visible destructive control sitting one thumb-width
 * from the card's own tap target, and the redesign's card has no room for it
 * next to the title — the confirmation dialog it opens is unchanged, so the
 * safety of the action did not move, only its discoverability, which the hint
 * under the list states outright.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ProjectCard(
    project: Project,
    selected: Boolean,
    /**
     * ROUND 24 item 108 — **drawn in the 2-column gallery.**
     *
     * Three differences and no fourth: a shorter thumbnail (a 2-column card is
     * half as wide, so the same 108 dp would be a letterbox), the chip row
     * trimmed to the sensor badge, and a smaller title. Everything that is a
     * BEHAVIOUR — the tap, the long-press, the ⋯ menu, the selection tick, the
     * progress chip — is identical, because it is the same composable.
     */
    gallery: Boolean = false,
    /** ROUND 22 item 96: 0..1 while a job started from this card runs; null otherwise. */
    progress: Float? = null,
    /** ROUND 23 item 104c: what the progress chip says — a reprocess and an export differ. */
    progressLabel: (Int) -> String = { percent -> Wording.fixingProgress(percent) },
    /** ROUND 23 item 104c: the list is in selection mode, so a tap picks. */
    selecting: Boolean = false,
    /** ROUND 23 item 104c: this card is in the selection. */
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
    var menuOpen by remember { mutableStateOf(false) }
    val shape = RoundedCornerShape(ScanDims.CardRadius)
    val manifest = project.manifest
    // ROUND 25 item 114. The card asks `:core` rather than deciding: one
    // layout fact, one place, one unit test.
    val showThumbnail = ProjectsView.showsThumbnail(
        if (gallery) ProjectsLayout.GALLERY else ProjectsLayout.LIST,
    )

    Column(
        Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainer, shape)
            .border(
                // A picked card is outlined in the primary colour and 2 dp, so
                // "which of these am I about to export" is answerable at arm's
                // length rather than by reading a small tick.
                if (checked) 2.dp else 1.dp,
                when {
                    checked -> MaterialTheme.colorScheme.primary
                    selected -> MaterialTheme.colorScheme.primary
                    else -> MaterialTheme.colorScheme.outlineVariant
                },
                shape,
            )
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongClick,
                onLongClickLabel = ProjectActionWording.SELECT_LABEL,
            )
            .padding(10.dp)
            .testTag(
                when {
                    checked -> "projectCardChecked"
                    selected -> "projectCardSelected"
                    else -> "projectCard"
                },
            ),
    ) {
        // ROUND 5 (item 8): the preview IS the selection. A selected card gives
        // its cloud twice the height — enough to read the shape of a scan —
        // instead of opening another screen to do it.
        //
        // ROUND 25 item 114: **only in the gallery.** The list row draws no
        // preview at all — see `ProjectsView.showsThumbnail`. The round-5
        // selection-expands-the-preview behaviour therefore only exists where
        // there is a preview to expand; in the list, selection is the 2 dp
        // primary border and nothing else, which is all it has needed since
        // round 22 made a tap open the viewer directly.
        if (showThumbnail) {
            ProjectThumbnail(
                project = project,
                // Tagged so item 114 is assertable on the emulator: the list
                // must draw ZERO of these and the gallery at least one. An
                // absence with no name is not something a test can check.
                modifier = Modifier
                    .fillMaxWidth()
                    .height(if (selected) 180.dp else 96.dp)
                    .testTag("projectPreview"),
            )
        }

        Row(
            // With no preview above it the title row IS the top of the card, so
            // its 11 dp gap from the thumbnail becomes 1 dp of breathing room
            // from the card's own padding. This is the "tighter rows" half of
            // item 114 — dropping the image and keeping the spacing it needed
            // would leave a list of tall empty cards.
            Modifier.fillMaxWidth().padding(
                start = 4.dp,
                end = 4.dp,
                top = if (showThumbnail) 11.dp else 1.dp,
            ),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (selecting) {
                Icon(
                    if (checked) Icons.Filled.CheckCircle else Icons.Outlined.RadioButtonUnchecked,
                    contentDescription = null,
                    tint = if (checked) MaterialTheme.colorScheme.primary else InkFaint,
                    modifier = Modifier
                        .height(18.dp)
                        .padding(end = 2.dp)
                        .testTag(if (checked) "projectCardTick" else "projectCardUntick"),
                )
                Spacer(Modifier.padding(horizontal = 3.dp))
            }
            Text(
                text = manifest.name,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.SemiBold,
                fontSize = if (gallery) 14.sp else 17.sp,
                letterSpacing = (-0.015).em,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f, fill = false),
            )
            Spacer(Modifier.padding(horizontal = 3.dp))
            if (!gallery) {
                Icon(
                    Icons.AutoMirrored.Filled.KeyboardArrowRight,
                    contentDescription = null,
                    tint = InkFaint,
                    modifier = Modifier.height(16.dp),
                )
            }
            // ── ROUND 22 item 96: the per-card ⋯ menu ────────────────────────
            //
            // Export, Process again and Delete — the three things Simple mode's
            // removed "Details, jobs & export" hub was actually used for, on
            // the card they are about. Delete was a long-press since ROUND 5,
            // which is a gesture the list hint had to TEACH; a menu is found.
            Box {
                IconButton(
                    onClick = { menuOpen = true },
                    modifier = Modifier.testTag("projectCardMenu"),
                ) {
                    Icon(
                        Icons.Filled.MoreVert,
                        contentDescription = "More actions",
                        tint = InkFaint,
                    )
                }
                DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                    DropdownMenuItem(
                        text = { Text(Wording.CARD_MENU_EXPORT) },
                        onClick = { menuOpen = false; onExport() },
                        modifier = Modifier.testTag("cardMenuExport"),
                    )
                    // ROUND 23 item 104b: Share, next to Export, because the
                    // owner's report is that BOTH vanished — and a menu that
                    // offers only "save it where you cannot browse to it" is
                    // half an answer.
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
                        onClick = { menuOpen = false; showDeleteConfirm = true },
                        modifier = Modifier.testTag("cardMenuDelete"),
                    )
                }
            }
        }

        // ROUND 22 item 96: a running job is a chip on the card, because Simple
        // mode has no Jobs screen to go and look at.
        if (progress != null) {
            Row(
                Modifier.fillMaxWidth().padding(start = 4.dp, end = 4.dp, top = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                CircularProgressIndicator(
                    progress = { progress },
                    modifier = Modifier.height(14.dp),
                    color = MaterialTheme.colorScheme.primary,
                    strokeWidth = 2.dp,
                )
                Text(
                    progressLabel((progress * 100).toInt()),
                    style = MonoMeta,
                    color = InkFaint,
                    modifier = Modifier.testTag("projectCardJobChip"),
                )
            }
        }

        Row(
            Modifier.fillMaxWidth().padding(
                start = 4.dp,
                end = 4.dp,
                top = if (showThumbnail) 8.dp else 6.dp,
            ),
            horizontalArrangement = Arrangement.spacedBy(5.dp),
        ) {
            ScanChip(
                text = manifest.sensor.badgeLabel.uppercase(),
                // ROUND 25 item 119: an exhaustive lookup, not an `else`.
                // The `else` painted the new STL-27L in the D6's teal.
                color = sensorBadgeColor(manifest.sensor),
                showDot = true,
            )
            if (!gallery) {
                ScanChip(
                    text = manifest.profile.displayName.uppercase(),
                    showDot = true,
                )
            }
            // ROUND 6 (owner item 20): a capture whose app-side metadata was
            // destroyed by the pre-0.3.0 `manifest.json` collision with the
            // engine's own container manifest, and which `FileProjectStore`
            // rebuilt so it is listable again. Its POINTS are intact; its
            // name/sensor/profile are a reconstruction, and saying so is the
            // difference between honest recovery and a quiet lie.
            if (manifest.recovered) {
                ScanChip(text = "RECOVERED", color = SemWarn, showDot = true)
            }
            if (manifest.crsEpsg != null && manifest.crsEpsg != 0) {
                ScanChip(text = "GEOREF ✓", color = SemGood, showDot = true)
            }
        }

        Text(
            text = metaLine(project),
            style = MonoMeta,
            color = InkFaint,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(
                start = 4.dp,
                end = 4.dp,
                top = if (showThumbnail) 9.dp else 6.dp,
                bottom = if (showThumbnail) 3.dp else 1.dp,
            ),
        )

        // ROUND 22 item 96: the two quiet doors are gone. "Open in viewer" is
        // what a tap does now, and "Details, jobs & export" is the hub Simple
        // mode removes — its three real uses are in the ⋯ menu above. Neither
        // screen is deleted: with Advanced on, ProjectDetail is back in
        // navigation exactly as it is today (see SimpleMode).
    }

    if (showDeleteConfirm) {
        AlertDialog(
            // ROUND 16 item 61: dialogs inherited the theme's pill too.
            shape = RoundedCornerShape(ScanDims.DialogRadius),
            onDismissRequest = { showDeleteConfirm = false },
            // ROUND 22 item 98. Was: 'Delete "<name>"?' over "This permanently
            // deletes the .lscan project directory, including any captured
            // streams. This can't be undone." — the on-disk format and the word
            // "streams" are facts about the implementation, not about what the
            // operator is about to lose.
            title = { Text(Wording.DELETE_TITLE) },
            text = { Text(Wording.DELETE_BODY) },
            confirmButton = {
                TextButton(onClick = {
                    showDeleteConfirm = false
                    ProjectPreviewCache.invalidate(project.id)
                    onDelete()
                }) { Text("Delete") }
            },
            dismissButton = {
                TextButton(onClick = { showDeleteConfirm = false }) { Text("Cancel") }
            },
        )
    }
}

/** The mono meta line: points · created · EPSG, in the mockup's order. */
private fun metaLine(project: Project): String {
    val m = project.manifest
    val points = m.pointCountEstimate?.let {
        when {
            it >= 1_000_000 -> "%.1f M pts".format(it / 1_000_000.0)
            it >= 1_000 -> "%.1f K pts".format(it / 1_000.0)
            else -> "$it pts"
        }
    } ?: "no capture"
    val created = com.lidarscan.app.ui.common.formatCreatedDate(m.createdAtEpochMillis)
    val epsg = m.crsEpsg?.takeIf { it != 0 }?.let { "EPSG $it" }
    return listOfNotNull(points, created, epsg).joinToString(" · ")
}

@Composable
private fun EmptyProjectsState(hiddenEmptyCount: Int, onNewScan: () -> Unit) {
    Box(
        Modifier.fillMaxSize().padding(horizontal = 28.dp, vertical = 24.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = Wording.PROJECTS_EMPTY_TITLE,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.SemiBold,
                fontSize = 22.sp,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Spacer(Modifier.height(10.dp))
            // ROUND 22 item 98. Was 26 words naming two products and a tab.
            Hint(Wording.PROJECTS_EMPTY_HINT)
            // ROUND 9 (item 33): "No projects yet" would be a lie on a phone
            // holding nothing BUT empty strays, so the empty state says which
            // kind of empty it is.
            if (hiddenEmptyCount > 0) {
                Spacer(Modifier.height(10.dp))
                Hint(
                    Wording.PROJECTS_EMPTY_HIDDEN,
                    color = InkFaint,
                    modifier = Modifier.testTag("hiddenEmptyScansNote"),
                )
            }
            Spacer(Modifier.height(22.dp))
            // The one exception to "Projects never creates a scan": with no
            // projects at all, a tab that only says "go somewhere else" is a dead
            // end, so this is a shortcut TO the Capture tab, not a second way to
            // create a project.
            PrimaryPill(
                text = Wording.PROJECTS_EMPTY_ACTION,
                icon = Icons.Filled.Add,
                onClick = onNewScan,
                modifier = Modifier.testTag("newScanButton"),
            )
        }
    }
}
