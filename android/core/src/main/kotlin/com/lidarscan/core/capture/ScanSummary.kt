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

    val grade: ScanGrade
        get() = when {
            pointsCaptured <= 0L -> ScanGrade.POOR
            sections > MAX_SECTIONS_FAIR -> ScanGrade.POOR
            trackingDrops > MAX_DROPS_FAIR -> ScanGrade.POOR
            pointsPerMeter < MIN_DENSITY_FAIR -> ScanGrade.POOR
            sections > MAX_SECTIONS_GOOD -> ScanGrade.FAIR
            trackingDrops > MAX_DROPS_GOOD -> ScanGrade.FAIR
            pointsPerMeter < MIN_DENSITY_GOOD -> ScanGrade.FAIR
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
            else ->
                "One section, no tracking drops, ${pointsPerMeter.roundToInt()} points per metre."
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
    }
}
