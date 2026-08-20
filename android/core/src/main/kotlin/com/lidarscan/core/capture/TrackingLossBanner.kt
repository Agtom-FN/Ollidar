package com.lidarscan.core.capture

import com.lidarscan.core.Wording

/**
 * ROUND 23 item 105 — **stop walking while the tracker is blind.**
 *
 * The owner's request, verbatim: *"a warning need to tell user stop walking
 * while tracking lost until the tracking back."* He is right, and his own
 * scan-070 is the arithmetic behind it. The 4.1 s gap at 12:02:22 was refused
 * by the ROUND 19 gyro gate with `gyro=73.34deg` against a reported
 * `12.70deg`: the phone turned through seventy-three degrees while ARCore had
 * no idea where it was. A gap that big cannot be healed by anything — not the
 * bridge, not the rescue, not the recovery — because there is no shared
 * geometry left on the two sides of it to agree about. Standing still through
 * a loss keeps the gap small enough to close; walking through it is what turns
 * a two-second stumble into a permanently broken map.
 *
 * Nothing on the screen said so. The tracking chip went amber and the cue
 * channel buzzed (`TRACKING_DEGRADED`), which tells the operator that
 * something is wrong but not what to DO about it, and doing the wrong thing is
 * free.
 *
 * ## Why a state machine in `:core`
 *
 * The banner is three states and two edges, and every interesting part of it
 * is timing — how long the green confirmation lingers, whether a flicker in
 * and out of tracking produces one banner or six, what happens when the
 * recording stops mid-loss. That is exactly the shape `OperatorCues` put in
 * `:core` in ROUND 11 and for the same reason: a unit test can hold time
 * still, and an emulator cannot.
 */
enum class TrackingBanner {
    /** Tracking is fine (or nothing is being recorded). Draw nothing. */
    NONE,

    /** Blind, right now. Amber, full width, and it does not go away by itself. */
    LOST,

    /** Tracking came back within the last [TrackingLossBanners.REGAINED_LINGER_MS]. Green. */
    REGAINED,
}

/**
 * [banner] plus the instant it was entered, which is all the display needs:
 * the amber banner counts up from [sinceMillis], and the green one expires
 * against it.
 */
data class TrackingBannerState(
    val banner: TrackingBanner = TrackingBanner.NONE,
    val sinceMillis: Long = 0L,
) {
    val isLost: Boolean get() = banner == TrackingBanner.LOST
}

object TrackingLossBanners {

    /**
     * How long "OK — keep walking." stays up.
     *
     * Two seconds: long enough to be read by someone who has just looked back
     * at the phone, short enough that it is gone before it becomes furniture.
     * The owner walks with the phone at hip height, so a confirmation that
     * lingers is a confirmation he will see attached to the wrong moment.
     */
    const val REGAINED_LINGER_MS = 2_000L

    /**
     * The next banner state.
     *
     * [recording] gates the whole thing: a preview that has not been started
     * is not losing anything, and shouting at someone who is lining up a shot
     * is how a warning gets ignored when it matters. Stopping mid-loss clears
     * the banner rather than freezing it on screen — the seal's own summary is
     * the thing to read at that point.
     */
    fun next(
        previous: TrackingBannerState,
        recording: Boolean,
        tracking: Boolean,
        nowMillis: Long,
    ): TrackingBannerState = when {
        !recording -> TrackingBannerState()
        !tracking -> if (previous.banner == TrackingBanner.LOST) {
            previous
        } else {
            TrackingBannerState(TrackingBanner.LOST, nowMillis)
        }
        previous.banner == TrackingBanner.LOST ->
            TrackingBannerState(TrackingBanner.REGAINED, nowMillis)
        previous.banner == TrackingBanner.REGAINED ->
            if (nowMillis - previous.sinceMillis >= REGAINED_LINGER_MS) {
                TrackingBannerState()
            } else {
                previous
            }
        else -> TrackingBannerState()
    }

    /** True exactly on the edge that must buzz and shout. */
    fun becameLost(previous: TrackingBannerState, next: TrackingBannerState): Boolean =
        next.banner == TrackingBanner.LOST && previous.banner != TrackingBanner.LOST

    /** True exactly on the edge that gets the light tick and the green line. */
    fun becameRegained(previous: TrackingBannerState, next: TrackingBannerState): Boolean =
        next.banner == TrackingBanner.REGAINED && previous.banner != TrackingBanner.REGAINED

    /** The instruction on the amber banner. Five words, item 98's law. */
    val LOST_TEXT: String get() = Wording.TRACKING_LOST

    /** The green one. Three words. */
    val REGAINED_TEXT: String get() = Wording.TRACKING_BACK

    /**
     * The one detail line under the amber banner: how long this has been going
     * on. Seconds, whole, because tenths on a banner read at walking pace are
     * noise.
     */
    fun lostDetail(elapsedMillis: Long): String = Wording.trackingLostFor(elapsedMillis / 1000L)
}
