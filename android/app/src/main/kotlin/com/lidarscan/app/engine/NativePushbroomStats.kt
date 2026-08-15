package com.lidarscan.app.engine

/**
 * Mirrors `scan_pushbroom_stats` (`scanengine_c.h`), constructed from JNI via
 * a cached constructor (`(JJJJJJJJJJJJJ)V` — `arcore_jni.cpp`'s
 * `nativePushbroomStats`).
 *
 * The three `flagged*` counters are broken out rather than summed because
 * A8 §3.2 is explicit that they mean different things to a user: tracking
 * lost is "walk back and rescan", stale is "your pose stream stuttered", low
 * confidence is "ARCore was struggling". The capture screen surfaces them
 * that way.
 */
data class NativePushbroomStats(
    val pointsIn: Long,
    val pointsOut: Long,
    val pointsPending: Long,
    val droppedRange: Long,
    val droppedNoPose: Long,
    val droppedOverflow: Long,
    val droppedPageFull: Long,
    val flaggedTrackingLost: Long,
    val flaggedStalePose: Long,
    val flaggedLowConfidence: Long,
    val flaggedEmitted: Long,
    val tFirstNs: Long,
    val tLastNs: Long,
) {
    val droppedTotal: Long
        get() = droppedRange + droppedNoPose + droppedOverflow + droppedPageFull

    /** Short user-facing reason for the dominant flag class, or null when nothing is flagged. */
    fun dominantFlagReason(): String? = when {
        flaggedTrackingLost == 0L && flaggedStalePose == 0L && flaggedLowConfidence == 0L -> null
        flaggedTrackingLost >= flaggedStalePose && flaggedTrackingLost >= flaggedLowConfidence ->
            "AR tracking was lost — walk back and rescan that area"
        flaggedStalePose >= flaggedLowConfidence -> "The pose stream stuttered"
        else -> "AR tracking was struggling (low confidence)"
    }
}
