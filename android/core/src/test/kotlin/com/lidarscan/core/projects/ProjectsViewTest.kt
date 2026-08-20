package com.lidarscan.core.projects

import com.lidarscan.core.WordingLaw
import com.lidarscan.core.model.ProjectManifest
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.store.Project
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * ROUND 24 item 108 — **the layout and the order.**
 *
 * The gallery/list toggle is a picture; the sort is arithmetic, and arithmetic
 * is what gets a scan lost. Three properties matter and all three are pinned
 * here: a sort never drops a scan, it is stable across ties (so two scans taken
 * in the same second do not swap places every time the list recomposes), and
 * names sort case-insensitively because a typed scan name is prose.
 */
class ProjectsViewTest {

    private fun project(name: String, createdAt: Long) = Project(
        id = "$name-$createdAt",
        directory = File("/tmp/$name.lscan"),
        manifest = ProjectManifest(
            name = name,
            sensor = SensorType.COIN_D6,
            profile = WorkflowProfile.QUICK_SCAN,
            createdAtEpochMillis = createdAt,
            appVersion = "0.9.9",
        ),
    )

    private val library = listOf(
        project("Attic", 300),
        project("basement", 100),
        project("Kitchen", 200),
    )

    @Test
    fun `newest first is the default order`() {
        assertEquals(ProjectSort.NEWEST, ProjectSort.DEFAULT)
        assertEquals(
            listOf("Attic", "Kitchen", "basement"),
            ProjectsView.sorted(library, ProjectSort.NEWEST).map { it.manifest.name },
        )
    }

    @Test
    fun `A to Z ignores case, because a scan name is prose`() {
        assertEquals(
            listOf("Attic", "basement", "Kitchen"),
            ProjectsView.sorted(library, ProjectSort.A_Z).map { it.manifest.name },
        )
    }

    @Test
    fun `Z to A is the exact reverse`() {
        assertEquals(
            ProjectsView.sorted(library, ProjectSort.A_Z).map { it.manifest.name }.reversed(),
            ProjectsView.sorted(library, ProjectSort.Z_A).map { it.manifest.name },
        )
    }

    @Test
    fun `no sort ever drops or duplicates a scan`() {
        for (sort in ProjectSort.entries) {
            val out = ProjectsView.sorted(library, sort)
            assertEquals("$sort changed the count", library.size, out.size)
            assertEquals("$sort lost a scan", library.map { it.id }.toSet(), out.map { it.id }.toSet())
        }
    }

    /**
     * Two scans created in the same millisecond must not swap places on every
     * recomposition — the list would visibly shuffle under the operator's
     * thumb. Kotlin's sorts are stable; this test is what stops someone
     * "improving" it into one that is not.
     */
    @Test
    fun `ties keep the order the store gave them`() {
        val tied = listOf(project("first", 500), project("second", 500))
        assertEquals(
            listOf("first", "second"),
            ProjectsView.sorted(tied, ProjectSort.NEWEST).map { it.manifest.name },
        )
    }

    @Test
    fun `an empty library sorts to an empty library`() {
        for (sort in ProjectSort.entries) {
            assertTrue(ProjectsView.sorted(emptyList(), sort).isEmpty())
        }
    }

    // ── the layout half ────────────────────────────────────────────────────

    @Test
    fun `list is the default and the toggle is its own inverse`() {
        assertEquals(ProjectsLayout.LIST, ProjectsLayout.DEFAULT)
        val start = ProjectsLayout.DEFAULT
        assertEquals(ProjectsLayout.GALLERY, ProjectsView.toggled(start))
        assertEquals(start, ProjectsView.toggled(ProjectsView.toggled(start)))
    }

    @Test
    fun `the gallery is two columns and the list is one`() {
        assertEquals(2, ProjectsView.columns(ProjectsLayout.GALLERY))
        assertEquals(1, ProjectsView.columns(ProjectsLayout.LIST))
    }

    /**
     * A persisted preference is read back from a string that may be from an
     * older build, a corrupted store or a hand-edited file. None of those may
     * be a crash on the app's start destination.
     */
    @Test
    fun `an unknown persisted value reads as the default`() {
        assertEquals(ProjectsLayout.DEFAULT, ProjectsLayout.parse(null))
        assertEquals(ProjectsLayout.DEFAULT, ProjectsLayout.parse("TILES"))
        assertEquals(ProjectsLayout.GALLERY, ProjectsLayout.parse("GALLERY"))
        assertEquals(ProjectSort.DEFAULT, ProjectSort.parse(null))
        assertEquals(ProjectSort.DEFAULT, ProjectSort.parse("BY_SIZE"))
        assertEquals(ProjectSort.Z_A, ProjectSort.parse("Z_A"))
    }

    @Test
    fun `the toggle names what a tap will do, not where you are`() {
        assertEquals("Show as gallery", ProjectsView.layoutActionLabel(ProjectsLayout.LIST))
        assertEquals("Show as list", ProjectsView.layoutActionLabel(ProjectsLayout.GALLERY))
    }

    @Test
    fun `every label obeys the wording law`() {
        for (line in ProjectsView.ALL) {
            assertTrue(
                "${WordingLaw.wordCount(line)} words: \"$line\"",
                WordingLaw.isInstruction(line),
            )
            assertTrue("jargon in \"$line\"", WordingLaw.jargonIn(line).isEmpty())
        }
    }

    /** The owner named these three words. They are the control's contract. */
    @Test
    fun `the sort options are Newest, A to Z and Z to A`() {
        assertEquals(listOf("Newest", "A–Z", "Z–A"), ProjectSort.entries.map { it.label })
    }

    // ── ROUND 25 item 114 ──────────────────────────────────────────────────

    /**
     * The gallery is thumbnail-first; the list is not. Asserted as an
     * exhaustive `for` over the enum rather than two literals, so adding a
     * third layout one day cannot leave this test quietly passing on two of
     * three cases.
     */
    @Test
    fun `only the gallery draws a preview image`() {
        for (layout in ProjectsLayout.entries) {
            assertEquals(
                "showsThumbnail(${'$'}layout)",
                layout == ProjectsLayout.GALLERY,
                ProjectsView.showsThumbnail(layout),
            )
        }
    }

    /**
     * The two layout facts have to agree about which layout is which. A
     * one-column gallery or a two-column thumbnail-less list would each be a
     * coherent function and an incoherent screen.
     */
    @Test
    fun `the layout with two columns is the layout with the thumbnail`() {
        for (layout in ProjectsLayout.entries) {
            assertEquals(
                "columns and showsThumbnail disagree about ${'$'}layout",
                ProjectsView.columns(layout) == 2,
                ProjectsView.showsThumbnail(layout),
            )
        }
    }

    /** The default layout is the list, so the default Projects tab shows no previews. */
    @Test
    fun `the default layout draws no preview`() {
        assertEquals(false, ProjectsView.showsThumbnail(ProjectsLayout.DEFAULT))
    }
}
