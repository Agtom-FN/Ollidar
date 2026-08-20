package com.lidarscan.core.projects

import com.lidarscan.core.Wording
import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 23 item 104 — the round-22 wording law, applied to the strings this
 * round adds.
 *
 * `Round22WordingTest` guards `Wording`; these strings live next to the state
 * machine they belong to, so they get the same guard here rather than escaping
 * it by being in a different file.
 */
class ProjectActionWordingTest {

    @Test
    fun `every new string is an instruction-length name or shorter`() {
        for (line in ProjectActionWording.ALL) {
            assertTrue(
                "\"$line\" is ${WordingLaw.wordCount(line)} words",
                WordingLaw.isInstruction(line),
            )
        }
    }

    @Test
    fun `no new string carries design-document jargon`() {
        for (line in ProjectActionWording.ALL) {
            assertTrue("jargon in \"$line\"", WordingLaw.jargonIn(line).isEmpty())
        }
    }

    @Test
    fun `the two actions the owner said vanished are named exactly`() {
        // Item 104: "Export" and "Share", spelled the way the brief spells them.
        assertEquals("Export", Wording.EXPORT_ACTION)
        assertEquals("Export", Wording.CARD_MENU_EXPORT)
        assertEquals("Share", ProjectActionWording.SHARE_ACTION)
        assertEquals("Delete", ProjectActionWording.DELETE_ACTION)
        assertEquals("Delete", Wording.CARD_MENU_DELETE)
    }

    @Test
    fun `the share sheet's title counts its files and gets the plural right`() {
        assertEquals("Send 1 file", ProjectActionWording.sendFilesTitle(1))
        assertEquals("Send 3 files", ProjectActionWording.sendFilesTitle(3))
    }

    @Test
    fun `the export chip does not blame light and does not explain itself`() {
        val chip = ProjectActionWording.exportingProgress(40)
        assertEquals("Exporting… 40%", chip)
        assertTrue(WordingLaw.isInstruction(chip))
    }

    @Test
    fun `no round-23 string blames light — the round-19 correction still stands`() {
        for (line in ProjectActionWording.ALL) {
            val lower = line.lowercase()
            assertTrue(line, !lower.contains("more light"))
            assertTrue(line, !lower.contains("lights"))
            assertTrue(line, !lower.contains("brighter"))
        }
    }
}
