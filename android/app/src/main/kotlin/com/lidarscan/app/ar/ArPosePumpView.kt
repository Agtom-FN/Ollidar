package com.lidarscan.app.ar

import android.opengl.GLSurfaceView
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView

/**
 * ROUND 5 (item 11), and the bug it exists to fix.
 *
 * **The D6 is a 3D scanner via the phone**: it has no IMU of its own
 * (owner-verified), it is mounted on the back of the phone with its fan
 * vertical, and the 6-DoF trajectory comes from ARCore's VIO. The engine's A8
 * pushbroom turns (D6 packet, interpolated pose) pairs into a 3D cloud. So a D6
 * capture with no pose stream is not a degraded 3D capture — it is a pile of 2D
 * fan slices, which is exactly the "2D" reading of the D6 the owner rejected.
 *
 * **What was actually wired before this**: `CaptureArController.onFrame()` — the
 * one call that runs `Session.update()`, publishes the pose and pushes it into
 * the engine — is driven *only* from [ArCameraBackgroundRenderer.onDrawFrame],
 * and that renderer only exists while [ArOverlayView] is composed, i.e. only in
 * `CameraMode.AR`. In the default 3D-orbit view the ARCore session was created
 * and resumed but **nothing ever pumped it**: zero `scan_engine_push_pose`
 * calls, zero keyframes (B8's recorder is fed from the same frame listener), and
 * a pushbroom that had no poses to interpolate against. The AR overlay was a
 * *view* option, and the pose pipeline was silently riding on it.
 *
 * This composable is the pump, decoupled from the view: a deliberately tiny
 * `GLSurfaceView` (2 dp, parked in a corner of the viewport, drawing a camera
 * quad nobody can see) whose GL thread calls the same
 * [ArCameraBackgroundRenderer] the AR overlay uses. Poses therefore flow
 * whenever a pose-tracked capture is on screen, in **either** view mode, and the
 * 3D-orbit view stops being a way to silently disable 3D.
 *
 * ### Why 2 dp of real surface and not an offscreen EGL context
 *
 * `Session.update()` needs a GL context and a camera texture bound in it
 * (`setCameraTextureName`); with none it throws `MissingGlContextException`. The
 * clean answer is a headless EGL pbuffer context on a dedicated thread — no
 * view, nothing on screen. That is the right follow-up, and it is *not* what
 * this does, for one honest reason: **no ARCore-capable device was available to
 * this task**, and a hand-rolled EGL context is precisely the code that works on
 * one driver and not another. This path reuses the exact renderer/texture/thread
 * arrangement B7 already shipped and (partly) validated, at the cost of a 2 dp
 * surface. `ArPosePump` is a single call site, so swapping in an offscreen pump
 * later touches nothing else. See android/NOTES.md's ROUND 5 section.
 *
 * Never composed at the same time as [ArOverlayView] — two pumps would call
 * `Session.update()` from two threads. The Capture screen picks exactly one (see
 * `CaptureViewport`).
 */
@Composable
fun ArPosePumpView(
    controller: CaptureArController,
    modifier: Modifier = Modifier,
) {
    // ── ROUND 22 item 89: THIS view instance's claim ────────────────────────
    //
    // Navigation Compose keeps the OUTGOING destination composed through the
    // transition, so on a trip back into the Scan tab the new pump's [factory]
    // runs BEFORE the old pump's [onRelease]. Until this round both spoke only
    // in terms of `RendererOwner.POSE_PUMP` — and with the AR overlay archived
    // that is the only value there is — so the dying view's release nulled the
    // live view's claim, `mayDrive` answered NOT_OWNER for the rest of the
    // process, and the owner's phone reported "tracking lost until app
    // restart". Holding the token here makes the release instance-scoped: a
    // stale pump can only ever release its own claim.
    //
    // `remember` and not a local: `factory` and `onRelease` are separate
    // lambdas invoked at different times, and this is the one piece of state
    // they must agree about.
    val claim = remember { java.util.concurrent.atomic.AtomicReference<ArSessionGate.Claim?>(null) }
    AndroidView(
        modifier = modifier.size(POSE_PUMP_DP.dp).testTag("arPosePump"),
        factory = { ctx ->
            // ROUND 5 AUDIT bugfix: claimed here, on the main/Compose thread,
            // at the exact moment this AndroidView is created — see
            // `CaptureArController.RendererOwner`'s doc for why an explicit
            // claim (rather than "whichever GL thread's factory ran") is what
            // actually prevents the black-camera race with ArOverlayView.
            claim.set(controller.claimRenderer(CaptureArController.RendererOwner.POSE_PUMP))
            GLSurfaceView(ctx).apply {
                preserveEGLContextOnPause = true
                setEGLContextClientVersion(2)
                setEGLConfigChooser(8, 8, 8, 8, 16, 0)
                // No onFrame consumer: the Filament camera is driven by the
                // orbit/follow manipulator in this mode, and the keyframe
                // recorder subscribes to the controller directly
                // (CaptureViewModel.startArPipelines) rather than through the
                // renderer, so it is fed by this pump too.
                setRenderer(ArCameraBackgroundRenderer(controller, claim.get()))
                renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
                controller.setDisplayGeometry(display, width.coerceAtLeast(1), height.coerceAtLeast(1))
            }
        },
        onRelease = {
            // ROUND 6: the surface is going away with this view, so the gate is
            // told that specifically (it is the "surface destroyed mid-claim"
            // case ArSessionGate's tests pin). Guarded because `onPause()` on a
            // GLSurfaceView whose thread already died can throw on some OEM
            // builds, and a throw out of `onRelease` is a Compose-thread crash.
            runCatching { it.onPause() }
            // Only relinquishes if the pump still owns it — see
            // `releaseRenderer`'s own doc for why an out-of-order release must
            // not undo a newer claim from the overlay switching in.
            controller.onRendererSurfaceDestroyed(claim.getAndSet(null))
        },
    )
}

/**
 * Small enough to be invisible next to a chip, large enough that the window
 * manager gives it a real surface and the GL thread actually runs (a 0 dp view
 * is never drawn, which would silently stop the pose stream again).
 */
private const val POSE_PUMP_DP = 2
