package com.lidarscan.app.capture

import com.google.ar.core.Frame
import com.google.ar.core.TrackingState
import com.lidarscan.core.capture.TrajectoryTrail
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * ROUND 5.3 (item 18): keeps the walked path from the ARCore pose stream and
 * publishes it, already fitted to a unit box, for the capture viewport to draw.
 *
 * Fed from the **same** `CaptureArController` frame listener list that B8's
 * keyframe recorder uses, so it costs no extra ARCore work and it runs in both
 * view modes (the round-5 pose pump drives frames even in 3D-orbit — see
 * `ArPosePumpView`). Runs during **preview as well as recording**: a walkthrough
 * operator wants to see the trail while framing the first few metres, not only
 * after pressing record.
 *
 * The decimation, the fit and the path length are [TrajectoryTrail]'s (`:core`,
 * unit-tested); this class is only the ARCore adapter and the `StateFlow`.
 *
 * **Mid-360 note.** For a Mid-360 the trajectory comes from A6's LIO inside the
 * engine, not from ARCore, and the C ABI exposes no live-trajectory getter
 * (`scan_engine_pose_gate_at` answers a *gate* question, not "give me the last N
 * poses"). So on a Mid-360 walk the trail only appears when ARCore is also
 * running (it is, whenever camera keyframes are on). Recorded as an engine seam in
 * android/NOTES.md's ROUND 5 section.
 */
class TrajectoryTrailRecorder(
    private val trail: TrajectoryTrail = TrajectoryTrail(),
) {
    private val _points = MutableStateFlow<List<TrajectoryTrail.NormalizedPoint>>(emptyList())

    /** The fitted trail, 0..1, screen-oriented (y down). Republished only when a point is actually kept. */
    val points: StateFlow<List<TrajectoryTrail.NormalizedPoint>> = _points.asStateFlow()

    private val _pathLengthM = MutableStateFlow(0f)
    val pathLengthM: StateFlow<Float> = _pathLengthM.asStateFlow()

    /**
     * One ARCore frame, from the GL thread. Cheap by construction: the trail
     * refuses points closer than its spacing, so a standing operator costs one
     * distance comparison per frame and publishes nothing.
     */
    fun onFrame(frame: Frame) {
        val camera = frame.camera
        val tracking = camera.trackingState == TrackingState.TRACKING
        // Ground plane in ARCore's Y-up world frame: x east-ish, z south-ish. The
        // physical camera pose, not the display-oriented one — the latter is
        // rotated for rendering and would make the trail turn when the phone is
        // rotated in the hand.
        val pose = camera.pose
        if (!trail.add(pose.tx(), pose.tz(), tracking)) return
        _points.value = trail.normalized()
        _pathLengthM.value = trail.pathLengthM()
    }

    /** Clears the trail — used when a new session starts, so one walk is one trail. */
    fun clear() {
        trail.clear()
        _points.value = emptyList()
        _pathLengthM.value = 0f
    }
}
