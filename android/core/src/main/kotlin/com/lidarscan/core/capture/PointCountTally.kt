package com.lidarscan.core.capture

/**
 * ROUND 11 — the point counter that was showing roughly double the truth.
 *
 * ROUND 10 measured it and put it on the backlog: the owner's scan-020 sealed
 * with `points=584315` in the capture log, while re-resolving the very same
 * container through the production pipeline yields **293,166** world points and
 * the pushbroom's own accounting says 293,524 returns in, 293,166 out, **zero
 * dropped**. The counter was not double-counting a bug in the engine — it was
 * adding two different, both-correct streams together.
 *
 * ## Why there are two streams
 *
 * During a D6 capture the engine's one `PageStore` carries the same returns
 * twice, on purpose:
 *
 *  * the **raw sensor-frame preview** (`SCAN_STREAM_LIDAR_D6`) — the flat 2 D
 *    fan the driver publishes so a device can be proved alive before any pose
 *    exists;
 *  * the **resolved world-frame map** (`SCAN_STREAM_SLAM_MAP`) — the same
 *    returns after the pushbroom assembler has placed them with the trajectory.
 *
 * `SCAN_EVENT_POINTS_AVAILABLE` fires once per page-append **per stream** and
 * carries the stream id in its payload. `RealEngineBridge` was summing the
 * count field and ignoring the id, so every return was counted once as a raw
 * fan point and again as a map point. `StreamFilter` has drawn only one of them
 * since B3 and `writeProjectPreview` has preferred the map since ROUND 8; the
 * counter was the last consumer that still believed there was one stream.
 *
 * ## What "points" should mean to the operator
 *
 * The number on the screen is the answer to "how much of the room have I got",
 * so it is the **resolved map**: raw fan points that never found a pose are not
 * in the room. So this reports the mapped count as soon as any mapped point has
 * arrived, and the raw count before that (a rig whose pushbroom is off, or the
 * first ~100 ms of a capture before the first batch closes — ROUND 10 bounded
 * that batch at 100 ms of point time, so the switch-over is immediate in
 * practice and the transition is invisible).
 *
 * ## Why the roles are an enum and not stream ids
 *
 * `EngineBridge`'s own KDoc states the rule this file follows: the numeric
 * `SCAN_STREAM_*` space belongs to the C ABI, and `:core` "has no dependency on
 * and should not re-encode" it. `:app` owns the mapping from id to role, in one
 * place, next to the other `ScanEngineNative` constants.
 *
 * Not thread-safe by itself; `RealEngineBridge` touches it only from the native
 * event-pump thread and from [reset] between sessions.
 */
enum class PointStreamRole {
    /** The driver's sensor-frame preview — real returns, not yet placed in the world. */
    RAW_SENSOR,

    /** The registered world-frame cloud: the pushbroom map, or A6's live SLAM map. */
    RESOLVED_MAP,

    /** Anything else the store may carry (a colorize re-publish, a merged cloud). */
    OTHER,
}

class PointCountTally {
    var rawPoints: Long = 0L
        private set

    var mappedPoints: Long = 0L
        private set

    var otherPoints: Long = 0L
        private set

    /** True once any resolved-map point has been seen, which is what switches the report over. */
    val mappedSeen: Boolean get() = mappedPoints > 0L

    /**
     * The number to show, to log, and to write into the manifest as
     * `pointCountEstimate`.
     */
    val points: Long get() = if (mappedSeen) mappedPoints else rawPoints

    fun add(role: PointStreamRole, count: Long) {
        if (count <= 0L) return
        when (role) {
            PointStreamRole.RAW_SENSOR -> rawPoints += count
            PointStreamRole.RESOLVED_MAP -> mappedPoints += count
            PointStreamRole.OTHER -> otherPoints += count
        }
    }

    fun reset() {
        rawPoints = 0L
        mappedPoints = 0L
        otherPoints = 0L
    }

    /**
     * For the capture log. Both halves, always, so the next field report can be
     * read without anyone having to re-derive which number was which — the
     * absence of exactly this line is what let the doubled count survive ten
     * rounds.
     */
    fun logSuffix(): String = "points=$points (map=$mappedPoints raw=$rawPoints other=$otherPoints)"
}
