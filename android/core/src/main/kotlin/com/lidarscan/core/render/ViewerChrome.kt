package com.lidarscan.core.render

/**
 * ROUND 26 item 126 — **the viewer's controls hide on a tap, and a measure tap
 * is not a tap on empty space.**
 *
 * Three lines of state that would otherwise be three `if`s scattered through a
 * composable, where nothing could check them against each other. They are here
 * because the interesting part is not the animation, it is the ARBITRATION:
 * one finger, one tap, and two things that both want it.
 *
 * The rule, in the order it is decided:
 *
 *  1. **Measure mode owns the tap.** With measure on, a tap is a measurement
 *     point — item 126 says a measure tap is not "empty space" — so it never
 *     toggles the chrome. This is not politeness: the operator is placing two
 *     points to get a distance, and losing the toolbar between them (or, worse,
 *     losing the point because the tap went to the toolbar) is the failure.
 *  2. **Measure mode also FORCES the controls visible.** Turning measure on
 *     while the chrome is hidden would otherwise leave the operator in a state
 *     with no visible way back out of it — the 📏 that turns it off is one of
 *     the controls that is hidden. A mode you cannot leave is a trap.
 *  3. Otherwise a tap flips them.
 *
 * Navigation gestures are deliberately absent from all of this. Orbit, pan,
 * dolly and the double-tap reset go through `PointCloudRenderer`'s own arbiter
 * and keep working whether the chrome is shown or hidden, because hiding the
 * controls is what an operator does in order to LOOK at the cloud.
 */
object ViewerChrome {

    /**
     * What a single confirmed tap on the viewport does.
     *
     * @return the new "controls shown" state.
     */
    fun onViewportTap(controlsShown: Boolean, measureMode: Boolean): Boolean =
        if (measureMode) controlsShown else !controlsShown

    /** True when the tap belongs to the measurement instead of to the chrome. */
    fun tapIsMeasurement(measureMode: Boolean): Boolean = measureMode

    /** Whether the floating controls are drawn at all. */
    fun controlsVisible(controlsShown: Boolean, measureMode: Boolean): Boolean =
        controlsShown || measureMode
}
