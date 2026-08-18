package com.lidarscan.core.capture

import kotlin.math.roundToInt

/**
 * ROUND 13 — what "Process this scan" did, in numbers and then in words.
 *
 * Lives in `:core` and takes plain `Double`s so the whole thing is unit
 * testable without a device: the flat `DoubleArray(16)` the JNI returns is
 * decoded in exactly one place ([fromNative]) and everything the operator
 * reads is derived here.
 *
 * ## Why the headline number is the VERTICAL EXTENT
 *
 * It is the only quantity in the report that does not come from the same
 * measurement that produced the correction. The operator walks on a flat
 * floor, so the trajectory's spread along gravity is bounded by the reach of
 * an arm — and on the owner's scan-030 the stitch takes it from 0.82 m to
 * 0.27 m. Applying the correction the other way round takes it to 1.55 m. It
 * is an independent check that the map got *righter*, not merely *different*,
 * and it is expressible in one clause the owner can act on.
 *
 * The start-to-end gap is deliberately NOT sold as an improvement. Before
 * stitching it compares two points in different world frames and means
 * nothing; after, it is ARCore's own drift, which this feature does not fix
 * and must not appear to.
 */
data class StitchResult(
    val ran: Boolean,
    val mapWritten: Boolean,
    val sections: Int,
    val seams: Int,
    val seamsRefined: Int,
    val points: Long,
    val poses: Long,
    val posesUntracked: Long,
    val movedMeters: Double,
    val movedDegrees: Double,
    val verticalExtentBeforeM: Double,
    val verticalExtentAfterM: Double,
    val endGapBeforeM: Double,
    val endGapAfterM: Double,
    val mountVerdict: MountVerdict,
    val mountImpossibleFraction: Double,
) {
    val changedAnything: Boolean get() = ran && sections > 1 && mapWritten

    /** The one line the operator reads. Never null once [ran]. */
    val headline: String
        get() = when {
            !ran -> "This scan could not be processed."
            sections <= 1 ->
                "Nothing to align — this scan was recorded in one piece."
            !mapWritten -> "Processed, but no points came back. The scan may be empty."
            else ->
                "$sections pieces aligned — height spread " +
                    "%.2f → %.2f m".format(verticalExtentBeforeM, verticalExtentAfterM) + "."
        }

    /** The supporting sentence, or null when there is nothing honest to add. */
    val detail: String?
        get() {
            if (!ran || sections <= 1) return null
            val moved = "The first piece moved %.2f m to meet the last.".format(movedMeters)
            val gap = if (endGapAfterM > 0.15) {
                // ONE string, ONE format call. Written the other way round on
                // the first attempt — `.format()` bound to the second fragment
                // and this shipped a literal "%.0f" — which is the exact bug
                // ROUND 13 fixed in three other places, caught here by the
                // test written for those.
                (
                    " Your walk still ends %.0f cm from where it began — that is the camera's " +
                        "own drift over the walk, and aligning the pieces does not remove it."
                ).format(endGapAfterM * 100)
            } else {
                ""
            }
            val refined = when {
                seamsRefined == seams && seams > 0 -> " All $seams joins were measured against the map."
                seamsRefined > 0 -> " $seamsRefined of $seams joins were measured against the map; " +
                    "the rest kept the camera's own correction because the walls there could not " +
                    "measure one."
                else -> " The joins kept the camera's own correction — the walls here could not " +
                    "measure a better one."
            }
            return moved + refined + gap
        }

    /** ROUND 13 item 48: shown only when the mount looks wrong. */
    val mountWarning: String?
        get() = when (mountVerdict) {
            MountVerdict.MISMATCH ->
                "The lidar was not where the mount reference says it is: " +
                    "${(mountImpossibleFraction * 100).roundToInt()}% of returns landed at " +
                    "heights no room has. Re-zero before the next scan."
            MountVerdict.SUSPECT ->
                "Each sweep covers an unusually tall space. If you were indoors, check the puck " +
                    "is seated and re-zero."
            MountVerdict.OK, MountVerdict.NOT_MEASURABLE -> null
        }

    companion object {
        /** Slot layout is pinned by `processing_jni.cpp`'s documented table. */
        fun fromNative(v: DoubleArray?): StitchResult? {
            if (v == null || v.size < 16) return null
            return StitchResult(
                ran = v[0] != 0.0,
                mapWritten = v[1] != 0.0,
                sections = v[2].toInt(),
                seams = v[3].toInt(),
                seamsRefined = v[4].toInt(),
                points = v[5].toLong(),
                movedMeters = v[6],
                verticalExtentBeforeM = v[7],
                verticalExtentAfterM = v[8],
                endGapBeforeM = v[9],
                endGapAfterM = v[10],
                movedDegrees = v[11],
                mountVerdict = MountVerdict.of(v[12].toInt()),
                mountImpossibleFraction = v[13],
                poses = v[14].toLong(),
                posesUntracked = v[15].toLong(),
            )
        }
    }
}

/** Mirrors `SCAN_MOUNT_*` / `post::MountWatchVerdict`. */
enum class MountVerdict {
    OK,
    NOT_MEASURABLE,
    SUSPECT,
    MISMATCH,
    ;

    companion object {
        fun of(v: Int): MountVerdict = when (v) {
            0 -> OK
            2 -> SUSPECT
            3 -> MISMATCH
            else -> NOT_MEASURABLE
        }
    }
}
