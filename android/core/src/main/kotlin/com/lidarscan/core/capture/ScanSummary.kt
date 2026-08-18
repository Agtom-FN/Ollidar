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
     * infinity — standing still and sweeping IS dense, and the grade should not
     * punish the stationary case the walkthrough default was never about.
     */
    val pointsPerMeter: Double
        get() = pointsCaptured.toDouble() / max(pathLengthMeters, MIN_PATH_FOR_DENSITY_M)

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
            pointsPerMeter < MIN_DENSITY_FAIR -> ScanGrade.POOR
            sections > MAX_SECTIONS_GOOD -> ScanGrade.FAIR
            trackingDrops > MAX_DROPS_GOOD -> ScanGrade.FAIR
            pointsPerMeter < MIN_DENSITY_GOOD -> ScanGrade.FAIR
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
                "$sections sections — tracking restarted too many times for the pieces to line up. Rescan."
            trackingDrops > MAX_DROPS_FAIR ->
                "$trackingDrops tracking drops — large gaps in the room. Rescan more slowly, with more light."
            pointsPerMeter < MIN_DENSITY_FAIR ->
                "${pointsPerMeter.roundToInt()} points per metre — you walked too fast to fill in the walls. Rescan."
            sections > MAX_SECTIONS_GOOD ->
                "$sections sections — usable, but the pieces may not line up perfectly."
            trackingDrops > MAX_DROPS_GOOD ->
                "$trackingDrops tracking drops — some gaps. Walk those parts again if they matter."
            pointsPerMeter < MIN_DENSITY_GOOD ->
                "${pointsPerMeter.roundToInt()} points per metre — a little thin. Slow down for finer detail."
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
                ("One section, no tracking drops, ${pointsPerMeter.roundToInt()} points per metre. " +
                    "Coverage checks passed; alignment is not measured on the phone.")
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
    }
}
