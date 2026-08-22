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

    /**
     * ROUND 26 item 122 — **the app's name, once.**
     *
     * The owner renamed the app to "Ollidar" at the DISPLAY level only. The
     * launcher label lives in `res/values/strings.xml` because the OS reads it
     * from there; every sentence, footer and device-card line that names the
     * app reads it from here, so the next rename is two edits rather than a
     * find-and-replace across a hundred files that also hits the package name.
     *
     * Not renamed, deliberately: `applicationId`/`namespace`
     * (`com.lidarscan.app`) — an applicationId change is a NEW app to Android,
     * which would strand the owner's installed beta and its scans — and the
     * `Downloads/LidarScan/` export directory, which is a path the owner's
     * existing files are already in. The repository is still `lidarscan`.
     */
    const val APP_NAME = "Ollidar"

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

    // ── ROUND 27 item 134: the Review screen's own Process ──────────────────

    /**
     * The button the "Run Process on this project" paragraph has been naming
     * since round 8, and which did not exist until round 27.
     *
     * It is the same word as [CARD_MENU_REPROCESS]'s and as
     * [AUTO_PROCESS_FAILED]'s — deliberately. Three surfaces tell the operator
     * to "tap Process"; if any of them spelled the button differently, the
     * instruction would be a scavenger hunt.
     */
    const val REVIEW_PROCESS = "Process"

    /** While it runs, when the job has not yet named its own stage. */
    const val REVIEW_PROCESSING = "Working on your scan…"

    /**
     * A processing run that failed, with the engine's own reason.
     *
     * An error says what happened AND what to do — the whole of the law's third
     * clause, and the whole of item 134(b): the run that prompted this round
     * cleared its progress chip, said nothing, and left the operator on the
     * same empty screen.
     */
    fun processFailed(why: String): String = "Processing failed: $why. Tap Process to retry."

    // ── the Lab switch (item 97, renamed by ROUND 29 item 174) ─────────────

    /**
     * ROUND 29 item 174 — the owner: *"the Advanced features remark (Lab
     * features)"*.
     *
     * Two things were wrong with "Advanced features". It told the operator the
     * problem was him — that the rows behind it are for people who are advanced
     * — when what they actually are is **unfinished**: floor plan, merge, the
     * cloud fields and the survey tools are work in progress the owner wanted
     * reachable without being promised. And it collided with the Scan screen's
     * **Advanced sheet**, which is neither a lab nor a switch, so one word led
     * to two unrelated doors.
     *
     * "Lab" says the true thing in one syllable, and it is the word the rest of
     * the industry already uses for exactly this switch. The detail line does
     * not move: what it reveals is unchanged.
     */
    const val ADVANCED_TITLE = "Lab features"

    /** The one detail line under the switch. */
    const val ADVANCED_DETAIL = "Floor plan, merge, cloud, survey tools."

    // ── the Scan screen (item 95) ──────────────────────────────────────────

    const val SCAN_BUTTON = "SCAN"
    const val SCAN_BUTTON_RECORDING = "STOP"
    const val SCAN_BUTTON_STARTING = "CANCEL"
    const val ADVANCED_SHEET_BUTTON = "Advanced"

    /** The Detail row's own label. */
    const val DETAIL_LABEL = "Detail"

    // ── ROUND 23 item 105: the tracking-loss banner ────────────────────────

    /**
     * The owner's request, and the most important five words in the app:
     * scan-070's refused gap measured 73.34° of gyro turn during 4.1 s of
     * blindness. Walking through a loss is what makes a gap unhealable.
     */
    const val TRACKING_LOST = "Tracking lost. Stop. Hold still."

    /** The green confirmation, two seconds, then gone. */
    const val TRACKING_BACK = "OK — keep walking."

    /** The one detail line under the amber banner. */
    fun trackingLostFor(seconds: Long): String = "Lost for ${seconds}s."

    // ── ROUND 23 item 101(b): a refused tap says why ───────────────────────

    /**
     * The scan button is SHOWN and will not start. Before this round that was
     * silent — the owner pressed it, nothing happened, nothing was logged, and
     * he restarted the app between every scan for three rounds. Each of these
     * is both the on-screen reason and the tail of the
     * `[session] start tap refused:` line, so the screen and the log can never
     * tell different stories.
     */
    const val START_NEEDS_SENSOR = "Connect the scanner first."

    /** A seal is still running. */
    const val START_SEALING = "Saving your last scan."

    /** A start is already running — the panel pulses as well (ROUND 21 item 85). */
    const val START_ALREADY = "Already starting…"

    /** A capture is live; the button is a STOP button right now. */
    const val START_ALREADY_RECORDING = "Already recording."

    // ── ROUND 23 item 106(c): the Mid-360 door on the Scan tab ─────────────

    const val MID360_SETUP = "Mid-360 setup"
    const val RTK_SETUP = "RTK position"

    // ── ROUND 24 item 110(a): the Scan page's remaining long wording ───────
    //
    // Round 22 wrote the law and applied it to the Projects tab, Review and
    // the export flow. The Scan screen kept a dozen sentences it did not
    // reach — mostly ROUND 5/6 explanations that were correct, useful once,
    // and then read on every single walk. Each of them is shortened here and
    // its content moved into the tutorial (item 110b), which is the place a
    // twenty-word explanation now belongs.

    /**
     * was: "Mount the D6 flat on the BACK of the phone with its scan fan
     * VERTICAL, then walk forward — the phone's camera + IMU supply the 6-DoF
     * path and the engine sweeps the fan into 3D." (33 words, and "6-DoF" is
     * the design document talking.)
     */
    const val D6_MOUNT_HINT = "Mount flat. Keep the fan vertical."

    /** The one detail line under it. The rest is [com.lidarscan.core.capture.TutorialStep]. */
    const val D6_MOUNT_DETAIL = "The camera tracks where you walk."

    /**
     * was: "Hold the rig still in the pose you will walk with, then tap — the
     * D6's angle on the phone is measured from the phone's own attitude and
     * applied to this scan." (30 words.)
     */
    const val MOUNT_REF_HINT = "Hold still, then tap."
    const val MOUNT_REF_DETAIL = "It measures the D6's angle on your phone."

    /** was: "No mount reference — the pushbroom is running on the bracket's CAD nominal." */
    const val MOUNT_REF_MISSING = "No mount reference yet."

    /**
     * was: "The D6 is clamped on by hand and comes off between scans, so its
     * real angle on the phone differs from the bracket's CAD nominal every
     * session — and that angle lands in every resolved point. Hold the rig in
     * the pose you will walk with, keep it still for about a second, and tap
     * Set mount ref on the capture screen. It measures attitude only; the
     * lever arm still needs the calibration wizard." (69 words, in a sheet.)
     */
    const val MOUNT_REF_WHY = "The D6 comes off between scans, so its angle changes each time."

    /**
     * was: "Without phone tracking the D6 can only record flat fan slices —
     * grant the camera permission (or install ARCore) for a 3D scan."
     */
    const val NO_TRACKING_HINT = "No tracking. Scans would be flat."
    const val NO_TRACKING_DETAIL = "Grant the camera permission for 3D."

    /**
     * was: "Phone tracking degraded — <error>. The recording is unaffected; a
     * COIN-D6 needs tracking to build 3D, so stop and start again if this
     * persists." (24 words plus an error string.)
     */
    const val AR_DEGRADED = "Phone tracking degraded."
    const val AR_DEGRADED_DETAIL = "Recording continues. Stop and start again if it lasts."

    /**
     * was: "Starting fresh will stop and save the current recording first —
     * nothing already captured is lost. The mount reference and other device
     * calibration are kept; scan settings go back to defaults." (30 words in a
     * dialog.)
     */
    const val NEW_CAPTURE_TITLE = "A scan is running."
    const val NEW_CAPTURE_BODY = "It stops and saves first. Settings return to defaults."
    const val NEW_CAPTURE_CONFIRM = "Stop and start fresh"
    const val NEW_CAPTURE_DISMISS = "Keep recording"

    /** was: "Heard you — this start is already running, no need to press again." */
    const val START_HEARD_YOU = "Heard you. Already starting…"

    /**
     * was: "Hold the phone in your scanning pose and keep the camera pointed at
     * the room — furniture and edges an arm's length or more away." (24 words,
     * shown during the one stage the operator is watching the screen.)
     */
    const val START_LOOK_AT = "Point at furniture, an arm away."

    /**
     * was: "No serial device attached. Plug the D6 into USB-C OTG — it appears
     * here as soon as it does."
     *
     * ROUND 25 item 119 dropped the product name: two different lidars now
     * arrive on that same USB-C port, so "the D6" was about to become wrong for
     * half the people reading it. "The scanner" is also the vocabulary rounds
     * 22 and 24 settled on everywhere else — `CaptureAutoConnectController`'s
     * NOTHING_FOUND already says "No scanner found."
     */
    const val NO_USB_DEVICE = "Plug the scanner into USB-C."
    const val NO_USB_DEVICE_DETAIL = "It appears here as soon as it does."

    /**
     * ROUND 25 item 119 — the label under the manual panel's two-option sensor
     * row.
     *
     * The row exists because the port cannot answer the question: a COIN-D6 and
     * an STL-27L use the same connector and the same USB-serial bridge chip, so
     * the only thing that knows which is on the cable is the person holding it.
     * Six words, and the first is what to do.
     */
    const val MANUAL_SERIAL_PICK = "Pick the scanner on this cable."

    /** was: "No self-test step: the live view above is the proof. If points appear, the device works." */
    const val LIVE_VIEW_IS_THE_PROOF = "Points on screen mean it works."

    /**
     * ROUND 28 item 163 — **the 62-word red paragraph's five-word replacement.**
     *
     * `ProcessingScreen` rendered `ProcessingJob`'s gate reason — sixty-two
     * words containing `Mid-360 LIO pipeline`, `pushbroom`, `trajectory`,
     * `registered result`, `ARCore pose stream`, `.lscan` and *"the engine
     * cannot write yet"* — in **error red**, against a six-word instruction law.
     * Nothing had failed. The message means "this scan type is already final",
     * and painting it `bad` told the operator his scan was broken.
     *
     * §D.6 moves the control it belongs to into Review's `⋯` menu and makes
     * this its **disabled** state, in `ink-mute`. They live here rather than in
     * a screen-local object so `WordingLaw` counts their words like every other
     * operator-facing string — which is the guard the original paragraph got
     * past by being generated in `:core`'s jobs package instead of declared
     * here.
     */
    const val PROCESS_AGAIN = "Process again"

    /** Five words; §C.6 allows twelve for a detail. **Never `bad`.** */
    const val ALREADY_FINAL = "Already final. Processed while you walked."

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
        REVIEW_PROCESS,
        REVIEW_PROCESSING,
        PROCESS_AGAIN,
        ADVANCED_TITLE,
        SCAN_BUTTON,
        SCAN_BUTTON_RECORDING,
        SCAN_BUTTON_STARTING,
        ADVANCED_SHEET_BUTTON,
        DETAIL_LABEL,
        // ── ROUND 23 ──
        TRACKING_LOST,
        TRACKING_BACK,
        START_NEEDS_SENSOR,
        START_SEALING,
        START_ALREADY,
        START_ALREADY_RECORDING,
        MID360_SETUP,
        RTK_SETUP,
        trackingLostFor(12),
        // ── ROUND 24 item 110(a) ──
        D6_MOUNT_HINT,
        MOUNT_REF_HINT,
        MOUNT_REF_MISSING,
        NO_TRACKING_HINT,
        NO_TRACKING_DETAIL,
        AR_DEGRADED,
        NEW_CAPTURE_TITLE,
        NEW_CAPTURE_CONFIRM,
        NEW_CAPTURE_DISMISS,
        START_HEARD_YOU,
        START_LOOK_AT,
        NO_USB_DEVICE,
        LIVE_VIEW_IS_THE_PROOF,
        // ── ROUND 25 item 119 ──
        MANUAL_SERIAL_PICK,
        D6_MOUNT_DETAIL,
    )

    /** Every sentence above that is a DETAIL line (twelve words or fewer). */
    val DETAILS: List<String> = listOf(
        ALREADY_FINAL,
        DELETE_BODY,
        MEASURE_DETAIL,
        PIECES_FIXED,
        EXPORT_DETAIL,
        AUTO_PROCESS_FAILED,
        ADVANCED_DETAIL,
        scanInPieces(4),
        fixingProgress(62),
        exportDone("scan-068.ply"),
        // ── ROUND 24 item 110(a) ──
        MOUNT_REF_DETAIL,
        MOUNT_REF_WHY,
        AR_DEGRADED_DETAIL,
        NEW_CAPTURE_BODY,
        NO_USB_DEVICE_DETAIL,
    )

    /** Every sentence above that is an ERROR: what happened + what to do. */
    val ERRORS: List<String> = listOf(
        AUTO_PROCESS_FAILED,
        exportFailed("no room on the phone"),
        // ── ROUND 27 item 134 ──
        processFailed("the recording is incomplete"),
    )
}
