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
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.lidarscan.app.data.AppSettings
import com.lidarscan.app.ui.nav.LidarScanApp
import com.lidarscan.app.ui.theme.LidarScanTheme

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
            val settings by container.settingsRepository.settings
                .collectAsStateWithLifecycle(initialValue = AppSettings())

            LidarScanTheme(themeMode = settings.themeMode) {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    LidarScanApp(container = container)
                }
            }
        }
    }

    override fun onDestroy() {
        runCatching { unregisterReceiver(usbAttachReceiver) }
        super.onDestroy()
    }
}
