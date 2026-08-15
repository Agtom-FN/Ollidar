package com.lidarscan.app.ui.common

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

// minSdk 29 has java.time natively (desugaring not needed).
private val DATE_FORMATTER = DateTimeFormatter.ofPattern("MMM d, yyyy")

fun formatCreatedDate(epochMillis: Long): String =
    DATE_FORMATTER.format(Instant.ofEpochMilli(epochMillis).atZone(ZoneId.systemDefault()))

fun formatPointCount(count: Long?): String {
    if (count == null) return "No capture yet"
    return when {
        count >= 1_000_000 -> "%.1fM pts".format(count / 1_000_000.0)
        count >= 1_000 -> "%.1fK pts".format(count / 1_000.0)
        else -> "$count pts"
    }
}

/**
 * A project's id is its `.lscan` DIRECTORY name (see `FileProjectStore`), so
 * naming an export after it produces `foo-ab12cd.lscan.lscan.zip`. This is the
 * one place the suffix is stripped — caught on a device, where the doubled
 * extension is exactly the kind of thing that looks fine in code review.
 */
fun exportBaseName(projectId: String): String = projectId.removeSuffix(".lscan")
