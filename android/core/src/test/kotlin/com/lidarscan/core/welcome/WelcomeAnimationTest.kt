package com.lidarscan.core.welcome

import com.lidarscan.core.Wording
import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 32 item 177 — **the gate in front of the welcome animation.**
 *
 * Four conditions, and each one of them is a thing the owner asked for in a
 * sentence rather than a thing the code makes obvious. So each one is a test.
 */
class WelcomeAnimationTest {

    @Test
    fun `a normal cold launch plays A`() {
        assertEquals(
            WelcomeAnimation.Variant.LIDAR_FLIP,
            WelcomeAnimation.variantFor(WelcomeAnimation.Launch()),
        )
    }

    @Test
    fun `developer mode selects B instead of A`() {
        assertEquals(
            WelcomeAnimation.Variant.LLAMA_SPIT,
            WelcomeAnimation.variantFor(WelcomeAnimation.Launch(developerMode = true)),
        )
    }

    /** "Cold LAUNCH only (process start), not tab switches, not rotations." */
    @Test
    fun `it plays once per process and never again`() {
        val launch = WelcomeAnimation.Launch()
        assertEquals(WelcomeAnimation.Variant.LIDAR_FLIP, WelcomeAnimation.variantFor(launch))
        assertNull(
            "a second composition in the same process must not replay",
            WelcomeAnimation.variantFor(launch.copy(firstInProcess = false)),
        )
        assertNull(
            "…including with developer mode on",
            WelcomeAnimation.variantFor(launch.copy(firstInProcess = false, developerMode = true)),
        )
    }

    /** The Settings switch. Off means off, in both variants. */
    @Test
    fun `the toggle off means never`() {
        assertNull(WelcomeAnimation.variantFor(WelcomeAnimation.Launch(enabled = false)))
        assertNull(
            WelcomeAnimation.variantFor(WelcomeAnimation.Launch(enabled = false, developerMode = true)),
        )
    }

    /**
     * The reduced-motion gate, and the reason it is a *skip* rather than a
     * freeze: a static frame held for three seconds costs the operator the
     * same three seconds and buys nothing.
     */
    @Test
    fun `reduced motion skips entirely, in both variants`() {
        assertNull(WelcomeAnimation.variantFor(WelcomeAnimation.Launch(reducedMotion = true)))
        assertNull(
            WelcomeAnimation.variantFor(
                WelcomeAnimation.Launch(reducedMotion = true, developerMode = true),
            ),
        )
    }

    /** Owner-approved at exactly three seconds, for both films. */
    @Test
    fun `both films are exactly three seconds`() {
        assertEquals(3_000, WelcomeAnimation.DURATION_MS)
    }

    /**
     * Settings is a **tab-bar** screen, so it gets no exemption at all
     * (round 28 item 169). The row's two strings are checked here rather than
     * eyeballed, for the reason that item exists.
     */
    @Test
    fun `the Settings row obeys the wording law`() {
        val screen = WordingLaw.TabBarScreen.SETTINGS
        assertTrue(Wording.WELCOME_TITLE, WordingLaw.passes(Wording.WELCOME_TITLE, screen))
        assertTrue(Wording.WELCOME_TITLE, WordingLaw.isInstruction(Wording.WELCOME_TITLE))
        assertTrue(Wording.WELCOME_DETAIL, WordingLaw.passes(Wording.WELCOME_DETAIL, screen))
        assertEquals(2, WordingLaw.wordCount(Wording.WELCOME_TITLE))
        assertEquals(6, WordingLaw.wordCount(Wording.WELCOME_DETAIL))
    }
}
