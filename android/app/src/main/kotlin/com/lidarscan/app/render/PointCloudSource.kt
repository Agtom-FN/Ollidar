package com.lidarscan.app.render

import com.lidarscan.app.engine.NativePointPage
import com.lidarscan.app.engine.ScanEngineNative

/**
 * What [com.lidarscan.app.render.PointCloudRenderer] reads pages from. Two
 * implementations below — one over the live capture engine's `scan_engine*`
 * handle, one over B4's standalone replay engine — so the renderer, and the
 * Capture screen around it, do not need to know or care which one is
 * driving the view. Mirrors desktop's `PagedCloudRenderer::sync()` polling
 * approach (see its header comment): enumerate page ids fresh every frame
 * and diff against what is already on the GPU, rather than trust an
 * incremental event stream that can drop under backpressure.
 */
interface PointCloudSource {
    /** True while this source has a live/replay engine handle to read from at all. */
    val isAvailable: Boolean

    fun pageCount(): Int
    fun pageIdAt(index: Int): Int
    fun getPage(pageId: Int): NativePointPage?
    fun totalPoints(): Long
}

/** Reads pages from the live capture engine (B2's `scan_engine*`, via the C ABI). */
class LiveEngineCloudSource(private val engineHandleProvider: () -> Long) : PointCloudSource {
    override val isAvailable: Boolean get() = engineHandleProvider() != 0L

    override fun pageCount(): Int {
        val h = engineHandleProvider()
        return if (h == 0L) 0 else ScanEngineNative.nativePageCount(h)
    }

    override fun pageIdAt(index: Int): Int {
        val h = engineHandleProvider()
        return if (h == 0L) -1 else ScanEngineNative.nativePageIdAt(h, index)
    }

    override fun getPage(pageId: Int): NativePointPage? {
        val h = engineHandleProvider()
        return if (h == 0L) null else ScanEngineNative.nativeGetPointPage(h, pageId)
    }

    override fun totalPoints(): Long {
        val h = engineHandleProvider()
        return if (h == 0L) 0L else ScanEngineNative.nativeTotalPoints(h)
    }
}

/** Reads pages from B4's standalone replay engine (`replay_engine.h`'s `ReplayEngine`). */
class ReplayEngineCloudSource(private val replayHandleProvider: () -> Long) : PointCloudSource {
    override val isAvailable: Boolean get() = replayHandleProvider() != 0L

    override fun pageCount(): Int {
        val h = replayHandleProvider()
        return if (h == 0L) 0 else ScanEngineNative.nativeReplayPageCount(h)
    }

    override fun pageIdAt(index: Int): Int {
        val h = replayHandleProvider()
        return if (h == 0L) -1 else ScanEngineNative.nativeReplayPageIdAt(h, index)
    }

    override fun getPage(pageId: Int): NativePointPage? {
        val h = replayHandleProvider()
        return if (h == 0L) null else ScanEngineNative.nativeReplayGetPointPage(h, pageId)
    }

    override fun totalPoints(): Long {
        val h = replayHandleProvider()
        return if (h == 0L) 0L else ScanEngineNative.nativeReplayTotalPoints(h)
    }
}

/**
 * B6/B11: reads pages from the **processing** engine — the standalone
 * `scanengine::Engine` `cpp/processing_engine.h` owns, into whose PageStore
 * every post-process/colorize job publishes. Same `NativePointPage`
 * marshalling as the live and replay paths, so the Review screen's viewer is
 * B4's renderer unchanged.
 */
class ProcessingCloudSource(private val handleProvider: () -> Long) : PointCloudSource {
    override val isAvailable: Boolean get() = handleProvider() != 0L

    override fun pageCount(): Int {
        val h = handleProvider()
        return if (h == 0L) 0 else ScanEngineNative.nativeProcPageCount(h)
    }

    override fun pageIdAt(index: Int): Int {
        val h = handleProvider()
        return if (h == 0L) -1 else ScanEngineNative.nativeProcPageIdAt(h, index)
    }

    override fun getPage(pageId: Int): NativePointPage? {
        val h = handleProvider()
        return if (h == 0L) null else ScanEngineNative.nativeProcGetPointPage(h, pageId)
    }

    override fun totalPoints(): Long {
        val h = handleProvider()
        return if (h == 0L) 0L else ScanEngineNative.nativeProcTotalPoints(h)
    }
}

/**
 * B12: the merged cloud's own store. A13 gives a merged product its own
 * `PageStore` ("the store's pages are shared with whatever else is in it, so a
 * merged product normally gets its own store"), so it needs its own source
 * rather than appearing mixed into [ProcessingCloudSource]'s pages.
 */
class MergedCloudSource(private val handleProvider: () -> Long) : PointCloudSource {
    override val isAvailable: Boolean get() = handleProvider() != 0L && totalPoints() > 0

    override fun pageCount(): Int {
        val h = handleProvider()
        return if (h == 0L) 0 else ScanEngineNative.nativeProcMergedPageCount(h)
    }

    override fun pageIdAt(index: Int): Int {
        val h = handleProvider()
        return if (h == 0L) -1 else ScanEngineNative.nativeProcMergedPageIdAt(h, index)
    }

    override fun getPage(pageId: Int): NativePointPage? {
        val h = handleProvider()
        return if (h == 0L) null else ScanEngineNative.nativeProcMergedGetPointPage(h, pageId)
    }

    override fun totalPoints(): Long {
        val h = handleProvider()
        return if (h == 0L) 0L else ScanEngineNative.nativeProcMergedTotalPoints(h)
    }
}

/**
 * B11: a bounded sample of a source's points, in world coordinates, for the
 * measure tool's screen-space pick.
 *
 * **Bounded on purpose.** A processed cloud is millions of points and the pick
 * runs per tap on a coroutine; `maxPoints` caps the copy, striding through each
 * page so the sample is spread over the whole cloud rather than being its first
 * N points. The consequence is stated where it matters: the measure readout
 * says a pick is "nearest sampled point", because at a 200k sample of a 5M
 * cloud the tapped return may be up to a few centimetres from the one actually
 * drawn under the finger.
 */
fun PointCloudSource.samplePoints(
    maxPoints: Int,
    /**
     * ROUND 8: which `SCAN_STREAM_*` pages to sample. Defaults to all, which is
     * right for the measure tool (Review holds exactly one resolved cloud) and
     * WRONG for a live D6 capture, whose store holds the resolved world-frame
     * map and the raw sensor-frame fan at the same time. See
     * [com.lidarscan.core.render.PreviewSanity] for what sampling both did to
     * the Projects-tab thumbnail, measured on a real export.
     */
    acceptStream: (Int) -> Boolean = { true },
): List<com.lidarscan.core.measure.Vec3> {
    if (!isAvailable) return emptyList()
    val total = totalPoints()
    if (total <= 0L) return emptyList()
    val stride = maxOf(1, (total / maxPoints.coerceAtLeast(1)).toInt())
    val out = ArrayList<com.lidarscan.core.measure.Vec3>(minOf(maxPoints, total.toInt()))
    val pages = pageCount()
    for (i in 0 until pages) {
        val id = pageIdAt(i)
        if (id < 0) continue
        val page = getPage(id) ?: continue
        if (!acceptStream(page.stream)) continue
        // `NewDirectByteBuffer` hands back a BIG_ENDIAN buffer regardless of
        // the platform — a JNI/NIO default, not an engine choice. The renderer
        // never noticed because `setBufferAt` copies raw bytes; `getFloat`
        // absolutely would, silently producing coordinates in the 1e-40 range.
        // Duplicating rather than mutating: the page buffer is shared with the
        // renderer, and flipping its order under it would be a lovely bug.
        val buf = page.buffer.duplicate().order(java.nio.ByteOrder.LITTLE_ENDIAN)
        var p = 0
        while (p < page.count) {
            val off = p * 16
            if (off + 12 <= buf.limit()) {
                out.add(
                    com.lidarscan.core.measure.Vec3(
                        buf.getFloat(off),
                        buf.getFloat(off + 4),
                        buf.getFloat(off + 8),
                    ),
                )
            }
            p += stride
        }
    }
    return out
}

/**
 * ROUND 8: the `SCAN_STREAM_*` ids this source currently has pages for.
 *
 * Cheap (page headers only, no buffer reads) and used for exactly one
 * decision: a preview or an export sample must take the RESOLVED map when one
 * exists and the raw sensor-frame preview only when it does not. Asking the
 * store rather than inferring it from `liveSlam`/`pushbroomActive` flags is
 * deliberate — those say what was REQUESTED, and the pages say what happened.
 */
fun PointCloudSource.streamsPresent(): Set<Int> {
    if (!isAvailable) return emptySet()
    val out = mutableSetOf<Int>()
    for (i in 0 until pageCount()) {
        val id = pageIdAt(i)
        if (id < 0) continue
        val page = getPage(id) ?: continue
        if (page.count > 0) out.add(page.stream)
    }
    return out
}

/**
 * Optional capability an [com.lidarscan.core.engine.EngineBridge]
 * implementation can advertise: "I have a native point-page source for the
 * live 3D view." [com.lidarscan.core.engine.FakeEngineBridge] deliberately
 * does not implement this (no native engine exists behind it) — the Capture
 * screen checks `engineBridge as? NativePointCloudProvider` and shows a
 * "3D view needs the real engine" placeholder instead of a crash when it is
 * absent.
 */
interface NativePointCloudProvider {
    /** Null when there is nothing to read from yet (not connected / replay not started). */
    fun currentPointCloudSource(): PointCloudSource?
}
