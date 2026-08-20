package com.lidarscan.core

import com.lidarscan.core.render.DisplayProfile
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 23 item 106d — **the Survey and Research chips obey the switch that
 * claims to hide them.**
 *
 * Round 22 decided Survey and Research are Advanced features; the Review
 * screen's Display panel went on drawing `DisplayProfile.entries`, so both were
 * one tap away with Advanced off. That is not cosmetic: applying the Survey
 * profile is what arms the GNSS capture gate the owner never asked for, and
 * Research is the 50 M-point budget item 100 exists to clamp.
 *
 * The filter is here, in `:core`, rather than as three conditions inside a
 * Compose `forEach`, so this is the only place that has to be right.
 */
class SimpleModeDisplayProfileTest {

    @Test
    fun `simple mode offers only Quick scan`() {
        assertEquals(listOf(DisplayProfile.QUICK_SCAN), SimpleMode.displayProfiles(advanced = false))
    }

    @Test
    fun `advanced mode offers every profile, in the enum's own order`() {
        assertEquals(DisplayProfile.entries.toList(), SimpleMode.displayProfiles(advanced = true))
    }

    @Test
    fun `the chip filter agrees with the predicates it is built from`() {
        for (advanced in listOf(false, true)) {
            assertEquals(
                SimpleMode.showsSurveyProfile(advanced),
                SimpleMode.showsDisplayProfile(advanced, DisplayProfile.SURVEY),
            )
            assertEquals(
                SimpleMode.showsResearchProfile(advanced),
                SimpleMode.showsDisplayProfile(advanced, DisplayProfile.RESEARCH),
            )
            assertEquals(
                SimpleMode.showsFloorPlan(advanced),
                SimpleMode.showsDisplayProfile(advanced, DisplayProfile.FLOOR_PLAN),
            )
        }
    }

    @Test
    fun `Quick scan is never hidden — a panel with no chips is a dangling heading`() {
        assertTrue(SimpleMode.showsDisplayProfile(advanced = false, profile = DisplayProfile.QUICK_SCAN))
        assertTrue(SimpleMode.showsDisplayProfile(advanced = true, profile = DisplayProfile.QUICK_SCAN))
    }

    @Test
    fun `the hidden profiles really are hidden — the regression this test exists for`() {
        val simple = SimpleMode.displayProfiles(advanced = false)
        assertFalse(simple.contains(DisplayProfile.SURVEY))
        assertFalse(simple.contains(DisplayProfile.RESEARCH))
        assertFalse(simple.contains(DisplayProfile.FLOOR_PLAN))
        assertTrue("the panel must never end up with zero chips", simple.isNotEmpty())
    }

    @Test
    fun `every profile is decided — a new enum value cannot fall through`() {
        // `when` over the enum is exhaustive; this pins the pairing so that a
        // fifth profile added in a later round has to be classified here.
        for (p in DisplayProfile.entries) {
            SimpleMode.showsDisplayProfile(advanced = false, profile = p)
        }
        assertEquals(4, DisplayProfile.entries.size)
    }
}
