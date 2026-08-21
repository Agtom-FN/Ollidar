package com.lidarscan.core.capture

/**
 * ROUND 27 item 142 — **the first user outside the owner could not scan at
 * all, and the app never said so.**
 *
 * An OPPO CPH2499 on Android 15, the first non-Pixel this app has ever run on.
 * Its log is hundreds of lines of
 * `[ar] gate refused FAILED … created=true resumed=true failure=the tracking
 * camera stopped (FatalException)`, across four separate pose pumps, and the
 * user opened the Scan tab six times and never got a scan. Nothing on screen
 * ever changed: the start line sat on its progress sentence while the camera
 * behind it was dead.
 *
 * That is the defect, and it is not the ARCore failure — a vendor power manager
 * taking the camera back from ARCore is a known ecosystem behaviour on ColorOS
 * and this app cannot stop it. The defect is that **a hard failure was
 * indistinguishable from a slow success**. Round 23 fixed exactly this shape one
 * layer up ("a button that will not act must still ANSWER"); this is the same
 * rule applied to the tracker.
 *
 * ## Why the model is here
 *
 * Three states, one predicate, and the predicate has a clock in it (a
 * FatalException that clears in 200 ms is a blink; one that persists for three
 * seconds is a dead camera). That is a unit test, not a screenshot — and the
 * one device that reproduces it is not on this desk.
 */
enum class ArTroubleKind {
    /** Nothing to report. */
    NONE,

    /** Google Play Services for AR is missing or too old. Actionable: send them to the store. */
    NEEDS_INSTALL,

    /** The device genuinely cannot do it. Honest, and not retryable. */
    UNSUPPORTED,

    /**
     * A session was created and resumed and the camera then stopped, and it has
     * stayed stopped. The OPPO's state.
     */
    CAMERA_STOPPED,
}

object ArTrouble {

    /**
     * How long a FAILED gate must persist before the screen calls it a fault.
     *
     * ARCore legitimately drops a frame or two through a hard turn, and round
     * 16's watchdog already learned that a banner which cries wolf on an
     * ordinary corner is a banner the operator stops reading. Three seconds is
     * the same number that watchdog settled on, for the same reason.
     */
    const val FATAL_PERSIST_MILLIS = 3_000L

    // ── the wording, under the law (instruction ≤6 words, detail ≤12) ───────

    const val NEEDS_INSTALL = "Update AR services."
    const val NEEDS_INSTALL_DETAIL = "Position tracking needs Google Play Services for AR."
    const val NEEDS_INSTALL_ACTION = "Update AR services"

    const val UNSUPPORTED = "This phone cannot track position."
    const val UNSUPPORTED_DETAIL = "Scans will be flat. The scanner still records."

    const val CAMERA_STOPPED = "Tracking camera stopped."

    /**
     * The one line that might actually get an OPPO user scanning.
     *
     * ARCore is handed the camera and a vendor power manager takes it away
     * again; the two settings that stop that are the ones named here. It is a
     * DETAIL line rather than an instruction because it is advice about
     * somebody else's settings app, and — like round 26 item 128's adapter
     * advice — it is offered as a thing to try, not as a fix this app has
     * verified on that device.
     */
    const val CAMERA_STOPPED_DETAIL = "Allow camera in background. Disable battery optimisation."

    const val RETRY = "Retry"
    const val SEND_LOGS = "Send logs"

    /**
     * What the Scan page should be showing, given what the app knows.
     *
     * @param availabilityReady ARCore is installed and up to date.
     * @param availabilityNeedsInstall the APK is missing or too old.
     * @param availabilityUnsupported this device cannot run ARCore.
     * @param fatalSinceMillis when the gate first reported a persistent
     *   failure, or null if it is not failing.
     * @param nowMillis the clock.
     */
    fun kindFor(
        availabilityReady: Boolean,
        availabilityNeedsInstall: Boolean,
        availabilityUnsupported: Boolean,
        fatalSinceMillis: Long?,
        nowMillis: Long,
    ): ArTroubleKind = when {
        // Order matters and is a judgement: a missing APK explains everything
        // downstream of it, so saying "tracking camera stopped" to somebody
        // whose ARCore is simply not installed would send them chasing a
        // battery setting for a problem the Play Store fixes.
        availabilityNeedsInstall -> ArTroubleKind.NEEDS_INSTALL
        availabilityUnsupported -> ArTroubleKind.UNSUPPORTED
        fatalSinceMillis != null &&
            nowMillis - fatalSinceMillis >= FATAL_PERSIST_MILLIS -> ArTroubleKind.CAMERA_STOPPED
        else -> ArTroubleKind.NONE
    }.let { kind ->
        // A READY device with no persistent failure has nothing to say, and a
        // device still CHECKING has nothing to say YET — neither is a fault.
        if (kind == ArTroubleKind.CAMERA_STOPPED && !availabilityReady) ArTroubleKind.NONE else kind
    }

    /** The headline for [kind], or null for [ArTroubleKind.NONE]. */
    fun title(kind: ArTroubleKind): String? = when (kind) {
        ArTroubleKind.NONE -> null
        ArTroubleKind.NEEDS_INSTALL -> NEEDS_INSTALL
        ArTroubleKind.UNSUPPORTED -> UNSUPPORTED
        ArTroubleKind.CAMERA_STOPPED -> CAMERA_STOPPED
    }

    /** Its one detail line. */
    fun detail(kind: ArTroubleKind): String? = when (kind) {
        ArTroubleKind.NONE -> null
        ArTroubleKind.NEEDS_INSTALL -> NEEDS_INSTALL_DETAIL
        ArTroubleKind.UNSUPPORTED -> UNSUPPORTED_DETAIL
        ArTroubleKind.CAMERA_STOPPED -> CAMERA_STOPPED_DETAIL
    }

    /**
     * True when the card should offer **Retry**.
     *
     * Never for [ArTroubleKind.UNSUPPORTED]: a retry button on a device that
     * cannot run ARCore is a promise the app cannot keep, and round 27 item
     * 134(a) is the same decision one screen over.
     */
    fun retryable(kind: ArTroubleKind): Boolean =
        kind == ArTroubleKind.CAMERA_STOPPED

    /** Every operator-facing string this item adds, for the wording guard. */
    val INSTRUCTIONS: List<String> = listOf(
        NEEDS_INSTALL,
        UNSUPPORTED,
        CAMERA_STOPPED,
        NEEDS_INSTALL_ACTION,
        RETRY,
        SEND_LOGS,
    )

    val DETAILS: List<String> = listOf(
        NEEDS_INSTALL_DETAIL,
        UNSUPPORTED_DETAIL,
        CAMERA_STOPPED_DETAIL,
    )
}
