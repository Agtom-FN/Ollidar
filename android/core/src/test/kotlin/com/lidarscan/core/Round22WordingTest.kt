package com.lidarscan.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 22 item 98 — **the wording guard, in the shape of the round-19 light
 * guard, and for the same reason.**
 *
 * Round 19 put an owner correction ("my sessions were in good light — stop
 * blaming light") into a test that fails the build. It has held ever since,
 * because a rule that only a document knows is a rule that comes back at the
 * next deadline.
 *
 * This is the same mechanism for the same class of problem: instructions
 * written by people who already know how the app works. Every operator-facing
 * sentence lives in [Wording]; every one of them is checked here.
 */
class Round22WordingTest {

    @Test
    fun `every instruction is six words or fewer`() {
        for (line in Wording.INSTRUCTIONS) {
            assertTrue(
                "instruction is ${WordingLaw.wordCount(line)} words " +
                    "(max ${WordingLaw.MAX_INSTRUCTION_WORDS}): \"$line\"",
                WordingLaw.isInstruction(line),
            )
        }
    }

    @Test
    fun `every detail line is twelve words or fewer`() {
        for (line in Wording.DETAILS) {
            assertTrue(
                "detail is ${WordingLaw.wordCount(line)} words " +
                    "(max ${WordingLaw.MAX_DETAIL_WORDS}): \"$line\"",
                WordingLaw.isDetail(line),
            )
        }
    }

    @Test
    fun `every error says what happened AND what to do`() {
        for (line in Wording.ERRORS) {
            assertTrue(
                "an error must end with something the operator can DO: \"$line\"",
                WordingLaw.isActionable(line),
            )
        }
    }

    @Test
    fun `no operator-facing string carries design-document jargon`() {
        for (line in Wording.INSTRUCTIONS + Wording.DETAILS + Wording.ERRORS) {
            val found = WordingLaw.jargonIn(line)
            assertTrue("jargon $found in \"$line\"", found.isEmpty())
        }
    }

    /**
     * The round-19 correction, restated over the round-22 strings. It is
     * checked in `Round19CoverageAdviceTest` for the advice chain; this is the
     * same rule applied to every sentence the simplification introduced, so a
     * reworded string cannot quietly bring it back.
     */
    @Test
    fun `no round-22 string blames light — the round-19 correction still stands`() {
        for (line in Wording.INSTRUCTIONS + Wording.DETAILS + Wording.ERRORS) {
            val lower = line.lowercase()
            assertTrue("blames light: \"$line\"", !lower.contains("more light"))
            assertTrue("blames light: \"$line\"", !lower.contains("lights"))
            assertTrue("blames light: \"$line\"", !lower.contains("brighter"))
            assertTrue("blames light: \"$line\"", !lower.contains("dark room"))
        }
    }

    // ── the specific rewrites the owner's brief named ──────────────────────

    @Test
    fun `the multi-piece card reports rather than asking - the fix auto-runs now`() {
        assertEquals("Scan is in 4 pieces. Fixing…", Wording.scanInPieces(4))
        assertTrue(WordingLaw.isDetail(Wording.scanInPieces(12)))
    }

    @Test
    fun `the walked-path row is called Show my path`() {
        assertEquals("Show my path", Wording.SHOW_MY_PATH)
    }

    @Test
    fun `the empty-Projects text does not explain the tab structure`() {
        // was 26 words naming two products and a tab.
        assertTrue(WordingLaw.isInstruction(Wording.PROJECTS_EMPTY_HINT))
        assertTrue(!Wording.PROJECTS_EMPTY_HINT.contains("COIN-D6"))
        assertTrue(!Wording.PROJECTS_EMPTY_HINT.contains("Mid-360"))
        assertTrue(!Wording.PROJECTS_EMPTY_HINT.contains("project"))
    }

    @Test
    fun `the delete dialog does not mention the on-disk format`() {
        assertTrue(!Wording.DELETE_BODY.contains(".lscan"))
        assertTrue(!Wording.DELETE_BODY.lowercase().contains("directory"))
        assertTrue(!Wording.DELETE_BODY.lowercase().contains("stream"))
        assertTrue(WordingLaw.isDetail(Wording.DELETE_BODY))
    }

    @Test
    fun `the detail-budget explainer does not describe the renderer`() {
        assertTrue(!Wording.DETAIL_BUDGET_HINT.contains("GPU"))
        assertTrue(!Wording.DETAIL_BUDGET_HINT.lowercase().contains("page"))
        assertTrue(!Wording.DETAIL_BUDGET_HINT.lowercase().contains("decimat"))
        assertTrue(WordingLaw.isInstruction(Wording.DETAIL_BUDGET_HINT))
    }

    @Test
    fun `the measure hint says what to do, and the detail admits the error`() {
        assertTrue(WordingLaw.isInstruction(Wording.MEASURE_HINT))
        assertTrue(WordingLaw.isActionable(Wording.MEASURE_HINT))
        // Honesty about the sampled pick survives the shortening — the operator
        // still learns the number can be a few centimetres out.
        assertTrue(Wording.MEASURE_DETAIL.contains("centimetres"))
    }

    @Test
    fun `the export error names the cause and the next tap`() {
        val line = Wording.exportFailed("no room on the phone")
        assertTrue(line, line.contains("no room on the phone"))
        assertTrue(line, WordingLaw.isActionable(line))
    }

    // ── the law itself ─────────────────────────────────────────────────────

    @Test
    fun `punctuation is not counted as a word`() {
        // "Tracking is drifting — hold on." is five words and one mark.
        assertEquals(5, WordingLaw.wordCount("Tracking is drifting — hold on."))
        assertEquals(4, WordingLaw.wordCount("Limited by this device"))
        assertEquals(0, WordingLaw.wordCount("  —  ·  "))
    }

    @Test
    fun `the law catches a real over-long instruction`() {
        val old = "Scans are created in the Capture tab: plug in the COIN-D6 or the " +
            "Mid-360 and it connects itself, then Start records into a new project."
        assertTrue("the guard must fail the string it replaced", !WordingLaw.isInstruction(old))
        assertTrue(!WordingLaw.isDetail(old))
    }

    @Test
    fun `the law catches jargon that really shipped`() {
        assertEquals(listOf("CRS"), WordingLaw.jargonIn("Set the CRS before exporting."))
        assertEquals(listOf("§"), WordingLaw.jargonIn("See §3.12 for the budget."))
        assertTrue(WordingLaw.jargonIn("A clean scan of the room.").isEmpty())
    }

    @Test
    fun `a bare failure is not an error message`() {
        assertTrue("\"Export failed.\" is half a sentence", !WordingLaw.isActionable("Export failed."))
        assertTrue(WordingLaw.isActionable("Export failed. Tap Export to retry."))
    }
}
