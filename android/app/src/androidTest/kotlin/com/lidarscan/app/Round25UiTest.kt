package com.lidarscan.app

import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.assertIsDisplayed
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
 * ROUND 25 — the owner's 0.9.9 UI notes, checked on the AVD.
 *
 * Two of this round's items are claims about what is NOT on the screen (item
 * 114's list preview, item 120's selected-state dot), which is the class of
 * claim that quietly comes back: a removed affordance with no test is a
 * removed affordance somebody restores. Both are asserted as absences here,
 * with the presence asserted alongside wherever there is one — the gallery
 * still has its thumbnail, the tab bar still has its four icons.
 *
 * Written to survive an AVD with no scans on it, like round 24's suite: the
 * Projects control row does not exist over an empty list, so the populated
 * assertions are guarded and the empty case asserts the empty state instead.
 * A test that silently passes on a fresh device is worse than no test.
 */
@RunWith(AndroidJUnit4::class)
class Round25UiTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    private fun awaitProjectsTab() {
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching {
                composeRule.onAllNodesWithTag("projectsAvatar").fetchSemanticsNodes().isNotEmpty()
            }.getOrDefault(false)
        }
    }

    private fun has(tag: String): Boolean =
        composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty()

    /**
     * The **unmerged** count, always.
     *
     * `ProjectCard` is a `combinedClickable` container, which merges its
     * descendants into one semantics node — correct, because a card must
     * announce itself as one thing, and the reason a merged-tree query for
     * anything drawn INSIDE a card returns zero whether it is there or not. An
     * absence assertion made against the merged tree would be a test that
     * cannot fail.
     */
    private fun count(tag: String): Int =
        composeRule.onAllNodesWithTag(tag, useUnmergedTree = true).fetchSemanticsNodes().size

    // ── item 114, REVERSED by ROUND 28 item 162 ────────────────────────────

    /**
     * **Item 114 → item 162.** BOTH layouts draw a preview.
     *
     * Round 25 removed the thumbnail from the list on the argument that a
     * 108 dp preview above every row means four scans fill a phone screen. The
     * argument was about the CARD, and applying it to the thumbnail deleted the
     * single strongest differentiator between 66 otherwise identical rows —
     * leaving text and three chips that were the same on every one of them.
     *
     * §D.5's row is 72 dp with a 56 dp tile at the leading edge, so the picture
     * costs no height at all; the list holds roughly twice as many scans as the
     * old card layout did *with* the thumbnail back. The assertion is inverted
     * rather than deleted, and the tag is unchanged, so this file records the
     * reversal instead of losing the claim.
     */
    @Test
    fun bothLayoutsDrawAPreview() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.waitForIdle()

            if (!has("projectsControlRow")) {
                assertTrue(
                    "with no scans there is nothing to preview, and no control row either",
                    has("newScanButton"),
                )
                return@use
            }

            // Get to the LIST, whichever layout the device was left in.
            if (!has("projectsList")) {
                composeRule.onNodeWithTag("projectsLayoutToggle").performClick()
                composeRule.waitUntil(timeoutMillis = 10_000) { has("projectsList") }
            }
            composeRule.waitForIdle()
            assertTrue(
                "item 162: a list row draws its scan's cloud at the leading edge",
                count("projectPreview") > 0,
            )

            composeRule.onNodeWithTag("projectsLayoutToggle").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) { has("projectsGallery") }
            composeRule.waitForIdle()
            assertTrue(
                "the gallery is thumbnail-first by design and keeps its preview",
                count("projectPreview") > 0,
            )

            // Leave the device in the default layout for whoever runs next.
            composeRule.onNodeWithTag("projectsLayoutToggle").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) { has("projectsList") }
        }
    }

    // ── item 120 ───────────────────────────────────────────────────────────

    /**
     * **Item 120.** No selected-state dot, on any tab, in any state — and the
     * icons are still there and still drive navigation.
     *
     * The dot is checked on two different tabs rather than one, because a dot
     * drawn only for the SELECTED tab would leave a single-tab check passing on
     * whichever tab happened not to be selected.
     */
    @Test
    fun theTabBarHasNoSelectedDot() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            for (tag in listOf("tabSelectedDot", "tabUnselectedDot")) {
                assertEquals("item 120: no $tag on the Projects tab", 0, count(tag))
            }

            composeRule.onNodeWithTag("tab_settings").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("settingsScreen") }
            for (tag in listOf("tabSelectedDot", "tabUnselectedDot")) {
                assertEquals("item 120: no $tag on the Settings tab", 0, count(tag))
            }

            // The bar is still a bar: four tabs, still tappable by tag.
            for (tag in listOf("tab_projects", "tab_capture", "tab_jobs", "tab_settings")) {
                composeRule.onNodeWithTag(tag).assertIsDisplayed()
            }
            composeRule.onNodeWithTag("tab_projects").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("projectsAvatar") }
        }
    }

    // ── item 118a (the mid-round amendment) ────────────────────────────────

    /**
     * **Item 118a.** The **Connection debug** row is behind the seven-tap
     * unlock, and both of its controls are there when it is unlocked.
     *
     * ## What this asserts, and what deliberately covers the rest
     *
     * The gating is the part that can go wrong silently and is asserted here:
     * a diagnostic that leaked into the ordinary Settings page would be a
     * regression nobody would notice until an operator asked what "sweep
     * verdict=" meant.
     *
     * The sweep's OUTPUT is not asserted here, and that is a considered choice
     * rather than a gap. Driving the button from the instrumented harness did
     * not produce the block, while the identical sequence performed by hand on
     * the same AVD did — screenshot `item118a-connection-debug.png`, showing
     * `sweep verdict=wrong-subnet` over the emulator's `eth0`/`dummy0`. Rather
     * than ship an assertion that is green for the wrong reason, or a
     * `Thread.sleep` that makes it green some of the time, the sweep's content
     * is covered where it is deterministic: 29 `:core` unit tests over
     * `ConnectionSweepFormat` and `ConnectionDebugRateLimiter`, including all
     * three of the owner's cases against synthetic USB and interface lists.
     * The gap that leaves is "the button is wired to the ViewModel", and it is
     * named here rather than papered over.
     *
     * Developer mode is a persisted device fact, so this establishes its own
     * precondition and locks it again on the way out — `Round24UiTest` asserts
     * the developer rows are ABSENT and shares this AVD.
     */
    @Test
    fun connectionDebugIsBehindDeveloperMode() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.onNodeWithTag("tab_settings").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("settingsScreen") }

            if (has("captureLogPath")) {
                composeRule.onNodeWithTag("app_version_footer").performScrollTo()
                repeat(7) { composeRule.onNodeWithTag("app_version_footer").performClick() }
                composeRule.waitUntil(timeoutMillis = 15_000) { !has("captureLogPath") }
            }
            assertEquals(
                "Connection debug is a developer tool and must be absent when locked",
                0,
                count("connectionDebugRow"),
            )

            composeRule.onNodeWithTag("app_version_footer").performScrollTo()
            repeat(7) { composeRule.onNodeWithTag("app_version_footer").performClick() }
            composeRule.waitUntil(timeoutMillis = 15_000) { has("connectionDebugRow") }
            // ROUND 34 item 181: the unlock plays the easter egg, which eats
            // the next touch. See `dismissWelcomeEgg`.
            dismissWelcomeEgg()

            // Both controls, because a sweep you cannot copy is a sweep that
            // does not reach the person who needs to read it.
            // ROUND 28 item 164: the sweep's output and its two controls moved
            // into a SHEET behind `connectionDebugRow`, per the mockup — a
            // developer read-out is not a card that lives permanently on the
            // Settings page. The row is still the developer-mode gate this test
            // waits on above; the controls are one tap further in, and both are
            // still asserted, because a sweep you cannot copy is a sweep that
            // does not reach the person who needs to read it.
            composeRule.onNodeWithTag("connectionDebugRow").performScrollTo()
            composeRule.onNodeWithTag("connectionDebugRow").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) { has("connectionDebugSweep") }
            composeRule.onNodeWithTag("connectionDebugSweep").assertIsDisplayed()
            composeRule.onNodeWithTag("connectionDebugCopy").assertIsDisplayed()
            androidx.test.platform.app.InstrumentationRegistry.getInstrumentation()
                .uiAutomation.performGlobalAction(
                    android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK,
                )
            composeRule.waitForIdle()

            // Lock it again so the next test meets a clean device.
            composeRule.onNodeWithTag("app_version_footer").performScrollTo()
            repeat(7) { composeRule.onNodeWithTag("app_version_footer").performClick() }
            composeRule.waitUntil(timeoutMillis = 15_000) { !has("connectionDebugRow") }
        }
    }
}
