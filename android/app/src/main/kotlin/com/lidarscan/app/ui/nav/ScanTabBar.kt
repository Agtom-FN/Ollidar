package com.lidarscan.app.ui.nav

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Radar
import androidx.compose.material.icons.outlined.Work
import androidx.compose.material3.HorizontalDivider
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
import com.lidarscan.app.ui.theme.ScanColors

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
    /**
     * ROUND 28 item 168 — **Layers, chosen by the owner off the mockup sheet.**
     *
     * A stack of scans is what this tab holds, and `FolderOpen` said "files on
     * a disk", which is the one thing about a project the operator never thinks
     * about.
     */
    PROJECTS("Projects", Icons.Filled.Layers),

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

    /**
     * ROUND 28 item 168. The outline briefcase — work queued and work done.
     * `Layers` moved to Projects, where a stack means something.
     */
    JOBS("Jobs", Icons.Outlined.Work),

    /**
     * ROUND 28 item 168 — **Menu, and the reason is the Scan screen.**
     *
     * `Tune` is Material's horizontal sliders glyph, and the Scan screen's
     * *scan-local* Advanced button used the same idea, so the app had two
     * different destinations wearing one icon: press the sliders on the Scan
     * page and you get per-scan settings, press the sliders in the tab bar and
     * you get app settings. The owner's fix, chosen from the icon sheet: the
     * tab is `Menu`, and Advanced on the Scan page becomes three **vertical**
     * faders ([com.lidarscan.app.ui.components.ScanIcons.AdvancedFaders]) so
     * the two can never be confused at a glance.
     */
    SETTINGS("Settings", Icons.Filled.Menu),
}

/**
 * Which tab a route lights, or **null** for a destination that is not under
 * any tab.
 *
 * This is the mockup's `TABOF` table, and it is the whole reason a secondary
 * screen can keep a plain back arrow: Review, Plan, Merge, RTK, the calibration
 * and connect wizards and the new-project flow are all *inside* Projects, so
 * they light Projects while showing their own back bar. Capture and Jobs light
 * themselves whether they were reached from the tab bar or from a project.
 *
 * ## ROUND 27 item 131 — Profile lights nothing
 *
 * The owner found the Profile page highlighting **Projects**, and the cause was
 * this function's `else` branch: Profile is not in the table, so it inherited
 * the Projects default that Review and Plan legitimately use.
 *
 * The item offered two answers — no tab, or the tab it was opened from — and
 * asked for the standard-Compose-navigation-correct one. That is **no tab**.
 * The `NavigationBar` contract in the official navigation-compose sample is
 * `currentDestination.hierarchy.any { it.route == tab.route }`: selection is a
 * statement that the current destination lives INSIDE that tab's graph, and
 * Profile does not live inside Projects — it is a peer of it, reachable from
 * the Projects hero's avatar today and from Settings' About row tomorrow. "The
 * tab it was opened from" would make the highlight depend on the back stack,
 * which is exactly the thing a tab bar must not do: the same page would light
 * a different tab depending on how you got there.
 *
 * So the return type is nullable, and null means every capsule is unlit. The
 * page keeps its own back arrow, which is the affordance that is actually true
 * there.
 */
fun tabForRoute(route: String?): ScanTab? = when {
    route == null -> ScanTab.PROJECTS
    route == Routes.SETTINGS -> ScanTab.SETTINGS
    // ROUND 27 item 131: a sub-screen of no tab. See the header.
    route == Routes.PROFILE -> null
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
 * ROUND 28 item 147 — **the tab bar comes down to level 0.**
 *
 * It was a floating capsule: inset 16 dp from each side and 12 dp from the
 * bottom, radius half its height, a translucent panel ground, a hairline border
 * and a 12 dp shadow "so it reads as floating *over* the content rather than as
 * a docked bar". That was a deliberate choice and it is the one being reversed,
 * for three reasons that are all consequences rather than taste:
 *
 *  * **It was the third floating layer.** The owner's own rule — only warnings
 *    and the scan FAB may float — was broken by the status card, the `?` FAB,
 *    the gear button and this. When four things float, nothing does.
 *  * **It guillotined the last row of every list.** A translucent bar over
 *    scrolling content with no scrim and no fade cuts the final Projects card
 *    through the middle of its title, which reads as a clipping bug because it
 *    is indistinguishable from one.
 *  * **It cost every screen 32 dp of width**, permanently, for the two side
 *    insets — on a 360 dp phone that is nearly a tenth of the display spent on
 *    the gap around a bar.
 *
 * So: opaque, full width, anchored to the bottom edge, 64 dp, with a top
 * hairline and no shadow. The active tab is still the Agtom orange glyph in its
 * soft ember capsule — item 168 keeps that, and it is the one place besides the
 * primary action where the accent law spends the orange.
 *
 * The `tab_*` test tags and the per-icon `contentDescription` accessible names
 * (round 24 item 107) are untouched: the emulator suite drives the whole app
 * through those tags, and a bar of four undescribed glyphs is not a
 * simplification.
 */
@Composable
fun ScanTabBar(
    /** ROUND 27 item 131: null while on a destination that is under no tab (Profile). */
    current: ScanTab?,
    onSelect: (ScanTab) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier
            .fillMaxWidth()
            .height(ScanDims.TabBar)
            .testTag("scanTabBar"),
        color = MaterialTheme.colorScheme.surfaceContainer,
        // Level 0. See the header.
        shadowElevation = 0.dp,
        tonalElevation = 0.dp,
    ) {
        Column {
            HorizontalDivider(thickness = ScanDims.Hair, color = MaterialTheme.colorScheme.outlineVariant)
            Row(
                modifier = Modifier.fillMaxWidth().weight(1f).padding(horizontal = ScanDims.S1),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(ScanDims.S1),
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
}

@Composable
private fun TabButton(
    tab: ScanTab,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val shape = RoundedCornerShape(ScanDims.S6)
    Box(
        modifier = modifier
            .height(ScanDims.Touch)
            .background(if (selected) EmberSoft else Color.Transparent, shape)
            .clickable(role = Role.Tab, onClick = onClick)
            .testTag("tab_${tab.name.lowercase()}"),
        contentAlignment = Alignment.Center,
    ) {
        // ROUND 28 item 168: all four glyphs at ONE size on ONE baseline. They
        // were 23 dp of four different families (a filled folder, an outlined
        // radar, filled layers, a filled tune) and the mixed weights are half
        // of why the bar read as four unrelated stickers.
        Icon(
            tab.icon,
            // ROUND 24 item 107: the label WAS the accessible name. With the
            // label gone this is the accessible name, and it is the same string
            // — `ScanTab.label` — so a rename can never desynchronise what is
            // seen from what is announced.
            contentDescription = tab.label,
            modifier = Modifier.size(24.dp),
            // ROUND 28 item 168: the unselected tint was `InkFaint`, the "UI
            // only, never text" step, which is exactly the complaint that the
            // icons were invisible. Unselected is `ink-mute` — a real ≥4.5:1
            // token — and selected is the accent.
            tint = if (selected) ScanColors.primary else ScanColors.inkMute,
        )
    }
}
