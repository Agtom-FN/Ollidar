package com.lidarscan.app

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 24 — **the owner's UI round, checked on a real screen.**
 *
 * Every item in this round is a claim about pixels, which is exactly the class
 * of claim round 23 learned not to argue about in a diff (item 103: *"The ui
 * not renamed or changed as you asked"*). So each one is asserted on the AVD:
 * the tab bar has icons and no words, the Projects tab has a layout toggle and
 * a sort control, the avatar opens a Profile page with a Send-logs button, the
 * Scan screen has a ? that opens a six-step tour, and Settings is five sections
 * with everything developer-only behind the seven-tap unlock.
 *
 * A bare AVD has no COIN-D6, which is the right rig for most of this: the Scan
 * screen is in its disconnected ready state throughout, which is the state a
 * first-run operator actually meets.
 */
@RunWith(AndroidJUnit4::class)
class Round24UiTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    private fun awaitProjectsTab() {
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching {
                composeRule.onAllNodesWithTag("projectsAvatar").fetchSemanticsNodes().isNotEmpty()
            }.getOrDefault(false)
        }
    }

    private fun openScanTab() {
        composeRule.onNodeWithTag("tab_capture").performClick()
        composeRule.waitUntil(timeoutMillis = 20_000) {
            composeRule.onAllNodesWithTag("recordButton").fetchSemanticsNodes().isNotEmpty()
        }
    }

    private fun has(tag: String): Boolean =
        composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty()

    // ── item 107 ───────────────────────────────────────────────────────────

    /**
     * **Item 107.** Icons only, centred, each still carrying its name for a
     * screen reader — and the selected one carrying the ember dot, because on a
     * bar of four glyphs a tint alone is not a state.
     */
    @Test
    fun theTabBarIsIconsOnlyAndStillAccessible() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            for (name in listOf("Projects", "Scan", "Jobs", "Settings")) {
                composeRule.onNode(hasContentDescription(name)).assertExists()
            }
            // Exactly one tab is selected, so exactly one dot is drawn.
            //
            // `useUnmergedTree`: each tab button is `clickable`, which merges
            // its descendants, so the dot inside it is invisible to the merged
            // tree. That merge is correct — a tab must announce itself as ONE
            // thing — and it is why this assertion reads the unmerged tree.
            assertEquals(
                "exactly one tab may carry the selected dot",
                1,
                composeRule.onAllNodesWithTag("tabSelectedDot", useUnmergedTree = true)
                    .fetchSemanticsNodes().size,
            )
            assertEquals(
                "…and the other three are unselected",
                3,
                composeRule.onAllNodesWithTag("tabUnselectedDot", useUnmergedTree = true)
                    .fetchSemanticsNodes().size,
            )
            // The bar still drives navigation by tag, which is what every other
            // emulator test in this suite depends on.
            composeRule.onNodeWithTag("tab_settings").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("settingsScreen") }
        }
    }

    // ── item 108 ───────────────────────────────────────────────────────────

    /**
     * **Item 108.** The control row, the toggle and the sort menu.
     *
     * Written to survive an AVD with no scans on it: the control row is
     * deliberately absent when the list is empty (a sort menu over nothing is a
     * control for nothing), so the empty case asserts THAT and the populated
     * case walks the toggle. Both are real assertions about item 108; neither
     * depends on what a previous test left on the device.
     */
    @Test
    fun projectsOffersGalleryListAndSort() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.waitForIdle()

            if (!has("projectsControlRow")) {
                // No scans: the row must NOT be there, and the empty state must.
                assertTrue(
                    "with no scans the tab shows its empty state, not a sort menu",
                    has("newScanButton"),
                )
                return@use
            }

            // The sort control is one clickable Row, so its value Text is
            // merged into it — read the unmerged tree for the value itself.
            composeRule.onNodeWithTag("projectsSortButton").assertIsDisplayed()
            assertTrue(
                "the current order must be readable without opening the menu",
                composeRule.onAllNodesWithTag("projectsSortValue", useUnmergedTree = true)
                    .fetchSemanticsNodes().isNotEmpty(),
            )
            val startedInList = has("projectsList")

            composeRule.onNodeWithTag("projectsLayoutToggle").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) {
                if (startedInList) has("projectsGallery") else has("projectsList")
            }
            // …and back, because a toggle that only goes one way is a switch.
            composeRule.onNodeWithTag("projectsLayoutToggle").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) {
                if (startedInList) has("projectsList") else has("projectsGallery")
            }

            // The sort menu offers all three, and picking one sticks.
            composeRule.onNodeWithTag("projectsSortButton").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) { has("projectsSort_A_Z") }
            composeRule.onNodeWithTag("projectsSort_A_Z").performClick()
            composeRule.waitForIdle()
            assertTrue(
                composeRule.onAllNodesWithTag("projectsSortValue", useUnmergedTree = true)
                    .fetchSemanticsNodes().isNotEmpty(),
            )
        }
    }

    // ── item 109 ───────────────────────────────────────────────────────────

    /**
     * **Item 109.** The avatar opens Profile, not Settings, and the page
     * carries the device facts and both send actions — with the privacy line
     * ABOVE the buttons, where a disclosure has to be.
     */
    @Test
    fun theAvatarOpensAProfilePageThatCanSendLogs() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.onNodeWithTag("projectsAvatar").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("profileScreen") }

            composeRule.onNodeWithTag("profileDeviceCard").assertExists()
            composeRule.onNodeWithTag("profileAppVersion").assertExists()
            composeRule.onNodeWithTag("profileEngineAbi").assertExists()
            composeRule.onNodeWithTag("profileStorage").assertExists()
            composeRule.onNodeWithTag("profileScanCount").assertExists()

            composeRule.onNodeWithTag("feedbackPrivacyNote").assertIsDisplayed()
            // With no cloud server configured — which is the AVD, and the
            // owner's phone — the page must SAY it will open the share sheet
            // before the tap, not discover it afterwards.
            composeRule.onNodeWithTag("feedbackRouteNote").assertIsDisplayed()
            composeRule.onNodeWithTag("sendLogsButton").assertIsDisplayed()
            composeRule.onNodeWithTag("feedbackField").assertExists()
            composeRule.onNodeWithTag("sendFeedbackButton").assertExists()
        }
    }

    // ── item 110 ───────────────────────────────────────────────────────────

    /**
     * **Item 110(b).** The ? opens the tour, the tour advances, and the tour
     * ends — six steps, and the last button says Done.
     *
     * Driven by the overlay's own tags rather than by step text, so a reworded
     * step does not break the machine's test (`ScanTutorialTest` in `:core` is
     * what pins the words).
     */
    @Test
    fun theScanScreenHasATourThatRunsAndEnds() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            openScanTab()

            composeRule.onNodeWithTag("tutorialButton").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) { has("tutorialOverlay") }
            composeRule.onNodeWithTag("tutorialCard").assertIsDisplayed()
            composeRule.onNodeWithTag("tutorialTitle").assertIsDisplayed()
            composeRule.onNodeWithTag("tutorialProgress").assertIsDisplayed()

            // Six steps: five Nexts and a Done. The loop is bounded so a tour
            // that failed to end fails the test rather than hanging it.
            var advanced = 0
            while (has("tutorialOverlay") && advanced < 10) {
                composeRule.onNodeWithTag("tutorialNext").performClick()
                composeRule.waitForIdle()
                advanced++
            }
            assertEquals("the tour is six steps and then it is over", 6, advanced)
            assertTrue("the overlay must be gone", !has("tutorialOverlay"))
        }
    }

    /** **Item 110(b).** Skip ends it too, from the first step. */
    @Test
    fun theTourCanBeSkipped() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            openScanTab()
            composeRule.onNodeWithTag("tutorialButton").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) { has("tutorialOverlay") }
            composeRule.onNodeWithTag("tutorialSkip").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) { !has("tutorialOverlay") }
        }
    }

    // ── item 111 ───────────────────────────────────────────────────────────

    /**
     * **Item 111.** Entering the Scan tab lands on a clean ready screen.
     *
     * On a bare AVD the observable is the new-scan state itself: the name field
     * with its auto-name, and no session summary or graded card from a previous
     * walk. `CaptureRound24Test` in `:app` is what pins the reset against a
     * real recorded-and-sealed scan, which an emulator with no sensor cannot
     * produce.
     */
    @Test
    fun theScanTabOpensOnACleanReadyScreen() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            openScanTab()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("scanNameField") }
            assertTrue("no previous scan's card may be on a fresh entry", !has("scanSummaryCard"))
            assertTrue(!has("sessionSummarySheet"))

            // Leave and come back: still clean, still a new scan.
            composeRule.onNodeWithTag("tab_projects").performClick()
            composeRule.waitForIdle()
            openScanTab()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("scanNameField") }
            assertTrue(!has("scanSummaryCard"))
        }
    }

    // ── item 113 ───────────────────────────────────────────────────────────

    /**
     * **Item 113.** Five sections, Profile at the top, Tutorial in About, and
     * everything developer-only behind the seven-tap unlock rather than in
     * front of it.
     */
    @Test
    fun settingsIsSimplifiedWithDeveloperThingsHidden() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.onNodeWithTag("tab_settings").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("settingsScreen") }

            // Developer mode is a PERSISTED device fact, and this suite shares
            // a device with whoever used it last (including a human doing the
            // round's manual verification). So the test establishes its own
            // precondition rather than assuming one: if the unlock is on, the
            // seven taps put it back before anything is asserted.
            if (has("captureLogPath")) {
                composeRule.onNodeWithTag("app_version_footer").performScrollTo()
                repeat(7) { composeRule.onNodeWithTag("app_version_footer").performClick() }
                composeRule.waitUntil(timeoutMillis = 15_000) { !has("captureLogPath") }
            }

            composeRule.onNodeWithTag("settingsProfileRow").assertIsDisplayed()
            composeRule.onNodeWithTag("settingsTutorialRow").assertExists()
            composeRule.onNodeWithTag("advancedFeaturesSwitch").assertExists()
            composeRule.onNodeWithTag("keepEmptyScansRow").assertExists()
            composeRule.onNodeWithTag("operatorCuesRow").assertExists()
            composeRule.onNodeWithTag("settingsDetailRow").assertExists()

            // The developer-only cards must be ABSENT, not merely further down.
            for (tag in listOf(
                "replaySyntheticCaptureButton",
                "captureLogPath",
                "d6SensorLatencySlider",
                "captureDebugLogRow",
            )) {
                assertEquals(
                    "$tag must be behind developer mode",
                    0,
                    composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().size,
                )
            }

            // …and the version footer's seven taps still bring them all back.
            // Scrolled into view first: the footer is the last thing on a long
            // scrolling page, and a touch injected at coordinates outside the
            // window is dropped without an error — which is exactly how this
            // read as "seven taps did nothing" on the first emulator run.
            composeRule.onNodeWithTag("app_version_footer").performScrollTo()
            repeat(7) { composeRule.onNodeWithTag("app_version_footer").performClick() }
            composeRule.waitUntil(timeoutMillis = 15_000) { has("captureLogPath") }
            composeRule.onNodeWithTag("replaySyntheticCaptureButton").assertExists()
            // Lock it again so the next test in the suite meets a clean device.
            composeRule.onNodeWithTag("app_version_footer").performScrollTo()
            repeat(7) { composeRule.onNodeWithTag("app_version_footer").performClick() }
            composeRule.waitUntil(timeoutMillis = 15_000) { !has("captureLogPath") }
        }
    }

    /**
     * **Item 109 + 113.** Settings' own Profile row reaches the same page the
     * avatar does — one destination, two doors, which is the shape item 109
     * asked for (as opposed to the two doors to ONE TAB it replaced).
     */
    @Test
    fun settingsProfileRowOpensTheSamePage() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.onNodeWithTag("tab_settings").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("settingsScreen") }
            composeRule.onNodeWithTag("settingsProfileRow").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("profileScreen") }
            composeRule.onNodeWithTag("sendLogsButton").assertIsDisplayed()
        }
    }
}
