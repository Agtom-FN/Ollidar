package com.lidarscan.app.render

import com.lidarscan.app.engine.ScanEngineNative

/**
 * Which point streams [PointCloudRenderer] draws.
 *
 * ## Why this exists (B3, and it is a fix, not a feature)
 *
 * B4's renderer polled `pageCount()`/`pageIdAt()`/`getPage()` and drew
 * **every page it found**, with no reference to `scan_point_page.stream` at
 * all. For a D6 record-only session that is right — there is exactly one
 * point stream — and it stayed right through B7/B8, because the D6's
 * pushbroom output only appears when pushbroom is explicitly enabled.
 *
 * It stops being right the moment a Mid-360 runs with `live_slam = true`.
 * Then the engine's one `PageStore` holds **two** point streams at once
 * (INT24-wiring.md §2):
 *
 *  * `SCAN_STREAM_LIDAR_MID360` (2) — the driver's sensor-frame live preview,
 *    every point in the device's own frame; and
 *  * `SCAN_STREAM_SLAM_MAP` (8) — A6's registered world-frame map, which is
 *    also where A8's assembled pushbroom cloud goes.
 *
 * Drawing both puts two versions of the same room on screen — one that
 * rotates with the sensor and one that does not — superimposed. The symptom
 * is not "points are missing", it is "the cloud smears and doubles as you
 * walk", which is far harder to attribute.
 *
 * So the task's question — "verify stream filtering doesn't drop kSlamMap
 * pages" — has an answer with a twist: **nothing was dropping them, because
 * nothing was filtering at all.** `kSlamMap` pages did reach the renderer.
 * The bug was the opposite one, and this is its fix.
 *
 * ## The policy
 *
 * [MAPPED_ONLY] when live SLAM is on, [RAW_ONLY] when it is not, chosen by
 * `CaptureViewModel` from the same `liveSlam` flag that goes into
 * `scan_session_config.live_slam`. [ALL] exists for the replay path and for a
 * deliberate "show me everything" debug view — a session that produces only
 * one stream renders identically under any of the three.
 *
 * [MAPPED_ONLY] deliberately falls back to drawing raw pages **until the
 * first mapped page exists**. Live SLAM can take a second or two to
 * initialise its ESKF, and it is allowed to fail entirely without failing the
 * session (INT24 §2: "Live SLAM failing to start does not fail the session")
 * — a strict map-only filter would show a black screen in exactly the case
 * where the operator most needs to see that points are arriving.
 */
enum class StreamFilter {
    /** Every stream. Replay, debug, and any single-stream session. */
    ALL,

    /** Sensor-frame lidar only — the Record-only live preview. */
    RAW_ONLY,

    /** The registered world-frame map, falling back to raw until a mapped page exists. */
    MAPPED_ONLY,
    ;

    /**
     * @param mappedSeen whether a `SCAN_STREAM_SLAM_MAP` page has been
     *   observed yet this session. Passed in rather than held here because
     *   enum entries are process-wide singletons — storing it on the enum
     *   would leak one session's state into the next one.
     */
    fun accepts(stream: Int, mappedSeen: Boolean): Boolean = when (this) {
        ALL -> true
        RAW_ONLY -> stream != ScanEngineNative.StreamId.SLAM_MAP
        MAPPED_ONLY -> stream == ScanEngineNative.StreamId.SLAM_MAP || !mappedSeen
    }

    companion object {
        /** The policy above, as one call: this is what the Capture screen uses. */
        fun forSession(liveSlam: Boolean): StreamFilter = if (liveSlam) MAPPED_ONLY else RAW_ONLY
    }
}
