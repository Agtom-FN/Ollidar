package com.lidarscan.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 28 item 169 — **the exemption that let "A15" onto the tab bar.**
 *
 * The design review's finding J6, in full: `WordingLaw` exempted "advanced
 * screens" from the [WordingLaw.JARGON] check; `JARGON` listed `"A15"` by name;
 * and the **Jobs** tab — one of the four screens in the primary tab bar, which
 * no ordinary operator opted into — printed *"Nothing queued. The queue runs
 * one job at a time — that is A15's design, not a limit of this screen."* The
 * law named the word and the exemption let it through, because the exemption
 * was a sentence in a KDoc comment rather than anything a compiler or a test
 * could see.
 *
 * This is that exemption with a shape. The two tests that matter are the first
 * two: the real string must fail, and no tab-bar screen must be able to claim
 * the pass that would have saved it.
 */
class Round28WordingLawTest {

    /** The string that actually shipped, on the screen it actually shipped on. */
    private val theStringThatShipped =
        "Nothing queued. The queue runs one job at a time — that is A15's design, " +
            "not a limit of this screen."

    @Test
    fun `the Jobs tab's A15 sentence FAILS the law`() {
        val faults = WordingLaw.violations(theStringThatShipped, WordingLaw.TabBarScreen.JOBS)
        assertFalse("the guard must fail the string it replaced", faults.isEmpty())
        assertTrue(faults.toString(), faults.any { it.contains("A15") })
        assertTrue(faults.toString(), faults.any { it.contains("words") })
    }

    /**
     * The structural half. A boolean flag would have let the next Jobs screen
     * set it to `true`; [WordingLaw.TabBarScreen] cannot, because
     * `jargonChecked` is a constant on the type rather than a constructor
     * parameter on the member.
     */
    @Test
    fun `no tab-bar screen can claim the advanced exemption`() {
        for (screen in WordingLaw.TabBarScreen.entries) {
            assertTrue("$screen must be jargon-checked", screen.jargonChecked)
            assertEquals("$screen must not get the paragraph budget", WordingLaw.MAX_DETAIL_WORDS, screen.maxWords)
            assertFalse(
                "$screen let A15 through",
                WordingLaw.passes(theStringThatShipped, screen),
            )
        }
    }

    /**
     * The exemption is not deleted — it is confined. RTK and the calibration
     * wizard are for someone who navigated there on purpose, and "CRS" is the
     * right word to use with them (this file's parent KDoc, unchanged since
     * round 22).
     */
    @Test
    fun `an advanced screen still gets the lighter pass`() {
        for (screen in WordingLaw.AdvancedScreen.entries) {
            assertFalse("$screen", screen.jargonChecked)
            assertEquals(WordingLaw.MAX_ADVANCED_WORDS, screen.maxWords)
        }
        assertTrue(WordingLaw.passes("Set the CRS before exporting.", WordingLaw.AdvancedScreen.RTK))
        // But the paragraph ceiling still applies: a lighter pass, not none.
        assertFalse(
            WordingLaw.passes(theStringThatShipped + " " + theStringThatShipped, WordingLaw.AdvancedScreen.RTK),
        )
    }

    /**
     * The replacement copy, per §D.6. Checked here rather than trusted, because
     * "No jobs yet" being three words is the entire point of the rewrite.
     */
    @Test
    fun `the Jobs empty state that replaced it passes on the tab bar`() {
        val title = "No jobs yet"
        val body = "Exports and reprocessing appear here."
        assertTrue(WordingLaw.isInstruction(title))
        assertTrue(WordingLaw.passes(title, WordingLaw.TabBarScreen.JOBS))
        assertTrue(WordingLaw.passes(body, WordingLaw.TabBarScreen.JOBS))
        assertEquals(3, WordingLaw.wordCount(title))
        assertEquals(5, WordingLaw.wordCount(body))
    }

    /** [WordingLaw.violations] has to name the offence, or a red build says nothing. */
    @Test
    fun `a violation names what is wrong`() {
        assertEquals(
            listOf("jargon: §"),
            WordingLaw.violations("See §3.12.", WordingLaw.TabBarScreen.SETTINGS),
        )
        assertTrue(
            WordingLaw.violations(
                "one two three four five six seven eight nine ten eleven twelve thirteen",
                WordingLaw.TabBarScreen.PROJECTS,
            ).single().contains("13 words"),
        )
    }
}
