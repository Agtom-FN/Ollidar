package com.lidarscan.core.store

import com.lidarscan.core.model.ProjectManifest
import java.io.File

/**
 * In-memory handle for a project: the on-disk `.lscan` directory paired with
 * its parsed manifest. [id] is the directory's own basename (e.g.
 * `"office-survey-3f9a2b.lscan"`) — the filesystem already guarantees this
 * is unique, so there's no separate UUID to keep in sync with it.
 */
data class Project(
    val id: String,
    val directory: File,
    val manifest: ProjectManifest,
) {
    val streamsDir: File get() = File(directory, "streams")
    val framesDir: File get() = File(streamsDir, "frames")
    val processedDir: File get() = File(directory, "processed")
    val mergedDir: File get() = File(directory, "merged")
    val exportsDir: File get() = File(directory, "exports")
}
