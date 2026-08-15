package com.lidarscan.core.calib

import kotlin.math.abs

/**
 * WIZARD.md screen 2's five live checks, as data rather than as UI. Each is a
 * chip that goes green; **the shutter is automatic** — when all five are green
 * for the dwell time the pose is captured, "which is what keeps the rig
 * still".
 *
 * The rule and the failing copy are both from the wizard spec's own table, so
 * that table has exactly one implementation.
 */
enum class PoseCheck(val label: String, val failingCopy: String) {
    BOARD_VISIBLE("Board fully visible", "Move back a little — the whole board must be in frame"),
    VIEWING_ANGLE("Viewing angle", "Too side-on — face the board more"),
    ROLL_MATCHED("Roll matched", "Tilt the phone to match the outline"),
    LIDAR_SEES_IT("Lidar sees it", "The lidar can't find the board — check it stands clear of the wall"),
    HOLD_STILL("Hold still", "Hold still…"),
}

data class PoseCheckThresholds(
    /** "all inner corners detected, >= 40 px from the edge" */
    val minEdgeMarginPx: Double = 40.0,
    /** "mean incidence < 62°" */
    val maxIncidenceDeg: Double = 62.0,
    /** "within ±15° of the prescribed roll" */
    val rollToleranceDeg: Double = 15.0,
    /** Minimum segmented lidar returns on the board — 20 (D6) / 150 (Mid-360). */
    val minLidarReturns: Int,
    /** Hold-still gate: ARCore linear + angular speed under threshold for the dwell. */
    val maxAngularRateRadPerS: Double = 0.20,
    val maxLinearSpeedMPerS: Double = 0.08,
    /** Ring-progress dwell: 1.5 s (D6), 1.0 s (Mid-360). */
    val dwellMillis: Long,
    /** WIZARD.md screen 1 rule 2: "Stand back 1.2–2 m". Enforced generously — the band is advice, not a cliff. */
    val minDistanceM: Double = 0.8,
    val maxDistanceM: Double = 2.6,
) {
    companion object {
        fun forProfile(profile: LidarProfile) = PoseCheckThresholds(
            minLidarReturns = profile.minReturnsPerPose,
            dwellMillis = if (profile.scanIs2d) 1_500L else 1_000L,
        )
    }
}

/** Everything the checks read for one live frame. Nulls mean "not available this frame", which fails the relevant check rather than passing it. */
data class LiveObservation(
    val detection: CheckerboardDetection?,
    val plane: TargetPlaneObservation?,
    val imageWidth: Int,
    val imageHeight: Int,
    val cameraRollDeg: Double,
    val prescribedRollDeg: Double,
    val lidarReturnsOnBoard: Int,
    val angularRateRadPerS: Double,
    val linearSpeedMPerS: Double,
    val trackingOk: Boolean,
)

data class PoseCheckState(val passing: Set<PoseCheck>, val distanceHintM: Double?) {
    val allGreen: Boolean get() = passing.size == PoseCheck.entries.size

    /** The first failing check, in the order a user should fix them. */
    fun firstFailure(): PoseCheck? = PoseCheck.entries.firstOrNull { it !in passing }
}

object PoseChecker {

    fun evaluate(obs: LiveObservation, thresholds: PoseCheckThresholds): PoseCheckState {
        val passing = mutableSetOf<PoseCheck>()

        val detection = obs.detection
        val plane = obs.plane
        if (detection != null &&
            detection.marginPx(obs.imageWidth, obs.imageHeight) >= thresholds.minEdgeMarginPx &&
            plane != null &&
            plane.distanceM in thresholds.minDistanceM..thresholds.maxDistanceM
        ) {
            passing.add(PoseCheck.BOARD_VISIBLE)
        }

        if (plane != null && plane.incidenceDeg < thresholds.maxIncidenceDeg) {
            passing.add(PoseCheck.VIEWING_ANGLE)
        }

        if (abs(angleDifferenceDeg(obs.cameraRollDeg, obs.prescribedRollDeg)) <=
            thresholds.rollToleranceDeg
        ) {
            passing.add(PoseCheck.ROLL_MATCHED)
        }

        if (obs.lidarReturnsOnBoard >= thresholds.minLidarReturns) {
            passing.add(PoseCheck.LIDAR_SEES_IT)
        }

        if (obs.trackingOk &&
            obs.angularRateRadPerS <= thresholds.maxAngularRateRadPerS &&
            obs.linearSpeedMPerS <= thresholds.maxLinearSpeedMPerS
        ) {
            passing.add(PoseCheck.HOLD_STILL)
        }

        return PoseCheckState(passing, plane?.distanceM)
    }

    /** Degrees version of [angleDifference], wrapping at ±180. */
    fun angleDifferenceDeg(a: Double, b: Double): Double =
        Math.toDegrees(angleDifference(Math.toRadians(a), Math.toRadians(b)))
}

/**
 * The automatic shutter: all checks green *continuously* for the dwell.
 * Any red chip resets the ring to zero — a pose that was only briefly still
 * is exactly the pose whose motion blur and pose error S6's budget cannot
 * absorb.
 */
class ShutterTimer(private val dwellMillis: Long) {
    private var greenSinceMillis: Long? = null

    /** Returns the ring progress in 0..1; [fire] tells the caller to capture. */
    fun update(allGreen: Boolean, nowMillis: Long): Progress {
        if (!allGreen) {
            greenSinceMillis = null
            return Progress(0f, fire = false)
        }
        val since = greenSinceMillis ?: nowMillis.also { greenSinceMillis = it }
        val elapsed = nowMillis - since
        if (elapsed >= dwellMillis) {
            greenSinceMillis = null
            return Progress(1f, fire = true)
        }
        return Progress((elapsed.toDouble() / dwellMillis).toFloat().coerceIn(0f, 1f), fire = false)
    }

    fun reset() {
        greenSinceMillis = null
    }

    data class Progress(val ring: Float, val fire: Boolean)
}
