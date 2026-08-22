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
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.ScanIconButton
import com.lidarscan.app.ui.components.ScanRow
import com.lidarscan.app.ui.components.ScanRowCard
import com.lidarscan.app.ui.components.SectionLabel
import com.lidarscan.app.ui.theme.ScanBody
import com.lidarscan.app.ui.theme.ScanColors
import com.lidarscan.app.ui.theme.ScanDisplay
import com.lidarscan.app.ui.theme.ScanMeta
import com.lidarscan.app.ui.theme.ScanMetaCaps
import com.lidarscan.app.ui.theme.ScanTitle
import com.lidarscan.core.Wording
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
 * ROUND 28 item 165 — **Profile, rebuilt as §D.8.**
 *
 * ## What was wrong (§A.8)
 *
 *  * **F1 — one card, two unrelated tasks, four labels, two duplicated words.**
 *    The card was titled *Send logs*, contained a button *Send logs*, then a
 *    text field whose only label was the placeholder *Send feedback*, then a
 *    disabled button *Send feedback*. Two jobs wearing four labels, two of them
 *    the same word twice. It is two rows now, each with one job: **Send
 *    diagnostics** hands the zip straight to the share sheet, **Send feedback**
 *    opens a compose sheet.
 *  * **F2** — the permanently-disabled *Send feedback* button was grey on grey
 *    at about 2:1 and read as broken. A control that can never be pressed until
 *    a field somewhere else is typed into does not belong on the page at all;
 *    it is inside the sheet, next to the field that enables it.
 *  * **F3** — the field's only label was a placeholder, which disappears on the
 *    first keystroke. The sheet's field carries a persistent label.
 *  * **F5** — about 600 px of dead space. The page is two cards of rows now and
 *    the second one absorbed three figures that used to live on the Projects
 *    header.
 *  * **F6 — ✅ the "This phone" table is the best-built pattern in the app** and
 *    is the model §C.4 generalised into `ScanRow`. It is **kept verbatim** in
 *    content and rhythm: meta-caps label left, mono value right, one row
 *    height. What is new is that `SCANS` now carries the point total and a
 *    `GEOREF` row carries the third figure, because item 151 deleted the
 *    Projects header that used to state all three.
 *
 * **F4 is not fixed here.** The tab bar is drawn on this page with no tab
 * active, which makes all four icons inert decoration — but the bar belongs to
 * `ui/nav`, so this screen only leaves the clearance for it.
 *
 * The back affordance stays (ROUND 27 pins it): an off-tab screen with no
 * active tab and no back arrow is a screen with no way out but the system
 * gesture.
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
    val facts = uiState.facts
    // F2/F3: composing feedback is a surface of its own, not four controls
    // stacked on a page.
    var composing by remember { mutableStateOf(false) }

    Column(
        Modifier
            .fillMaxSize()
            .background(ScanColors.page)
            .statusBarsPadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .testTag("profileScreen"),
    ) {
        // §C.2: one ScanDisplay per screen, with the way out on the same line.
        Row(
            Modifier
                .fillMaxWidth()
                .padding(
                    start = ScanDims.S2,
                    end = ScanDims.ScreenMargin,
                    top = ScanDims.S2,
                    bottom = ScanDims.S2,
                ),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
        ) {
            ScanIconButton(
                icon = Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = "Back",
                onClick = onBack,
                modifier = Modifier.testTag("profileBack"),
            )
            Text("Profile", style = ScanDisplay, color = ScanColors.ink)
        }

        // ── SUPPORT ─────────────────────────────────────────────────────────
        //
        // F1: two rows, one job each. They are first because they are why
        // anybody opens this page — the table below is what a support
        // conversation asks for next, and it is already on screen by then.
        SectionLabel("Support")
        ScanRowCard(
            modifier = Modifier
                .padding(horizontal = ScanDims.ScreenMargin)
                .testTag("feedbackCard"),
            rows = buildList<@Composable () -> Unit> {
                add {
                    // The disclosure sits ON the row, above the tap, because a
                    // disclosure the operator reads after tapping is not a
                    // disclosure. The route line is the second half of the same
                    // honesty: "Sent." later must not be a surprise about a
                    // server nobody configured.
                    SupportRow(
                        title = if (sending != null) FeedbackWording.SENDING else "Send diagnostics",
                        privacyNote = FeedbackWording.PRIVACY_NOTE,
                        routeNote = uiState.note,
                        onClick = onSendLogs,
                        enabled = sending == null,
                        testTag = "sendLogsButton",
                    )
                }
                add {
                    ScanRow(
                        title = FeedbackWording.SEND_FEEDBACK,
                        detail = "Tell us what went wrong.",
                        trailing = { Chevron() },
                        onClick = { composing = true },
                        modifier = Modifier.testTag("sendFeedbackRow"),
                    )
                }
                // ROUND 22 item 96's shape: a running job is visible progress on
                // the surface that started it, not a spinner somewhere else.
                if (sending != null) {
                    add {
                        LinearProgressIndicator(
                            progress = { sending },
                            color = ScanColors.primary,
                            trackColor = ScanColors.trough,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(ScanDims.CardPadding)
                                .testTag("feedbackProgress"),
                        )
                    }
                }
                // ROUND 7's rule: no user-triggered file operation ends
                // silently, and the failure says where the file is.
                uiState.result?.let { line ->
                    add {
                        Text(
                            line,
                            style = ScanMeta,
                            color = ScanColors.inkMute,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable(onClick = onDismissResult)
                                .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S3)
                                .testTag("feedbackResult"),
                        )
                    }
                }
            },
        )

        // ── THIS PHONE ──────────────────────────────────────────────────────
        //
        // F6, kept verbatim. Mono because every line is a value somebody is
        // going to read out over a phone call or paste into a message, and a
        // proportional font makes "0.9.13 (913)" harder to read aloud than it
        // needs to be. What the operator can see is what gets sent
        // (`DeviceFacts.asText`), which is the point of the card.
        SectionLabel("This phone")
        ScanRowCard(
            modifier = Modifier
                .padding(horizontal = ScanDims.ScreenMargin)
                .testTag("profileDeviceCard"),
            rows = listOf<@Composable () -> Unit>(
                {
                    FactRow(
                        "App",
                        ProfileFacts.appLine(Wording.APP_NAME, facts.appVersion, facts.versionCode),
                        "profileAppVersion",
                    )
                },
                {
                    FactRow(
                        "Device",
                        ProfileFacts.deviceLine(facts.deviceModel, facts.androidVersion),
                        "profileDeviceModel",
                    )
                },
                {
                    FactRow(
                        "Scans",
                        ProfileFacts.scansLine(facts.scanCount, uiState.totalPoints),
                        "profileScanCount",
                    )
                },
                {
                    FactRow(
                        "Georef",
                        ProfileFacts.georeferencedLine(uiState.georeferencedCount, facts.scanCount),
                        "profileGeoreferenced",
                    )
                },
                { FactRow("Storage", facts.storageLabel(), "profileStorage") },
            ),
        )

        // ROUND 28 item 165 (finding F4): the tab bar is HIDDEN on this route
        // now — a bar with no tab active is four inert glyphs — so the page
        // reserves a section gap and nothing else. Finding F5 is the other half
        // of the same line: this page used to end in ~600 px of dead space.
        Spacer(Modifier.height(ScanDims.SectionGap))
    }

    if (composing) {
        FeedbackSheet(
            message = message,
            onMessageChange = onMessageChange,
            sending = sending,
            onSend = {
                onSendFeedback()
                composing = false
            },
            onDismiss = { composing = false },
        )
    }
}

/**
 * The Send-diagnostics row: a title, the disclosure, and the route.
 *
 * Hand-built rather than a [ScanRow] for one reason — it carries **two** detail
 * lines with **two** test tags, and the emulator suite asserts both are
 * displayed. Everything else about it is a row: 56 dp floor, 16 dp padding, one
 * chevron because it goes somewhere (the system share sheet).
 */
@Composable
private fun SupportRow(
    title: String,
    privacyNote: String,
    routeNote: String,
    onClick: () -> Unit,
    enabled: Boolean,
    testTag: String,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S3)
            .testTag(testTag),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = ScanBody, color = ScanColors.ink)
            Text(
                privacyNote,
                style = ScanMeta,
                color = ScanColors.inkMute,
                modifier = Modifier.testTag("feedbackPrivacyNote"),
            )
            Text(
                routeNote,
                style = ScanMeta,
                color = ScanColors.inkMute,
                modifier = Modifier.testTag("feedbackRouteNote"),
            )
        }
        Chevron()
    }
}

/**
 * F6's row, unchanged: meta-caps label on the left, mono value on the right,
 * one row height.
 *
 * `ScanMetaCaps` for the label because `APP` / `DEVICE` / `GEOREF` are codes a
 * person spells out, not words a person says; `ScanMeta` for the value because
 * every one of them is a number or a version. This is the distinction §C.3
 * generalised from this exact card.
 */
@Composable
private fun FactRow(label: String, value: String, testTag: String) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = ScanDims.CardPadding, vertical = ScanDims.S3),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(ScanDims.IconGap),
    ) {
        Text(
            label.uppercase(),
            style = ScanMetaCaps,
            color = ScanColors.inkMute,
            modifier = Modifier.weight(1f),
        )
        Text(value, style = ScanMeta, color = ScanColors.ink, modifier = Modifier.testTag(testTag))
    }
}

/** A chevron means this row goes somewhere. */
@Composable
private fun Chevron() {
    Icon(
        Icons.AutoMirrored.Filled.KeyboardArrowRight,
        contentDescription = null,
        tint = ScanColors.inkFaint,
    )
}

/**
 * F2 + F3 — **composing feedback, on a surface of its own.**
 *
 * The page used to carry a text field with no persistent label and, under it, a
 * Primary button that was disabled every single time the page was opened. Both
 * defects come from putting a compose form on a summary page: the button has
 * nothing to do until the field has content, and the field has no room for a
 * label because it is sharing a card with an unrelated action.
 *
 * Here the field is the only thing on the surface, so it gets a real label that
 * survives the first keystroke, and the button is disabled for exactly as long
 * as it takes to type a word — which is what a disabled state is for. A blank
 * box is still not feedback: **Send diagnostics** on the page behind is what "I
 * have nothing to add" looks like.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FeedbackSheet(
    message: String,
    onMessageChange: (String) -> Unit,
    sending: Float?,
    onSend: () -> Unit,
    onDismiss: () -> Unit,
) {
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        containerColor = ScanColors.card,
        contentColor = ScanColors.ink,
        // ROUND 16 item 61: the app's sheet radius. Material would otherwise
        // hand this the PILL `extraLarge` shape the buttons use.
        shape = androidx.compose.foundation.shape.RoundedCornerShape(
            topStart = ScanDims.SheetRadius,
            topEnd = ScanDims.SheetRadius,
        ),
        modifier = Modifier.testTag("feedbackSheet"),
    ) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = ScanDims.ScreenMargin)
                .padding(bottom = ScanDims.S8)
                .navigationBarsPadding(),
        ) {
            Text(FeedbackWording.SEND_FEEDBACK, style = ScanTitle, color = ScanColors.ink)
            Spacer(Modifier.height(ScanDims.S3))
            OutlinedTextField(
                value = message,
                onValueChange = onMessageChange,
                // F3: a LABEL, which stays, rather than a placeholder that
                // vanishes on the first keystroke.
                label = { Text("What went wrong?") },
                minLines = 3,
                modifier = Modifier.fillMaxWidth().testTag("feedbackField"),
            )
            Spacer(Modifier.height(ScanDims.S3))
            Text(
                FeedbackWording.PRIVACY_NOTE,
                style = ScanMeta,
                color = ScanColors.inkMute,
            )
            Spacer(Modifier.height(ScanDims.S3))
            PrimaryPill(
                text = FeedbackWording.SEND_FEEDBACK,
                enabled = sending == null && message.isNotBlank(),
                onClick = onSend,
                modifier = Modifier.fillMaxWidth().testTag("sendFeedbackButton"),
            )
        }
    }
}
