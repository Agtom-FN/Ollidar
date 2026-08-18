package com.lidarscan.core.capture

import kotlin.math.max
import kotlin.math.roundToInt

/**
 * ROUND 11 item 44 — the card the operator sees the moment a scan is sealed.
 *
 * > "thumbnail, points, duration, path length, sections, tracking-drop count,
 * >  and a plain quality grade so the owner knows keep-or-rescan in five
 * >  seconds."
 *
 * Five seconds is the design constraint and it rules out a dashboard. The card
 * shows six numbers and one word, and the word is the only thing that has to be
 * right at a glance.
 *
 * ## The grade is deterministic and its thresholds are argued, not tuned
 *
 * A grade nobody can predict is worse than no grade, so every threshold below
 * is a consequence of a number this project has already measured:
 *
 *  * **Sections.** A section break is ARCore relocalizing — the world frame
 *    jumped, and everything after the break is in a different frame from
 *    everything before it (ROUND 7 §5). One section is a clean scan. Two or
 *    three are recoverable. More than three is a scan whose pieces no longer
 *    agree, and no post-process fixes it.
 *  * **Tracking drops.** Points taken during tracking loss are excluded by the
 *    assembler (`exclude_flagged`, Tech Spec §3.3), so every drop is a hole in
 *    the room, not merely a warning.
 *  * **Density along the path.** ROUND 10 measured the owner's rig at
 *    **1,453 resolved points per second**. Walking at 1 m/s that is ~1,450
 *    points per metre of path; at 2 m/s it is ~725. So points-per-metre is
 *    walking speed in disguise, which is exactly the thing the operator can act
 *    on ("you were moving too fast"), and it is the honest way to say it
 *    because it does not depend on the room's size. The floors are 800/m for
 *    GOOD and 400/m for FAIR — about 1.8 m/s and 3.6 m/s on this rig.
 *  * **Zero points** is POOR whatever else is true, and it is the one case the
 *    app already refuses to seal quietly (ROUND 7 item 26).
 *  * **ROUND 14: density along the path is the wrong axis when there is no
 *    path.** The bullet above assumes the operator walks. The owner also scans
 *    by standing still and sweeping the phone around the room, which is a
 *    perfectly sensible way to use a spinning fan lidar — and dividing by a
 *    2.7 m path gave his scan-034 a grade computed from **50,124 points per
 *    metre**. See [isFromTheSpot]: such a scan is judged on points per SECOND,
 *    which is the quantity a fixed viewpoint actually controls.
 *
 * Pure `:core`, no Android, so the grade can be asserted rather than eyeballed.
 */
enum class ScanGrade {
    GOOD,
    FAIR,
    POOR,
}

data class ScanSummary(
    val pointsCaptured: Long,
    val elapsedMillis: Long,
    val pathLengthMeters: Double,
    val sections: Int,
    val trackingDrops: Int,
    val recordingSizeBytes: Long,
    /**
     * ROUND 12 — the mount trim's measured split-half accuracy in degrees, or
     * null when the trim predates 0.7.1 or was never converged.
     *
     * This is the ONE geometric input the app can supply without the engine,
     * and it is a real one: ROUND 11 measured through the production assembler
     * that 1.4 degrees of trim error paints an overhead feature 16.3 cm apart
     * between the two legs of an out-and-back at 3 m.
     */
    val mountTrimAccuracyDeg: Double? = null,
    /**
     * ROUND 12 — how far the walk's END finished from its START, metres, or
     * null when the walk was too short to be a loop.
     * See [com.lidarscan.core.capture.LoopReturnTracker]: this is REPORTED and
     * deliberately not GRADED, because the app cannot know whether the operator
     * meant to finish where they began.
     */
    val loopEndGapMeters: Double? = null,
) {
    /**
     * Resolved points per metre walked. The denominator is floored at 0.5 m so a
     * tripod scan (no path at all) reports a large density rather than an
     * infinity.
     *
     * ROUND 14: that floor was the whole of the stationary-case handling, and
     * it was not enough — it made the number finite without making it mean
     * anything. Reported still, because a walking scan's density is a real and
     * useful figure; but the GRADE now reads [densityValue], which switches
     * unit when [isFromTheSpot].
     */
    val pointsPerMeter: Double
        get() = pointsCaptured.toDouble() / max(pathLengthMeters, MIN_PATH_FOR_DENSITY_M)

    /**
     * ROUND 14 — the operator stood in one place and painted the room by
     * turning the phone, instead of walking it.
     *
     * This is a real and reasonable way to use a spinning fan lidar, and the
     * app had no idea it was happening. The owner's scan-034 is 67.9 s of it:
     * 2.7 m of path, 135,702 points, and therefore **50,124 "points per
     * metre"** — a number the summary card printed with a straight face and
     * the grader then used. Dividing by a path that is essentially zero does
     * not measure density; it measures how little the operator moved. The
     * floor at [MIN_PATH_FOR_DENSITY_M] stopped it being infinite and stopped
     * nothing else.
     *
     * The gate is path AND duration together: a 3 m path over 4 s is a scan
     * that was stopped early, which is a different fault and must keep the old
     * treatment. A 3 m path over a minute is somebody sweeping.
     */
    val isFromTheSpot: Boolean
        get() = elapsedMillis >= FROM_THE_SPOT_MIN_MILLIS &&
            pathLengthMeters < FROM_THE_SPOT_MAX_PATH_M

    /**
     * ROUND 14 — the density figure that is actually meaningful for THIS scan,
     * with the unit it is measured in. Walking scans are unchanged; a
     * from-the-spot scan is judged on returns per second, because that is what
     * a fixed viewpoint controls. A D6 delivers ~4,000 returns/s and the
     * owner's sweeps land near 2,000 resolved points/s.
     */
    val densityValue: Double
        get() = if (isFromTheSpot) pointsPerSecond else pointsPerMeter

    val densityUnit: String
        get() = if (isFromTheSpot) "points per second" else "points per metre"

    private val densityFairFloor: Double
        get() = if (isFromTheSpot) MIN_RATE_FAIR else MIN_DENSITY_FAIR

    private val densityGoodFloor: Double
        get() = if (isFromTheSpot) MIN_RATE_GOOD else MIN_DENSITY_GOOD

    val pointsPerSecond: Double
        get() = if (elapsedMillis <= 0L) 0.0 else pointsCaptured.toDouble() * 1000.0 / elapsedMillis

    val averageSpeedMps: Double
        get() = if (elapsedMillis <= 0L) 0.0 else pathLengthMeters * 1000.0 / elapsedMillis

    /**
     * ROUND 12 — true when the mount trim that went into this scan was measured
     * and did not converge. See [com.lidarscan.core.calib.MountTrim] —
     * the threshold is one degree, from ROUND 11's measured cost table.
     */
    val mountTrimIsPoor: Boolean
        get() = mountTrimAccuracyDeg?.let { it > POOR_TRIM_DEG } == true

    val grade: ScanGrade
        get() = when {
            pointsCaptured <= 0L -> ScanGrade.POOR
            sections > MAX_SECTIONS_FAIR -> ScanGrade.POOR
            trackingDrops > MAX_DROPS_FAIR -> ScanGrade.POOR
            densityValue < densityFairFloor -> ScanGrade.POOR
            sections > MAX_SECTIONS_GOOD -> ScanGrade.FAIR
            trackingDrops > MAX_DROPS_GOOD -> ScanGrade.FAIR
            densityValue < densityGoodFloor -> ScanGrade.FAIR
            // ROUND 12: a scan taken through a trim that never converged is not
            // a GOOD scan, whatever the counting says about it.
            mountTrimIsPoor -> ScanGrade.FAIR
            else -> ScanGrade.GOOD
        }

    /**
     * One sentence naming the WORST thing about the scan, in the operator's
     * vocabulary. The order matches the grade's own `when`, so the reason can
     * never disagree with the grade it explains.
     */
    val gradeReason: String
        get() = when {
            pointsCaptured <= 0L ->
                "No points were recorded. Check the D6 cable and rescan."
            sections > MAX_SECTIONS_FAIR ->
                // ROUND 13. The old sentence said "tracking restarted", which is
                // not what happens, and "Rescan", which does not tell the
                // operator what to do differently. Measured against the recorded
                // gyro, every one of the owner's section breaks is ARCore
                // RE-ANCHORING — the phone rotated under 1.2 deg while the pose
                // stream jumped 8-13 deg — so it is the camera losing confidence
                // in where it is, and the two things that change that are light
                // and texture in front of the lens.
                "$sections sections — the camera re-anchored $breaks times, so the room was " +
                    "rebuilt from scratch that often. Check nothing is covering the rear camera, " +
                    "turn more lights on, and keep the lens pointed at furniture and edges rather " +
                    "than blank wall. Process this scan to stitch the pieces back together."
            trackingDrops > MAX_DROPS_FAIR ->
                "$trackingDrops tracking drops — the camera stopped knowing where it was, and " +
                    "those seconds are holes in the room. More light, and keep the rear camera " +
                    "clear and pointed at something with detail."
            densityValue < densityFairFloor ->
                if (isFromTheSpot) {
                    "${densityValue.roundToInt()} points per second — the sensor was barely " +
                        "returning. Check the D6 cable and rescan."
                } else {
                    "${densityValue.roundToInt()} points per metre — you walked too fast to fill in the walls. Rescan."
                }
            sections > MAX_SECTIONS_GOOD ->
                // ROUND 13: "may not line up" was true and useless. It is now
                // known exactly how far apart they are — the pose jump itself —
                // and there is something to do about it.
                "$sections sections — the camera re-anchored $breaks time" +
                    (if (breaks == 1) "" else "s") +
                    " and the pieces are about a metre apart until you Process this scan, " +
                    "which stitches them back into one frame."
            trackingDrops > MAX_DROPS_GOOD ->
                "$trackingDrops tracking drops — some gaps. Walk those parts again if they matter."
            densityValue < densityGoodFloor ->
                if (isFromTheSpot) {
                    "${densityValue.roundToInt()} points per second — thinner than this sensor " +
                        "should manage. Check nothing is blocking the puck."
                } else {
                    "${densityValue.roundToInt()} points per metre — a little thin. Slow down for finer detail."
                }
            mountTrimIsPoor ->
                ("Mount reference is only accurate to %.1f deg, which doubles features by " +
                    "several centimetres. Re-zero with a longer, steadier hold and rescan.")
                    .format(mountTrimAccuracyDeg ?: 0.0)
            else ->
                // ── ROUND 12: this sentence used to over-claim. ─────────────
                //
                // It said "One section, no tracking drops, N points per metre"
                // in the voice of a verdict, and the owner read it as one: the
                // app graded `scan-026` **GOOD SCAN** with exactly that
                // sentence, and offline measurement puts that capture's map
                // 5.26 cm out of agreement with ITSELF at 8 s separation — the
                // worst of the three captures examined. Nothing on this card
                // looked at geometry, and the sentence did not say so.
                //
                // It now names what was checked, which is counting, and what
                // was not, which is whether the room lines up.
                ("One section, no tracking drops, ${densityValue.roundToInt()} $densityUnit. " +
                    "Coverage checks passed; alignment is not measured on the phone.")
        }

    /** Re-anchoring events. One section means none happened. */
    val breaks: Int get() = (sections - 1).coerceAtLeast(0)

    /**
     * ROUND 13 — what to DO before the next walk, or null when the scan needs
     * nothing. Shown under the grade beside [loopReturnNote].
     *
     * Separate from [gradeReason] on purpose: the reason describes the scan
     * that just happened, and this describes the next one. The owner's
     * complaint was never that the app failed to count sections; it is that
     * "5 sections" is not an instruction.
     */
    val nextWalkAdvice: String?
        get() = when {
            pointsCaptured <= 0L -> null
            breaks >= 2 ->
                "Before the next walk: uncover the rear camera (a case or the bracket edge is " +
                    "enough), turn the lights up, and walk the turns slowly — the camera re-anchors " +
                    "when it cannot recognise what it is looking at."
            breaks == 1 ->
                "Before the next walk: give the camera more to look at through turns — corners and " +
                    "furniture rather than blank wall."
            trackingDrops > 0 ->
                "Before the next walk: more light. The camera lost tracking $trackingDrops time" +
                    (if (trackingDrops == 1) "" else "s") + "."
            // ROUND 14 — the advice that fits what the operator actually did.
            //
            // "Slow down a little" is the wrong instruction for someone who
            // never moved, and the owner's scan-034 and scan-035 were both
            // taken standing still. What a from-the-spot scan is short of is
            // not speed, it is PARALLAX: measured over 1 s windows, the owner's
            // walking scan gave ARCore 2.43 cm of travel per degree turned and
            // his two sweeps gave 0.53 and 0.56 — a fifth as much. A camera
            // that rotates without translating gets bearing changes with no
            // depth, which is the degenerate case for visual tracking, and it
            // is why scan-035's world frame snapped 162.57 deg while the gyro
            // read 1.56 deg. So the advice for a sweep is to keep moving.
            isFromTheSpot ->
                "Before the next scan: keep walking while you sweep. Turning the phone on the " +
                    "spot gives the camera nothing to judge distance by — a few slow steps " +
                    "sideways as you turn is what keeps the room from jumping."
            densityValue < densityGoodFloor ->
                "Before the next walk: slow down a little — the returns are thinner than the walls " +
                    "want."
            else -> null
        }

    /**
     * ROUND 12 — the conditional drift line, or null when there is nothing
     * worth saying. Shown UNDER the grade, never as part of it.
     */
    val loopReturnNote: String?
        get() {
            val gap = loopEndGapMeters ?: return null
            if (gap < LoopReturnTracker.WORTH_MENTIONING_M) {
                return "Your walk ended %.0f cm from where it started — the tracker held.".format(gap * 100)
            }
            return ("Your walk ended %.0f cm from where it started after %.1f m. If you finished " +
                "where you began, that gap is tracker drift and it is the largest error in this scan.")
                .format(gap * 100, pathLengthMeters)
        }

    companion object {
        const val MIN_PATH_FOR_DENSITY_M = 0.5
        const val MAX_SECTIONS_GOOD = 1
        const val MAX_SECTIONS_FAIR = 3
        const val MAX_DROPS_GOOD = 0
        const val MAX_DROPS_FAIR = 3

        /** ~1.8 m/s on the owner's measured 1,453 points/s rig. */
        const val MIN_DENSITY_GOOD = 800.0

        /** ~3.6 m/s. Below this the walls have visible stripes. */
        const val MIN_DENSITY_FAIR = 400.0

        /**
         * ROUND 12. Mirrors `MountTrim.WARN_STABILITY_DEG`; kept as its own
         * constant here so `:core`'s capture package does not have to reach
         * into `:core`'s calib package for a grading threshold, and so the two
         * can be moved independently if the measured cost table ever changes.
         */
        const val POOR_TRIM_DEG = 1.0

        /**
         * ROUND 14 — the from-the-spot gate. Path AND duration, so a scan that
         * was stopped after four seconds keeps the old (correct) treatment.
         * The owner's two sweeps are 67.8 s / 2.7 m and 62.1 s / 4.5 m; his
         * walk is 110.9 s / 26.6 m.
         */
        const val FROM_THE_SPOT_MIN_MILLIS = 20_000L
        const val FROM_THE_SPOT_MAX_PATH_M = 5.0

        /**
         * ROUND 14 — density floors for a from-the-spot scan, in resolved
         * points per second. These are deliberately NOT a technique judgement:
         * a fixed viewpoint cannot be told to sweep faster to get more points,
         * because the D6's return rate is fixed by the sensor, not by the
         * operator. They exist only to catch a puck that has stopped
         * delivering. ROUND 10 measured this rig at ~1,453 resolved points/s
         * and all three of the owner's 0.8.0 captures land near 2,000/s
         * (1,985 / 2,001 / 2,006), walking and sweeping alike — which is itself
         * the evidence that sweeping costs no density at all.
         */
        const val MIN_RATE_GOOD = 1_000.0
        const val MIN_RATE_FAIR = 400.0
    }
}
