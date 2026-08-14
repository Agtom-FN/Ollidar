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
}
