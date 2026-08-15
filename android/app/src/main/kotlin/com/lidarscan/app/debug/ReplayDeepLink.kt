package com.lidarscan.app.debug

import com.lidarscan.app.di.AppContainer
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile

/**
 * A debug-only launch intent extra (boolean) that skips straight past the
 * Projects list and Settings screen into the "Replay synthetic capture"
 * acceptance path (see [SettingsViewModel.replaySyntheticCapture][
 * com.lidarscan.app.ui.settings.SettingsViewModel.replaySyntheticCapture],
 * NOTES.md's B4 section) — [MainActivity][com.lidarscan.app.MainActivity]
 * checks its own launch [android.content.Intent] for this extra and, if
 * present, navigates directly to `Routes.replayCapture(projectId)` once the
 * synthetic-replay project exists.
 *
 * Added for the CI Android-emulator smoke test
 * (`app/src/androidTest/.../ReplayCaptureSmokeTest.kt`, see NOTES.md's
 * "Android emulator smoke test" section) so a run does not have to tap
 * through two screens on every push — a UI-navigation path is exactly what
 * the *other* smoke test in that file already covers (a cold launch reaching
 * the Projects list). Left in as a real, reusable deep link rather than a
 * test-only branch: nothing about it depends on the instrumentation test
 * process, it is just an intent extra a future harness (or a manual `adb
 * shell am start -e ... true`) can use the same way.
 */
const val EXTRA_LAUNCH_REPLAY_CAPTURE = "com.lidarscan.app.EXTRA_LAUNCH_REPLAY_CAPTURE"

/** Shared with [com.lidarscan.app.ui.settings.SettingsViewModel] so the two call sites can't drift on the project name. */
const val REPLAY_PROJECT_NAME = "Synthetic Replay Demo"

/**
 * Finds-or-creates the same "Synthetic Replay Demo" project
 * [com.lidarscan.app.ui.settings.SettingsViewModel.replaySyntheticCapture]
 * does, so repeated launches (including repeated CI runs) reuse one project
 * directory under `Projects/` instead of piling up duplicates.
 */
suspend fun findOrCreateReplayProjectId(container: AppContainer): String {
    val existing = container.projectStore.list().firstOrNull { it.manifest.name == REPLAY_PROJECT_NAME }
    val project = existing
        ?: container.projectStore.create(REPLAY_PROJECT_NAME, SensorType.COIN_D6, WorkflowProfile.QUICK_SCAN)
    return project.id
}
