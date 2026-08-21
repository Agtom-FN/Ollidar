package com.lidarscan.core.capture

/**
 * ROUND 27 item 133(a) — **where the coach-mark goes.**
 *
 * Round 24 already had the rule ("the card goes to whichever half of the screen
 * the spotlight is NOT in") and expressed it as one line inside the Compose
 * overlay: `spot.center.y > screenMiddle`. Two things were wrong with that, and
 * neither could fail a test because the rule was not a function.
 *
 * **It compared the wrong two numbers.** The spotlight's bounds are in ROOT
 * pixels — the whole window, status bar included — and the middle it was
 * compared against came from `Configuration.screenHeightDp`, which on this
 * phone is 24 dp shorter than the window. A rule about halves decided by a
 * midpoint that is not the midpoint is a rule that flips near the boundary.
 *
 * **It used the spotlight's CENTRE.** For a target that fills most of the
 * screen — `TutorialAnchor.VIEWPORT`, the tracking-lost step, whose spotlight
 * is the entire live view — the centre is the middle of the screen, so the
 * comparison decides on a rounding error and then places a card over one end
 * of a target that occupies both. What matters is not where the target's centre
 * is but **how much clear room is left above and below it**.
 *
 * So the rule is stated in those terms and is a pure function of three numbers,
 * which makes it a unit test rather than a screenshot.
 */
object TutorialCardPlacement {

    /** Which end of the screen the card takes. */
    enum class Half { TOP, BOTTOM, CENTER }

    /**
     * The share of the screen a half must have clear before the card will go
     * there.
     *
     * The card is a title, a body line and a button row — about 150 dp on a
     * 890 dp phone, which is 0.17. 0.22 leaves margin for a two-line title in a
     * large font scale. Below it, both halves are refused and the card centres:
     * a target that leaves no clear half (the fullscreen viewport) cannot be
     * avoided, and pretending to avoid it by picking the marginally larger
     * sliver would put the card half over the target and half off the screen.
     */
    const val MIN_CLEAR_FRACTION = 0.22f

    /**
     * Where to put the card for a spotlight spanning [spotTop]..[spotBottom]
     * in a window [screenHeight] tall. All three in the SAME units — pass
     * pixels, pass dp, but do not mix, which is the bug this replaces.
     *
     * A null spotlight (a control this screen has not composed — the Projects
     * tab lives in the app shell) centres, which is round 24's deliberate
     * degradation and is unchanged.
     */
    fun cardHalf(spotTop: Float?, spotBottom: Float?, screenHeight: Float): Half {
        if (spotTop == null || spotBottom == null || screenHeight <= 0f) return Half.CENTER
        val clearAbove = spotTop.coerceAtLeast(0f)
        val clearBelow = (screenHeight - spotBottom).coerceAtLeast(0f)
        val floor = screenHeight * MIN_CLEAR_FRACTION
        return when {
            clearAbove < floor && clearBelow < floor -> Half.CENTER
            // Ties go DOWN. A card at the bottom is under the thumb that will
            // press Next, and the two steps that tie in practice ring the SCAN
            // button, which is at the bottom — so a tie here never happens
            // without the target being off-centre anyway.
            clearAbove > clearBelow -> Half.TOP
            else -> Half.BOTTOM
        }
    }
}
