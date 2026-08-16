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
import androidx.compose.material.icons.filled.Person
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.Project

@Composable
fun ProjectsListRoute(
    container: AppContainer,
    onOpenProject: (String) -> Unit,
    onNewProject: () -> Unit,
    onSettings: () -> Unit,
) {
    val viewModel: ProjectsListViewModel = viewModel(
        factory = viewModelFactory {
            initializer { ProjectsListViewModel(container.projectStore) }
        },
    )
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    ProjectsListScreen(
        uiState = uiState,
        onOpenProject = onOpenProject,
        onNewProject = onNewProject,
        onSettings = onSettings,
        onDeleteProject = viewModel::delete,
    )
}

/**
 * The redesign's Projects screen: a hero header over cloud-thumbnail cards and
 * one ember action.
 *
 * The header's second line is the mockup's aggregate — project count,
 * georeferenced count, total points — computed from the manifests already in
 * memory, not a second query.
 */
@Composable
fun ProjectsListScreen(
    uiState: ProjectsUiState,
    onOpenProject: (String) -> Unit,
    onNewProject: () -> Unit,
    onSettings: () -> Unit,
    onDeleteProject: (String) -> Unit,
) {
    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        HeroHeader(
            title = "LidarScan",
            subtitle = aggregateLine(uiState.projects),
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

                uiState.projects.isEmpty() -> EmptyProjectsState(onNewProject)

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
                            onClick = { onOpenProject(project.id) },
                            onDelete = { onDeleteProject(project.id) },
                        )
                    }
                    item {
                        Spacer(Modifier.height(2.dp))
                        PrimaryPill(
                            text = "New scan",
                            icon = Icons.Filled.Add,
                            onClick = onNewProject,
                            modifier = Modifier.fillMaxWidth().testTag("newScanButton"),
                        )
                        Spacer(Modifier.height(10.dp))
                        Hint(
                            "Long-press a card to delete it.",
                            color = InkFaint,
                            modifier = Modifier.padding(horizontal = 4.dp),
                        )
                    }
                }
            }
        }
    }
}

private fun aggregateLine(projects: List<Project>): String {
    if (projects.isEmpty()) return "no projects yet"
    val georeferenced = projects.count { it.manifest.crsEpsg != null && it.manifest.crsEpsg != 0 }
    val totalPoints = projects.sumOf { it.manifest.pointCountEstimate ?: 0L }
    val pts = when {
        totalPoints >= 1_000_000 -> "%.1f M points".format(totalPoints / 1_000_000.0)
        totalPoints > 0 -> "%,d points".format(totalPoints)
        else -> "no captures yet"
    }
    return "${projects.size} project${if (projects.size == 1) "" else "s"} · $georeferenced georeferenced · $pts"
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
    onClick: () -> Unit,
    onDelete: () -> Unit,
) {
    var showDeleteConfirm by remember { mutableStateOf(false) }
    val shape = RoundedCornerShape(ScanDims.CardRadius)
    val manifest = project.manifest

    Column(
        Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainer, shape)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
            .combinedClickable(
                onClick = onClick,
                onLongClick = { showDeleteConfirm = true },
                onLongClickLabel = "Delete ${manifest.name}",
            )
            .padding(10.dp),
    ) {
        ProjectThumbnail(project = project, modifier = Modifier.fillMaxWidth().height(108.dp))

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
    }

    if (showDeleteConfirm) {
        AlertDialog(
            onDismissRequest = { showDeleteConfirm = false },
            title = { Text("Delete \"${manifest.name}\"?") },
            text = {
                Text(
                    "This permanently deletes the .lscan project directory, including any captured streams. " +
                        "This can't be undone.",
                )
            },
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
private fun EmptyProjectsState(onNewProject: () -> Unit) {
    Box(
        Modifier.fillMaxSize().padding(horizontal = 28.dp, vertical = 24.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = "No projects yet",
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.SemiBold,
                fontSize = 22.sp,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Spacer(Modifier.height(10.dp))
            Hint("Start a new project to capture with the COIN-D6 or Mid-360.")
            Spacer(Modifier.height(22.dp))
            PrimaryPill(
                text = "New scan",
                icon = Icons.Filled.Add,
                onClick = onNewProject,
                modifier = Modifier.testTag("newScanButton"),
            )
        }
    }
}
