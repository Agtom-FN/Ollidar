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

    // B3: the Mid-360 (Ethernet) connect wizard. Reachable two ways, and the
    // route takes an OPTIONAL project id for that reason: from Project Detail
    // (where the settings can be saved into that project's manifest, per
    // §3.1's "Save per project"), and from the D6/global connect wizard,
    // where there may be no project yet and the wizard is purely a transport
    // check.
    const val MID360_CONNECT_NO_PROJECT = "mid360_connect"
    private const val MID360_CONNECT_PATTERN = "project/{projectId}/mid360_connect"
    const val MID360_CONNECT = MID360_CONNECT_PATTERN
    fun mid360Connect(projectId: String): String = "project/${Uri.encode(projectId)}/mid360_connect"

    // B7: Device setup -> Mount calibration (Tech Spec §3.13's app structure,
    // WIZARD.md §2's "a five-screen wizard inside Device setup -> Mount
    // calibration"). Per-project because the resulting calibration is written
    // into that project's manifest; the device-level per-bracket store is what
    // makes it reusable across projects.
    private const val MOUNT_CALIBRATION_PATTERN = "project/{projectId}/mount_calibration"
    const val MOUNT_CALIBRATION = MOUNT_CALIBRATION_PATTERN
    fun mountCalibration(projectId: String): String = "project/${Uri.encode(projectId)}/mount_calibration"
}
