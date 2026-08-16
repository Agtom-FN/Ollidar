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
    errorContainer = Color(0x33E05252),
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
)

private val LightColors = lightColorScheme(
    primary = EmberDim,
    onPrimary = Color.White,
    primaryContainer = EmberSoft,
    onPrimaryContainer = EmberDim,
    secondary = Color(0xFF1E8C7C),
    onSecondary = Color.White,
    tertiary = Color(0xFF9A7A1E),
    onTertiary = Color.White,
    error = SemBad,
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

    MaterialTheme(
        colorScheme = colorScheme,
        typography = LidarScanTypography,
        shapes = LidarScanShapes,
        content = content,
    )
}
