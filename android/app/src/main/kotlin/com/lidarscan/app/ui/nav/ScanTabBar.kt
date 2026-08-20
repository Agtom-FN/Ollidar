package com.lidarscan.app.ui.nav

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.Radar
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.EmberSoft
import com.lidarscan.app.ui.theme.InkFaint

/**
 * The four top-level sections. `Capture` and `Jobs` are top-level in the
 * redesign — they used to be reachable only through a project's detail screen,
 * which is two taps from anywhere and the reason the mockup's checklist item
 * `a-nav-tabbar` says "all reachable in one tap".
 */
enum class ScanTab(val label: String, val icon: ImageVector) {
    /**
     * ROUND 22 item 94: **Projects keeps its name**, at the owner's explicit
     * request. It was the one tab the simplification was not asked to touch.
     */
    PROJECTS("Projects", Icons.Filled.FolderOpen),

    /**
     * ROUND 22 item 94 — labelled **"Scan"**, not "Capture".
     *
     * "Capture" is what the engine does; "Scan" is what the operator does, and
     * it is the word the owner uses. Only the LABEL changes: the enum constant,
     * `Routes.CAPTURE_NEW`, `tabForRoute`, every test tag (`tab_capture`) and
     * every route string keep their names. Renaming a route mid-round is a
     * back-stack risk (ROUND 16 named it) bought for nothing — the operator
     * never sees a route string.
     */
    CAPTURE("Scan", Icons.Filled.Radar),

    JOBS("Jobs", Icons.Filled.Layers),
    SETTINGS("Settings", Icons.Filled.Tune),
}

/**
 * Which tab a route lights.
 *
 * This is the mockup's `TABOF` table, and it is the whole reason a secondary
 * screen can keep a plain back arrow: Review, Plan, Merge, RTK, the calibration
 * and connect wizards and the new-project flow are all *inside* Projects, so
 * they light Projects while showing their own back bar. Capture and Jobs light
 * themselves whether they were reached from the tab bar or from a project.
 */
fun tabForRoute(route: String?): ScanTab = when {
    route == null -> ScanTab.PROJECTS
    route == Routes.SETTINGS -> ScanTab.SETTINGS
    route == Routes.CAPTURE_NEW -> ScanTab.CAPTURE
    route == Routes.JOBS_PICK -> ScanTab.JOBS
    // ROUND 23 item 106(c): the project-less Mid-360 wizard is a Scan-tab
    // errand — it is opened from the Scan tab and it comes back to it.
    route == Routes.MID360_SETUP -> ScanTab.CAPTURE
    route.endsWith("/capture") || route.endsWith("/capture/replay") -> ScanTab.CAPTURE
    route.endsWith("/processing") -> ScanTab.JOBS
    else -> ScanTab.PROJECTS
}

/**
 * The floating capsule tab bar: inset 16 dp from each side and 12 dp from the
 * bottom, 58 dp tall, radius half its height, a translucent panel ground with a
 * hairline border and a shadow so it reads as floating *over* the content
 * rather than as a docked bar.
 *
 * The active tab is an ember-washed capsule with ember ink.
 *
 * ## ROUND 24 item 107 — **icons only, centred**
 *
 * The four labels are gone at the owner's request and the icons centre in
 * their capsules. Two consequences were handled rather than discovered:
 *
 *  * **The accessible name moved.** It used to be the visible `Text`; it is now
 *    each icon's `contentDescription` ("Scan", "Projects", "Jobs",
 *    "Settings"), which is the same string from the same enum. A bar of four
 *    undescribed glyphs is not a simplification, it is an app a screen reader
 *    cannot use.
 *  * **"Settings" was already taken.** The Projects hero's avatar carried
 *    `contentDescription = "Settings"`, and the smoke test asserts that node is
 *    unambiguous. Item 109 turns that button into the door to the **Profile**
 *    page, so it is described as "Profile" now and the collision resolves
 *    itself — one name, one node, one destination.
 *
 * Selection is the Agtom orange **plus a 4 dp dot** under the icon. Colour
 * alone was survivable while a bold label sat beneath it; on a bar that is now
 * four glyphs, one of them tinted, it is not — the dot is the state, and the
 * tint is the emphasis.
 */
@Composable
fun ScanTabBar(
    current: ScanTab,
    onSelect: (ScanTab) -> Unit,
    modifier: Modifier = Modifier,
) {
    val shape = RoundedCornerShape(ScanDims.TabBar / 2)
    Surface(
        modifier = modifier
            .fillMaxWidth()
            .padding(
                start = ScanDims.TabBarSideInset,
                end = ScanDims.TabBarSideInset,
                bottom = ScanDims.TabBarBottomInset,
            )
            .height(ScanDims.TabBar)
            .testTag("scanTabBar"),
        shape = shape,
        color = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.94f),
        shadowElevation = 12.dp,
        tonalElevation = 0.dp,
    ) {
        Row(
            modifier = Modifier
                .border(1.dp, MaterialTheme.colorScheme.outline, shape)
                .padding(5.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            ScanTab.entries.forEach { tab ->
                TabButton(
                    tab = tab,
                    selected = tab == current,
                    onClick = { onSelect(tab) },
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}

@Composable
private fun TabButton(
    tab: ScanTab,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val shape = RoundedCornerShape(24.dp)
    Box(
        modifier = modifier
            .height(48.dp)
            .background(if (selected) EmberSoft else Color.Transparent, shape)
            .clickable(role = Role.Tab, onClick = onClick)
            .testTag("tab_${tab.name.lowercase()}"),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(
                tab.icon,
                // ROUND 24 item 107: the label WAS the accessible name. With
                // the label gone this is the accessible name, and it is the
                // same string — `ScanTab.label` — so a rename can never
                // desynchronise what is seen from what is announced.
                contentDescription = tab.label,
                modifier = Modifier.size(23.dp),
                tint = if (selected) Ember else InkFaint,
            )
            // The selected dot. 4 dp, ember, 4 dp below the glyph; an
            // invisible spacer of the same height on the unselected tabs so
            // the icons stay on one baseline instead of hopping 8 dp as the
            // selection moves.
            Spacer(Modifier.height(4.dp))
            Box(
                Modifier
                    .size(4.dp)
                    .background(
                        if (selected) Ember else Color.Transparent,
                        RoundedCornerShape(2.dp),
                    )
                    .testTag(if (selected) "tabSelectedDot" else "tabUnselectedDot"),
            )
        }
    }
}
