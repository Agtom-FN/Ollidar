package com.lidarscan.core.capture

import java.time.Instant
import java.time.ZoneId
import java.time.ZonedDateTime

/**
 * Round 5 item 9: **Start always creates a NEW project**, and when the operator
 * typed no name the project is named from a series counter plus the wall clock.
 *
 * The owner's example was `Scan-014 2026-08-17 19:32`. This produces
 * `Scan-014-2026-08-17-1932` — the same three parts in the same order, with the
 * spaces and the colon replaced, because the name is also what
 * [com.lidarscan.core.store.FileProjectStore] slugifies into a directory name
 * and what a desktop later sees on an exported `.lscan`. A colon in particular
 * is illegal on exFAT/NTFS (every SD card and every Windows desktop this
 * project ships to), so it never gets typed in the first place rather than being
 * stripped later by a store that would then disagree with the manifest.
 *
 * Kept in `:core` (plain JVM, no Android) so the series/format logic is
 * unit-testable without an emulator; the counter itself is persisted by
 * `:app`'s `SettingsRepository` (DataStore) because that is where every other
 * device-level preference lives.
 */
object ScanAutoName {

    const val PREFIX = "Scan"

    /**
     * The characters a project name must never contain. The union of what
     * Windows forbids (`\ / : * ? " < > |`) and what POSIX/`FileProjectStore`
     * care about (`/`, NUL) — plus control characters, which no operator types
     * on purpose but a paste can carry.
     */
    private val ILLEGAL = charArrayOf('\\', '/', ':', '*', '?', '"', '<', '>', '|')

    /**
     * `Scan-<series>-<yyyy-MM-dd>-<HHmm>`, e.g. `Scan-014-2026-08-17-1932`.
     *
     * [series] is zero-padded to three digits up to 999 and then simply grows
     * (`Scan-1000-…`) rather than truncating — a counter that wrapped or
     * clipped would produce two projects with the same name, which is the one
     * thing this function exists to prevent. Non-positive values are treated as
     * 1: a counter that somehow read back as 0 or negative should still name a
     * usable project rather than `Scan-000-…`, which reads like a bug.
     */
    fun format(series: Int, epochMillis: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val n = if (series < 1) 1 else series
        val t: ZonedDateTime = Instant.ofEpochMilli(epochMillis).atZone(zone)
        return "%s-%03d-%04d-%02d-%02d-%02d%02d".format(
            PREFIX,
            n,
            t.year,
            t.monthValue,
            t.dayOfMonth,
            t.hour,
            t.minute,
        )
    }

    /**
     * What the Capture tab's Start button names the project it is about to
     * create: the operator's own text when they typed something, otherwise
     * [format].
     *
     * A typed name is trimmed and made filesystem-safe ([sanitize]) but is
     * otherwise left alone — including a name that is *only* punctuation, which
     * sanitizes to empty and therefore falls back to the auto name rather than
     * creating a project called `-`.
     */
    fun resolve(typedName: String?, series: Int, epochMillis: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        val cleaned = sanitize(typedName.orEmpty())
        return cleaned.ifEmpty { format(series, epochMillis, zone) }
    }

    /**
     * Replaces path separators and the Windows-illegal set with a space, drops
     * control characters, collapses runs of whitespace to one space, and trims
     * (including trailing dots, which ext4 keeps and Windows silently eats).
     *
     * A **space**, not deletion: `C:\Site A` should read `C Site A`, not
     * `CSite A`, and `bay?3` should read `bay 3`. Deleting the separator glues
     * two words together and quietly changes what the operator typed, which is
     * worse than a space they can see. Never throws, and never returns a name
     * containing a directory separator — the two properties every caller
     * depends on.
     */
    fun sanitize(name: String): String {
        val mapped = buildString(name.length) {
            for (c in name) {
                when {
                    c.isWhitespace() -> append(' ')
                    c.code < 0x20 || c.code == 0x7F -> Unit
                    ILLEGAL.contains(c) -> append(' ')
                    else -> append(c)
                }
            }
        }
        return mapped.replace(Regex(" {2,}"), " ").trim().trim('.').trim()
    }
}
