package com.lidarscan.core.capture

import com.lidarscan.core.calib.MountTrim

/**
 * ROUND 23 item 106(b) — **the pre-scan checklist, folded into the start
 * panel.**
 *
 * ROUND 22 item 95 said this in one sentence and then deferred it: *"The
 * pre-scan checklist stops being a separate modal — its checks fold into the
 * round-21 `StartProgressPanel` and surface only when a check actually
 * blocks."*
 *
 * The ROUND 19 sheet was right about what to check and wrong about when to say
 * it. It intercepted the FIRST press per device with a modal in front of a
 * screen whose whole design rule (round 5) is that it has no steps — so the
 * operator's first ever Start became two taps, and every check was shown
 * whether or not it had anything to report. A row that reads "Mount: set 4
 * minutes ago · 0.18° measured" is a row nobody needs to read.
 *
 * What survives is the part that earns its interruption: a check with
 * something WRONG to say, said inside the panel that is already on screen for
 * the four to eight seconds the start takes anyway. Nothing is added to the
 * critical path, nothing is deleted — `PreScanChecklistSheet`,
 * `startCapture(skipChecklist)` and their tests are untouched behind
 * `FeatureFlags.PRE_SCAN_CHECKLIST_SHEET`.
 *
 * Each note obeys item 98's law: an instruction of six words or fewer.
 */
object PreScanChecks {

    /** was a row on the sheet; now only appears when the number is bad. */
    const val MOUNT_ROUGH = "Mount reference is rough."

    /** ditto — an unprotected walk is worth one line, not a screen. */
    const val NOTIFICATIONS_UNPROTECTED = "Notifications may interrupt."

    /**
     * The notes worth showing for THIS press, in priority order. Empty is the
     * normal answer and is the point of the change.
     *
     * @param trimAccuracyDeg the mount trim's measured split-half accuracy, or
     *   null when there is no trim (which is not itself a warning — the start
     *   hold measures one, and ROUND 20 made that automatic).
     * @param dndProtected whether the Do Not Disturb filter is actually in
     *   force for this walk.
     */
    fun notesFor(trimAccuracyDeg: Double?, dndProtected: Boolean): List<String> = buildList {
        if (trimAccuracyDeg != null && trimAccuracyDeg > MountTrim.WARN_STABILITY_DEG) add(MOUNT_ROUGH)
        if (!dndProtected) add(NOTIFICATIONS_UNPROTECTED)
    }
}
