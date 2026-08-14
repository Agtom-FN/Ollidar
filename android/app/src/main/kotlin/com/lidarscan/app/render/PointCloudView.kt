package com.lidarscan.app.render

import android.view.ScaleGestureDetector
import android.view.SurfaceView
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap

/**
 * Compose entry point for B4's live 3D view — wraps a plain
 * [android.view.SurfaceView] (Filament's documented Android idiom is
 * `SurfaceView` + `UiHelper`, not a Compose-native surface) via
 * `AndroidView`, and owns exactly one [PointCloudRenderer] for the
 * composable's lifetime. [source] is null before a capture/replay session
 * has anything to read from — the renderer just draws an empty scene then
 * (no crash, no placeholder needed at this layer; `CaptureScreen` is what
 * decides when to show this view at all).
 */
@Composable
fun PointCloudView(
    source: PointCloudSource?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    cameraMode: CameraMode,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val renderer = remember { PointCloudRenderer(context) }

    DisposableEffect(renderer) {
        onDispose { renderer.detach() }
    }

    // Plain calls, not LaunchedEffect: these are synchronous, idempotent
    // setters on the renderer's own fields (no coroutine/suspend work), and
    // PointCloudView only recomposes when one of these params actually
    // changes (Compose skips otherwise), so there is no meaningful
    // redundant-call cost to avoid.
    renderer.setSource(source)
    renderer.setColorMode(colorMode)
    renderer.setColormap(colormap)
    renderer.setPointSizePx(pointSizePx)
    renderer.setCameraMode(cameraMode)

    AndroidView(
        modifier = modifier.fillMaxSize(),
        factory = { ctx ->
            SurfaceView(ctx).also { sv ->
                renderer.attach(sv)
                val scaleDetector = ScaleGestureDetector(
                    ctx,
                    object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
                        override fun onScale(detector: ScaleGestureDetector): Boolean {
                            renderer.onScale(detector.focusX, detector.focusY, detector.scaleFactor - 1f)
                            return true
                        }
                    },
                )
                sv.setOnTouchListener { _, event ->
                    scaleDetector.onTouchEvent(event)
                    renderer.onTouch(event)
                }
            }
        },
    )
}
