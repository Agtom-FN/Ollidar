package com.lidarscan.app.ui.theme

import androidx.compose.ui.graphics.Color

/**
 * The redesign's palette (docs/design/REVIEW_FEEDBACK.md + the
 * `lidarscan-interfaces.html` mockup's `:root` block, with the owner-approved
 * token values from the redesign brief).
 *
 * These are **the** product colours now — the "neutral placeholder palette"
 * comment this file used to carry is gone, and so is dynamic colour by
 * default (see [LidarScanTheme]): a wallpaper-derived scheme would repaint
 * the one brand accent the whole cockpit is built around.
 *
 * Everything in `ui/` should reach for `MaterialTheme.colorScheme` where a
 * Material role exists ([ScanColors] maps them below), and for the values in
 * this file only for the fixed semantics — sensor identity, fix quality,
 * point-cloud ramp — which are deliberately theme-invariant so a "Float" badge
 * looks the same on every screen and in every theme.
 */

// ── surfaces ────────────────────────────────────────────────────────────────
/** Blue-biased near-black — the app's ground plane. */
val Ground = Color(0xFF12161B)

/** Raised panel (cards, tab bar, stat strip). */
val Panel = Color(0xFF1A2027)

/** Second panel step (segmented-control troughs, tiles inside a card). */
val PanelAlt = Color(0xFF222A33)

/** Hairline between panels. */
val LineColor = Color(0xFF2B3540)

/** A softer hairline for card borders that should read as edge, not divider. */
val LineSoft = Color(0xFF222A33)

/** The 3D viewport's own ground — darker than [Ground] so the cloud carries. */
val ViewportGround = Color(0xFF0B0E12)

// ── ink ─────────────────────────────────────────────────────────────────────
val Ink = Color(0xFFECF1F5)
val InkMute = Color(0xFF94A1AD)

/** Third ink step: units, sub-labels, "off" readouts. Derived from [InkMute]. */
val InkFaint = Color(0xFF63707C)

// ── accent ──────────────────────────────────────────────────────────────────

/**
 * ROUND 22 item 93 — **Agtom orange, and it is the one token.**
 *
 * The owner's brand orange replaces the redesign's laser ember as the app's
 * single accent, in **both** themes. Everything else about the accent stays
 * exactly where it was: no component changes shape, nothing moves, no control
 * is restyled. This is a repaint of one value.
 *
 * It is a `val` on its own line, and every other accent token below is derived
 * from it, so swapping the brand is one edit here rather than a hunt through
 * a palette. That is the whole reason it exists separately from [Ember].
 */
val AgtomOrange = Color(0xFFF26A1B)

/**
 * The one brand accent. Record, primary action, active tab, progress bars,
 * active chips, sliders.
 *
 * Kept under its ROUND-5 name rather than renamed to `AgtomOrange` at every
 * call site: `Ember` appears across the tab bar, the record button, the
 * capture HUD and the theme's `primary` mapping, and a rename touching every
 * one of those files would be a large diff that changes nothing — while
 * burying the actual change (the hex) inside it.
 */
val Ember = AgtomOrange

/**
 * Ember pressed/disabled companion — a darker Agtom orange, kept in the same
 * hue family rather than reused from the old ember ramp.
 */
val EmberDim = Color(0xFFC0530F)

/** The active-tab capsule wash and other ember-tinted grounds (15 % Agtom). */
val EmberSoft = Color(0x26F26A1B)

/**
 * Ink that sits **on** ember (record button core, primary-button label).
 *
 * Deliberately a near-black rather than white, on both themes: against
 * #F26A1B this near-black measures about 6.9:1 where white measures about
 * 3.0:1 — and the primary button carries the one instruction on the Scan
 * screen. See [com.lidarscan.app.ui.theme.LidarScanTheme].
 */
val OnEmber = Color(0xFF1A0D08)

// ── point-cloud ramp ────────────────────────────────────────────────────────
/** Height ramp, low end. Doubles as the COIN-D6 sensor identity. */
val ScanTeal = Color(0xFF3EC4B0)

/** Height ramp, high end. */
val ScanSand = Color(0xFFE5C468)

/** Trajectory / pose stream. Doubles as the Mid-360 sensor identity. */
val PoseBlue = Color(0xFF6AA7E8)

// ── semantics ───────────────────────────────────────────────────────────────
/** RTK fixed / healthy / done. */
val SemGood = Color(0xFF49D17F)

/** Float / degraded / warning. */
val SemWarn = Color(0xFFE5B93C)

/**
 * ROUND 19 item 75 — the coverage amber, matching the CoverageGrid tint's
 * (255, 176, 48) exactly, so the guidance ring and the coverage colour mode
 * speak the same "thin" in the same shade. Round 11's choice, restated there:
 * amber, not red — thin coverage is an instruction, not an alarm.
 */
val CoverageAmber = Color(0xFFFFB030)

/** Single / fault / failed. */
val SemBad = Color(0xFFE05252)

// ── semantic CONTAINERS (ROUND 25 item 116) ────────────────────────────────
//
// Owner, on the round-24 tracking-lost popup: *"revise the warning align the
// style."* The popup was an amber-bordered `surfaceContainer` card with amber
// text — legible, and in its own dialect: nowhere else in the app does a card
// state its severity with a 3 dp ring. Material's own answer to "a surface
// that carries a meaning" is a **container** pair (a tinted ground plus the
// on-colour that is legible against it), and the app had semantic FOREGROUNDS
// only, which is exactly why the popup had to invent something.
//
// These are **derived**, not new hex. Each container is its own semantic
// colour at low alpha composited over the dark ground, so a token change to
// `SemWarn` moves its container with it and the two can never drift apart —
// the same single-token discipline round 22 applied to the Agtom orange. The
// `on` colours stay the pure semantic hue, which is what keeps the contrast:
// against a 14 %-amber ground, full-strength amber reads far better than the
// near-black `onSurface` a Material container would pair.
private fun Color.over(ground: Color, alpha: Float): Color = Color(
    red = red * alpha + ground.red * (1f - alpha),
    green = green * alpha + ground.green * (1f - alpha),
    blue = blue * alpha + ground.blue * (1f - alpha),
)

/** Warning ground: amber at 14 % over the panel. Pairs with [OnSemWarnContainer]. */
val SemWarnContainer = SemWarn.over(Panel, 0.14f)

/** The legible foreground on [SemWarnContainer]. */
val OnSemWarnContainer = SemWarn

/** Success ground: green at 14 % over the panel. Pairs with [OnSemGoodContainer]. */
val SemGoodContainer = SemGood.over(Panel, 0.14f)

/** The legible foreground on [SemGoodContainer]. */
val OnSemGoodContainer = SemGood

/**
 * Sensor badge colours. Fixed regardless of theme so the D6-vs-Mid-360 badge
 * stays recognisable at a glance across projects — the same reasoning this
 * file carried before the redesign, now expressed in the redesign's own ramp
 * colours (the mockup's `.chip.sensor-d6` / `.chip.sensor-mid`).
 */
val SensorD6Badge = ScanTeal
val SensorMid360Badge = PoseBlue

/**
 * ROUND 25 item 119 — the STL-27L's badge.
 *
 * A THIRD colour rather than a reuse of [SensorD6Badge], even though the
 * STL-27L behaves like the D6 nearly everywhere else in this app. The badge's
 * entire job is telling an operator scrolling the Projects list which box was
 * on the bracket for a given scan; two 2-D serial lidars sharing one teal pill
 * would make the badge stop answering the only question it is asked. Sand is
 * the ramp's remaining unclaimed colour and is distinguishable from both teal
 * and blue for the common colour-vision deficiencies, where a second green or
 * a second blue would not be.
 */
val SensorStl27lBadge = ScanSand

/**
 * ROUND 25 item 119 — **the one place a `SensorType` becomes a badge colour.**
 *
 * Four draw sites (the Projects card, the card thumbnail, the picker, the
 * detail screen) each spelled this as `if (sensor == MID360) PoseBlue else
 * ScanTeal`. That `else` is what painted a brand new STL-27L in the COIN-D6's
 * teal the moment the enum grew — the label said "STL-27L" and the colour said
 * D6, and on a card read at arm's length the colour is what is read first.
 *
 * The exhaustive decision lives in `:core` (`SensorType.badgeTint`), so a
 * fourth sensor breaks the build there; this is only the palette lookup, which
 * is the half that genuinely belongs to the theme.
 */
fun sensorBadgeColor(sensor: com.lidarscan.core.model.SensorType): Color =
    when (sensor.badgeTint) {
        com.lidarscan.core.model.SensorType.BadgeTint.D6 -> SensorD6Badge
        com.lidarscan.core.model.SensorType.BadgeTint.MID360 -> SensorMid360Badge
        com.lidarscan.core.model.SensorType.BadgeTint.STL27L -> SensorStl27lBadge
    }

// ── light-theme companions ──────────────────────────────────────────────────
//
// The mockup is deliberately single-theme ("dark-cockpit instrument UIs, shown
// in their own world"). The Settings screen's Light/Dark/System control
// predates the redesign and stays functional, so the tokens get a light
// counterpart rather than the switch quietly doing nothing: same ember, same
// semantics, inverted ground.
val GroundLight = Color(0xFFF4F6F8)
val PanelLight = Color(0xFFFFFFFF)
val PanelAltLight = Color(0xFFE8ECF0)
val LineLight = Color(0xFFD3DAE1)
val InkLight = Color(0xFF12161B)
val InkMuteLight = Color(0xFF5C6873)
