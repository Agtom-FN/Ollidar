package com.lidarscan.app.ui.common

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

// minSdk 29 has java.time natively (desugaring not needed).
private val DATE_FORMATTER = DateTimeFormatter.ofPattern("MMM d, yyyy")

fun formatCreatedDate(epochMillis: Long): String =
    DATE_FORMATTER.format(Instant.ofEpochMilli(epochMillis).atZone(ZoneId.systemDefault()))

/**
 * ROUND 28 item 150 — **the third formatter, folded into the one.**
 *
 * The item started from two — a correct adaptive one on Projects and a broken
 * divide-by-a-million on Review — and the screenshot sweep found a third here,
 * feeding the project picker and the detail screen. Its output differed from
 * Projects' by a space (`120.3K pts` against `120.3 K pts`), which is exactly
 * the kind of difference that is invisible in code review, invisible to every
 * test, and immediately visible when two screens showing the same scan are put
 * side by side.
 *
 * One number, one formatter. `PointCountFormat` lives in `:core` with its own
 * tests; this is a forwarder so the two call sites did not need editing, and
 * `rowClause` also gives an empty scan its own words rather than `0 pts`.
 */
fun formatPointCount(count: Long?): String =
    com.lidarscan.core.render.PointCountFormat.rowClause(count)

/**
 * A project's id is its `.lscan` DIRECTORY name (see `FileProjectStore`), so
 * naming an export after it produces `foo-ab12cd.lscan.lscan.zip`. This is the
 * one place the suffix is stripped — caught on a device, where the doubled
 * extension is exactly the kind of thing that looks fine in code review.
 */
fun exportBaseName(projectId: String): String = projectId.removeSuffix(".lscan")
