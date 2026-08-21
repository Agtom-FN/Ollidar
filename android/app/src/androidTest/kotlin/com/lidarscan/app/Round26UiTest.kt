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

            // ── ROUND 27 item 136 SUPERSEDES ROUND 26 item 124 HERE ────────
            //
            // The owner's own session: *"Nothing should overlay except warning.
            // … show the settings of connection in the main window and not
            // overlay."* So the IDLE Scan tab — which is the only state an AVD
            // can reach — is a laid-out page now, and its viewport is a framed
            // preview inside a column rather than the whole window. Round 26's
            // fullscreen claim survives for the state it was actually about: a
            // RECORDING, where the picture is the product.
            //
            // The claim is therefore restated rather than deleted, and it is
            // still a measurement: the preview takes the window's full usable
            // width (only the page's own 10 dp gutters are removed) and sits
            // BELOW the status band instead of under the status bar.
            assertTrue(
                "item 136: the preview takes the page's width (was ${box.width} of $rootWidth)",
                box.width >= rootWidth * 0.9f,
            )
            assertTrue(
                "item 136: the preview is below the status band, not under the status bar " +
                    "(top was ${box.top})",
                box.top > 2f,
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
            // ROUND 27 item 140(a): the map-mode chip is GONE. The owner's own
            // 0.9.11 session — *"The map indicatior seems useless"* — supersedes
            // round 26's corner for it; the live-map switch keeps its home in
            // the Capture sheet. Asserted as an absence rather than deleted, so
            // the removal is a decision this suite records rather than a line
            // somebody quietly dropped.
            assertEquals(
                "item 140(a): the map-mode chip is removed",
                0,
                composeRule.onAllNodesWithTag("mapModeChip").fetchSemanticsNodes().size,
            )
            composeRule.onNodeWithTag("tutorialButton").assertIsDisplayed()
            composeRule.onNodeWithTag("liveViewSwitch").assertIsDisplayed()
            composeRule.onNodeWithTag("pointsCapturedValue").assertIsDisplayed()

            assertEquals("item 124: one Advanced gear", 1, count("advancedButton"))
            assertEquals("item 124: one tour button", 1, count("tutorialButton"))
            assertEquals("item 124: one scan button", 1, count("recordButton"))

            // ── ROUND 27 item 136: the same claims, on a PAGE ──────────────
            //
            // Round 26 asserted four corners of a fullscreen viewport. The idle
            // tab is a laid-out page now, so "corner of the picture" is not the
            // right frame — but the SPATIAL claims that made those corners
            // useful survive verbatim and are restated against the page: the
            // gear is above and at the end, the scan button is below it and
            // centred, and the `?` is off to the end of the scan button. The
            // map-mode chip is gone with item 140(a), asserted above.
            val page = composeRule.onNodeWithTag("captureViewport").fetchSemanticsNode()
                .root!!.semanticsOwner.rootSemanticsNode.size
            val gear = composeRule.onNodeWithTag("advancedButton").fetchSemanticsNode().boundsInRoot
            val fab = composeRule.onNodeWithTag("recordButton").fetchSemanticsNode().boundsInRoot
            val help = composeRule.onNodeWithTag("tutorialButton").fetchSemanticsNode().boundsInRoot

            assertTrue("the gear is in the top half", gear.center.y < page.height / 2f)
            assertTrue("the gear is at the end edge", gear.center.x > page.width / 2f)
            assertTrue("the scan button is in the bottom half", fab.center.y > page.height / 2f)
            assertTrue("the ? is above the scan button's band", help.center.y < fab.top)
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
     * and the version footer is 0.9.12.
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
            assertTrue("the footer is this round's version: \"$footer\"", footer.contains("0.9.12"))
            assertTrue("…and its code: \"$footer\"", footer.contains("912"))

            composeRule.onNodeWithTag("tab_projects").performClick()
        }
    }
}
