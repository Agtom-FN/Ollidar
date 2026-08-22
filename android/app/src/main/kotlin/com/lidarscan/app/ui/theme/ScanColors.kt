package com.lidarscan.app.ui.theme

import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import com.lidarscan.core.model.SensorType

/**
 * ROUND 28 items 144/145/146 — **the half of the palette Material has no role
 * for, resolved against the theme instead of frozen at a hex.**
 *
 * Material's `ColorScheme` covers primary/surface/error and stops. Everything
 * this app actually means — *good*, *warn*, *thin coverage*, *this is a
 * COIN-D6*, *this is the 3-D viewport's own ground* — lived in `Color.kt` as
 * top-level `val`s, and a top-level `val` cannot know which theme is running.
 * That is not a stylistic complaint: it is the mechanism by which the light
 * theme shipped with nine tokens between 1.56:1 and 3.06:1, and the mechanism
 * by which `SemWarn.over(Panel, …)` painted a dark olive box on a white page.
 *
 * So the semantics become a **scheme**, provided by [LidarScanTheme] and read
 * through [ScanColors] the same way a Material role is read through
 * `MaterialTheme.colorScheme`. A call site that says `ScanColors.warn` gets the
 * light amber on a white page and the dark amber on a dark one, and there is no
 * spelling of "the warning colour" left that can get it wrong.
 *
 * `staticCompositionLocalOf` rather than `compositionLocalOf`: the scheme
 * changes only when the theme does, and when it does every screen is
 * recomposing anyway, so the cheaper read is the right trade — this is the same
 * choice `MaterialTheme` makes for its own `LocalColorScheme`.
 */
@Immutable
data class ScanColorScheme(
    /** True for the dark theme. Read it to choose an asset, never to choose a colour. */
    val isDark: Boolean,

    // ── surfaces (§C.3) ────────────────────────────────────────────────────
    /** The screen's ground. */
    val page: Color,

    /** A card or a row group sitting on [page]. */
    val card: Color,

    /** Segmented-control tracks, tiles, thumbnails' empty state, disabled fills. */
    val trough: Color,

    /** The 1 dp hairline that separates rows and edges cards. */
    val line: Color,

    /**
     * The 3-D viewport's own ground — always darker than [page], in both
     * themes, because a point cloud is drawn in light and needs a dark room.
     */
    val viewport: Color,

    // ── ink ────────────────────────────────────────────────────────────────
    val ink: Color,

    /** Secondary text. ≥4.5:1 on [page] in both themes. */
    val inkMute: Color,

    /**
     * ~3.5:1 — **UI only, never text.** Inert chevrons, disabled labels, ticks.
     * The one exception to "every string is at least 4.5:1", named so it cannot
     * be reached for by accident.
     */
    val inkFaint: Color,

    // ── brand ──────────────────────────────────────────────────────────────
    /** The Agtom orange fill. Identical in both themes — see [AgtomOrange]. */
    val primary: Color,

    /** The near-black that sits on [primary]. */
    val onPrimary: Color,

    /**
     * **Orange as ink.** The brand fill measures 3.06:1 as text on the light
     * page; this is its text-safe companion (4.9:1), and in the dark theme it
     * is the same value as [primary] because there it already passes.
     */
    val primaryInk: Color,

    // ── semantics ──────────────────────────────────────────────────────────
    val good: Color,
    val warn: Color,

    /** Thin coverage. Amber, not red: an instruction, not an alarm (round 11). */
    val coverageAmber: Color,

    /**
     * **`bad` means an operation failed and the operator lost something.**
     *
     * Round 28 item 163: it does not mean "unavailable", "not applicable" or
     * "a different pipeline". Those are [inkMute]. This one sentence is what
     * deleted the 62-word red paragraph from the Jobs screen.
     */
    val bad: Color,

    // ── sensor identity ────────────────────────────────────────────────────
    val sensorD6: Color,
    val sensorMid360: Color,
    val sensorStl27l: Color,
) {
    /**
     * A semantic's **container**: the colour itself at low alpha over [card].
     *
     * ROUND 28 item 145. Round 25 derived these the same way and composited
     * over a dark constant; the derivation is kept because it is what stops a
     * container drifting from its semantic when a token moves, and the ground
     * is now whatever the current theme's card is.
     */
    fun container(semantic: Color, alpha: Float = 0.14f): Color = semantic.over(card, alpha)

    val warnContainer: Color get() = container(warn)
    val goodContainer: Color get() = container(good)
    val badContainer: Color get() = container(bad)

    /**
     * ROUND 25 item 119's exhaustive decision, unchanged — only the palette
     * lookup moved here so it can be theme-correct. A fourth sensor still
     * breaks the build in `:core` (`SensorType.badgeTint`) rather than being
     * silently painted teal by an `else`.
     */
    fun sensor(sensor: SensorType): Color = when (sensor.badgeTint) {
        SensorType.BadgeTint.D6 -> sensorD6
        SensorType.BadgeTint.MID360 -> sensorMid360
        SensorType.BadgeTint.STL27L -> sensorStl27l
    }

    /**
     * ROUND 28 item 149 — **the tint a chip or a dot uses for a grade.**
     *
     * `GOOD` is good, `FAIR` is the norm and therefore [inkMute] rather than a
     * colour (a fleet where most scans are FAIR must not be a wall of amber),
     * `POOR` and `2D ONLY` warn, and an empty scan is the one that genuinely
     * lost the operator something.
     */
    fun grade(grade: String?): Color = when (grade?.uppercase()) {
        "GOOD" -> good
        "POOR", "2D ONLY", "2D_ONLY" -> warn
        "EMPTY" -> bad
        else -> inkMute
    }
}

private val DarkScanColors = ScanColorScheme(
    isDark = true,
    page = Ground,
    card = Panel,
    trough = PanelAlt,
    line = LineColor,
    viewport = ViewportGround,
    ink = Ink,
    inkMute = InkMute,
    inkFaint = InkFaint,
    primary = Ember,
    onPrimary = OnEmber,
    // Dark's orange already passes as text (5.35:1), so the split role
    // collapses back to one value here. See [ScanColorScheme.primaryInk].
    primaryInk = Ember,
    good = SemGood,
    warn = SemWarn,
    coverageAmber = CoverageAmber,
    bad = SemBad,
    sensorD6 = ScanTeal,
    sensorMid360 = PoseBlue,
    sensorStl27l = ScanSand,
)

private val LightScanColors = ScanColorScheme(
    isDark = false,
    page = GroundLight,
    card = PanelLight,
    trough = PanelAltLight,
    line = LineLight,
    viewport = ViewportGroundLight,
    ink = InkLight,
    inkMute = InkMuteLight,
    inkFaint = InkFaintLight,
    primary = Ember,
    onPrimary = OnEmber,
    primaryInk = EmberInkLight,
    good = SemGoodLight,
    warn = SemWarnLight,
    coverageAmber = CoverageAmberLight,
    bad = SemBadLight,
    sensorD6 = ScanTealLight,
    sensorMid360 = PoseBlueLight,
    sensorStl27l = ScanSandLight,
)

internal fun scanColorScheme(dark: Boolean): ScanColorScheme =
    if (dark) DarkScanColors else LightScanColors

/**
 * Defaults to the dark scheme so a preview or a test that forgets
 * [LidarScanTheme] still renders the app's own colours rather than transparent
 * black — the same defensive default `MaterialTheme` uses for its scheme.
 */
val LocalScanColors = staticCompositionLocalOf { DarkScanColors }

/**
 * The accessor. `ScanColors.warn` reads like `MaterialTheme.colorScheme.error`
 * and costs the same: a static local read, resolved at composition.
 */
object ScanColors {
    val current: ScanColorScheme
        @Composable @ReadOnlyComposable get() = LocalScanColors.current

    val isDark: Boolean @Composable @ReadOnlyComposable get() = current.isDark
    val page: Color @Composable @ReadOnlyComposable get() = current.page
    val card: Color @Composable @ReadOnlyComposable get() = current.card
    val trough: Color @Composable @ReadOnlyComposable get() = current.trough
    val line: Color @Composable @ReadOnlyComposable get() = current.line
    val viewport: Color @Composable @ReadOnlyComposable get() = current.viewport
    val ink: Color @Composable @ReadOnlyComposable get() = current.ink
    val inkMute: Color @Composable @ReadOnlyComposable get() = current.inkMute
    val inkFaint: Color @Composable @ReadOnlyComposable get() = current.inkFaint
    val primary: Color @Composable @ReadOnlyComposable get() = current.primary
    val onPrimary: Color @Composable @ReadOnlyComposable get() = current.onPrimary
    val primaryInk: Color @Composable @ReadOnlyComposable get() = current.primaryInk
    val good: Color @Composable @ReadOnlyComposable get() = current.good
    val warn: Color @Composable @ReadOnlyComposable get() = current.warn
    val coverageAmber: Color @Composable @ReadOnlyComposable get() = current.coverageAmber
    val bad: Color @Composable @ReadOnlyComposable get() = current.bad
    val sensorD6: Color @Composable @ReadOnlyComposable get() = current.sensorD6
    val sensorMid360: Color @Composable @ReadOnlyComposable get() = current.sensorMid360
    val sensorStl27l: Color @Composable @ReadOnlyComposable get() = current.sensorStl27l
    val warnContainer: Color @Composable @ReadOnlyComposable get() = current.warnContainer
    val goodContainer: Color @Composable @ReadOnlyComposable get() = current.goodContainer
    val badContainer: Color @Composable @ReadOnlyComposable get() = current.badContainer
}

/**
 * ROUND 25 item 119's four draw sites, now theme-correct.
 *
 * Kept as a free function under its original name so the Projects card, the
 * thumbnail, the picker and the capture pill did not all need rewriting to gain
 * a light theme — the signature is the same, only the resolution moved inside.
 */
@Composable
@ReadOnlyComposable
fun sensorBadgeColor(sensor: SensorType): Color = ScanColors.current.sensor(sensor)
