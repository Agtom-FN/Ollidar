package com.lidarscan.core.welcome

import com.lidarscan.core.Wording
import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 32 item 177 / **ROUND 34 items 181 + 183(a)** — **the two gates in
 * front of the welcome animations.**
 *
 * Each condition is a thing the owner asked for in a sentence rather than a
 * thing the code makes obvious. So each one is a test.
 */
class WelcomeAnimationTest {

    @Test
    fun `a normal cold launch plays A`() {
        assertEquals(
            WelcomeAnimation.Variant.LIDAR_FLIP,
            WelcomeAnimation.variantFor(WelcomeAnimation.Launch()),
        )
    }

    /**
     * **ROUND 34 item 181(a).** A cold launch plays A whoever is holding the
     * phone. Round 32 gave a developer B instead — which cost him the app's own
     * welcome film every morning and showed him the joke when he had not asked
     * for it.
     */
    @Test
    fun `a developer gets A on a cold launch too`() {
        assertEquals(
            WelcomeAnimation.Variant.LIDAR_FLIP,
            WelcomeAnimation.variantFor(WelcomeAnimation.Launch()),
        )
        assertNull(
            "B is not a launch film at all any more",
            WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = null, to = true)),
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
    }

    /** The Settings switch. Off means off — for A, which is what it governs. */
    @Test
    fun `the toggle off means never`() {
        assertNull(WelcomeAnimation.variantFor(WelcomeAnimation.Launch(enabled = false)))
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
            WelcomeAnimation.eggFor(
                WelcomeAnimation.DeveloperToggle(from = false, to = true, reducedMotion = true),
            ),
        )
    }

    // ══ ROUND 34 item 181(b) / 183(a) — the easter egg ═════════════════════

    /** The unlock, which is the one event that plays B. */
    @Test
    fun `switching developer mode on plays B`() {
        assertEquals(
            WelcomeAnimation.Variant.LLAMA_SPIT,
            WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = false, to = true)),
        )
    }

    /**
     * **Item 183(a), the owner's amendment.** Item 181 asked for both
     * directions and he changed his mind having watched it: the re-lock is
     * silent.
     */
    @Test
    fun `switching developer mode off is silent`() {
        assertNull(
            WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = true, to = false)),
        )
    }

    /**
     * The first reading after a process start is not a toggle.
     *
     * This is the whole reason the gate takes a PAIR rather than a value: the
     * app learns developer mode by collecting a store, and a gate written
     * against the value alone would fire the egg on every cold launch of a
     * developer's phone — the exact behaviour item 181 exists to remove.
     */
    @Test
    fun `the launch reading is never an egg`() {
        assertNull(WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = null, to = true)))
        assertNull(
            WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = null, to = false)),
        )
    }

    /** A settings emission that did not change developer mode is not a toggle. */
    @Test
    fun `an unchanged value is not a toggle`() {
        assertNull(WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = true, to = true)))
        assertNull(
            WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = false, to = false)),
        )
    }

    /**
     * **Item 181(d).** The "Welcome animation" switch governs A. The egg is
     * feedback for an action rather than a launch animation, so it plays with
     * the switch off — and the test says so, because a reader who saw
     * `enabled` missing from [WelcomeAnimation.DeveloperToggle] would otherwise
     * have to guess whether that was a decision or an oversight.
     */
    @Test
    fun `the welcome toggle does not govern the egg`() {
        assertNull(WelcomeAnimation.variantFor(WelcomeAnimation.Launch(enabled = false)))
        assertEquals(
            WelcomeAnimation.Variant.LLAMA_SPIT,
            WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = false, to = true)),
        )
    }

    /**
     * **Item 181(e).** Two unlocks in a row are two plays, and each one is
     * decided on its own. There is no queue in the gate because there is no
     * state in the gate: it is a function of the transition it is handed, and
     * what "the second replaces the first" means is settled by the caller
     * (`MainActivity`'s `key(playId)`), not here.
     */
    @Test
    fun `each transition is decided on its own`() {
        val first = WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = false, to = true))
        val relock = WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = true, to = false))
        val second = WelcomeAnimation.eggFor(WelcomeAnimation.DeveloperToggle(from = false, to = true))
        assertEquals(WelcomeAnimation.Variant.LLAMA_SPIT, first)
        assertNull(relock)
        assertEquals(WelcomeAnimation.Variant.LLAMA_SPIT, second)
    }

    /** Owner-approved at exactly three seconds, for both films. */
    @Test
    fun `both films are exactly three seconds`() {
        assertEquals(3_000, WelcomeAnimation.DURATION_MS)
    }

    /**
     * ROUND 35 item 187 — **the launch film is on screen for four seconds, and
     * the egg is not.**
     *
     * The owner asked for the film to run all the way through and then be held
     * for a second before the app appears. The hold belongs to the launch only:
     * the egg is feedback for a toggle, and a second of a pleased llama over a
     * Settings page nobody can touch is a second of a frozen app (item 187(e)).
     */
    @Test
    fun `the launch film holds a second before the app, and the egg does not`() {
        assertEquals(1_000, WelcomeAnimation.HOLD_MS)
        assertEquals(4_000, WelcomeAnimation.totalMsFor(WelcomeAnimation.Variant.LIDAR_FLIP))
        assertEquals(3_000, WelcomeAnimation.totalMsFor(WelcomeAnimation.Variant.LLAMA_SPIT))
    }

    /**
     * …and the hold is the film's **last frame**, held — not a second drawing.
     *
     * `filmProgress` reaches 1 exactly when the three seconds are up and stays
     * there for the rest of the four, which is the whole of the mechanism: one
     * description of the resting pose, and it is [WelcomeTimeline]'s own.
     */
    @Test
    fun `the hold is the film's last frame, clamped rather than redrawn`() {
        val a = WelcomeAnimation.Variant.LIDAR_FLIP
        assertEquals(0f, WelcomeAnimation.filmProgress(a, 0f), 1e-4f)
        assertEquals(0.5f, WelcomeAnimation.filmProgress(a, 0.375f), 1e-4f)
        assertEquals(1f, WelcomeAnimation.filmProgress(a, 0.75f), 1e-4f)
        for (p in listOf(0.75f, 0.8f, 0.9f, 1f)) {
            assertEquals("held at $p", 1f, WelcomeAnimation.filmProgress(a, p), 1e-4f)
        }
        // The egg has no hold, so its two clocks are the same clock.
        val b = WelcomeAnimation.Variant.LLAMA_SPIT
        assertEquals(0.6f, WelcomeAnimation.filmProgress(b, 0.6f), 1e-4f)
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
