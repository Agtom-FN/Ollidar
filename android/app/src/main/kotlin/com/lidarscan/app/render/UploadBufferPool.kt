package com.lidarscan.app.render

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ROUND 39 item 191 — **the buffers Filament is allowed to keep a pointer to.**
 *
 * `VertexBuffer.setBufferAt` does not copy. Filament's JNI takes the direct
 * buffer's address into a `BufferDescriptor`, queues it, and the actual
 * `glBufferSubData` — the `__memcpy_aarch64_simd` in the round-38 tombstone —
 * runs later on the `FEngine::loop` thread. Every byte the renderer handed it
 * until now was a view straight into the engine's page memory
 * (`NativePointPage.buffer`, a `NewDirectByteBuffer` over the store's own
 * allocation), so "the store was cleared between the frame and the driver
 * catching up" was a use-after-free with no lock able to prevent it: the read
 * happens on a thread the app does not own, at a time the app cannot name.
 *
 * So the renderer uploads from **its own** memory. One copy per slice, out of
 * the store and into a buffer this pool owns, made while
 * [com.lidarscan.app.engine.CloudStoreGate.read] is held — so the source cannot
 * be freed mid-copy either — and handed to Filament with a release callback
 * that returns it here when the driver is done with it.
 *
 * **Pooled, not allocated per upload**, and that is the whole reason this class
 * exists rather than a `ByteBuffer.allocateDirect` at the call site.
 * `allocateDirect` is an `mmap` plus a zero-fill plus a `Cleaner` registration;
 * at the renderer's ceiling of 4 MiB of uploads per frame that is real work on
 * the frame's critical path, every frame, for the life of a capture. Buffers
 * come back through [release] and are handed out again.
 *
 * **Why a size class rather than an exact fit.** A live capture uploads a few
 * kilobytes per frame and a Review load uploads a whole page at once; exact
 * fits would make the pool a museum of one-off sizes that never match the next
 * request. Requests round up to [GRANULE_BYTES] — 64 KiB, which is
 * `GpuPageBudget`'s own 4 096-point allocation granule at 16 bytes a point — so
 * every small slice shares one class and large ones share a handful.
 *
 * **Bounded.** [maxPooledBytes] caps what is *retained*; a buffer released when
 * the pool is full is simply dropped for the GC to reclaim. The pool can
 * therefore never hold more than twice a frame's upload budget hostage, which
 * is the same accounting `GpuPageBudget` does one layer up.
 *
 * Thread-safety: [acquire] runs on the render thread and [release] on whatever
 * thread the caller's Filament handler posts to (the renderer uses the main
 * looper, which is the same thread) — but the class synchronises anyway. An
 * uncontended monitor is a few nanoseconds and the alternative is a data race
 * that would only ever show up as the corruption this whole file exists to
 * prevent.
 */
class UploadBufferPool(private val maxPooledBytes: Long = MAX_POOLED_BYTES) {

    private val free = ArrayList<ByteBuffer>()
    private var pooled = 0L

    /** Diagnostics, for the round-39 measurement and for tests. */
    var allocations: Long = 0L
        private set
    var reuses: Long = 0L
        private set

    /**
     * A direct, little-endian buffer with `position == 0` and `limit == bytes`,
     * ready to be filled and handed to `setBufferAt`.
     *
     * LITTLE_ENDIAN because that is what the vertex layout is and what every
     * writer here assumes — `NewDirectByteBuffer` hands back BIG_ENDIAN
     * regardless of platform (see `PointCloudSource.samplePoints`, which was
     * bitten by exactly that), and a pooled buffer must not inherit that trap.
     */
    @Synchronized
    fun acquire(bytes: Int): ByteBuffer {
        require(bytes > 0) { "an upload of $bytes bytes is not an upload" }
        val want = sizeClass(bytes)
        var picked: ByteBuffer? = null
        for (i in free.indices) {
            if (free[i].capacity() == want) {
                picked = free.removeAt(i)
                pooled -= want
                reuses++
                break
            }
        }
        val buf = picked ?: ByteBuffer.allocateDirect(want).also { allocations++ }
        buf.order(ByteOrder.LITTLE_ENDIAN)
        buf.clear()
        buf.limit(bytes)
        return buf
    }

    /**
     * Hand a buffer back. Safe to call with anything: a foreign or
     * heap-allocated buffer is ignored rather than poisoning the pool, because
     * the caller is a Filament release callback and the cost of being wrong
     * there is silent corruption.
     */
    @Synchronized
    fun release(buf: ByteBuffer) {
        if (!buf.isDirect) return
        if (buf.capacity() != sizeClass(buf.capacity())) return
        if (pooled + buf.capacity() > maxPooledBytes) return
        for (b in free) if (b === buf) return
        pooled += buf.capacity()
        free.add(buf)
    }

    /** Retained bytes, for tests and for the stats line. */
    @Synchronized
    fun pooledBytes(): Long = pooled

    /** Drop everything retained — `detach()`, when the Engine is going away. */
    @Synchronized
    fun clear() {
        free.clear()
        pooled = 0L
    }

    companion object {
        /** 64 KiB = `GpuPageBudget`'s 4 096-point granule at 16 bytes a point. */
        const val GRANULE_BYTES = 64 * 1024

        /**
         * Twice `PointCloudRenderer.MAX_UPLOAD_BYTES_PER_FRAME`: enough that a
         * frame's worth of uploads can all be in flight and a second frame's
         * worth already acquired, and no more.
         */
        const val MAX_POOLED_BYTES = 8L * 1024 * 1024

        /** [bytes] rounded up to the next [GRANULE_BYTES]. */
        fun sizeClass(bytes: Int): Int {
            require(bytes > 0)
            val granules = (bytes + GRANULE_BYTES - 1) / GRANULE_BYTES
            return granules * GRANULE_BYTES
        }
    }
}
