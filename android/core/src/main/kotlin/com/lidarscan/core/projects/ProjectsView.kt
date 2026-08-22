package com.lidarscan.core.projects

import com.lidarscan.core.store.Project

/**
 * ROUND 24 item 108 — **how the Projects tab is laid out, and in what order.**
 *
 * Two preferences, both persisted, both pure enough to live here rather than in
 * a composable's `remember`: the round-23 selection state machine already
 * proved that "which scan is picked" belongs in `:core` where a unit test can
 * hold it still, and "which scans are shown first" is the same kind of fact.
 *
 * The layout is deliberately NOT a density setting or a card-size slider. The
 * owner asked for two things an operator can name — a wall of pictures, or a
 * column of rows — and a third option would be a preference nobody has an
 * opinion about.
 */
enum class ProjectsLayout {
    /** A 2-column grid of thumbnail-first cards. Pictures first, names second. */
    GALLERY,

    /** The round-5 row card, full width. Names first, pictures second. */
    LIST,
    ;

    companion object {
        /**
         * **List, not gallery.**
         *
         * The list card is what every previous round was designed and tested
         * against, it is the one that has room for the chip row and the mono
         * meta line, and a default that changes the app for someone who never
         * asked is the opposite of what item 108 is for. Gallery is one tap
         * away and it is remembered.
         */
        val DEFAULT = LIST

        /** Unparseable / unknown reads as the default, never as a crash. */
        fun parse(raw: String?): ProjectsLayout =
            entries.firstOrNull { it.name == raw } ?: DEFAULT
    }
}

/**
 * The order the cards appear in.
 *
 * Three, with the labels the control actually draws. They are **names**, not
 * instructions (the round-22 law's own distinction), and they are the shortest
 * names that cannot be misread: "Newest" rather than "Date", because "Date"
 * does not say which end.
 */
enum class ProjectSort(val label: String) {
    /** Most recently created first. What a scanning app is for. */
    NEWEST("Newest"),

    /** By name, ascending, case-insensitive. */
    A_Z("A–Z"),

    /** By name, descending, case-insensitive. */
    Z_A("Z–A"),
    ;

    companion object {
        /**
         * The scan you just took is the scan you want, so it is at the top.
         * This is also what the list has always done implicitly (the store
         * returns newest-first), which means turning the control on changes
         * nothing until it is used — the right shape for a new preference.
         */
        val DEFAULT = NEWEST

        fun parse(raw: String?): ProjectSort =
            entries.firstOrNull { it.name == raw } ?: DEFAULT
    }
}

object ProjectsView {

    /**
     * [projects] in [sort] order.
     *
     * Three properties this must have, and all three are tested:
     *
     *  * **Stable.** Two scans created in the same millisecond, or two with the
     *    same name, keep the order the store gave them. `sortedBy` and friends
     *    are stable sorts in Kotlin/JVM, and the tie-break is deliberately left
     *    to that rather than invented here — inventing one (by id, say) would
     *    reorder a list for a reason the operator cannot see.
     *  * **Case-insensitive by name.** "attic" sorts with "Attic", because a
     *    typed scan name is prose, not an identifier. `lowercase()` rather than
     *    `String.CASE_INSENSITIVE_ORDER` so the behaviour is the same on every
     *    locale's default collator.
     *  * **Total.** Every input list comes out the same length. A sort that can
     *    drop a scan is a sort that loses one.
     */
    fun sorted(projects: List<Project>, sort: ProjectSort): List<Project> = when (sort) {
        ProjectSort.NEWEST -> projects.sortedByDescending { it.manifest.createdAtEpochMillis }
        ProjectSort.A_Z -> projects.sortedBy { it.manifest.name.lowercase() }
        ProjectSort.Z_A -> projects.sortedByDescending { it.manifest.name.lowercase() }
    }

    /** How many columns [layout] draws. The grid's only geometric fact. */
    fun columns(layout: ProjectsLayout): Int = if (layout == ProjectsLayout.GALLERY) 2 else 1

    /**
     * ROUND 28 item 162 — **retired, and the reasoning it carried is why.**
     *
     * Round 25 item 114 wrote: *"a list is name-first, and a 108 dp preview
     * above every row means four scans fill a phone screen and finding the one
     * you want is scrolling, not reading."* That is correct, and it is an
     * argument against a 108 dp preview **above** a row — which is what the
     * card was. It was applied as an argument against the thumbnail, and it
     * took with it the single strongest differentiator between 66 otherwise
     * identical rows, leaving text and three chips that were the same on every
     * one of them.
     *
     * §D.5's row is 72 dp with a 56 dp tile at its LEADING edge, so the
     * thumbnail costs no height at all — four scans per screen became roughly
     * twice as many *with* the picture. The predicate had one remaining
     * property, that the fact be named and testable rather than an `if
     * (gallery)` at the draw site, and that property is preserved: there is no
     * condition to name any more, because both layouts draw one.
     */

    /** The layout the toggle switches TO — one control, two states, no menu. */
    fun toggled(layout: ProjectsLayout): ProjectsLayout =
        if (layout == ProjectsLayout.GALLERY) ProjectsLayout.LIST else ProjectsLayout.GALLERY

    // ── the words on the control row ───────────────────────────────────────

    /** The layout button's accessible name: what a tap will DO, not where you are. */
    fun layoutActionLabel(current: ProjectsLayout): String =
        if (current == ProjectsLayout.GALLERY) "Show as list" else "Show as gallery"

    /** The sort menu's own name, for a screen reader. */
    const val SORT_LABEL = "Sort scans"

    /** Every operator-facing string this item adds, for the wording guard. */
    val ALL: List<String> = ProjectSort.entries.map { it.label } + listOf(
        SORT_LABEL,
        layoutActionLabel(ProjectsLayout.GALLERY),
        layoutActionLabel(ProjectsLayout.LIST),
    )
}
