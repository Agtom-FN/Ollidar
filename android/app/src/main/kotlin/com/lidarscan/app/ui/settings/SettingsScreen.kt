package com.lidarscan.app.ui.settings

import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.data.AppSettings
import com.lidarscan.app.data.ThemeMode
import com.lidarscan.app.data.Units
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.HeroHeader
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.ScanCard
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SecondaryPill
import com.lidarscan.app.ui.components.SegmentedPill
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.app.ui.theme.MonoMeta
import com.lidarscan.core.model.CaptureDefaults
import com.lidarscan.core.model.WorkflowProfile

@Composable
fun SettingsRoute(
    container: AppContainer,
    onBack: () -> Unit,
    onReplaySyntheticCapture: (projectId: String) -> Unit = {},
) {
    val viewModel: SettingsViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                SettingsViewModel(
                    settingsRepository = container.settingsRepository,
                    projectStore = container.projectStore,
                    storageLocation = container.projectsRootDir.absolutePath,
                )
            }
        },
    )
    val settings by viewModel.settings.collectAsStateWithLifecycle()

    SettingsScreen(
        settings = settings,
        storageLocation = viewModel.storageLocation,
        nativeEngineAvailable = com.lidarscan.app.engine.ScanEngineNative.isAvailable,
        onUnitsChange = viewModel::setUnits,
        onThemeModeChange = viewModel::setThemeMode,
        onUseFakeEngineChange = viewModel::setUseFakeEngine,
        onCloudChange = viewModel::setCloud,
        onAllowPoorSyncChange = viewModel::setAllowPoorSyncColorize,
        onReplaySyntheticCapture = { viewModel.replaySyntheticCapture(onReplaySyntheticCapture) },
        onBack = onBack,
    )
}

/**
 * Settings, restyled to the redesign and otherwise unchanged.
 *
 * Every setting B1–D3 put here is still here and still does the same thing:
 * units, theme, the cloud server + token, the poor-clock-sync colorize policy,
 * the simulated-engine developer switch, the "Replay synthetic capture"
 * acceptance path (still tagged `replaySyntheticCaptureButton` for the
 * emulator smoke test), the read-only workflow-profile reference and the
 * storage location. What changed is the chrome: a hero title instead of a
 * `TopAppBar`, `ScanCard` panels, and the three-option rows became the same
 * segmented pill the rest of the app uses.
 *
 * Settings is a **tab** now, so it has no back arrow — the tab bar is the way
 * out, and [onBack] is what the system back gesture routes through.
 */
@Composable
fun SettingsScreen(
    settings: AppSettings,
    storageLocation: String,
    nativeEngineAvailable: Boolean,
    onUnitsChange: (Units) -> Unit,
    onThemeModeChange: (ThemeMode) -> Unit,
    onUseFakeEngineChange: (Boolean) -> Unit,
    onCloudChange: (String, String) -> Unit = { _, _ -> },
    onAllowPoorSyncChange: (Boolean) -> Unit = {},
    onReplaySyntheticCapture: () -> Unit = {},
    onBack: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .testTag("settingsScreen"),
    ) {
        HeroHeader(title = "Settings", subtitle = "device-level · applies to every project")

        Column(
            Modifier.padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(22.dp),
        ) {
            SettingsSection("Workflow profiles") { WorkflowProfilesCard() }

            SettingsSection("Units") {
                SegmentedPill(
                    options = Units.entries.map { it to it.displayName },
                    selected = settings.units,
                    onSelect = onUnitsChange,
                    height = 44.dp,
                )
            }

            SettingsSection("Theme") {
                SegmentedPill(
                    options = ThemeMode.entries.map { it to it.displayName },
                    selected = settings.themeMode,
                    onSelect = onThemeModeChange,
                    height = 44.dp,
                )
                Spacer(Modifier.height(8.dp))
                Hint(
                    "The redesign is a dark cockpit by intent; Light is a faithful inversion of the same " +
                        "tokens, not a second design.",
                    color = InkFaint,
                )
            }

            SettingsSection("Cloud processing") {
                CloudCard(settings.cloudBaseUrl, settings.cloudToken, onCloudChange)
            }

            SettingsSection("Processing") {
                ProcessingOptionsCard(settings.allowPoorSyncColorize, onAllowPoorSyncChange)
            }

            SettingsSection("Engine (developer)") {
                EngineCard(
                    useFakeEngine = settings.useFakeEngine,
                    nativeEngineAvailable = nativeEngineAvailable,
                    onUseFakeEngineChange = onUseFakeEngineChange,
                    onReplaySyntheticCapture = onReplaySyntheticCapture,
                )
            }

            SettingsSection("Storage location") {
                ScanCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Filled.Folder, contentDescription = null, tint = Ember)
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(storageLocation, style = MonoMeta, color = MaterialTheme.colorScheme.onSurface)
                            Spacer(Modifier.height(6.dp))
                            Hint(
                                "Where .lscan projects are stored on this device. A location picker is a " +
                                    "future addition.",
                                color = InkFaint,
                            )
                        }
                    }
                }
            }
        }

        Spacer(Modifier.height(ScanDims.TabBarClearance))
    }
}

/** A mono, uppercase, ember-tinted section rule + its content. */
@Composable
private fun SettingsSection(title: String, content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        Text(title.uppercase(), style = MonoLabel, color = Ember)
        Spacer(Modifier.height(6.dp))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Spacer(Modifier.height(12.dp))
        content()
    }
}

@Composable
private fun CardTitle(text: String) {
    Text(
        text,
        fontFamily = DisplayFontFamily,
        fontWeight = FontWeight.SemiBold,
        fontSize = 16.sp,
        color = MaterialTheme.colorScheme.onSurface,
    )
}

@Composable
private fun ScanSwitchRow(
    title: String,
    detail: String,
    checked: Boolean,
    enabled: Boolean = true,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            CardTitle(title)
            Spacer(Modifier.height(4.dp))
            Hint(detail, color = InkFaint)
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            enabled = enabled,
            onCheckedChange = onCheckedChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = androidx.compose.ui.graphics.Color.White,
                checkedTrackColor = Ember,
                checkedBorderColor = Ember,
            ),
        )
    }
}

/**
 * B5 — what each workflow profile actually sets, surfaced where a user can read
 * it (Tech Spec §3.9's "profiles set defaults").
 *
 * This is deliberately a READ-ONLY reference rather than an editor. The
 * defaults are applied at project creation and then belong to the project
 * (`ProjectManifest.captureDefaults`), so a device-level editor here would
 * change nothing about any existing capture while looking like it did — and
 * the settings that matter per project are already editable where they are
 * used: Live-SLAM on the Capture screen, the export format on Processing, and
 * the whole display block in Review's panel.
 */
@Composable
private fun WorkflowProfilesCard() {
    var expanded by remember { mutableStateOf(false) }
    ScanCard {
        Hint(
            "A profile is a bundle of capture and display defaults, applied when a project is created. " +
                "Changing a project afterwards changes that project only.",
            color = InkFaint,
        )
        if (expanded) {
            WorkflowProfile.entries.forEach { profile ->
                val d = CaptureDefaults.forProfile(profile)
                Spacer(Modifier.height(14.dp))
                CardTitle(profile.displayName)
                Spacer(Modifier.height(3.dp))
                Hint(profile.description, color = InkFaint)
                Spacer(Modifier.height(4.dp))
                Text(
                    listOf(
                        if (d.liveSlam) "Live-SLAM" else "Record-only",
                        "export ${d.exportFormat.displayName}",
                        "display ${d.displayProfile.displayName}",
                        if (d.captureCameraKeyframes) "camera keyframes on" else "no camera",
                        if (d.requireRtkFixForCapture) "blocks below ${d.minFixForCapture.label}" else "no RTK gate",
                    ).joinToString(" · "),
                    style = MonoMeta,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.height(8.dp))
        TextButton(onClick = { expanded = !expanded }, contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
            Text(
                if (expanded) "Hide details" else "Show what each profile sets",
                color = Ember,
            )
        }
    }
}

/** D3 — where the Cloud processing mode uploads to (§3.8). */
@Composable
private fun CloudCard(baseUrl: String, token: String, onChange: (String, String) -> Unit) {
    var url by remember(baseUrl) { mutableStateOf(baseUrl) }
    var tok by remember(token) { mutableStateOf(token) }
    ScanCard {
        OutlinedTextField(
            value = url,
            onValueChange = { url = it },
            label = { Text("Server URL") },
            placeholder = { Text("https://cloud.example.com") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(
            value = tok,
            onValueChange = { tok = it },
            label = { Text("Token") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(8.dp))
        Hint(
            "Single-tenant, token auth, one worker — the MVP boundaries §3.8 makes contractual. The token is " +
                "stored in app-private preferences, not the Android Keystore; that is a stated limitation, and " +
                "it is why the service is meant to sit behind TLS.",
            color = InkFaint,
        )
        Spacer(Modifier.height(10.dp))
        SecondaryPill(text = "Save", height = 44.dp, onClick = { onChange(url, tok) })
    }
}

/** B6 — the one processing setting that is a policy statement rather than a per-run choice. */
@Composable
private fun ProcessingOptionsCard(allowPoorSync: Boolean, onChange: (Boolean) -> Unit) {
    ScanCard {
        ScanSwitchRow(
            title = "Colorize with poor clock sync",
            detail = "Above 15 ms of jitter, time sync alone eats most of the reprojection budget (S6), and the " +
                "colorizer refuses by default. Turning this on says the result is worth looking at, not worth " +
                "quoting.",
            checked = allowPoorSync,
            onCheckedChange = onChange,
        )
    }
}

@Composable
private fun EngineCard(
    useFakeEngine: Boolean,
    nativeEngineAvailable: Boolean,
    onUseFakeEngineChange: (Boolean) -> Unit,
    onReplaySyntheticCapture: () -> Unit,
) {
    ScanCard {
        ScanSwitchRow(
            title = "Use simulated engine",
            detail = if (nativeEngineAvailable) {
                "Takes effect after restarting the app."
            } else {
                "Native engine not loaded on this build — always simulated regardless of this switch."
            },
            checked = useFakeEngine || !nativeEngineAvailable,
            enabled = nativeEngineAvailable,
            onCheckedChange = onUseFakeEngineChange,
        )

        Spacer(Modifier.height(14.dp))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Spacer(Modifier.height(14.dp))

        // B4's acceptance path: drives the full Capture screen (live 3D view,
        // stat panel, session summary) from the bundled synthetic D6 capture
        // (S1's d6synth output) via ReplayEngineBridge — no hardware needed.
        CardTitle("Replay synthetic capture")
        Spacer(Modifier.height(4.dp))
        Hint(
            if (nativeEngineAvailable) {
                "Opens Capture driven by a bundled synthetic D6 recording — verifies the live 3D view end to " +
                    "end with no hardware."
            } else {
                "Native engine not loaded on this build — replay needs it, same as live capture."
            },
            color = InkFaint,
        )
        Spacer(Modifier.height(10.dp))
        SecondaryPill(
            text = "Replay synthetic capture",
            height = 44.dp,
            enabled = nativeEngineAvailable,
            onClick = onReplaySyntheticCapture,
            // testTag: the CI emulator smoke test's UI-navigation variant taps
            // this node directly — two Text nodes on this screen share the
            // string "Replay synthetic capture" (this button's label and the
            // heading above it), so a plain text match is ambiguous.
            modifier = Modifier.testTag("replaySyntheticCaptureButton"),
        )
    }
}
