package com.lidarscan.core.capture

/**
 * ROUND 13 (owner item 47) — "Do Not Disturb during capture", the part of it
 * that can be decided without an Android framework object, and therefore the
 * part that can be tested.
 *
 * ## Why the owner asked for it, and what this round measured
 *
 * A notification does not merely distract the operator: on a phone that is
 * being used as an inertial measurement device it fires the vibration motor,
 * which shakes the IMU and smears the camera for the ~100-300 ms the buzz
 * lasts. That is a physical corruption of the measurement, not a UX
 * annoyance.
 *
 * The honest field result, from the 400 Hz accelerometer of the owner's own
 * scan-028/029/030, has to be recorded here because it bounds what this
 * feature can claim: **every** high-frequency vibration burst in those three
 * captures maps 1:1 onto one of the app's OWN logged cues (10 bursts / 10
 * cues on scan-030, 1/1 on each of the other two). **No external
 * notification fired during any of them.** So Do Not Disturb is hygiene that
 * removes a real failure mode before it bites — it is not the explanation for
 * scan-030, and this file must not be cited as if it were.
 *
 * One thing that measurement DID find, and it is ours: scan-030's fourth
 * section break at t = 27.63 s came 0.51 s after the 130 ms haptic cue this
 * app fired for the third break at t = 27.12 s, and it is the only one of the
 * four with any high-frequency energy in the half second before it (z = 178
 * against 2.0 / 7.3 / 10.2 for the other three). See [SECTION_BREAK_CUE_NOTE].
 *
 * ## What must NOT be suppressed
 *
 * The operator cannot watch the screen (ROUND 11 item 43), so the app's own
 * haptic and audio cues are the only channel that survives a walk. An
 * interruption filter governs NOTIFICATIONS; a direct `Vibrator.vibrate()`
 * from the foreground app is not a notification and is unaffected by it, and
 * `ToneGenerator` plays on `STREAM_NOTIFICATION`, which
 * `INTERRUPTION_FILTER_ALARMS` would mute. [DESIRED_FILTER] is therefore
 * PRIORITY and not ALARMS or NONE: it silences messages and calls while
 * leaving the app's own audio path alone, which is the weakest filter that
 * does the job. Taking more than is needed from a user's phone is how a
 * default-on feature gets switched off.
 */
enum class DndState {
    /** The filter was changed for this capture and will be put back. */
    PROTECTED,

    /** Policy access has not been granted, so nothing was changed. */
    NO_PERMISSION,

    /** Granted, but the user had already set a filter at least as strict. */
    ALREADY_QUIET,

    /** Granted and attempted, but the framework refused or threw. */
    FAILED,

    /** The operator switched the feature off in Settings. */
    DISABLED,
}

object CaptureFocus {
    /**
     * `NotificationManager.INTERRUPTION_FILTER_PRIORITY`. Duplicated as a plain
     * Int so `:core` stays free of the Android framework; the `:app` side
     * asserts the two agree.
     */
    const val DESIRED_FILTER: Int = 2

    /** `INTERRUPTION_FILTER_ALL` — the "everything through" state. */
    const val FILTER_ALL: Int = 1

    /** `INTERRUPTION_FILTER_NONE` — total silence, stricter than we ask for. */
    const val FILTER_NONE: Int = 3

    /** `INTERRUPTION_FILTER_ALARMS` — stricter than we ask for. */
    const val FILTER_ALARMS: Int = 4

    const val SECTION_BREAK_CUE_NOTE: String =
        "scan-030 break #4 followed this app's own break cue by 0.51 s"

    /**
     * Should the filter be changed, given what the phone is already doing?
     *
     * Only [FILTER_ALL] is worth acting on. If the operator already has DND on
     * — priority, alarms or total silence — the capture is protected by their
     * own choice and touching it would mean *restoring* a weaker filter at
     * stop, i.e. this feature would end up turning someone's DND off.
     */
    fun shouldEngage(currentFilter: Int): Boolean = currentFilter == FILTER_ALL

    /**
     * What to restore at stop. Null means "leave it alone".
     *
     * `previous` is what was read at start, `current` what the phone reads now.
     * If something else moved the filter mid-capture (the user, a work-profile
     * policy, a scheduled Bedtime rule) that is a newer decision than ours and
     * it wins — restoring over it would be this app overriding a live user
     * action several minutes after the fact.
     */
    fun filterToRestore(previous: Int?, current: Int): Int? {
        if (previous == null) return null
        if (current != DESIRED_FILTER) return null
        if (previous == DESIRED_FILTER) return null
        return previous
    }

    /** The stable token that goes in the session-start log line. */
    fun logToken(state: DndState): String = when (state) {
        DndState.PROTECTED -> "protected"
        DndState.NO_PERMISSION -> "unprotected-no-permission"
        DndState.ALREADY_QUIET -> "already-quiet"
        DndState.FAILED -> "unprotected-refused"
        DndState.DISABLED -> "unprotected-disabled"
    }

    /**
     * ROUND 14 — the ask-once flow ROUND 13's brief called for and ROUND 13
     * did not build.
     *
     * 0.8.0 shipped the whole engage/restore machine and no way whatsoever to
     * obtain the permission it needs: `DoNotDisturbGuard.policyAccessIntent()`
     * had zero call sites, and every session in the owner's field log recorded
     * `dnd=unprotected-no-permission` — correctly, and with nothing the owner
     * could do about it. The Settings switch even described the prerequisite
     * ("Needs Do Not Disturb access") while offering no route to satisfy it.
     *
     * The decision is here rather than in the composable so it is testable,
     * and it has exactly three inputs. Ask when the feature is on, the grant
     * is missing, and we have not already asked — once, ever, because a
     * permission the operator declined is an answer and re-asking a person
     * mid-fieldwork is how a tool gets uninstalled. After a decline the
     * capture screen keeps a quiet [note] instead, and Settings keeps a row
     * they can come back to.
     */
    fun shouldAsk(enabled: Boolean, granted: Boolean, alreadyAsked: Boolean): Boolean =
        enabled && !granted && !alreadyAsked

    /**
     * The explainer's title and body. Deliberately leads with the physics
     * rather than the permission: "allow Do Not Disturb access" means nothing
     * to someone holding a lidar, and "a buzz shakes the sensors" means
     * everything.
     */
    const val ASK_TITLE: String = "Silence notifications while scanning?"

    // ROUND 24 item 110(a): was 74 words in a dialog that opens on a first
    // scan. The three things it has to say survive — a buzz shakes the phone,
    // the grant lives on a system screen, and scans work without it — one
    // short line each. The ROUND 14 test asserts the first and the third by
    // their own words, which is why those two clauses are kept verbatim.
    const val ASK_BODY: String =
        "A notification buzz shakes the phone, which is the tracker.\n\n" +
            "Android asks you to allow this.\n\n" +
            "Scans still run without this."

    const val ASK_CONFIRM: String = "Open settings"

    const val ASK_DISMISS: String = "Not now"

    /** The Settings row's status line, so the operator can always see where they stand. */
    // ROUND 24 item 113: shortened to the law. Was "Access granted — scans are
    // silenced and your setting is restored afterwards." / "Access not granted
    // — scans will run unprotected. Tap to open the system screen." Both said
    // three things where one was needed; the word "granted" survives in both,
    // which is what the ROUND 14 tests are actually about.
    fun accessStatus(granted: Boolean): String =
        if (granted) {
            "Access granted. Scans run silenced."
        } else {
            "Access not granted. Scans run unprotected."
        }

    /** One sentence for the capture screen. Null when there is nothing to say. */
    fun note(state: DndState): String? = when (state) {
        DndState.PROTECTED, DndState.ALREADY_QUIET, DndState.DISABLED -> null
        // ROUND 24 item 110(a): both were 22 words on the Scan screen, read on
        // every walk. An instruction and its one detail line instead.
        DndState.NO_PERMISSION ->
            "Notifications are not silenced.\nAllow Do Not Disturb in Settings."
        DndState.FAILED ->
            "Notifications are not silenced.\nA buzz can break tracking mid-walk."
    }
}
