package com.lidarscan.core.welcome

/**
 * ROUND 32 item 177 / **ROUND 34 item 181** — **which welcome animation
 * plays, when, and why.**
 *
 * There are now **two** gates in here and they are deliberately separate
 * functions rather than one function with a mode flag, because they answer
 * two different questions about two different events:
 *
 *  * [variantFor] answers *"this process just started — is there a film?"*,
 *    and the answer is always **A** or nothing.
 *  * [eggFor] answers *"developer mode was just switched ON — is there a
 *    film?"*, and the answer is always **B** or nothing.
 *
 * Round 32 had one function and one event: B *replaced* A while developer mode
 * was on. The owner's order in round 34 is that a developer should still see
 * the app's own welcome film every morning, and should see the joke **at the
 * moment he unlocks the thing** — once, on the seventh tap that turns
 * developer mode ON. So B stops being a launch animation and becomes feedback
 * for an action, which is why the launch toggle no longer governs it (item
 * 181(d)) and why reduced motion still does.
 *
 * Both live somewhere a bare JVM can reach, because these are the kind of
 * conditions that are easy to write and hard to notice being wrong. Round 24's
 * tutorial offer is the cautionary tale — "once, ever" lived inside a
 * composable and had to be fixed twice.
 *
 * The animation itself is Compose (see `com.lidarscan.app.ui.welcome`); what
 * lives here is the two gates in front of it and the [WelcomeTimeline] behind
 * them.
 */
object WelcomeAnimation {

    /**
     * The **film**: both variants are exactly three seconds, owner-approved at
     * that number on 2026-08-22. Not a range and not a maximum — the
     * storyboard's keyframes are percentages of this, so changing it re-times
     * both films.
     */
    const val DURATION_MS: Int = 3_000

    /**
     * ROUND 35 item 187 — **the beat the finished film is looked at for.**
     *
     * > *"the welcome animation please run all the frame then wait for 1s then
     * > open the app."* — the owner, 2026-08-23.
     *
     * Round 32 dissolved A into the app on the flash's own tail: the overlay
     * began fading at 90 % of the three seconds, so the last thing the film did
     * was disappear while it was still happening. The owner wants it to
     * **finish** — and then to be there for a second, on its resting pose, with
     * the lidar seated and lit — and only then to get out of the way.
     *
     * A hold and not a longer film, for the reason the constant above gives:
     * every keyframe in [WelcomeTimeline] is a percentage of [DURATION_MS], so
     * stretching the film to four seconds would re-time the toss, the landing
     * and the flash rather than add a pause after them.
     */
    const val HOLD_MS: Int = 1_000

    /**
     * How long an overlay of [variant] is on screen in total.
     *
     * Only the launch film holds. The egg is feedback for a toggle rather than
     * something to be admired, and a second of a llama looking pleased with
     * itself after the joke has landed is a second of a Settings page nobody
     * can touch (item 187(e)).
     */
    fun totalMsFor(variant: Variant): Int = when (variant) {
        Variant.LIDAR_FLIP -> DURATION_MS + HOLD_MS
        Variant.LLAMA_SPIT -> DURATION_MS
    }

    /**
     * The fraction of the **film** to draw, [totalMsFor] fractions in.
     *
     * Clamped at 1, which is the hold: the film's last frame is held rather
     * than a separate still being drawn, so there is exactly one description of
     * what the resting pose looks like and it is the one the last keyframe of
     * every track in [WelcomeTimeline] already gives.
     */
    fun filmProgress(variant: Variant, totalProgress: Float): Float {
        val total = totalMsFor(variant)
        return (totalProgress * total / DURATION_MS).coerceIn(0f, 1f)
    }

    /** Which film plays. */
    enum class Variant {
        /**
         * A — every cold launch. The lidar puck flips off the llama's head
         * dark, turns once, lands, and the scan light goes off.
         */
        LIDAR_FLIP,

        /**
         * B — the easter egg. The llama turns to face you and spits at the
         * glass, once, at the moment developer mode is toggled.
         *
         * ROUND 34 item 181: it used to replace [LIDAR_FLIP] on every cold
         * launch while developer mode was on. It is never a launch film now —
         * see [eggFor].
         */
        LLAMA_SPIT,
    }

    /**
     * Everything a launch knows about itself at the moment the start
     * destination first composes.
     *
     * @param enabled Settings → Display → "Welcome animation". Default ON.
     *   ROUND 34 item 181(d): this switch governs **A only**. The egg is
     *   feedback for an action rather than a launch animation, so it is not
     *   this switch's business.
     * @param reducedMotion the platform's reduce-motion equivalent — see
     *   `WelcomeReducedMotion`. **Skips entirely**, rather than freezing a
     *   frame: a static picture held for three seconds is the same three
     *   seconds of not being able to use the app, minus the reason.
     * @param firstInProcess false for every composition after the first in
     *   this process — a tab switch, a back press, a rotation. Process start
     *   is the event; an Activity is not.
     */
    data class Launch(
        val enabled: Boolean = true,
        val reducedMotion: Boolean = false,
        val firstInProcess: Boolean = true,
    )

    /**
     * The launch gate. `null` means show nothing and start the app.
     *
     * Order matters only for readability — the three refusals are disjoint in
     * effect — but it is written cheapest-first so the overwhelmingly common
     * case (every launch after the first) costs one boolean.
     *
     * ROUND 34 item 181(a): there is no developer branch here any more. A cold
     * launch plays **A** or nothing, whoever is holding the phone.
     */
    fun variantFor(launch: Launch): Variant? = when {
        !launch.firstInProcess -> null
        !launch.enabled -> null
        launch.reducedMotion -> null
        else -> Variant.LIDAR_FLIP
    }

    /**
     * ROUND 34 item 181(b) — **a developer-mode toggle, as the app observed
     * it.**
     *
     * The observation is a *pair*, not a value, and that is the whole design.
     * The app learns developer mode by collecting the settings store, and the
     * store's **first** emission after a process start is not a toggle — it is
     * the state the app was already in. A gate written against the value alone
     * would fire the egg on every cold launch of a developer's phone, which is
     * precisely the behaviour item 181 exists to remove.
     *
     * @param from the previously observed value, or `null` if nothing has been
     *   observed yet in this process. `null` is *"this is the launch reading"*
     *   and never an egg.
     * @param to the value just observed. **ROUND 34 item 183(a)**: only
     *   `false` → `true` is an egg. Item 181 asked for both directions and the
     *   owner amended it after seeing it: locking developer mode away is
     *   tidying up, and being sung at for tidying up gets old on the second
     *   time. The reward belongs on the unlock.
     * @param reducedMotion the same platform read [Launch] uses. The egg still
     *   respects it (item 181(d)); a person who has turned animations off has
     *   said what they want, and a joke is not an exception.
     */
    data class DeveloperToggle(
        val from: Boolean?,
        val to: Boolean,
        val reducedMotion: Boolean = false,
    )

    /**
     * The egg gate. `null` means this was not an unlock, or motion is off.
     *
     * **One direction only** (item 183(a)): the transition `false` → `true`.
     * The re-lock is silent.
     *
     * There is no persistence and there is deliberately none: item 181(e) says
     * a process death between the toggle and the play loses the play. An egg
     * that arrived on the next launch to settle a debt from yesterday would be
     * a launch animation again, by a slower road.
     */
    fun eggFor(toggle: DeveloperToggle): Variant? = when {
        toggle.from == null -> null
        toggle.from -> null
        !toggle.to -> null
        toggle.reducedMotion -> null
        else -> Variant.LLAMA_SPIT
    }
}
