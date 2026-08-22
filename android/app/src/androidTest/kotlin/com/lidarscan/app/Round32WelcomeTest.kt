package com.lidarscan.app

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.click
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performTouchInput
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.lidarscan.app.ui.theme.LidarScanTheme
import com.lidarscan.app.ui.welcome.WelcomeLaunchGate
import com.lidarscan.app.ui.welcome.WelcomeOverlay
import com.lidarscan.core.welcome.WelcomeAnimation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.atomic.AtomicInteger

/**
 * ROUND 32 item 177 — **the welcome film, on a device.**
 *
 * The `:core` suite pins the storyboard's arithmetic and cannot see a screen.
 * What only a device can answer is the part the item states as behaviour rather
 * than as timing: the overlay is really up over the page, a touch anywhere
 * removes it *at once* and leaves nothing behind, and the page underneath is
 * the thing the touch must **not** have pressed.
 *
 * Both films are composed here — the production `WelcomeOverlay`, not a copy —
 * because B is the one a developer sees and "it compiles" is not evidence that
 * a `Path.op` union and a nine-scallop crown draw on a real GPU.
 */
@RunWith(AndroidJUnit4::class)
class Round32WelcomeTest {

    @get:Rule
    val composeRule = createComposeRule()

    /** The gate is process-wide, and an instrumentation process outlives every launch it tests. */
    @Before
    fun resetGate() = WelcomeLaunchGate.resetForTest()

    /** The page the film covers, and the count of times the film let a touch through to it. */
    private val pageTaps = AtomicInteger(0)

    private fun stage(variant: WelcomeAnimation.Variant, onFinished: () -> Unit) {
        composeRule.setContent {
            LidarScanTheme {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    Box(Modifier.fillMaxSize()) {
                        Box(
                            Modifier
                                .fillMaxSize()
                                .testTag("pageUnderneath")
                                .clickable { pageTaps.incrementAndGet() },
                        ) { Text("the app", Modifier.testTag("pageLabel")) }
                        WelcomeOverlay(variant = variant, onFinished = onFinished)
                    }
                }
            }
        }
    }

    @Test
    fun theOverlayIsUpAndOneTouchRemovesIt() {
        var finished = 0
        composeRule.mainClock.autoAdvance = false
        stage(WelcomeAnimation.Variant.LIDAR_FLIP) { finished++ }

        composeRule.mainClock.advanceTimeBy(300)
        composeRule.onNodeWithTag("welcomeOverlay").assertIsDisplayed()

        composeRule.onNodeWithTag("welcomeOverlay").performTouchInput { click() }
        composeRule.mainClock.advanceTimeBy(16)
        assertEquals("the skip must fire exactly once", 1, finished)
        assertEquals("the touch must not reach the page underneath", 0, pageTaps.get())
    }

    /**
     * Idempotent, which is the half of "zero residue" a screenshot cannot show:
     * a second and third touch on the way out must not fire a second skip, and
     * the film must not restart.
     */
    @Test
    fun skippingTwiceStillFinishesOnce() {
        var finished = 0
        composeRule.mainClock.autoAdvance = false
        stage(WelcomeAnimation.Variant.LIDAR_FLIP) { finished++ }
        composeRule.mainClock.advanceTimeBy(200)

        repeat(3) {
            composeRule.onNodeWithTag("welcomeOverlay").performTouchInput { click() }
            composeRule.mainClock.advanceTimeBy(16)
        }
        assertEquals(1, finished)
    }

    /** Left alone, it ends by itself — once, at three seconds, and not before. */
    @Test
    fun itFinishesOnItsOwnAtThreeSeconds() {
        var finished = 0
        composeRule.mainClock.autoAdvance = false
        stage(WelcomeAnimation.Variant.LIDAR_FLIP) { finished++ }

        composeRule.mainClock.advanceTimeBy(WelcomeAnimation.DURATION_MS - 400L)
        composeRule.waitForIdle()
        assertEquals("it must not have ended early", 0, finished)

        composeRule.mainClock.advanceTimeBy(600)
        composeRule.waitForIdle()
        assertEquals(1, finished)
    }

    /**
     * B draws — the crossfade, the constructed front pose with its unioned
     * crown, the droplet and the splat — on a real GPU, and skips the same way.
     *
     * The clock is walked through every act rather than jumped to the end,
     * because each act composes different geometry and "it drew the last frame"
     * would not have exercised the `Path.op` union or the droplet transform.
     */
    @Test
    fun theDeveloperFilmDrawsAndSkipsToo() {
        var finished = 0
        composeRule.mainClock.autoAdvance = false
        stage(WelcomeAnimation.Variant.LLAMA_SPIT) { finished++ }

        // Twinkle, turn, puff — stopping short of the end so the skip is real.
        for (ms in listOf(200L, 500L, 500L, 400L)) {
            composeRule.mainClock.advanceTimeBy(ms)
            composeRule.waitForIdle()
        }
        assertEquals("still mid-film", 0, finished)
        composeRule.onNodeWithTag("welcomeOverlay").assertIsDisplayed()

        composeRule.onNodeWithTag("welcomeOverlay").performTouchInput { click() }
        composeRule.mainClock.advanceTimeBy(16)
        assertEquals(1, finished)
        assertEquals(0, pageTaps.get())
    }

    /** The gate, on the device it ships on. */
    @Test
    fun theFilmIsClaimedOncePerProcess() {
        assertFalse(WelcomeLaunchGate.claimedInThisProcess)
        assertTrue(WelcomeLaunchGate.claimFirstLaunch())
        assertFalse(WelcomeLaunchGate.claimFirstLaunch())
    }
}
