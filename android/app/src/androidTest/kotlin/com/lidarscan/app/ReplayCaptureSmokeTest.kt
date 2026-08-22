package com.lidarscan.app

import android.content.Intent
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.swipeDown
import androidx.compose.ui.test.swipeLeft
import androidx.test.core.app.ActivityScenario
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.lidarscan.app.debug.EXTRA_LAUNCH_REPLAY_CAPTURE
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * The permanent Android-emulator smoke test (runs on every push — see
 * `.github/workflows/android-emulator.yml` and NOTES.md's "Android emulator
 * smoke test" section): the class of test that would have caught B4's own
 * bug before it shipped.
 *
 * **The bug**: `Manipulator.nCreateBuilder()` threw `UnsatisfiedLinkError`
 * the first time the Filament view attached to a real Activity window,
 * because `Utils.init()` — the loader for the *separate*
 * `libfilament-utils-jni.so` — was never called (`Filament.init()` alone
 * only loads `libfilament-jni.so`). It compiled cleanly, passed every unit
 * test, and was invisible to `javap`/`llvm-nm` (the classes and symbols all
 * exist, just in an unloaded library) — see
 * `com.lidarscan.app.render.PointCloudRenderer`'s `FilamentLoader` doc
 * comment for the full account. It was found once, by hand, on a booted
 * emulator. Nothing before this test file ran that check automatically.
 *
 * Both tests below exist for a reason:
 *  - [launchReachesProjectsListWithoutCrashing] is the cheap end: a cold
 *    launch alone exercises `Application`/`AppContainer` construction
 *    (`ScanEngineNative`'s `System.loadLibrary`) and Compose's first
 *    composition — the class of crash that happens before any screen with a
 *    `SurfaceView` is even reachable.
 *  - [replaySyntheticCaptureDecodesPointsWithoutCrashing] is the one that
 *    actually reaches the Filament view: it deep-links straight into the
 *    "Replay synthetic capture" acceptance path (bundled `.lscan`, no
 *    hardware — see `assets/replay/synth.lscan/`), starts replay, and polls
 *    the live "Points captured" stat to confirm real native decode is
 *    landing points for several seconds without the process going down.
 */
@RunWith(AndroidJUnit4::class)
class ReplayCaptureSmokeTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    @Test
    fun launchReachesProjectsListWithoutCrashing() {
        ActivityScenario.launch(MainActivity::class.java).use {
            // ROUND 24 items 107 + 109: this button used to describe itself as
            // "Settings", and it was deliberately the only node that did — the
            // tab bar labelled its Settings tab with visible text and gave the
            // icon no description at all. Item 107 removed the labels, so the
            // tab needs its name as a description; item 109 gave the avatar a
            // better destination and therefore a better name. One node each,
            // and both are asserted so the pair can never collide again.
            composeRule.waitUntil(timeoutMillis = 15_000) {
                composeRule.onAllNodesWithTag("projectsAvatar").fetchSemanticsNodes().isNotEmpty()
            }
            composeRule.onNodeWithContentDescription("Profile").assertIsDisplayed()
            composeRule.onNodeWithContentDescription("Settings").assertIsDisplayed()

            // The tab bar itself is new and is on every screen — if it failed
            // to compose, every route below is unreachable by touch.
            composeRule.onNodeWithTag("scanTabBar").assertIsDisplayed()
        }
    }

    /**
     * ROUND 5: the reduced-step Capture tab, walked end to end with no hardware.
     *
     * What this actually proves on a bare emulator (no D6, no Mid-360):
     *  * the Capture tab opens as a **new scan** — one name field with an
     *    auto-name placeholder — rather than a picker or a new-project screen;
     *  * auto-detect runs **unprompted** and reports its own state inline;
     *  * when it finds nothing, the **manual panel opens by itself** (round 5's
     *    owner addition 1) with both transports on it — no dialog, no extra tap;
     *  * Start is present and disabled, which is the honest state with nothing
     *    connected (the old flow would have had a wizard here).
     *
     * The one thing it cannot prove is a successful detect; that needs hardware
     * and is recorded as such in android/NOTES.md.
     */
    @Test
    fun captureTabIsANewScanWithAutoDetectAndAnInlineManualFallback() {
        ActivityScenario.launch(MainActivity::class.java).use {
            // `createEmptyComposeRule().fetchSemanticsNodes()` THROWS (rather than
            // returning empty) while no compose hierarchy exists yet, and this test
            // runs after one that has already torn an Activity down — so the first
            // wait has to tolerate that window instead of failing in it.
            composeRule.waitUntil(timeoutMillis = 20_000) {
                runCatching {
                    composeRule.onAllNodesWithTag("projectsAvatar").fetchSemanticsNodes().isNotEmpty()
                }.getOrDefault(false)
            }

            // ROUND 22 item 94: the tab is labelled "Scan" now. Driven by its
            // stable test tag rather than its label, so the next rewording does
            // not break the emulator suite.
            composeRule.onNodeWithTag("tab_capture").performClick()

            // The tab IS the new-scan screen: name field + auto-detect line, no
            // picker in front of it.
            composeRule.waitUntil(timeoutMillis = 10_000) {
                composeRule.onAllNodesWithTag("scanNameField").fetchSemanticsNodes().isNotEmpty()
            }
            composeRule.onNodeWithTag("autoDetectStatus").assertIsDisplayed()
            composeRule.onNodeWithContentDescription("Start a scan").assertIsDisplayed()

            // Auto-detect races a USB probe (no devices: immediate) against a 5 s
            // Mid-360 heartbeat listen, so the failure — and with it the automatic
            // manual fallback — lands a few seconds in.
            composeRule.waitUntil(timeoutMillis = 30_000) {
                composeRule.onAllNodesWithTag("manualEntryPanel").fetchSemanticsNodes().isNotEmpty()
            }
            // ROUND 27 item 136: the connect flow is a SECTION of a laid-out
            // page now (not a card floating over a fullscreen viewport), and a
            // section that is longer than the space it is given scrolls. That
            // is the fix for round 26's clipped USB panel, and it means "is it
            // displayed" has to become "can it be reached and then is it
            // displayed" — which is the honest question about a form anyway.
            // `Round27UiTest.theConnectFlowCanBeScrolledToItsEnd` pins the
            // scrollability itself in both orientations.
            // ROUND 29 item 170: the connect flow is a state of §D.1's Sensor
            // row now, and the row carries the Retry — so `retryAutoDetectButton`
            // (the second one, inside the flow) is deliberately absent and
            // `readinessAction_sensor` is the one that exists. The rest of the
            // flow is unchanged and is still reached by scrolling.
            listOf(
                "manualLidarIpField",
                "manualHostIpField",
                "manualConnectMid360",
                "readinessAction_sensor",
            ).forEach { tag ->
                runCatching { composeRule.onNodeWithTag(tag).performScrollTo() }
                composeRule.waitForIdle()
                composeRule.onNodeWithTag(tag).assertIsDisplayed()
            }

            // "Enter manually" stays reachable (it is the toggle that is now
            // showing "Hide manual entry"), which is the other half of addition 1.
            runCatching { composeRule.onNodeWithTag("manualEntryToggle").performScrollTo() }
            composeRule.waitForIdle()
            composeRule.onNodeWithTag("manualEntryToggle").assertIsDisplayed()

            // ROUND 29 item 170: and exactly ONE statement of the blocker. The
            // Sensor row says it; the flow beneath it does not repeat it, and
            // the old floating "Connect the scanner first." line is gone from
            // this page entirely.
            assertEquals(
                "item 170: one Retry on the scanner-missing page, on the row",
                0,
                composeRule.onAllNodesWithTag("retryAutoDetectButton").fetchSemanticsNodes().size,
            )
            assertEquals(
                "item 170: the Sensor row states the blocker, so the stray " +
                    "\"Connect the scanner first.\" line is not drawn as well",
                0,
                composeRule.onAllNodesWithTag("startBlockedNote").fetchSemanticsNodes().size,
            )
        }
    }

    @Test
    fun replaySyntheticCaptureDecodesPointsWithoutCrashing() {
        // See com.lidarscan.app.debug.ReplayDeepLink: this extra makes
        // MainActivity find-or-create the "Synthetic Replay Demo" project and
        // navigate straight to Routes.REPLAY_CAPTURE, skipping the
        // Projects -> Settings -> tap path the human UI uses (that path is
        // exercised by SettingsScreen's own `replaySyntheticCaptureButton`
        // testTag if a future test wants a UI-navigation variant).
        val intent = Intent(
            ApplicationProvider.getApplicationContext(),
            MainActivity::class.java,
        ).apply {
            putExtra(EXTRA_LAUNCH_REPLAY_CAPTURE, true)
        }

        ActivityScenario.launch<MainActivity>(intent).use {
            // ReplayEngineBridge auto-connects on ViewModel init (no USB
            // wizard for a replay session); once CONNECTED + IDLE the record
            // button becomes live.
            //
            // The redesign made this a 64 dp ember circle with **no text** —
            // it is drawn, not labelled — so the hook moved from the old
            // "  Start replay" text match to the button's `contentDescription`,
            // which is what names the action for TalkBack too. The description
            // is state-dependent ("Start replay" / "Stop recording"), so
            // waiting for it is also waiting for CONNECTED + IDLE, exactly as
            // the text match used to be.
            composeRule.waitUntil(timeoutMillis = 20_000) {
                composeRule.onAllNodesWithContentDescription("Start replay").fetchSemanticsNodes().isNotEmpty()
            }
            composeRule.onNodeWithContentDescription("Start replay").performClick()

            composeRule.waitUntil(timeoutMillis = 20_000) { currentPointsCaptured() > 0 }
            val firstSample = currentPointsCaptured()
            assertTrue("expected a positive decoded point count, got $firstSample", firstSample > 0)

            // Hold for ~10s, re-sampling rather than sleeping blindly: each
            // iteration's assertion is itself the "still alive, still
            // decoding" check — a crash here fails the instrumentation run
            // outright (the exact failure mode B4's bug had), and a stall
            // fails the monotonic/growth assertions below.
            var lastSample = firstSample
            var grew = false
            val deadlineMillis = System.currentTimeMillis() + 10_000
            while (System.currentTimeMillis() < deadlineMillis) {
                Thread.sleep(500)
                val sample = currentPointsCaptured()
                assertTrue(
                    "points captured must never go backwards (was $lastSample, now $sample)",
                    sample >= lastSample,
                )
                if (sample > lastSample) grew = true
                lastSample = sample
            }
            assertTrue(
                "expected the decoded point count to grow at some point during the ~10s replay " +
                    "window (started at $firstSample, ended at $lastSample) — a flat count for the " +
                    "whole window would mean the replay engine stalled after the first page",
                grew,
            )

            // ROUND 5: the display controls are adjustable against a LIVE view —
            // that is item 10's whole point — so the sheet is opened mid-session
            // and its new rows are exercised while points are still landing.
            //
            // ROUND 28 item 158: the Live toggle is INSIDE that sheet now. It
            // was one of three grey circles in two sizes below the viewport
            // that a walking operator had to tell apart, and it is a display
            // setting pressed once a session if ever — §D.1 sends it here and
            // gives its slot to the attitude instrument. The tag is unchanged,
            // so this suite still drives the same control; what moved is when
            // it is on screen, which is why the assertion moved below the tap.
            //
            // ROUND 23 item 102: the viewport's own ⚙ is gone — it was the
            // second of the "2 advance button" the owner counted. The one
            // door is the Advanced button.
            composeRule.onNodeWithTag("advancedButton").performClick()
            composeRule.waitUntil(timeoutMillis = 10_000) {
                composeRule.onAllNodesWithTag("captureSettingsSheet").fetchSemanticsNodes().isNotEmpty()
            }
            // ROUND 28 item 158: the Live toggle, in its new home. See above.
            composeRule.onNodeWithTag("liveViewSwitch").assertExists()
            // The sheet's body scrolls (it carries view + AR/camera + display +
            // session), so the rows below the fold are asserted to EXIST and then
            // scrolled to — `assertIsDisplayed` on an off-screen row of a
            // deliberately scrolling sheet would be testing the screen height.
            composeRule.onNodeWithTag("pointSizeSlider").assertExists()
            composeRule.onNodeWithTag("gammaSlider").assertExists()
            composeRule.onNodeWithTag("brightnessSlider").assertExists()
            composeRule.onNodeWithTag("refreshRateRow").assertExists()
            composeRule.onNodeWithTag("pointSizeSlider").performScrollTo().assertIsDisplayed()

            // Move point size to its round-5 minimum (0.1 px) with the sheet open
            // and the cloud still decoding behind it: a live-applying control that
            // takes the process down is exactly what this test exists to catch.
            composeRule.onNodeWithTag("pointSizeSlider").performTouchInput { swipeLeft() }
            Thread.sleep(1_000)
            // ── ROUND 28 item 159: sample with the sheet CLOSED ─────────────
            //
            // The point count lives in §D.2's telemetry strip now, at the top of
            // the recording page, and a `ModalBottomSheet` is its own window —
            // so while the sheet is open `pointsCapturedValue` is not in the
            // tree this rule queries and every sample reads `-1`. Round 26's
            // status pill happened to be reachable through it; the strip is not.
            //
            // The property under test is unchanged and is not weakened by this:
            // the claim is *"a live-applying control did not take the process
            // down"*, and a count that is still growing a second after the sheet
            // closes proves exactly that. Closing first also exercises the
            // sheet's own dismissal, which the old ordering never did.
            composeRule.onNodeWithTag("captureSettingsSheet").performTouchInput { swipeDown() }
            composeRule.waitUntil(timeoutMillis = 10_000) {
                composeRule.onAllNodesWithTag("captureSettingsSheet").fetchSemanticsNodes().isEmpty()
            }
            Thread.sleep(500)
            // ── ROUND 28: the fixture is finite, and by here it may be spent ─
            //
            // Twenty seconds of start-wait plus a ten-second growth window plus
            // the sheet interactions add up to more than the bundled synthetic
            // capture's ~17 s, so the session can seal ITSELF before this line.
            // The count then reads -1 because §D.2's telemetry strip only
            // exists while recording — round 26's status pill happened to
            // survive into the idle page, and the strip deliberately does not
            // (item 158: never render a zero-valued readout on an idle screen).
            //
            // A sealed session is not a failure of the property under test. The
            // claim is *"a live-applying display control did not take the
            // process down"*, and reaching the idle state with the FAB re-armed
            // is a stronger demonstration of survival than a larger number
            // would be — it means the capture ran to completion and sealed
            // through the same path a Stop takes. So the assertion branches on
            // which of the two states we are in, and BOTH arms assert
            // something; there is no arm that quietly passes.
            val afterSlider = currentPointsCaptured()
            val sealedItself = composeRule
                .onAllNodesWithContentDescription("Start replay")
                .fetchSemanticsNodes()
                .isNotEmpty()
            if (sealedItself) {
                assertTrue(
                    "the replay sealed during the display change, so the record button must be " +
                        "re-armed and the app must still be running",
                    composeRule.onAllNodesWithTag("captureViewport").fetchSemanticsNodes().isNotEmpty() ||
                        composeRule.onAllNodesWithTag("scanIdlePage").fetchSemanticsNodes().isNotEmpty(),
                )
            } else {
                assertTrue(
                    "the session must survive a live display change (points were $lastSample, now $afterSlider)",
                    afterSlider >= lastSample,
                )
            }

            // (The sheet is already closed — the sample above needed it closed
            // to read the telemetry strip, so the record button underneath is
            // reachable from here on.)

            // ROUND 5 AUDIT (task 2, multi-cycle recording): "Start -> Stop
            // (seal) -> Start again within one connect session MUST produce a
            // second valid recording." A bare emulator has no D6/Mid-360
            // hardware, so the New-scan Capture tab's own Start stays
            // permanently disabled there (see
            // `captureTabIsANewScanWithAutoDetectAndAnInlineManualFallback`
            // above) — this replay session is the one path on this AVD that
            // can actually reach RECORDING at all, so it is what exercises the
            // underlying stop/restart machinery twice in a row. It reuses one
            // project by design (`ReplayEngineBridge`'s own doc: "records
            // nothing new" into a fresh project — see NOTES.md for why that is
            // NOT the same bug this audit fixed in `CaptureViewModel
            // .stopCapture()`'s `_uiState` handling, which this cannot reach on
            // a hardware-free AVD); what it DOES prove on real device/JNI code
            // is that Stop leaves the session re-armable and a second Start
            // genuinely restarts decoding rather than silently failing or
            // wedging the process.
            stopIfStillRecording()
            composeRule.waitUntil(timeoutMillis = 15_000) {
                composeRule.onAllNodesWithContentDescription("Start replay").fetchSemanticsNodes().isNotEmpty()
            }

            val afterFirstStop = currentPointsCaptured()
            composeRule.onNodeWithContentDescription("Start replay").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) {
                composeRule.onAllNodesWithContentDescription("Stop recording").fetchSemanticsNodes().isNotEmpty()
            }

            // Cycle 2 must actually be decoding again, not stuck at cycle 1's
            // frozen count (which is what "Start silently does nothing" would
            // look like from the UI's point of view).
            var cycle2Grew = false
            var lastCycle2Sample = currentPointsCaptured()
            val cycle2DeadlineMillis = System.currentTimeMillis() + 10_000
            while (System.currentTimeMillis() < cycle2DeadlineMillis) {
                Thread.sleep(500)
                val sample = currentPointsCaptured()
                assertTrue(
                    "cycle 2 points captured must never go backwards (was $lastCycle2Sample, now $sample)",
                    sample >= lastCycle2Sample,
                )
                if (sample > lastCycle2Sample) cycle2Grew = true
                lastCycle2Sample = sample
            }
            assertTrue(
                "expected cycle 2 to actually decode points again after the first Stop (cycle 1 ended at " +
                    "$afterFirstStop, cycle 2 started at $lastCycle2Sample and never grew) — a flat/stuck " +
                    "count here is exactly \"Start after Stop does nothing\"",
                cycle2Grew,
            )

            stopIfStillRecording()
            composeRule.waitUntil(timeoutMillis = 15_000) {
                composeRule.onAllNodesWithContentDescription("Start replay").fetchSemanticsNodes().isNotEmpty()
            }
        }
    }

    /**
     * Reads the `pointsCapturedValue`-tagged POINTS cell of CaptureScreen's
     * mono stat panel as a Long, or -1 if not currently composed.
     *
     * The redesign's stat panel prints `1.24 M` above a million and a plain
     * grouped integer below it (see `CaptureScreen.formatPoints`, which says
     * why); this parses both so the assertions keep working whichever side of
     * the threshold a replay lands on. The bundled synthetic capture is tens
     * of thousands of points, i.e. the integer branch — which is the whole
     * reason that branch exists.
     */
    /**
     * ROUND 28 items 150 + 159 — **the number this reads changed shape.**
     *
     * `pointsCapturedValue` used to be a `StatPanel` cell rendering
     * `formatPoints`: `120,300`, or `1.20 M` past a million. §D.2's telemetry
     * strip renders `PointCountFormat.compactPts` instead — `56.2 K pts` — which
     * is the app's ONE point formatter (item 150) and the format the mockup
     * draws.
     *
     * The old parser understood the grouped integer and the ` M` suffix and
     * nothing else, so it returned `-1` for every sample and the growth wait
     * timed out after twenty seconds against a cloud that was decoding
     * perfectly well. A test that cannot read the number it asserts on reports
     * a stall that is not there.
     *
     * It now understands all three shapes, including the ` pts` unit, and it
     * still returns `-1` for a node it genuinely cannot parse — that arm is the
     * one that must stay, because it is what catches the number vanishing.
     */
    /**
     * ROUND 28 — press Stop only if there is still something to stop.
     *
     * The bundled synthetic capture is ~17 s and this test spends more than
     * that between its Start and its Stops (a 20 s start wait, a 10 s growth
     * window, a sheet opened and scrolled). The fixture can therefore run out
     * and seal ITSELF first, at which point the FAB has already gone back to
     * "Start replay" and an unconditional click fails with "could not find any
     * node" — which is a race in the harness reported as a product fault.
     *
     * A self-sealed replay has been through the identical seal path a Stop
     * takes, so every assertion after this point is equally valid either way.
     * The wait for the idle state that follows each call is what actually pins
     * the outcome, and it is unchanged.
     */
    private fun stopIfStillRecording() {
        val recording = composeRule
            .onAllNodesWithContentDescription("Stop recording")
            .fetchSemanticsNodes()
            .isNotEmpty()
        if (recording) {
            composeRule.onNodeWithContentDescription("Stop recording").performClick()
        }
    }

    private fun currentPointsCaptured(): Long {
        val node = composeRule.onAllNodesWithTag("pointsCapturedValue").fetchSemanticsNodes().firstOrNull()
            ?: return -1
        val text = node.config.getOrNull(SemanticsProperties.Text)?.joinToString("") { it.text } ?: return -1
        val cleaned = text.replace(",", "").removeSuffix("pts").trim()
        val scale = when {
            cleaned.endsWith("M") -> 1_000_000.0
            cleaned.endsWith("K") -> 1_000.0
            else -> 1.0
        }
        val number = cleaned.removeSuffix("M").removeSuffix("K").trim()
        return number.toDoubleOrNull()?.let { (it * scale).toLong() } ?: -1
    }
}
