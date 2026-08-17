package com.lidarscan.app.ui.capture

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SheetState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.ui.components.DiagRow
import com.lidarscan.app.ui.components.Hint
import com.lidarscan.app.ui.components.PrimaryPill
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.components.SegmentedPill
import com.lidarscan.app.ui.components.SheetRowLabel
import com.lidarscan.app.ui.components.SheetSection
import com.lidarscan.app.ui.components.SheetSlider
import com.lidarscan.app.ui.components.SheetSwitchRow
import com.lidarscan.app.ui.theme.DisplayFontFamily
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.MonoLabel
import com.lidarscan.app.ui.theme.SemGood
import com.lidarscan.app.ui.theme.SemWarn
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap

/**
 * Which of the Capture screen's two bottom sheets is up.
 *
 * A single enum rather than two booleans is round 4's "one sheet at a time"
 * rule expressed in the type: opening one *is* closing the other, there is no
 * state in which both are true, and there is exactly one dialog to own the back
 * gesture. The mockup had to enforce this with `closeCapSheets()` and two
 * `S.cap.*` flags; here it is structural.
 */
enum class CaptureSheet {
    NONE,

    /**
     * ROUND 8, owner item 28 — everything that is *about this capture* rather
     * than about how it is drawn: the name, the profile, the Light/Optimal/Full
     * chips, the connection (auto-detect + the manual fallback) and the D6
     * mount reference.
     *
     * All of it used to be stacked on the capture screen itself, above the
     * viewport, where it took roughly half the display. The rule this sheet
     * exists to enforce is [CaptureLayout.MIN_VIEWPORT_FRACTION]: the live 3D
     * view is the only thing on that screen that can tell an operator whether
     * the scan is any good, so it keeps 60 % of the height and the
     * configuration comes to the front only when it is asked for.
     */
    CAPTURE,

    /** Display: colour, colormap, point size, gamma, brightness, refresh, LOD. */
    SETTINGS,

    DIAGNOSTICS,
}

/** The 30 dp grabber both sheets share — a real dismiss target, not decoration. */
@Composable
private fun SheetGrabber() {
    Box(
        Modifier.fillMaxWidth().height(30.dp).padding(top = 10.dp),
        contentAlignment = Alignment.TopCenter,
    ) {
        Box(
            Modifier
                .width(44.dp)
                .height(5.dp)
                .background(MaterialTheme.colorScheme.outline, RoundedCornerShape(3.dp)),
        )
    }
}

@Composable
private fun SheetHead(title: String, hint: String) {
    Row(
        Modifier.fillMaxWidth().padding(start = 18.dp, end = 18.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        Text(
            title,
            fontFamily = DisplayFontFamily,
            fontWeight = FontWeight.SemiBold,
            fontSize = 19.sp,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Spacer(Modifier.weight(1f))
        Text(hint.uppercase(), style = MonoLabel, color = InkFaint)
    }
}

/**
 * **Capture settings** (mockup v6/v7): the single entry point for view, AR and
 * display, opened from the 48 dp button on the viewport.
 *
 * Height is capped at 74 % of the screen so a live band of the cloud stays
 * visible above it — that band is the whole point, because colour, point size
 * and LOD are verified against the live view while the sheet is still open.
 * The **View row is pinned** between the head and the scrolling body: once AR &
 * Camera moved in, the content stopped fitting, and round 3's documented
 * fallback is to pin View rather than shrink targets.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CaptureSettingsSheet(
    sheetState: SheetState,
    cameraMode: CameraMode,
    arAvailable: Boolean,
    arTrackingLabel: String,
    arTrackingIsGood: Boolean,
    keyframesEnabled: Boolean,
    keyframeRateFps: Int,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    lodBudgetMPoints: Int,
    // ── ROUND 5 additions ────────────────────────────────────────────────────
    /** Item 10: viewport refresh cap in fps, 0 = uncapped. */
    refreshHz: Int,
    /**
     * ROUND 5.3 (item 17): the refresh choices **this device** can actually reach —
     * `RefreshGovernor.optionsFor(Display.getRefreshRate())`. A 60 Hz phone never
     * sees a 120 fps option.
     */
    refreshOptions: List<Int>,
    /** Item 10: A14 scalar gamma / brightness, live against the preview. */
    gamma: Float,
    brightness: Float,
    onRefreshHzChange: (Int) -> Unit,
    onGammaChange: (Float) -> Unit,
    onBrightnessChange: (Float) -> Unit,
    onCameraModeChange: (CameraMode) -> Unit,
    onKeyframesEnabledChange: (Boolean) -> Unit,
    onKeyframeRateChange: (Int) -> Unit,
    onColorModeChange: (ColorMode) -> Unit,
    onColormapChange: (Colormap) -> Unit,
    onPointSizeChange: (Float) -> Unit,
    onLodChange: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val screenHeight = LocalConfiguration.current.screenHeightDp.dp

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        contentColor = MaterialTheme.colorScheme.onSurface,
        dragHandle = { SheetGrabber() },
        shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp),
        modifier = Modifier.testTag("captureSettingsSheet"),
    ) {
        Column(Modifier.height(screenHeight * 0.74f)) {
            SheetHead("Display", "A14 · live")

            // ── pinned: view mode ────────────────────────────────────────
            Column(Modifier.fillMaxWidth().padding(horizontal = 18.dp)) {
                SheetRowLabel(
                    label = "View",
                    hint = "how the cloud is framed",
                    readout = viewLabel(cameraMode),
                )
                // ── ROUND 7: THE AR OVERLAY IS ARCHIVED PER OWNER DIRECTIVE ──
                //
                // The overlay is gone from the UI; ARCore is not. Phone tracking
                // is the whole third dimension of a D6 capture, so
                // `ArPosePumpView` still runs `Session.update()` on every frame
                // and still pushes every pose — nothing about the trajectory
                // changed. What was removed is the *view* that painted the
                // camera image behind the points, which the owner asked to shelve
                // while the scan-quality work lands.
                //
                // [com.lidarscan.app.ar.ArOverlayView], [CameraMode.AR] and the
                // ROUND 6 crash fixes (`ArSessionGate`) all stay compiled and
                // tested — `MountCalibrationScreen` still uses the overlay, which
                // is why deleting it was never the right move. The only thing
                // that changed is that nothing on the Capture tab can select it.
                // Revive by putting `CameraMode.AR` back in this list.
                SegmentedPill(
                    options = listOf(
                        CameraMode.ORBIT to "3D orbit",
                        CameraMode.FOLLOW to "Follow",
                    ),
                    selected = if (cameraMode == CameraMode.FOLLOW) CameraMode.FOLLOW else CameraMode.ORBIT,
                    onSelect = onCameraModeChange,
                    height = ScanDims.SegmentTall,
                    modifier = Modifier.testTag("captureViewRow"),
                )
                Spacer(Modifier.height(10.dp))
            }
            HorizontalDivider(color = MaterialTheme.colorScheme.outline)

            // ── scrolling: AR & Camera, then Display ─────────────────────
            Column(
                Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 18.dp),
            ) {
                // ROUND 7 (owner directive): the phone + D6 IS the 3D lidar, not "an AR
                // app with a lidar". Camera keyframes are a COLORIZATION feature and
                // stay; the section is named for what it does.
                SheetSection("Tracking & camera")

                SheetSwitchRow(
                    title = "Camera keyframes",
                    subtitle = "for colorization · motion-gated",
                    checked = keyframesEnabled,
                    enabled = arAvailable,
                    onCheckedChange = onKeyframesEnabledChange,
                    modifier = Modifier.testTag("keyframesSwitch"),
                )

                SheetRowLabel(
                    label = "Keyframe rate",
                    hint = "2 – 5 fps",
                    readout = "$keyframeRateFps fps",
                )
                SegmentedPill(
                    options = listOf(2 to "2 fps", 3 to "3 fps", 5 to "5 fps"),
                    selected = keyframeRateFps,
                    onSelect = onKeyframeRateChange,
                    // Dimmed, not blank: the labels and the read-out stay
                    // legible with keyframes off, they just stop taking taps.
                    enabled = keyframesEnabled && arAvailable,
                    modifier = Modifier.testTag("keyframeRateRow"),
                )
                Spacer(Modifier.height(12.dp))

                SheetRowLabel(
                    label = "Scanner tracking",
                    hint = "phone-tracked · read-only",
                    readout = arTrackingLabel,
                    readoutColor = when {
                        !arAvailable -> InkFaint
                        arTrackingIsGood -> SemGood
                        else -> SemWarn
                    },
                )

                SheetSection("Display")

                SheetRowLabel(label = "Colour mode", readout = colorModeLabel(colorMode))
                SegmentedPill(
                    options = listOf(
                        ColorMode.HEIGHT to "Height",
                        ColorMode.RGB to "RGB",
                        ColorMode.INTENSITY to "Intensity",
                    ),
                    selected = colorMode,
                    onSelect = onColorModeChange,
                    modifier = Modifier.testTag("colourModeRow"),
                )
                Spacer(Modifier.height(12.dp))

                // The colormap picker only exists for the two scalar modes —
                // A14's own rule, and the reason RGB has no ramp to choose.
                if (colorMode == ColorMode.HEIGHT || colorMode == ColorMode.INTENSITY) {
                    SheetRowLabel(label = "Colormap", readout = colormapLabel(colormap))
                    SegmentedPill(
                        options = listOf(
                            Colormap.GRAYSCALE to "Grey",
                            Colormap.SPECTRUM to "Spectrum",
                            Colormap.THERMAL to "Thermal",
                        ),
                        selected = colormap,
                        onSelect = onColormapChange,
                    )
                    Spacer(Modifier.height(12.dp))
                }

                // ROUND 5 (owner addition 2): 0.1 – 3.0 px in 0.1 steps. The
                // snapping itself lives in the ViewModel/DisplayLimits so the
                // slider, the read-out and the manifest cannot disagree.
                SheetRowLabel(
                    label = "Point size",
                    hint = "0.1 – 3.0 px · 0.1 steps",
                    readout = "%.1f px".format(pointSizePx),
                )
                SheetSlider(
                    value = pointSizePx,
                    range = com.lidarscan.core.render.DisplayLimits.POINT_SIZE_MIN_PX..
                        com.lidarscan.core.render.DisplayLimits.POINT_SIZE_MAX_PX,
                    steps = com.lidarscan.core.render.DisplayLimits.POINT_SIZE_STEPS,
                    onValueChange = onPointSizeChange,
                    contentDescription = "Point size",
                    modifier = Modifier.testTag("pointSizeSlider"),
                )
                Spacer(Modifier.height(8.dp))

                // ROUND 5 (item 10): gamma + brightness. A14's rule is that the
                // scalar block only applies in HEIGHT/INTENSITY — in RGB the
                // shader reads the identity block — so in RGB these rows dim
                // rather than lying about having an effect.
                val toneActive = colorMode == ColorMode.HEIGHT || colorMode == ColorMode.INTENSITY
                SheetRowLabel(
                    label = "Gamma",
                    hint = if (toneActive) "0.1 – 4.0" else "height / intensity only",
                    readout = "%.2f".format(gamma),
                    readoutColor = if (toneActive) MaterialTheme.colorScheme.onSurface else InkFaint,
                )
                SheetSlider(
                    value = gamma,
                    range = com.lidarscan.core.render.DisplayLimits.GAMMA_MIN..
                        com.lidarscan.core.render.DisplayLimits.GAMMA_MAX,
                    onValueChange = onGammaChange,
                    contentDescription = "Gamma",
                    modifier = Modifier.alpha(if (toneActive) 1f else 0.5f).testTag("gammaSlider"),
                )
                Spacer(Modifier.height(8.dp))

                SheetRowLabel(
                    label = "Brightness",
                    hint = if (toneActive) "0.1 – 3.0" else "height / intensity only",
                    readout = "%.2f".format(brightness),
                    readoutColor = if (toneActive) MaterialTheme.colorScheme.onSurface else InkFaint,
                )
                SheetSlider(
                    value = brightness,
                    range = com.lidarscan.core.render.DisplayLimits.BRIGHTNESS_MIN..
                        com.lidarscan.core.render.DisplayLimits.BRIGHTNESS_MAX,
                    onValueChange = onBrightnessChange,
                    contentDescription = "Brightness",
                    modifier = Modifier.alpha(if (toneActive) 1f else 0.5f).testTag("brightnessSlider"),
                )
                Spacer(Modifier.height(12.dp))

                // ROUND 5 (item 10): the live refresh rate. A viewport throttle
                // and nothing else — the caption says so, because "refresh rate"
                // next to a record button otherwise reads as the sensor's.
                SheetRowLabel(
                    label = "Live refresh",
                    hint = "this phone's own range · recording unaffected",
                    readout = com.lidarscan.core.render.DisplayLimits.refreshLabel(refreshHz),
                )
                SegmentedPill(
                    options = refreshOptions.map {
                        it to com.lidarscan.core.render.DisplayLimits.refreshLabel(it)
                    },
                    selected = refreshHz,
                    onSelect = onRefreshHzChange,
                    modifier = Modifier.testTag("refreshRateRow"),
                )
                Spacer(Modifier.height(12.dp))

                SheetRowLabel(
                    label = "LOD budget",
                    hint = "sparse → every return",
                    readout = "$lodBudgetMPoints M",
                )
                SheetSlider(
                    value = lodBudgetMPoints.toFloat(),
                    range = 2f..20f,
                    steps = 17,
                    onValueChange = { onLodChange(it.toInt()) },
                    contentDescription = "LOD budget",
                )
                Spacer(Modifier.height(8.dp))

                // ROUND 8 (item 28): the Session block — Live 3D map, Live SLAM
                // and the workflow profile — moved to [CaptureConfigSheet].
                // Those three are properties of the CAPTURE (what is recorded,
                // and whether a map is built at all); everything left in here is
                // a property of the PICTURE. Splitting them is what lets the
                // capture screen carry two chips instead of half a screen of
                // controls — see CaptureLayout.
                Hint(
                    "Recording, connection, performance and the mount reference are in the Capture sheet — " +
                        "this one is only about how the cloud is drawn. Nothing here touches the recording.",
                    color = InkFaint,
                )
                Spacer(Modifier.height(14.dp))
            }

            SheetFooter(onDismiss)
        }
    }
}

/**
 * ROUND 8, owner item 28 — **the Capture sheet**: everything that configures
 * the scan itself, one tap behind the chip that names the current preset.
 *
 * This is where the capture screen's whole pre-capture stack went. Top to
 * bottom it is the same order it had on the screen, so an operator who knew the
 * old layout finds every control in the place they expect it, just one tap
 * further in:
 *
 *  * **Performance** — ROUND 6 item 22's Light / Optimal / Full, with its
 *    "what changed" line and its weak-phone caution;
 *  * **Scan** — the name field (blank = auto-name) and the workflow profile;
 *  * **Connection** — the auto-detect status line and the inline manual
 *    fallback, passed in as a slot so they stay the *same* composables the
 *    disconnected screen shows inline (there is exactly one implementation of
 *    "enter the device by hand", not two that can drift);
 *  * **Mount reference** — the D6 re-zero's explanation and its Clear. The
 *    *state* and the Set button stay on the capture screen itself, always
 *    visible, because ROUND 8 item 30c is precisely that they must not be
 *    behind a sheet;
 *  * **Recording** — Live 3D map and Live SLAM, which decide what the engine
 *    builds rather than how it is painted.
 *
 * 74 % tall, matching the Display sheet: a live band of the cloud stays on
 * screen, which is what makes a re-zero taken with this sheet open verifiable.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CaptureConfigSheet(
    sheetState: SheetState,
    preset: com.lidarscan.core.capture.PerformancePreset,
    deviceTierLabel: String,
    presetChangeNote: String?,
    presetCaution: String?,
    onPresetChange: (com.lidarscan.core.capture.PerformancePreset) -> Unit,
    onDismissPresetNote: () -> Unit,
    autoName: String?,
    scanName: String,
    onScanNameChange: (String) -> Unit,
    profile: com.lidarscan.core.model.WorkflowProfile?,
    onProfileChange: (com.lidarscan.core.model.WorkflowProfile) -> Unit,
    liveMapEnabled: Boolean,
    onLiveMapEnabledChange: (Boolean) -> Unit,
    liveSlam: Boolean,
    liveSlamEditable: Boolean,
    onLiveSlamChange: (Boolean) -> Unit,
    /** The auto-detect line + the inline manual panel, exactly as the disconnected screen draws them. */
    connection: @Composable () -> Unit,
    /** The D6 mount-reference explanation and Clear, or nothing for a sensor that has no trim. */
    mount: (@Composable () -> Unit)?,
    onDismiss: () -> Unit,
) {
    val screenHeight = LocalConfiguration.current.screenHeightDp.dp

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        contentColor = MaterialTheme.colorScheme.onSurface,
        dragHandle = { SheetGrabber() },
        shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp),
        modifier = Modifier.testTag("captureConfigSheet"),
    ) {
        Column(Modifier.height(screenHeight * 0.74f)) {
            SheetHead("Capture", "this scan")

            Column(
                Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 18.dp),
            ) {
                SheetSection("Performance")
                SheetRowLabel(
                    label = "Preset",
                    hint = "$deviceTierLabel phone",
                    readout = if (preset.isSelectable) preset.tagline else "custom",
                )
                SegmentedPill(
                    options = com.lidarscan.core.capture.PerformancePreset.entries
                        .filter { it.isSelectable }
                        .map { it to it.displayName },
                    selected = preset,
                    onSelect = onPresetChange,
                    height = ScanDims.SegmentTall,
                    modifier = Modifier.testTag("presetRow"),
                )
                if (presetChangeNote != null) {
                    Spacer(Modifier.height(6.dp))
                    // ROUND 6 (item 22): "switching preset shows what it
                    // changed" — and stays until read, or tapped away.
                    Hint(
                        presetChangeNote,
                        color = SemGood,
                        modifier = Modifier
                            .clickable(onClick = onDismissPresetNote)
                            .testTag("presetChangeNote"),
                    )
                }
                if (presetCaution != null) {
                    Spacer(Modifier.height(6.dp))
                    Hint(presetCaution, color = SemWarn, modifier = Modifier.testTag("presetCaution"))
                }

                SheetSection("Scan")
                if (autoName != null) {
                    OutlinedTextField(
                        value = scanName,
                        onValueChange = onScanNameChange,
                        singleLine = true,
                        label = { Text("Scan name — optional") },
                        placeholder = { Text(autoName, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                        modifier = Modifier.fillMaxWidth().testTag("scanNameField"),
                    )
                    Spacer(Modifier.height(6.dp))
                }
                if (profile != null) {
                    SheetRowLabel(
                        label = "Workflow profile",
                        hint = "stamped on the project Start creates",
                        readout = profile.displayName,
                    )
                    SegmentedPill(
                        options = com.lidarscan.core.model.WorkflowProfile.entries.map { it to it.displayName },
                        selected = profile,
                        onSelect = onProfileChange,
                        modifier = Modifier.testTag("profileRow"),
                    )
                }

                SheetSection("Connection")
                connection()

                if (mount != null) {
                    SheetSection("Mount reference")
                    mount()
                }

                SheetSection("Recording")
                // ROUND 6 (item 22): Light's "raw preview + record, no live
                // map" as a plain switch. Above Live SLAM because it is the
                // stronger statement — with this off, nothing draws a map at
                // all, whichever engine would have produced one.
                SheetSwitchRow(
                    title = "Live 3D map",
                    subtitle = "draw the registered / pushbroom cloud · recording unaffected",
                    checked = liveMapEnabled,
                    onCheckedChange = onLiveMapEnabledChange,
                    modifier = Modifier.testTag("liveMapSwitch"),
                )
                SheetSwitchRow(
                    title = "Live SLAM",
                    subtitle = if (liveSlamEditable) {
                        "registered map while recording · editable while idle · Mid-360"
                    } else {
                        "locked during a session"
                    },
                    checked = liveSlam,
                    enabled = liveSlamEditable,
                    onCheckedChange = onLiveSlamChange,
                    modifier = Modifier.testTag("liveSlamSwitch"),
                )
                Spacer(Modifier.height(14.dp))
            }

            SheetFooter(onDismiss)
        }
    }
}

/**
 * **Diagnostics** (mockup v7): everything the capture body used to carry, one
 * tap behind the viewport's health chip.
 *
 * 60 % tall rather than the settings sheet's 74 %: nothing in here is a target,
 * read-only rows need no thumb room, and the extra live viewport it leaves on
 * screen is the thing being diagnosed. Every value is painted from the same
 * state the screen behind it reads, so the two can never disagree — a frozen
 * diagnostic is a lie.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DiagnosticsSheet(
    sheetState: SheetState,
    device: DeviceDiagnostics,
    ar: ArDiagnostics,
    onDismiss: () -> Unit,
) {
    val screenHeight = LocalConfiguration.current.screenHeightDp.dp

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        contentColor = MaterialTheme.colorScheme.onSurface,
        dragHandle = { SheetGrabber() },
        shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp),
        modifier = Modifier.testTag("diagnosticsSheet"),
    ) {
        Column(Modifier.height(screenHeight * 0.60f)) {
            SheetHead("Diagnostics", "read-only · live")

            Column(
                Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 18.dp),
            ) {
                SheetSection("Device")
                DiagRow("State", device.state, valueColor = device.stateColor)
                DiagRow("Points / sec", device.pointsPerSecond, testTag = "diagPointsPerSecond")
                DiagRow("Rotation", device.rotation)
                DiagRow("IMU", device.imu)
                DiagRow("Checksum pass rate", device.checksum, valueColor = device.checksumColor)
                DiagRow("Packet loss", device.packetLoss)

                SheetSection("Tracking + camera")
                DiagRow("Scanner tracking", ar.tracking, valueColor = ar.trackingColor)
                DiagRow("Keyframes written", ar.keyframes, testTag = "diagKeyframes")
                DiagRow("Tracking-loss episodes", ar.trackingLossEpisodes)
                DiagRow("Skipped (turning too fast)", ar.skippedTurning)
                DiagRow("Rolling shutter", ar.rollingShutter, valueColor = MaterialTheme.colorScheme.onSurfaceVariant)

                SheetSection("3D + georeference")
                DiagRow("Poses pushed to engine", ar.posesPushed, testTag = "diagPosesPushed")
                DiagRow("Mount extrinsic", ar.mountExtrinsic, valueColor = ar.mountExtrinsicColor)
                DiagRow("Georeference source", ar.georefSource, testTag = "diagGeorefSource")

                Spacer(Modifier.height(10.dp))
                Hint(
                    "Read-only. The capture settings that drive these — camera keyframes and their rate — " +
                        "live in the Capture-settings sheet.",
                    color = InkFaint,
                )
                Spacer(Modifier.height(14.dp))
            }

            SheetFooter(onDismiss)
        }
    }
}

@Composable
private fun SheetFooter(onDone: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 18.dp)) {
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Spacer(Modifier.height(10.dp))
        PrimaryPill(
            text = "Done",
            onClick = onDone,
            height = 55.dp,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(10.dp))
        Spacer(Modifier.navigationBarsPadding())
    }
}

/** Everything the Device section of Diagnostics prints, already formatted. */
data class DeviceDiagnostics(
    val state: String,
    val stateColor: Color,
    val pointsPerSecond: String,
    val rotation: String,
    val imu: String,
    val checksum: String,
    val checksumColor: Color,
    val packetLoss: String,
)

/** Everything the AR + camera section of Diagnostics prints, already formatted. */
data class ArDiagnostics(
    val tracking: String,
    val trackingColor: Color,
    val keyframes: String,
    val trackingLossEpisodes: String,
    val skippedTurning: String,
    val rollingShutter: String,
    /**
     * ROUND 5: the two numbers that say whether a phone-tracked D6 capture is
     * actually producing 3D — poses pushed into the engine, and whether the
     * extrinsic behind the pushbroom was measured or assumed — plus round 5.2's
     * georeference source. Read-only rows, per round 3: the *hint* is inline on
     * the capture screen, the numbers live here.
     */
    val posesPushed: String,
    val mountExtrinsic: String,
    val mountExtrinsicColor: Color,
    val georefSource: String,
)

private fun viewLabel(mode: CameraMode) = when (mode) {
    // ROUND 7: archived, not deleted — see the View row above. Unreachable from
    // the product; the label exists so the `when` stays exhaustive.
    CameraMode.AR -> "archived"
    CameraMode.FOLLOW -> "Follow"
    CameraMode.ORBIT -> "3D orbit"
}

private fun colorModeLabel(mode: ColorMode) = when (mode) {
    ColorMode.HEIGHT -> "Height"
    ColorMode.RGB -> "RGB"
    ColorMode.INTENSITY -> "Intensity"
    ColorMode.TIME -> "Time"
    ColorMode.FIX_QUALITY -> "Fix"
}

private fun colormapLabel(cm: Colormap) = when (cm) {
    Colormap.GRAYSCALE -> "Grey"
    Colormap.SPECTRUM -> "Spectrum"
    Colormap.THERMAL -> "Thermal"
}
