package com.lidarscan.core.projects

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 23 item 104c — the selection state machine, pinned.
 *
 * These are the rules a screen would otherwise re-decide every time somebody
 * touched the list: what a tap means in each mode, what emptying the selection
 * does, and what happens to ids after a group delete.
 */
class ProjectSelectionTest {

    @Test
    fun `a fresh selection is inactive and empty`() {
        val s = ProjectSelection.EMPTY
        assertFalse(s.isActive)
        assertEquals(0, s.count)
        assertTrue(s.isEmpty)
    }

    @Test
    fun `long-press enters the mode AND picks the pressed card`() {
        val s = ProjectSelection.EMPTY.enter("scan-1")
        assertTrue(s.isActive)
        assertEquals(setOf("scan-1"), s.selectedIds)
        assertEquals(1, s.count)
        assertTrue(s.contains("scan-1"))
    }

    @Test
    fun `a tap OUTSIDE selection mode changes nothing — only long-press starts it`() {
        // The card's tap still opens the scan (ROUND 22 item 96); if toggle
        // could start a selection, every mis-tap would drop the operator into
        // a mode they did not ask for.
        val s = ProjectSelection.EMPTY.toggle("scan-1")
        assertFalse(s.isActive)
        assertTrue(s.isEmpty)
    }

    @Test
    fun `taps toggle while selecting`() {
        var s = ProjectSelection.EMPTY.enter("scan-1")
        s = s.toggle("scan-2")
        s = s.toggle("scan-3")
        assertEquals(3, s.count)
        s = s.toggle("scan-2")
        assertEquals(setOf("scan-1", "scan-3"), s.selectedIds)
        assertTrue(s.isActive)
    }

    @Test
    fun `un-picking the last card LEAVES the mode`() {
        var s = ProjectSelection.EMPTY.enter("scan-1")
        s = s.toggle("scan-1")
        assertFalse("a bar reading \"0 selected\" is a mode with nothing in it", s.isActive)
        assertTrue(s.isEmpty)
    }

    @Test
    fun `the X exits whatever is selected`() {
        val s = ProjectSelection.EMPTY.enter("a").toggle("b").toggle("c").exit()
        assertFalse(s.isActive)
        assertEquals(0, s.count)
    }

    @Test
    fun `entering twice on the same card does not double-count`() {
        val s = ProjectSelection.EMPTY.enter("scan-1").enter("scan-1")
        assertEquals(1, s.count)
    }

    @Test
    fun `the batch runs in the LIST's order, not a hash set's`() {
        val listOrder = listOf("scan-9", "scan-4", "scan-7", "scan-1")
        val s = ProjectSelection.EMPTY.enter("scan-1").toggle("scan-9").toggle("scan-7")
        assertEquals(listOf("scan-9", "scan-7", "scan-1"), s.ordered(listOrder))
    }

    @Test
    fun `ordered ignores ids the list no longer holds`() {
        val s = ProjectSelection.EMPTY.enter("gone").toggle("here")
        assertEquals(listOf("here"), s.ordered(listOf("here")))
    }

    @Test
    fun `retain drops ids a group delete removed, and exits when nothing is left`() {
        val s = ProjectSelection.EMPTY.enter("a").toggle("b")
        assertEquals(setOf("b"), s.retain(listOf("b", "c")).selectedIds)
        assertTrue(s.retain(listOf("b", "c")).isActive)
        assertFalse(s.retain(listOf("c")).isActive)
    }

    @Test
    fun `the bar's title is two words and counts what is picked`() {
        val s = ProjectSelection.EMPTY.enter("a").toggle("b").toggle("c")
        assertEquals("3 selected", s.title())
        assertTrue(WordingLaw.isInstruction(s.title()))
    }

    @Test
    fun `selection is a value — an earlier state is not mutated by a later one`() {
        val one = ProjectSelection.EMPTY.enter("a")
        val two = one.toggle("b")
        assertEquals(1, one.count)
        assertEquals(2, two.count)
    }
}
