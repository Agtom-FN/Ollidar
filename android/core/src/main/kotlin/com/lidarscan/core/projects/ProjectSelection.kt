package com.lidarscan.core.projects

/**
 * ROUND 23 item 104c — **the Projects list's selection mode, as a value.**
 *
 * The owner asked for group export and group share, which means the list needs
 * a second mode: one where a tap picks a scan instead of opening it. That is a
 * genuine state machine (enter, toggle, exit, and a rule for what an empty
 * selection means), and every previous attempt to keep one of those in a
 * handful of `remember { mutableStateOf(...) }` booleans in this codebase ended
 * with two of them disagreeing.
 *
 * So it is one immutable value in `:core`, testable on a bare JVM:
 *
 *  * **`isActive` is not "selectedIds is non-empty".** They are kept together
 *    deliberately — the top bar's Export/Share/Delete are only meaningful with
 *    at least one scan under them, and a mode with nothing in it is a mode the
 *    operator cannot leave by un-picking the last card. Emptying the selection
 *    therefore leaves the mode ([toggle]), which is what every file manager on
 *    the phone already does.
 *  * **A tap outside selection mode does nothing here.** The list's tap still
 *    opens the scan (ROUND 22 item 96); [toggle] refuses to start a selection,
 *    because starting one on an ordinary tap is exactly the accidental-mode
 *    problem long-press exists to avoid. Only [enter] starts it.
 *  * **[retain] exists because Delete is one of the batch actions.** After a
 *    group delete the ids in [selectedIds] name scans that are gone; carrying
 *    them into the next action would queue jobs against directories that no
 *    longer exist.
 */
data class ProjectSelection(
    /** True while the list is picking scans rather than opening them. */
    val isActive: Boolean = false,
    val selectedIds: Set<String> = emptySet(),
) {

    /** What the selection top bar counts. */
    val count: Int get() = selectedIds.size

    val isEmpty: Boolean get() = selectedIds.isEmpty()

    fun contains(id: String): Boolean = id in selectedIds

    /**
     * Long-press: enter selection mode **and** select the card that was
     * pressed. Entering with nothing selected would show a bar of three dead
     * actions, so the two are one step.
     */
    fun enter(id: String): ProjectSelection =
        ProjectSelection(isActive = true, selectedIds = selectedIds + id)

    /** The X in the selection bar, and what a finished batch action does. */
    fun exit(): ProjectSelection = EMPTY

    /**
     * Tap while in selection mode. Un-picking the last scan leaves the mode —
     * see the class comment.
     */
    fun toggle(id: String): ProjectSelection {
        if (!isActive) return this
        val next = if (id in selectedIds) selectedIds - id else selectedIds + id
        return if (next.isEmpty()) EMPTY else ProjectSelection(isActive = true, selectedIds = next)
    }

    /** Drops ids that are no longer in the list (a group delete just removed them). */
    fun retain(existing: Collection<String>): ProjectSelection {
        val keep = selectedIds.intersect(existing.toSet())
        return if (keep.isEmpty()) EMPTY else ProjectSelection(isActive = isActive, selectedIds = keep)
    }

    /**
     * The selection in the **list's** order rather than a hash set's.
     *
     * Batch export runs one job at a time and reports "2 of 3"; that numbering
     * has to mean the same thing on two consecutive runs, which a `Set`'s
     * iteration order does not promise.
     */
    fun ordered(listOrder: List<String>): List<String> = listOrder.filter { it in selectedIds }

    /** The selection bar's title. Two words, per the ROUND 22 wording law. */
    fun title(): String = "$count selected"

    companion object {
        val EMPTY = ProjectSelection()
    }
}
