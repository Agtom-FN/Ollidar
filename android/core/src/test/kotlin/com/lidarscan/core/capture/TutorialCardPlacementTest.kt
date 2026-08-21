package com.lidarscan.core.capture

import com.lidarscan.core.capture.TutorialCardPlacement.Half
import com.lidarscan.core.capture.TutorialCardPlacement.cardHalf
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * ROUND 27 item 133(a) — the coach-mark never covers what it is explaining.
 *
 * The rule was one line inside a composable and could only be checked by
 * looking at a screenshot of one step on one screen size. Here it is six cases
 * on a bare JVM, including the two the old line got wrong: a spotlight near the
 * midpoint (where comparing the CENTRE against a midpoint taken from a
 * different coordinate space flips the answer) and a spotlight that fills the
 * screen (where there is no clear half at all).
 */
class TutorialCardPlacementTest {

    private val screen = 2400f

    @Test
    fun aTargetLowOnTheScreenPutsTheCardAtTheTop() {
        // The SCAN button: the bottom ~200 px of a 2400 px window.
        assertEquals(Half.TOP, cardHalf(spotTop = 2150f, spotBottom = 2380f, screenHeight = screen))
    }

    @Test
    fun aTargetHighOnTheScreenPutsTheCardAtTheBottom() {
        // The gear: the top-end corner.
        assertEquals(Half.BOTTOM, cardHalf(spotTop = 60f, spotBottom = 200f, screenHeight = screen))
    }

    @Test
    fun aTargetThatFillsTheScreenCentresTheCard() {
        // `TutorialAnchor.VIEWPORT` on the fullscreen Scan tab. Neither half is
        // clear, so neither half is claimed — the old rule compared the spot's
        // centre against the screen's middle and decided this on a rounding
        // error, then placed the card over one end of a target that occupies
        // both.
        assertEquals(Half.CENTER, cardHalf(spotTop = 0f, spotBottom = screen, screenHeight = screen))
    }

    @Test
    fun aTargetJustBelowTheMiddleStillLeavesMoreRoomAbove() {
        // 1250..1450 of 2400: 1250 px clear above, 950 clear below. The card
        // goes where the room is, which is the top — and this is exactly the
        // band where a midpoint borrowed from `Configuration.screenHeightDp`
        // (24 dp shorter than the window) would have answered differently.
        assertEquals(Half.TOP, cardHalf(spotTop = 1250f, spotBottom = 1450f, screenHeight = screen))
    }

    @Test
    fun noSpotlightCentres() {
        // The Projects tab lives in the app shell, not on this screen, so it
        // never registers bounds. Round 24's deliberate degradation, kept.
        assertEquals(Half.CENTER, cardHalf(spotTop = null, spotBottom = null, screenHeight = screen))
        assertEquals(Half.CENTER, cardHalf(spotTop = 10f, spotBottom = 20f, screenHeight = 0f))
    }

    @Test
    fun aHalfTooShallowForTheCardIsRefusedRatherThanSqueezed() {
        // 400 px clear above (0.17 of the screen) and 300 below: the larger of
        // the two is still under `MIN_CLEAR_FRACTION`, so the card centres
        // instead of being placed in a sliver it does not fit in.
        assertEquals(Half.CENTER, cardHalf(spotTop = 400f, spotBottom = 2100f, screenHeight = screen))
    }

    @Test
    fun everyTutorialStepThatRingsAControlHasAHalfToGoTo() {
        // A property rather than an example: for every anchor whose spotlight is
        // a real control (i.e. not the fullscreen viewport), a card placement
        // exists that is not CENTER — so no step can silently start covering
        // its own subject when a control moves.
        val controlSpots = mapOf(
            TutorialAnchor.SCAN_BUTTON to (2150f to 2380f),
            TutorialAnchor.ADVANCED to (60f to 200f),
            TutorialAnchor.CHIP_ROW to (300f to 420f),
            TutorialAnchor.PROJECTS_TAB to (2260f to 2380f),
        )
        TutorialStep.entries.forEach { step ->
            val spot = controlSpots[step.anchor] ?: return@forEach
            val half = cardHalf(spot.first, spot.second, screen)
            assertEquals(
                "step ${step.number} (${step.anchor}) must not centre over its own target",
                true,
                half != Half.CENTER,
            )
        }
    }
}
