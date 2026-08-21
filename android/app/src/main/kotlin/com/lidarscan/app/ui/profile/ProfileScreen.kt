package com.lidarscan.app.ui.profile

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.BackBar
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanCard
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ink
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.core.Wording
import com.lidarscan.core.feedback.DeviceFacts
import com.lidarscan.core.feedback.FeedbackWording

@Composable
fun ProfileRoute(
    container: AppContainer,
    onBack: () -> Unit,
) {
    val viewModel: ProfileViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ProfileViewModel(
                    settingsRepository = container.settingsRepository,
                    projectStore = container.projectStore,
                    projectsRootDir = container.projectsRootDir,
                    sender = com.lidarscan.app.share.FeedbackSender(
                        // The APPLICATION context: the sender outlives this
                        // screen (see jobScope below) and holding an Activity
                        // across that is a leak with a nice name.
                        context = container.applicationContext,
                        captureLog = container.captureLog,
                    ),
                    // ROUND 22 item 90: a job the operator started must not die
                    // because they left the screen.
                    jobScope = container.containerScope,
                    appVersion = com.lidarscan.app.BuildConfig.VERSION_NAME,
                    versionCode = com.lidarscan.app.BuildConfig.VERSION_CODE,
                    deviceModel = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}",
                    androidVersion = android.os.Build.VERSION.RELEASE.orEmpty(),
                    engineAbi = runCatching {
                        if (com.lidarscan.app.engine.ScanEngineNative.isAvailable) {
                            com.lidarscan.app.engine.ScanEngineNative.nativeAbiVersion()
                        } else {
                            0
                        }
                    }.getOrDefault(0),
                )
            }
        },
    )
    LaunchedEffect(Unit) { viewModel.refresh() }
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val message by viewModel.message.collectAsStateWithLifecycle()

    ProfileScreen(
        uiState = uiState,
        message = message,
        onMessageChange = viewModel::setMessage,
        onSendLogs = { viewModel.send(withMessage = false) },
        onSendFeedback = { viewModel.send(withMessage = true) },
        onDismissResult = viewModel::dismissResult,
        onBack = onBack,
    )
}

/**
 * ROUND 24 item 109 — **Profile: what this phone is, and how to tell us about
 * it.**
 *
 * The avatar on the Projects hero opened Settings, which is a second door to a
 * tab that already has one in the bar three centimetres below it. It opens
 * this instead, and this is the page that answers the two questions a support
 * conversation always starts with — *what version are you on* and *can you
 * send me the log* — without either party having to explain where anything is.
 *
 * Three blocks, in the order they are needed:
 *
 *  1. **the device card** — the version footer's two numbers, the phone, the
 *     Android release, the engine ABI, and what the scans are costing in
 *     storage. Every one of these is a fact the app already knew and had
 *     scattered across the Settings footer, the diagnostics sheet and nowhere.
 *  2. **Send logs** — one button.
 *  3. **Feedback** — a box and one button.
 *
 * The privacy line sits above both buttons rather than under them, because a
 * disclosure the operator reads after tapping is not a disclosure.
 */
@Composable
fun ProfileScreen(
    uiState: ProfileViewModel.UiState,
    message: String,
    onMessageChange: (String) -> Unit,
    onSendLogs: () -> Unit,
    onSendFeedback: () -> Unit,
    onDismissResult: () -> Unit,
    onBack: () -> Unit,
) {
    val sending = uiState.sending
    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .testTag("profileScreen"),
    ) {
        BackBar(title = "Profile", subtitle = "this phone, and how to reach us", onBack = onBack)

        Column(
            Modifier.padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(18.dp),
        ) {
            DeviceCard(uiState.facts)

            // ── send logs + feedback ────────────────────────────────────────
            //
            // One card, because they are one action with and without a
            // sentence attached — and because two cards would imply two
            // different things happen to the log.
            ScanCard(modifier = Modifier.testTag("feedbackCard")) {
                Text(
                    FeedbackWording.SEND_LOGS,
                    fontFamily = DisplayFontFamily,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 16.sp,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Spacer(Modifier.height(4.dp))
                // The whole disclosure, before the button, in one line.
                Hint(
                    FeedbackWording.PRIVACY_NOTE,
                    color = InkFaint,
                    modifier = Modifier.testTag("feedbackPrivacyNote"),
                )
                Spacer(Modifier.height(2.dp))
                // …and which way it will go, so "Sent." later is not a
                // surprise about a server nobody configured.
                Hint(uiState.note, color = InkFaint, modifier = Modifier.testTag("feedbackRouteNote"))
                Spacer(Modifier.height(10.dp))
                SecondaryPill(
                    text = if (sending != null) FeedbackWording.SENDING else FeedbackWording.SEND_LOGS,
                    height = 48.dp,
                    enabled = sending == null,
                    onClick = onSendLogs,
                    modifier = Modifier.fillMaxWidth().testTag("sendLogsButton"),
                )

                Spacer(Modifier.height(14.dp))
                OutlinedTextField(
                    value = message,
                    onValueChange = onMessageChange,
                    label = { Text(FeedbackWording.SEND_FEEDBACK) },
                    placeholder = { Text(FeedbackWording.FEEDBACK_PLACEHOLDER) },
                    minLines = 3,
                    modifier = Modifier.fillMaxWidth().testTag("feedbackField"),
                )
                Spacer(Modifier.height(10.dp))
                PrimaryPill(
                    text = FeedbackWording.SEND_FEEDBACK,
                    height = 48.dp,
                    // A blank box is not feedback. The Send-logs button above
                    // is what "I have nothing to add" looks like.
                    enabled = sending == null && message.isNotBlank(),
                    onClick = onSendFeedback,
                    modifier = Modifier.fillMaxWidth().testTag("sendFeedbackButton"),
                )

                // ROUND 22 item 96's shape: a running job is visible progress
                // on the surface that started it, not a spinner somewhere else.
                if (sending != null) {
                    Spacer(Modifier.height(10.dp))
                    LinearProgressIndicator(
                        progress = { sending },
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.fillMaxWidth().testTag("feedbackProgress"),
                    )
                }
                // ROUND 7's rule: no user-triggered file operation ends
                // silently, and the failure says where the file is.
                uiState.result?.let { line ->
                    Spacer(Modifier.height(10.dp))
                    Text(
                        line,
                        style = MaterialTheme.typography.bodySmall,
                        color = Ink,
                        modifier = Modifier
                            .clickable(onClick = onDismissResult)
                            .testTag("feedbackResult"),
                    )
                }
            }
        }

        Spacer(Modifier.height(ScanDims.TabBarClearance))
    }
}

/**
 * The facts, as a mono block.
 *
 * Mono because every line is a value someone is going to read out over a
 * phone call or paste into a message, and a proportional font makes "0.9.9
 * (909)" harder to read aloud than it needs to be. The same six lines go into
 * the bundle (`DeviceFacts.asText`), which is the point: what the operator can
 * see is exactly what gets sent.
 */
@Composable
private fun DeviceCard(facts: DeviceFacts) {
    ScanCard(modifier = Modifier.testTag("profileDeviceCard")) {
        Text(
            "This phone",
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 16.sp,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.height(8.dp))
        FactRow("App", "${Wording.APP_NAME} ${facts.appVersion} (${facts.versionCode})", "profileAppVersion")
        FactRow("Device", facts.deviceModel, "profileDeviceModel")
        FactRow("Android", facts.androidVersion, "profileAndroidVersion")
        // The engine ABI is the one line here that is not for the operator —
        // it is for whoever reads the bundle, and it costs one row.
        FactRow("Engine", if (facts.engineAbi > 0) "ABI ${facts.engineAbi}" else "not loaded", "profileEngineAbi")
        FactRow("Scans", "${facts.scanCount}", "profileScanCount")
        FactRow("Storage", facts.storageLabel(), "profileStorage")
    }
}

@Composable
private fun FactRow(label: String, value: String, testTag: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
        Text(label.uppercase(), style = MonoLabel, color = InkFaint, modifier = Modifier.weight(1f))
        Text(
            value,
            style = MonoMeta,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.testTag(testTag),
        )
    }
}
