package com.lidarscan.core

/**
 * ROUND 22 item 98 — **the operator-facing sentences, in one place, under the
 * law.**
 *
 * These used to be string literals inline in composables, which is why they
 * drifted: nothing could see them all at once, so nobody could tell that the
 * Projects tab was explaining the app's architecture to someone who wanted to
 * press one button. Collecting them here is what makes
 * [com.lidarscan.core.WordingLaw] enforceable — `Round22WordingTest` walks
 * this object and fails the build on a breach, exactly as the round-19 guard
 * fails it on the word "light".
 *
 * Only the sentences an ORDINARY operator meets live here. Advanced screens
 * (RTK, the calibration wizard, the Mid-360 preflight) keep their own text and
 * get the lighter pass — see [WordingLaw]'s header for why "CRS" is the right
 * word to use with someone who went looking for the RTK screen.
 *
 * Each entry records the sentence it replaces, because the diff is the
 * argument.
 */
object Wording {

    // ── Projects, empty ────────────────────────────────────────────────────

    /** was: "No projects yet" */
    const val PROJECTS_EMPTY_TITLE = "No scans yet"

    /**
     * was: "Scans are created in the Capture tab: plug in the COIN-D6 or the
     * Mid-360 and it connects itself, then Start records into a new project."
     * — 26 words explaining the app's tab structure and two product names.
     */
    const val PROJECTS_EMPTY_HINT = "Plug in the scanner. Tap Scan."

    /** was: "Go to Capture" */
    const val PROJECTS_EMPTY_ACTION = "Start a scan"

    /**
     * was: "Tap a scan to preview it · long-press to delete · new scans start
     * in the Capture tab."
     *
     * Item 96 changed the behaviour under it: a tap opens the viewer now, and
     * delete moved into the card's own ⋯ menu where it is discoverable instead
     * of being explained.
     */
    const val PROJECTS_LIST_HINT = "Tap a scan to open it."

    /** was: "$n scan(s) recorded no points and is/are hidden. Settings › Scans deletes them, or shows them again." */
    const val PROJECTS_EMPTY_HIDDEN = "Some empty scans are hidden."

    // ── the per-card ⋯ menu (item 96) ──────────────────────────────────────

    const val CARD_MENU_EXPORT = "Export"
    const val CARD_MENU_REPROCESS = "Process again"
    const val CARD_MENU_DELETE = "Delete"

    /** was: 'Delete "<name>"?' — unchanged in meaning, shortened. */
    const val DELETE_TITLE = "Delete this scan?"

    /**
     * was: "This permanently deletes the .lscan project directory, including
     * any captured streams. This can't be undone."
     */
    const val DELETE_BODY = "The scan and its data go. Cannot undo."

    // ── Review ─────────────────────────────────────────────────────────────

    /** was: "Loading…" with no object. */
    const val REVIEW_LOADING = "Opening your scan…"

    /** was: a bare empty state. */
    const val REVIEW_EMPTY = "Nothing recorded in this scan."

    /** was: "Tap a point to start." */
    const val MEASURE_HINT = "Tap two points to measure."

    /**
     * was: "Between the two nearest sampled points — the tool picks from a
     * bounded sample of the cloud, not every point, so a pick can sit a few
     * centimetres from the return drawn under your finger."  (37 words.)
     */
    const val MEASURE_DETAIL = "Picks the nearest point. Off by a few centimetres."

    /**
     * was: "A ceiling on how many points are uploaded to the GPU, applied in
     * page order. It stops before the budget rather than decimating within a
     * page — that is what this renderer does, stated plainly."  (37 words, and
     * "decimating within a page" is not a thing an operator can act on.)
     */
    const val DETAIL_BUDGET_HINT = "More detail needs more memory."

    /** was: "Walked path" */
    const val SHOW_MY_PATH = "Show my path"

    /** ROUND 22 item 100's note, restated here so the guard covers it too. */
    const val DEVICE_LIMITED = "Limited by this device"

    // ── the multi-piece card (item 96: it auto-runs now) ───────────────────

    /**
     * was: "This scan is in N pieces. The camera re-anchored while you walked,
     * so the room is about a metre apart at each join." — followed by a button
     * asking the operator to start the fix. The fix runs by itself now, so the
     * card reports rather than asks.
     */
    fun scanInPieces(pieces: Int): String = "Scan is in $pieces pieces. Fixing…"

    /** was: "Aligning the pieces… NN%." */
    fun fixingProgress(percent: Int): String = "Fixing your scan… $percent%"

    /** was: "N pieces, already aligned — you are looking at the corrected map." */
    const val PIECES_FIXED = "Pieces joined. This is the fixed scan."

    // ── export (item 96: Export is a row on Review now) ────────────────────

    const val EXPORT_ACTION = "Export"

    /** was: "Save to Downloads via MediaStore". */
    const val EXPORT_DETAIL = "Saves to your Downloads folder."

    /** was: "Export started." */
    const val EXPORT_RUNNING = "Exporting…"

    /** was: "Export OK <file> -> <where>". */
    fun exportDone(fileName: String): String = "Saved $fileName to Downloads."

    /**
     * An ERROR: what happened, and what to do. was: "Export failed." (which is
     * half a sentence, and the half that does not help).
     */
    fun exportFailed(why: String): String = "Export failed: $why. Tap Export to retry."

    // ── the auto-process card after Stop ───────────────────────────────────

    /** was: "auto-process FAILED … the scan is saved and untouched; open it and tap Process" */
    const val AUTO_PROCESS_FAILED = "Could not finish. Tap Process to retry."

    /** was: "saved — tap Process to retry" */
    const val SCAN_SAVED = "Scan saved."

    // ── the Advanced switch (item 97) ──────────────────────────────────────

    const val ADVANCED_TITLE = "Advanced features"

    /** The one detail line under the switch. */
    const val ADVANCED_DETAIL = "Floor plan, merge, cloud, survey tools."

    // ── the Scan screen (item 95) ──────────────────────────────────────────

    const val SCAN_BUTTON = "SCAN"
    const val SCAN_BUTTON_RECORDING = "STOP"
    const val SCAN_BUTTON_STARTING = "CANCEL"
    const val ADVANCED_SHEET_BUTTON = "Advanced"

    /** The Detail row's own label. */
    const val DETAIL_LABEL = "Detail"

    /**
     * Every sentence above that is an INSTRUCTION (six words or fewer). The
     * guard walks this list; a new instruction that is not in it is not
     * guarded, so adding one here is part of adding one at all.
     */
    val INSTRUCTIONS: List<String> = listOf(
        PROJECTS_EMPTY_TITLE,
        PROJECTS_EMPTY_HINT,
        PROJECTS_EMPTY_ACTION,
        PROJECTS_LIST_HINT,
        PROJECTS_EMPTY_HIDDEN,
        CARD_MENU_EXPORT,
        CARD_MENU_REPROCESS,
        CARD_MENU_DELETE,
        DELETE_TITLE,
        REVIEW_LOADING,
        REVIEW_EMPTY,
        MEASURE_HINT,
        DETAIL_BUDGET_HINT,
        SHOW_MY_PATH,
        DEVICE_LIMITED,
        EXPORT_ACTION,
        EXPORT_RUNNING,
        SCAN_SAVED,
        ADVANCED_TITLE,
        SCAN_BUTTON,
        SCAN_BUTTON_RECORDING,
        SCAN_BUTTON_STARTING,
        ADVANCED_SHEET_BUTTON,
        DETAIL_LABEL,
    )

    /** Every sentence above that is a DETAIL line (twelve words or fewer). */
    val DETAILS: List<String> = listOf(
        DELETE_BODY,
        MEASURE_DETAIL,
        PIECES_FIXED,
        EXPORT_DETAIL,
        AUTO_PROCESS_FAILED,
        ADVANCED_DETAIL,
        scanInPieces(4),
        fixingProgress(62),
        exportDone("scan-068.ply"),
    )

    /** Every sentence above that is an ERROR: what happened + what to do. */
    val ERRORS: List<String> = listOf(
        AUTO_PROCESS_FAILED,
        exportFailed("no room on the phone"),
    )
}
