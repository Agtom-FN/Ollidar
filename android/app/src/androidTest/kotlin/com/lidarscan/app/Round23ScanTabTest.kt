package com.lidarscan.app

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 23 items 102, 103 and 101(b) — **the three things the owner could see**,
 * asserted on a real emulator because "the UI is not renamed" and "there are 2
 * advance button" are claims about pixels, and the only honest way to answer
 * them is to look.
 *
 * A bare AVD has no COIN-D6 and no Mid-360, which makes it the perfect rig for
 * item 101(b): the scan button is in its refusing state for the whole test, so
 * a tap on it is exactly the tap that used to do nothing at all.
 */
@RunWith(AndroidJUnit4::class)
class Round23ScanTabTest {

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

    /**
     * **Item 103.** The owner: *"The ui not renamed or changed as you asked."*
     *
     * `ScanTab.CAPTURE`'s label has read "Scan" since round 22, so this test
     * exists to make the claim checkable rather than arguable: the node that
     * carries the `tab_capture` tag must carry the word Scan, and nothing in
     * the tab bar may say Capture.
     */
    @Test
    fun theScanTabIsLabelledScan() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            // ── ROUND 24 item 107 ───────────────────────────────────────────
            //
            // The visible label is gone — the bar is icons only now — so the
            // claim this test exists to make moved to where the name lives:
            // the tab's `contentDescription`, which is the SAME `ScanTab.label`
            // string it used to draw. The rename stays checkable, and it is now
            // also what a screen reader announces.
            composeRule.onNode(hasTestTag("tab_capture") and hasContentDescription("Scan"))
                .assertExists()
            assertEquals(
                "the Scan tab must not still be named \"Capture\"",
                0,
                composeRule.onAllNodes(hasTestTag("tab_capture") and hasContentDescription("Capture"))
                    .fetchSemanticsNodes().size,
            )
            // …and no tab draws a text label at all any more.
            assertEquals(
                "ROUND 24 item 107: the tab bar carries icons, not words",
                0,
                composeRule.onAllNodes(hasTestTag("tab_capture") and hasText("Scan"))
                    .fetchSemanticsNodes().size,
            )
        }
    }

    /**
     * **Item 102.** The owner: *"there are 2 advance button in the scan."*
     *
     * There was: the round-22 `advancedButton` beside the scan button, and the
     * round-5 `captureSettingsButton` floating on the viewport's right edge,
     * both `Icons.Filled.Tune`, both opening `CaptureSheet.SETTINGS`. Exactly
     * one survives, and the tag of the other must be gone from the tree.
     */
    @Test
    fun thereIsExactlyOneAdvancedButton() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            openScanTab()

            assertEquals(
                "the Scan screen must carry exactly ONE Advanced door",
                1,
                composeRule.onAllNodesWithTag("advancedButton").fetchSemanticsNodes().size,
            )
            assertEquals(
                "the viewport's duplicate ⚙ must be gone, not merely moved",
                0,
                composeRule.onAllNodesWithTag("captureSettingsButton").fetchSemanticsNodes().size,
            )
            assertEquals(
                "…and so must the chip that opened the same sheet a third time",
                0,
                composeRule.onAllNodesWithTag("displaySheetChip").fetchSemanticsNodes().size,
            )

            // The one that is left must actually open the sheet.
            composeRule.onNodeWithTag("advancedButton").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) {
                composeRule.onAllNodesWithTag("captureSettingsSheet").fetchSemanticsNodes().isNotEmpty()
            }
            // ROUND 23 item 106(a): and the Detail row is IN it, at last.
            composeRule.onNodeWithTag("detailRow").assertExists()
        }
    }

    /**
     * **Item 101(b): a tap on the scan button is never silent.**
     *
     * With no sensor attached the press cannot start anything — which used to
     * mean `clickable(enabled = connected && …)` swallowed it with no log line,
     * no ViewModel call and no pixel changed. That is indistinguishable, from
     * the outside, from the navigation defect that actually caused this round's
     * complaint, and it is why three rounds of "the button is dead" were so
     * expensive. The button now answers.
     */
    @Test
    fun aScanTapThatCannotStartSaysWhy() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            openScanTab()

            // The standing reason, before anything is even pressed.
            //
            // ROUND 29 item 170: it is the **Sensor row** now, not a floating
            // `startBlockedNote`. The note said *"Connect the scanner first."*
            // in ink-faint under a row of pills; §D.1's row says
            // `Sensor · Not found` in bad colour, carries *"Plug it in, then
            // retry."* and has its own **Retry** — so the note became a second
            // sentence about one blocker and was removed from this page. The
            // claim item 101(b) makes is unchanged: the reason is on screen
            // before the press, and the press is answered.
            composeRule.waitUntil(timeoutMillis = 20_000) {
                composeRule.onAllNodesWithTag("readinessRow_sensor").fetchSemanticsNodes().isNotEmpty()
            }

            composeRule.onNodeWithTag("recordButton").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) {
                composeRule.onAllNodesWithTag("startTapRefusalNote").fetchSemanticsNodes().isNotEmpty()
            }
        }
    }
}
