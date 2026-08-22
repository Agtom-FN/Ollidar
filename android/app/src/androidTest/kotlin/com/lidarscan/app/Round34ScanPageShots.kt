package com.lidarscan.app

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.app.data.ThemeMode
import com.lidarscan.app.ui.capture.ScanControlCluster
import com.lidarscan.app.ui.capture.ScanReadyPage
import com.lidarscan.app.ui.theme.LidarScanTheme
import com.lidarscan.core.capture.ScanReadiness
import com.lidarscan.core.engine.CaptureState
import org.junit.Assume
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 34 item 180 — **the CONNECTED idle page, held still for a camera.**
 *
 * No COIN-D6 connects to an emulator (round 29 item 170's whole lesson), so the
 * state the owner actually scans in cannot be reached by driving the app. This
 * composes the **production** [ScanReadyPage] with a connected readiness set
 * and the **production** [ScanControlCluster] as its FAB, and holds it up while
 * the host takes the picture — the same harness pattern rounds 30 and 33 used
 * for the attitude and posture shots, and for the same reason: a screenshot of
 * a copy of a screen is evidence about the copy.
 *
 * Off unless asked for (`-e scanPageShots 1`), because a test that stands still
 * for twenty seconds has no business in a suite that runs on every push.
 */
@RunWith(AndroidJUnit4::class)
class Round34ScanPageShots {

    @get:Rule
    val composeRule = createComposeRule()

    /**
     * **The photographs for item 180's CONNECTED variant.**
     *
     * No COIN-D6 connects to an emulator (round 29 item 170's whole lesson), so
     * the state the owner actually scans in cannot be reached by driving the
     * app. This composes the **production** `ScanReadyPage` with a connected
     * readiness set and the **production** control cluster as its FAB, holds it
     * up, and lets the host take the screenshots — the same harness pattern
     * round 30 and round 33 used for the attitude and posture shots.
     *
     * Off by default (`-e scanPageShots 1`): a test that stands still for
     * twenty seconds has no business in a suite that runs on every push.
     */
    @Test
    fun theConnectedIdlePageStaysUpForTheScreenshots() {
        Assume.assumeTrue(
            "run with -e scanPageShots 1",
            InstrumentationRegistry.getArguments().getString("scanPageShots") == "1",
        )
        val dark = InstrumentationRegistry.getArguments().getString("scanPageShotsTheme") != "light"
        val rows = listOf(
            ScanReadiness.Row(
                title = "Sensor",
                value = "COIN-D6 connected",
                state = ScanReadiness.State.GOOD,
            ),
            ScanReadiness.Row(
                title = "Mount",
                value = "Set · 91.0°",
                state = ScanReadiness.State.GOOD,
            ),
            ScanReadiness.Row(
                title = "Tracking",
                value = "Ready",
                state = ScanReadiness.State.GOOD,
            ),
        )
        composeRule.setContent {
            LidarScanTheme(
                themeMode = if (dark) {
                    ThemeMode.DARK
                } else {
                    ThemeMode.LIGHT
                },
            ) {
                Surface(
                    Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background,
                ) {
                    ScanReadyPage(
                        statusLine = "COIN-D6 · Ready",
                        blocked = false,
                        onOpenAdvanced = {},
                        readiness = rows,
                        onReadinessAction = {},
                        connectFlow = null,
                        banners = {},
                        tutorialBanner = {},
                        fab = {
                            ScanControlCluster(
                                vertical = false,
                                captureState = com.lidarscan.core.engine.CaptureState.IDLE,
                                connected = true,
                                liveView = false,
                                isReplaySession = false,
                                pauseSupported = true,
                                onLiveViewChange = {},
                                onStart = {},
                                onPause = {},
                                onResume = {},
                                onStop = {},
                            )
                        },
                    )
                }
            }
        }
        composeRule.waitForIdle()
        Thread.sleep(SHOT_HOLD_MS)
    }

    private companion object {
        const val SHOT_HOLD_MS = 20_000L
    }

}
