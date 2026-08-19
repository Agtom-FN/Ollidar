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

/**
 * Sensor badge colours. Fixed regardless of theme so the D6-vs-Mid-360 badge
 * stays recognisable at a glance across projects — the same reasoning this
 * file carried before the redesign, now expressed in the redesign's own ramp
 * colours (the mockup's `.chip.sensor-d6` / `.chip.sensor-mid`).
 */
val SensorD6Badge = ScanTeal
val SensorMid360Badge = PoseBlue

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
