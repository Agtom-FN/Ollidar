package com.lidarscan.core.capture

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 24 item 110(b) — **the step machine and the seen-flag.**
 *
 * Two things about a tour are worth a test rather than an inspection: that it
 * ENDS (a Next on the last step must close it, not wrap to the first, and not
 * sit there), and that the offer is made exactly once ever. Both are the kind
 * of rule that reads as obviously right in a composable and turns out, in the
 * field, to have been an off-by-one that shows a first-run prompt on every cold
 * start.
 */
class ScanTutorialTest {

    @Test
    fun `the tour is six steps, within item 110's ceiling`() {
        assertEquals(6, ScanTutorial.stepCount)
        assertTrue("item 110 caps the tour at eight steps", ScanTutorial.stepCount <= 8)
    }

    @Test
    fun `it starts on the scan button, which is the only control that must be understood`() {
        assertEquals(TutorialStep.SCAN_BUTTON, ScanTutorial.start().step)
        assertTrue(ScanTutorial.start().running)
    }

    @Test
    fun `next walks every step in order and then ends`() {
        var state = ScanTutorial.start()
        val seen = mutableListOf<TutorialStep>()
        var guard = 0
        while (state.running && guard++ < 50) {
            seen += state.step!!
            state = ScanTutorial.next(state)
        }
        assertEquals(TutorialStep.entries.toList(), seen)
        assertFalse("the last Next must END the tour", state.running)
        assertNull(state.step)
    }

    /** A Next that silently closes leaves someone wondering what they missed. */
    @Test
    fun `the last step's button says Done, not Next`() {
        var state = ScanTutorial.start()
        while (!state.isLastStep) state = ScanTutorial.next(state)
        assertEquals(TutorialStep.PROJECTS, state.step)
        assertEquals("Done", ScanTutorial.advanceLabel(state))
        assertEquals("Next", ScanTutorial.advanceLabel(ScanTutorial.start()))
    }

    @Test
    fun `skip ends the tour from any step`() {
        var state = ScanTutorial.start()
        repeat(3) { state = ScanTutorial.next(state) }
        assertTrue(state.running)
        assertFalse(ScanTutorial.skip().running)
    }

    @Test
    fun `advancing past the end is idempotent rather than a crash`() {
        val ended = ScanTutorial.next(TutorialState(TutorialStep.entries.last()))
        assertFalse(ended.running)
        assertFalse(ScanTutorial.next(ended).running)
    }

    @Test
    fun `the position label counts from one`() {
        assertEquals("1 of 6", ScanTutorial.progressLabel(ScanTutorial.start()))
        assertEquals("6 of 6", ScanTutorial.progressLabel(TutorialState(TutorialStep.PROJECTS)))
        assertEquals("", ScanTutorial.progressLabel(TutorialState(null)))
    }

    // ── the offer, exactly once ────────────────────────────────────────────

    @Test
    fun `the offer is made on a first launch and never again`() {
        assertTrue("a fresh install is offered the tour", ScanTutorial.shouldOffer(false, false))
        assertFalse("dismissing it is final", ScanTutorial.shouldOffer(false, true))
        assertFalse("seeing the tour retires the offer", ScanTutorial.shouldOffer(true, false))
        assertFalse(ScanTutorial.shouldOffer(true, true))
    }

    // ── the words ──────────────────────────────────────────────────────────

    /**
     * The tutorial is where the twenty-word explanations went, so it is the one
     * place most likely to quietly become the design document again. It obeys
     * exactly the same law as the screen it describes.
     */
    @Test
    fun `every step title is an instruction and every body is a detail`() {
        for (step in TutorialStep.entries) {
            assertTrue(
                "${step.name} title is ${WordingLaw.wordCount(step.title)} words: \"${step.title}\"",
                WordingLaw.isInstruction(step.title),
            )
            assertTrue(
                "${step.name} body is ${WordingLaw.wordCount(step.body)} words: \"${step.body}\"",
                WordingLaw.isDetail(step.body),
            )
        }
    }

    @Test
    fun `no tutorial string carries design-document jargon`() {
        for (line in ScanTutorial.INSTRUCTIONS + ScanTutorial.DETAILS) {
            assertTrue("jargon in \"$line\"", WordingLaw.jargonIn(line).isEmpty())
        }
    }

    /** The round-19 owner correction, restated over this round's new prose. */
    @Test
    fun `no tutorial string blames the light`() {
        for (line in ScanTutorial.INSTRUCTIONS + ScanTutorial.DETAILS) {
            val lower = line.lowercase()
            assertFalse(line, lower.contains("more light"))
            assertFalse(line, lower.contains("brighter"))
            assertFalse(line, lower.contains("dark room"))
        }
    }

    @Test
    fun `every chrome string obeys the law too`() {
        for (line in ScanTutorial.INSTRUCTIONS) {
            assertTrue("\"$line\" is ${WordingLaw.wordCount(line)} words", WordingLaw.isInstruction(line))
        }
        for (line in ScanTutorial.DETAILS) {
            assertTrue("\"$line\" is ${WordingLaw.wordCount(line)} words", WordingLaw.isDetail(line))
        }
    }

    /**
     * The step the tour exists for. scan-070 lost 4.1 s and 73° of turn because
     * nothing had ever told the operator to stand still.
     */
    @Test
    fun `the tracking step teaches the one thing that cannot be undone`() {
        val step = TutorialStep.TRACKING_LOST
        assertTrue(step.title.lowercase().contains("stop"))
        assertTrue(step.body.lowercase().contains("still"))
    }

    @Test
    fun `every anchor a step names is a real anchor`() {
        for (step in TutorialStep.entries) {
            assertTrue(step.anchor in TutorialAnchor.entries)
        }
        // The SCAN button earns two steps: how to start, and what the hold is.
        assertEquals(
            2,
            TutorialStep.entries.count { it.anchor == TutorialAnchor.SCAN_BUTTON },
        )
    }
}
