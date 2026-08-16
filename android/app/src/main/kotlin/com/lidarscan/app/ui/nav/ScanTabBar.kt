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
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.theme.Ember
import com.lidarscan.app.ui.theme.EmberSoft
import com.lidarscan.app.ui.theme.InkFaint
import com.lidarscan.app.ui.theme.UiFontFamily

/**
 * The four top-level sections. `Capture` and `Jobs` are top-level in the
 * redesign — they used to be reachable only through a project's detail screen,
 * which is two taps from anywhere and the reason the mockup's checklist item
 * `a-nav-tabbar` says "all reachable in one tap".
 */
enum class ScanTab(val label: String, val icon: ImageVector) {
    PROJECTS("Projects", Icons.Filled.FolderOpen),
    CAPTURE("Capture", Icons.Filled.Radar),
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
    route == Routes.CAPTURE_PICK -> ScanTab.CAPTURE
    route == Routes.JOBS_PICK -> ScanTab.JOBS
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
 * The active tab is an ember-washed capsule with ember ink. Each button's icon
 * carries **no** content description on purpose: the visible label is the
 * accessible name, and duplicating it as a description would put a second
 * "Settings" node on the Projects screen next to the hero's avatar button.
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
                contentDescription = null,
                modifier = Modifier.size(21.dp),
                tint = if (selected) Ember else InkFaint,
            )
            Spacer(Modifier.height(3.dp))
            Text(
                tab.label,
                fontFamily = UiFontFamily,
                fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
                fontSize = 11.sp,
                color = if (selected) Ember else InkFaint,
            )
        }
    }
}
