package com.lidarscan.app

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.ui.capture.ScanControlCluster
import com.lidarscan.app.ui.capture.StartHoldModal
import com.lidarscan.app.ui.theme.LidarScanTheme
import com.lidarscan.core.calib.DeviceOrientation
import com.lidarscan.core.calib.HoldOrientation
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assume
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 30 item 175 — **the attitude instrument, on a device, moving.**
 *
 * The owner's report was that the needle does not move on his Pixel. The angle
 * mapping had been unit tested since round 28 and was right; what was wrong was
 * the wire, and a wire is exactly the thing a JVM test cannot see. So these
 * tests compose the **production** hold-still card — `StartHoldModal`, not a
 * copy of it — and drive it two ways:
 *
 *  * [theCardsNeedleFollowsTheFlowItIsGiven] pushes values into the flow the
 *    screen passes and reads the instrument's own accessibility description
 *    back. That is the state half: the composable re-reads and re-draws when
 *    the source moves, which round 28's frozen `startOrientation` never did.
 *  * [theCardsNeedleReadsThisDevicesRealAttitude] binds the card to the REAL
 *    `AppContainer.attitudeSource` and waits for the phone (or the emulator's
 *    virtual accelerometer) to reach the needle. That is the wire half, end to
 *    end: `SensorManager` → `LiveAttitudeFeed` → `StateFlow` → the card.
 *
 * [theHoldCardStaysUpForTheScreenshots] is the same wiring held on screen long
 * enough for a host to drive `adb emu sensor set acceleration` between shots.
 * It is assumed-skipped unless the run asks for it by name
 * (`-e attitudeShots 1`), because a test that idles for half a minute has no
 * business in a suite that runs on every push.
 */
@RunWith(AndroidJUnit4::class)
class Round30AttitudeTest {

    @get:Rule
    val composeRule = createComposeRule()

    private val container: com.lidarscan.app.di.AppContainer
        get() = (ApplicationProvider.getApplicationContext() as LidarScanApplication).container

    /** What the instrument says about itself — the one thing a Canvas can be asked. */
    private fun needleDescription(): String? =
        composeRule.onAllNodesWithTag("attitudeIndicator", useUnmergedTree = true)
            .fetchSemanticsNodes()
            .firstOrNull()
            ?.config
            ?.getOrNull(SemanticsProperties.ContentDescription)
            ?.firstOrNull()

    private fun holdCard(attitude: kotlinx.coroutines.flow.StateFlow<HoldOrientation?>) {
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
                        )
                    }
                }
            }
        }
        composeRule.waitForIdle()
    }

    private fun recordingStrip(attitude: kotlinx.coroutines.flow.StateFlow<HoldOrientation?>) {
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
                            attitude = attitude,
                        )
                    }
                }
            }
        }
        composeRule.waitForIdle()
    }

    private fun upright(angleDeg: Double) = HoldOrientation(
        orientation = DeviceOrientation.PORTRAIT,
        screenUpAngleDeg = angleDeg,
        tiltFromFlatDeg = 90.0,
        confident = true,
    )

    @Test
    fun theCardsNeedleFollowsTheFlowItIsGiven() {
        val source = MutableStateFlow<HoldOrientation?>(null)
        holdCard(source)

        // Nothing yet: a ring and no needle, and it says so.
        composeRule.onNodeWithTag("startModalCard").assertExists()
        assertEquals("Attitude unavailable", needleDescription())

        source.value = upright(0.0)
        composeRule.waitForIdle()
        assertEquals("Rig level", needleDescription())

        source.value = upright(15.0)
        composeRule.waitForIdle()
        val tipped = needleDescription()
        assertEquals("Rig 15 degrees off square", tipped)

        // Round 26's orientation detection, in the instrument: a landscape hold
        // is level, not 90 degrees wrong. This is what makes the card usable in
        // the hold the owner actually uses.
        source.value = HoldOrientation(DeviceOrientation.LANDSCAPE_LEFT, 90.0, 90.0, true)
        composeRule.waitForIdle()
        assertEquals("Rig level", needleDescription())

        source.value = HoldOrientation(DeviceOrientation.LANDSCAPE_LEFT, 130.0, 90.0, true)
        composeRule.waitForIdle()
        assertEquals("Rig 40 degrees off square", needleDescription())

        // And back to nothing, which is what releasing the feed publishes.
        source.value = null
        composeRule.waitForIdle()
        assertEquals("Attitude unavailable", needleDescription())
    }

    @Test
    fun theCardsNeedleReadsThisDevicesRealAttitude() {
        val source = container.attitudeSource
        Assume.assumeTrue("this device has no gravity or accelerometer sensor", source.available)
        source.acquire()
        try {
            holdCard(source.attitude)
            // The emulator reports gravity from the moment it boots and a real
            // phone from the moment it is picked up, so a reading has to arrive
            // — the failure this asserts against is the round-28 one, where
            // nothing ever arrived because nothing was ever connected.
            composeRule.waitUntil(timeoutMillis = 10_000) {
                needleDescription() != null && needleDescription() != "Attitude unavailable"
            }
            assertNotEquals("Attitude unavailable", needleDescription())
            assertTrue(
                "the instrument read '${needleDescription()}'",
                needleDescription()!!.startsWith("Rig "),
            )
        } finally {
            source.release()
        }
    }

    @Test
    fun releasingTheFeedStopsTheInstrument() {
        val source = container.attitudeSource
        Assume.assumeTrue("this device has no gravity or accelerometer sensor", source.available)
        source.acquire()
        holdCard(source.attitude)
        composeRule.waitUntil(timeoutMillis = 10_000) {
            needleDescription() != null && needleDescription() != "Attitude unavailable"
        }
        source.release()
        composeRule.waitForIdle()
        // Battery, stated as a visible fact: with nothing acquired the feed
        // publishes null and the needle goes, rather than sitting on the last
        // value it happened to hold.
        assertEquals("Attitude unavailable", needleDescription())
    }

    @Test
    fun theRecordingStripsInstrumentReadsTheSameFeed() {
        // Item 175's second placement. The two draw the same component from the
        // same flow, and the point of asserting both is that round 28 wired
        // them from one `startOrientationRollDeg` and BOTH were dead — a fix
        // proven on one placement proves nothing about the other.
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
        assertEquals("Attitude unavailable", needleDescription())
        source.value = upright(0.0)
        composeRule.waitForIdle()
        assertEquals("Rig level", needleDescription())
        source.value = upright(-40.0)
        composeRule.waitForIdle()
        assertEquals("Rig 40 degrees off square", needleDescription())
    }

    /**
     * Not a test of the app — a stage for one. Held for
     * [SHOT_HOLD_MS] so a host script can set three accelerations and take
     * three screenshots of the same production card; it then asserts that the
     * needle actually visited more than one reading while it waited, so a run
     * that produced identical screenshots fails rather than being filed.
     */
    @Test
    fun theHoldCardStaysUpForTheScreenshots() {
        Assume.assumeTrue(
            "run with -e attitudeShots 1",
            InstrumentationRegistry.getArguments().getString("attitudeShots") == "1",
        )
        val source = container.attitudeSource
        source.acquire()
        try {
            // Both placements are photographable from one harness: the card is
            // the default, `-e attitudeShotsPlacement strip` swaps in the
            // recording control row. One stage, two sets of shots, and no
            // second copy of the wiring under either.
            if (InstrumentationRegistry.getArguments().getString("attitudeShotsPlacement") == "strip") {
                recordingStrip(source.attitude)
            } else {
                holdCard(source.attitude)
            }
            val seen = linkedSetOf<String>()
            val end = android.os.SystemClock.elapsedRealtime() + SHOT_HOLD_MS
            while (android.os.SystemClock.elapsedRealtime() < end) {
                needleDescription()?.let { seen += it }
                Thread.sleep(200)
            }
            assertTrue("the needle never moved while the host drove the sensor: $seen", seen.size >= 3)
        } finally {
            source.release()
        }
    }

    private companion object {
        const val SHOT_HOLD_MS = 45_000L
    }
}
