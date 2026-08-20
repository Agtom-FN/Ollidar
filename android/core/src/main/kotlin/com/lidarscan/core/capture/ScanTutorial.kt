package com.lidarscan.core.capture

/**
 * ROUND 24 item 110(b) — **the guided tour of the Scan screen.**
 *
 * Every round since 17 has ended with a sentence on the Scan screen explaining
 * something: what the hold is for, what a lost tracker means, why the button is
 * dimmed. Round 22 then made every one of those sentences six words long, which
 * was right — and which left the app with nowhere to put the twenty words that
 * genuinely have to be said *once*. This is that place.
 *
 * Six steps, each one control, each with a title of six words or fewer and a
 * body of twelve or fewer, so the tour obeys the same law as the screen it
 * describes rather than being the one place jargon is allowed back in.
 *
 * ## Why a state machine in `:core`
 *
 * The same reason as `TrackingLossBanners` and `OperatorCues`: everything
 * interesting about a tour is sequencing and persistence — what Next does on
 * the last step, what Skip does on the first, whether an interrupted tour comes
 * back, whether the offer is ever made twice. A unit test can hold all of that
 * still; an emulator can only watch it.
 *
 * The Compose overlay draws [TutorialState.step] and calls [next] / [skip]. It
 * owns no rules.
 */
enum class TutorialStep(
    /** Which control this step rings. Null means "no single control" — see [TutorialAnchor]. */
    val anchor: TutorialAnchor,
    val title: String,
    val body: String,
) {
    /**
     * First, because it is the only control on the screen that has to be
     * understood, and because a tour that opens on something peripheral reads
     * as a tour of trivia.
     */
    SCAN_BUTTON(
        anchor = TutorialAnchor.SCAN_BUTTON,
        title = "Tap SCAN to start.",
        body = "Tap it again to stop. Your scan saves itself.",
    ),

    /**
     * ROUND 20 item 78's hold, explained BEFORE the first scan rather than
     * discovered during it. The owner spent two field sessions learning what
     * the app was doing in those four seconds.
     */
    START_HOLD(
        anchor = TutorialAnchor.SCAN_BUTTON,
        title = "Hold still after tapping.",
        body = "It measures the mount for a few seconds. Then walk.",
    ),

    /** The chips are the only read-out during a walk. */
    STATUS_CHIPS(
        anchor = TutorialAnchor.CHIP_ROW,
        title = "These chips show your state.",
        body = "Sensor, tracking and scan name, at a glance.",
    ),

    /** ROUND 22 item 95's one door. */
    ADVANCED(
        anchor = TutorialAnchor.ADVANCED,
        title = "Advanced holds the settings.",
        body = "Detail, display and the reset all live here.",
    ),

    /**
     * ROUND 23 item 105 / ROUND 24 item 112. This is the step the tour exists
     * for: scan-070 lost 4.1 seconds and 73 degrees of turn because nothing
     * had ever said what to do.
     */
    TRACKING_LOST(
        anchor = TutorialAnchor.VIEWPORT,
        title = "If tracking is lost, stop.",
        body = "An amber card appears. Stand still until it clears.",
    ),

    /** Where the thing you just made went. */
    PROJECTS(
        anchor = TutorialAnchor.PROJECTS_TAB,
        title = "Finished scans land in Projects.",
        body = "Tap one to open it, export it or share it.",
    ),
    ;

    /** The step after this one, or null when the tour is over. */
    val next: TutorialStep? get() = entries.getOrNull(ordinal + 1)

    /** 1-based, for "3 of 6". */
    val number: Int get() = ordinal + 1
}

/**
 * What a step points at.
 *
 * A separate enum from [TutorialStep] because two steps legitimately share a
 * target (the SCAN button is both "how to start" and "what the hold is"), and
 * because the Compose side registers targets by anchor — a screen that has not
 * composed a given control simply has no bounds for it, and the overlay centres
 * its card instead of ringing nothing. That degradation is deliberate: the tour
 * must work on the disconnected ready screen, which is the screen a first-run
 * operator is actually looking at.
 */
enum class TutorialAnchor {
    SCAN_BUTTON,
    CHIP_ROW,
    ADVANCED,
    VIEWPORT,

    /**
     * The Projects tab, which lives in the app shell rather than on this
     * screen. The overlay's scrim is drawn INSIDE the Scan screen and the
     * floating tab bar is drawn over it by `LidarScanApp`, so on this step the
     * tab bar is the one bright thing on a dimmed screen without the overlay
     * having to reach into another composable to arrange it.
     */
    PROJECTS_TAB,
}

/**
 * The tour's whole state: which step, or none.
 *
 * `null` is "not running", rather than a `running: Boolean` beside a step,
 * because the two-field version has an unrepresentable-but-constructible state
 * (`running = true, step = null`) and this one does not.
 */
data class TutorialState(val step: TutorialStep? = null) {
    val running: Boolean get() = step != null
    val isLastStep: Boolean get() = step != null && step.next == null
}

object ScanTutorial {

    /** How many steps the tour has. Item 110 caps it at eight; it uses six. */
    val stepCount: Int get() = TutorialStep.entries.size

    /** The offer, once, on a first launch after install. */
    const val OFFER_TITLE = "New here? Take the tour."

    /** The offer's two buttons. */
    const val OFFER_ACCEPT = "Take the tour"
    const val OFFER_DISMISS = "No thanks"

    /** The overlay's own two buttons. */
    const val NEXT = "Next"
    const val SKIP = "Skip"

    /** The last step's button, because "Next" on the last step is a lie. */
    const val DONE = "Done"

    /** The Scan screen's ? button, described for a screen reader. */
    const val HELP_LABEL = "Show the tour"

    /** The Settings row that replays it. */
    const val SETTINGS_ROW = "Tutorial"
    const val SETTINGS_DETAIL = "Walk through the Scan screen again."

    /** Start at the beginning. Replaying a finished tour is the same call. */
    fun start(): TutorialState = TutorialState(TutorialStep.entries.first())

    /**
     * Advance. The last step's Next **ends** the tour, which is why the button
     * says "Done" there — a Next that silently closes is how a tour leaves
     * someone wondering whether they missed a step.
     */
    fun next(state: TutorialState): TutorialState =
        TutorialState(state.step?.next)

    /** Skip, from anywhere, including the last step. */
    fun skip(): TutorialState = TutorialState(null)

    /** The forward button's word for [state]. */
    fun advanceLabel(state: TutorialState): String = if (state.isLastStep) DONE else NEXT

    /** "2 of 6". Not an instruction — a position. */
    fun progressLabel(state: TutorialState): String =
        state.step?.let { "${it.number} of $stepCount" }.orEmpty()

    /**
     * **Should the offer be made?**
     *
     * Exactly once per install, and never once the tour has been seen by any
     * road (the ? button counts). Both flags are persisted; this function is
     * the only place the two are combined, so "never auto-repeats" is one
     * testable rule rather than a condition copied into a composable.
     */
    fun shouldOffer(tutorialSeen: Boolean, offerMade: Boolean): Boolean =
        !tutorialSeen && !offerMade

    /** Every operator-facing string this item adds, for the wording guard. */
    val INSTRUCTIONS: List<String> = listOf(
        OFFER_TITLE,
        OFFER_ACCEPT,
        OFFER_DISMISS,
        NEXT,
        SKIP,
        DONE,
        HELP_LABEL,
        SETTINGS_ROW,
    ) + TutorialStep.entries.map { it.title }

    val DETAILS: List<String> = listOf(SETTINGS_DETAIL) + TutorialStep.entries.map { it.body }
}
