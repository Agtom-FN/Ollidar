package com.lidarscan.app

import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 34 — **the card that went, and the film that became a secret.**
 *
 * Two owner orders, and both of them are claims about a screen rather than
 * about a function, so both are asserted on the AVD:
 *
 *  * **item 180** — the LAST SCAN card is gone from the idle Scan page, in both
 *    idle variants, and what is left is still a laid-out page rather than a
 *    readiness card floating in a void. Its absence is asserted as an ABSENCE
 *    (round 26's rule: a removal is recorded in exactly one place) and the
 *    order that replaces it is asserted as geometry, because "the flex region
 *    absorbs the space" is a sentence about rectangles;
 *  * **items 181 + 183(a)** — unlocking developer mode plays the easter egg
 *    over whatever is on screen, one touch removes it, the Settings page is
 *    live underneath it afterwards, and **re-locking is silent**.
 */
@RunWith(AndroidJUnit4::class)
class Round34UiTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    private fun has(tag: String): Boolean =
        composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty()

    private fun count(tag: String): Int =
        composeRule.onAllNodesWithTag(tag, useUnmergedTree = true).fetchSemanticsNodes().size

    private fun bounds(tag: String): Rect =
        composeRule.onNodeWithTag(tag, useUnmergedTree = true).fetchSemanticsNode().boundsInRoot

    private fun boundsOrNull(tag: String): Rect? =
        composeRule.onAllNodesWithTag(tag, useUnmergedTree = true)
            .fetchSemanticsNodes()
            .firstOrNull()
            ?.boundsInRoot

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

    // ══ item 180 — the LAST SCAN card ══════════════════════════════════════

    /**
     * **Item 180.** The card, its section label and its tag are absent.
     *
     * All three, because they are three different ways for it to survive a
     * removal: the tag catches the composable, the text catches a label left
     * behind above nothing, and the child tags catch a card whose own tag was
     * dropped while its contents were not.
     */
    @Test
    fun theLastScanCardIsGoneFromTheScanTab() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            for (tag in listOf("lastScanCard", "lastScanName", "lastScanMeta", "lastScanGrade")) {
                assertEquals("item 180: \"$tag\" must not be composed", 0, count(tag))
            }
            assertEquals(
                "item 180: the LAST SCAN section label goes with the card",
                0,
                composeRule.onAllNodesWithText("Last scan", ignoreCase = true)
                    .fetchSemanticsNodes().size,
            )
        }
    }

    /**
     * **Item 180, the half that is not an absence.** §D.1's order still holds
     * and the freed height went to the flex region.
     *
     * The claim in rectangles: the readiness card starts **immediately under**
     * the status bar (the content block is top-aligned, which is what the item
     * asks for on a tall screen), the FAB is below the whole card, and the tab
     * bar is below the FAB. A page that had centred its remaining content, or
     * pushed the card down to where the card used to end, would fail the first
     * of those three.
     */
    @Test
    fun theIdlePageIsStatusBarThenReadinessThenFab() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            composeRule.onNodeWithTag("scanIdlePage").assertIsDisplayed()
            val chrome = bounds("scanChromeColumn")
            val sensorRow = bounds("readinessRow_sensor")
            val fab = bounds("recordButton")

            assertTrue(
                "item 180: the readiness rows must sit at the TOP of the flex " +
                    "region (row top ${sensorRow.top}, column top ${chrome.top})",
                sensorRow.top - chrome.top < 220f,
            )
            assertTrue(
                "item 180: the FAB is below the readiness card " +
                    "(FAB top ${fab.top}, row bottom ${sensorRow.bottom})",
                fab.top >= sensorRow.bottom - 1f,
            )
            boundsOrNull("scanTabBar")?.let { bar ->
                assertTrue(
                    "item 180: the FAB keeps its own band above the tab bar",
                    fab.bottom <= bar.top + 1f,
                )
            }
        }
    }

    /**
     * **Item 180 at 320 × 687** — the compact profile the owner asked to be
     * checked, because a page that looks intentional on a tall screen is not
     * evidence about a short one.
     *
     * The same three-way order, plus the one thing that only goes wrong when
     * the window is short: the readiness card and the FAB must not collide.
     */
    @Test
    fun theIdlePageSurvivesTheCompactProfile() {
        val automation = InstrumentationRegistry.getInstrumentation().uiAutomation
        fun shell(cmd: String) {
            automation.executeShellCommand(cmd).close()
            Thread.sleep(1_500)
        }
        try {
            shell("wm size 320x687")
            shell("wm density 160")
            ActivityScenario.launch(MainActivity::class.java).use {
                openScanTab()
                val sensorRow = bounds("readinessRow_sensor")
                val fab = bounds("recordButton")
                assertTrue(
                    "item 180 (compact): the FAB must not print through the " +
                        "readiness card (FAB $fab, row $sensorRow)",
                    fab.top >= sensorRow.bottom - 1f,
                )
                assertEquals("item 180 (compact): still no card", 0, count("lastScanCard"))
            }
        } finally {
            shell("wm size reset")
            shell("wm density reset")
        }
    }

    // ══ items 181 + 183(a) — the easter egg ══════════════════════════════
    //
    // Not here. `Round34EggTest` carries it, and it has no Compose test rule,
    // because the film cannot be observed by a test that owns Compose's clock —
    // see that class's own note.
}
