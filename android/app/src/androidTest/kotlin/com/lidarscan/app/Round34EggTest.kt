package com.lidarscan.app

import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 34 items 181(b) + 183(a) — **the easter egg, on a device, and the one
 * class in this module with no Compose test rule.**
 *
 * That absence is the point, and it cost three runs to learn. `ComposeTestRule`
 * — even `createEmptyComposeRule`, which composes nothing itself — installs a
 * **test frame clock** for the whole test. With `autoAdvance` on, that clock
 * advances only while the test is *inside* a Compose API. A test that taps and
 * then watches from outside Compose gives the composition no frames at all, so
 * the state change that puts the film up is never applied and the film never
 * appears. And a test that watches from *inside* Compose gets the opposite
 * failure, which round 34 also hit first: every Compose accessor waits for the
 * composition to go idle, a three-second animation is not idle, so the question
 * "is the film up?" is answered after it has ended.
 *
 * The film is played by the real `MainActivity` in response to a real settings
 * write. So this test uses the real clock: `ActivityScenario` for the Activity,
 * `input tap` for the fingers, and the accessibility tree for the eyes. Nothing
 * here synchronises with Compose, and nothing here needs to.
 *
 * (Round 32's own suite avoided all of this by owning its composition and
 * driving `mainClock` by hand — the right answer for a test of the film's
 * timing, and no answer at all for a test of what a seventh tap does.)
 */
@RunWith(AndroidJUnit4::class)
class Round34EggTest {

    private val width = 1_080
    private fun centreX() = width / 2

    /**
     * Brings the version footer into view and returns its centre.
     *
     * Scrolling to the foot of the page is not enough and that is the whole
     * reason this is a function: **unlocking developer mode adds a section
     * BELOW the version row**, so the bottom of the page is a different place
     * before and after, and a test that scrolls to the bottom and taps where
     * the row used to be taps a filesystem path instead. So the page is
     * scrolled to its foot and then walked back UP until the row is genuinely
     * on screen — bounds inside the display, not merely present in the tree,
     * because an off-screen node still has coordinates and `input tap` will
     * happily aim at them and hit nothing.
     */
    private fun scrollToVersionRow(): Pair<Int, Int> {
        repeat(14) {
            val bounds = nodeBoundsContaining("Version")
            when {
                bounds == null -> scrollDown()          // not composed yet: it is below
                bounds.bottom < TOP_EDGE -> scrollUp()  // scrolled past it
                bounds.top > BOTTOM_EDGE -> scrollDown()
                else -> return bounds.centerX() to bounds.centerY()
            }
            Thread.sleep(350)
        }
        error("the version footer never came into view: ${screenDescriptions().takeLast(10)}")
    }

    private fun scrollDown() = swipe(centreX(), 1_700, centreX(), 800, 220)

    private fun scrollUp() = swipe(centreX(), 800, centreX(), 1_700, 220)

    private fun tapVersionSevenTimes() {
        val (x, y) = scrollToVersionRow()
        // The app's own count, read rather than repeated: a test that counted
        // to seven by itself would keep passing after somebody changed it.
        repeat(com.lidarscan.app.ui.settings.DEVELOPER_UNLOCK_TAPS) {
            tapScreen(x, y)
            Thread.sleep(120)
        }
    }

    private fun openSettings() {
        assertTrue("the app must reach its first screen", awaitText("Ollidar", 40_000))
        // The tab bar's Settings tab, by its own content description.
        val tab = nodeBoundsContaining("Settings")
            ?: error("no Settings tab in ${screenDescriptions().take(12)}")
        tapScreen(tab.centerX(), tab.centerY())
        // "Theme" rather than "Version": the version row is the last thing on a
        // long scrolling page and is not on screen when the page opens.
        assertTrue(
            "Settings must open — saw ${screenDescriptions().take(12)}",
            awaitText("Theme"),
        )
    }

    /**
     * The whole sequence a person performs: seven taps on the version footer,
     * the film over the Settings page, one touch to remove it, the page live
     * underneath — and then the seven taps that put developer mode back, which
     * play **nothing**.
     *
     * The precondition is established rather than assumed: this suite shares an
     * AVD with whoever ran last (round 25's lesson, three rounds running).
     */
    @Test
    fun unlockingDeveloperModePlaysTheEggAndRelockingIsSilent() {
        // A connected run is, by `testOptions.animationsDisabled`, a phone with
        // animations OFF — which is the reduced-motion case, in which the
        // correct behaviour is no film at all. See `withAnimationsOn`.
        withAnimationsOn {
            ActivityScenario.launch(MainActivity::class.java).use {
                openSettings()

                // Arrive locked, whatever the device was left in. Re-locking is
                // silent, so this cannot produce the film being asserted.
                if (screenContains("Connection debug")) {
                    tapVersionSevenTimes()
                    assertTrue(
                        "the precondition must lock developer mode",
                        awaitTextGone(DEVELOPER_ROW),
                    )
                    }
                assertTrue(
                    "the egg must not be up before the unlock",
                    !screenDescribes(EGG_DESCRIPTION),
                )

                // ── the unlock.
                tapVersionSevenTimes()
                assertTrue(
                    "item 181(b): unlocking developer mode plays the egg — " +
                        "scales ${animationScaleReport()}, on screen ${screenDescriptions().take(8)}",
                    awaitDescribed(EGG_DESCRIPTION, timeoutMillis = 15_000),
                )

                // ── one touch, anywhere, and it is gone.
                tapScreen(centreX(), 1_200)
                assertTrue("item 181(b): one touch removes it", awaitGone(EGG_DESCRIPTION))

                // ── the page was underneath the whole time, and the unlock it
                // was celebrating actually happened.
                assertTrue(
                    "the Settings page is live under the film",
                    awaitText("Connection debug"),
                )

                // ── item 183(a): the re-lock is silent, animations still on.
                tapVersionSevenTimes()
                val end = System.currentTimeMillis() + 4_000
                while (System.currentTimeMillis() < end) {
                    assertTrue(
                        "item 183(a): locking developer mode away plays nothing",
                        !screenDescribes(EGG_DESCRIPTION),
                    )
                    Thread.sleep(120)
                }
                assertTrue("developer mode must be locked again", awaitTextGone(DEVELOPER_ROW))
            }
        }
    }

    /**
     * **Item 181(d)'s other half, and the harness's own reduced-motion proof.**
     *
     * The same unlock with the animations as an instrumentation run leaves them
     * — zero, which is exactly what Accessibility → *Remove animations* writes
     * — plays nothing, and developer mode still comes on. An instrument that
     * refused to work because a film was suppressed would be the worse bug.
     */
    @Test
    fun withAnimationsOffTheUnlockIsSilentAndStillUnlocks() {
        org.junit.Assume.assumeTrue(
            "this claim is about a device with animations off",
            animationsAreDisabled(),
        )
        ActivityScenario.launch(MainActivity::class.java).use {
            openSettings()
            if (screenContains("Connection debug")) {
                tapVersionSevenTimes()
                assertTrue("the precondition must lock developer mode", awaitTextGone(DEVELOPER_ROW))
            }
            tapVersionSevenTimes()
            assertTrue("the unlock still works", awaitText("Connection debug"))
            assertTrue(
                "reduced motion means no film, in either variant",
                !screenDescribes(EGG_DESCRIPTION) && !screenDescribes(LAUNCH_FILM_DESCRIPTION),
            )
            // Leave the device as it was found.
            tapVersionSevenTimes()
            assertTrue("re-locked", awaitTextGone(DEVELOPER_ROW))
        }
    }

    private companion object {
        /** The first row of the Developer section — present iff it is unlocked. */
        const val DEVELOPER_ROW = "Connection debug"

        /** The band of the display a tap can actually land in, past the bars. */
        const val TOP_EDGE = 200
        const val BOTTOM_EDGE = 2_150
    }
}
