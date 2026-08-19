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
     * ROUND 19 item 75 — one plain-words sentence naming the largest thin arc
     * of wall coverage around the walked path, measured by the live coverage
     * grid, or null when coverage was healthy or unmeasured. Coverage of what
     * the D6 could see from the walked path — no pretense of global
     * completeness, and no sentence at all when there is nothing to say.
     */
    val coverageAdvice: String? = null,
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
    /**
     * ROUND 18 item 70 — the metres the path JUMPED rather than walked: the
     * re-acquisition teleports after tracking losses, and the steps of refused
     * re-anchors. [pathLengthMeters] no longer contains them (a teleport is
     * not distance the operator covered), so this is where they went — the
     * two numbers together are the whole polyline, and the seal logs both.
     * Reported, deliberately not graded: the drops that produced it are
     * already graded via [trackingDrops].
     */
    val jumpLengthMeters: Double = 0.0,
    /**
     * ROUND 16 item 58 — how many ARCore poses this capture actually recorded.
     *
     * Defaulted to null so every existing construction of this type keeps its
     * exact meaning; null is "this build did not measure it", which is a
     * different statement from zero and must not be graded as one.
     *
     * ZERO IS THE CASE THIS FIELD EXISTS FOR, and it is a case that shipped.
     * The owner's scan-039 recorded 184,454 points over 51 s with no pose
     * stream at all — no `poses_ar.bin`, no `map.bin`, `pathM=0.0` — because a
     * race in the ROUND 14 world-frame reset left the replacement ARCore
     * session alive, resumed, and never delivering a camera frame. The app
     * graded it **FAIR**. It is not a FAIR scan; it is not a scan of a room at
     * all. Every return in it was resolved against nothing, so what is on disk
     * is one sensor's worth of ranges with no trajectory to place them in —
     * two-dimensional data, honestly described.
     */
    val posesRecorded: Long? = null,
    /**
     * ROUND 17 item 64 — did the ENGINE session actually start?
     *
     * `null` for any caller that does not know (the unit tests, and every
     * summary built before this round). `false` means
     * `scan_engine_start` refused, and a capture whose engine never started
     * recorded nothing that can become a room, whatever else the counters say.
     *
     * The owner's scan-045 is why this field exists. Two `[session] start`
     * lines fired for one project six seconds apart; the second one hit
     * `scan_engine_start failed: invalid state`, and on the way to that failure
     * it had already wiped the trail and the pose counters of the capture that
     * WAS running. The card then read the wreckage — `pathM=0.0`, 55,228
     * points, 225 poses — and graded it **GOOD**, because a zero path looks
     * exactly like a deliberate from-the-spot sweep and flips the grader onto
     * points-per-SECOND, which the wreckage passes comfortably.
     */
    val engineStarted: Boolean? = null,
    /**
     * ROUND 17 item 64 — how many points the pushbroom actually RESOLVED into
     * the world frame, as opposed to how many returns arrived.
     *
     * [pointsCaptured] counts what the sensor delivered. This counts what
     * became a room. They are the same number on a healthy capture and they
     * are very different on scan-045, whose exported bundle has no `map.bin`
     * at all: 55,228 returns arrived and not one of them landed anywhere.
     *
     * `null` when the caller cannot tell, which must never be read as zero.
     */
    val worldPointsResolved: Long? = null,
) {
    /**
     * ROUND 17 item 64 — the engine refused to start this capture.
     *
     * Kept separate from [isNoRoom] because the two ask for different things
     * from the operator: this one is "press Start again", that one is "your
     * scan has no positions in it".
     */
    val engineStartFailed: Boolean
        get() = engineStarted == false

    /**
     * ROUND 17 item 64 — returns arrived and none of them became world points.
     *
     * The ROUND 16 check ([isTwoDimensionalOnly]) asked whether any POSES were
     * recorded, and scan-045 had 225 of them, so it passed — while the bundle
     * it sealed contains no `map.bin` and no `processed/` at all. Poses are a
     * necessary condition for a room and not a sufficient one; this is the
     * sufficient one, and it is the number the file itself is made of.
     */
    val isNoRoom: Boolean
        get() = worldPointsResolved != null && worldPointsResolved <= 0L && pointsCaptured > 0L
    /**
     * ROUND 16 item 58 — true when the capture recorded points but no poses.
     *
     * Deliberately not "posesRecorded == 0": a capture that recorded no points
     * either is the ROUND 7 no-data case, which already has its own banner and
     * its own sentence, and stacking a second diagnosis on it would tell the
     * operator to fix the wrong thing.
     */
    val isTwoDimensionalOnly: Boolean
        get() = posesRecorded != null && posesRecorded <= 0L && pointsCaptured > 0L
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
            // ROUND 17 item 64, above everything: a capture whose engine
            // refused to start, or which resolved no world points, is not a
            // scan that can be graded on density at all. scan-045 was graded
            // GOOD on 1,970 points per second of data that never reached a
            // world frame.
            engineStartFailed -> ScanGrade.POOR
            isNoRoom -> ScanGrade.POOR
            // ROUND 16 item 58, and it goes FIRST for the same reason zero
            // points does: a scan with no trajectory cannot be improved by
            // anything the rest of this `when` measures, and every one of those
            // measurements is meaningless without one. scan-039's density was
            // 368,908 "points per metre" over a path of 0.0 m.
            isTwoDimensionalOnly -> ScanGrade.POOR
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
            engineStartFailed ->
                "The recording engine never started, so nothing in this file was placed in a " +
                    "room. Nothing on the phone can recover it. Press Start again — and if it " +
                    "refuses twice, close the app and reopen it."
            isNoRoom ->
                "NO ROOM — ${pointsCaptured} returns arrived and none of them were placed in " +
                    "space, so this file has a sensor recording and no map. Start a new scan: " +
                    "point the rear camera at something with texture for a couple of seconds " +
                    "and wait for \"Tracking steady\" before you press Start."
            isTwoDimensionalOnly ->
                "2D ONLY — the camera never told this scan where it was, so the " +
                    "${pointsCaptured} returns have no positions and there is no room in this " +
                    "file. Nothing can recover it on the phone. Start a new scan: point the " +
                    "rear camera at something with texture and detail for a couple of seconds " +
                    "before you press Start, and wait for \"Tracking steady\"."
            sections > MAX_SECTIONS_FAIR ->
                // ROUND 13. The old sentence said "tracking restarted", which is
                // not what happens, and "Rescan", which does not tell the
                // operator what to do differently. Measured against the recorded
                // gyro, every one of the owner's section breaks is ARCore
                // RE-ANCHORING — the phone rotated under 1.2 deg while the pose
                // stream jumped 8-13 deg — so it is the camera losing confidence
                // in where it is, and the two things that change that are light
                // and texture in front of the lens.
                // ROUND 20: reworded to the MEASURED diet (round 18: close
                // feature-poor surfaces and fast turns, in good light) — the
                // owner's correction is on record and light is not mentioned.
                "$sections sections — the camera re-anchored $breaks times, so the room was " +
                    "rebuilt from scratch that often. Check nothing is covering the rear camera, " +
                    "keep about an arm's length or more from blank close surfaces, and ease " +
                    "through the turns. Process this scan to stitch the pieces back together."
            trackingDrops > MAX_DROPS_FAIR ->
                "$trackingDrops tracking drops — the camera stopped knowing where it was, and " +
                    "those seconds are holes in the room. Keep the rear camera clear, stay an " +
                    "arm's length from blank close surfaces, and ease through the turns."
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
     * ROUND 16 item 58 — the word on the card, which is not always the grade.
     *
     * A 2D-only capture is POOR, but "POOR" invites "rescan and it will be
     * better", and this one will not be better for anything the operator does
     * differently about walking speed or coverage. The card says what it is.
     */
    val headline: String
        get() = when {
            // ROUND 17 item 64. Same argument as ROUND 16's: "POOR" invites
            // "rescan and it will be better", and neither of these gets better
            // for anything the operator does differently about walking.
            engineStartFailed -> "NOT RECORDED"
            isNoRoom -> "NO ROOM"
            isTwoDimensionalOnly -> "2D ONLY"
            else -> grade.name
        }

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
            // ROUND 17 item 64.
            engineStartFailed ->
                "Press Start once and wait — the button takes a few seconds to arm while the " +
                    "camera settles. Pressing it twice starts nothing and stops the scan that " +
                    "was already running."
            isNoRoom ->
                "Before the next scan: give the rear camera a lit, textured surface to look at " +
                    "and let the Start button say \"Tracking steady\" before you press it."
            isTwoDimensionalOnly ->
                "Before the next scan: give the rear camera a lit, textured surface to look at " +
                    "and let the Start button say \"Tracking steady\" before you press it. If it " +
                    "never does, close the app and reopen it."
            // ROUND 19 (owner correction on record): NOT light. Round 18
            // measured every long loss on this rig at ~1 m from feature-poor
            // surfaces (63-70 % of returns under 1.5 m) in good light, and the
            // one short loss in a 127 deg/5 s turn. The advice names what was
            // measured and nothing else.
            breaks >= 2 ->
                "Before the next walk: keep about an arm's length or more from blank close " +
                    "surfaces and ease through the turns — every long tracking loss this rig has " +
                    "measured happened close to a feature-poor wall or in a fast turn."
            breaks == 1 ->
                "Before the next walk: give the camera more to look at through turns — corners and " +
                    "furniture rather than blank wall, from a step further back."
            trackingDrops > 0 ->
                "Before the next walk: a little more distance from blank close surfaces. The " +
                    "camera lost tracking $trackingDrops time" +
                    (if (trackingDrops == 1) "" else "s") +
                    " — the measured cause on this rig is close feature-poor walls and fast " +
                    "turns."
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
            // ROUND 19 item 75: rooms rarely close because of COVERAGE, not
            // math — only scan-029 ever closed a loop. When the coverage grid
            // measured a thin arc, naming it beats a generic pace note.
            coverageAdvice != null -> coverageAdvice
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
