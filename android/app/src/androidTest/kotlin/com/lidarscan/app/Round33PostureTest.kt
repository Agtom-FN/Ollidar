package com.lidarscan.app

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.lidarscan.app.ui.capture.ScanControlCluster
import com.lidarscan.app.ui.capture.StartHoldModal
import com.lidarscan.app.ui.theme.LidarScanTheme
import com.lidarscan.core.calib.DeviceOrientation
import com.lidarscan.core.calib.HoldOrientation
import com.lidarscan.core.capture.PostureIndicator
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 33 item 179(e) — **both placements, on a device, against the production
 * composables.**
 *
 * The same standard round 30 set and for the same reason: the failure this
 * feature can have is a WIRE, and a wire is the one thing a JVM test cannot
 * see. So nothing here is a copy — [StartHoldModal] and [ScanControlCluster]
 * are the composables the Scan screen builds, driven through the `StateFlow`
 * the Scan screen passes them.
 *
 * What is new in this round and therefore under test here:
 *
 *  * the card's ghost carries **two** axes, and shows the dominant correction
 *    under itself when it is out of tolerance — and nothing at all when it is
 *    not, which is the half a screenshot of a good posture would never catch;
 *  * the **edge**: one tick per good→bad crossing, not one per publication.
 *    At 20 Hz a rig held at 11° would buzz twenty times a second if this were
 *    a level test rather than an edge test;
 *  * the strip's bubble reads the same feed and has **no** text on it, which is
 *    item 179(c)'s explicit instruction rather than an omission.
 */
@RunWith(AndroidJUnit4::class)
class Round33PostureTest {

    @get:Rule
    val composeRule = createComposeRule()

    private fun description(): String? =
        composeRule.onAllNodesWithTag("attitudeIndicator", useUnmergedTree = true)
            .fetchSemanticsNodes()
            .firstOrNull()
            ?.config
            ?.getOrNull(SemanticsProperties.ContentDescription)
            ?.firstOrNull()

    /** A hold that leans [pitch] back and sits [roll] off square, held portrait. */
    private fun posture(pitch: Double, roll: Double) = HoldOrientation(
        orientation = DeviceOrientation.PORTRAIT,
        screenUpAngleDeg = roll,
        screenPitchDeg = pitch,
        tiltFromFlatDeg = 90.0 - kotlin.math.abs(pitch),
        confident = true,
    )

    private fun holdCard(
        attitude: MutableStateFlow<HoldOrientation?>,
        onPostureLost: () -> Unit = {},
    ) {
        composeRule.setContent {
            LidarScanTheme {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    Box(Modifier.fillMaxSize()) {
                        StartHoldModal(
                            secondsLeft = 8,
                            fraction = 0.25f,
                            label = "Hold still",
                            attitude = attitude,
                            onCancel = {},
                            onPostureLost = onPostureLost,
                        )
                    }
                }
            }
        }
        composeRule.waitForIdle()
    }

    @Test
    fun theCardsGhostReadsBothAxesFromTheOneFeed() {
        val source = MutableStateFlow<HoldOrientation?>(null)
        holdCard(source)

        composeRule.onNodeWithTag("startModalCard").assertExists()
        assertEquals("Attitude unavailable", description())
        composeRule.onNodeWithTag("postureGhost", useUnmergedTree = true).assertDoesNotExist()

        source.value = posture(pitch = 0.0, roll = 0.0)
        composeRule.waitForIdle()
        assertEquals("Rig level", description())
        composeRule.onNodeWithTag("postureGhost", useUnmergedTree = true).assertExists()
        // Nothing to correct, so nothing is said. A permanently reserved line
        // under a green instrument reads as a thing that failed to load.
        composeRule.onNodeWithTag("postureHint", useUnmergedTree = true).assertDoesNotExist()

        // Round 28's dial could not have seen this: dead level in the screen
        // plane, and the rig aimed twenty degrees at the floor.
        source.value = posture(pitch = 20.0, roll = 0.0)
        composeRule.waitForIdle()
        assertEquals("Rig 20 degrees off square", description())
        composeRule.onNodeWithText(PostureIndicator.HINT_TILT_FORWARD, useUnmergedTree = true).assertIsDisplayed()

        source.value = posture(pitch = -20.0, roll = 0.0)
        composeRule.waitForIdle()
        composeRule.onNodeWithText(PostureIndicator.HINT_TILT_BACK, useUnmergedTree = true).assertIsDisplayed()

        // Roll only: the old axis, still there, still snapping to the nearest
        // square hold.
        source.value = posture(pitch = 0.0, roll = -20.0)
        composeRule.waitForIdle()
        assertEquals("Rig 20 degrees off square", description())
        composeRule.onNodeWithText(PostureIndicator.HINT_LEVEL_LEFT, useUnmergedTree = true).assertIsDisplayed()

        // Both at once: 20 back and 15 over is 25 off posture, and exactly ONE
        // instruction — the dominant axis — is on screen.
        source.value = posture(pitch = 20.0, roll = 15.0)
        composeRule.waitForIdle()
        assertEquals("Rig 25 degrees off square", description())
        composeRule.onNodeWithText(PostureIndicator.HINT_TILT_FORWARD, useUnmergedTree = true).assertIsDisplayed()
        composeRule.onNodeWithText(PostureIndicator.HINT_LEVEL_RIGHT, useUnmergedTree = true).assertDoesNotExist()

        // A landscape hold is level, in both axes, exactly as it was in 0.9.15.
        source.value = HoldOrientation(DeviceOrientation.LANDSCAPE_LEFT, 90.0, 0.0, 90.0, true)
        composeRule.waitForIdle()
        assertEquals("Rig level", description())

        source.value = null
        composeRule.waitForIdle()
        assertEquals("Attitude unavailable", description())
        composeRule.onNodeWithTag("postureGhost", useUnmergedTree = true).assertDoesNotExist()
    }

    @Test
    fun theTickFiresOnTheEdgeAndNotOnEveryPublication() {
        var ticks = 0
        val source = MutableStateFlow<HoldOrientation?>(posture(0.0, 0.0))
        holdCard(source) { ticks++ }
        assertEquals("a level start must not buzz", 0, ticks)

        // Out. One tick.
        source.value = posture(pitch = 14.0, roll = 0.0)
        composeRule.waitForIdle()
        assertEquals(1, ticks)

        // Still out, four more publications at 20 Hz. Still one tick — this is
        // the assertion that stops the instrument becoming a rattle.
        listOf(15.0, 16.5, 15.2, 14.8).forEach {
            source.value = posture(pitch = it, roll = 0.0)
            composeRule.waitForIdle()
        }
        assertEquals("the tick fired on the level rather than on the edge", 1, ticks)

        // Back inside re-arms it, and the next crossing ticks again.
        source.value = posture(pitch = 2.0, roll = 0.0)
        composeRule.waitForIdle()
        assertEquals(1, ticks)
        source.value = posture(pitch = 0.0, roll = -30.0)
        composeRule.waitForIdle()
        assertEquals(2, ticks)

        // And losing the reading is not a crossing.
        source.value = null
        composeRule.waitForIdle()
        assertEquals(2, ticks)
    }

    @Test
    fun theRecordingStripsBubbleReadsTheSameFeedAndSaysNothing() {
        val source = MutableStateFlow<HoldOrientation?>(null)
        composeRule.setContent {
            LidarScanTheme {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    Box(Modifier.fillMaxSize()) {
                        ScanControlCluster(
                            vertical = false,
                            captureState = com.lidarscan.core.engine.CaptureState.RECORDING,
                            connected = true,
                            liveView = true,
                            isReplaySession = false,
                            pauseSupported = true,
                            onLiveViewChange = {},
                            onStart = {},
                            onPause = {},
                            onResume = {},
                            onStop = {},
                            attitude = source,
                        )
                    }
                }
            }
        }
        composeRule.waitForIdle()

        composeRule.onNodeWithTag("recordButton").assertExists()
        assertEquals("Attitude unavailable", description())

        source.value = posture(pitch = 0.0, roll = 0.0)
        composeRule.waitForIdle()
        assertEquals("Rig level", description())

        source.value = posture(pitch = 20.0, roll = 15.0)
        composeRule.waitForIdle()
        assertEquals("Rig 25 degrees off square", description())
        // Item 179(c): no text in the walking row. The instruction belongs to
        // the card, where the operator is standing still and reading it.
        composeRule.onNodeWithTag("postureHint", useUnmergedTree = true).assertDoesNotExist()
        composeRule.onNodeWithText(PostureIndicator.HINT_TILT_FORWARD, useUnmergedTree = true).assertDoesNotExist()
    }
}
