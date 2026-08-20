package com.lidarscan.core

import com.lidarscan.core.capture.ScanTutorial
import com.lidarscan.core.feedback.FeedbackWording
import com.lidarscan.core.projects.ProjectsView
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 24 item 110(a) — **the wording law, over the Scan screen at last.**
 *
 * Round 22 wrote the law and applied it where the owner had complained:
 * Projects, Review, export, delete. The Scan screen — the one he actually
 * looks at while walking — kept a dozen ROUND 5/6 sentences that were accurate,
 * useful the first time, and read on every single scan afterwards. This test is
 * the same guard `Round22WordingTest` is, extended over the strings this round
 * shortened and the two new vocabularies (the tutorial and the feedback
 * sender), so none of them can drift back into prose.
 */
class Round24WordingTest {

    /** Everything ROUND 24 adds, in one list, so nothing is guarded by accident. */
    private val newInstructions: List<String> =
        ScanTutorial.INSTRUCTIONS + FeedbackWording.ALL + ProjectsView.ALL

    private val newDetails: List<String> = ScanTutorial.DETAILS + FeedbackWording.DETAILS

    @Test
    fun `every new instruction is six words or fewer`() {
        for (line in newInstructions) {
            assertTrue(
                "instruction is ${WordingLaw.wordCount(line)} words " +
                    "(max ${WordingLaw.MAX_INSTRUCTION_WORDS}): \"$line\"",
                WordingLaw.isInstruction(line),
            )
        }
    }

    @Test
    fun `every new detail line is twelve words or fewer`() {
        for (line in newDetails) {
            assertTrue(
                "detail is ${WordingLaw.wordCount(line)} words " +
                    "(max ${WordingLaw.MAX_DETAIL_WORDS}): \"$line\"",
                WordingLaw.isDetail(line),
            )
        }
    }

    @Test
    fun `no new string carries design-document jargon`() {
        for (line in newInstructions + newDetails) {
            assertTrue("jargon ${WordingLaw.jargonIn(line)} in \"$line\"", WordingLaw.jargonIn(line).isEmpty())
        }
    }

    // ── the specific Scan-page rewrites ────────────────────────────────────

    /**
     * The three worst offenders on the Scan screen, each with the sentence it
     * replaces recorded in [Wording]. The assertion is not merely "shorter" —
     * it is that the words which meant nothing to an operator are GONE.
     */
    @Test
    fun `the D6 mount hint no longer explains the pipeline`() {
        assertTrue(WordingLaw.isInstruction(Wording.D6_MOUNT_HINT))
        assertFalse(Wording.D6_MOUNT_HINT.contains("6-DoF"))
        assertFalse(Wording.D6_MOUNT_HINT.contains("IMU"))
        assertFalse(Wording.D6_MOUNT_HINT.lowercase().contains("engine"))
    }

    @Test
    fun `the mount reference note stops naming the bracket's CAD nominal`() {
        for (line in listOf(Wording.MOUNT_REF_MISSING, Wording.MOUNT_REF_HINT, Wording.MOUNT_REF_WHY)) {
            assertFalse(line, line.contains("CAD"))
            assertFalse(line, line.lowercase().contains("pushbroom"))
            assertFalse(line, line.lowercase().contains("attitude"))
        }
        assertTrue(WordingLaw.isInstruction(Wording.MOUNT_REF_MISSING))
        assertTrue(WordingLaw.isInstruction(Wording.MOUNT_REF_HINT))
        assertTrue(WordingLaw.isDetail(Wording.MOUNT_REF_WHY))
    }

    @Test
    fun `the no-tracking warning says what it costs, not what a fan slice is`() {
        assertFalse(Wording.NO_TRACKING_HINT.lowercase().contains("fan slice"))
        assertFalse(Wording.NO_TRACKING_HINT.contains("ARCore"))
        assertTrue(WordingLaw.isInstruction(Wording.NO_TRACKING_HINT))
        assertTrue(WordingLaw.isInstruction(Wording.NO_TRACKING_DETAIL))
    }

    @Test
    fun `the AR-degraded hint is an instruction plus one detail, not a paragraph`() {
        assertTrue(WordingLaw.isInstruction(Wording.AR_DEGRADED))
        assertTrue(WordingLaw.isDetail(Wording.AR_DEGRADED_DETAIL))
        assertFalse(Wording.AR_DEGRADED_DETAIL.contains("COIN-D6"))
    }

    @Test
    fun `the new-capture dialog fits on a phone`() {
        assertTrue(WordingLaw.isInstruction(Wording.NEW_CAPTURE_TITLE))
        assertTrue(WordingLaw.isDetail(Wording.NEW_CAPTURE_BODY))
        assertTrue(WordingLaw.isInstruction(Wording.NEW_CAPTURE_CONFIRM))
        assertTrue(WordingLaw.isInstruction(Wording.NEW_CAPTURE_DISMISS))
    }

    @Test
    fun `the swallowed-press answer is four words`() {
        assertEquals("Heard you. Already starting…", Wording.START_HEARD_YOU)
        assertTrue(WordingLaw.isInstruction(Wording.START_HEARD_YOU))
    }

    /**
     * The round-19 correction: the start instruction may say what to point at,
     * and may never say anything about how bright the room is.
     */
    @Test
    fun `the start instruction names furniture and never light`() {
        assertTrue(WordingLaw.isInstruction(Wording.START_LOOK_AT))
        val lower = Wording.START_LOOK_AT.lowercase()
        assertFalse(lower.contains("light"))
        assertFalse(lower.contains("bright"))
    }

    @Test
    fun `the manual-entry panel stops explaining USB`() {
        assertTrue(WordingLaw.isInstruction(Wording.NO_USB_DEVICE))
        assertTrue(WordingLaw.isDetail(Wording.NO_USB_DEVICE_DETAIL))
        assertFalse(Wording.NO_USB_DEVICE.contains("OTG"))
        assertTrue(WordingLaw.isInstruction(Wording.LIVE_VIEW_IS_THE_PROOF))
        assertFalse(Wording.LIVE_VIEW_IS_THE_PROOF.lowercase().contains("self-test"))
    }

    /**
     * The ROUND 22 guard walks `Wording.INSTRUCTIONS`; a string that is not in
     * that list is not guarded at all. Every constant this round added must be
     * in one of the two lists, or the law simply does not apply to it — which
     * is exactly how the Scan screen's dozen sentences escaped round 22.
     */
    @Test
    fun `every round-24 Wording constant is actually under guard`() {
        val guarded = (Wording.INSTRUCTIONS + Wording.DETAILS).toSet()
        val added = listOf(
            Wording.D6_MOUNT_HINT,
            Wording.D6_MOUNT_DETAIL,
            Wording.MOUNT_REF_HINT,
            Wording.MOUNT_REF_DETAIL,
            Wording.MOUNT_REF_MISSING,
            Wording.MOUNT_REF_WHY,
            Wording.NO_TRACKING_HINT,
            Wording.NO_TRACKING_DETAIL,
            Wording.AR_DEGRADED,
            Wording.AR_DEGRADED_DETAIL,
            Wording.NEW_CAPTURE_TITLE,
            Wording.NEW_CAPTURE_BODY,
            Wording.NEW_CAPTURE_CONFIRM,
            Wording.NEW_CAPTURE_DISMISS,
            Wording.START_HEARD_YOU,
            Wording.START_LOOK_AT,
            Wording.NO_USB_DEVICE,
            Wording.NO_USB_DEVICE_DETAIL,
            Wording.LIVE_VIEW_IS_THE_PROOF,
        )
        for (line in added) {
            assertTrue("\"$line\" is not in Wording.INSTRUCTIONS or Wording.DETAILS", line in guarded)
        }
    }

    /**
     * The Do Not Disturb explainer and its Scan-screen note.
     *
     * They are `CaptureFocus`'s, not `Wording`'s, which is exactly how they
     * escaped round 22 — the guard walks one object and these live in another.
     * The dialog is the FIRST thing a new operator sees on the Scan tab, so it
     * gets the law: each paragraph an instruction plus at most one detail, and
     * the two clauses the ROUND 14 tests pin are kept verbatim.
     */
    @Test
    fun `the Do Not Disturb explainer is three short lines, not three paragraphs`() {
        val body = com.lidarscan.core.capture.CaptureFocus.ASK_BODY
        assertTrue(
            "the whole dialog is ${WordingLaw.wordCount(body)} words",
            WordingLaw.wordCount(body) <= WordingLaw.MAX_ADVANCED_WORDS,
        )
        for (line in body.split("\n").filter { it.isNotBlank() }) {
            assertTrue(
                "\"$line\" is ${WordingLaw.wordCount(line)} words",
                WordingLaw.isDetail(line),
            )
        }
        // ROUND 14's two load-bearing clauses survive the shortening.
        assertTrue(body.contains("shakes the phone"))
        assertTrue(body.contains("Scans still run without this"))
        assertTrue(WordingLaw.isInstruction(com.lidarscan.core.capture.CaptureFocus.ASK_TITLE))
        assertTrue(WordingLaw.isInstruction(com.lidarscan.core.capture.CaptureFocus.ASK_CONFIRM))
        assertTrue(WordingLaw.isInstruction(com.lidarscan.core.capture.CaptureFocus.ASK_DISMISS))
    }

    @Test
    fun `the Do Not Disturb status and note obey the law`() {
        for (granted in listOf(true, false)) {
            val line = com.lidarscan.core.capture.CaptureFocus.accessStatus(granted)
            assertTrue("\"$line\" is ${WordingLaw.wordCount(line)} words", WordingLaw.isDetail(line))
        }
        for (state in com.lidarscan.core.capture.DndState.entries) {
            val note = com.lidarscan.core.capture.CaptureFocus.note(state) ?: continue
            for (line in note.split("\n")) {
                assertTrue(
                    "\"$line\" is ${WordingLaw.wordCount(line)} words",
                    WordingLaw.isInstruction(line),
                )
            }
        }
    }

    /**
     * The auto-detect failure line — the first sentence on the Scan tab of a
     * phone with nothing plugged into it, which makes it the first sentence a
     * new operator reads anywhere in the app.
     */
    @Test
    fun `the auto-detect failure reports and points, in one short line`() {
        val line = com.lidarscan.core.capture.CaptureAutoConnectController.NOTHING_FOUND
        assertTrue("\"$line\" is ${WordingLaw.wordCount(line)} words", WordingLaw.isDetail(line))
        assertTrue("an error must say what to do", WordingLaw.isActionable(line))
        assertFalse("no product names on the first screen", line.contains("COIN-D6"))
        assertFalse(line.contains("Mid-360"))
        assertFalse(line.contains("Ethernet"))
    }

    /** The round-19 correction, restated over everything this round wrote. */
    @Test
    fun `no round-24 string blames the light`() {
        for (line in newInstructions + newDetails) {
            val lower = line.lowercase()
            assertFalse(line, lower.contains("more light"))
            assertFalse(line, lower.contains("lights"))
            assertFalse(line, lower.contains("brighter"))
            assertFalse(line, lower.contains("dark room"))
        }
    }
}
