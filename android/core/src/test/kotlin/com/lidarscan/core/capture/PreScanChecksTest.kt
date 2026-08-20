package com.lidarscan.core.capture

import com.lidarscan.core.WordingLaw
import com.lidarscan.core.calib.MountTrim
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 23 item 106(b) — the pre-scan checklist, folded into the start panel.
 *
 * The property that matters is what is NOT said: ROUND 19's modal showed four
 * rows on the first press of every device, most of them reporting that
 * everything was fine, in front of a screen whose design rule is that it has
 * no steps. These checks now surface only when one of them has something to
 * report, inside a panel that is on screen anyway for the four to eight
 * seconds the start takes.
 */
class PreScanChecksTest {

    @Test
    fun `a good rig says nothing at all`() {
        assertEquals(
            emptyList<String>(),
            PreScanChecks.notesFor(trimAccuracyDeg = 0.18, dndProtected = true),
        )
    }

    @Test
    fun `no trim yet is not a warning - the start hold measures one`() {
        // ROUND 20 made the re-zero automatic at Start, so "no trim" is a
        // normal first-scan state and not something to interrupt anyone about.
        assertEquals(
            emptyList<String>(),
            PreScanChecks.notesFor(trimAccuracyDeg = null, dndProtected = true),
        )
    }

    @Test
    fun `a rough mount is reported, using the same threshold the trim itself uses`() {
        val justOver = MountTrim.WARN_STABILITY_DEG + 0.01
        val justUnder = MountTrim.WARN_STABILITY_DEG - 0.01
        assertTrue(PreScanChecks.notesFor(justOver, dndProtected = true).contains(PreScanChecks.MOUNT_ROUGH))
        assertTrue(PreScanChecks.notesFor(justUnder, dndProtected = true).isEmpty())
    }

    @Test
    fun `the owner's 0-9-7 numbers - 0-18 and 0-10 degrees - raise nothing`() {
        // scan-070 and scan-071's measured trims, from the field log.
        assertTrue(PreScanChecks.notesFor(0.18, dndProtected = true).isEmpty())
        assertTrue(PreScanChecks.notesFor(0.10, dndProtected = true).isEmpty())
        // …and the 3.18° hold round 22 refused would have said so.
        assertTrue(PreScanChecks.notesFor(3.18, dndProtected = true).isNotEmpty())
    }

    @Test
    fun `an unprotected walk is one line`() {
        val notes = PreScanChecks.notesFor(trimAccuracyDeg = 0.2, dndProtected = false)
        assertEquals(listOf(PreScanChecks.NOTIFICATIONS_UNPROTECTED), notes)
    }

    @Test
    fun `both at once keeps the mount first - it is the one that costs geometry`() {
        val notes = PreScanChecks.notesFor(trimAccuracyDeg = 2.5, dndProtected = false)
        assertEquals(listOf(PreScanChecks.MOUNT_ROUGH, PreScanChecks.NOTIFICATIONS_UNPROTECTED), notes)
    }

    @Test
    fun `every note obeys the wording law`() {
        for (note in listOf(PreScanChecks.MOUNT_ROUGH, PreScanChecks.NOTIFICATIONS_UNPROTECTED)) {
            assertTrue("$note is ${WordingLaw.wordCount(note)} words", WordingLaw.isInstruction(note))
            assertTrue("jargon in $note", WordingLaw.jargonIn(note).isEmpty())
        }
    }
}
