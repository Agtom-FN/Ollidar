package com.lidarscan.app.ui.nav

import android.net.Uri

/**
 * Route strings for Navigation Compose. Project ids are directory basenames
 * (see `FileProjectStore`) and are URL-encoded/decoded at the nav boundary
 * defensively — today's slugs are plain ASCII, but nothing here depends on
 * that staying true.
 */
object Routes {
    const val PROJECTS = "projects"
    const val NEW_PROJECT = "new_project"
    const val SETTINGS = "settings"

    private const val PROJECT_DETAIL_PATTERN = "project/{projectId}"
    const val PROJECT_DETAIL = PROJECT_DETAIL_PATTERN
    const val PROJECT_ID_ARG = "projectId"

    fun projectDetail(projectId: String): String = "project/${Uri.encode(projectId)}"

    private const val CAPTURE_PATTERN = "project/{projectId}/capture"
    const val CAPTURE = CAPTURE_PATTERN
    fun capture(projectId: String): String = "project/${Uri.encode(projectId)}/capture"

    // B4: the "Replay synthetic capture" debug-drawer acceptance path — same
    // Capture screen, backed by a ReplayEngineBridge instead of
    // AppContainer.engineBridge. A distinct route (not a query param on
    // CAPTURE) keeps CaptureRoute's two entry points structurally explicit
    // in the NavHost rather than threading an optional-bool nav argument.
    private const val REPLAY_CAPTURE_PATTERN = "project/{projectId}/capture/replay"
    const val REPLAY_CAPTURE = REPLAY_CAPTURE_PATTERN
    fun replayCapture(projectId: String): String = "project/${Uri.encode(projectId)}/capture/replay"

    const val CONNECT_WIZARD = "connect_wizard"
}
