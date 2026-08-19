package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 22 item 91 — **the LOD-slider crash**, pinned as arithmetic.
 *
 * Every number below is a real one: the engine's default page store is
 * `page_capacity = 1 << 20` (`page_store.h`), a `PointVertex` is 16 bytes, so a
 * Review page is 16 777 216 bytes of VertexBuffer whether it holds a million
 * points or a thousand. The old renderer charged **points** against the LOD
 * budget and allocated **capacity**, and these tests exist so that the two can
 * never drift apart again.
 */
class GpuPageBudgetTest {

    /** The engine's default page capacity — `page_store.h`, `1 << 20` points. */
    private val reviewPageCapacity = 1 shl 20

    /** …which is 16 777 216 bytes of VertexBuffer per page. */
    private val reviewPageBytes = 16_777_216L

    // ── (ii) sizing a VertexBuffer to the page's actual count ───────────────

    @Test
    fun `a sparse review page allocates kilobytes, not sixteen megabytes`() {
        // The exact case in the diagnosis: 1 000 points sitting in a 1 << 20 page.
        val old = maxOf(reviewPageCapacity, 1_000).toLong() * GpuPageBudget.POINT_STRIDE_BYTES
        assertEquals(reviewPageBytes, old)

        assertEquals(4_096, GpuPageBudget.allocationVertices(1_000, reviewPageCapacity))
        assertEquals(65_536L, GpuPageBudget.allocationBytes(1_000, reviewPageCapacity))
        // 64 KiB against 16 MiB: 256× less. (The points themselves are 16 000
        // bytes; the rest is the 4 096-point granule, which is the slack the
        // next 3 096 points land in without touching the allocator.)
        assertEquals(256L, old / GpuPageBudget.allocationBytes(1_000, reviewPageCapacity))
    }

    @Test
    fun `allocation rounds up to the next 4096 points and no further`() {
        assertEquals(4_096, GpuPageBudget.allocationVertices(1, reviewPageCapacity))
        assertEquals(4_096, GpuPageBudget.allocationVertices(4_095, reviewPageCapacity))
        assertEquals(4_096, GpuPageBudget.allocationVertices(4_096, reviewPageCapacity))
        assertEquals(8_192, GpuPageBudget.allocationVertices(4_097, reviewPageCapacity))
        assertEquals(8_192, GpuPageBudget.allocationVertices(8_192, reviewPageCapacity))
        assertEquals(61_440, GpuPageBudget.allocationVertices(60_000, reviewPageCapacity))
        // A genuinely full page still allocates its full size — the fix costs
        // nothing when the page really is full.
        assertEquals(
            reviewPageCapacity,
            GpuPageBudget.allocationVertices(reviewPageCapacity, reviewPageCapacity),
        )
        assertEquals(
            reviewPageBytes,
            GpuPageBudget.allocationBytes(reviewPageCapacity, reviewPageCapacity),
        )
    }

    @Test
    fun `allocation never exceeds the page capacity`() {
        // A small store (LivePageStoreSizing's "modest" tier is 32 k pages) must
        // not be rounded UP past what the page can ever hold.
        assertEquals(2_000, GpuPageBudget.allocationVertices(1_000, 2_000))
        assertEquals(100, GpuPageBudget.allocationVertices(100, 100))
        assertEquals(32_768, GpuPageBudget.allocationVertices(30_000, 32_768))
        // Defensive: a source reporting count > capacity is clamped to capacity
        // rather than trusted (the renderer clamps its uploads to match).
        assertEquals(4_000, GpuPageBudget.allocationVertices(5_000, 4_000))
    }

    @Test
    fun `an unreported capacity falls back to the rounded count`() {
        assertEquals(8_192, GpuPageBudget.allocationVertices(5_000, 0))
        assertEquals(8_192, GpuPageBudget.allocationVertices(5_000, -1))
    }

    @Test
    fun `an empty page still gets one granule, never a zero-vertex buffer`() {
        // Filament rejects a zero-vertex VertexBuffer outright.
        assertEquals(4_096, GpuPageBudget.allocationVertices(0, reviewPageCapacity))
        assertEquals(4_096, GpuPageBudget.allocationVertices(-5, reviewPageCapacity))
        assertEquals(65_536L, GpuPageBudget.allocationBytes(0, reviewPageCapacity))
        assertEquals(0L, GpuPageBudget.bytesForVertices(0))
        assertEquals(0L, GpuPageBudget.bytesForVertices(-3))
        assertEquals(16L, GpuPageBudget.bytesForVertices(1))
    }

    // ── (ii) growth: what happens when a live page outgrows its buffer ──────

    @Test
    fun `growth is only needed once the count passes the allocation`() {
        assertFalse(GpuPageBudget.needsGrowth(allocatedVertices = 4_096, neededCount = 4_096))
        assertFalse(GpuPageBudget.needsGrowth(allocatedVertices = 4_096, neededCount = 100))
        assertTrue(GpuPageBudget.needsGrowth(allocatedVertices = 4_096, neededCount = 4_097))
        // No growth means no reallocation: the same buffer is returned.
        assertEquals(4_096, GpuPageBudget.growthVertices(4_096, 4_096, reviewPageCapacity))
        assertEquals(4_096, GpuPageBudget.growthVertices(4_096, 1, reviewPageCapacity))
    }

    @Test
    fun `growth doubles rather than fitting exactly`() {
        assertEquals(8_192, GpuPageBudget.growthVertices(4_096, 4_097, reviewPageCapacity))
        assertEquals(8_192, GpuPageBudget.growthVertices(4_096, 8_000, reviewPageCapacity))
        // Needing more than double jumps straight to the rounded need.
        assertEquals(102_400, GpuPageBudget.growthVertices(4_096, 100_000, reviewPageCapacity))
        // A first allocation through the growth path still gets a whole granule.
        assertEquals(4_096, GpuPageBudget.growthVertices(0, 100, reviewPageCapacity))
    }

    @Test
    fun `a live page reaches the store's capacity in eight reallocations`() {
        // The amortisation argument, run: a page climbing from its first
        // 4 096-vertex buffer to the engine's 1 << 20 capacity one revolution at
        // a time. Fitting exactly at 4 096-point steps would be 256 of these,
        // each re-uploading the whole prefix — ~2 GB of re-upload for one page.
        var allocated = GpuPageBudget.allocationVertices(1_000, reviewPageCapacity)
        assertEquals(4_096, allocated)
        var reallocations = 0
        var reuploadedVertices = 0L
        var count = 4_096
        while (count < reviewPageCapacity) {
            count = minOf(count + 4_096, reviewPageCapacity)
            if (GpuPageBudget.needsGrowth(allocated, count)) {
                // The prefix already on the GPU is re-sent into the new buffer.
                reuploadedVertices += allocated.toLong()
                allocated = GpuPageBudget.growthVertices(allocated, count, reviewPageCapacity)
                reallocations++
            }
        }
        assertEquals(8, reallocations)
        assertEquals(reviewPageCapacity, allocated)
        // 4 096 + 8 192 + … + 524 288 = 1 048 576 − 4 096 vertices re-uploaded in
        // total across the page's whole life: one page's worth, not 128 of them.
        assertEquals((reviewPageCapacity - 4_096).toLong(), reuploadedVertices)
        assertEquals(16_711_680L, reuploadedVertices * GpuPageBudget.POINT_STRIDE_BYTES)
    }

    @Test
    fun `growth stops at the page capacity`() {
        assertEquals(
            reviewPageCapacity,
            GpuPageBudget.growthVertices(524_288, 600_000, reviewPageCapacity),
        )
        assertEquals(
            reviewPageCapacity,
            GpuPageBudget.growthVertices(reviewPageCapacity, reviewPageCapacity, reviewPageCapacity),
        )
        // Doubling would overshoot a small store's page; capacity wins.
        assertEquals(4_500, GpuPageBudget.growthVertices(4_096, 4_500, 4_500))
        assertEquals(32_768, GpuPageBudget.growthVertices(16_384, 20_000, 32_768))
    }

    // ── (i) admission accounted in allocated bytes ──────────────────────────

    @Test
    fun `admission is a byte ceiling with an exact boundary`() {
        val budget = 1_000_000L
        assertTrue(GpuPageBudget.admits(residentBytes = 900_000L, pageAllocBytes = 100_000L, budgetBytes = budget))
        assertFalse(GpuPageBudget.admits(residentBytes = 900_000L, pageAllocBytes = 100_001L, budgetBytes = budget))
        assertFalse(GpuPageBudget.admits(residentBytes = 1_000_000L, pageAllocBytes = 1L, budgetBytes = budget))
        assertFalse(GpuPageBudget.admits(residentBytes = 1_000_001L, pageAllocBytes = 0L, budgetBytes = budget))
    }

    @Test
    fun `the first page is always admitted and a zero budget admits nothing`() {
        // Otherwise dragging the slider to its floor would empty the screen with
        // no diagnostic at all.
        assertTrue(GpuPageBudget.admits(0L, reviewPageBytes, budgetBytes = 1L))
        assertFalse(GpuPageBudget.admits(reviewPageBytes, reviewPageBytes, budgetBytes = 1L))
        // "Off" is not "unlimited".
        assertFalse(GpuPageBudget.admits(0L, 16L, budgetBytes = 0L))
        assertFalse(GpuPageBudget.admits(0L, 16L, budgetBytes = -1L))
    }

    @Test
    fun `an unlimited budget does not overflow into refusing everything`() {
        // `residentBytes + pageAllocBytes` would go negative here; the
        // subtraction form does not.
        assertTrue(GpuPageBudget.admits(Long.MAX_VALUE / 2, reviewPageBytes, Long.MAX_VALUE))
        assertTrue(GpuPageBudget.admits(reviewPageBytes, reviewPageBytes, Long.MAX_VALUE))
    }

    @Test
    fun `the point budget converts to bytes and is capped at the resident ceiling`() {
        assertEquals(268_435_456L, GpuPageBudget.MAX_RESIDENT_BYTES)
        // Small budgets convert straight through: points x 16.
        assertEquals(16_000L, GpuPageBudget.budgetBytesFor(1_000L))
        assertEquals(80_000_000L, GpuPageBudget.budgetBytesFor(5_000_000L)) // DisplayParams default
        assertEquals(128_000_000L, GpuPageBudget.budgetBytesFor(8_000_000L)) // FLOOR_PLAN preset
        // 16 777 216 points x 16 B is exactly the ceiling.
        assertEquals(268_435_456L, GpuPageBudget.budgetBytesFor(16_777_216L))
        // Above it, the slider is advisory: 20 M points would be 320 000 000 B
        // and 50 M would be 800 000 000 B, and neither is a number a phone can
        // hand to a GPU driver.
        assertEquals(268_435_456L, GpuPageBudget.budgetBytesFor(20_000_000L))
        assertEquals(268_435_456L, GpuPageBudget.budgetBytesFor(50_000_000L))
        assertEquals(268_435_456L, GpuPageBudget.budgetBytesFor(200_000_000L)) // DisplayParams clamp
        // Long.MAX_VALUE is the renderer's "no DisplayParams bound yet" default
        // and must not wrap negative on the x16.
        assertEquals(268_435_456L, GpuPageBudget.budgetBytesFor(Long.MAX_VALUE))
        assertEquals(0L, GpuPageBudget.budgetBytesFor(0L))
        assertEquals(0L, GpuPageBudget.budgetBytesFor(-1L))
    }

    @Test
    fun `the old point accounting reaches 336 MB under a 20 M budget and the new one cannot`() {
        // OLD: charge page.count, allocate max(capacity, count).
        val oldPages = pagesAdmittedByPointBudget(pageCount = reviewPageCapacity, pointBudget = 20_000_000L)
        assertEquals(20, oldPages)
        assertEquals(335_544_320L, oldPages * reviewPageBytes) // 336 MB, the crash

        // NEW: charge what is actually allocated, against the byte ceiling.
        val budget = GpuPageBudget.budgetBytesFor(20_000_000L)
        val newPages = pagesAdmittedByByteBudget(reviewPageCapacity, reviewPageCapacity, budget)
        assertEquals(16, newPages)
        assertEquals(268_435_456L, newPages * reviewPageBytes)
        assertTrue(newPages * reviewPageBytes < 335_544_320L)
    }

    @Test
    fun `a 50 M budget used to allocate over 800 MB and now cannot exceed the ceiling`() {
        // The RESEARCH preset (profileDefaults(): lodPointBudget = 50 000 000).
        val oldPages = pagesAdmittedByPointBudget(pageCount = reviewPageCapacity, pointBudget = 50_000_000L)
        assertEquals(48, oldPages)
        assertEquals(805_306_368L, oldPages * reviewPageBytes)
        assertTrue(oldPages * reviewPageBytes > 800_000_000L)

        val budget = GpuPageBudget.budgetBytesFor(50_000_000L)
        val newPages = pagesAdmittedByByteBudget(reviewPageCapacity, reviewPageCapacity, budget)
        assertEquals(16, newPages)
        assertEquals(268_435_456L, newPages * reviewPageBytes)
    }

    @Test
    fun `the thousand-point page is where the old accounting was off by a factor of a thousand`() {
        // 1 000 points charged, 16 777 216 bytes allocated. A 20 M point budget
        // therefore admitted twenty THOUSAND pages.
        val oldPages = pagesAdmittedByPointBudget(pageCount = 1_000, pointBudget = 20_000_000L)
        assertEquals(20_000, oldPages)
        assertEquals(335_544_320_000L, oldPages * reviewPageBytes) // 335 GB

        val budget = GpuPageBudget.budgetBytesFor(20_000_000L)
        val newPages = pagesAdmittedByByteBudget(1_000, reviewPageCapacity, budget)
        // Right-sized at 65 536 B each, the same ceiling holds 4 096 of them —
        // and the ceiling, not the page count, is what bounds the memory.
        assertEquals(4_096, newPages)
        assertEquals(268_435_456L, newPages * GpuPageBudget.allocationBytes(1_000, reviewPageCapacity))
        // Same slider setting, 1 250x less memory — and, unlike the old rule,
        // a number that does not depend on how full the pages happen to be.
        assertEquals(
            1_250L,
            (oldPages * reviewPageBytes) / (newPages * GpuPageBudget.allocationBytes(1_000, reviewPageCapacity)),
        )
    }

    // ── (iii) the per-frame allocation budget ───────────────────────────────

    @Test
    fun `twenty-four new review pages in one frame was 402 MB and is now refused`() {
        assertEquals(16_777_216L, GpuPageBudget.MAX_NEW_PAGE_BYTES_PER_FRAME)
        // What MAX_NEW_PAGES_PER_FRAME = 24 authorised inside one Choreographer
        // callback, with Review's page size:
        assertEquals(402_653_184L, 24 * reviewPageBytes)

        var spent = 0L
        var created = 0
        repeat(24) {
            if (GpuPageBudget.admitsNewAllocation(spent, reviewPageBytes)) {
                spent += reviewPageBytes
                created++
            }
        }
        // One full page per frame — the twenty-page Review cloud materialises
        // over twenty frames (a third of a second) instead of in one.
        assertEquals(1, created)
        assertEquals(16_777_216L, spent)
        assertTrue(spent < 402_653_184L)
    }

    @Test
    fun `right-sized pages are cheap enough that the frame budget never bites`() {
        val alloc = GpuPageBudget.allocationBytes(1_000, reviewPageCapacity) // 65 536 B
        var spent = 0L
        var created = 0
        repeat(1_000) {
            if (GpuPageBudget.admitsNewAllocation(spent, alloc)) {
                spent += alloc
                created++
            }
        }
        // 16 MiB / 64 KiB = 256 fresh pages per frame, which no source produces.
        assertEquals(256, created)
        assertEquals(16_777_216L, spent)
        // And the first allocation of a frame is never refused, however big it
        // is, or a page larger than the frame budget would never be built.
        assertTrue(GpuPageBudget.admitsNewAllocation(0L, 64L * 1024 * 1024))
        assertFalse(GpuPageBudget.admitsNewAllocation(16_777_216L, 16L))
    }

    // ── (vi) reaping pages the engine has evicted ───────────────────────────

    @Test
    fun `the reaper drops exactly the ids that vanished`() {
        // kEvictOldest retires the oldest pages: 1 and 2 are gone, 5 is new.
        val resident = linkedSetOf(1, 2, 3, 4)
        val seen = setOf(3, 4, 5)
        assertEquals(setOf(1, 2), GpuPageBudget.reap(resident, seen))
        // …and nothing else: the resident set is untouched by the call.
        assertEquals(linkedSetOf(1, 2, 3, 4), resident)
    }

    @Test
    fun `the reaper is empty when nothing vanished`() {
        assertEquals(emptySet<Int>(), GpuPageBudget.reap(setOf(1, 2, 3), setOf(1, 2, 3)))
        assertEquals(emptySet<Int>(), GpuPageBudget.reap(setOf(1, 2, 3), setOf(1, 2, 3, 4, 5)))
        assertEquals(emptySet<Int>(), GpuPageBudget.reap(emptySet(), setOf(1, 2, 3)))
    }

    @Test
    fun `an empty observation reaps nothing, so a transient hiccup cannot nuke the cloud`() {
        // pageCount() == 0 for one frame — a store between epochs, a replay
        // seek, a bridge handle that went to 0 — must NOT destroy and re-upload
        // the whole cloud.
        assertEquals(emptySet<Int>(), GpuPageBudget.reap(setOf(1, 2, 3), emptySet()))
        assertEquals(emptySet<Int>(), GpuPageBudget.reap(emptySet(), emptySet()))
    }

    @Test
    fun `the reaper drops everything when the store has moved on entirely`() {
        // A long walk: every id resident is older than the eviction window.
        assertEquals(setOf(1, 2, 3), GpuPageBudget.reap(setOf(1, 2, 3), setOf(90, 91)))
    }

    @Test
    fun `the reaper preserves the resident insertion order`() {
        val resident = linkedSetOf(7, 3, 11, 5, 2)
        val doomed = GpuPageBudget.reap(resident, setOf(3, 5))
        assertEquals(listOf(7, 11, 2), doomed.toList())
    }

    // ── helpers: the two accounting rules, run head to head ─────────────────

    /**
     * The OLD admission rule, verbatim: `if (resident >= lodPointBudget) stop`,
     * with `resident += page.count`. Returns how many pages it lets in.
     */
    private fun pagesAdmittedByPointBudget(pageCount: Int, pointBudget: Long): Int {
        var resident = 0L
        var pages = 0
        while (resident < pointBudget) {
            resident += pageCount.toLong()
            pages++
        }
        return pages
    }

    /** The NEW rule: allocated bytes against a byte ceiling. */
    private fun pagesAdmittedByByteBudget(pageCount: Int, pageCapacity: Int, budgetBytes: Long): Int {
        val alloc = GpuPageBudget.allocationBytes(pageCount, pageCapacity)
        var resident = 0L
        var pages = 0
        while (GpuPageBudget.admits(resident, alloc, budgetBytes)) {
            resident += alloc
            pages++
        }
        return pages
    }
}
