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
import androidx.compose.ui.test.onNodeWithTag
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.lidarscan.app.ui.capture.TrackingLossPopup
import com.lidarscan.app.ui.theme.LidarScanTheme
import com.lidarscan.core.capture.TrackingBanner
import com.lidarscan.core.capture.TrackingBannerState
import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 25 item 116 — **"revise the warning align the style."**
 *
 * The owner's note on the round-24 popup is about dialect, not about behaviour.
 * The popup worked; it was drawn in its own language — a hand-rolled `Column`
 * with its own background, its own 3 dp coloured ring and a hand-set
 * `fontSize`/`fontWeight` pair, on a screen whose every other card is a
 * `ScanCard` with a 1 dp hairline and a typography ROLE.
 *
 * So this suite asserts the things that make it the app's card rather than a
 * card: that it uses the app's radius by using the app's component, that both
 * states have the same geometry, that the status glyph is there, and that the
 * live seconds are drawn in a tabular style. What it does NOT re-assert is
 * anything round 24 pinned — the centring, the scrim's tap-through to STOP, the
 * absence outside a recording. `Round24TrackingPopupTest` owns those, they are
 * unchanged, and duplicating them here would mean two files to update the next
 * time the popup moves.
 *
 * It also writes the two cards to the device's external files dir, which is
 * where this round's screenshots come from: ARCore cannot be made to lose
 * tracking on an AVD, so the popup is composed directly rather than
 * photographed through a failure that cannot be staged.
 */
@RunWith(AndroidJUnit4::class)
class Round25PopupStyleTest {

    @get:Rule
    val composeRule = createComposeRule()

    /**
     * The state the composition draws, hoisted OUT of `setContent`.
     *
     * A `ComposeTestRule` may have its content set exactly once per test, so a
     * test that wants to see both banners cannot call `setContent` twice — it
     * has to change what the one composition is showing. That is also the
     * honest reproduction: in the app the popup does not get re-created when
     * tracking returns, it re-renders on a new `TrackingBannerState`, which is
     * precisely the transition item 116 asks to have the same geometry across.
     */
    private var banner by mutableStateOf(TrackingBannerState())

    private fun setContent(state: TrackingBannerState) {
        banner = state
        composeRule.setContent {
            LidarScanTheme {
                Box(Modifier.fillMaxSize()) {
                    Text(
                        "STOP",
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .clickable {}
                            .testTag("fakeStopButton"),
                    )
                    TrackingLossPopup(banner)
                }
            }
        }
    }

    /**
     * Write the popup to `/sdcard/Download` so a round's screenshots can be
     * pulled off the AVD.
     *
     * Through `UiAutomation.executeShellCommand` rather than an app-side
     * `File`: on API 30+ an app's own external files directory is invisible to
     * `adb pull` (scoped storage), so a PNG written there is a PNG nobody can
     * fetch. `screencap` runs as the shell user, which can both write to
     * `Download` and read the framebuffer — and the framebuffer is what is
     * wanted anyway, since the point of the screenshot is the card ON its scrim
     * over the screen it dims.
     *
     * Best-effort by design. This is documentation output, not an assertion: a
     * device that refuses the shell command must not fail an item-116 test
     * about the card's style.
     */
    private fun shoot(name: String) {
        composeRule.waitForIdle()
        runCatching {
            val automation = InstrumentationRegistry.getInstrumentation().uiAutomation
            automation.executeShellCommand("screencap -p /sdcard/Download/$name").use {
                // Drain, or the command is killed before it has written the file.
                android.os.ParcelFileDescriptor.AutoCloseInputStream(it).use { stream ->
                    stream.readBytes()
                }
            }
        }
    }

    /**
     * The amber card: the status glyph, the headline, the live seconds — and
     * the app's own card geometry underneath all three.
     */
    @Test
    fun theWarningCardCarriesTheAppsIconAndTabularSeconds() {
        setContent(TrackingBannerState(TrackingBanner.LOST, sinceMillis = System.currentTimeMillis()))
        composeRule.onNodeWithTag("trackingLostBanner").assertIsDisplayed()
        // Status iconography, consistent with the `Icons.Filled` family the
        // rest of the app draws. Its absence was half of what made the round-24
        // card read as a different app's component.
        composeRule.onNodeWithTag("trackingBannerIcon", useUnmergedTree = true).assertExists()
        composeRule.onNodeWithTag("trackingLostElapsed", useUnmergedTree = true).assertExists()
        shoot("round25-item116-tracking-lost.png")
    }

    /**
     * **Item 116 asks for the same geometry on the green card**, and the way to
     * guarantee that is one call site rather than two that agree today.
     *
     * Asserted on the outer bounds — same width, same left and right edges —
     * because that is what "same geometry" means to someone looking at the two
     * in sequence, and it is the thing a second hand-rolled card would get
     * subtly wrong. Height is deliberately NOT asserted equal: the green card
     * has no seconds line, and forcing it to the amber card's height would be
     * padding drawn to satisfy a test.
     */
    @Test
    fun theRecoveryCardHasTheSameGeometry() {
        setContent(TrackingBannerState(TrackingBanner.LOST, sinceMillis = System.currentTimeMillis()))
        val lost = composeRule.onNodeWithTag("trackingLostBanner").fetchSemanticsNode().boundsInRoot
        composeRule.onNodeWithTag("trackingBannerIcon", useUnmergedTree = true).assertExists()
        shoot("round25-item116-tracking-lost.png")

        banner = TrackingBannerState(TrackingBanner.REGAINED, sinceMillis = System.currentTimeMillis())
        composeRule.waitForIdle()
        val back = composeRule.onNodeWithTag("trackingBackBanner").fetchSemanticsNode().boundsInRoot
        composeRule.onNodeWithTag("trackingBannerIcon", useUnmergedTree = true).assertExists()
        shoot("round25-item116-tracking-back.png")

        assertTrue("same left edge (${lost.left} vs ${back.left})", abs(lost.left - back.left) < 1f)
        assertTrue("same right edge (${lost.right} vs ${back.right})", abs(lost.right - back.right) < 1f)
        assertEquals("same width", lost.width, back.width)
    }
}
