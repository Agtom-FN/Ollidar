package com.lidarscan.core.render

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * ROUND 8 — **the third-person follow camera**, and the owner question that
 * killed the one before it.
 *
 * > "will the follow still work? the camera is facing forward while the
 * > scanning is the two side"
 *
 * He is right, twice over.
 *
 * ## 1. A forward-facing follow camera frames the one direction a D6 never scans
 *
 * A COIN-D6 is a **2D lidar spinning in a vertical fan**, clamped to the back of
 * the phone *across* the direction of travel. It paints a ring around the
 * operator — left wall, ceiling, right wall, floor — one revolution every 100 ms,
 * and the 3D cloud only exists because the rig *translates* between revolutions
 * (`engine/src/drivers/d6/…` + A8's pushbroom; see NOTES.md ROUND 7 §1). There is
 * no forward-looking beam at all. A first-person camera sitting at the rig and
 * looking down the walk direction therefore points at the **one solid angle with
 * no returns in it**: the operator would see the ring's left and right edges
 * streaking past the periphery and an empty hole in the middle. That is the
 * literal complaint.
 *
 * The view that shows a D6 walk is the one that looks **at the operator from
 * behind and above**: the ring is then a tube receding toward the camera, the
 * newest revolution is a bright annulus around the rig, and the walked corridor
 * is the tunnel the tube has already carved. So: third person, over the shoulder,
 * pitched down.
 *
 * ## 2. The old FOLLOW branch was not a follow
 *
 * `PointCloudRenderer`'s pre-ROUND-8 FOLLOW branch put the eye at a fixed offset
 * from the **whole cloud's bounding-box centroid**, with the distance fitted to
 * the whole cloud's span:
 *
 * ```
 *   val span = max(boundsMax - boundsMin)   // the ENTIRE walk
 *   val distance = max(span * 1.2f, 3f)
 * ```
 *
 * Both halves are wrong the moment the walk is longer than a room. The centroid
 * of a 60 m corridor walk sits at the 30 m mark and stops moving, and the
 * distance grows without bound — so "follow" progressively becomes "zoom out
 * until the whole building is a smudge", which is the opposite of what a
 * walkthrough operator needs (the same argument [TrajectoryTrail] makes for why
 * the trail is a *recent* ring buffer and not the whole path). **Everything this
 * class fits, it fits against a bounded recent window.**
 *
 * ## Why this is pure `:core`
 *
 * Identical reasoning to `calib/MountTrim.kt`: no Android type, no Filament
 * type, no `float3` — just doubles in and doubles out — so the whole derivation
 * runs on a bare JVM under `:core:test` with a synthetic gait trail, which is
 * the only place the smoothing claims below can actually be *measured*. `:app`
 * owns exactly two things: feeding it poses and handing the result to
 * `Camera.lookAt`.
 *
 * ## World frame: this class does NOT guess which axis is up
 *
 * The codebase has two live conventions and they disagree:
 *
 * * the **runtime** frame is ARCore's, which is **Y-up** — `PointVertex` is "the
 *   session's local metric frame" (`point_page.h`), and for a phone-tracked D6
 *   that frame is ARCore's world frame verbatim (`CaptureArController
 *   .publishPose` pushes `camera.pose` straight into `scan_engine_push_pose`).
 *   `TrajectoryTrail`'s header says the same thing — "ARCore's world frame is
 *   Y-up, so the trail is (x, z)" — and `PointCloudRenderer`'s existing ORBIT and
 *   FOLLOW branches both pass `0, 1, 0` as the up vector.
 * * the **engine's pushbroom test geometry** (`engine/tests/test_pushbroom.cpp`)
 *   builds its synthetic wall with **+z up**.
 *
 * Assuming either one silently is how a camera ends up lying on its side in a
 * frame nobody wrote down. [FollowCameraConfig.upAxis] is therefore an explicit,
 * required-by-construction choice with no "obvious" default beyond the one the
 * runtime actually uses, and every ground-plane/height decomposition in here goes
 * through it. `PointCloudRenderer` passes [UpAxis.Y_UP] and says why at the call
 * site.
 *
 * ## What it is not
 *
 * Not a replacement for 3D orbit. Orbit stays exactly as it was — the free
 * camera, driven by the operator's own drag/pinch through filament-utils'
 * `Manipulator`. This is the hands-off alternative for someone who is walking and
 * cannot also be dragging.
 */
class FollowCamera(
    private val config: FollowCameraConfig = FollowCameraConfig(),
) {

    /** One accepted rig sample. Time in seconds (from the caller's nanos) because every filter below is a time constant in seconds. */
    private class Sample(val t: Double, val x: Double, val y: Double, val z: Double)

    /**
     * The recent pose window, oldest first, trimmed to
     * [FollowCameraConfig.historySeconds]. Bounded by *time*, not by count, so a
     * 10 Hz pose stream and a 30 Hz one both describe the same physical stretch
     * of walk — and so nothing in here can grow with the length of the capture,
     * which is the failure mode being fixed.
     */
    private val trail = ArrayDeque<Sample>()

    private var lastT: Double = Double.NaN

    /** False until the first finite sample; keeps the filters from easing out of a fictional origin. */
    private var primed = false

    private var smoothX = 0.0
    private var smoothY = 0.0
    private var smoothZ = 0.0
    private var smoothVx = 0.0
    private var smoothVy = 0.0
    private var smoothVz = 0.0
    private var smoothDistance = 0.0

    private var headingRad = defaultHeadingRadFor(config.upAxis)
    private var headingEstablished = false

    private var last: FollowCameraSolution = solve()

    /** The most recent solution. Always finite, from construction onward — see [solve]'s degenerate contract. */
    fun solution(): FollowCameraSolution = last

    /** True once the walk direction has been measured rather than assumed. Exposed for the capture UI to explain a still-guessing camera if it ever wants to. */
    fun hasMeasuredHeading(): Boolean = headingEstablished

    /**
     * Forgets the walk. Called when the session changes under the renderer (a new
     * capture, a new replay, the point source detached): the previous walk's
     * heading and framing distance are not merely stale, they are in a different
     * local metric frame, and easing from one into the other would swing the
     * camera through the void between two origins.
     */
    fun reset() {
        trail.clear()
        lastT = Double.NaN
        primed = false
        smoothX = 0.0; smoothY = 0.0; smoothZ = 0.0
        smoothVx = 0.0; smoothVy = 0.0; smoothVz = 0.0
        smoothDistance = 0.0
        headingRad = defaultHeadingRadFor(config.upAxis)
        headingEstablished = false
        last = solve()
    }

    /**
     * Offers one rig pose and returns the camera for this frame.
     *
     * [tNanos] is a monotonic clock; which one does not matter as long as it is
     * *the same one* across consecutive calls, because only differences are used.
     * Out-of-order and repeated timestamps are **dropped**, not clamped — same
     * rule and same reason as `CaptureArController.publishPose`, which drops a
     * repeated ARCore frame timestamp rather than feeding it downstream where it
     * would corrupt every interpolation that follows. Here it would divide a
     * displacement by a zero or negative interval.
     *
     * [recentGeometryRadiusM] is the caller's measurement of how far the *recent*
     * returns reach from the rig — for a D6, effectively the half-width of the
     * room the ring is painting. Null means "not measured"; the framing then
     * falls back to [FollowCameraConfig.nominalGeometryRadiusM]. It is
     * deliberately a nullable `Double` rather than a NaN sentinel: this is called
     * once per rendered frame, so the one boxed Double per frame is not a cost
     * worth trading legibility for.
     *
     * Non-finite inputs are rejected outright. A single NaN reaching the filters
     * is permanent — `NaN * alpha + x` is NaN forever after — and a NaN camera in
     * Filament is a black screen with no error, so this is the only place it can
     * be stopped.
     */
    fun update(
        tNanos: Long,
        x: Double,
        y: Double,
        z: Double,
        recentGeometryRadiusM: Double? = null,
    ): FollowCameraSolution {
        if (!x.isFinite() || !y.isFinite() || !z.isFinite()) return last
        val t = tNanos / 1e9
        if (!t.isFinite()) return last
        if (!lastT.isNaN() && t <= lastT) return last

        // dt is clamped at BOTH ends. Zero (or the first sample) makes every
        // alpha zero, so the filters simply do not advance. The upper clamp
        // matters when the operator switches away from Follow for a minute and
        // back, or the render thread stalls behind a page-upload burst: the real
        // dt is then tens of seconds, every alpha saturates at 1, and the camera
        // teleports. Clamping to maxDtSeconds turns that into a fast ease
        // (alpha = 1 - e^(-0.5/0.5) = 0.63 per frame) instead of a jump cut.
        val dt = if (lastT.isNaN()) 0.0 else (t - lastT).coerceIn(0.0, config.maxDtSeconds)
        lastT = t

        trail.addLast(Sample(t, x, y, z))
        val cutoff = t - config.historySeconds
        while (trail.size > 2 && trail.first().t < cutoff) trail.removeFirst()

        if (!primed) {
            primed = true
            smoothX = x; smoothY = y; smoothZ = z
            smoothVx = 0.0; smoothVy = 0.0; smoothVz = 0.0
            smoothDistance = fitDistance(recentGeometryRadiusM)
        }

        val aPos = alpha(dt, config.positionTauSeconds)
        val aHead = alpha(dt, config.headingTauSeconds)
        val aDist = alpha(dt, config.distanceTauSeconds)

        smoothX += (x - smoothX) * aPos
        smoothY += (y - smoothY) * aPos
        smoothZ += (z - smoothZ) * aPos

        val chord = measureChord()
        if (chord != null) {
            smoothVx += (chord.vx - smoothVx) * aPos
            smoothVy += (chord.vy - smoothVy) * aPos
            smoothVz += (chord.vz - smoothVz) * aPos
            if (chord.groundBaselineM >= config.minHeadingBaselineM) {
                val raw = headingOf(chord)
                headingRad = if (!headingEstablished) {
                    // The first measured heading is ADOPTED, not eased into. The
                    // alternative is a camera that spends its first second and a
                    // half swinging round from the assumed heading while the
                    // operator has already walked five metres — a "broken on
                    // startup" impression bought for nothing, since there is no
                    // earlier heading worth preserving continuity with.
                    raw
                } else {
                    wrapPi(headingRad + wrapPi(raw - headingRad) * aHead)
                }
                headingEstablished = true
            }
            // else: BELOW the baseline the walk direction is unobservable — the
            // operator has stopped, or is turning on the spot. Hold the last good
            // heading. This is the whole no-divide-by-a-near-zero-displacement
            // guard: atan2 of two millimetres of gait noise is a uniformly random
            // angle, and easing toward it would make a standing rig spin.
        }

        val targetDistance = fitDistance(recentGeometryRadiusM)
        smoothDistance += (targetDistance - smoothDistance) * aDist

        last = solve()
        return last
    }

    // --- the derivation ------------------------------------------------------

    /**
     * The two-half-window centroid chord: the low-noise walk-direction and
     * velocity estimator this camera is built on.
     *
     * The obvious estimator — "newest sample minus oldest sample" — is dominated
     * by whatever the two *endpoints* happen to be doing, and per ROUND 7's
     * measured gait model the endpoints are doing ±2 cm laterally and ±3 cm
     * vertically at 2 Hz. Instead: split the last
     * [FollowCameraConfig.headingWindowSeconds] into an early half and a late
     * half, average the positions in each, and take the chord between the two
     * centroids.
     *
     * Why that kills the gait rather than merely attenuating it: the window is
     * **1.0 s = two full gait cycles at 2 Hz**, so each half is exactly one
     * complete cycle, and the mean of a full cycle of any periodic sway is its DC
     * value — zero. The residue is only the window-edge quantisation (one pose
     * period), millimetres at 30 Hz. Nothing downstream has to remove sway that
     * never entered.
     *
     * The chord is divided by the difference of the two halves' **mean times**,
     * not by the nominal half-window, so an irregular or dropped-frame pose
     * stream still yields a correctly scaled velocity instead of one biased by
     * however the samples happened to land.
     *
     * Returns null when there is not yet a populated sample on each side.
     */
    private fun measureChord(): Chord? {
        if (trail.size < 2) return null
        val now = trail.last().t
        val start = now - config.headingWindowSeconds
        val mid = now - config.headingWindowSeconds / 2.0

        var nEarly = 0; var ex = 0.0; var ey = 0.0; var ez = 0.0; var et = 0.0
        var nLate = 0; var lx = 0.0; var ly = 0.0; var lz = 0.0; var lt = 0.0
        for (s in trail) {
            if (s.t < start) continue
            if (s.t < mid) {
                nEarly++; ex += s.x; ey += s.y; ez += s.z; et += s.t
            } else {
                nLate++; lx += s.x; ly += s.y; lz += s.z; lt += s.t
            }
        }
        if (nEarly == 0 || nLate == 0) return null
        ex /= nEarly; ey /= nEarly; ez /= nEarly; et /= nEarly
        lx /= nLate; ly /= nLate; lz /= nLate; lt /= nLate

        val dt = lt - et
        if (dt <= 0.0) return null
        val dx = lx - ex
        val dy = ly - ey
        val dz = lz - ez
        val (g0, g1) = groundOf(dx, dy, dz)
        return Chord(
            dx = dx, dy = dy, dz = dz,
            vx = dx / dt, vy = dy / dt, vz = dz / dt,
            groundBaselineM = hypot(g0, g1),
        )
    }

    private class Chord(
        val dx: Double, val dy: Double, val dz: Double,
        val vx: Double, val vy: Double, val vz: Double,
        /** Length of the chord projected onto the ground plane — the part that carries walk direction. */
        val groundBaselineM: Double,
    )

    private fun headingOf(c: Chord): Double {
        val (g0, g1) = groundOf(c.dx, c.dy, c.dz)
        return atan2(g1, g0)
    }

    /**
     * How far back the camera has to sit for the recent geometry to fit in the
     * frame.
     *
     * A sphere of radius R centred on the look-at point subtends the full frame
     * when `d = R / sin(halfFov)`. The half-angle used is the **vertical** one,
     * matching `PointCloudRenderer`'s `Camera.Fov.VERTICAL` 45° projection, and
     * deliberately *not* `min(vertical, horizontal)`: the phone is held portrait,
     * so the horizontal FoV is the narrow one, and containing the ring
     * horizontally in portrait would push the camera roughly three times further
     * back — a room framed as a distant smudge, which is the exact failure being
     * fixed. Letting the ring's left and right edges run off the sides of a
     * portrait frame is not a loss; it is what makes a corridor read as a
     * corridor.
     *
     * **The trail gets no allowance and needs none.** The last few metres of
     * walked path lie along −heading, which is precisely where the camera is:
     * everything between the eye and the rig is in frame by construction, and
     * path older than that is behind the camera on purpose.
     *
     * R itself is clamped. [FollowCameraConfig.minGeometryRadiusM] stops a rig
     * standing in a doorway (every recent return at 40 cm) from pulling the
     * camera into the operator's back; [FollowCameraConfig.maxGeometryRadiusM]
     * stops one long return down an open corridor — or a replay whose first
     * uploaded slice covers thirty seconds of walk at once — from throwing the
     * camera to the far end of the building. That clamp is the difference between
     * fitting *recent* geometry and re-inventing the old whole-cloud zoom-out.
     */
    private fun fitDistance(recentGeometryRadiusM: Double?): Double {
        val measured = recentGeometryRadiusM
        val radius = if (measured != null && measured.isFinite() && measured > 0.0) {
            measured.coerceIn(config.minGeometryRadiusM, config.maxGeometryRadiusM)
        } else {
            config.nominalGeometryRadiusM
        }
        val halfFov = Math.toRadians(config.verticalFovDeg / 2.0)
        val d = radius / max(sin(halfFov), 1e-3)
        return d.coerceIn(config.minDistanceM, config.maxDistanceM)
    }

    /**
     * Assembles eye/target/up from the smoothed state. Total function: called
     * once in the constructor, so [solution] is finite and usable before a single
     * pose has arrived (the degenerate contract — see below).
     */
    private fun solve(): FollowCameraSolution {
        val distance = if (smoothDistance > 0.0) smoothDistance else fitDistance(null)
        val pitch = Math.toRadians(config.pitchDownDeg)

        // The look-at target is the RIG, not the cloud centroid — that swap is
        // the entire behavioural difference from the branch this replaces.
        //
        // What is used is the *smoothed* rig position plus a lead term. A
        // first-order low-pass has a steady-state lag of exactly tau * v: at
        // walking pace and tau = 0.5 s the camera would frame a point half a
        // metre behind the operator, and worse the faster he walks. Adding
        // tau * v_hat back removes that lag exactly for constant velocity, and
        // v_hat is the half-window centroid chord above, which carries no gait
        // energy — so the correction costs no jitter. During a turn it is only
        // approximate, and there the heading filter dominates the framing
        // anyway.
        val tau = config.positionTauSeconds
        val tx = smoothX + smoothVx * tau
        val ty = smoothY + smoothVy * tau
        val tz = smoothZ + smoothVz * tau

        val (hx, hy, hz) = headingVector(headingRad)
        val (ux, uy, uz) = upVector()

        // Behind along −heading, above along +up. Pitch is the angle of the
        // eye→target ray below horizontal, so back = d·cos(pitch) and
        // up = d·sin(pitch): at the default 35° that is 0.82 d behind and 0.57 d
        // above, i.e. for a 5.2 m leash roughly 4.3 m back and 3.0 m up.
        val back = distance * cos(pitch)
        val rise = distance * sin(pitch)
        return FollowCameraSolution(
            eyeX = tx - hx * back + ux * rise,
            eyeY = ty - hy * back + uy * rise,
            eyeZ = tz - hz * back + uz * rise,
            targetX = tx, targetY = ty, targetZ = tz,
            upX = ux, upY = uy, upZ = uz,
            headingRad = headingRad,
            distanceM = distance,
            headingMeasured = headingEstablished,
        )
    }

    // --- frame conventions ---------------------------------------------------

    /** Splits a vector into its two ground-plane components, in the order the heading angle is measured in. */
    private fun groundOf(x: Double, y: Double, z: Double): Pair<Double, Double> = when (config.upAxis) {
        UpAxis.Y_UP -> x to z
        UpAxis.Z_UP -> x to y
    }

    private fun headingVector(theta: Double): Triple<Double, Double, Double> = when (config.upAxis) {
        UpAxis.Y_UP -> Triple(cos(theta), 0.0, sin(theta))
        UpAxis.Z_UP -> Triple(cos(theta), sin(theta), 0.0)
    }

    private fun upVector(): Triple<Double, Double, Double> = when (config.upAxis) {
        UpAxis.Y_UP -> Triple(0.0, 1.0, 0.0)
        UpAxis.Z_UP -> Triple(0.0, 0.0, 1.0)
    }

    private companion object {
        /**
         * The heading assumed before the walk has ever exceeded the baseline —
         * the "no trail yet / single pose / rig has not moved" case.
         *
         * In a Y-up (ARCore) session the world frame is *defined* by the phone at
         * session start, and the phone's camera looks down **−z**. An operator
         * about to walk through a space is overwhelmingly likely to start by
         * walking the way he is already facing, so −z is the best available prior
         * — and it happens to place the camera on +z, which is exactly where the
         * old FOLLOW branch and `Manipulator`'s `orbitHomePosition(4, 3, 8)` both
         * put it, so switching modes before the first step does not jump the view.
         *
         * A Z-up frame carries no such convention (it is the engine's test
         * geometry, not a tracked session), so +x is used and labelled a
         * placeholder rather than dressed up as a prior.
         */
        fun defaultHeadingRadFor(upAxis: UpAxis): Double = when (upAxis) {
            UpAxis.Y_UP -> -PI / 2.0
            UpAxis.Z_UP -> 0.0
        }

        /**
         * Frame-rate-independent first-order low-pass coefficient:
         * `alpha = 1 - e^(-dt/tau)`.
         *
         * Not the `alpha = 0.1`-per-frame form, which silently changes its cutoff
         * with the frame rate — and this renderer's frame rate is explicitly
         * variable (ROUND 5's operator refresh cap, ROUND 5.3's `RefreshGovernor`
         * auto-downshift). A camera that shakes only once the phone gets hot
         * would be an unpleasant bug to chase.
         */
        fun alpha(dt: Double, tau: Double): Double {
            if (dt <= 0.0) return 0.0
            if (tau <= 0.0) return 1.0
            return 1.0 - exp(-dt / tau)
        }

        fun wrapPi(a: Double): Double {
            var v = a
            while (v > PI) v -= 2.0 * PI
            while (v < -PI) v += 2.0 * PI
            return v
        }
    }
}

/** Which world axis points up. See [FollowCamera]'s header — the codebase has two conventions and neither may be assumed. */
enum class UpAxis { Y_UP, Z_UP }

/**
 * Every tunable [FollowCamera] has, with the measurement or trade-off behind it.
 *
 * The gait numbers quoted are ROUND 7's, and they are not invented: they are the
 * model `engine/tests/test_pushbroom.cpp` and
 * `core/…/calib/D6WalkingGaitPlanarityTest.kt` both walk their synthetic rig
 * with — **±2 cm lateral sway, ±3 cm bob, ±3° yaw and ±1.7° roll per step, at
 * 2 Hz** — and which the per-point-timestamp fix was validated against. Reusing
 * them here means the camera is tuned against the same rig motion the scan
 * quality was.
 */
data class FollowCameraConfig(
    /**
     * Which axis is up in the frame the poses and points are expressed in.
     * Y_UP for a live ARCore-tracked session (see [FollowCamera]'s header); the
     * caller states it, this class never guesses.
     */
    val upAxis: UpAxis = UpAxis.Y_UP,

    /**
     * How much walk the camera keeps. 4 s is 3–6 m at normal indoor walking pace
     * (0.8–1.4 m/s) — "the last few metres", bounded by time so it cannot grow
     * with the capture. Only the heading window (below) actually reads back this
     * far; the extra history is headroom for a slow pose stream, so a 10 Hz
     * trickle still fills both halves of the heading window.
     */
    val historySeconds: Double = 4.0,

    /**
     * The chord window for walk direction and velocity: **1.0 s = exactly two
     * gait cycles at 2 Hz**, split into two one-cycle halves. That equality is
     * the point — see [FollowCamera.measureChord]. Shortening it to 0.5 s would
     * leave each half a *half* cycle, where sway averages to its peak rather than
     * to zero, and the heading would breathe at 2 Hz. Lengthening it buys nothing
     * and adds turn lag.
     */
    val headingWindowSeconds: Double = 1.0,

    /**
     * Below this chord length the walk direction is not observable and the last
     * good heading is held.
     *
     * 5 cm. Two independent reasons it is the right order of magnitude: (a) a rig
     * standing still cannot fake it — full-cycle-averaged gait sway leaves
     * millimetres, so there is better than 10:1 margin against a false heading;
     * (b) over the 0.5 s between the two half-window centroids, 5 cm is 0.1 m/s,
     * a tenth of walking pace, so the heading holds while the operator pauses to
     * look at a corner and resumes the moment he actually moves.
     */
    val minHeadingBaselineM: Double = 0.05,

    /**
     * Position/velocity low-pass time constant. **0.5 s.**
     *
     * A first-order low-pass attenuates by `1/sqrt(1 + (2·pi·f·tau)^2)`. At the
     * 2 Hz gait fundamental with tau = 0.5 s that is 1/sqrt(1 + 39.5) = **0.157**,
     * so ROUND 7's ±3 cm bob reaches the camera as ±4.7 mm and the ±2 cm lateral
     * sway as ±3.1 mm — under a tenth of a degree of angular motion at the
     * framing distances below, i.e. invisible.
     *
     * The cost of a longer tau is lag, and lag is why it stops at 0.5 s rather
     * than going further: at 1 s the camera would take a second and a half to
     * settle after the operator changed pace. The steady-state part of the lag is
     * cancelled by the velocity lead term in [FollowCamera.solve], and what
     * remains is collinear with the offset the camera already has — it reads as a
     * slightly longer leash, never as mis-framing.
     */
    val positionTauSeconds: Double = 0.5,

    /**
     * Heading low-pass time constant. **0.6 s** — slightly slower than position,
     * because a rotating frame is far more nauseating than a translating one.
     *
     * Turn response: a 90° corner is 95 % complete in 3·tau = 1.8 s, on top of the
     * ~0.5 s the chord window itself takes to see the corner — so roughly two and
     * a half seconds to come round, which is about how long the operator takes to
     * walk the corner anyway. Residual 2 Hz yaw: gain 0.113 against ROUND 7's ±3°
     * per step, i.e. ±0.34° — and that is the worst case, since the chord
     * estimator has already removed most of it before the filter sees it.
     */
    val headingTauSeconds: Double = 0.6,

    /**
     * Framing-distance low-pass. **1.0 s**, the slowest of the three: distance is
     * the least urgent thing to get right and the most obnoxious to get wrong.
     * Walking past an open doorway momentarily doubles the measured geometry
     * radius, and at a faster tau the camera would visibly pump in and out once
     * per door.
     */
    val distanceTauSeconds: Double = 1.0,

    /**
     * Downward pitch of the eye→target ray. **35°**, in the middle of the 30–45°
     * band that reads as "over the shoulder" rather than either first-person
     * (which frames the D6's blind direction — the whole bug) or top-down (which
     * throws away the ring's vertical structure, the only reason the scan is 3D).
     *
     * Honest cost: at 35° and a ~5 m leash the eye sits ~3 m up, which in a room
     * with a 2.6 m ceiling is **above the ceiling plane**, so ceiling returns are
     * drawn between the camera and the rig. In a point cloud that is a scattering
     * of dots over the view, not an occluding surface, and the alternatives are
     * both worse: a shallower pitch walks back toward the first-person framing
     * that fails for a D6, and a shorter leash frames nothing.
     */
    val pitchDownDeg: Double = 35.0,

    /**
     * Must match the renderer's own projection, or the fit is wrong by whatever
     * the two disagree by. `PointCloudRenderer.onResized` sets
     * `camera.setProjection(45.0, aspect, …, Camera.Fov.VERTICAL)`.
     */
    val verticalFovDeg: Double = 45.0,

    /**
     * Framing radius when the caller has not measured one. **2 m** — the
     * half-width of an ordinary room or corridor, which is what a D6 ring is
     * painting most of the time. With a 45° vertical FoV that is a 5.2 m leash.
     */
    val nominalGeometryRadiusM: Double = 2.0,

    /** Floor on the measured radius, so a rig in a doorway does not pull the camera into the operator's back. */
    val minGeometryRadiusM: Double = 1.0,

    /** Ceiling on the measured radius — the clamp that keeps this a *recent*-geometry fit and not the old whole-cloud zoom-out. 8 m is a large hall. */
    val maxGeometryRadiusM: Double = 8.0,

    /** Absolute leash limits, after the FoV fit. The lower one keeps the camera outside the operator's own ring; the upper one is a hard stop against any framing pathology reaching the screen. */
    val minDistanceM: Double = 2.5,
    val maxDistanceM: Double = 20.0,

    /** Upper clamp on the integration step — see [FollowCamera.update]. 0.5 s turns a stall or a mode switch into a fast ease rather than a jump cut. */
    val maxDtSeconds: Double = 0.5,
)

/**
 * What [FollowCamera] hands the renderer: an eye, a target and an up vector, in
 * the same world frame the poses came in.
 *
 * **Degenerate contract.** Every field is finite from construction onward, before
 * any pose has arrived — no trail, one pose, a rig that never moves and a stream
 * of NaN all produce a usable camera rather than a null the renderer would have
 * to invent a fallback for. With no measured heading it is the axis convention's
 * documented default ([FollowCamera.hasMeasuredHeading] reports which), anchored
 * at the last known rig position or the frame origin if there has never been one.
 */
data class FollowCameraSolution(
    val eyeX: Double, val eyeY: Double, val eyeZ: Double,
    val targetX: Double, val targetY: Double, val targetZ: Double,
    val upX: Double, val upY: Double, val upZ: Double,
    /** Smoothed walk heading, radians, in the ground plane of the configured [UpAxis]. Exposed for tests and telemetry, not needed to drive a camera. */
    val headingRad: Double,
    /** Smoothed eye→target distance in metres. */
    val distanceM: Double,
    /** False while the heading is still the assumed default (nothing has walked far enough to measure one). */
    val headingMeasured: Boolean,
) {
    /** True when nothing in here is NaN or infinite — the invariant the degenerate tests assert. */
    fun isFinite(): Boolean =
        eyeX.isFinite() && eyeY.isFinite() && eyeZ.isFinite() &&
            targetX.isFinite() && targetY.isFinite() && targetZ.isFinite() &&
            upX.isFinite() && upY.isFinite() && upZ.isFinite() &&
            headingRad.isFinite() && distanceM.isFinite()

    /** Straight-line eye→target distance actually realised, for tests that would rather measure than trust [distanceM]. */
    fun eyeToTargetM(): Double = sqrt(
        (eyeX - targetX) * (eyeX - targetX) +
            (eyeY - targetY) * (eyeY - targetY) +
            (eyeZ - targetZ) * (eyeZ - targetZ),
    )
}

/**
 * Shortest signed difference between two headings, radians, in (−pi, pi].
 * Exposed because "the camera heading converged on the walk heading" is a
 * statement about angles on a circle, and a test that subtracts them naively
 * passes at 179° and fails at 181°.
 */
fun headingDeltaRad(a: Double, b: Double): Double {
    var v = a - b
    while (v > PI) v -= 2.0 * PI
    while (v <= -PI) v += 2.0 * PI
    return v
}

/** [headingDeltaRad] in degrees, absolute — the form the assertions actually read in. */
fun headingErrorDeg(a: Double, b: Double): Double = abs(Math.toDegrees(headingDeltaRad(a, b)))
