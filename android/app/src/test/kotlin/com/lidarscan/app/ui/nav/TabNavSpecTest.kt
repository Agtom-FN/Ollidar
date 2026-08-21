package com.lidarscan.app.ui.nav

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 22 item 88 — the navigation defect that rebuilt the capture ViewModel
 * on every tab switch.
 *
 * `NavOptionsBuilder` is an Android type and cannot be built on a bare JVM, so
 * `goTab` was split: [tabNavSpec] decides, and `LidarScanApp.goTab` copies the
 * decision into the builder verbatim. This suite pins the decision, which is
 * where the bug was — the pre-round-22 call set `popUpTo` and `launchSingleTop`
 * and neither `saveState` nor `restoreState`, and that omission destroys the
 * popped entry's `ViewModelStore`.
 *
 * The end-to-end property — a tab round trip constructs **no** second
 * `CaptureViewModel` — is pinned on the emulator by
 * `TabSwitchKeepsCaptureViewModelTest`, which counts
 * `CaptureViewModel.constructions` across a real `NavController`. Both halves
 * are needed: this one runs on every build, that one proves the property is
 * actually what Navigation Compose does with these flags.
 */
class TabNavSpecTest {

    // ── ROUND 27 item 131 ──────────────────────────────────────────────────

    /**
     * **Item 131.** Profile lights NO tab.
     *
     * The owner found the Profile page highlighting Projects, and the cause was
     * `tabForRoute`'s `else` branch — a default that Review, Plan and Merge
     * legitimately use, inherited by a destination that is a peer of Projects
     * rather than a child of it. The navigation-compose contract for a bottom
     * bar is `currentDestination.hierarchy.any { it.route == tab.route }`:
     * selection asserts that the current destination is INSIDE that tab's
     * graph, and Profile is not inside any.
     *
     * The rejected alternative is pinned by the same test: "highlight the tab
     * it was opened from" would make the same page light different tabs
     * depending on the back stack, and a tab bar whose state depends on how you
     * arrived is worse than one capsule left unlit.
     */
    @Test
    fun `Profile is a sub-screen of no tab and lights none`() {
        assertEquals(null, tabForRoute(Routes.PROFILE))
    }

    /**
     * **Item 131.** And nothing else lost its tab in the process — the four
     * tabs, and the project-scoped screens that legitimately light Projects.
     */
    @Test
    fun `every other destination still lights the tab it belongs to`() {
        assertEquals(ScanTab.PROJECTS, tabForRoute(Routes.PROJECTS))
        assertEquals(ScanTab.CAPTURE, tabForRoute(Routes.CAPTURE_NEW))
        assertEquals(ScanTab.JOBS, tabForRoute(Routes.JOBS_PICK))
        assertEquals(ScanTab.SETTINGS, tabForRoute(Routes.SETTINGS))
        assertEquals(ScanTab.CAPTURE, tabForRoute(Routes.MID360_SETUP))
        assertEquals(ScanTab.CAPTURE, tabForRoute("project/abc/capture"))
        assertEquals(ScanTab.JOBS, tabForRoute("project/abc/processing"))
        // Review and the other project-scoped screens keep the Projects
        // default, which is correct for them: they ARE inside Projects.
        assertEquals(ScanTab.PROJECTS, tabForRoute("project/abc/review"))
        assertEquals(ScanTab.PROJECTS, tabForRoute(null))
    }

    private val start = Routes.PROJECTS

    @Test
    fun `every tab saves the outgoing stack - this is the whole fix`() {
        for (target in ALL_TABS) {
            val spec = tabNavSpec(start, target)
            assertTrue(
                "$target must save the popped stack, or its ViewModelStore dies with it",
                spec.saveState,
            )
            assertTrue("$target must not stack duplicates", spec.launchSingleTop)
        }
    }

    @Test
    fun `the pop target is the graph start, never the tab being opened`() {
        for (target in ALL_TABS) {
            assertEquals(start, tabNavSpec(start, target).popUpToRoute)
        }
    }

    @Test
    fun `the Projects tab no longer pops itself inclusively`() {
        // `inclusive = route == Routes.PROJECTS` was the pre-round-22 special
        // case. Popping the start destination inclusively and re-adding it made
        // Projects a fresh entry on every tap too — with `launchSingleTop`, the
        // non-inclusive pop lands on the entry that is already there, which is
        // what a tab bar means by "go to Projects".
        assertFalse(tabNavSpec(start, Routes.PROJECTS).popUpToInclusive)
    }

    @Test
    fun `no tab pops inclusively`() {
        for (target in ALL_TABS) {
            assertFalse("$target", tabNavSpec(start, target).popUpToInclusive)
        }
    }

    @Test
    fun `every NON-start tab restores the stack it saved`() {
        // This is the half that keeps the Scan tab's ViewModelStore alive: it
        // is saved on the way out by `saveState`, and this is what brings it
        // back instead of building a fresh CaptureViewModel.
        for (target in ALL_TABS.filter { it != start }) {
            assertTrue("$target must restore its saved stack", tabNavSpec(start, target).restoreState)
        }
    }

    /**
     * ROUND 22 item 88, second cut — **the start destination does not restore.**
     *
     * `popUpTo(start) { saveState = true }` in the same call has just saved
     * whatever was above the start destination; asking to restore while
     * navigating TO it puts that straight back, and the screen never changes.
     * The emulator caught exactly that: from the replay-capture destination,
     * tapping Projects left `recordButton` on screen and both
     * `CaptureFlowNoStrayTest` cases timed out waiting for the Projects hero.
     *
     * It is degenerate only because this graph's tabs are flat destinations
     * rather than nested graphs — the start destination is both the pop target
     * and a tab.
     */
    @Test
    fun `the start destination pops and lands, and does NOT restore`() {
        assertFalse(tabNavSpec(start, Routes.PROJECTS).restoreState)
        assertTrue("it must still SAVE what it popped", tabNavSpec(start, Routes.PROJECTS).saveState)
        assertTrue(tabNavSpec(start, Routes.PROJECTS).launchSingleTop)
    }

    @Test
    fun `the Scan tab gets the same treatment as every other non-start tab`() {
        // The defect was specific to the Scan tab only because Scan is the tab
        // with a ViewModel worth keeping. The RULE must not be: one behaviour,
        // applied uniformly, is what makes it hard to reintroduce.
        assertEquals(tabNavSpec(start, Routes.CAPTURE_NEW), tabNavSpec(start, Routes.SETTINGS))
    }

    @Test(expected = IllegalArgumentException::class)
    fun `a blank route is refused rather than navigated to`() {
        tabNavSpec(start, "")
    }

    private companion object {
        val ALL_TABS = listOf(
            Routes.PROJECTS,
            Routes.CAPTURE_NEW,
            Routes.JOBS_PICK,
            Routes.SETTINGS,
        )
    }
}
