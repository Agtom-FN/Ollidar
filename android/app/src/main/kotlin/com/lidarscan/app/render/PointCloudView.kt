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
    /**
     * B3: which point streams to draw. Defaults to [StreamFilter.ALL] so
     * existing callers (the replay path, the AR overlay) are unchanged; the
     * live Capture screen passes [StreamFilter.forSession] so a Mid-360 +
     * live-SLAM session does not draw the sensor-frame preview and the
     * registered map on top of each other. See StreamFilter's own doc.
     */
    streamFilter: StreamFilter = StreamFilter.ALL,
    /**
     * B10: the whole §3.9 parameter set. Null keeps B4's original three-setter
     * behaviour, which is what the Capture screen still uses — a live capture
     * has no per-project display panel open and its point size is its own
     * slider.
     */
    displayParams: com.lidarscan.core.render.DisplayParams? = null,
    /**
     * B11: handed the renderer once it exists, so the measure tool can ask for
     * the current view-projection and viewport. Deliberately a callback rather
     * than a hoisted `remember`: the renderer's lifetime is this composable's,
     * and letting a caller construct one would make it easy to leak a Filament
     * Engine across navigation.
     */
    onRendererReady: ((PointCloudRenderer) -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val renderer = remember { PointCloudRenderer(context) }

    DisposableEffect(renderer) {
        onRendererReady?.invoke(renderer)
        onDispose { renderer.detach() }
    }

    // Plain calls, not LaunchedEffect: these are synchronous, idempotent
    // setters on the renderer's own fields (no coroutine/suspend work), and
    // PointCloudView only recomposes when one of these params actually
    // changes (Compose skips otherwise), so there is no meaningful
    // redundant-call cost to avoid.
    renderer.setSource(source)
    if (displayParams != null) {
        // setDisplayParams owns colour mode, colormap and point size, so the
        // three individual setters would fight it.
        renderer.setDisplayParams(displayParams)
    } else {
        renderer.setColorMode(colorMode)
        renderer.setColormap(colormap)
        renderer.setPointSizePx(pointSizePx)
    }
    renderer.setCameraMode(cameraMode)
    renderer.setStreamFilter(streamFilter)

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
