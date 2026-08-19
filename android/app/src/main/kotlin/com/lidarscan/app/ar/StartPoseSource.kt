package com.lidarscan.app.ar

/**
 * ROUND 21 (item 84) — the slice of [CaptureArController] the **start
 * sequence** actually consumes: the pose ring for the ROUND-12 tracking gate
 * and the ROUND-20 hold-steady stage, and the world-frame reset + pose-counter
 * arm that precede them.
 *
 * Why this exists at all: the round-20 regression (three dead Starts in the
 * owner's 2026-08-20 01:33 log) lived on the ONE path no JVM test could run —
 * the hold stage requires a live controller, `CaptureArController` requires
 * ARCore and an Android `Context`, so "the hold stage's own re-entry into
 * `startCapture` with the in-flight atomic held" had never executed in any
 * test. This interface is the seam that makes the REAL start path — gate →
 * hold → record call — drivable by a fake pose source on a bare JVM, so a
 * deadlock in the sequence's own plumbing can never again hide behind "needs
 * a phone".
 *
 * [CaptureArController] implements it verbatim; production wiring is
 * unchanged ([com.lidarscan.app.ui.capture.CaptureViewModel]'s
 * `startPoseSource` defaults to the controller).
 */
interface StartPoseSource {
    /** The controller's pose ring, oldest first — what the gate and the hold refine over. */
    fun poseWindow(): List<com.lidarscan.core.capture.PoseSample>

    /** Arms the accepted-pose counters for a new capture (or a new reset). */
    fun resetPoseCounters()

    /** ROUND 14/16: throw the world frame away and rebuild the tracking session. */
    fun resetWorldFrame(attempts: Int): CaptureArController.ResetResult
}
