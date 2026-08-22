package com.lidarscan.core.render

import java.util.Locale

/**
 * ROUND 28 item 150 — **one number, one formatter, one source.**
 *
 * The owner's Review screenshot showed a header reading `0.0 M pts` above a
 * floating chip reading `52,041 pts`, for the same scan. Two defects wearing one
 * symptom:
 *
 *  1. **The formatter.** `ReviewScreen.kt:464` was
 *     `"%.1f M pts".format(it / 1_000_000.0)` — an unconditional divide by a
 *     million. 46,500 points became 0.0465, which prints as `0.0`. Meanwhile
 *     `ProjectsListScreen.metaLine` had a perfectly good adaptive K/M formatter
 *     twelve files away, which is why the Projects card for that same scan read
 *     `46.5 K pts` and the Review header did not. Two formatters is one
 *     formatter too many; this is the one, and both call it.
 *  2. **The source.** The header read `manifest.pointCountEstimate` (46,500)
 *     while the viewport chip read the *loaded cloud* (52,041). Where an
 *     estimate and an actual disagree, the actual wins — a number on screen
 *     should be a measurement, not a guess, and the operator has no way to know
 *     which one he is looking at. §D.4 deletes the chip and the header reads the
 *     loaded cloud.
 *
 * It lives in `:core` rather than in a UI helper so it is testable on a bare JVM
 * and so that neither screen can be tempted to grow a local variant again.
 *
 * A **visibly wrong number on the headline screen** is the fastest way an app
 * has of looking unfinished, which is why a formatter gets its own file and its
 * own tests.
 */
object PointCountFormat {

    /**
     * `46,500` → `46.5 K` · `8,100,000` → `8.1 M` · `842` → `842` · `0` → `0`.
     *
     * The thresholds are exact powers of ten rather than "about a thousand":
     * 999 must not round up into `1.0 K` while 1,000 renders as `1.0 K`, or the
     * same scan reads differently on two screens after one more point arrives.
     */
    fun compact(points: Long): String = when {
        points >= 1_000_000L -> String.format(Locale.US, "%.1f M", points / 1_000_000.0)
        points >= 1_000L -> String.format(Locale.US, "%.1f K", points / 1_000.0)
        else -> points.toString()
    }

    /** [compact] with the unit the operator reads: `46.5 K pts`. */
    fun compactPts(points: Long): String = "${compact(points)} pts"

    /**
     * The same number for a whole library — `8.1 M points`, spelled out because
     * a header has the room and a row does not.
     *
     * Below a thousand it is grouped rather than compacted: `842 points` is
     * both shorter and more precise than `842 points` would be after a rounding
     * pass, and a library with fewer than a thousand points in it is a library
     * whose exact count is interesting.
     */
    fun longForm(points: Long): String = when {
        points >= 1_000_000L -> String.format(Locale.US, "%.1f M points", points / 1_000_000.0)
        points > 0L -> String.format(Locale.US, "%,d points", points)
        else -> "no points yet"
    }

    /**
     * The row's points clause, including the case that is not a number.
     *
     * ROUND 28 item 162: an unsealed or empty scan says **"Empty — no points"**
     * and is *shown in the list*, not hidden behind a header clause. Seven of
     * the owner's seventy-four scans sealed with zero points; they are a result
     * he needs to see and delete.
     */
    fun rowClause(points: Long?): String = when {
        points == null -> "No capture"
        points <= 0L -> "Empty — no points"
        else -> compactPts(points)
    }
}
