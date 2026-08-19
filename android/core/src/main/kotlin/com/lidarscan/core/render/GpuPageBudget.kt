package com.lidarscan.core.render

/**
 * ROUND 22 item 91 — **the LOD-slider crash: the renderer's GPU memory
 * arithmetic, pulled out of the renderer so it can be tested without a GPU.**
 *
 * ### The bug this exists to make impossible
 *
 * `PointCloudRenderer.syncPointCloud()` had two numbers that were supposed to
 * be the same number and were not:
 *
 *  * the **admission** test counted `page.count` — the points a page currently
 *    holds — against `DisplayParams.lodPointBudget`;
 *  * the **allocation** two dozen lines later built a `VertexBuffer` of
 *    `max(page.capacity, page.count)` vertices × 16 bytes.
 *
 * The Review screen replays an `.lscan` through the engine's *default* page
 * store, which `engine/include/scanengine/cloud/page_store.h` sizes at
 * `page_capacity = 1 << 20` — **1 048 576 points, i.e. 16 777 216 bytes
 * (16 MiB) of VertexBuffer per page.** And pages are single-stream and only the
 * tail is ever appended to (see [LivePageStoreSizing]'s header for the same
 * mechanism costing a live D6 a gigabyte), so a page that holds **1 000 points
 * is charged 1 000 against the budget and allocates 16 MiB** — under-accounting
 * by a factor of ~1 000.
 *
 * The arithmetic of the crash, with the numbers actually on screen:
 *
 * | budget the slider says | pages the OLD rule admits | bytes it really allocated |
 * | --- | --- | --- |
 * | 20 000 000 pts, full pages | 20 × 1 048 576 | **335 544 320 (336 MB)** |
 * | 50 000 000 pts, full pages | 48 × 1 048 576 | **805 306 368 (805 MB)** |
 * | 20 000 000 pts, 1 000-pt pages | 20 000 pages | **335 544 320 000 (335 GB)** |
 *
 * The last row is the LOD slider: dragging it right does not add points, it
 * adds *pages*, and every page costs 16 MiB whether it is full or holds a
 * thousand returns. The process dies somewhere in the second row.
 *
 * ### The rules here
 *
 *  1. **Admission is accounted in allocated BYTES** ([admits], [budgetBytesFor]),
 *     never in points, because bytes are what the driver has to find.
 *  2. **A VertexBuffer is sized to the page's actual count**, rounded up by
 *     [ALLOC_GRANULARITY_POINTS] ([allocationVertices]) — not to the page's
 *     capacity. The 1 000-point page above allocates 65 536 bytes instead of
 *     16 777 216: **256× less**, and 4 KB of that is slack for the next 3 096
 *     points to land in without touching the allocator.
 *  3. **A page that outgrows its buffer grows geometrically**
 *     ([growthVertices]) — see that function for why doubling, and not
 *     "reallocate to fit", is the only affordable answer for a LIVE page that
 *     climbs from 4 096 to 1 048 576 points one lidar revolution at a time.
 *  4. **New allocation per frame is a byte budget too**
 *     ([MAX_NEW_PAGE_BYTES_PER_FRAME]), replacing a count of pages. Twenty-four
 *     new pages sounded modest and was 24 × 16 MiB = **402 653 184 bytes of
 *     allocation inside one Choreographer callback**.
 *  5. **Pages the engine has evicted are reaped** ([reap]). The live store runs
 *     `PageFullPolicy::kEvictOldest`, so a pageId that resolved last frame can
 *     simply stop existing; nothing used to notice, and the orphaned GpuPage
 *     stayed in the Filament scene for the life of the surface.
 *
 * Everything here is pure integer arithmetic on purpose: the renderer it serves
 * cannot be unit-tested on the JVM (Filament is a native library and the module
 * has no Robolectric), so the part that can be tested is the part that was
 * wrong.
 */
object GpuPageBudget {

    /**
     * `PointVertex` is float3 position + RGBA8. The same 16 bytes as
     * `PointCloudRenderer.POINT_STRIDE_BYTES`, [LivePageStoreSizing.BYTES_PER_POINT]
     * and the engine's `static_assert(sizeof(PointVertex) == 16)` — three
     * names for one layout, and the number that turns a point budget into a
     * memory budget.
     */
    const val POINT_STRIDE_BYTES = 16L

    /**
     * Vertex allocations are rounded up to a multiple of this many points.
     *
     * 4 096 points = 65 536 bytes. The rounding buys two things and costs
     * almost nothing:
     *
     *  * a live page that grows a few hundred points per frame (a COIN-D6 emits
     *    3 600 pts/s, i.e. ~60 per 60 Hz frame) does not hit the growth path
     *    every frame — it hits it every ~68 frames at worst, and after the
     *    first doubling, far less often than that;
     *  * a page's buffer size is quantised, so the same size is asked of the
     *    driver over and over rather than 700 slightly different ones.
     *
     * The waste is bounded by 65 536 bytes per page — against a 64-page store
     * that is 4 MiB worst case, versus the **1 GiB** the capacity-sized
     * allocation cost.
     */
    const val ALLOC_GRANULARITY_POINTS = 4096

    /**
     * ROUND 22 item 91 (iii): the per-frame ceiling on **new vertex-buffer
     * allocation**, in bytes — the direct replacement for
     * `MAX_NEW_PAGES_PER_FRAME = 24`, and the same idea as the round-5.3
     * `MAX_UPLOAD_BYTES_PER_FRAME = 4 MiB` that already sat beside it.
     *
     * Counting *pages* was meaningless the moment pages stopped being the same
     * size: 24 pages is 1.5 MiB of a live 32 k-point store and 402 653 184 bytes
     * (384 MiB) of Review's 1 << 20 store, allocated inside a single
     * Choreographer frame. That is the allocation spike the LOD slider produced.
     *
     * 16 MiB is exactly one worst-case full page (1 048 576 × 16 B), so the
     * pathological case admits **one** page per frame instead of 24 — a 20-page
     * Review cloud materialises over 20 frames, a third of a second, instead of
     * asking the driver for 384 MiB at once. Right-sized pages are far smaller,
     * so the same budget admits 256 fresh 4 096-point pages per frame, which no
     * real source produces.
     *
     * Work refused this frame is not lost: the page is simply not created yet
     * and the next frame reconsiders it — the same "spread it, never drop it"
     * contract as the upload budget.
     */
    const val MAX_NEW_PAGE_BYTES_PER_FRAME = 16L * 1024 * 1024

    /**
     * ROUND 22 item 91 (i): the absolute ceiling on resident vertex-buffer
     * bytes, whatever the LOD slider says.
     *
     * `DisplayParams.lodPointBudget` is clamped to 200 000 000 points and the
     * RESEARCH preset asks for 50 000 000. Honestly converted those are 3.2 GB
     * and 800 MB of VertexBuffer — an honest conversion of a number the UI
     * never presented as a memory figure. So the point budget is advisory above
     * this line and the renderer stops admitting pages, which is Tech Spec
     * §3.12's "degrade via LOD, never framerate" applied to memory as well as
     * to time.
     *
     * 256 MiB = 16 777 216 points at 16 B each — more points than any phone
     * draws at an interactive rate, and it sits beside Filament's own render
     * targets, the ARCore camera path and the engine's page store (itself
     * capped at 96–384 MB by [LivePageStoreSizing]) inside a typical 512 MB
     * per-process budget.
     */
    const val MAX_RESIDENT_BYTES = 256L * 1024 * 1024

    // ── ROUND 22 item 100: the ceiling belongs to the DEVICE ────────────────
    //
    // The owner: *"ceiling the LOD depends on the device. User will not able to
    // increase the LOD due to the detected hardware they are using."*
    //
    // [MAX_RESIDENT_BYTES] above was reasoned about one phone and then written
    // as a constant, which makes it wrong in both directions: a `MODEST` device
    // still overruns it and a `FLAGSHIP` is starved by it. The ladder below
    // keys off the SAME `DeviceTier` the app has probed since ROUND 6
    // (`PerformancePresets.tierFor(totalRamMb, cpuCores, displayCeilingHz)`,
    // logged at every start as `device tier=STANDARD ram=11573MB cores=9`) —
    // deliberately not a new probe, because a second definition of "what can
    // this phone do" is a second thing that can disagree with the first.
    //
    // The numbers are the same reasoning applied per rung. A phone's whole
    // graphics budget has to hold Filament's render targets, the ARCore camera
    // path, and the engine's own page store — which `LivePageStoreSizing`
    // already sizes per tier at 96 / 384 / 384 MB. These are the VERTEX BUFFER
    // share of what is left:
    //
    //   MODEST    96 MiB =  6.3 M points — 4.5 GB-class phones, 4-6 cores
    //   STANDARD 256 MiB = 16.8 M points — the mainstream target; item 91's
    //                                      original constant, unchanged
    //   FLAGSHIP 512 MiB = 33.6 M points — >=7.5 GB RAM, >=8 cores, >60 Hz
    //                                      panel (the owner's Pixel 8 Pro)
    //
    // Even the FLAGSHIP rung is below the 50 M the RESEARCH profile used to
    // ask for, and far below `DisplayParams`' own 200 M clamp. That is the
    // point: those were point counts nobody had converted into bytes.

    const val MODEST_RESIDENT_BYTES = 96L * 1024 * 1024
    const val STANDARD_RESIDENT_BYTES = MAX_RESIDENT_BYTES
    const val FLAGSHIP_RESIDENT_BYTES = 512L * 1024 * 1024

    /**
     * The resident-bytes ceiling for [tier]. This is the number no control may
     * exceed and no persisted setting may survive above — see
     * [maxSelectableLodPoints] and [clampLodPointBudget].
     */
    fun ceilingBytesFor(tier: com.lidarscan.core.capture.DeviceTier): Long = when (tier) {
        com.lidarscan.core.capture.DeviceTier.MODEST -> MODEST_RESIDENT_BYTES
        com.lidarscan.core.capture.DeviceTier.STANDARD -> STANDARD_RESIDENT_BYTES
        com.lidarscan.core.capture.DeviceTier.FLAGSHIP -> FLAGSHIP_RESIDENT_BYTES
    }

    /**
     * The largest LOD budget, **in points**, this device may be offered.
     *
     * Every Detail/LOD control reads this rather than
     * `DisplayLimits.LOD_MAX_M`, because a control that offers 50 M points on a
     * phone that can hold 6.3 M is the app inviting the exact crash item 91
     * fixed. There is deliberately no override: the owner asked for one
     * ceiling, and a switch whose only function is to exceed a memory limit is
     * a switch whose only function is to crash.
     */
    fun maxSelectableLodPoints(tier: com.lidarscan.core.capture.DeviceTier): Int =
        (ceilingBytesFor(tier) / POINT_STRIDE_BYTES)
            .coerceAtMost(Int.MAX_VALUE.toLong())
            .toInt()

    /**
     * Clamps a LOD point budget to what [tier] may hold. Returns [requested]
     * unchanged when it already fits, so a caller can compare identity to
     * decide whether anything is worth logging.
     *
     * Applied on LOAD as well as on change: a project saved on a flagship, or
     * an old RESEARCH profile carrying 50 000 000, must not be able to walk a
     * modest phone into an out-of-memory kill just by being opened.
     */
    fun clampLodPointBudget(requested: Int, tier: com.lidarscan.core.capture.DeviceTier): Int {
        val ceiling = maxSelectableLodPoints(tier)
        return if (requested > ceiling) ceiling else requested
    }

    /**
     * One short line for the operator when the ceiling is doing something —
     * item 98's wording law: four words, no jargon, no number they cannot act
     * on. Null when nothing is being limited, so the caller renders nothing.
     */
    fun ceilingNote(requested: Int, tier: com.lidarscan.core.capture.DeviceTier): String? =
        if (requested > maxSelectableLodPoints(tier)) "Limited by this device" else null

    /** Bytes a vertex buffer of [vertices] `PointVertex` occupies. Negative counts read as zero. */
    fun bytesForVertices(vertices: Int): Long =
        if (vertices <= 0) 0L else vertices.toLong() * POINT_STRIDE_BYTES

    /**
     * ROUND 22 item 91 (ii): how big the page's `VertexBuffer` is actually
     * built — **the page's count rounded up to [ALLOC_GRANULARITY_POINTS]**,
     * never its capacity.
     *
     * [pageCapacity] is still respected as a *ceiling*: a page can never hold
     * more than its capacity, so allocating past it is pure waste. A page whose
     * capacity is smaller than one granule (or is not reported at all, `<= 0`)
     * falls back sensibly rather than allocating zero vertices — Filament
     * rejects a zero-vertex buffer.
     *
     * Worked example, the one from the crash: `allocationVertices(1_000,
     * 1_048_576)` = 4 096 → 65 536 bytes, where the old
     * `max(capacity, count)` = 1 048 576 → 16 777 216 bytes.
     */
    fun allocationVertices(pageCount: Int, pageCapacity: Int): Int {
        // A capacity of 0 or less means "not reported" — the rounded count is
        // then the whole answer. Otherwise capacity is a hard ceiling: a page
        // cannot hold more than it can hold, so allocating past it is waste,
        // and a source reporting count > capacity is clamped (the renderer
        // clamps its uploads to the allocation for the same reason).
        val cap = if (pageCapacity > 0) pageCapacity else Int.MAX_VALUE
        val count = pageCount.coerceAtLeast(0).coerceAtMost(cap)
        return roundUpToGranularity(count).coerceAtMost(cap).coerceAtLeast(count)
    }

    /** [allocationVertices] in bytes — what [admits] is fed. */
    fun allocationBytes(pageCount: Int, pageCapacity: Int): Long =
        bytesForVertices(allocationVertices(pageCount, pageCapacity))

    /** True when [neededCount] no longer fits in a buffer of [allocatedVertices] vertices. */
    fun needsGrowth(allocatedVertices: Int, neededCount: Int): Boolean =
        neededCount > allocatedVertices

    /**
     * ROUND 22 item 91 (ii), the growth half: the new vertex count for a page
     * that has outgrown its buffer — **at least double the current
     * allocation**, at least enough for [neededCount] rounded up, and never
     * past [pageCapacity].
     *
     * Growing "to fit" instead would be quadratic where it hurts most. A live
     * page climbs from its first 4 096-vertex buffer all the way to the store's
     * 1 << 20 capacity, and every growth event destroys the buffer, builds a
     * new one and re-uploads the prefix from the source page. Fit-exactly
     * growth at 4 096-point steps is 256 reallocations and 2 GB of re-upload for
     * one page; doubling is **8 reallocations (4 096 → 8 192 → … → 1 048 576)
     * and about one page's worth of re-upload in total**, amortised O(1) per
     * point, which is the standard vector argument and applies unchanged.
     *
     * The renderer charges a growth against [MAX_NEW_PAGE_BYTES_PER_FRAME] at
     * its full new size, which is what bounds the re-upload that follows it:
     * the prefix being re-sent is always smaller than the buffer being paid for.
     */
    fun growthVertices(allocatedVertices: Int, neededCount: Int, pageCapacity: Int): Int {
        val cap = if (pageCapacity > 0) pageCapacity else Int.MAX_VALUE
        val current = allocatedVertices.coerceAtLeast(0)
        val needed = neededCount.coerceAtLeast(0).coerceAtMost(cap)
        if (needed <= current) return current
        val doubled = if (current >= Int.MAX_VALUE / 2) Int.MAX_VALUE else current * 2
        return maxOf(roundUpToGranularity(needed), doubled)
            .coerceAtMost(cap)
            .coerceAtLeast(needed)
    }

    /**
     * ROUND 22 item 91 (i): may a page allocating [pageAllocBytes] be admitted
     * when [residentBytes] are already committed against a [budgetBytes]
     * ceiling?
     *
     * Two deliberate asymmetries:
     *
     *  * **The first page is always admitted** (`residentBytes <= 0`). A budget
     *    smaller than a single page would otherwise render an empty screen and
     *    no diagnostic — the operator drags the slider to its floor and the
     *    cloud vanishes. One page over budget is bounded and visible; nothing
     *    at all is neither.
     *  * **A zero or negative budget admits nothing.** That is "the caller means
     *    off", not "the caller means unlimited"; unlimited is expressed as a
     *    very large budget, and [budgetBytesFor] never produces zero from a
     *    positive point budget.
     *
     * The subtraction is written as `pageAllocBytes <= budgetBytes -
     * residentBytes` rather than `residentBytes + pageAllocBytes <=
     * budgetBytes` so that a `Long.MAX_VALUE` budget cannot overflow the sum
     * into a negative and start refusing everything.
     */
    fun admits(residentBytes: Long, pageAllocBytes: Long, budgetBytes: Long): Boolean {
        if (budgetBytes <= 0L) return false
        if (residentBytes <= 0L) return true
        if (residentBytes >= budgetBytes) return false
        return pageAllocBytes <= budgetBytes - residentBytes
    }

    /**
     * ROUND 22 item 91 (iii): may one more allocation happen **this frame**,
     * given [spentBytes] already allocated in it?
     *
     * Same shape and the same first-one-always rule as [admits] — a page larger
     * than the whole frame budget must still get built eventually, or a Review
     * cloud of 16 MiB pages would never draw at all.
     */
    fun admitsNewAllocation(
        spentBytes: Long,
        allocBytes: Long,
        frameBudgetBytes: Long = MAX_NEW_PAGE_BYTES_PER_FRAME,
    ): Boolean = admits(spentBytes, allocBytes, frameBudgetBytes)

    /**
     * The user-facing LOD point budget, converted to the byte ceiling the
     * renderer actually enforces — and clamped to [MAX_RESIDENT_BYTES].
     *
     * `DisplayParams.lodPointBudget` is an `Int` clamped to 1 000 .. 200 000 000
     * points; `Long` here because the renderer holds it as one (and because
     * `200_000_000 * 16` overflows an `Int` at 3.2 e9, which is precisely the
     * kind of silent wrap this whole item is about).
     */
    @JvmOverloads
    fun budgetBytesFor(
        lodPointBudget: Long,
        tier: com.lidarscan.core.capture.DeviceTier = com.lidarscan.core.capture.DeviceTier.STANDARD,
    ): Long {
        if (lodPointBudget <= 0L) return 0L
        val raw =
            if (lodPointBudget > Long.MAX_VALUE / POINT_STRIDE_BYTES) {
                Long.MAX_VALUE
            } else {
                lodPointBudget * POINT_STRIDE_BYTES
            }
        // ROUND 22 item 100: the ceiling is the DEVICE's. The default keeps
        // every existing caller on the STANDARD rung — which is the 256 MiB
        // constant item 91 shipped — so adding the parameter changed no
        // behaviour until a caller passes a real tier.
        return minOf(raw, ceilingBytesFor(tier))
    }

    /**
     * ROUND 22 item 91 (vi): which resident GPU pages no longer exist in the
     * source, and must be destroyed.
     *
     * The live store runs `PageFullPolicy::kEvictOldest`
     * (`page_store.h`: *"when full, the OLDEST page is retired to make room, so
     * the view is a moving window over the newest data"*), so on a long walk
     * page ids stop resolving one by one. Nothing reaped them, so every evicted
     * page left a 16 MiB VertexBuffer and a Filament entity resident for the
     * life of the surface — a leak that grows without bound in exactly the
     * session that is already short of memory.
     *
     * **[seenIds] empty means "no answer", not "no pages".** A frame where the
     * source reported nothing — a store between epochs, a replay seek, a bridge
     * whose handle went to 0 for one poll — must not destroy the entire cloud
     * and re-upload it. So an empty observation reaps nothing; only ids missing
     * from a non-empty observation are reaped.
     *
     * Insertion order of [residentIds] is preserved in the result, which keeps
     * the renderer's destruction order (and therefore any log of it) stable.
     */
    fun reap(residentIds: Set<Int>, seenIds: Set<Int>): Set<Int> {
        if (seenIds.isEmpty() || residentIds.isEmpty()) return emptySet()
        return residentIds.filterTo(LinkedHashSet()) { it !in seenIds }
    }

    /** Rounds up to a whole number of [ALLOC_GRANULARITY_POINTS] granules, with an `Int` saturation guard. */
    private fun roundUpToGranularity(points: Int): Int {
        if (points <= ALLOC_GRANULARITY_POINTS) return ALLOC_GRANULARITY_POINTS
        val granule = ALLOC_GRANULARITY_POINTS.toLong()
        val rounded = ((points.toLong() + granule - 1L) / granule) * granule
        return rounded.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
    }
}
