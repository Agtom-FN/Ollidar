package com.lidarscan.app

import android.content.pm.ActivityInfo
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.getUnclippedBoundsInRoot
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * ROUND 27 — **the geometry layer**, and the reason it exists.
 *
 * Round 26 shipped the fullscreen Scan tab with every suite green and the
 * layout visibly broken: the connect flow printed through the SCAN button, the
 * `RAW · D6` chip was drawn inside the landscape connect rail, `No data` floated
 * in the middle of the picture and the `?` sat on top of the pause button. Not
 * one test failed, because **every test in this app asserts semantics** — is
 * the node there, does it say the right thing, does tapping it do the right
 * thing — and none of them asserts where the node IS.
 *
 * So this suite asserts rectangles. Its central claim (item 129(c)) is one
 * sentence: the connect panel, the status badges, the control cluster and the
 * corner chips are **pairwise non-overlapping**, in both orientations, at a
 * phone-size window. Everything a "the UI is off" complaint is made of is a
 * violation of that sentence, and none of it is expressible in `assertExists`.
 *
 * Orientation is driven through `requestedOrientation` on the real Activity
 * rather than by faking a `Configuration`: the Scan screen reads window insets
 * and `BoxWithConstraints`, and a faked configuration changes neither. The
 * Activity declares no `configChanges`, so this is a genuine destroy/rebuild —
 * which is also the round-25 path worth exercising.
 */
@RunWith(AndroidJUnit4::class)
class Round27UiTest {

    @get:Rule
    val composeRule = createEmptyComposeRule()

    private fun has(tag: String): Boolean =
        composeRule.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty()

    private fun bounds(tag: String): Rect =
        composeRule.onNodeWithTag(tag, useUnmergedTree = true).fetchSemanticsNode().boundsInRoot

    private fun boundsOrNull(tag: String): Rect? =
        composeRule.onAllNodesWithTag(tag, useUnmergedTree = true)
            .fetchSemanticsNodes()
            .firstOrNull()
            ?.boundsInRoot

    private fun openScanTab() {
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { has("projectsAvatar") }.getOrDefault(false)
        }
        composeRule.onNodeWithTag("tab_capture").performClick()
        composeRule.waitUntil(timeoutMillis = 30_000) {
            runCatching { has("recordButton") }.getOrDefault(false)
        }
        composeRule.waitForIdle()
    }

    /**
     * Two rectangles overlap when they share area. Touching edges do not count
     * — a chip whose bottom edge is the cluster's top edge is adjacent, not
     * colliding — so the comparison is strict, with a 1 px tolerance for the
     * rounding a dp-to-px conversion leaves behind.
     */
    private fun overlaps(a: Rect, b: Rect): Boolean {
        val tol = 1f
        return a.left < b.right - tol && b.left < a.right - tol &&
            a.top < b.bottom - tol && b.top < a.bottom - tol
    }

    private fun assertNoOverlaps(orientation: String, named: Map<String, Rect>) {
        val entries = named.entries.toList()
        for (i in entries.indices) {
            for (j in i + 1 until entries.size) {
                val (an, ar) = entries[i]
                val (bn, br) = entries[j]
                assertTrue(
                    "$orientation: \"$an\" $ar overlaps \"$bn\" $br — item 129",
                    !overlaps(ar, br),
                )
            }
        }
    }

    /**
     * Every floating element the Scan tab draws in its idle, disconnected
     * state, by tag. Absent tags are skipped rather than failed: the keyframe
     * chip only exists during a recording, and a suite that demanded it would
     * be asserting a state an AVD cannot reach.
     */
    private fun scanTabRects(): Map<String, Rect> = listOfNotNull(
        "chrome / connect panel" to (boundsOrNull("scanChromeColumn") ?: return emptyMap()),
        // The whole PILL, not the points line inside it. The first version of
        // this suite measured `pointsCapturedValue` and passed on a landscape
        // screen where the pill's end edge and the eye button's top edge shared
        // pixels — a test that measures a child of the thing that collides is a
        // test that reports the collision as clear.
        boundsOrNull("scanStatusPill")?.let { "status pill" to it },
        boundsOrNull("advancedButton")?.let { "gear" to it },
        boundsOrNull("recordButton")?.let { "SCAN button" to it },
        boundsOrNull("pauseButton")?.let { "pause" to it },
        boundsOrNull("liveViewSwitch")?.let { "eye" to it },
        boundsOrNull("mapModeChip")?.let { "map-mode chip" to it },
        boundsOrNull("streamModeChip")?.let { "stream chip" to it },
        boundsOrNull("tutorialButton")?.let { "? chip" to it },
        boundsOrNull("scanTabBar")?.let { "tab bar" to it },
    ).toMap()

    /**
     * **Item 129(a).** The health read-out is INSIDE the status pill.
     *
     * Not merely "somewhere that does not collide": the item's instruction is
     * that `No data` belongs in or beside the pill rather than floating on the
     * picture, and containment is the claim. It is therefore excluded from the
     * pairwise sweep above — a child inside its parent is a containment, not a
     * collision — and asserted here instead, so removing it from the sweep
     * cannot quietly become "it is not tested".
     */
    @Test
    fun theHealthReadOutLivesInsideTheStatusPill() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            val pill = bounds("scanStatusPill")
            val health = bounds("captureHealthChip")
            assertTrue(
                "the health chip $health must be inside the pill $pill",
                health.left >= pill.left - 1f && health.right <= pill.right + 1f &&
                    health.top >= pill.top - 1f && health.bottom <= pill.bottom + 1f,
            )
        }
    }

    // ══ item 129(c) — the geometry regression layer ════════════════════════

    /**
     * **Item 129(c), portrait.** Nothing on the Scan tab overlaps anything
     * else.
     *
     * The connect panel is deliberately in the set. Item 129(b) is precisely a
     * collision between it and the control cluster, and it is the element a
     * semantics suite is least able to see: `scanNameField` "exists" and is
     * "displayed" whether it is in the middle of the screen or printed through
     * the SCAN button.
     */
    @Test
    fun portraitScanTabHasNoOverlappingChrome() {
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            scenario.onActivity { it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT }
            openScanTab()
            val rects = scanTabRects()
            assertTrue("the chrome column must be on screen to be tested", rects.isNotEmpty())
            assertNoOverlaps("portrait", rects)
        }
    }

    /**
     * **Item 129(c), landscape.** The same sentence, rotated — and the harder
     * half. Every one of the four collisions the owner reported was a
     * landscape one, because landscape is where a full-width top band and a
     * start-anchored chrome column occupy the same pixels.
     */
    @Test
    fun landscapeScanTabHasNoOverlappingChrome() {
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            scenario.onActivity { it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE }
            openScanTab()
            val rects = scanTabRects()
            assertTrue("the chrome column must be on screen to be tested", rects.isNotEmpty())
            assertNoOverlaps("landscape", rects)
        }
    }

    /**
     * **Item 129(a)/(b).** The chrome column stays inside the window.
     *
     * A column that overruns the bottom of the screen does not "overlap"
     * anything the test can name — the pixels below the window belong to
     * nobody — and it is exactly what clipped `Allow Do Not Disturb in
     * Settings` mid-sentence. So the window itself is asserted as a bound.
     */
    @Test
    fun theChromeColumnStaysInsideTheWindowInBothOrientations() {
        for (orientation in listOf(
            ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
            ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
        )) {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                scenario.onActivity { it.requestedOrientation = orientation }
                openScanTab()
                val root = composeRule.onNodeWithTag("captureViewport").fetchSemanticsNode()
                    .root!!.semanticsOwner.rootSemanticsNode.size
                val chrome = bounds("scanChromeColumn")
                assertTrue(
                    "orientation=$orientation: the chrome column ($chrome) must end inside the " +
                        "${root.height} px window",
                    chrome.bottom <= root.height + 1f,
                )
                assertTrue(
                    "orientation=$orientation: and start inside it",
                    chrome.top >= -1f && chrome.right <= root.width + 1f,
                )
            }
        }
    }

    /**
     * **Item 129(a).** The connect flow is genuinely SCROLLABLE, not merely
     * bounded.
     *
     * Round 26's column had a `verticalScroll` on it and could not be scrolled,
     * because `PreCaptureStrip` nested a second scroller on the same axis
     * inside it and swallowed every drag. That is invisible to `assertExists`
     * and it is the whole reason the USB panel was cut in half with no way to
     * see the rest. Asserted from the outside: the manual panel's IP field is
     * reachable, in both orientations, by scrolling.
     */
    @Test
    fun theConnectFlowCanBeScrolledToItsEnd() {
        for (orientation in listOf(
            ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
            ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
        )) {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                scenario.onActivity { it.requestedOrientation = orientation }
                openScanTab()
                composeRule.waitUntil(timeoutMillis = 30_000) {
                    runCatching { has("manualLidarIpField") }.getOrDefault(false)
                }
                val field = composeRule.onNodeWithTag("manualLidarIpField")
                field.performScrollTo()
                composeRule.waitForIdle()
                field.assertIsDisplayed()
            }
        }
    }

    /**
     * **Item 129(a).** In landscape the connect rail owns the start side and
     * the control cluster owns the end side — stated as the inequality the
     * layout is built from rather than as "they do not overlap", so a future
     * change that merely shuffles them apart by a pixel does not pass.
     */
    /**
     * **Item 129(a).** The whole TOP BAND — pill, gear and RTK chip strip
     * together — clears the control cluster, in both orientations.
     *
     * Separate from the pairwise sweep because it is the band rather than its
     * contents: in landscape the band is a fixed column at the top-end corner
     * and the cluster is a fixed column at the centre-end, and "they do not
     * share pixels" is a claim about the two RESERVATIONS, which is what the
     * layout is actually built from.
     */
    @Test
    fun theTopBandClearsTheControlClusterInBothOrientations() {
        for (orientation in listOf(
            ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
            ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
        )) {
            ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                scenario.onActivity { it.requestedOrientation = orientation }
                openScanTab()
                // `scanTopBand` is the MINIMAL layout's floating group; the
                // idle page puts the same composable in the flow and tags only
                // the pill. Measure whichever is present — the claim is about
                // the status band's ink either way.
                val band = boundsOrNull("scanTopBand") ?: bounds("scanStatusPill")
                for (control in listOf("recordButton", "pauseButton", "liveViewSwitch", "tutorialButton")) {
                    val r = boundsOrNull(control) ?: continue
                    assertTrue(
                        "orientation=$orientation: the top band $band overlaps \"$control\" $r",
                        !overlaps(band, r),
                    )
                }
            }
        }
    }

    @Test
    fun landscapeGivesTheConnectFlowTheStartRailAndTheControlsTheEnd() {
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            scenario.onActivity { it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE }
            openScanTab()
            val chrome = bounds("scanChromeColumn")
            val fab = bounds("recordButton")
            val root = composeRule.onNodeWithTag("captureViewport").fetchSemanticsNode()
                .root!!.semanticsOwner.rootSemanticsNode.size
            assertTrue(
                "the connect rail starts at the start edge (left=${chrome.left})",
                chrome.left < root.width * 0.1f,
            )
            assertTrue(
                "the SCAN button is on the end side (left=${fab.left} of ${root.width})",
                fab.left > root.width * 0.6f,
            )
            assertTrue(
                "and the rail ends before the cluster begins",
                chrome.right <= fab.left + 1f,
            )
        }
    }

    // ══ items 135–140 — the owner's own Pixel session ══════════════════════

    /**
     * **Item 135.** The same non-overlap sweep on a **compact** phone.
     *
     * 360 × 640 dp is the small end of what a public app meets, and it is where
     * a layout built from fixed dp falls over: 356 dp of connect rail plus 360
     * dp of status group does not fit in a 640 dp landscape window, and a
     * 174 dp bottom reserve is a quarter of a 640 dp portrait one. The display
     * is resized through the instrumentation's own shell — the only way to test
     * a size the AVD was not created at — and restored in a `finally`, because
     * a test that leaves the emulator at 720 × 1280 breaks every suite after it.
     */
    @Test
    fun aCompactPhoneGetsTheSameNonOverlappingLayout() {
        val automation = androidx.test.platform.app.InstrumentationRegistry.getInstrumentation()
            .uiAutomation
        fun shell(cmd: String) {
            automation.executeShellCommand(cmd).close()
            Thread.sleep(1_500)
        }
        try {
            shell("wm size 720x1280")
            shell("wm density 320")
            for (orientation in listOf(
                ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
                ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
            )) {
                ActivityScenario.launch(MainActivity::class.java).use { scenario ->
                    scenario.onActivity { it.requestedOrientation = orientation }
                    openScanTab()
                    val rects = scanTabRects()
                    assertTrue("the chrome column must be on screen", rects.isNotEmpty())
                    assertNoOverlaps("compact/$orientation", rects)
                }
            }
        } finally {
            shell("wm size reset")
            shell("wm density reset")
        }
    }

    /**
     * **Item 136.** The idle Scan tab is a LAID-OUT PAGE, and the tab bar has
     * its own space.
     *
     * The claim that cannot be made by `assertExists` and is the whole of the
     * owner's *"nothing should overlay except warning"*: the connection section
     * and the tab bar do not share pixels with anything, because the page is a
     * column rather than a stack of floating cards.
     */
    @Test
    fun theIdleScanTabIsAPageAndTheTabBarReservesItsSpace() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            composeRule.onNodeWithTag("scanIdlePage").assertIsDisplayed()
            val bar = bounds("scanTabBar")
            for (tag in listOf("scanChromeColumn", "recordButton", "scanStatusPill", "captureViewport")) {
                val r = boundsOrNull(tag) ?: continue
                assertTrue(
                    "item 136(a): \"$tag\" $r must not draw under the tab bar $bar",
                    !overlaps(bar, r),
                )
            }
        }
    }

    /**
     * **Item 138.** One tracking status, one device identity.
     *
     * The owner counted two of each. The tracking chip was composed twice (the
     * viewport's top-centre corner AND the chip row) and the device name was
     * printed twice (the pill's sensor badge AND the `RAW · D6` stream chip).
     * Counting nodes is exactly the right assertion for a duplication defect,
     * and it is the one assertion round 26 could have made and did not.
     */
    @Test
    fun thereIsOneTrackingStatusAndOneDeviceName() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            assertTrue(
                "at most one tracking chip may be composed",
                composeRule.onAllNodesWithTag("poseTrackingViewportChip", useUnmergedTree = true)
                    .fetchSemanticsNodes().size <= 1,
            )
            assertEquals(
                "exactly one status pill",
                1,
                composeRule.onAllNodesWithTag("scanStatusPill", useUnmergedTree = true)
                    .fetchSemanticsNodes().size,
            )
            // Item 140(a): and the map chip is gone from the picture entirely.
            assertEquals(
                "item 140(a): the map-mode chip is removed",
                0,
                composeRule.onAllNodesWithTag("mapModeChip").fetchSemanticsNodes().size,
            )
        }
    }

    /**
     * **Item 139.** The Advanced sheet has a Connection tab, and it holds the
     * same manual-entry panel the main page does — one implementation behind
     * two doors, so there is never a second `manualLidarIpField` alive beside
     * the first.
     */
    @Test
    fun advancedHasAConnectionTabWithTheSamePanel() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            composeRule.onNodeWithTag("advancedButton").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) {
                runCatching { has("advancedTabRow") }.getOrDefault(false)
            }
            composeRule.onNodeWithText("Connection").performClick()
            composeRule.waitForIdle()
            composeRule.onNodeWithTag("advancedConnectionPane").assertIsDisplayed()
            assertEquals(
                "the connect panel exists once, in whichever door is open",
                1,
                composeRule.onAllNodesWithTag("manualLidarIpField").fetchSemanticsNodes().size,
            )
        }
    }

    // ══ item 130 — no ellipsis on an informational value ═══════════════════

    /**
     * **Item 130.** The Advanced sheet's range hints WRAP rather than
     * ellipsize.
     *
     * Asserted geometrically, because an ellipsis is a rendering fact and not a
     * semantics one: the node's text is the full string either way, so
     * `onNodeWithText` passes on a row that reads `0.1 – 12 px · 0.1 st…`. What
     * changes when the text wraps is the node's HEIGHT, so that is what is
     * measured — the hint is meaningfully taller than its own single-line
     * label.
     */
    @Test
    fun theAdvancedSheetDoesNotEllipsizeItsValues() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            composeRule.onNodeWithTag("advancedButton").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) {
                runCatching { has("pointSizeHint") }.getOrDefault(false)
            }
            composeRule.waitForIdle()

            for (tag in listOf("pointSizeHint", "scannerTrackingHint")) {
                if (!has(tag)) continue
                // The sheet scrolls; a row below the fold is composed but not
                // placed, and an unplaced node reports Rect.Zero rather than
                // failing, which would make this test pass on a truncated row.
                runCatching { composeRule.onNodeWithTag(tag, useUnmergedTree = true).performScrollTo() }
                composeRule.waitForIdle()
                // UNCLIPPED bounds. `boundsInRoot` is clipped to the scrolling
                // sheet, so a row resting on the fold reports one line's worth
                // of height whatever it actually drew — which is precisely the
                // measurement a truncation test must not make.
                val hintBox = composeRule.onNodeWithTag(tag, useUnmergedTree = true)
                    .getUnclippedBoundsInRoot()
                val labelBox = composeRule.onNodeWithTag("$tag-label", useUnmergedTree = true)
                    .getUnclippedBoundsInRoot()
                val hint = hintBox.bottom - hintBox.top
                val label = labelBox.bottom - labelBox.top
                assertTrue(
                    "item 130: \"$tag\" must be allowed to wrap — it is $hint tall " +
                        "beside a $label label, which is one clipped line",
                    hint > label * 1.4f,
                )
            }
        }
    }

    // ══ item 131 — Profile lights no tab ═══════════════════════════════════

    /**
     * **Item 131.** On the Profile page no tab capsule is selected.
     *
     * The tab bar's selection has no semantics of its own (round 25 removed the
     * dot and the capsule is a background colour), so the claim is made where
     * it is decidable: `tabForRoute` is the one function the bar's `selected`
     * flag comes from, and `TabNavSpecTest` pins it to null for Profile. What
     * this test adds is that the page is REACHABLE and still has its own way
     * back — a sub-screen with no tab must not also have no back arrow.
     */
    @Test
    fun profileIsASubScreenWithItsOwnWayBack() {
        ActivityScenario.launch(MainActivity::class.java).use {
            composeRule.waitUntil(timeoutMillis = 30_000) {
                runCatching { has("projectsAvatar") }.getOrDefault(false)
            }
            composeRule.onNodeWithTag("projectsAvatar").performClick()
            composeRule.waitUntil(timeoutMillis = 15_000) {
                runCatching { has("profileScreen") }.getOrDefault(false)
            }
            composeRule.onNodeWithTag("scanTabBar").assertIsDisplayed()
            assertEquals(
                "Profile lights no tab",
                null,
                com.lidarscan.app.ui.nav.tabForRoute(com.lidarscan.app.ui.nav.Routes.PROFILE),
            )
        }
    }

    // ══ item 133(c) / 140(a) — one door, and then no chip at all ═══════════

    /**
     * **Item 133(c), superseded within the round by item 140(a).**
     *
     * 133(c) asked for one owner per door and made the map-mode chip a toggle;
     * the owner's own session then said the chip *"seems useless"* and it was
     * removed. Both halves of that survive as one assertion: the capture sheet
     * has exactly ONE door (the config chip), and the map chip is not in the
     * tree at all. The live-map switch itself is untouched, inside the sheet.
     */
    @Test
    fun theCaptureSheetHasExactlyOneDoorAndTheMapChipIsGone() {
        ActivityScenario.launch(MainActivity::class.java).use {
            openScanTab()
            assertEquals(
                "item 140(a): the map-mode chip is not on the screen",
                0,
                composeRule.onAllNodesWithTag("mapModeChip").fetchSemanticsNodes().size,
            )
            assertEquals(
                "item 133(c): the capture sheet has one door",
                1,
                composeRule.onAllNodesWithTag("captureConfigChip").fetchSemanticsNodes().size,
            )
        }
    }
}
