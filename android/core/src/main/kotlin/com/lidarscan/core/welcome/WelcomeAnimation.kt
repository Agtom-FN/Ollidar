package com.lidarscan.core.welcome

/**
 * ROUND 32 item 177 — **which welcome animation a launch gets, if any.**
 *
 * The decision is four booleans wide and it is worth having somewhere a bare
 * JVM can reach, because three of the four are the kind of condition that is
 * easy to write and hard to notice being wrong: *cold launch only*, *developer
 * mode selects B*, *reduced motion means nothing at all*. Round 24's tutorial
 * offer is the cautionary tale — "once, ever" lived inside a composable and
 * had to be fixed twice.
 *
 * The animation itself is Compose (see `com.lidarscan.app.ui.welcome`); what
 * lives here is the gate in front of it and the [WelcomeTimeline] behind it.
 */
object WelcomeAnimation {

    /**
     * Both variants are **exactly** three seconds, owner-approved at that
     * number on 2026-08-22. Not a range and not a maximum: the storyboard's
     * keyframes are percentages of this, so changing it re-times both films.
     */
    const val DURATION_MS: Int = 3_000

    /** Which film plays. */
    enum class Variant {
        /**
         * A — every cold launch. The lidar puck flips off the llama's head
         * dark, turns once, lands, and the scan light goes off.
         */
        LIDAR_FLIP,

        /**
         * B — replaces [LIDAR_FLIP] while developer mode is on. The llama
         * turns to face you and spits at the glass.
         */
        LLAMA_SPIT,
    }

    /**
     * Everything a launch knows about itself at the moment the start
     * destination first composes.
     *
     * @param enabled Settings → Display → "Welcome animation". Default ON.
     * @param developerMode the seven-tap unlock (`AppSettings.developerMode`).
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
        val developerMode: Boolean = false,
        val reducedMotion: Boolean = false,
        val firstInProcess: Boolean = true,
    )

    /**
     * The whole gate. `null` means show nothing and start the app.
     *
     * Order matters only for readability — the three refusals are disjoint in
     * effect — but it is written cheapest-first so the overwhelmingly common
     * case (every launch after the first) costs one boolean.
     */
    fun variantFor(launch: Launch): Variant? = when {
        !launch.firstInProcess -> null
        !launch.enabled -> null
        launch.reducedMotion -> null
        launch.developerMode -> Variant.LLAMA_SPIT
        else -> Variant.LIDAR_FLIP
    }
}
