package com.lidarscan.app.ui.capture

import androidx.compose.foundation.background
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
import androidx.compose.material3.SheetState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
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
enum class CaptureSheet { NONE, SETTINGS, DIAGNOSTICS }

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
            SheetHead("Capture settings", "A14 · live")

            // ── pinned: view mode ────────────────────────────────────────
            Column(Modifier.fillMaxWidth().padding(horizontal = 18.dp)) {
                SheetRowLabel(
                    label = "View",
                    hint = "how the cloud is framed",
                    readout = viewLabel(cameraMode),
                )
                SegmentedPill(
                    options = listOf(
                        CameraMode.ORBIT to "3D orbit",
                        CameraMode.AR to "AR overlay",
                    ),
                    selected = if (cameraMode == CameraMode.AR) CameraMode.AR else CameraMode.ORBIT,
                    onSelect = { mode ->
                        // A dead control is worse than an absent one, and the
                        // sheet has no room to hide a half of a two-half row —
                        // so AR stays visible and simply refuses when there is
                        // no ARCore session behind it. The read-out below says
                        // why (`off`).
                        if (mode != CameraMode.AR || arAvailable) onCameraModeChange(mode)
                    },
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
                SheetSection("AR & Camera")

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
                    label = "AR tracking",
                    hint = "ARCore · read-only",
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

                SheetRowLabel(
                    label = "Point size",
                    hint = "0.5 – 12 px",
                    readout = "%.1f px".format(pointSizePx),
                )
                SheetSlider(
                    value = pointSizePx,
                    range = 0.5f..12f,
                    onValueChange = onPointSizeChange,
                    contentDescription = "Point size",
                )
                Spacer(Modifier.height(8.dp))

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

                SheetSection("AR + camera")
                DiagRow("AR tracking", ar.tracking, valueColor = ar.trackingColor)
                DiagRow("Keyframes written", ar.keyframes, testTag = "diagKeyframes")
                DiagRow("Tracking-loss episodes", ar.trackingLossEpisodes)
                DiagRow("Skipped (turning too fast)", ar.skippedTurning)
                DiagRow("Rolling shutter", ar.rollingShutter, valueColor = MaterialTheme.colorScheme.onSurfaceVariant)

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
)

private fun viewLabel(mode: CameraMode) = when (mode) {
    CameraMode.AR -> "AR overlay"
    else -> "3D orbit"
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
