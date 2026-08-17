package com.lidarscan.core.capture

import com.lidarscan.core.render.LivePageStoreSizing
import com.lidarscan.core.render.RefreshGovernor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 6, owner items 21 + 22.
 *
 * The point of these tests is the owner's actual sentence — *"dont default to
 * use the max setting for the phone device which may cause crush"* — so most of
 * them are assertions that a default is **strictly below** what the control can
 * reach, on every device class, rather than assertions about specific numbers.
 */
class PerformancePresetTest {

    private val pixel8ProCeiling = 120
    private val budgetPhoneCeiling = 60

    // ── item 21: no default is a maximum ───────────────────────────────────

    @Test
    fun `the default preset is never the maximum on any device class`() {
        for (tier in DeviceTier.entries) {
            val optimal = PerformancePresets.tuningFor(PerformancePresets.DEFAULT, tier, pixel8ProCeiling)
            val full = PerformancePresets.tuningFor(PerformancePreset.FULL, tier, pixel8ProCeiling)

            assertTrue(
                "$tier: the default refresh cap must be a real cap, not uncapped",
                optimal.refreshHz > 0,
            )
            assertTrue(
                "$tier: the default refresh cap must be below the panel's own ceiling",
                optimal.refreshHz < pixel8ProCeiling,
            )
            assertTrue(
                "$tier: the default point budget must be below the slider's 20 M top",
                optimal.lodBudgetMPoints < 20,
            )
            assertTrue(
                "$tier: the default must cost less than Full's point budget",
                optimal.lodBudgetMPoints < full.lodBudgetMPoints,
            )
            assertTrue(
                "$tier: the default keyframe rate must be at or below Full's",
                optimal.keyframeRateFps <= full.keyframeRateFps,
            )
        }
    }

    @Test
    fun `Full on a modest phone is still capped - Full must not mean crash`() {
        val modest = PerformancePresets.tuningFor(PerformancePreset.FULL, DeviceTier.MODEST, pixel8ProCeiling)
        assertTrue("a modest phone never gets an uncapped live view", modest.refreshHz > 0)

        val flagship = PerformancePresets.tuningFor(PerformancePreset.FULL, DeviceTier.FLAGSHIP, pixel8ProCeiling)
        assertEquals("a flagship on Full is the one place 'Max' is offered", 0, flagship.refreshHz)
    }

    @Test
    fun `no preset can select a refresh rate the panel cannot reach`() {
        for (tier in DeviceTier.entries) {
            for (preset in PerformancePreset.entries.filter { it.isSelectable }) {
                val tuning = PerformancePresets.tuningFor(preset, tier, budgetPhoneCeiling)
                assertTrue(
                    "$preset on a $tier 60 Hz phone asked for ${tuning.refreshHz} fps",
                    tuning.refreshHz == 0 || tuning.refreshHz <= budgetPhoneCeiling,
                )
                assertTrue(
                    "$preset on $tier must land on a governor notch",
                    tuning.refreshHz == 0 || tuning.refreshHz in RefreshGovernor.NOTCHES,
                )
            }
        }
    }

    @Test
    fun `Light is the cheapest on every axis and draws no live map`() {
        for (tier in DeviceTier.entries) {
            val light = PerformancePresets.tuningFor(PerformancePreset.LIGHT, tier, pixel8ProCeiling)
            val optimal = PerformancePresets.tuningFor(PerformancePreset.OPTIMAL, tier, pixel8ProCeiling)
            assertTrue("$tier: Light draws raw preview only", !light.liveMapEnabled)
            assertTrue("$tier: Light writes no keyframes", !light.keyframesEnabled)
            assertTrue("$tier: Light's budget is the smallest", light.lodBudgetMPoints <= optimal.lodBudgetMPoints)
            assertTrue("$tier: Light's trail is the shortest", light.trailPoints <= optimal.trailPoints)
        }
    }

    // ── device tiering ─────────────────────────────────────────────────────

    @Test
    fun `device tiering separates a budget phone from a Pixel 8 Pro`() {
        assertEquals(
            DeviceTier.MODEST,
            PerformancePresets.tierFor(totalRamMb = 3_800, cpuCores = 8, displayCeilingHz = 60),
        )
        assertEquals(
            DeviceTier.MODEST,
            PerformancePresets.tierFor(totalRamMb = 8_000, cpuCores = 4, displayCeilingHz = 60),
        )
        assertEquals(
            DeviceTier.STANDARD,
            PerformancePresets.tierFor(totalRamMb = 6_000, cpuCores = 8, displayCeilingHz = 60),
        )
        assertEquals(
            "Pixel 8 Pro: 12 GB, 9 cores, 120 Hz",
            DeviceTier.FLAGSHIP,
            PerformancePresets.tierFor(totalRamMb = 11_500, cpuCores = 9, displayCeilingHz = 120),
        )
    }

    @Test
    fun `an unknown device reports STANDARD rather than guessing high`() {
        assertEquals(
            DeviceTier.STANDARD,
            PerformancePresets.tierFor(totalRamMb = 0, cpuCores = 0, displayCeilingHz = 0),
        )
    }

    // ── item 22: presets are starting points, not caps ─────────────────────

    @Test
    fun `moving one parameter away from a preset reads as CUSTOM, and nothing else changes`() {
        val tier = DeviceTier.FLAGSHIP
        val optimal = PerformancePresets.tuningFor(PerformancePreset.OPTIMAL, tier, pixel8ProCeiling)
        assertEquals(
            PerformancePreset.OPTIMAL,
            PerformancePresets.match(optimal, tier, pixel8ProCeiling),
        )

        val edited = optimal.copy(lodBudgetMPoints = optimal.lodBudgetMPoints + 3)
        assertEquals(
            "one edited slider means CUSTOM",
            PerformancePreset.CUSTOM,
            PerformancePresets.match(edited, tier, pixel8ProCeiling),
        )
        assertEquals("and nothing else moved", optimal.refreshHz, edited.refreshHz)
        assertEquals(optimal.keyframeRateFps, edited.keyframeRateFps)
    }

    @Test
    fun `every selectable preset round-trips through match`() {
        for (tier in DeviceTier.entries) {
            for (preset in PerformancePreset.entries.filter { it.isSelectable }) {
                val tuning = PerformancePresets.tuningFor(preset, tier, pixel8ProCeiling)
                assertEquals(
                    "$preset on $tier must be recognisable as itself",
                    preset,
                    PerformancePresets.match(tuning, tier, pixel8ProCeiling),
                )
            }
        }
    }

    @Test
    fun `switching preset reports exactly what it changed`() {
        val tier = DeviceTier.FLAGSHIP
        val optimal = PerformancePresets.tuningFor(PerformancePreset.OPTIMAL, tier, pixel8ProCeiling)
        val light = PerformancePresets.tuningFor(PerformancePreset.LIGHT, tier, pixel8ProCeiling)

        val changes = PerformancePresets.changes(optimal, light)
        assertTrue("Optimal -> Light must report the live map going off", changes.any { it.contains("live 3D map off") })
        assertTrue("and the point budget dropping", changes.any { it.contains("point budget") })
        assertTrue("and keyframes going off", changes.any { it.contains("camera keyframes off") })

        assertTrue(
            "a no-op switch reports nothing rather than an invented line",
            PerformancePresets.changes(optimal, optimal).isEmpty(),
        )
    }

    @Test
    fun `the keyframe-rate line is suppressed when keyframes are off entirely`() {
        val on = CaptureTuning(true, 30, 8, keyframesEnabled = true, keyframeRateFps = 3, trailEnabled = true, trailPoints = 600)
        val off = on.copy(keyframesEnabled = false, keyframeRateFps = 5)
        val changes = PerformancePresets.changes(on, off)
        assertTrue(changes.any { it.contains("camera keyframes off") })
        assertTrue(
            "quoting a rate change for a feature that is off is noise",
            changes.none { it.contains("keyframe rate") },
        )
    }

    @Test
    fun `Full carries a caution on the phones that need one, and none on a flagship`() {
        assertNotNull(PerformancePresets.cautionFor(PerformancePreset.FULL, DeviceTier.MODEST))
        assertNotNull(PerformancePresets.cautionFor(PerformancePreset.FULL, DeviceTier.STANDARD))
        assertNull(PerformancePresets.cautionFor(PerformancePreset.FULL, DeviceTier.FLAGSHIP))
        assertNull(PerformancePresets.cautionFor(PerformancePreset.OPTIMAL, DeviceTier.MODEST))
        assertNull(PerformancePresets.cautionFor(PerformancePreset.LIGHT, DeviceTier.MODEST))
    }

    // ── item 21: the live page store ───────────────────────────────────────

    @Test
    fun `every tier's page store is far smaller per page than the engine's desktop default`() {
        for (tier in DeviceTier.entries) {
            val sizing = LivePageStoreSizing.forTier(tier)
            assertTrue(
                "$tier: a 16 MB page wastes 16 MB on every D6 stream alternation",
                sizing.pageCapacityPoints < LivePageStoreSizing.ENGINE_DEFAULT.pageCapacityPoints,
            )
            assertTrue(
                "$tier: more pages than the engine's 64, so the map keeps growing longer",
                sizing.maxPages > LivePageStoreSizing.ENGINE_DEFAULT.maxPages,
            )
            assertTrue(
                "$tier: worst-case residency must stay well under the engine default's 1 GB",
                sizing.worstCaseBytes < LivePageStoreSizing.ENGINE_DEFAULT.worstCaseBytes,
            )
        }
    }

    @Test
    fun `the full-store note says which half of the app it costs`() {
        val note = LivePageStoreSizing.fullNote(LivePageStoreSizing.forTier(DeviceTier.STANDARD))
        assertTrue("it must name the live map", note.contains("Live map"))
        assertTrue("and it must exonerate the recording, in words", note.contains("Recording is unaffected"))
    }
}
