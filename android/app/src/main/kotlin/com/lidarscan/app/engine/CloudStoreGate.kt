package com.lidarscan.app.engine

import java.util.concurrent.locks.ReentrantReadWriteLock

/**
 * ROUND 39 item 191 — **"consumers must stop reading first", made enforceable.**
 *
 * `PageStore::clear()` says exactly what it costs, in the engine's own header
 * (`engine/include/scanengine/cloud/page_store.h`):
 *
 * > *Drop all points. Pages and their buffers are freed; **every PageView taken
 * > before this call becomes dangling, so consumers must stop reading first**
 * > (the Engine only calls this between sessions).*
 *
 * On Android that precondition had nothing holding it up. The processing
 * engine's store is **process-wide** — one `ProcessingRepository`, one native
 * handle, for the life of the app — and opening a saved scan in Review calls
 * `open_recorded_cloud`, whose first act is `clear_cloud()`. So the second scan
 * an operator opens frees the first scan's pages **on `Dispatchers.IO`**, while
 * `PointCloudRenderer` is enumerating and uploading those very pages on the
 * choreographer thread. That is the round-38 crash: `SIGSEGV`/`SEGV_MAPERR` in
 * `__memcpy_aarch64_simd` under `glBufferSubData`, reading a page buffer whose
 * backing store had been unmapped.
 *
 * This is the missing half of the contract. A store that can be cleared under a
 * live reader hands out a gate; the reader takes [read] around **every** touch
 * of a page — enumerating ids, reading `page.buffer`, copying bytes out of it —
 * and the clear takes [mutate]. Nothing else changes about the store: appends
 * still race freely with readers, which is the contract's *"append() is safe
 * from any number of producer threads"* and is what a live capture has always
 * done. Only the free is serialised, because only the free invalidates.
 *
 * ## [epoch] is the other half, and it is not a nicety
 *
 * The crash was the loud failure. The quiet one, reproduced on the same AVD in
 * the same run, is that the second scan opened **showing the first scan's
 * height range** — the renderer had already uploaded the previous project's
 * leftover pages before the clear landed, and `updateCombinedBounds` only ever
 * grows, so the new cloud arrived inside the old one's box. A monotonic epoch,
 * bumped on every clear, is what lets the renderer notice that the pages it is
 * holding belong to a store generation that no longer exists and drop them,
 * rather than blending two rooms into one.
 *
 * ## What the lock is, and what it is not
 *
 * A plain [ReentrantReadWriteLock]: many readers (the render thread's frame
 * sync, the measure tool's `samplePoints` on a background coroutine), one
 * writer (the clear). Unfair, deliberately — Java's unfair RW lock still blocks
 * *new* readers when a writer is queued, so a 60 fps reader cannot starve the
 * one clear that is waiting, and paying for fairness on the hot path to protect
 * a once-per-scan writer would be the wrong trade.
 *
 * It is **not** a substitute for copying page bytes before handing them to
 * Filament. The lock can only bound what happens while a thread is *inside*
 * [read]; a `BufferDescriptor` queued inside it is copied by the driver thread
 * long after it returns. Both are needed, and `PointCloudRenderer.uploadPool`
 * is the other one.
 */
interface CloudStoreGate {

    /**
     * Bumped on every [mutate]. A reader that cached anything derived from the
     * store's pages must throw it away when this changes.
     */
    val epoch: Long

    /** Run [block] with the store guaranteed not to be cleared under it. */
    fun <T> read(block: () -> T): T

    /** Run [block] as the store's only reader/writer, then bump [epoch]. */
    fun <T> mutate(block: () -> T): T

    companion object {
        /**
         * For sources whose store cannot be cleared under a live reader — the
         * live capture engine and the replay engine both own a store for the
         * life of their session and tear it down with the session. Costs
         * nothing and keeps [PointCloudSource] implementations that do not need
         * a gate from having to invent one.
         */
        val OPEN: CloudStoreGate = object : CloudStoreGate {
            override val epoch: Long get() = 0L
            override fun <T> read(block: () -> T): T = block()
            override fun <T> mutate(block: () -> T): T = block()
        }
    }
}

/** The real gate. See [CloudStoreGate]. */
class ReadWriteCloudStoreGate : CloudStoreGate {

    private val lock = ReentrantReadWriteLock(false)

    @Volatile
    private var generation: Long = 1L

    override val epoch: Long get() = generation

    override fun <T> read(block: () -> T): T {
        lock.readLock().lock()
        try {
            return block()
        } finally {
            lock.readLock().unlock()
        }
    }

    override fun <T> mutate(block: () -> T): T {
        lock.writeLock().lock()
        try {
            return block()
        } finally {
            // Bumped BEFORE the lock is released, so no reader can observe the
            // cleared store still wearing the old epoch and conclude its cached
            // pages are current.
            generation++
            lock.writeLock().unlock()
        }
    }
}
