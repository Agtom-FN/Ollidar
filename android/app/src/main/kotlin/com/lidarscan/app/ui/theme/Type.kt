package com.lidarscan.app.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.lidarscan.app.R

/**
 * The redesign's three-family type system, bundled — not fetched.
 *
 * * **Space Grotesk** (display / titles) — the geometric-with-a-kink face the
 *   mockup's `--display` token names. Hero titles, card titles, sheet titles.
 * * **Inter** (UI) — body, labels, buttons.
 * * **JetBrains Mono** (telemetry) — every number that ticks: the stat panel,
 *   the diagnostics rows, project meta lines, chip codes.
 *
 * All three are SIL OFL 1.1, which permits bundling; the licences ship in the
 * APK at `assets/fonts/OFL-*.txt` rather than only in a repo file, so the
 * attribution travels with the binary.
 *
 * **What is actually in `res/font/`**: not the upstream files. Each family is
 * downloaded from google/fonts as its *variable* master, then
 * `instantiateVariableFont` pins the weights this app uses and `pyftsubset`
 * cuts the charset to Latin + the punctuation/symbols the UI genuinely draws
 * (`·`, `→`, `✓`, `Δ`, `±`, `—`, the arrows and math blocks). Interpolating
 * once at build-prep time rather than shipping the variable master keeps the
 * runtime free of `FontVariation` support questions on the minSdk-29 floor,
 * and the subset is the difference between ~1.2 MB and ~690 KB of APK. The
 * recipe is recorded in android/NOTES.md so a later weight can be regenerated
 * the same way.
 */
val DisplayFontFamily = FontFamily(
    Font(R.font.space_grotesk_medium, FontWeight.Medium),
    Font(R.font.space_grotesk_semibold, FontWeight.SemiBold),
    Font(R.font.space_grotesk_bold, FontWeight.Bold),
)

val UiFontFamily = FontFamily(
    Font(R.font.inter_regular, FontWeight.Normal),
    Font(R.font.inter_medium, FontWeight.Medium),
    Font(R.font.inter_semibold, FontWeight.SemiBold),
)

val MonoFontFamily = FontFamily(
    Font(R.font.jetbrains_mono_regular, FontWeight.Normal),
    Font(R.font.jetbrains_mono_medium, FontWeight.Medium),
)

/**
 * The mockup's telemetry treatment as one style: mono, tight size, the wide
 * tracking that makes a code (`MID-360`, `EPSG 32650`, `RTK FIXED`) read as an
 * instrument label rather than a word.
 */
val MonoLabel = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Normal,
    fontSize = 10.sp,
    lineHeight = 14.sp,
    letterSpacing = 0.14.em,
)

/** Mono, but for values that are read rather than scanned — no extra tracking. */
val MonoValue = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Medium,
    fontSize = 13.sp,
    lineHeight = 17.sp,
    letterSpacing = 0.sp,
)

/**
 * ROUND 25 item 116 — **a live number that does not twitch.**
 *
 * The tracking-lost popup counts seconds under its headline, centred. JetBrains
 * Mono is fixed-pitch, so "9 s" and "10 s" already differ by one cell — but the
 * count is CENTRED, so every digit added or removed shifts the whole string
 * half a cell sideways, once a second, in the middle of a card the operator is
 * being told to hold still in front of. `tnum` states the intent explicitly
 * (and is what makes this correct if the family ever changes), and the tracking
 * drops to zero: [MonoLabel]'s 0.14 em is for scanning a code like `MID-360`,
 * not for reading a number that is changing while you look at it.
 */
val MonoTabular = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Medium,
    fontSize = 15.sp,
    lineHeight = 20.sp,
    letterSpacing = 0.sp,
    fontFeatureSettings = "tnum",
)

/** The mono meta line under a project card. */
val MonoMeta = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Normal,
    fontSize = 11.sp,
    lineHeight = 15.sp,
    letterSpacing = 0.01.em,
)

/** Section header inside a bottom sheet — mono, uppercase, ember (colour applied at use site). */
val SheetSectionLabel = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Medium,
    fontSize = 10.sp,
    lineHeight = 14.sp,
    letterSpacing = 0.16.em,
)

/**
 * Display for headlines/titles, Inter for everything else, and the mono styles
 * above applied explicitly where telemetry is drawn. Material's own roles are
 * kept intact so un-restyled Material components (dialogs, snackbars, text
 * fields) still land on the right family.
 */
val LidarScanTypography = Typography().let { base ->
    Typography(
        displayLarge = base.displayLarge.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.02).em),
        displayMedium = base.displayMedium.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.02).em),
        displaySmall = base.displaySmall.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.02).em),
        headlineLarge = base.headlineLarge.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.Bold, letterSpacing = (-0.025).em),
        headlineMedium = base.headlineMedium.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.Bold, letterSpacing = (-0.025).em),
        headlineSmall = base.headlineSmall.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.02).em),
        titleLarge = base.titleLarge.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.02).em),
        titleMedium = base.titleMedium.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.015).em),
        titleSmall = base.titleSmall.copy(fontFamily = DisplayFontFamily, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.01).em),
        bodyLarge = base.bodyLarge.copy(fontFamily = UiFontFamily),
        bodyMedium = base.bodyMedium.copy(fontFamily = UiFontFamily),
        bodySmall = base.bodySmall.copy(fontFamily = UiFontFamily),
        labelLarge = base.labelLarge.copy(fontFamily = UiFontFamily, fontWeight = FontWeight.SemiBold),
        labelMedium = base.labelMedium.copy(fontFamily = UiFontFamily, fontWeight = FontWeight.Medium),
        labelSmall = base.labelSmall.copy(fontFamily = UiFontFamily, fontWeight = FontWeight.Medium),
    )
}
