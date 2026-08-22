package com.lidarscan.app.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.lidarscan.app.data.ThemeMode

/**
 * The redesign's tokens mapped onto Material roles, so a component that was
 * never restyled (a dialog, a snackbar, a text field) still lands in the right
 * world without every call site being rewritten:
 *
 * | token          | role                                                    |
 * |----------------|---------------------------------------------------------|
 * | ground         | `background`, `surface`                                  |
 * | panel          | `surfaceContainer`, `surfaceContainerLow`                 |
 * | panel-2        | `surfaceContainerHigh`, `surfaceVariant`                  |
 * | line           | `outline`, `outlineVariant`                              |
 * | ink            | `onBackground`, `onSurface`                              |
 * | mute           | `onSurfaceVariant`                                       |
 * | ember          | `primary` (with `OnEmber` as `onPrimary`)                |
 * | teal / sand    | `secondary` / `tertiary`                                 |
 * | bad            | `error`                                                  |
 *
 * The semantic trio (good/warn/bad) and the sensor identities stay as fixed
 * values in `Color.kt` — a "Float" badge must not change meaning with the
 * theme.
 */
private val DarkColors = darkColorScheme(
    primary = Ember,
    onPrimary = OnEmber,
    primaryContainer = EmberSoft,
    onPrimaryContainer = Ember,
    secondary = ScanTeal,
    onSecondary = OnEmber,
    secondaryContainer = PanelAlt,
    onSecondaryContainer = ScanTeal,
    tertiary = ScanSand,
    onTertiary = OnEmber,
    tertiaryContainer = PanelAlt,
    onTertiaryContainer = ScanSand,
    error = SemBad,
    onError = OnEmber,
    errorContainer = SemBad.over(Panel, 0.14f),
    onErrorContainer = SemBad,
    background = Ground,
    onBackground = Ink,
    surface = Ground,
    onSurface = Ink,
    surfaceVariant = PanelAlt,
    onSurfaceVariant = InkMute,
    surfaceContainerLowest = Ground,
    surfaceContainerLow = Panel,
    surfaceContainer = Panel,
    surfaceContainerHigh = PanelAlt,
    surfaceContainerHighest = PanelAlt,
    outline = LineColor,
    outlineVariant = LineSoft,
    scrim = Color(0xCC0A0D11),
    inverseSurface = Ink,
    inverseOnSurface = Ground,
    inversePrimary = Ember,
    surfaceTint = Ember,
)

/**
 * ROUND 28 item 148 — **every role, filled.**
 *
 * The light scheme used to declare twenty-three of Material's roles and leave
 * the rest to `lightColorScheme()`'s own defaults, which are the M3 baseline
 * purple. That is not a theoretical hazard; it shipped two visible bugs:
 *
 *  * `inverseOnSurface` was never set, so `Snackbar` — which takes its content
 *    colour from `SnackbarDefaults.contentColor` = `inverseOnSurface` — drew
 *    near-white `#F4EFF4` text on the `#E8ECF0` container the export toast
 *    overrode. **1.05:1.** The owner exported the same file twice, 22 seconds
 *    apart, because he could not read the message telling him the first one
 *    worked.
 *  * `secondaryContainer` was never set, so Review's selected `PLY` filter chip
 *    inherited the baseline lavender — the review's "a colour that exists
 *    nowhere in the palette", and it was right, because it came from Material
 *    rather than from this file.
 *
 * Both are the same defect: a hole in the scheme is a hole any un-restyled
 * Material component can fall into, and the app has dozens it has never
 * restyled. So the list below is complete, and the two `inverse*` roles that
 * caused the toast are pinned deliberately — `inverseSurface` is the ink
 * colour and `inverseOnSurface` the page colour, which is what "inverse"
 * means and what makes a default-styled snackbar legible without being
 * restyled at all.
 */
private val LightColors = lightColorScheme(
    // ROUND 22 item 93: the SAME Agtom orange as the dark theme. The light
    // theme used to run a darkened ember (`EmberDim`) as its primary so that
    // white could sit on it; with one brand token that split is gone, and
    // `OnEmber`'s near-black carries about 6.9:1 against #F26A1B where white
    // carried about 3.0:1 — better contrast AND one accent instead of two.
    primary = Ember,
    onPrimary = OnEmber,
    primaryContainer = EmberSoft,
    onPrimaryContainer = EmberDim,
    secondary = ScanTealLight,
    onSecondary = Color.White,
    // The container pair a `FilterChip` reaches for when nothing restyles it —
    // the trough, not Material's lavender. See the header.
    secondaryContainer = PanelAltLight,
    onSecondaryContainer = InkLight,
    tertiary = ScanSandLight,
    onTertiary = Color.White,
    tertiaryContainer = PanelAltLight,
    onTertiaryContainer = InkLight,
    error = SemBadLight,
    onError = Color.White,
    errorContainer = SemBadLight.over(PanelLight, 0.14f),
    onErrorContainer = SemBadLight,
    background = GroundLight,
    onBackground = InkLight,
    surface = GroundLight,
    onSurface = InkLight,
    surfaceVariant = PanelAltLight,
    onSurfaceVariant = InkMuteLight,
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = PanelLight,
    surfaceContainer = PanelLight,
    surfaceContainerHigh = PanelAltLight,
    surfaceContainerHighest = PanelAltLight,
    outline = LineLight,
    outlineVariant = LineLight,
    scrim = Color(0x99000000),
    // The two roles the export toast fell through. Inverse means inverse:
    // ink-coloured ground, page-coloured ink.
    inverseSurface = InkLight,
    inverseOnSurface = GroundLight,
    inversePrimary = Ember,
    surfaceTint = Ember,
)

/**
 * Rounder shapes, per the redesign brief: cards land on 20 dp
 * (`--r-panel`), tiles on 14 dp (`--r-tile`), and anything button-shaped is a
 * pill. `extraLarge` is the pill — Material's `Button`/`FilterChip`/
 * `SegmentedButton` defaults read from the shape scheme, so setting it here is
 * what makes an un-restyled button round the same way a hand-built one does.
 */
val LidarScanShapes = Shapes(
    extraSmall = RoundedCornerShape(8.dp),
    small = RoundedCornerShape(12.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(20.dp),
    extraLarge = RoundedCornerShape(percent = 50),
)

/**
 * App-wide Material 3 theme.
 *
 * **Dynamic colour is off by default now.** It was on while the palette was an
 * admitted placeholder; the redesign's ember accent is the product's one
 * identity colour and a wallpaper-derived scheme would replace exactly that.
 * The parameter is kept so a caller can still opt in.
 */
@Composable
fun LidarScanTheme(
    themeMode: ThemeMode = ThemeMode.SYSTEM,
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit,
) {
    val useDarkTheme = when (themeMode) {
        ThemeMode.SYSTEM -> isSystemInDarkTheme()
        ThemeMode.LIGHT -> false
        ThemeMode.DARK -> true
    }

    val supportsDynamicColor = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
    val context = LocalContext.current
    val colorScheme = when {
        dynamicColor && supportsDynamicColor ->
            if (useDarkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        useDarkTheme -> DarkColors
        else -> LightColors
    }

    // ROUND 28 item 144: the semantics travel with the Material scheme rather
    // than beside it. Providing them here — not at each screen — is what makes
    // `ScanColors.warn` correct on every surface in the app including the ones
    // nobody remembered to audit.
    CompositionLocalProvider(LocalScanColors provides scanColorScheme(useDarkTheme)) {
        MaterialTheme(
            colorScheme = colorScheme,
            typography = LidarScanTypography,
            shapes = LidarScanShapes,
            content = content,
        )
    }
}
