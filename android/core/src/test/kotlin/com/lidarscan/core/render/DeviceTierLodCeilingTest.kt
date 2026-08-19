package com.lidarscan.core.render

import com.lidarscan.core.capture.DeviceTier
import com.lidarscan.core.capture.PerformancePresets
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 22 item 100 — **the LOD ceiling belongs to the device.**
 *
 * The owner: *"ceiling the LOD depends on the device. User will not able to
 * increase the LOD due to the detected hardware they are using."*
 *
 * Item 91 capped resident vertex-buffer bytes at a flat 256 MiB, which is right
 * for one phone and wrong as a constant. This suite pins the ladder, the
 * clamp, and — the part that actually protects the operator — that **no
 * selectable value on any tier can exceed that tier's ceiling**. A control that
 * offers a budget the device cannot hold is the app inviting the crash item 91
 * just fixed.
 */
class DeviceTierLodCeilingTest {

    // ── the ladder ─────────────────────────────────────────────────────────

    @Test
    fun `each tier gets its own ceiling, and STANDARD is item 91's constant unchanged`() {
        assertEquals(96L * 1024 * 1024, GpuPageBudget.ceilingBytesFor(DeviceTier.MODEST))
        assertEquals(256L * 1024 * 1024, GpuPageBudget.ceilingBytesFor(DeviceTier.STANDARD))
        assertEquals(512L * 1024 * 1024, GpuPageBudget.ceilingBytesFor(DeviceTier.FLAGSHIP))
        assertEquals(
            "the STANDARD rung must remain byte-identical to the round-91 constant",
            GpuPageBudget.MAX_RESIDENT_BYTES,
            GpuPageBudget.ceilingBytesFor(DeviceTier.STANDARD),
        )
    }

    @Test
    fun `the ladder is monotonic - a better phone is never given less`() {
        val modest = GpuPageBudget.ceilingBytesFor(DeviceTier.MODEST)
        val standard = GpuPageBudget.ceilingBytesFor(DeviceTier.STANDARD)
        val flagship = GpuPageBudget.ceilingBytesFor(DeviceTier.FLAGSHIP)
        assertTrue(modest < standard)
        assertTrue(standard < flagship)
    }

    @Test
    fun `the ceilings convert to the point counts the ladder claims`() {
        // 16 bytes per PointVertex, so the arithmetic is stated rather than
        // implied: these are the numbers the doc table carries.
        assertEquals(6_291_456, GpuPageBudget.maxSelectableLodPoints(DeviceTier.MODEST))
        assertEquals(16_777_216, GpuPageBudget.maxSelectableLodPoints(DeviceTier.STANDARD))
        assertEquals(33_554_432, GpuPageBudget.maxSelectableLodPoints(DeviceTier.FLAGSHIP))
    }

    /**
     * The tier comes from the probe the app already had — no new heuristic was
     * invented for this item. The owner's Pixel 8 Pro (11 573 MB, 9 cores,
     * 120 Hz) is `FLAGSHIP`, so his ceiling is the 512 MiB rung.
     */
    @Test
    fun `the owner's Pixel 8 Pro lands on the FLAGSHIP rung through the existing probe`() {
        val tier = PerformancePresets.tierFor(totalRamMb = 11_573, cpuCores = 9, displayCeilingHz = 120)
        assertEquals(DeviceTier.FLAGSHIP, tier)
        assertEquals(512L * 1024 * 1024, GpuPageBudget.ceilingBytesFor(tier))
    }

    @Test
    fun `a low-RAM phone lands on MODEST and gets the smallest ceiling`() {
        val tier = PerformancePresets.tierFor(totalRamMb = 3_800, cpuCores = 4, displayCeilingHz = 60)
        assertEquals(DeviceTier.MODEST, tier)
        assertEquals(96L * 1024 * 1024, GpuPageBudget.ceilingBytesFor(tier))
    }

    // ── the clamp ──────────────────────────────────────────────────────────

    @Test
    fun `the RESEARCH profile's 50 M is clamped on every tier`() {
        // 50 000 000 points = 800 MB of VertexBuffer. No rung allows it.
        for (tier in DeviceTier.entries) {
            val clamped = GpuPageBudget.clampLodPointBudget(50_000_000, tier)
            assertTrue(
                "$tier must clamp 50 M",
                clamped < 50_000_000,
            )
            assertEquals(GpuPageBudget.maxSelectableLodPoints(tier), clamped)
        }
    }

    @Test
    fun `DisplayParams' own 200 M clamp is still far above every tier ceiling`() {
        for (tier in DeviceTier.entries) {
            assertTrue(
                "$tier: the point-domain clamp was never a memory limit",
                GpuPageBudget.maxSelectableLodPoints(tier) < 200_000_000,
            )
        }
    }

    @Test
    fun `a budget that already fits is returned unchanged - identity is the no-op signal`() {
        assertEquals(2_000_000, GpuPageBudget.clampLodPointBudget(2_000_000, DeviceTier.MODEST))
        assertEquals(20_000_000, GpuPageBudget.clampLodPointBudget(20_000_000, DeviceTier.FLAGSHIP))
    }

    @Test
    fun `a flagship setting arriving on a modest phone is cut to the modest ceiling`() {
        // The travelling-project case: 30 M is legal on a flagship and is three
        // times what a modest phone can hold.
        val onFlagship = GpuPageBudget.clampLodPointBudget(30_000_000, DeviceTier.FLAGSHIP)
        assertEquals(30_000_000, onFlagship)
        val onModest = GpuPageBudget.clampLodPointBudget(30_000_000, DeviceTier.MODEST)
        assertEquals(GpuPageBudget.maxSelectableLodPoints(DeviceTier.MODEST), onModest)
    }

    @Test
    fun `clamping is idempotent`() {
        for (tier in DeviceTier.entries) {
            val once = GpuPageBudget.clampLodPointBudget(50_000_000, tier)
            assertEquals(once, GpuPageBudget.clampLodPointBudget(once, tier))
        }
    }

    // ── the renderer's own enforcement agrees with the controls ────────────

    @Test
    fun `budgetBytesFor never exceeds the tier ceiling, whatever the slider says`() {
        for (tier in DeviceTier.entries) {
            val ceiling = GpuPageBudget.ceilingBytesFor(tier)
            for (points in listOf(1L, 1_000L, 20_000_000L, 50_000_000L, 200_000_000L, Long.MAX_VALUE)) {
                assertTrue(
                    "$tier at $points points",
                    GpuPageBudget.budgetBytesFor(points, tier) <= ceiling,
                )
            }
        }
    }

    @Test
    fun `budgetBytesFor defaults to STANDARD so pre-item-100 callers are unchanged`() {
        assertEquals(
            GpuPageBudget.budgetBytesFor(50_000_000L, DeviceTier.STANDARD),
            GpuPageBudget.budgetBytesFor(50_000_000L),
        )
    }

    @Test
    fun `the maximum selectable value is exactly the largest one that is not clamped`() {
        for (tier in DeviceTier.entries) {
            val max = GpuPageBudget.maxSelectableLodPoints(tier)
            assertEquals("$tier", max, GpuPageBudget.clampLodPointBudget(max, tier))
            assertTrue(
                "$tier: one point more must be refused",
                GpuPageBudget.clampLodPointBudget(max + 1, tier) < max + 1,
            )
        }
    }

    // ── what the operator is told ──────────────────────────────────────────

    @Test
    fun `the ceiling note appears only when something is actually limited`() {
        assertNull(GpuPageBudget.ceilingNote(1_000_000, DeviceTier.MODEST))
        val note = GpuPageBudget.ceilingNote(50_000_000, DeviceTier.MODEST)
        assertNotNull(note)
        assertEquals("Limited by this device", note)
    }

    @Test
    fun `the ceiling note obeys the wording law`() {
        val note = GpuPageBudget.ceilingNote(50_000_000, DeviceTier.MODEST)!!
        assertTrue(note, note.trim().split(Regex("\\s+")).size <= 6)
        for (jargon in listOf("§", "A12", "A15", "RANSAC", "CRS", "ECEF", "LOD", "VBO")) {
            assertTrue("no jargon in an operator-facing note: $note", !note.contains(jargon))
        }
    }
}
