package com.lidarscan.app.render

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 39 item 191 — the pool the renderer uploads out of instead of out of
 * the engine's page memory.
 */
class UploadBufferPoolTest {

    @Test
    fun aRequestRoundsUpToTheGranuleAndNeverDown() {
        assertEquals(UploadBufferPool.GRANULE_BYTES, UploadBufferPool.sizeClass(1))
        assertEquals(UploadBufferPool.GRANULE_BYTES, UploadBufferPool.sizeClass(UploadBufferPool.GRANULE_BYTES))
        assertEquals(
            2 * UploadBufferPool.GRANULE_BYTES,
            UploadBufferPool.sizeClass(UploadBufferPool.GRANULE_BYTES + 1),
        )
        // scan-030's whole cloud: 78 389 points at 16 bytes.
        assertEquals(20 * UploadBufferPool.GRANULE_BYTES, UploadBufferPool.sizeClass(78_389 * 16))
    }

    @Test
    fun anAcquiredBufferIsDirectLittleEndianAndExactlyAsLongAsAsked() {
        val pool = UploadBufferPool()
        val buf = pool.acquire(1_234)
        assertTrue("Filament reads the address; a heap buffer would be copied", buf.isDirect)
        assertEquals(ByteOrder.LITTLE_ENDIAN, buf.order())
        assertEquals(0, buf.position())
        assertEquals(1_234, buf.limit())
        assertEquals(UploadBufferPool.GRANULE_BYTES, buf.capacity())
    }

    @Test
    fun aReleasedBufferIsHandedOutAgainRatherThanReallocated() {
        val pool = UploadBufferPool()
        val first = pool.acquire(1_000)
        assertEquals(1L, pool.allocations)
        pool.release(first)
        val second = pool.acquire(2_000)
        assertSame("same size class, so the pool must reuse it", first, second)
        assertEquals(1L, pool.allocations)
        assertEquals(1L, pool.reuses)
    }

    /**
     * The property the crash is really about: while a buffer is OUT — Filament
     * holds its address until the driver has copied it — no other upload can be
     * handed the same memory.
     */
    @Test
    fun anOutstandingBufferIsNeverHandedToASecondUpload() {
        val pool = UploadBufferPool()
        val a = pool.acquire(1_000)
        val b = pool.acquire(1_000)
        val c = pool.acquire(1_000)
        assertNotSame(a, b)
        assertNotSame(b, c)
        assertNotSame(a, c)
        assertEquals(3L, pool.allocations)
    }

    @Test
    fun releasingTheSameBufferTwiceDoesNotPutItInTwice() {
        val pool = UploadBufferPool()
        val buf = pool.acquire(1_000)
        pool.release(buf)
        pool.release(buf)
        val first = pool.acquire(1_000)
        val second = pool.acquire(1_000)
        assertSame(buf, first)
        assertNotSame("a double release must not alias two live uploads", buf, second)
    }

    @Test
    fun foreignBuffersAreIgnoredRatherThanPooled() {
        val pool = UploadBufferPool()
        pool.release(ByteBuffer.allocate(UploadBufferPool.GRANULE_BYTES))
        pool.release(ByteBuffer.allocateDirect(12_345))
        assertEquals(0L, pool.pooledBytes())
    }

    @Test
    fun whatItRetainsIsBounded() {
        val cap = 4L * UploadBufferPool.GRANULE_BYTES
        val pool = UploadBufferPool(maxPooledBytes = cap)
        val held = (1..8).map { pool.acquire(UploadBufferPool.GRANULE_BYTES) }
        held.forEach { pool.release(it) }
        assertTrue("the pool must not grow without limit", pool.pooledBytes() <= cap)
    }

    @Test
    fun clearDropsEverythingRetained() {
        val pool = UploadBufferPool()
        pool.release(pool.acquire(1_000))
        assertTrue(pool.pooledBytes() > 0)
        pool.clear()
        assertEquals(0L, pool.pooledBytes())
    }
}
