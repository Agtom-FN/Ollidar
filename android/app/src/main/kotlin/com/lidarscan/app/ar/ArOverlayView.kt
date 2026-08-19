package com.lidarscan.app.ar

import android.opengl.GLSurfaceView
import android.view.ScaleGestureDetector
import android.view.SurfaceView
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import com.lidarscan.app.render.CameraMode
import com.lidarscan.app.render.PointCloudRenderer
import com.lidarscan.app.render.PointCloudSource
import com.lidarscan.core.render.ColorMode
import com.lidarscan.core.render.Colormap

/**
 * Tech Spec §3.7's AR view: "Live captured cloud rendered anchored in the
 * ARCore camera view during capture (shared Filament scene, camera-image
 * background) — coverage gaps visible on the spot."
 *
 * Two stacked surfaces (see [ArCameraBackgroundRenderer]'s header for why
 * this composition and not Filament's external-texture path):
 *
 * ```
 *   +-------------------------------------------+
 *   | Filament SurfaceView  (translucent, top)  |  <- B4's PointCloudRenderer,
 *   |   camera = live ARCore pose + projection  |     CameraMode.AR
 *   +-------------------------------------------+
 *   | GLSurfaceView         (opaque, bottom)    |  <- ARCore camera image,
 *   |   drives Session.update() on its thread   |     and the frame pump
 *   +-------------------------------------------+
 * ```
 *
 * The GL thread is the one that calls `Session.update()`, so it is also the
 * thread that pushes poses into the engine and hands frames to B8's keyframe
 * recorder — everything AR-clocked happens there, and only the resulting
 * matrices cross to the UI thread (latched, in
 * [PointCloudRenderer.setArCamera]).
 *
 * Free-orbit mode does not mount this composable at all: it uses
 * `PointCloudView` unchanged, so the toggle is a *structural* switch between
 * two renderers rather than a mode flag inside one.
 */
@Composable
fun ArOverlayView(
    controller: CaptureArController,
    source: PointCloudSource?,
    colorMode: ColorMode,
    colormap: Colormap,
    pointSizePx: Float,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val renderer = remember { PointCloudRenderer(context, translucent = true) }

    DisposableEffect(renderer) {
        onDispose { renderer.detach() }
    }

    renderer.setSource(source)
    renderer.setColorMode(colorMode)
    renderer.setColormap(colormap)
    renderer.setPointSizePx(pointSizePx)
    renderer.setCameraMode(CameraMode.AR)

    // ROUND 22 item 89: this overlay instance's own claim — see
    // [ArPosePumpView]'s note for the race it closes. Archived today
    // (`AR_OVERLAY_ARCHIVED`), and kept correct so that reviving it does not
    // revive the bug.
    val overlayClaim = remember { java.util.concurrent.atomic.AtomicReference<ArSessionGate.Claim?>(null) }
    Box(modifier = modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                // ROUND 5 AUDIT bugfix (field report: "the AR camera not show
                // up"): claimed here, on the main/Compose thread, at the exact
                // moment this AndroidView is created — see
                // `CaptureArController.RendererOwner`'s doc. Without this, the
                // pump's still-alive GL thread could keep re-binding the
                // session's camera texture to ITS OWN (2 dp, off-screen)
                // texture after this overlay's surface came up, and ARCore
                // would never write a frame into the texture this quad
                // actually samples — a permanently black background with no
                // exception and no log line, which is exactly the reported
                // symptom.
                overlayClaim.set(controller.claimRenderer(CaptureArController.RendererOwner.OVERLAY))
                GLSurfaceView(ctx).apply {
                    preserveEGLContextOnPause = true
                    setEGLContextClientVersion(2)
                    // 8888 with an 8-bit alpha and a depth buffer: the alpha
                    // is unused HERE (this surface is the opaque bottom one),
                    // but ARCore requires a config it can attach its camera
                    // stream to, and matching the common sample config avoids
                    // per-device EGL surprises.
                    setEGLConfigChooser(8, 8, 8, 8, 16, 0)
                    setRenderer(
                        ArCameraBackgroundRenderer(controller, overlayClaim.get()) { frame ->
                            // Runs on the GL thread, immediately after the
                            // pose for this frame has been pushed. Hand the
                            // Filament camera the SAME frame's matrices so the
                            // overlay and the background can never disagree by
                            // a frame — which is what a stale pose looks like
                            // to a user: points sliding on the image.
                            val projection = FloatArray(16)
                            val model = FloatArray(16)
                            frame.camera.getProjectionMatrix(projection, 0, AR_NEAR_M, AR_FAR_M)
                            frame.camera.displayOrientedPose.toMatrix(model, 0)
                            renderer.setArCamera(projection, model)
                        },
                    )
                    renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
                    controller.setDisplayGeometry(display, width.coerceAtLeast(1), height.coerceAtLeast(1))
                }
            },
            onRelease = {
                it.onPause()
                controller.releaseRenderer(overlayClaim.getAndSet(null))
            },
        )

        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                SurfaceView(ctx).also { sv ->
                    // Above the GLSurfaceView in the window's z-order. Both
                    // are SurfaceViews with their own windows, so plain view
                    // ordering is not enough — the media-overlay flag is what
                    // puts this one in front of the other rather than behind
                    // the whole window.
                    sv.setZOrderMediaOverlay(true)
                    renderer.attach(sv)
                    // Pinch still zooms the free-orbit view; in AR the camera
                    // is the device, so gestures are deliberately inert here
                    // rather than silently fighting the ARCore pose.
                    val scaleDetector = ScaleGestureDetector(
                        ctx,
                        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
                            override fun onScale(detector: ScaleGestureDetector): Boolean = false
                        },
                    )
                    sv.setOnTouchListener { _, event -> scaleDetector.onTouchEvent(event) }
                }
            },
        )
    }
}

private const val AR_NEAR_M = 0.05f
private const val AR_FAR_M = 100.0f
