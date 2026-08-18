package com.lidarscan.app.ui.plan

import android.content.Context
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.testTag
import com.lidarscan.core.capture.FloorPlanResult
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.core.plan.OpeningKind
import com.lidarscan.core.plan.PlanModel
import com.lidarscan.core.plan.WallEvidence
import kotlin.math.min

@Composable
fun PlanRoute(container: AppContainer, projectId: String, onBack: () -> Unit) {
    val vm: PlanViewModel = viewModel(
        factory = viewModelFactory {
            initializer { PlanViewModel(container, container.projectStore, projectId) }
        },
    )
    val state by vm.uiState.collectAsStateWithLifecycle()
    PlanScreen(state, vm, onBack)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlanScreen(state: PlanUiState, vm: PlanViewModel, onBack: () -> Unit) {
    val snackbar = remember { SnackbarHostState() }
    val context = LocalContext.current
    var showOptions by remember { mutableStateOf(true) }

    LaunchedEffect(state.message) {
        state.message?.let {
            snackbar.showSnackbar(it)
            vm.dismissMessage()
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Floor plan") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            Box(Modifier.weight(1f).fillMaxWidth()) {
                val plan = state.plan
                val rendered = state.rendered
                when {
                    state.rendering -> Column(
                        Modifier.fillMaxSize().padding(32.dp),
                        verticalArrangement = Arrangement.Center,
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text("Drawing the plan…", style = MaterialTheme.typography.titleMedium)
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth().padding(top = 12.dp))
                    }
                    state.running -> Column(
                        Modifier.fillMaxSize().padding(32.dp),
                        verticalArrangement = Arrangement.Center,
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text("Extracting…", style = MaterialTheme.typography.titleMedium)
                        LinearProgressIndicator(progress = { state.progress }, modifier = Modifier.fillMaxWidth().padding(top = 12.dp))
                        OutlinedButton(onClick = vm::cancel, modifier = Modifier.padding(top = 12.dp)) { Text("Cancel") }
                    }
                    // ROUND 15 item 56: the RENDERED plan wins the viewport
                    // when there is one. It is the artifact that leaves the
                    // phone, so it is the one the operator judges — and it is
                    // drawn from the sealed container by path, which the
                    // in-memory PlanCanvas path below is not.
                    rendered != null && rendered.hasImage -> RenderedPlanView(rendered)
                    plan == null -> CenteredText(
                        "No plan yet.\n\nTap Floor plan below: the plan is cut from this scan's own " +
                            "container, so it works on a sealed scan whether or not you have processed it.",
                    )
                    plan.isEmpty -> CenteredText(plan.emptyDiagnosis())
                    else -> PlanCanvas(plan)
                }
            }

            HorizontalDivider()
            // ROUND 15 item 56 — the row that does the thing this screen is
            // named after. Deliberately ABOVE the options fold: eleven rounds
            // of "Floor plan" led to a screen whose only actions were behind
            // an Options button and whose empty state told the operator to go
            // and run something else first.
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Button(
                    onClick = vm::render,
                    enabled = !state.rendering && !state.running,
                    modifier = Modifier.testTag("planRenderButton"),
                ) { Text(if (state.rendered == null) "Floor plan" else "Redraw") }
                val r = state.rendered
                OutlinedButton(
                    onClick = { vm.shareRendered(context, PlanViewModel.RenderedKind.PNG) },
                    enabled = r != null && r.pngPath.isNotEmpty(),
                    modifier = Modifier.testTag("planSharePng"),
                ) { Text("PNG") }
                OutlinedButton(
                    onClick = { vm.shareRendered(context, PlanViewModel.RenderedKind.PDF) },
                    enabled = r != null && r.pdfPath.isNotEmpty(),
                    modifier = Modifier.testTag("planSharePdf"),
                ) { Text("PDF") }
                OutlinedButton(
                    onClick = { vm.shareRendered(context, PlanViewModel.RenderedKind.DXF) },
                    enabled = r != null && r.dxfPath.isNotEmpty(),
                    modifier = Modifier.testTag("planShareDxf"),
                ) { Text("DXF") }
            }
            state.rendered?.let { r ->
                Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
                    Text(
                        r.headline,
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.testTag("planHeadline"),
                    )
                    r.detail?.let {
                        Text(
                            it,
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.testTag("planDetail"),
                        )
                    }
                }
            }
            HorizontalDivider()
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    state.rendered?.let {
                        "${it.walls} walls · ${it.openings} openings · ${it.rooms} rooms · " +
                            "%.1f m²".format(it.roomAreaM2)
                    } ?: state.plan?.let {
                        "${it.walls.size} walls · ${it.openings.size} openings · ${it.rooms.size} rooms · " +
                            "%.1f m²".format(it.stats.totalRoomAreaM2)
                    } ?: "—",
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedButton(onClick = { showOptions = !showOptions }) {
                    Text(if (showOptions) "Hide options" else "Options")
                }
            }

            if (showOptions) {
                Column(
                    Modifier
                        .fillMaxWidth()
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    val o = state.options
                    Text("Slice band", style = MaterialTheme.typography.titleSmall)
                    Text(
                        "The horizontal band the plan is cut from — §3.6's default is 1.0–1.5 m, which clears furniture " +
                            "and catches door frames.",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    SliderRow("Floor", o.zMinM, -2f..8f, "%.2f m".format(o.zMinM)) { vm.setOptions(o.copy(zMinM = it)) }
                    SliderRow("Ceiling", o.zMaxM, -2f..8f, "%.2f m".format(o.zMaxM)) { vm.setOptions(o.copy(zMaxM = it)) }
                    SliderRow("Grid", o.gridResM, 0.005f..0.10f, "%.0f mm".format(o.gridResM * 1000)) {
                        vm.setOptions(o.copy(gridResM = it))
                    }
                    SliderRow(
                        "Min points per cell",
                        o.minCellPoints.toFloat(),
                        1f..10f,
                        "${o.minCellPoints}",
                    ) { vm.setOptions(o.copy(minCellPoints = it.toInt())) }
                    Text(
                        "Three is A12's default: fewer widens every wall face to about five cells, which is wider than " +
                            "the partition the pipeline is trying to split into two faces. Lower it for a sparse cloud.",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    ToggleRow("Snap to orthogonal", o.snapOrthogonal) { vm.setOptions(o.copy(snapOrthogonal = it)) }
                    ToggleRow("Detect openings", o.detectOpenings) { vm.setOptions(o.copy(detectOpenings = it)) }
                    ToggleRow("Window sill check", o.windowSillCheck) { vm.setOptions(o.copy(windowSillCheck = it)) }
                    ToggleRow("Detect rooms", o.detectRooms) { vm.setOptions(o.copy(detectRooms = it)) }

                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = vm::extract, enabled = !state.running) { Text("Extract") }
                        OutlinedButton(onClick = { vm.exportDxf(context) }, enabled = state.plan != null) { Text("DXF") }
                        OutlinedButton(onClick = { vm.exportPdf(context) }, enabled = state.plan != null) { Text("PDF") }
                    }
                    state.plan?.let { LegendCard(it) }
                }
            }
        }
    }
}

/**
 * ROUND 15 item 56 — the rendered plan, on screen.
 *
 * A plain zoom/pan of the PNG the engine wrote. Decoded once, keyed on the
 * path, and with `inSampleSize` set from the view budget rather than loading a
 * 1600 x 1600 bitmap at full depth for a phone viewport — a 5 MB PNG of stored
 * deflate decodes to ~7.7 MB of ARGB, which is fine once and not fine on every
 * recomposition.
 */
@Composable
private fun RenderedPlanView(result: FloorPlanResult) {
    val bitmap = remember(result.pngPath) {
        runCatching {
            val opts = android.graphics.BitmapFactory.Options().apply { inSampleSize = 2 }
            android.graphics.BitmapFactory.decodeFile(result.pngPath, opts)
        }.getOrNull()
    }
    if (bitmap == null) {
        CenteredText("The floor plan was written but could not be opened for preview.")
        return
    }
    var scale by remember(result.pngPath) { mutableFloatStateOf(1f) }
    var panX by remember(result.pngPath) { mutableFloatStateOf(0f) }
    var panY by remember(result.pngPath) { mutableFloatStateOf(0f) }
    Image(
        bitmap = bitmap.asImageBitmap(),
        contentDescription = "Floor plan preview",
        contentScale = ContentScale.Fit,
        modifier = Modifier
            .fillMaxSize()
            .testTag("planPreview")
            .pointerInput(result.pngPath) {
                detectTransformGestures { _, pan, zoom, _ ->
                    scale = (scale * zoom).coerceIn(1f, 8f)
                    panX += pan.x
                    panY += pan.y
                }
            }
            .graphicsLayer(
                scaleX = scale,
                scaleY = scale,
                translationX = panX,
                translationY = panY,
            ),
    )
}

@Composable
private fun CenteredText(text: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(
            text,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(32.dp),
        )
    }
}

/**
 * B11's plan viewer.
 *
 * Everything drawn comes from A12's [PlanModel] and nothing is re-derived:
 * walls are drawn as their **footprint** (two faces offset by half the
 * thickness) exactly like A12's DXF writer, because that is what reads as a
 * wall; a single-face wall is drawn thinner and dashed-in-spirit (lighter) so
 * "we measured one side and assumed the rest" is visible rather than implied.
 */
@Composable
private fun PlanCanvas(plan: PlanModel) {
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    val wallColor = MaterialTheme.colorScheme.onSurface
    val assumedColor = MaterialTheme.colorScheme.onSurfaceVariant
    val doorColor = MaterialTheme.colorScheme.primary
    val windowColor = MaterialTheme.colorScheme.tertiary
    val roomColor = MaterialTheme.colorScheme.secondary

    Canvas(
        Modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                detectTransformGestures { _, pan, zoom, _ ->
                    scale = (scale * zoom).coerceIn(0.2f, 20f)
                    offset += pan
                }
            },
    ) {
        val b = plan.bounds
        if (!b.valid || b.width <= 0 || b.height <= 0) return@Canvas
        val margin = 32f
        val fit = min(
            (size.width - margin * 2) / b.width.toFloat(),
            (size.height - margin * 2) / b.height.toFloat(),
        )
        val s = fit * scale
        val cx = size.width / 2f + offset.x
        val cy = size.height / 2f + offset.y
        val centre = b.center()

        // Plan +y is "up" in the world and screen +y is down, so y is negated
        // once, here — the same single flip A12's PDF writer does.
        fun px(x: Double, y: Double) = Offset(
            cx + ((x - centre.first) * s).toFloat(),
            cy - ((y - centre.second) * s).toFloat(),
        )

        // Rooms first, so walls draw over them.
        plan.rooms.forEach { room ->
            if (room.polygon.size < 3) return@forEach
            val path = Path()
            room.polygon.forEachIndexed { i, v ->
                val p = px(v.x, v.y)
                if (i == 0) path.moveTo(p.x, p.y) else path.lineTo(p.x, p.y)
            }
            path.close()
            drawPath(path, roomColor.copy(alpha = if (room.fullyMeasured) 0.14f else 0.07f))
            drawPath(path, roomColor.copy(alpha = 0.5f), style = Stroke(width = 1.5f))
        }

        plan.walls.forEach { w ->
            val measured = w.evidence == WallEvidence.PAIRED_FACES
            val half = (if (measured) w.thicknessM else 0.0) * 0.5
            val dx = w.b.x - w.a.x
            val dy = w.b.y - w.a.y
            val len = kotlin.math.hypot(dx, dy).coerceAtLeast(1e-9)
            val nx = -dy / len * half
            val ny = dx / len * half
            if (measured) {
                val path = Path()
                val p1 = px(w.a.x + nx, w.a.y + ny)
                val p2 = px(w.b.x + nx, w.b.y + ny)
                val p3 = px(w.b.x - nx, w.b.y - ny)
                val p4 = px(w.a.x - nx, w.a.y - ny)
                path.moveTo(p1.x, p1.y)
                path.lineTo(p2.x, p2.y)
                path.lineTo(p3.x, p3.y)
                path.lineTo(p4.x, p4.y)
                path.close()
                drawPath(path, wallColor.copy(alpha = 0.85f))
            } else {
                drawLine(assumedColor, px(w.a.x, w.a.y), px(w.b.x, w.b.y), strokeWidth = 3f)
            }
        }

        plan.openings.forEach { o ->
            val c = when (o.kind) {
                OpeningKind.DOOR_CANDIDATE -> doorColor
                OpeningKind.WINDOW_CANDIDATE -> windowColor
                else -> Color.Gray
            }
            drawLine(c, px(o.a.x, o.a.y), px(o.b.x, o.b.y), strokeWidth = 6f)
        }
    }
}

private fun com.lidarscan.core.plan.PlanBounds.center(): Pair<Double, Double> =
    Pair((minX + maxX) / 2.0, (minY + maxY) / 2.0)

@Composable
private fun LegendCard(plan: PlanModel) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("What you are looking at", style = MaterialTheme.typography.titleSmall)
            Text(
                "Solid walls have both faces measured, so their thickness is real. Thin lines are single-face walls: " +
                    "one side was scanned and the thickness is an assumption, which is also why rooms are not inset " +
                    "against them.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "Openings are candidates, not confirmed doors and windows — the pipeline sees a hole in a horizontal " +
                    "slice, not a door leaf. A faded room polygon closed itself through a bridged opening, so its area " +
                    "rests on an inference.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "${plan.stats.occupiedCells} occupied cells · ${plan.stats.ransacLines} RANSAC lines · " +
                    "${plan.stats.pairedWalls} paired · ${plan.stats.snappedWalls} snapped",
                style = MaterialTheme.typography.labelSmall,
            )
        }
    }
}

@Composable
private fun SliderRow(label: String, value: Float, range: ClosedFloatingPointRange<Float>, readout: String, onChange: (Float) -> Unit) {
    Column {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            Text(readout, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Slider(value = value.coerceIn(range), valueRange = range, onValueChange = onChange)
    }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
