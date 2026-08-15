package com.lidarscan.app

import android.content.Intent
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.core.app.ActivityScenario
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.lidarscan.app.debug.EXTRA_LAUNCH_REPLAY_CAPTURE
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
            composeRule.waitUntil(timeoutMillis = 15_000) {
                composeRule.onAllNodesWithContentDescription("Settings").fetchSemanticsNodes().isNotEmpty()
            }
            composeRule.onNodeWithContentDescription("Settings").assertIsDisplayed()
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
            // wizard for a replay session); once CONNECTED + IDLE the
            // "Start replay" button renders (CaptureScreen.kt's
            // RecordingControls). substring = true: the button's actual
            // text is "  Start replay" (two leading spaces, for icon
            // spacing — RecordingControls' Icon+Text row) and
            // onNodeWithText defaults to an EXACT match, which silently
            // never matches and spins the full waitUntil timeout — found by
            // running this test for real and adding temporary diagnostic
            // logs (see NOTES.md's "Android emulator smoke test" section).
            composeRule.waitUntil(timeoutMillis = 20_000) {
                composeRule.onAllNodesWithText("Start replay", substring = true).fetchSemanticsNodes().isNotEmpty()
            }
            composeRule.onNodeWithText("Start replay", substring = true).performClick()

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
        }
    }

    /** Reads CaptureScreen.kt's `pointsCapturedValue`-tagged StatRow value ("%,d" formatted) as a Long, or -1 if not currently composed. */
    private fun currentPointsCaptured(): Long {
        val node = composeRule.onAllNodesWithTag("pointsCapturedValue").fetchSemanticsNodes().firstOrNull()
            ?: return -1
        val text = node.config.getOrNull(SemanticsProperties.Text)?.joinToString("") { it.text } ?: return -1
        return text.replace(",", "").trim().toLongOrNull() ?: -1
    }
}
