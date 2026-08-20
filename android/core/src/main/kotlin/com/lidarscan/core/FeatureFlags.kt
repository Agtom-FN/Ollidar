package com.lidarscan.core

/**
 * ROUND 10 (owner item 39) — **what is switched off for the camera-less rig,
 * in one place, without deleting the code.**
 *
 * The owner's words: *"disable the follow and rgb since we dont use the camera
 * now… pause, disable and hide the colorize function and features."*
 *
 * ## Why flags and not deletion
 *
 * All three of these are *paused*, not wrong. The follow camera is ROUND 8's
 * third-person solve and its maths is tested and correct; RGB is a live shader
 * mode whose enum ordinal is pinned to `scanengine::cloud::ColorMode` across
 * the C ABI; colorization is a whole engine subsystem (A11) with its own job
 * kind, its own gate and its own tests. Deleting any of them would cost a
 * round to bring back and would silently renumber an ABI-coupled enum on the
 * way out.
 *
 * So this file follows the house style the AR overlay was retired with
 * (`CaptureScreen.AR_OVERLAY_ARCHIVED`, ROUND 7 item 25): a compile-time
 * constant that removes the control from the UI and pins the default, while
 * every code path behind it stays compiled and stays under test. Reviving one
 * is a one-line change here plus whatever the KDoc below says.
 *
 * ## Why `:core` and why `const val`
 *
 * `:core` has no `BuildConfig` of its own (see `FileProjectStore`'s note), and
 * these flags have to be readable from `:core` defaults (`DisplayParams`,
 * `PerformancePreset`) as well as from `:app` composables. A `const val` is
 * visible to both, is constant-folded, and — unlike a DataStore setting —
 * cannot end up in a state where the UI is hidden but the default is not.
 */
object FeatureFlags {

    /**
     * The **Follow camera** (third-person, heading from the trajectory).
     *
     * OFF for ROUND 10. It was ROUND 8's answer to "the camera is facing
     * forward while the scanning is the two sides" and it works, but it is
     * only useful when the operator wants the view to chase them, and with the
     * camera unused the owner wants the 3D orbit view they can turn by hand.
     *
     * To revive: set this true. The enum value, `FollowCamera`, its config and
     * its tests are untouched; the two controls (the Display sheet's View row
     * and the on-viewport Orbit/Follow pill) come back with it, and
     * `CaptureViewModel`'s initial camera mode goes back to FOLLOW.
     */
    const val FOLLOW_CAMERA_ENABLED = false

    /**
     * The **RGB point colour mode** — per-point colour from camera keyframes.
     *
     * OFF for ROUND 10, and it is downstream of [COLORIZE_ENABLED] rather than
     * independent of it: with no camera keyframes there is nothing for RGB to
     * show but the intensity triple the D6 already writes into r/g/b, which
     * INTENSITY renders better and honestly.
     *
     * NOTE: `ColorMode.RGB` itself is NOT removed and must not be. Its ordinal
     * crosses the C ABI into the shader (`PointCloudRenderer` passes
     * `colorMode.ordinal`), and it is the fallback every unsupported mode
     * degrades to. This flag hides it from the pickers; it does not renumber
     * anything.
     */
    const val RGB_COLOR_MODE_ENABLED = false

    /**
     * **Colorization** — camera keyframe capture (`KeyframeRecorder`), the
     * Colorize processing job, and every control that offers either.
     *
     * OFF for ROUND 10. The rig is a D6 on the back of the phone and the
     * camera is not being used, so keyframes cost storage, battery and thermal
     * headroom during a walk and produce nothing anyone looks at.
     *
     * ARCore itself is NOT affected and must not be: it is the pose engine,
     * and the D6's third dimension IS the ARCore trajectory (ROUND 5 item 11).
     * This flag stops the app WRITING KEYFRAMES, not tracking.
     *
     * To revive: set this true. `KeyframeRecorder`, `ProcessingJob.COLORIZE`,
     * the engine's whole `color/` module and their tests are untouched.
     */
    const val COLORIZE_ENABLED = false

    /**
     * ROUND 23 item 106(b) — the **pre-scan checklist as a MODAL SHEET**.
     *
     * OFF. ROUND 22 item 95 asked for the checklist to stop being a separate
     * modal and for its checks to fold into the start panel, which is what
     * `PreScanChecks` + `StartProgress.checks` now do: the checks are shown
     * inside the panel that is already up, and only when one of them has
     * something to report.
     *
     * Nothing is deleted, in this file's own tradition. `PreScanChecklistSheet`,
     * `CaptureViewModel.startFromChecklist` / `dismissPreScanChecklist`, the
     * `startCapture(skipChecklist)` API and the ROUND 19 tests that drive them
     * are all untouched — the ViewModel simply reads the persisted
     * "already dismissed" bit as `true` while this is false, so the sheet is
     * never armed in the shipping app. To revive: set this true.
     */
    const val PRE_SCAN_CHECKLIST_SHEET = false
}
