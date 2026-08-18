package com.lidarscan.app.ui.calib

import android.app.Activity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.RadioButtonUnchecked
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.ar.ArAvailability
import com.lidarscan.app.ar.ArOverlayView
import com.lidarscan.app.ar.message
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.calib.CalibrationGate
import com.lidarscan.core.calib.DiversityWheel
import com.lidarscan.core.calib.LidarProfile
import com.lidarscan.core.calib.PoseCheck
import com.lidarscan.core.calib.formatDegrees
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap

@Composable
fun MountCalibrationRoute(
    container: AppContainer,
    projectId: String,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    val viewModel: MountCalibrationViewModel = viewModel(
        key = "mount-calib-$projectId",
        factory = viewModelFactory {
            initializer {
                MountCalibrationViewModel(
                    arController = container.arController,
                    engineBridge = container.engineBridge,
                    projectStore = container.projectStore,
                    calibrationStore = container.mountCalibrationStore,
                    projectId = projectId,
                    appVersion = com.lidarscan.app.BuildConfig.VERSION_NAME,
                    engineHandleProvider = container::currentEngineHandle,
                    pointCloudSourceProvider = container::currentPointCloudSource,
                )
            }
        },
    )
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val arStatus by container.arController.status.collectAsStateWithLifecycle()

    // Camera permission: ARCore needs CAMERA and will not create a session
    // without it. Requested here rather than at app start because this (and
    // the Capture screen's AR mode) are the only places that need it, and a
    // permission asked for in context is one a user can actually answer.
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        if (granted) container.arController.createSession()
    }

    LaunchedEffect(Unit) {
        container.arController.refreshAvailability()
        val activity = context as? Activity
        when {
            !container.hasCameraPermission() -> permissionLauncher.launch(android.Manifest.permission.CAMERA)
            container.arController.status.value.availability == ArAvailability.NEEDS_INSTALL && activity != null ->
                container.requestArInstall(activity)
            else -> {
                container.arController.createSession()
                container.arController.resume()
            }
        }
    }

    DisposableEffect(Unit) {
        onDispose { container.arController.pause() }
    }

    MountCalibrationScreen(
        state = state,
        arMessage = arStatus.trackingHint ?: arStatus.message,
        onBack = onBack,
        onSquareSizeChange = viewModel::setSquareSize,
        onBracketIdChange = viewModel::setBracketId,
        onStep = viewModel::goTo,
        onSolve = viewModel::solve,
        onAccept = viewModel::accept,
        onRetry = viewModel::restartCapture,
        onDropWorst = { viewModel.dropWorstPosesAndRetry() },
        onVerified = viewModel::confirmVerification,
        arOverlay = { modifier ->
            ArOverlayView(
                controller = container.arController,
                source = container.currentPointCloudSource(),
                colorMode = ColorMode.INTENSITY,
                colormap = Colormap.SPECTRUM,
                pointSizePx = 4f,
                modifier = modifier,
            )
        },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MountCalibrationScreen(
    state: WizardUiState,
    arMessage: String?,
    onBack: () -> Unit,
    onSquareSizeChange: (Double) -> Unit,
    onBracketIdChange: (String) -> Unit,
    onStep: (WizardStep) -> Unit,
    onSolve: () -> Unit,
    onAccept: () -> Unit,
    onRetry: () -> Unit,
    onDropWorst: () -> Unit,
    onVerified: (Boolean) -> Unit,
    arOverlay: @Composable (Modifier) -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Mount calibration") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            when (state.step) {
                WizardStep.PREPARE -> PrepareStep(state, onSquareSizeChange, onBracketIdChange) { onStep(WizardStep.CAPTURE) }
                WizardStep.CAPTURE -> CaptureStep(state, arMessage, arOverlay, onSolve) { onStep(WizardStep.CLOCK_SWEEP) }
                WizardStep.CLOCK_SWEEP -> ClockSweepStep(onSolve) { onStep(WizardStep.CAPTURE) }
                WizardStep.VERDICT -> VerdictStep(state, onAccept, onRetry, onDropWorst)
                WizardStep.VERIFY -> VerifyStep(state, arOverlay, onVerified)
            }
            state.message?.let { message ->
                Card(
                    modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth().padding(16.dp),
                ) {
                    Text(message, Modifier.padding(16.dp), style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
    }
}

// --- screen 1 ---------------------------------------------------------------

@Composable
private fun PrepareStep(
    state: WizardUiState,
    onSquareSizeChange: (Double) -> Unit,
    onBracketIdChange: (String) -> Unit,
    onStart: () -> Unit,
) {
    var squareSizeText by remember { mutableStateOf("%.0f".format(state.spec.squareSizeM * 1000)) }
    var bracketText by remember { mutableStateOf(com.lidarscan.core.calib.BracketNominals.DEFAULT_BRACKET_ID) }
    val ready = state.arAvailability.canRunAr && state.engineConnected

    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
        Text("Print the target", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text(
                    "A1 checkerboard, ${state.spec.cols} x ${state.spec.rows} inner corners " +
                        "(${state.spec.cols + 1} x ${state.spec.rows + 1} squares), " +
                        "%.0f mm squares.".format(state.spec.squareSizeM * 1000),
                    style = MaterialTheme.typography.bodyMedium,
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    // ROUND 13: `.format()` bound to the SECOND fragment (no
                    // placeholders), so this read "Pattern size: %.2f x %.2f m"
                    // on screen. One string, one format call.
                    (
                        "Pattern size: %.2f x %.2f m. Print at 100%% scale on rigid backing " +
                            "(foam board bows by several millimetres — aluminium composite is " +
                            "better)."
                    ).format(state.spec.patternWidthM, state.spec.patternHeightM),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(12.dp))
                OutlinedTextField(
                    value = squareSizeText,
                    onValueChange = {
                        squareSizeText = it
                        it.toDoubleOrNull()?.let { mm -> onSquareSizeChange(mm / 1000.0) }
                    },
                    label = { Text("Measured square size (mm)") },
                    supportingText = { Text("Measure a printed square — printers silently rescale PDFs.") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                Spacer(Modifier.height(12.dp))
                OutlinedTextField(
                    value = bracketText,
                    onValueChange = {
                        bracketText = it
                        onBracketIdChange(it)
                    },
                    label = { Text("Bracket ID") },
                    supportingText = { Text("The calibration is stored against this bracket, not this project.") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }

        Spacer(Modifier.height(16.dp))
        Text("Three rules", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        RuleRow("1", "Mount the board on a wall or stand, at least 0.5 m clear of anything behind it — the lidar has to separate it from the background.")
        RuleRow("2", "Stand back 1.2–2 m. Matte, evenly lit, no glare.")
        RuleRow("3", "Do not move the board once you start.")

        Spacer(Modifier.height(16.dp))
        Text("Before you start", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        PreconditionRow("Phone tracking", state.arAvailability.canRunAr, state.arAvailability.message())
        PreconditionRow(
            "Lidar streaming",
            state.engineConnected,
            "Connect the ${state.sensor.displayName} first — the wizard needs live returns on the board.",
        )
        PreconditionRow(
            "Native engine",
            ScanEngineNative.isAvailable,
            "The simulated-engine build cannot solve a calibration.",
        )

        if (state.profile == LidarProfile.D6) {
            Spacer(Modifier.height(16.dp))
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    Text("This wizard is a check, not a calibration, for the D6", style = MaterialTheme.typography.titleSmall)
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "A 2-D scanner returns a line across the board rather than a patch, which is 2 constraints " +
                            "per pose instead of 3. Handheld, the best it reaches is about 26 px of reprojection " +
                            "error at 3 m against a 20 px budget. Use it to verify and slightly correct a bench " +
                            "calibration (45 tripod poses, run once per bracket design) — not to replace one.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        Spacer(Modifier.height(24.dp))
        Button(onClick = onStart, enabled = ready, modifier = Modifier.fillMaxWidth()) {
            Text("Start — ${state.poseTarget} poses")
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun RuleRow(number: String, text: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        Text(number, style = MaterialTheme.typography.titleSmall, modifier = Modifier.width(24.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun PreconditionRow(label: String, ok: Boolean, fixIt: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.Top) {
        Icon(
            if (ok) Icons.Filled.CheckCircle else Icons.Filled.RadioButtonUnchecked,
            contentDescription = null,
            tint = if (ok) Color(0xFF2E7D32) else MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(20.dp),
        )
        Spacer(Modifier.width(12.dp))
        Column {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            if (!ok) {
                Text(fixIt, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

// --- screen 2 ---------------------------------------------------------------

@Composable
private fun CaptureStep(
    state: WizardUiState,
    arMessage: String?,
    arOverlay: @Composable (Modifier) -> Unit,
    onSolve: () -> Unit,
    onClockSweep: () -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxWidth().weight(1f)) {
            arOverlay(Modifier.fillMaxSize())
            // The live target ghost: the prescribed roll, drawn as a hint over
            // the preview. A full perspective outline of where the board
            // should sit would need the board's intended pose in the CURRENT
            // camera frame, which is only known once the board has been seen —
            // so what is shown is the part that is actually known and actually
            // actionable: the roll to match.
            state.currentPose?.let { pose ->
                Card(
                    modifier = Modifier.align(Alignment.TopCenter).padding(12.dp),
                ) {
                    Column(Modifier.padding(12.dp)) {
                        Text(
                            "Pose ${state.poseCount + 1} of ${state.poseTarget}",
                            style = MaterialTheme.typography.titleSmall,
                        )
                        Text(
                            "Tilt to ${formatDegrees(pose.rollDeg)} · stand ${formatDegrees(pose.azimuthDeg)} to the side",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                }
            }
            if (state.ringProgress > 0f) {
                CircularProgressIndicator(
                    progress = { state.ringProgress },
                    modifier = Modifier.align(Alignment.Center).size(96.dp),
                )
            }
        }

        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
            arMessage?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                Spacer(Modifier.height(8.dp))
            }
            PoseCheck.entries.forEach { check ->
                CheckChip(check, check in state.checks.passing, state)
            }
            Spacer(Modifier.height(12.dp))
            DiversityWheelRow(state)
            Spacer(Modifier.height(12.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = onSolve, enabled = state.canSolve) {
                    Text("Solve (${state.poseCount})")
                }
                OutlinedButton(onClick = onClockSweep) { Text("Clock sweep") }
            }
        }
    }
}

@Composable
private fun CheckChip(check: PoseCheck, passing: Boolean, state: WizardUiState) {
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp), verticalAlignment = Alignment.Top) {
        Icon(
            if (passing) Icons.Filled.CheckCircle else Icons.Filled.RadioButtonUnchecked,
            contentDescription = null,
            tint = if (passing) Color(0xFF2E7D32) else MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(18.dp),
        )
        Spacer(Modifier.width(10.dp))
        Column {
            Text(check.label, style = MaterialTheme.typography.bodyMedium)
            if (!passing) {
                Text(
                    check.failingCopy,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else if (check == PoseCheck.LIDAR_SEES_IT) {
                Text(
                    "${state.lidarReturnsOnBoard} returns on the board",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/**
 * WIZARD.md screen 2's progress indicator: three coverage bars, not a
 * counter. "This makes 'you have 8 samples but they are all the same view'
 * visible, which a bare '6 / 8' cannot."
 */
@Composable
private fun DiversityWheelRow(state: WizardUiState) {
    val (azimuth, elevation, roll) = state.diversity
    Column(Modifier.fillMaxWidth()) {
        Text("Pose diversity", style = MaterialTheme.typography.titleSmall)
        Spacer(Modifier.height(6.dp))
        DiversityBar("Side to side", azimuth)
        DiversityBar("Up and down", elevation)
        DiversityBar("Tilt (roll)", roll)
        if (state.poseCount >= 3 && state.weakestAxis == DiversityWheel.Axis.ROLL && roll < 0.5) {
            Text(
                "Tilt the phone more between shots — poses that vary only in position roughly double the error.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }
    }
}

@Composable
private fun DiversityBar(label: String, value: Double) {
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = MaterialTheme.typography.bodySmall, modifier = Modifier.width(110.dp))
        LinearProgressIndicator(
            progress = { value.toFloat() },
            modifier = Modifier.weight(1f).height(8.dp).background(Color.Transparent, RoundedCornerShape(4.dp)),
        )
    }
}

// --- screen 3 ---------------------------------------------------------------

@Composable
private fun ClockSweepStep(onSolve: () -> Unit, onBack: () -> Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
        Text("Clock sweep", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Text(
            "Sweep the phone smoothly left and right across the board, about one sweep per second, for 8 seconds.",
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(16.dp))
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("Not available in this build", style = MaterialTheme.typography.titleSmall)
                Spacer(Modifier.height(6.dp))
                Text(
                    "This step recovers the constant camera<->lidar clock offset — S6 calls it \"the " +
                        "highest-value 8 seconds in the wizard\", because time-sync error is the largest term in " +
                        "the colorization budget and a constant offset is the cheap part of it to remove.\n\n" +
                        "The estimator itself is implemented in the engine (color/clock_sweep.h), but it has no " +
                        "C-ABI entry point yet: A11 §8.2 lists scan_clock_sweep_estimate() as a seam still to be " +
                        "added, and engine/ is not this task's to change. Rather than record a zero offset as if " +
                        "it had been measured, the saved calibration leaves the clock offset unset.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.height(24.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(onClick = onSolve) { Text("Solve without it") }
            OutlinedButton(onClick = onBack) { Text("Back to capture") }
        }
    }
}

// --- screen 4 ---------------------------------------------------------------

@Composable
private fun VerdictStep(
    state: WizardUiState,
    onAccept: () -> Unit,
    onRetry: () -> Unit,
    onDropWorst: () -> Unit,
) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
        if (state.solving) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                CircularProgressIndicator(Modifier.size(24.dp))
                Spacer(Modifier.width(12.dp))
                Text("Solving…")
            }
            return@Column
        }
        val readout = state.readout
        val result = state.result
        if (readout == null || result == null) {
            Text("No result yet.", style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(16.dp))
            OutlinedButton(onClick = onRetry) { Text("Back to capture") }
            return@Column
        }

        Text(readout.headline, style = MaterialTheme.typography.headlineSmall)
        Spacer(Modifier.height(8.dp))
        Text(readout.detail, style = MaterialTheme.typography.bodyMedium)

        state.diagnosis?.let {
            Spacer(Modifier.height(12.dp))
            Card(Modifier.fillMaxWidth()) {
                Text(it, Modifier.padding(16.dp), style = MaterialTheme.typography.bodyMedium)
            }
        }

        Spacer(Modifier.height(20.dp))
        Text("Details", style = MaterialTheme.typography.titleSmall)
        Spacer(Modifier.height(6.dp))
        DetailRow("Poses used", "${result.observations}")
        DetailRow("Lidar residuals", "${result.residuals}")
        DetailRow("Residual RMS", "%.1f mm".format(result.rmsResidualM * 1000))
        DetailRow("Split-half gate", "%.1f px at %.0f m".format(result.splitHalfPx, result.gateRangeM))
        DetailRow("Converged", if (result.converged) "yes" else "no")
        if (result.degenerate) DetailRow("Degenerate", "yes — too few or too similar poses")

        Spacer(Modifier.height(12.dp))
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("Diagnostics — not the gate", style = MaterialTheme.typography.titleSmall)
                Spacer(Modifier.height(4.dp))
                Text(
                    "σ_rot %.3f° · σ_trans %.1f mm · condition %.3g".format(
                        result.sigmaRotDeg, result.sigmaTransMm, result.conditionNumber,
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    "The solver's own covariance is near-constant from session to session and cannot rank a good " +
                        "capture against a bad one, so it is shown for the bench and never gated on.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        Spacer(Modifier.height(24.dp))
        val gate = CalibrationGate.fromEngine(result.gate)
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            if (gate != CalibrationGate.REJECT) {
                Button(onClick = onAccept) { Text("Save and verify") }
            }
            OutlinedButton(onClick = onRetry) { Text("Redo the capture") }
            if (gate == CalibrationGate.REJECT && state.poseCount > WizardUiState.MIN_OBSERVATIONS) {
                OutlinedButton(onClick = onDropWorst) { Text("Drop the worst 2") }
            }
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun DetailRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodySmall)
    }
}

// --- screen 5 ---------------------------------------------------------------

@Composable
private fun VerifyStep(
    state: WizardUiState,
    arOverlay: @Composable (Modifier) -> Unit,
    onVerified: (Boolean) -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxWidth().weight(1f)) {
            arOverlay(Modifier.fillMaxSize())
        }
        Column(Modifier.fillMaxWidth().padding(16.dp)) {
            Text("Does it line up?", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(6.dp))
            Text(
                "Point the rig at a doorframe or a table edge. The lidar points should sit on the edge you can " +
                    "see. This catches what no residual can: the wrong bracket slot, the phone seated backwards, " +
                    "the wrong lidar selected.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            state.saved?.let {
                Spacer(Modifier.height(8.dp))
                Text(
                    "Saved for bracket ${it.bracketId} · ${it.readout().headline}",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Spacer(Modifier.height(16.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = { onVerified(true) }) { Text("Looks aligned") }
                OutlinedButton(onClick = { onVerified(false) }) { Text("Looks off") }
            }
        }
    }
}
