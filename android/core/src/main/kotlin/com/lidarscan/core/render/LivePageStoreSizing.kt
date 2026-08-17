package com.lidarscan.core.render

import com.lidarscan.core.capture.DeviceTier

/**
 * ROUND 6, owner item 21 ("its bearly maping the 3d slam for d6") — **how the
 * live point store is sized on a phone, and why the default was wrong.**
 *
 * ### The mechanism
 *
 * `engine/include/scanengine/cloud/page_store.h` defaults to
 * `page_capacity = 1 << 20` points and `max_pages = 64`. A `PointVertex` is
 * 16 bytes, so that is **16 MB per page and a 1 GB ceiling** — sized for a
 * desktop. `RealEngineBridge` passed `0, 0` (meaning "engine defaults") from B2
 * until now, so a phone got the desktop numbers.
 *
 * That would be merely wasteful if pages filled up. They do not, because of one
 * line in `PageStore::append()`:
 *
 * ```cpp
 *   const bool need_page = tail == nullptr
 *       || tail->count >= tail->capacity
 *       || tail->stream != stream;   // pages are single-stream
 * ```
 *
 * Only the **tail** page is ever appended to, and a page belongs to exactly one
 * stream. A D6 capture has two producers publishing continuously and
 * alternately — the driver's raw sensor-frame preview on `kLidarD6`, and A8's
 * pushbroom on `kSlamMap` (`engine_pushbroom_defaults()` sets `out_stream =
 * kSlamMap`). **Every alternation allocates a fresh 16 MB page for the ~4096
 * points of one pushbroom batch.** Utilisation is well under 1 %, and the 64-page
 * ceiling is reached after a few dozen alternations — around a minute of walking.
 *
 * And when the store is full, `append()` returns `kCapacityExceeded` and stores
 * **nothing, forever**: the header says so outright ("When full it appends
 * nothing… A14 replaces the cap with an LOD/eviction policy"). So the live map
 * grows for a minute and then silently stops, having allocated up to a gigabyte
 * to hold a couple of hundred thousand points. "Barely mapping", exactly.
 * (The same store filling was independently proven on the desktop shell in this
 * same round — `page store full (64 pages): dropped N points`, 1400× in one
 * session.)
 *
 * ### The Android-side fix
 *
 * `scan_engine_config` already exposes both numbers and the JNI already
 * marshals them (`scanengine_jni.cpp`'s `nativeCreateEngine`), so this needs no
 * engine change at all — just for the app to stop asking for desktop defaults.
 * Small pages make an alternation cost ~1 MB instead of 16 MB, and a higher page
 * count buys back the headroom that costs:
 *
 * | tier | page | pages | resident ceiling | worst-case bytes |
 * | --- | --- | --- | --- | --- |
 * | modest | 32 k pts | 192 | 6.3 M pts | 96 MB |
 * | standard | 64 k pts | 256 | 16.8 M pts | 256 MB |
 * | flagship | 128 k pts | 192 | 25.2 M pts | 384 MB |
 *
 * ### What this still does NOT fix
 *
 * A long enough walk fills any bounded store, and the engine's policy on full is
 * still "drop everything from here on". **Eviction is an engine change** and
 * the engine tree is read-only for this task — a concurrent task owns it. Until that
 * seam exists the honest thing is to *say* the live map stopped growing, which
 * [com.lidarscan.core.render.LivePageStoreSizing.fullNote] is for, and to be
 * clear that it costs the *preview* and never the capture: record-always writes
 * the raw streams straight to the `.lscan`, and post-processing reads those.
 */
data class LivePageStoreSizing(
    /** Points per page. Smaller pages waste far less on a stream alternation. */
    val pageCapacityPoints: Int,
    /** Hard page ceiling. `PageStore` stops accepting points once this many pages exist. */
    val maxPages: Int,
) {
    /** Points the store can hold if every page fills — the optimistic ceiling. */
    val residentPointCeiling: Long get() = pageCapacityPoints.toLong() * maxPages

    /** Bytes the store occupies with every page allocated. `PointVertex` is 16 bytes, pinned by a static_assert. */
    val worstCaseBytes: Long get() = residentPointCeiling * BYTES_PER_POINT

    companion object {
        const val BYTES_PER_POINT = 16L

        /** What the engine would use if the app passed `0, 0` — a desktop's numbers. */
        val ENGINE_DEFAULT = LivePageStoreSizing(pageCapacityPoints = 1 shl 20, maxPages = 64)

        fun forTier(tier: DeviceTier): LivePageStoreSizing = when (tier) {
            DeviceTier.MODEST -> LivePageStoreSizing(pageCapacityPoints = 32 * 1024, maxPages = 192)
            DeviceTier.STANDARD -> LivePageStoreSizing(pageCapacityPoints = 64 * 1024, maxPages = 256)
            DeviceTier.FLAGSHIP -> LivePageStoreSizing(pageCapacityPoints = 128 * 1024, maxPages = 192)
        }

        /**
         * The inline line shown when the live store has filled. One sentence, and
         * the second half is the part that matters: the capture is untouched.
         */
        fun fullNote(sizing: LivePageStoreSizing): String {
            val millions = sizing.residentPointCeiling / 1_000_000.0
            return "Live map is full (%.1f M points) and has stopped growing — the phone's preview buffer, not the scan. "
                .format(millions) +
                "Recording is unaffected; processing the project afterwards uses every point."
        }
    }
}
