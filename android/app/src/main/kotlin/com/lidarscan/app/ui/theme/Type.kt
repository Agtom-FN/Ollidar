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
 * ROUND 28 item 167 — **four sizes, and a rule for which one.**
 *
 * The app had eleven text treatments and no rule, so the same row could carry
 * `QUICK SCAN` in 10 sp over-tracked uppercase mono beside `Re-zero` in
 * sentence-case Space Grotesk. Two typographic voices arguing inside one row is
 * the most reliable single signal of an amateur interface, and it was not a
 * drift — it was the *default*, because [MonoLabel] was the chip style and
 * chips carried nouns.
 *
 * The scale below is the whole system:
 *
 * | role | face | size / line | use |
 * |---|---|---|---|
 * | [ScanDisplay] | Space Grotesk Bold | 28 / 34 | screen title — **one per screen** |
 * | [ScanTitle] | Space Grotesk SemiBold | 17 / 24 | card title, row title, button label |
 * | [ScanBody] | Inter Regular | 15 / 22 | prose, instructions, secondary labels |
 * | [ScanMeta] | JetBrains Mono | 12 / 16 | **every** number, timestamp, unit |
 * | [ScanMetaCaps] | JetBrains Mono Medium | 12 / 16, +0.08 em | **codes only** |
 *
 * **The rule that ends the argument:** if it is a word a person would say out
 * loud — "Quick scan", "Height", "Mount set", "No rover" — it is [ScanTitle] or
 * [ScanBody], sentence case. If it is a code a person would spell out — `D6`,
 * `EPSG 32650`, `PLY`, `FAIR`, `REC` — it is [ScanMetaCaps]. Tracking is for
 * codes. `QUICK SCAN` and `MOUNT SET` are words; `D6` is a code.
 *
 * `tnum` is on for every Meta style, always. Round 25 item 116 discovered why
 * on one centred countdown — a digit added or removed shifts the whole string
 * half a cell, once a second, in front of an operator being told to hold still
 * — and that reason applies everywhere a number ticks, which is everywhere this
 * app draws a number.
 */
val ScanDisplay = TextStyle(
    fontFamily = DisplayFontFamily,
    fontWeight = FontWeight.Bold,
    fontSize = 28.sp,
    lineHeight = 34.sp,
    letterSpacing = (-0.03).em,
)

/** Card title, list-row title, button label. */
val ScanTitle = TextStyle(
    fontFamily = DisplayFontFamily,
    fontWeight = FontWeight.SemiBold,
    fontSize = 17.sp,
    lineHeight = 24.sp,
    letterSpacing = (-0.015).em,
)

/** All prose, all instructions, all secondary labels. */
val ScanBody = TextStyle(
    fontFamily = UiFontFamily,
    fontWeight = FontWeight.Normal,
    fontSize = 15.sp,
    lineHeight = 22.sp,
)

/** Every number, timestamp, unit and code. Tabular figures, always. */
val ScanMeta = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Normal,
    fontSize = 12.sp,
    lineHeight = 16.sp,
    letterSpacing = 0.sp,
    fontFeatureSettings = "tnum",
)

/**
 * **Codes only** — never a word, never a phrase. See the header's rule.
 *
 * 12 sp rather than [MonoLabel]'s 10 sp: 10 sp is below the practical floor and
 * at fontScale 1.3 the 0.14 em tracking made these wrap or ellipsise, which is
 * how `GEOREF ✓` became `GEOREF…` on the owner's phone.
 */
val ScanMetaCaps = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Medium,
    fontSize = 12.sp,
    lineHeight = 16.sp,
    letterSpacing = 0.08.em,
    fontFeatureSettings = "tnum",
)

/**
 * ROUND 28 item 167 — **the five legacy mono styles, kept only as aliases
 * while the sweep lands.**
 *
 * `MonoLabel`, `MonoValue`, `MonoTabular`, `MonoMeta` and `SheetSectionLabel`
 * are all the same idea at five sizes and three trackings, invented one at a
 * time. They collapse into [ScanMeta] and [ScanMetaCaps]. They are aliases
 * rather than deletions for one round so that a call site that has not been
 * swept yet still renders on the new scale instead of failing to compile in a
 * file nobody is editing this round — the point of the item is that the *pixels*
 * agree, and an alias delivers that immediately.
 */
@Deprecated("ROUND 28 item 167: use ScanMetaCaps (codes) or ScanMeta (numbers).", ReplaceWith("ScanMetaCaps"))
val MonoLabel = ScanMetaCaps

/** ROUND 28 item 167 alias — see [ScanMeta]. */
@Deprecated("ROUND 28 item 167: use ScanMeta.", ReplaceWith("ScanMeta"))
val MonoValue = ScanMeta

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

/**
 * ROUND 28 item 160 — the start panel's countdown, at Display size.
 *
 * The one place a number is the largest thing on screen: "6s" under "Hold
 * still", read at arm's length by someone who has been asked not to move. Mono
 * and tabular for round 25 item 116's reason, which this style inherits
 * wholesale.
 */
val ScanCountdown = TextStyle(
    fontFamily = MonoFontFamily,
    fontWeight = FontWeight.Medium,
    fontSize = 28.sp,
    lineHeight = 34.sp,
    letterSpacing = (-0.01).em,
    fontFeatureSettings = "tnum",
)

/** ROUND 28 item 167 alias — see [ScanMeta]. */
@Deprecated("ROUND 28 item 167: use ScanMeta.", ReplaceWith("ScanMeta"))
val MonoMeta = ScanMeta

/**
 * ROUND 28 item 167 alias — see [ScanMetaCaps].
 *
 * Item 164 also takes the *colour* off this: a section header is a passive
 * label and the accent law allows the Agtom orange at most twice per screen,
 * for the primary action and the active tab. Settings was spending it on three
 * headers that do nothing.
 */
@Deprecated("ROUND 28 item 167: use ScanMetaCaps.", ReplaceWith("ScanMetaCaps"))
val SheetSectionLabel = ScanMetaCaps

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
