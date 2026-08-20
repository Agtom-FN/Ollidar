package com.lidarscan.core.projects

/**
 * ROUND 23 items 104a–c — **the words on the new export/share controls.**
 *
 * They live in `:core` next to the state machine they belong to, and they are
 * held to the ROUND 22 wording law (`WordingLaw`) by
 * `Round23GroupExportTest`, for the reason round 22 gave: a rule a document
 * knows and a test does not is a rule that comes back.
 *
 * They are **names**, not instructions — "Export", "Share", "3 selected". The
 * owner's complaint in item 104 is that the two actions vanished, and a name
 * that says exactly what it does is how a control is found again. Nothing here
 * explains anything; the one line that does ([BatchReport.summary]) is a
 * report, and is checked against the twelve-word detail ceiling.
 */
object ProjectActionWording {

    /** Review's new pill, and the ⋯ menu's new item. Mirrors `Wording.EXPORT_ACTION`'s shape. */
    const val SHARE_ACTION = "Share"

    /** The selection bar's destructive action — the SAME confirm dialog follows. */
    const val DELETE_ACTION = "Delete"

    /** The selection bar's close/X, described for a screen reader. */
    const val SELECTION_CLOSE = "Stop selecting"

    /** The long-press affordance, announced by the card's `onLongClickLabel`. */
    const val SELECT_LABEL = "Select scan"

    /** How the list says selection mode exists at all. Four words. */
    const val SELECTION_HINT = "Long-press to pick several."

    /** The card's progress chip while ITS export job runs — the reprocess chip's shape. */
    fun exportingProgress(percent: Int): String = "Exporting… $percent%"

    /** The chooser's own title for a group share. */
    fun sendFilesTitle(count: Int): String =
        if (count == 1) "Send 1 file" else "Send $count files"

    /** Every operator-facing string this round adds, for the wording guard. */
    val ALL: List<String> = listOf(
        SHARE_ACTION,
        DELETE_ACTION,
        SELECTION_CLOSE,
        SELECT_LABEL,
        SELECTION_HINT,
        exportingProgress(40),
        sendFilesTitle(3),
    )
}
