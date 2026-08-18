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
    /** ROUND 6 (item 22): the preset's trail length, applied live. */
    fun setCapacity(points: Int) {
        trail.capacity = points
        _points.value = trail.normalized()
        _pathLengthM.value = trail.pathLengthM()
        // ROUND 16 item 59: the metric polyline, for the 3D ribbon. Published
        // from the same instant as the normalized one so the tile and the cloud
        // can never show two different walks.
        _worldPoints.value = trail.snapshot()
    }

    private val _points = MutableStateFlow<List<TrajectoryTrail.NormalizedPoint>>(emptyList())

    /** The fitted trail, 0..1, screen-oriented (y down). Republished only when a point is actually kept. */
    /**
     * ROUND 16 item 59 — the walk in METRES, in ARCore's world frame, for the
     * ribbon drawn inside the 3D cloud.
     *
     * `TrajectoryTrail.snapshot()` has existed since ROUND 5 and had no callers
     * at all; this is the accessor that was missing, and adding it is most of
     * what the live half of item 59 needed.
     */
    private val _worldPoints = MutableStateFlow<List<TrajectoryTrail.Point>>(emptyList())
    val worldPoints: StateFlow<List<TrajectoryTrail.Point>> = _worldPoints.asStateFlow()

    val points: StateFlow<List<TrajectoryTrail.NormalizedPoint>> = _points.asStateFlow()

    private val _pathLengthM = MutableStateFlow(0f)
    val pathLengthM: StateFlow<Float> = _pathLengthM.asStateFlow()

    /**
     * ROUND 11 (owner item 44): the TOTAL distance walked, accumulated as
     * points are kept and never reduced.
     *
     * [pathLengthM] is the length of the DRAWN trail, and the trail is a ring —
     * once a walk exceeds the preset's point budget the oldest points fall off
     * and that number stops growing. Correct for the overlay it feeds
     * (`where have I just been`), wrong for a scan summary, which wants the
     * whole walk. Both exist rather than one being fixed, because the ring
     * falling off is the overlay's feature.
     *
     * Accumulated from the KEPT points, so it is a polyline through 15 cm
     * samples: it can under-read a very twisty path by a few percent and can
     * never over-read one.
     */
    private val _totalPathM = MutableStateFlow(0f)
    val totalPathM: StateFlow<Float> = _totalPathM.asStateFlow()

    private var lastKeptX = Float.NaN
    private var lastKeptZ = Float.NaN

    /**
     * ROUND 12: the walk's return to its own start, in constant space. The
     * trail ring drops its oldest points, so the start is not in it after a
     * long walk — see [com.lidarscan.core.capture.LoopReturnTracker].
     */
    private val loopReturn = com.lidarscan.core.capture.LoopReturnTracker()

    /** Metres from the walk's current position to its start, once it is a loop. */
    val loopEndGapM: Float? get() = loopReturn.endGapMeters

    /** The closest the walk ever came back to its start. */
    val loopClosestApproachM: Float? get() = loopReturn.closestApproachMeters

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
        onPose(pose.tx(), pose.ty(), pose.tz(), tracking)
    }

    /**
     * ROUND 17 item 65 — the whole of [onFrame] except the ARCore types.
     *
     * Split out because the bug this method now contains the fix for could not
     * be tested: everything here used to live inside a method whose only
     * argument is a `com.google.ar.core.Frame`, which cannot be constructed on
     * a bare JVM and does not exist on the capture emulator either. So the one
     * line that decides whether the operator sees their path in the 3D cloud
     * had no test, and shipped without one.
     *
     * `x`/`z` are the ground plane and `y` is height, all in ARCore's world
     * frame, exactly as `Pose.tx()/ty()/tz()` report them.
     */
    fun onPose(x: Float, y: Float, z: Float, tracking: Boolean) {
        if (!trail.add(x, z, tracking, y)) return
        if (!lastKeptX.isNaN()) {
            val dx = x - lastKeptX
            val dz = z - lastKeptZ
            _totalPathM.value += kotlin.math.sqrt(dx * dx + dz * dz)
        }
        lastKeptX = x
        lastKeptZ = z
        loopReturn.add(x, z, _totalPathM.value)
        _points.value = trail.normalized()
        _pathLengthM.value = trail.pathLengthM()
        // ROUND 17 item 65 — THE OWNER'S "my path not show in the point cloud.
        // its just a 2d map of my path", in one line.
        //
        // ROUND 16 added `_worldPoints`, added its accessor, added the ribbon
        // builder, the material, the LINE_STRIP draw and the Review reader —
        // and published `_worldPoints` from `setCapacity()` and `clear()` and
        // from nowhere else. `setCapacity()` is called when the operator
        // changes the performance preset, so the live 3D ribbon held whatever
        // the walk looked like at the moment a preset was last touched, which
        // on every real capture is the empty list. The 108 dp bird's-eye tile
        // beside it kept updating, because it is published two lines up. So the
        // operator got exactly one view of their path and it was the 2D one —
        // which is what he said, precisely.
        //
        // Same instant, same gate: both flows are published only when the trail
        // actually kept a point, so the tile and the cloud can never show two
        // different walks.
        _worldPoints.value = trail.snapshot()
    }

    /** Clears the trail — used when a new session starts, so one walk is one trail. */
    fun clear() {
        trail.clear()
        _worldPoints.value = emptyList()
        _points.value = emptyList()
        _pathLengthM.value = 0f
        _totalPathM.value = 0f
        lastKeptX = Float.NaN
        lastKeptZ = Float.NaN
        loopReturn.reset()
    }
}
