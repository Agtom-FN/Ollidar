package com.lidarscan.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.compose.rememberNavController
import com.lidarscan.app.data.AppSettings
import com.lidarscan.app.debug.EXTRA_LAUNCH_REPLAY_CAPTURE
import com.lidarscan.app.debug.findOrCreateReplayProjectId
import com.lidarscan.app.ui.nav.LidarScanApp
import com.lidarscan.app.ui.nav.Routes
import com.lidarscan.app.ui.theme.LidarScanTheme
import com.lidarscan.app.ui.welcome.WelcomeLaunchGate
import com.lidarscan.app.ui.welcome.WelcomeOverlay
import com.lidarscan.app.ui.welcome.WelcomeReducedMotion
import com.lidarscan.core.welcome.WelcomeAnimation

class MainActivity : ComponentActivity() {

    /**
     * B2: covers the app-already-in-foreground USB attach/detach case (the
     * manifest's USB_DEVICE_ATTACHED intent-filter only fires a cold or
     * background launch). Forwards into [com.lidarscan.app.di.AppContainer.usbAttachEvents]
     * so an active [com.lidarscan.app.ui.connect.ConnectWizardViewModel] re-scans.
     */
    private val usbAttachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED ||
                intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED
            ) {
                (application as LidarScanApplication).container.usbAttachEvents.tryEmit(Unit)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val container = (application as LidarScanApplication).container

        val usbFilter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbAttachReceiver, usbFilter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(usbAttachReceiver, usbFilter)
        }

        setContent {
            // ── ROUND 32 item 177 ────────────────────────────────────────────
            //
            // Collected as a NULLABLE here, and the null is load-bearing.
            // `AppSettings()` is a perfectly good placeholder for a theme — a
            // fresh install and an unread DataStore want the same dark screen —
            // but it is a WRONG ANSWER for the welcome animation: its defaults
            // say "developer mode off", so deciding from them would show a
            // developer the ordinary film every single launch and there would
            // be no symptom other than the wrong llama. So the decision waits
            // for the store's first real emission, which arrives in a few
            // milliseconds and is the same moment the page underneath draws.
            val storedSettings by container.settingsRepository.settings
                .collectAsStateWithLifecycle(initialValue = null)
            val settings = storedSettings ?: AppSettings()
            val navController = rememberNavController()

            var welcome by remember { mutableStateOf<WelcomeAnimation.Variant?>(null) }
            var welcomeDecided by remember { mutableStateOf(false) }
            LaunchedEffect(storedSettings != null) {
                val loaded = storedSettings ?: return@LaunchedEffect
                if (welcomeDecided) return@LaunchedEffect
                welcomeDecided = true
                welcome = WelcomeAnimation.variantFor(
                    WelcomeAnimation.Launch(
                        enabled = loaded.welcomeAnimation,
                        developerMode = loaded.developerMode,
                        reducedMotion = WelcomeReducedMotion.isOn(applicationContext),
                        // The one-shot, taken here: a rotation re-runs this
                        // effect, finds the claim gone, and shows nothing.
                        firstInProcess = WelcomeLaunchGate.claimFirstLaunch(),
                    ),
                )
            }

            // Debug-only deep link (see com.lidarscan.app.debug.ReplayDeepLink):
            // `-e EXTRA_LAUNCH_REPLAY_CAPTURE true` / an Intent extra jumps
            // straight to the "Replay synthetic capture" acceptance path
            // instead of requiring Projects -> Settings -> tap. Read once per
            // Activity instance (LaunchedEffect(Unit)); a config change does
            // not re-trigger a second navigate.
            LaunchedEffect(Unit) {
                if (intent.getBooleanExtra(EXTRA_LAUNCH_REPLAY_CAPTURE, false)) {
                    val projectId = findOrCreateReplayProjectId(container)
                    navController.navigate(Routes.replayCapture(projectId))
                }
            }

            LidarScanTheme(themeMode = settings.themeMode) {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    Box(Modifier.fillMaxSize()) {
                        // The app is composed FIRST and underneath: the page
                        // loads while the film plays, so "interactive the
                        // moment it ends" is not a promise the overlay has to
                        // keep — there is nothing left to do when it goes.
                        LidarScanApp(container = container, navController = navController)
                        // ROUND 32 item 177 — the ~80 ms before the store's
                        // first emission, which is the window the decision
                        // above waits for.
                        //
                        // Found in the recording: the launch splash left, the
                        // Projects page drew one frame, and THEN the film
                        // started over the top of it. One frame of a page you
                        // are about to cover reads as a glitch, not as
                        // honesty. This holds the page's own colour across
                        // that gap — the same ground the film's scrim uses, so
                        // when the film does start there is no seam, and when
                        // the answer is "no film" it simply goes.
                        if (!welcomeDecided) {
                            Box(
                                Modifier
                                    .fillMaxSize()
                                    .background(MaterialTheme.colorScheme.background),
                            )
                        }
                        welcome?.let { variant ->
                            WelcomeOverlay(variant = variant, onFinished = { welcome = null })
                        }
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        runCatching { unregisterReceiver(usbAttachReceiver) }
        super.onDestroy()
    }
}
