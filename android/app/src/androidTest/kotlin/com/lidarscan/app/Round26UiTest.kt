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

            // ── ROUND 29 item 170 SUPERSEDES BOTH EARLIER READINGS ─────────
            //
            // Round 26 asserted the viewport was the whole window; round 27
            // item 136 restated it as "the preview takes the page's width and
            // sits below the status band", because the idle tab had become a
            // laid-out page with a framed preview in it.
            //
            // §D.1 removes the premise. The idle page has **no live view at
            // all** — item 158's whole argument is that the floor was gated on
            // "is a sensor attached" when the question is "is there anything to
            // draw", and before Start the answer is no. Round 28 built that for
            // the connected page; this round built it for the disconnected one,
            // which is the only page an AVD can render. So `captureViewport` is
            // not merely smaller here, it is absent, and the claim to assert is
            // the absence.
            //
            // Round 26's fullscreen claim survives untouched for the state it
            // was actually about — a RECORDING, where the picture is the
            // product and `ScanRecordingPage` gives it every weighted pixel.
            // `ReplayCaptureSmokeTest` is what reaches that state on this bench.
            assertEquals(
                "item 170: the idle page composes no live view — there is " +
                    "nothing in it until Start is pressed",
                0,
                count("captureViewport"),
            )
            // What the space became — the LAST SCAN card — is not asserted
            // here: it is present only on a phone that HAS a scan, and this
            // suite runs against whatever the AVD happens to hold. §D.1's own
            // rule is that the card collapses when there is nothing to show.
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
            // ── ROUND 28 item 158 supersedes three of round 26's claims ────
            //
            // Round 26 asserted a floating `?`, a live-view eye and a
            // `0 pts` readout on the idle Scan page. §D.1 removes all three,
            // and each removal is a decision this suite now RECORDS rather
            // than a line somebody quietly deleted — the same treatment round
            // 27 item 140(a) gave the map-mode chip six lines above.
            //
            //  * the `?` FAB floated inside the empty viewport, anchored to the
            //    one region with no content to explain. It is a row in the
            //    Advanced sheet.
            //  * the eye was one of three grey circles in two sizes. It is a
            //    switch in the Advanced sheet.
            //  * `0 pts` was one of three zero-valued readouts — `00:00`,
            //    `0 pts`, `0.0 m` — occupying the most valuable position on the
            //    screen to say that nothing had happened.
            // ── ROUND 29 item 170: the owner call the hotfix reported ───────
            //
            // The round-28 hotfix scoped this assertion to ONE and said why:
            // §D.1's page was the `compact && !isLandscape` branch, `compact`
            // is `useCompactChrome(connected, …)`, no D6 connects to an
            // emulator, so the AVD fell to round 27's `IdleScanLayout` and its
            // corner `?`. It also reported the open question — should the `?`
            // and the zero readout leave the DISCONNECTED page too — as an
            // owner call rather than taking it.
            //
            // He took it: *"the scan page design not aligned with your
            // [mockups]"*. Portrait idle is §D.1 in both states now, so this
            // bench finally renders the page item 158 designed, and all three
            // of its removals are visible here. **Zero**, not one: the tour is
            // reached from the Advanced sheet and from Settings › Tutorial,
            // which is where item 158 sent it, and `Round24UiTest` opens the
            // sheet to run it.
            assertEquals(
                "item 170: no corner ? on either idle page — the tour is a row " +
                    "in the Advanced sheet and a row in Settings",
                0,
                count("tutorialButton"),
            )
            assertEquals(
                "item 158: the live-view eye has left the transport row",
                0,
                count("liveViewSwitch"),
            )
            // ROUND 29 item 170, same reversal: `00:00 / 0 pts / 0.0 m` was
            // three readouts whose entire content is "nothing has happened",
            // and the owner photographed them again on 0.9.13 because the
            // disconnected page still carried round 27's status band. The only
            // other `pointsCapturedValue` in `CaptureScreen` belongs to item
            // 159's REC strip, which is composed while recording and never on
            // an idle page — so zero is the whole assertion.
            assertEquals(
                "item 170: never render a zero-valued readout — the idle page " +
                    "has no point count on either variant",
                0,
                count("pointsCapturedValue"),
            )
            // ROUND 28 item 168: the ATTITUDE instrument takes the eye's slot
            // while recording — and only while recording, which is why it is
            // absent here.
            assertEquals("item 168: no attitude instrument on an idle page", 0, count("attitudeIndicator"))

            assertEquals("item 124: one Advanced gear", 1, count("advancedButton"))
            assertEquals("item 124: one scan button", 1, count("recordButton"))

            // ── ROUND 27 item 136: the same claims, on a PAGE ──────────────
            //
            // Round 26 asserted four corners of a fullscreen viewport. The idle
            // tab is a laid-out page now, so "corner of the picture" is not the
            // right frame — but the SPATIAL claims that made those corners
            // useful survive verbatim and are restated against the page: the
            // gear is above and at the end, and the scan button is below it and
            // centred. The map-mode chip is gone with item 140(a) and the `?`
            // with item 158, both asserted above.
            val page = composeRule.onNodeWithTag("recordButton").fetchSemanticsNode()
                .root!!.semanticsOwner.rootSemanticsNode.size
            val gear = composeRule.onNodeWithTag("advancedButton").fetchSemanticsNode().boundsInRoot
            val fab = composeRule.onNodeWithTag("recordButton").fetchSemanticsNode().boundsInRoot

            assertTrue("the gear is in the top half", gear.center.y < page.height / 2f)
            assertTrue("the gear is at the end edge", gear.center.x > page.width / 2f)
            assertTrue("the scan button is in the bottom half", fab.center.y > page.height / 2f)
            // ROUND 28 item 158: the FAB is the ONE action on this page and it
            // is centred. It used to be a 156 dp circle between a 72 dp pause
            // and a 72 dp eye, which is not a centred primary — it is the
            // middle of three.
            assertTrue(
                "item 158: the scan button is centred (${fab.center.x} of ${page.width})",
                kotlin.math.abs(fab.center.x - page.width / 2f) < page.width * 0.08f,
            )
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
     * and the version footer is 0.9.13.
     *
     * The footer is read from `BuildConfig`, so a stale APK fails this rather
     * than passing quietly.
     *
     * ROUND 28 item 164: the footer used to be `Ollidar v0.9.12 (build 912)`,
     * rendered at about 1.5:1 — effectively invisible, which is finding T4.
     * §D.7 makes it a ROW: the label says `Version` and the value says
     * `0.9.13 (913)`, in `ScanMeta` at a legible ink-mute. So the NAME is
     * asserted where the operator actually reads it — the Projects hero, two
     * lines above — and this node keeps the two numbers, which is the half that
     * catches a stale APK.
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
            assertTrue("the footer is this round's version: \"$footer\"", footer.contains("0.9.14"))
            assertTrue("…and its code: \"$footer\"", footer.contains("914"))

            composeRule.onNodeWithTag("tab_projects").performClick()
        }
    }
}
