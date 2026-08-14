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
