package com.lidarscan.app.ui.pick

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.outlined.Work
import androidx.compose.material3.HorizontalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.HeroHeader
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.ScanEmptyState
import com.lidarscan.app.ui.components.ScanRow
import com.lidarscan.app.ui.projects.ProjectThumbnail
import com.lidarscan.app.ui.projects.ProjectsListViewModel
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.core.store.Project

/**
 * What the picker is standing in front of — only the copy differs.
 *
 * ROUND 28 item 169 — **the prompt is a detail line, not a lesson.**
 *
 * It read *"A queue belongs to a project. Pick the one whose jobs you want to
 * see."* — 15 words, and the first sentence is a statement about this app's
 * data model that the operator neither asked for nor can act on. This screen is
 * one tap from the tab bar (`WordingLaw.TabBarScreen.JOBS`), so it gets the
 * 12-word budget and no exemption.
 */
enum class PickPurpose(val title: String, val prompt: String) {
    // ROUND 5 (item 8): the CAPTURE purpose is gone. The Capture tab creates a
    // new project on Start, so there is nothing to pick in front of it — this
    // picker now stands in front of Jobs only.
    JOBS(
        title = "Jobs",
        prompt = "Pick a scan",
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
 *
 * ROUND 28 item 163: it is now built out of the same three pieces as the queue
 * it stands in front of — [HeroHeader], [ScanRow], [ScanEmptyState] — because
 * it is the *first* thing an operator sees when they tap Jobs, and a picker
 * with its own card geometry, its own chip pair and its own row height made
 * that first impression a fourth visual language. The thumbnail, the scan name
 * and the point count are what a person picks by; the sensor and profile chips
 * carried the same value on all 66 of the owner's projects (§C.4's chip law) so
 * they are gone.
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
            .background(ScanColors.page)
            .statusBarsPadding()
            .navigationBarsPadding()
            .testTag("projectPicker"),
    ) {
        HeroHeader(title = purpose.title, subtitle = purpose.prompt)

        if (uiState.projects.isEmpty() && !uiState.loading) {
            ScanEmptyState(
                icon = Icons.Outlined.Work,
                title = "No scans yet",
                body = "Jobs appear here once you record something.",
                action = {
                    PrimaryPill(text = "New scan", icon = Icons.Filled.Add, onClick = onNewProject)
                },
            )
            return@Column
        }

        LazyColumn(
            modifier = Modifier.fillMaxWidth().weight(1f),
            contentPadding = PaddingValues(bottom = ScanDims.TabBarClearance),
        ) {
            items(uiState.projects, key = { it.id }) { project ->
                HorizontalDivider(thickness = ScanDims.Hair, color = ScanColors.line)
                PickerRow(project = project, onClick = { onPick(project.id) })
            }
            item {
                HorizontalDivider(thickness = ScanDims.Hair, color = ScanColors.line)
                PrimaryPill(
                    text = "New scan",
                    icon = Icons.Filled.Add,
                    onClick = onNewProject,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(ScanDims.ScreenMargin),
                )
            }
        }
    }
}

@Composable
private fun PickerRow(project: Project, onClick: () -> Unit) {
    ScanRow(
        title = project.manifest.name,
        meta = com.lidarscan.app.ui.common.formatPointCount(project.manifest.pointCountEstimate),
        onClick = onClick,
        minHeight = ScanDims.RowWithThumb,
        leading = {
            ProjectThumbnail(project = project, modifier = Modifier.size(ScanDims.Thumb))
        },
    )
}
