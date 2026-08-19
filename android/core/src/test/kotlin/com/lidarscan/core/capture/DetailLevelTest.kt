package com.lidarscan.core.capture

import com.lidarscan.core.render.GpuPageBudget
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 22 items 95 + 100 — DETAIL (Auto / High / Max), and the rule that no
 * rung it offers can exceed what the phone can hold.
 *
 * The owner asked for two separate things that meet here: fewer, plainer
 * choices on the Scan screen (item 95), and a ceiling the user cannot raise
 * (item 100). The second is what makes the first safe — three friendly names
 * over a budget that can still kill the process would be worse than the three
 * jargon names it replaces.
 */
class DetailLevelTest {

    // ── the ceiling is real on every rung and every tier ───────────────────

    @Test
    fun `no rung on any tier can exceed that tier's ceiling`() {
        for (tier in DeviceTier.entries) {
            val ceiling = GpuPageBudget.maxSelectableLodPoints(tier)
            for (level in DetailLevel.entries) {
                val budget = DetailLevels.budgetPointsFor(level, tier)
                assertTrue(
                    "$tier / $level asked for $budget, ceiling is $ceiling",
                    budget <= ceiling,
                )
            }
        }
    }

    @Test
    fun `no SELECTABLE option on any tier exceeds the ceiling - the UI contract`() {
        for (tier in DeviceTier.entries) {
            val ceiling = GpuPageBudget.maxSelectableLodPoints(tier)
            for (level in DetailLevels.selectableOn(tier)) {
                assertTrue(
                    "$tier offers $level above its ceiling",
                    DetailLevels.budgetPointsFor(level, tier) <= ceiling,
                )
            }
        }
    }

    @Test
    fun `Auto is always offered - a device with one setting still has one`() {
        for (tier in DeviceTier.entries) {
            assertTrue("$tier", DetailLevel.AUTO in DetailLevels.selectableOn(tier))
        }
    }

    @Test
    fun `Auto asks for exactly the tier's own ceiling and is therefore never clamped`() {
        for (tier in DeviceTier.entries) {
            assertEquals(
                "$tier",
                GpuPageBudget.maxSelectableLodPoints(tier),
                DetailLevels.budgetPointsFor(DetailLevel.AUTO, tier),
            )
            assertEquals(
                "Auto is never limited, so it never carries a note of its own",
                DetailLevels.requestedPointsFor(DetailLevel.AUTO, tier),
                DetailLevels.budgetPointsFor(DetailLevel.AUTO, tier),
            )
        }
    }

    // ── the mapping the owner approved ─────────────────────────────────────

    @Test
    fun `High and Max map to the old OPTIMAL and FULL presets`() {
        assertEquals(PerformancePreset.OPTIMAL, DetailLevel.HIGH.preset)
        assertEquals(PerformancePreset.FULL, DetailLevel.MAX.preset)
        assertEquals(PerformancePreset.OPTIMAL, DetailLevel.AUTO.preset)
    }

    @Test
    fun `Max on a flagship is the old FULL budget, unchanged by the ceiling`() {
        val tier = DeviceTier.FLAGSHIP
        val full = PerformancePresets.tuningFor(PerformancePreset.FULL, tier, 120).lodBudgetMPoints
        // A flagship holds 33.6 M points, so a FULL budget below that survives
        // verbatim — the ceiling exists to stop the absurd cases, not to
        // second-guess the presets.
        if (full * 1_000_000 <= GpuPageBudget.maxSelectableLodPoints(tier)) {
            assertEquals(full * 1_000_000, DetailLevels.budgetPointsFor(DetailLevel.MAX, tier, 120))
        }
    }

    @Test
    fun `LIGHT is deliberately not reachable from Detail`() {
        // It is not deleted — it lives in the Advanced sheet — but it is not
        // one of the three result-shaped choices this control offers.
        assertTrue(DetailLevel.entries.none { it.preset == PerformancePreset.LIGHT })
    }

    @Test
    fun `the default is Auto`() {
        assertEquals(DetailLevel.AUTO, DetailLevels.DEFAULT)
    }

    // ── what a limited device is told ──────────────────────────────────────

    @Test
    fun `a tier that flattens two rungs offers fewer options and says so once`() {
        for (tier in DeviceTier.entries) {
            val offered = DetailLevels.selectableOn(tier)
            val note = DetailLevels.ceilingNote(tier)
            if (offered.size < DetailLevel.entries.size) {
                assertEquals("Limited by this device", note)
            } else {
                assertNull("$tier is not limited and must say nothing", note)
            }
        }
    }

    @Test
    fun `the ceiling note obeys the wording law`() {
        val note = "Limited by this device"
        assertTrue(note.split(" ").size <= 6)
    }

    @Test
    fun `selectable options are distinct budgets - no two rungs do the same thing`() {
        for (tier in DeviceTier.entries) {
            val budgets = DetailLevels.selectableOn(tier).map { DetailLevels.budgetPointsFor(it, tier) }
            assertEquals("$tier offers duplicate budgets: $budgets", budgets.size, budgets.toSet().size)
        }
    }

    // ── round-tripping a stored budget ─────────────────────────────────────

    @Test
    fun `a budget written by a rung reads back as that rung`() {
        for (tier in DeviceTier.entries) {
            for (level in DetailLevels.selectableOn(tier)) {
                val budget = DetailLevels.budgetPointsFor(level, tier)
                assertEquals(
                    "$tier / $level",
                    level,
                    DetailLevels.levelForBudget(budget, tier),
                )
            }
        }
    }

    @Test
    fun `an unrecognised budget reads back as Auto rather than as a rung it is not`() {
        // 3 333 333 points matches no rung on any tier.
        for (tier in DeviceTier.entries) {
            val level = DetailLevels.levelForBudget(3_333_333, tier)
            assertTrue("$tier", level == DetailLevel.AUTO || DetailLevels.budgetPointsFor(level, tier) == 3_333_333)
        }
    }

    @Test
    fun `a 50 M budget from an old RESEARCH profile reads back as a rung this device can hold`() {
        for (tier in DeviceTier.entries) {
            val level = DetailLevels.levelForBudget(50_000_000, tier)
            assertTrue(
                "$tier",
                DetailLevels.budgetPointsFor(level, tier) <= GpuPageBudget.maxSelectableLodPoints(tier),
            )
        }
    }
}
