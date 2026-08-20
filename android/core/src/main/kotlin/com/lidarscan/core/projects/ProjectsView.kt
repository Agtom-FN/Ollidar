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
     * ROUND 25 item 114 — **does this layout draw the lidar preview?**
     *
     * Owner: the LIST row loses its preview image; the GALLERY keeps it. That
     * is not an arbitrary split, it is what the two layouts are FOR. A gallery
     * is thumbnail-first — remove the thumbnail and it is a list with fewer
     * words per card. A list is name-first, and a 108 dp preview above every
     * row means four scans fill a phone screen and finding the one you want is
     * scrolling, not reading.
     *
     * It lives here rather than as an `if (gallery)` in the composable for the
     * same reason [columns] does: round 24's property is that there is exactly
     * ONE `ProjectCard` and the layout supplies its differences, so each
     * difference has to be a named, testable fact rather than a condition
     * spelled out at the draw site.
     */
    fun showsThumbnail(layout: ProjectsLayout): Boolean = layout == ProjectsLayout.GALLERY

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
