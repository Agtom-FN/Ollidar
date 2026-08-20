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
    const val SETTINGS = "settings"

    /**
     * ROUND 5 (item 8): **the Capture tab creates new scans and nothing else.**
     *
     * It takes no project id — Start creates the project (item 9) — so there is
     * no picker in front of it any more and no "which project did you mean"
     * state to carry. The redesign's `CAPTURE_PICK` is gone with it.
     *
     * Jobs is still inherently per-project (a queue is a queue *for* a project),
     * so it keeps its picker for the no-active-project case.
     */
    const val CAPTURE_NEW = "capture"
    const val JOBS_PICK = "jobs"

    private const val PROJECT_DETAIL_PATTERN = "project/{projectId}"
    const val PROJECT_DETAIL = PROJECT_DETAIL_PATTERN
    const val PROJECT_ID_ARG = "projectId"

    fun projectDetail(projectId: String): String = "project/${Uri.encode(projectId)}"

    // ROUND 5: the project-scoped capture route is GONE. Every Start creates a new
    // project (item 9), so "capture into this existing project" is not a state the
    // app has any more — and a route nothing can reach is worse than no route.
    // REPLAY_CAPTURE below still carries a project id, which is what keeps
    // CaptureViewModel's project-scoped path exercised.

    // B4: the "Replay synthetic capture" debug-drawer acceptance path — same
    // Capture screen, backed by a ReplayEngineBridge instead of
    // AppContainer.engineBridge. A distinct route (not a query param on
    // CAPTURE) keeps CaptureRoute's two entry points structurally explicit
    // in the NavHost rather than threading an optional-bool nav argument.
    private const val REPLAY_CAPTURE_PATTERN = "project/{projectId}/capture/replay"
    const val REPLAY_CAPTURE = REPLAY_CAPTURE_PATTERN
    fun replayCapture(projectId: String): String = "project/${Uri.encode(projectId)}/capture/replay"

    // ROUND 5 (item 7): the standalone D6 connect wizard is GONE. Its two jobs —
    // find the device, and let the operator pick one by hand — are now the Capture
    // tab's auto-detect line and its inline manual panel, with the live preview as
    // the proof of life that the wizard's health panel used to be.

    // B3: the Mid-360 (Ethernet) connect wizard, per project — the entry point
    // that can save the addresses into that project's manifest (§3.1's "Save per
    // project"). ROUND 5: its project-less variant went with the D6 wizard; the
    // Capture tab's own inline manual panel covers the "no project yet" case, and
    // the addresses it uses are stored device-level anyway.
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

    // B6: Processing (mode chooser + queue), per project.
    const val PROCESSING = "project/{projectId}/processing"
    fun processing(projectId: String): String = "project/${Uri.encode(projectId)}/processing"

    // B10/B11: Review (viewer, display params, measure) and the floor-plan viewer.
    const val REVIEW = "project/{projectId}/review"
    fun review(projectId: String): String = "project/${Uri.encode(projectId)}/review"

    const val PLAN = "project/{projectId}/plan"
    fun plan(projectId: String): String = "project/${Uri.encode(projectId)}/plan"

    /**
     * B9: RTK is **not** per project, and the route says so.
     *
     * The engine owns one GnssSource, one NTRIP client and one georef fusion for
     * its whole lifetime, because an operator pairs the rover and waits for RTK
     * Fixed *before* any session exists (§3.4's capture gate is that
     * pre-session decision). A per-project route would imply a per-project
     * rover, which is not what the engine does.
     */
    const val RTK = "rtk"

    /**
     * ROUND 23 item 106(c) — the Mid-360 wizard, reached from the **Scan tab**,
     * before any project exists.
     *
     * The owner is testing Mid-360 + RTK next, and with Advanced OFF the only
     * doors to the wizard were inside the Details hub that Simple mode hides.
     * `Mid360ConnectViewModel` has taken a nullable `projectId` since B3 (the
     * addresses are stored device-level as well as per project), so this is a
     * second entry into the SAME screen rather than a second screen — with no
     * project id, "save into this project's manifest" is simply not offered
     * and the device-level save is.
     */
    const val MID360_SETUP = "mid360_setup"

    /** B12: georeferenced auto-merge — inherently multi-project, so also global. */
    const val MERGE = "merge"
}
