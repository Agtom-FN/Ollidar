package com.lidarscan.core.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 27 item 141 — **"height rgb showing not working too."**
 *
 * Round 26 changed two defaults and wrote in its own resolution that *"a
 * project with a saved colormap deserialises it and never reaches any of these
 * lines."* That sentence is the bug: every project on the owner's phone has a
 * saved colormap, all of them say GRAYSCALE, and so the Turbo default reached
 * only projects that did not exist yet.
 *
 * The properties below are the ones that make a migration different from a
 * default — it runs on saved state, it runs ONCE, and it does not overrule a
 * choice made afterwards.
 */
class DisplayMigrationsTest {

    private val savedBefore = DisplayParams(
        height = ScalarColorParams(manualMin = 0f, manualMax = 3f, colormap = Colormap.GRAYSCALE),
        intensity = ScalarColorParams(colormap = Colormap.GRAYSCALE),
    )

    @Test
    fun `a project saved before 0_9_12 gets Turbo on height`() {
        val r = DisplayMigrations.migrate(savedBefore, applied = null)
        assertTrue("the migration must report that it did something", r.changed)
        assertEquals(Colormap.TURBO, r.params.height.colormap)
    }

    @Test
    fun `intensity grayscale is left alone - it is item 39's answer, not a leftover`() {
        val r = DisplayMigrations.migrate(savedBefore, applied = null)
        assertEquals(
            "intensity keeps CAPTURE_COLORMAP",
            Colormap.GRAYSCALE,
            r.params.intensity.colormap,
        )
    }

    @Test
    fun `a deliberate height colormap is never touched`() {
        for (chosen in listOf(Colormap.SPECTRUM, Colormap.THERMAL, Colormap.TURBO)) {
            val saved = savedBefore.copy(height = savedBefore.height.copy(colormap = chosen))
            val r = DisplayMigrations.migrate(saved, applied = null)
            assertFalse(r.changed)
            assertEquals(chosen, r.params.height.colormap)
        }
    }

    /**
     * The property that makes this a migration rather than a rule. Grayscale on
     * height is a legitimate thing to want — it is the flattest reading of a
     * ceiling — and an unversioned "if grayscale then Turbo" would take it back
     * every time the project was reopened.
     */
    @Test
    fun `it runs once - a grayscale chosen AFTER the migration survives`() {
        val migrated = DisplayMigrations.migrate(savedBefore, applied = null).params
        assertEquals(DisplayMigrations.CURRENT, migrated.migration)

        // The operator now deliberately picks grayscale on the migrated project.
        val reChosen = migrated.copy(height = migrated.height.copy(colormap = Colormap.GRAYSCALE))
        val again = DisplayMigrations.migrate(reChosen, applied = reChosen.migration)
        assertFalse("a migrated project is never re-examined", again.changed)
        assertEquals(Colormap.GRAYSCALE, again.params.height.colormap)
    }

    @Test
    fun `a project that needed nothing is still stamped, so it is never re-examined`() {
        val fine = savedBefore.copy(height = savedBefore.height.copy(colormap = Colormap.SPECTRUM))
        val r = DisplayMigrations.migrate(fine, applied = null)
        assertEquals(DisplayMigrations.CURRENT, r.params.migration)
    }

    @Test
    fun `stamp is idempotent`() {
        val once = DisplayMigrations.stamp(savedBefore)
        assertEquals(DisplayMigrations.CURRENT, once.migration)
        assertTrue(DisplayMigrations.stamp(once) === once)
    }

    /**
     * The two defaults round 26 set are still what a NEW project gets — the
     * migration exists because they were not enough, not because they were
     * wrong.
     */
    @Test
    fun `the new-project defaults are unchanged`() {
        assertEquals(Colormap.TURBO, DisplayParams().height.colormap)
        assertEquals(Colormap.TURBO, DisplayParams.CAPTURE_HEIGHT_COLORMAP)
        assertEquals(Colormap.GRAYSCALE, DisplayParams.CAPTURE_COLORMAP)
    }

    /** The log line is a fixed string so a field report can be grepped for it. */
    @Test
    fun `the log line names the migration`() {
        assertTrue(DisplayMigrations.LOG_LINE.contains("grayscale"))
        assertTrue(DisplayMigrations.LOG_LINE.contains("turbo"))
    }
}
