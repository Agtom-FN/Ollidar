package com.lidarscan.app.ui.projects

import androidx.compose.foundation.ExperimentalFoundationApi
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Person
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
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.app.ui.theme.PoseBlue
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.app.ui.theme.SemGood
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.core.Wording
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.Project
import kotlinx.coroutines.flow.first

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

    // ROUND 5: the tab is a list + a preview, so the list has to be fresh when
    // the Capture tab has just created a project behind it.
    LaunchedEffect(Unit) { viewModel.refresh() }

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
        onReprocessProject = viewModel::reprocessProject,
        onOpenDetails = if (com.lidarscan.core.SimpleMode.showsProjectDetailHub(advanced)) onOpenProject else null,
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
    onReprocessProject: (String) -> Unit = {},
    onOpenDetails: ((String) -> Unit)? = null,
) {
    var selectedId by rememberSaveable { mutableStateOf<String?>(null) }
    // ROUND 8 (item 31): adopt the just-sealed scan ONCE, keyed on the id.
    // Keyed rather than run on every composition because the user must stay in
    // charge afterwards: collapsing the card and having it spring back open on
    // the next recomposition would be worse than not highlighting it at all.
    // `LaunchedEffect(id)` also means returning to this tab later does not
    // re-select a scan the operator has since dismissed.
    LaunchedEffect(initialSelectedId) {
        if (initialSelectedId != null) selectedId = initialSelectedId
    }
    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        HeroHeader(
            title = "LidarScan",
            subtitle = aggregateLine(uiState.projects, uiState.hiddenEmptyCount),
            trailing = {
                AvatarButton(
                    icon = Icons.Filled.Person,
                    // The smoke test's cold-launch check looks for exactly one
                    // node with this description; the tab bar's Settings tab
                    // deliberately carries none (see ScanTabBar).
                    contentDescription = "Settings",
                    onClick = onSettings,
                    modifier = Modifier.testTag("projectsAvatar"),
                )
            },
        )

        // Weighted so the list gets the height LEFT OVER under the hero,
        // not the full screen height (which would run the last card under the
        // floating tab bar).
        Box(Modifier.fillMaxWidth().weight(1f)) {
            when {
                uiState.loading -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator(color = MaterialTheme.colorScheme.primary)
                }

                uiState.projects.isEmpty() -> EmptyProjectsState(uiState.hiddenEmptyCount, onNewScan)

                else -> LazyColumn(
                    modifier = Modifier.fillMaxSize().testTag("projectsList"),
                    contentPadding = PaddingValues(
                        start = 16.dp,
                        end = 16.dp,
                        top = 2.dp,
                        bottom = ScanDims.TabBarClearance,
                    ),
                    verticalArrangement = Arrangement.spacedBy(14.dp),
                ) {
                    items(uiState.projects, key = { it.id }) { project ->
                        ProjectCard(
                            project = project,
                            selected = selectedId == project.id,
                            progress = uiState.running[project.id],
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
                            onClick = {
                                onSelectProject(project.id)
                                onOpenReview(project.id)
                            },
                            onExport = { onExportProject(project.id) },
                            onReprocess = { onReprocessProject(project.id) },
                            onOpenDetails = onOpenDetails?.let { open -> { open(project.id) } },
                            onDelete = { onDeleteProject(project.id) },
                        )
                    }
                    item {
                        Spacer(Modifier.height(2.dp))
                        Hint(
                            // ROUND 22 item 98. Was: "Tap a scan to preview it ·
                            // long-press to delete · new scans start in the
                            // Capture tab." — three instructions in one line,
                            // two of which describe behaviour item 96 changed
                            // (a tap opens now; delete is in the ⋯ menu, where
                            // it is discovered rather than explained).
                            Wording.PROJECTS_LIST_HINT +
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
}

private fun aggregateLine(projects: List<Project>, hiddenEmptyCount: Int = 0): String {
    // ROUND 9 (item 33): the hidden strays are named here rather than left to be
    // discovered — "2 projects" on a phone with five directories on it would be
    // the app quietly disagreeing with the file manager.
    val hidden = if (hiddenEmptyCount > 0) " · $hiddenEmptyCount empty hidden" else ""
    if (projects.isEmpty()) return "no projects yet$hidden"
    val georeferenced = projects.count { it.manifest.crsEpsg != null && it.manifest.crsEpsg != 0 }
    val totalPoints = projects.sumOf { it.manifest.pointCountEstimate ?: 0L }
    val pts = when {
        totalPoints >= 1_000_000 -> "%.1f M points".format(totalPoints / 1_000_000.0)
        totalPoints > 0 -> "%,d points".format(totalPoints)
        else -> "no captures yet"
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
    /** ROUND 22 item 96: 0..1 while a reprocess started from this card runs; null otherwise. */
    progress: Float? = null,
    onClick: () -> Unit,
    onExport: () -> Unit,
    onReprocess: () -> Unit,
    /** ROUND 22 item 97: non-null only with Advanced on. */
    onOpenDetails: (() -> Unit)? = null,
    onDelete: () -> Unit,
) {
    var showDeleteConfirm by remember { mutableStateOf(false) }
    var menuOpen by remember { mutableStateOf(false) }
    val shape = RoundedCornerShape(ScanDims.CardRadius)
    val manifest = project.manifest

    Column(
        Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainer, shape)
            .border(
                1.dp,
                if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant,
                shape,
            )
            .combinedClickable(
                onClick = onClick,
                onLongClick = { showDeleteConfirm = true },
                onLongClickLabel = "Delete ${manifest.name}",
            )
            .padding(10.dp)
            .testTag(if (selected) "projectCardSelected" else "projectCard"),
    ) {
        // ROUND 5 (item 8): the preview IS the selection. A selected card gives
        // its cloud two and a half times the height — enough to read the shape of
        // a scan — instead of opening another screen to do it.
        ProjectThumbnail(
            project = project,
            modifier = Modifier.fillMaxWidth().height(if (selected) 260.dp else 108.dp),
        )

        Row(
            Modifier.fillMaxWidth().padding(start = 4.dp, end = 4.dp, top = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = manifest.name,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.SemiBold,
                fontSize = 17.sp,
                letterSpacing = (-0.015).em,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f, fill = false),
            )
            Spacer(Modifier.padding(horizontal = 3.dp))
            Icon(
                Icons.AutoMirrored.Filled.KeyboardArrowRight,
                contentDescription = null,
                tint = InkFaint,
                modifier = Modifier.height(16.dp),
            )
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
                    Wording.fixingProgress((progress * 100).toInt()),
                    style = MonoMeta,
                    color = InkFaint,
                    modifier = Modifier.testTag("projectCardJobChip"),
                )
            }
        }

        Row(
            Modifier.fillMaxWidth().padding(start = 4.dp, end = 4.dp, top = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(5.dp),
        ) {
            ScanChip(
                text = manifest.sensor.badgeLabel.uppercase(),
                color = if (manifest.sensor == SensorType.MID360) PoseBlue else ScanTeal,
                showDot = true,
            )
            ScanChip(
                text = manifest.profile.displayName.uppercase(),
                showDot = true,
            )
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
            modifier = Modifier.padding(start = 4.dp, end = 4.dp, top = 9.dp, bottom = 3.dp),
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
