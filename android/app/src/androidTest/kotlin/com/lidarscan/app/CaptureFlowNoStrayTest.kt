package com.lidarscan.app

import android.content.Intent
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.core.app.ActivityScenario
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.lidarscan.app.debug.EXTRA_LAUNCH_REPLAY_CAPTURE
import com.lidarscan.app.debug.REPLAY_PROJECT_NAME
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 9, owner item 33, on a real device/emulator:
 *
 * > "Entering Capture = a new-scan context; leaving WITHOUT ever starting a
 * > recording must leave NO project behind (no dir, no list entry).
 * > record+stop keeps + redirects (as shipped)."
 *
 * `CaptureRound9FlowTest` pins the same contract at the ViewModel, against a
 * temp directory and a fake bridge. This one is the version the owner would
 * perform by hand: open the app, count the scans, go to Capture, come back
 * without touching Start, count again — and then actually record something and
 * confirm THAT is still kept. It is the only place the whole shell (navigation,
 * the real `AppContainer` store, the real Projects list) is in the loop.
 *
 * Deliberately counts `projectCard` nodes rather than reading the store: what
 * the owner complained about is a list with junk in it, so the list is what is
 * asserted on.
 */
@RunWith(AndroidJUnit4::class)
class CaptureFlowNoStrayTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    /** Cards currently on the Projects list — selected or not. */
    private fun visibleProjectCards(): Int =
        composeRule.onAllNodesWithTag("projectCard").fetchSemanticsNodes().size +
            composeRule.onAllNodesWithTag("projectCardSelected").fetchSemanticsNodes().size

    private fun awaitProjectsTab() {
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching {
                composeRule.onAllNodesWithTag("projectsAvatar").fetchSemanticsNodes().isNotEmpty()
            }.getOrDefault(false)
        }
    }

    /**
     * The leave-without-recording half.
     *
     * On a bare emulator the Capture tab's Start is permanently disabled (no D6,
     * no Mid-360 — see `ReplayCaptureSmokeTest`), which makes this AVD the
     * perfect stand-in for the operator who opens Capture, looks at it and
     * leaves: there is no way to accidentally record, so anything that appears
     * in the list afterwards is a stray by definition.
     */
    @Test
    fun enteringCaptureAndLeavingWithoutRecordingAddsNoProject() {
        ActivityScenario.launch(MainActivity::class.java).use {
            awaitProjectsTab()
            val before = visibleProjectCards()

            // ROUND 22 item 94: the tab is labelled "Scan" now. Driven by its
            // stable test tag rather than its label, so the next rewording does
            // not break the emulator suite.
            composeRule.onNodeWithTag("tab_capture").performClick()

            // Fully entered: the new-scan name field is up and auto-detect is
            // running. Anything screen entry was going to create, it has created
            // by the time the manual fallback appears (auto-detect races a 5 s
            // Mid-360 listen, so this also covers the slow path).
            composeRule.waitUntil(timeoutMillis = 20_000) {
                composeRule.onAllNodesWithTag("scanNameField").fetchSemanticsNodes().isNotEmpty()
            }
            composeRule.onNodeWithContentDescription("Start a scan").assertExists()
            composeRule.waitUntil(timeoutMillis = 40_000) {
                composeRule.onAllNodesWithTag("manualEntryPanel").fetchSemanticsNodes().isNotEmpty()
            }

            // …and out again, without ever pressing Start.
            // ROUND 22 item 94: driven by the tab's stable test tag rather than by
            // its visible label, like the Scan tab above — a rewording must not
            // be able to break the emulator suite.
            composeRule.onNodeWithTag("tab_projects").performClick()
            awaitProjectsTab()
            composeRule.waitForIdle()

            val after = visibleProjectCards()
            assertEquals(
                "opening the Capture tab and leaving without recording must add NOTHING to the Projects " +
                    "list (was $before card(s), now $after)",
                before,
                after,
            )
        }
    }

    /**
     * The record-and-stop half: a capture that actually produced points is
     * **kept**, and is in the list afterwards.
     *
     * The replay deep link is the one path on a hardware-free AVD that can reach
     * RECORDING at all (same reasoning as `ReplayCaptureSmokeTest`), and a
     * replay decodes real points into the live stats — which is exactly the
     * condition item 33's prune is gated on. If the prune were ever wired to
     * fire on a session with points in it, or on a replay, this test loses the
     * "Synthetic Replay Demo" row.
     */
    @Test
    fun aRecordedScanIsStillKept() {
        val intent = Intent(
            ApplicationProvider.getApplicationContext(),
            MainActivity::class.java,
        ).apply { putExtra(EXTRA_LAUNCH_REPLAY_CAPTURE, true) }

        ActivityScenario.launch<MainActivity>(intent).use {
            composeRule.waitUntil(timeoutMillis = 30_000) {
                runCatching {
                    composeRule.onAllNodesWithContentDescription("Start replay").fetchSemanticsNodes().isNotEmpty()
                }.getOrDefault(false)
            }
            composeRule.onNodeWithContentDescription("Start replay").performClick()
            composeRule.waitUntil(timeoutMillis = 20_000) {
                composeRule.onAllNodesWithContentDescription("Stop recording").fetchSemanticsNodes().isNotEmpty()
            }

            // ~2 s of real decoding, so the session seals with a non-zero point
            // count rather than racing the first page.
            Thread.sleep(2_000)

            composeRule.onNodeWithContentDescription("Stop recording").performClick()
            composeRule.waitUntil(timeoutMillis = 20_000) {
                composeRule.onAllNodesWithContentDescription("Start replay").fetchSemanticsNodes().isNotEmpty()
            }

            // The replay route does not auto-navigate (that redirect is for real
            // captures), so walk to Projects the way the operator would.
            // ROUND 22 item 94: driven by the tab's stable test tag rather than by
            // its visible label, like the Scan tab above — a rewording must not
            // be able to break the emulator suite.
            composeRule.onNodeWithTag("tab_projects").performClick()
            awaitProjectsTab()
            composeRule.waitForIdle()

            assertTrue(
                "a scan that recorded points must still be in the Projects list after Stop",
                visibleProjectCards() > 0,
            )
            composeRule.waitUntil(timeoutMillis = 10_000) {
                composeRule.onAllNodesWithText(REPLAY_PROJECT_NAME).fetchSemanticsNodes().isNotEmpty()
            }
        }
    }
}
