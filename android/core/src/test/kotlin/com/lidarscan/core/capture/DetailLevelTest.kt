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

    // ── ROUND 29 item 172: the ladder ascends ──────────────────────────────

    @Test
    fun `Max asks for exactly the tier's own ceiling and is therefore never clamped`() {
        for (tier in DeviceTier.entries) {
            assertEquals(
                "$tier",
                GpuPageBudget.maxSelectableLodPoints(tier),
                DetailLevels.budgetPointsFor(DetailLevel.MAX, tier),
            )
            assertEquals(
                "Max is the ceiling itself, so the clamp is a no-op on it",
                DetailLevels.requestedPointsFor(DetailLevel.MAX, tier),
                DetailLevels.budgetPointsFor(DetailLevel.MAX, tier),
            )
        }
    }

    /**
     * **The regression pin for item 172.** The owner's report was
     * *"detail of auto is 16M while high is 5M"* — Auto had been given Max's
     * meaning, so the ladder ran downhill. Reverting `AUTO` to the ceiling
     * fails this on every tier.
     */
    @Test
    fun `Auto is at or below High, and High is at or below Max, on every tier`() {
        for (tier in DeviceTier.entries) {
            val auto = DetailLevels.budgetPointsFor(DetailLevel.AUTO, tier)
            val high = DetailLevels.budgetPointsFor(DetailLevel.HIGH, tier)
            val max = DetailLevels.budgetPointsFor(DetailLevel.MAX, tier)
            assertTrue("$tier: Auto $auto > High $high", auto <= high)
            assertTrue("$tier: High $high > Max $max", high <= max)
            assertTrue("$tier: Auto and Max must not be the same rung", auto < max)
        }
    }

    @Test
    fun `the offered rungs read out in ascending order`() {
        for (tier in DeviceTier.entries) {
            val budgets = DetailLevels.selectableOn(tier).map { DetailLevels.budgetPointsFor(it, tier) }
            assertEquals("$tier offers $budgets", budgets.sorted(), budgets)
        }
    }

    @Test
    fun `Auto is the tier's own OPTIMAL recommendation - the adaptive rung`() {
        for (tier in DeviceTier.entries) {
            val optimal = PerformancePresets
                .tuningFor(PerformancePreset.OPTIMAL, tier, 60).lodBudgetMPoints * 1_000_000
            assertEquals("$tier", optimal, DetailLevels.budgetPointsFor(DetailLevel.AUTO, tier))
        }
    }

    // ── what each rung reads out ───────────────────────────────────────────

    @Test
    fun `Auto prints no number at all - it does not have one`() {
        for (tier in DeviceTier.entries) {
            assertEquals("$tier", "Fits this device", DetailLevels.readoutFor(DetailLevel.AUTO, tier))
        }
        assertEquals("Fits this device", DetailLevels.AUTO_READOUT)
    }

    @Test
    fun `every other rung prints the budget it actually applies`() {
        for (tier in DeviceTier.entries) {
            for (level in DetailLevel.entries - DetailLevel.AUTO) {
                val millions = DetailLevels.budgetPointsFor(level, tier) / 1_000_000
                assertEquals("$tier / $level", "$millions M", DetailLevels.readoutFor(level, tier))
            }
        }
    }

    @Test
    fun `the readout obeys the wording law`() {
        for (tier in DeviceTier.entries) {
            for (level in DetailLevel.entries) {
                assertTrue(
                    "$tier / $level",
                    com.lidarscan.core.WordingLaw.isInstruction(DetailLevels.readoutFor(level, tier)),
                )
            }
        }
    }

    // ── the mapping the owner approved ─────────────────────────────────────

    @Test
    fun `Auto drives OPTIMAL and the two upper rungs drive FULL`() {
        assertEquals(PerformancePreset.OPTIMAL, DetailLevel.AUTO.preset)
        assertEquals(PerformancePreset.FULL, DetailLevel.HIGH.preset)
        assertEquals(PerformancePreset.FULL, DetailLevel.MAX.preset)
    }

    @Test
    fun `High on a flagship is the old FULL budget, unchanged by the ceiling`() {
        val tier = DeviceTier.FLAGSHIP
        val full = PerformancePresets.tuningFor(PerformancePreset.FULL, tier, 120).lodBudgetMPoints
        // A flagship holds 33.6 M points, so a FULL budget below that survives
        // verbatim — the ceiling exists to stop the absurd cases, not to
        // second-guess the presets.
        if (full * 1_000_000 <= GpuPageBudget.maxSelectableLodPoints(tier)) {
            assertEquals(full * 1_000_000, DetailLevels.budgetPointsFor(DetailLevel.HIGH, tier, 120))
        }
    }

    /** A flagship is the one tier with three genuinely different rungs. */
    @Test
    fun `a flagship offers all three rungs and a standard phone offers two`() {
        assertEquals(3, DetailLevels.selectableOn(DeviceTier.FLAGSHIP).size)
        assertEquals(
            listOf(DetailLevel.AUTO, DetailLevel.HIGH),
            DetailLevels.selectableOn(DeviceTier.STANDARD),
        )
        assertEquals("Limited by this device", DetailLevels.ceilingNote(DeviceTier.STANDARD))
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

    /**
     * **The regression pin for the round-trip.** The budget is persisted as an
     * Int in MILLIONS, so a rung whose budget is 16 777 216 points is written
     * as `16` and must still read back as that rung. Comparing exact points
     * fails this on `HIGH`/`MAX` for every tier whose ceiling is not a round
     * number of millions — which is all three of them.
     */
    @Test
    fun `a rung survives the round trip through the millions-resolution store`() {
        for (tier in DeviceTier.entries) {
            for (level in DetailLevels.selectableOn(tier)) {
                val stored = DetailLevels.budgetPointsFor(level, tier) / 1_000_000
                assertEquals(
                    "$tier / $level stored as $stored M",
                    level,
                    DetailLevels.levelForBudget(stored * 1_000_000, tier),
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
