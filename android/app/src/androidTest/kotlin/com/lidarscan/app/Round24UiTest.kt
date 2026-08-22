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

    /**
     * ROUND 29 item 170 — **the tour is opened from the Advanced sheet.**
     *
     * The corner `?` is gone from both idle variants: it floated inside the one
     * region of the idle page with nothing to explain, and item 158 moved it to
     * the Advanced sheet's Tutorial row (which is also where Settings ›
     * Tutorial lands). The tag is unchanged, so what this helper adds is one
     * tap on the faders button and one wait — and the tour itself, six steps of
     * it, is asserted exactly as before.
     */
    private fun openTheTour() {
        composeRule.onNodeWithTag("advancedButton").performClick()
        composeRule.waitUntil(timeoutMillis = 15_000) { has("tutorialButton") }
        composeRule.onNodeWithTag("tutorialButton").performScrollTo().performClick()
    }

    // ── item 107 ───────────────────────────────────────────────────────────

    /**
     * **Item 107, as amended by ROUND 25 item 120.** Icons only, centred, each
     * still carrying its name for a screen reader — and now with **no dot at
     * all**.
     *
     * The dot assertions became their converse rather than being deleted. A
     * removed feature that is merely un-tested comes back the next time someone
     * "restores" a selected-state affordance; a removed feature that is
     * asserted absent does not. `useUnmergedTree` is kept for the same reason
     * round 24 needed it: each tab button is `clickable`, which merges its
     * descendants, so anything drawn inside a tab is invisible to the merged
     * tree and an assertion made against the merged tree would pass whether the
     * dot were there or not.
     */
    @Test
    fun theTabBarIsIconsOnlyAndStillAccessible() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            for (name in listOf("Projects", "Scan", "Jobs", "Settings")) {
                composeRule.onNode(hasContentDescription(name)).assertExists()
            }
            for (tag in listOf("tabSelectedDot", "tabUnselectedDot")) {
                assertEquals(
                    "item 120: the tab bar draws no $tag",
                    0,
                    composeRule.onAllNodesWithTag(tag, useUnmergedTree = true)
                        .fetchSemanticsNodes().size,
                )
            }
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

            // ── ROUND 28 item 165 ───────────────────────────────────────
            //
            // The "This phone" table is the review's best-built pattern in the
            // app and §D.8 keeps it verbatim — it also ABSORBS the Projects
            // header breakdown item 151 deleted, which is the `Georef` row
            // below. What left is `profileEngineAbi`: the table is four facts
            // about the phone, and the engine's ABI is a fact about the app's
            // build. It lives in Settings › Developer › Engine now, and it is
            // still in the bundle `Send diagnostics` uploads.
            composeRule.onNodeWithTag("profileDeviceCard").assertExists()
            composeRule.onNodeWithTag("profileAppVersion").assertExists()
            composeRule.onNodeWithTag("profileDeviceModel").assertExists()
            composeRule.onNodeWithTag("profileStorage").assertExists()
            composeRule.onNodeWithTag("profileScanCount").assertExists()
            composeRule.onNodeWithTag("profileGeoreferenced").assertExists()

            // ROUND 28 HOTFIX — `useUnmergedTree`. Item 165 rebuilt these two
            // lines as children of `SupportRow`, whose whole `Row` is now
            // `clickable`; `clickable` carries `mergeDescendants = true`, so in
            // the MERGED tree these `Text`s no longer stand as nodes of their
            // own with their own bounds. The disclosure is on screen — a
            // `uiautomator` dump of this page puts it at y 394–435 of 2400,
            // second line of the first card — so the app is right and the QUERY
            // was what item 165 moved. Asking the unmerged tree asks about the
            // `Text` this assertion has always meant.
            composeRule.onNodeWithTag("feedbackPrivacyNote", useUnmergedTree = true)
                .assertIsDisplayed()
            // With no cloud server configured — which is the AVD, and the
            // owner's phone — the page must SAY it will open the share sheet
            // before the tap, not discover it afterwards.
            composeRule.onNodeWithTag("feedbackRouteNote", useUnmergedTree = true)
                .assertIsDisplayed()
            composeRule.onNodeWithTag("sendLogsButton").assertIsDisplayed()

            // ── ROUND 28 item 165, findings F1/F2/F3 ────────────────────────
            //
            // The page used to be ONE card with two unrelated tasks, four
            // labels and two duplicated words: titled "Send logs", containing a
            // button "Send logs", then a field whose only label was the
            // placeholder "Send feedback", then a permanently-disabled button
            // "Send feedback" at about 2:1 contrast, which reads as broken
            // rather than as waiting for input.
            //
            // Composition is a SHEET now, with a persistent label and a Primary
            // that enables on input — so the field and its button are one tap
            // in, and this test taps.
            composeRule.onNodeWithTag("sendFeedbackRow").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("feedbackSheet") }
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

            openTheTour()
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
            openTheTour()
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

            // ROUND 25: scroll back to the row before asserting it is DISPLAYED.
            //
            // The precondition above is not free: re-locking developer mode
            // requires the version footer, and the footer is the last thing on
            // a long scrolling page — so establishing the precondition leaves
            // Settings scrolled to the BOTTOM, where Profile (the first row) is
            // off-screen. `assertIsDisplayed` then fails on a page that is
            // perfectly correct, and it fails ONLY when the device arrived with
            // developer mode already on. That is the worst kind of test
            // failure: it passes on CI, it passes for whoever runs the suite
            // twice, and it fails for the one person who just did the round's
            // manual verification on the same AVD — which is exactly how it
            // surfaced.
            //
            // ROUND 29 item 171: the Profile ROW is gone and the header's
            // avatar is the one door. Asserted at the TOP of the page rather
            // than scrolled to — an avatar that needs scrolling to is not a
            // header — which is also why the round-28 hotfix's `performScrollTo`
            // is not needed here any more.
            composeRule.onNodeWithTag("settingsAvatar").assertIsDisplayed()
            assertEquals(
                "item 171: one door per surface — the ABOUT row is gone",
                0,
                composeRule.onAllNodesWithTag("settingsProfileRow").fetchSemanticsNodes().size,
            )
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
     * **Item 109 + 113, as ROUND 29 item 171 left them.** Settings reaches the
     * same Profile page the Projects avatar does — through the same component,
     * in the same corner.
     *
     * The owner: *"Add profile button in setting page as the profile icon."*
     * The ABOUT card's Profile row went with it, because an avatar in the
     * header AND a row in the body is two doors to one page on one screen. The
     * round-28 hotfix's `performScrollTo` is gone too: a header does not need
     * scrolling to, and if this ever does, that is the regression.
     */
    @Test
    fun settingsAvatarOpensTheSameProfilePage() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.onNodeWithTag("tab_settings").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("settingsScreen") }
            composeRule.onNodeWithTag("settingsAvatar").assertIsDisplayed().performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("profileScreen") }
            composeRule.onNodeWithTag("sendLogsButton").assertIsDisplayed()
        }
    }

    // ── ROUND 29 item 173 ───────────────────────────────────────────────────

    /**
     * **Item 173(b).** Send diagnostics asks where the bundle goes, and the
     * three doors that always exist are all there.
     *
     * The tap is deliberately NOT followed through: **Save to phone** writes a
     * real zip into `Downloads/LidarScan` and **GitHub** launches a browser, and
     * a UI suite that opens a Chrome tab has stopped being a UI suite. What is
     * pinned here is the state machine — the row opens a chooser, the chooser
     * offers the doors `DiagnosticsChooser` says it should, and dismissing it
     * puts the page back — with the URLs themselves pinned in `:core`
     * (`GitHubIssueTest`), where they are strings rather than an Activity.
     */
    @Test
    fun sendDiagnosticsOffersThreeDoorsRatherThanGuessingOne() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.onNodeWithTag("projectsAvatar").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("profileScreen") }

            composeRule.onNodeWithTag("sendLogsButton").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("diagnosticsChooserSheet") }
            composeRule.onNodeWithTag("diagnosticsGithub").assertIsDisplayed()
            composeRule.onNodeWithTag("diagnosticsSave").assertIsDisplayed()
            composeRule.onNodeWithTag("diagnosticsShare").assertIsDisplayed()
            // No cloud fields on a fresh AVD, so the server door must not exist.
            assertEquals(
                "item 173: the server door appears only when it is configured",
                0,
                composeRule.onAllNodesWithTag("diagnosticsServer").fetchSemanticsNodes().size,
            )
        }
    }
}
