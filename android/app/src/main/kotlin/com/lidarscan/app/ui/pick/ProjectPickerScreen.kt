package com.lidarscan.app.ui.pick

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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
import com.lidarscan.app.ui.components.ScanChip
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.projects.ProjectThumbnail
import com.lidarscan.app.ui.projects.ProjectsListViewModel
import com.lidarscan.app.ui.theme.sensorBadgeColor
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.app.ui.theme.PoseBlue
import com.lidarscan.app.ui.theme.ScanTeal
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.store.Project

/** What the picker is standing in front of — only the copy differs. */
enum class PickPurpose(val title: String, val prompt: String) {
    // ROUND 5 (item 8): the CAPTURE purpose is gone. The Capture tab creates a
    // new project on Start, so there is nothing to pick in front of it — this
    // picker now stands in front of Jobs only.
    JOBS(
        title = "Processing",
        prompt = "A queue belongs to a project. Pick the one whose jobs you want to see.",
    ),
}

/**
 * The **Jobs** tab's landing screen when no project is active yet.
 *
 * It exists because a queue is per-project underneath: A15's queue is scoped to a
 * project directory. A tab that silently picked the newest project would
 * eventually process the wrong one, and one that opened onto an empty screen would
 * be a dead tab — so this asks, once, and then the tab remembers (see
 * `LidarScanApp`'s `activeProjectId`).
 *
 * ROUND 5: Capture no longer comes through here — it creates its own project.
 */
@Composable
fun ProjectPickerRoute(
    container: AppContainer,
    purpose: PickPurpose,
    onPick: (String) -> Unit,
    onNewProject: () -> Unit,
    onBack: () -> Unit,
) {
    val viewModel: ProjectsListViewModel = viewModel(
        key = "picker-${purpose.name}",
        factory = viewModelFactory {
            initializer { ProjectsListViewModel(container.projectStore) }
        },
    )
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding()
            .testTag("projectPicker"),
    ) {
        HeroHeader(title = purpose.title, subtitle = "choose a project")

        Hint(
            purpose.prompt,
            modifier = Modifier.padding(horizontal = 20.dp, vertical = 4.dp),
        )

        if (uiState.projects.isEmpty() && !uiState.loading) {
            Box(Modifier.fillMaxWidth().weight(1f).padding(28.dp), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Hint("No projects yet — a scan needs one to record into.")
                    Spacer(Modifier.height(20.dp))
                    PrimaryPill(text = "New scan", icon = Icons.Filled.Add, onClick = onNewProject)
                    Spacer(Modifier.height(10.dp))
                    androidx.compose.material3.TextButton(onClick = onBack) { Text("Back to projects") }
                }
            }
            return@Column
        }

        LazyColumn(
            modifier = Modifier.fillMaxWidth().weight(1f),
            contentPadding = PaddingValues(
                start = 16.dp,
                end = 16.dp,
                top = 8.dp,
                bottom = ScanDims.TabBarClearance,
            ),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            items(uiState.projects, key = { it.id }) { project ->
                PickerRow(project = project, onClick = { onPick(project.id) })
            }
            item {
                Spacer(Modifier.height(4.dp))
                PrimaryPill(
                    text = "New scan",
                    icon = Icons.Filled.Add,
                    onClick = onNewProject,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}

@Composable
private fun PickerRow(project: Project, onClick: () -> Unit) {
    val shape = RoundedCornerShape(ScanDims.CardRadius)
    Row(
        Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainer, shape)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, shape)
            .clickable(onClick = onClick)
            .padding(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ProjectThumbnail(project = project, modifier = Modifier.size(width = 84.dp, height = 60.dp))
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                project.manifest.name,
                fontFamily = DisplayFontFamily,
                fontWeight = FontWeight.SemiBold,
                fontSize = 16.sp,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(5.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                ScanChip(
                    text = project.manifest.sensor.badgeLabel.uppercase(),
                    // ROUND 25 item 119: exhaustive, see `sensorBadgeColor`.
                    color = sensorBadgeColor(project.manifest.sensor),
                    showDot = true,
                )
                ScanChip(text = project.manifest.profile.displayName.uppercase())
            }
            Spacer(Modifier.height(6.dp))
            Text(
                com.lidarscan.app.ui.common.formatPointCount(project.manifest.pointCountEstimate),
                style = MonoMeta,
                color = InkFaint,
                maxLines = 1,
            )
        }
    }
}
