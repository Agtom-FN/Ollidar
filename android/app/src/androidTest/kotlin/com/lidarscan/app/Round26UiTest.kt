package com.lidarscan.app

import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
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
 * ROUND 26 — the rename, the fullscreen Scan tab and its floating controls, on
 * the AVD.
 *
 * The claims here are the ones a screenshot cannot make stick. "It looks
 * fullscreen" is a picture; "the viewport is exactly as wide as the window and
 * starts above the status bar" is a measurement, and it is the measurement that
 * catches the day somebody re-adds a 14 dp padding "to tidy the edges".
 *
 * Written to survive an AVD with no sensor attached — which is every AVD — so
 * the recording-only half of item 124 (the tab bar hiding) is asserted from the
 * side that IS reachable: the bar is present and displayed whenever nothing is
 * scanning, which is the state that must never be lost, and the hiding itself
 * is covered by the unit-level arithmetic in `CaptureLayoutTest` plus the
 * `AppContainer.scanInProgress` contract. A test that claims to prove a
 * recording behaviour on a device that cannot record would be a test that
 * silently passes.
 */
@RunWith(AndroidJUnit4::class)
class Round26UiTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    private fun has(tag: String): Boolean =
        composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty()

    private fun count(tag: String): Int =
        composeRule.onAllNodesWithTag(tag, useUnmergedTree = true).fetchSemanticsNodes().size

    private fun awaitProjectsTab() {
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { has("projectsAvatar") }.getOrDefault(false)
        }
    }

    private fun openScanTab() {
        awaitProjectsTab()
        composeRule.onNodeWithTag("tab_capture").performClick()
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { has("recordButton") }.getOrDefault(false)
        }
        composeRule.waitForIdle()
    }

    // ── item 124 ───────────────────────────────────────────────────────────

    /**
     * **Item 124.** The live view is the screen: full window width, and its top
     * edge is at or above the status bar rather than below it.
     *
     * Measured against the ROOT's bounds rather than against a number, so the
     * test says the same thing on every screen size and cannot be satisfied by
     * a device that happens to be 14 dp narrower than the constant somebody
     * hard-coded.
     */
    @Test
    fun theScanViewportIsEdgeToEdge() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            val viewport = composeRule.onNodeWithTag("captureViewport").fetchSemanticsNode()
            val root = viewport.root!!
            val rootWidth = root.semanticsOwner.rootSemanticsNode.size.width
            val box = viewport.boundsInRoot

            assertEquals(
                "item 124: the viewport spans the whole window width — no card inset",
                rootWidth.toFloat(),
                box.width,
                2f,
            )
            assertTrue(
                "item 124: the viewport starts at the top of the window, not below the status bar " +
                    "(top was ${box.top})",
                box.top <= 2f,
            )
        }
    }

    /**
     * **Item 124.** The four floating controls are all on screen at once, in
     * the corners the owner asked for, and there is still exactly ONE of each
     * of the two that the other suites drive the app through.
     *
     * The uniqueness half matters more than it looks: a fullscreen relayout
     * that leaves the old row composed as well would put a second
     * `advancedButton` and a second `tutorialButton` in the tree, and every
     * `onNodeWithTag` in `Round23ScanTabTest` and `Round24UiTest` would fail
     * with "expected exactly one node" rather than with anything that names the
     * cause.
     */
    @Test
    fun theScanControlsFloatInTheCorners() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()

            composeRule.onNodeWithTag("recordButton").assertIsDisplayed()
            composeRule.onNodeWithTag("advancedButton").assertIsDisplayed()
            composeRule.onNodeWithTag("mapModeChip").assertIsDisplayed()
            composeRule.onNodeWithTag("tutorialButton").assertIsDisplayed()
            composeRule.onNodeWithTag("liveViewSwitch").assertIsDisplayed()
            composeRule.onNodeWithTag("pointsCapturedValue").assertIsDisplayed()

            assertEquals("item 124: one Advanced gear", 1, count("advancedButton"))
            assertEquals("item 124: one tour button", 1, count("tutorialButton"))
            assertEquals("item 124: one scan button", 1, count("recordButton"))

            val root = composeRule.onNodeWithTag("captureViewport").fetchSemanticsNode().boundsInRoot
            val gear = composeRule.onNodeWithTag("advancedButton").fetchSemanticsNode().boundsInRoot
            val fab = composeRule.onNodeWithTag("recordButton").fetchSemanticsNode().boundsInRoot
            val mapChip = composeRule.onNodeWithTag("mapModeChip").fetchSemanticsNode().boundsInRoot
            val help = composeRule.onNodeWithTag("tutorialButton").fetchSemanticsNode().boundsInRoot

            assertTrue("the gear is in the top half", gear.center.y < root.center.y)
            assertTrue("the gear is at the end edge", gear.center.x > root.center.x)
            assertTrue("the scan button is in the bottom half", fab.center.y > root.center.y)
            assertTrue("the map-mode chip is at the start edge", mapChip.center.x < fab.left)
            assertTrue("the ? is at the end edge", help.center.x > fab.right)
        }
    }

    /**
     * **Item 124, from the side an AVD can reach.** With nothing scanning the
     * tab bar is up and all four tabs are hittable.
     *
     * This is the assertion that protects the hiding rather than the one that
     * proves it: `AppContainer.scanInProgress` is cleared when the Scan screen
     * leaves the composition, so the failure mode that would actually strand an
     * operator is a bar that stays hidden after a scan ends — and that shows up
     * here, on the very next entry into the tab.
     */
    @Test
    fun theTabBarIsUpWheneverNothingIsScanning() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            composeRule.onNodeWithTag("scanTabBar").assertIsDisplayed()
            for (tag in listOf("tab_projects", "tab_capture", "tab_jobs", "tab_settings")) {
                composeRule.onNodeWithTag(tag).assertIsDisplayed()
            }
            // Leave and come back: the bar must still be there, which is the
            // dispose-clears-the-flag contract seen from the outside.
            composeRule.onNodeWithTag("tab_projects").performClick()
            composeRule.waitForIdle()
            composeRule.onNodeWithTag("tab_capture").performClick()
            composeRule.waitUntil(timeoutMillis = 30_000) {
                runCatching { has("recordButton") }.getOrDefault(false)
            }
            composeRule.onNodeWithTag("scanTabBar").assertIsDisplayed()
        }
    }

    // ── item 122 ───────────────────────────────────────────────────────────

    /**
     * **Item 122.** The app calls itself Ollidar where the operator can see it,
     * and the version footer is 0.9.11.
     *
     * The footer is the strongest single check available on-device: it is the
     * one string that carries the name AND both version numbers, and it is read
     * from `BuildConfig`, so a stale APK fails it rather than passing quietly.
     */
    @Test
    fun theAppIsCalledOllidar() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            composeRule.onNodeWithText("Ollidar").assertIsDisplayed()

            composeRule.onNodeWithTag("tab_settings").performClick()
            composeRule.waitUntil(timeoutMillis = 20_000) {
                runCatching { has("app_version_footer") }.getOrDefault(false)
            }
            composeRule.onNodeWithTag("app_version_footer").performScrollTo()
            val footer = composeRule.onNodeWithTag("app_version_footer")
                .fetchSemanticsNode()
                .config
                .getOrNull(androidx.compose.ui.semantics.SemanticsProperties.Text)
                ?.joinToString(" ")
                .orEmpty()
            assertTrue("the footer names the app: \"$footer\"", footer.contains("Ollidar"))
            assertTrue("the footer is this round's version: \"$footer\"", footer.contains("0.9.11"))
            assertTrue("…and its code: \"$footer\"", footer.contains("911"))

            composeRule.onNodeWithTag("tab_projects").performClick()
        }
    }
}
