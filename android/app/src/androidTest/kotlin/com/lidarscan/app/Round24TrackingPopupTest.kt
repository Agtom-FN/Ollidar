package com.lidarscan.app

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.lidarscan.app.ui.capture.TrackingLossPopup
import com.lidarscan.app.ui.theme.LidarScanTheme
import com.lidarscan.core.capture.TrackingBanner
import com.lidarscan.core.capture.TrackingBannerState
import com.lidarscan.core.capture.TrackingLossBanners
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 24 item 112 — **the centered popup, and the STOP button underneath
 * it.**
 *
 * The owner's correction to round 23 is that a warning banner at the top of a
 * screen he is not looking at is not a warning. The popup that replaces it has
 * one requirement that is easy to state and easy to get wrong: *"The STOP
 * button must remain visible/tappable beneath or within the popup (a user may
 * want to abandon)."*
 *
 * A `Dialog` would have failed that outright — it is its own window and eats
 * every touch in the app. What ships is a scrim `Box` with a background and no
 * pointer-input modifier, which draws over the screen and consumes nothing.
 * That property is invisible in a screenshot and obvious in a test, so it is
 * tested: a stand-in STOP button sits underneath, the popup is composed over
 * it, and the button is clicked THROUGH the scrim.
 *
 * The rest of the item — that the popup never appears outside a recording, and
 * that it holds until tracking returns — is `TrackingLossBanners`' own
 * behaviour and is unit-tested in `:core`. Nothing about it changed this round;
 * this file is about the presentation that did.
 */
@RunWith(AndroidJUnit4::class)
class Round24TrackingPopupTest {

    @get:Rule
    val composeRule = createComposeRule()

    /** The Scan screen's shape at the moment of a loss: a STOP button, with the popup over it. */
    private fun setContent(state: TrackingBannerState, onStop: () -> Unit = {}) {
        composeRule.setContent {
            LidarScanTheme {
                Box(Modifier.fillMaxSize()) {
                    // Stands in for the transport row's record/STOP control,
                    // in the same place: bottom centre, under everything.
                    Text(
                        "STOP",
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .clickable(onClick = onStop)
                            .testTag("fakeStopButton"),
                    )
                    TrackingLossPopup(state)
                }
            }
        }
    }

    @Test
    fun theLostPopupIsCenteredAndCarriesTheOwnersFiveWords() {
        setContent(TrackingBannerState(TrackingBanner.LOST, sinceMillis = System.currentTimeMillis()))
        composeRule.onNodeWithTag("trackingPopupScrim").assertExists()
        composeRule.onNodeWithTag("trackingLostBanner").assertIsDisplayed()
        composeRule.onNodeWithText(TrackingLossBanners.LOST_TEXT).assertIsDisplayed()
        // The live seconds count — the one detail line.
        composeRule.onNodeWithTag("trackingLostElapsed").assertExists()

        // Centred, not a band at the top: the card's centre must sit in the
        // middle third of the screen.
        val root = composeRule.onNodeWithTag("trackingPopupScrim").fetchSemanticsNode()
        val card = composeRule.onNodeWithTag("trackingLostBanner").fetchSemanticsNode()
        val cardCentre = card.boundsInRoot.center.y
        val height = root.boundsInRoot.height
        assertTrue(
            "the card must be centred, not banded at the top (centre=$cardCentre of $height)",
            cardCentre > height / 3f && cardCentre < height * 2f / 3f,
        )
    }

    /**
     * **The requirement item 112 states outright.** A user may want to abandon
     * the scan, so the control that abandons it stays reachable.
     */
    @Test
    fun theStopButtonIsStillTappableThroughTheScrim() {
        var stops = 0
        setContent(
            TrackingBannerState(TrackingBanner.LOST, sinceMillis = System.currentTimeMillis()),
            onStop = { stops++ },
        )
        composeRule.onNodeWithTag("fakeStopButton").performClick()
        composeRule.waitForIdle()
        assertEquals("STOP must survive the popup — this is the whole of item 112's caveat", 1, stops)
    }

    /** The green half: the same shape, the other colour, no seconds line. */
    @Test
    fun theRegainedPopupSaysKeepWalking() {
        setContent(TrackingBannerState(TrackingBanner.REGAINED, sinceMillis = System.currentTimeMillis()))
        composeRule.onNodeWithTag("trackingBackBanner").assertIsDisplayed()
        composeRule.onNodeWithText(TrackingLossBanners.REGAINED_TEXT).assertIsDisplayed()
        assertEquals(
            "the green card has no count-up — there is nothing left to wait for",
            0,
            composeRule.onAllNodesWithTag("trackingLostElapsed").fetchSemanticsNodes().size,
        )
    }

    /** NONE draws nothing at all — no scrim, no card, no dimmed screen. */
    @Test
    fun nothingIsDrawnWhenTrackingIsFine() {
        setContent(TrackingBannerState())
        assertEquals(
            0,
            composeRule.onAllNodesWithTag("trackingPopupScrim").fetchSemanticsNodes().size,
        )
        composeRule.onNodeWithTag("fakeStopButton").assertIsDisplayed()
    }
}
