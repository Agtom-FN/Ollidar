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
/**
 * Blue-biased near-black — the app's ground plane.
 *
 * ROUND 28 item 146 — **the ground drops so the card can be seen.** It was
 * `#12161B` against a `#1A2027` panel: **1.11:1**, which is not a step, it is a
 * rounding error. A card that cannot be perceived as a card is why every panel
 * in this app was given a border AND a drop shadow, and that pair is what the
 * owner reads as "floating card soup". Moving the ground down two points and
 * the panel up one takes card-vs-page to 1.17:1 — still quiet, but a real rung
 * — which is what lets item 147 delete the shadows rather than merely ban them.
 */
val Ground = Color(0xFF0E1216)

/** Raised panel (cards, tab bar, stat strip). See [Ground] for the change. */
val Panel = Color(0xFF1B222A)

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
/**
 * ROUND 28 item 145 — **the derivation was right; its ground was a constant.**
 *
 * Round 25 wrote `SemWarn.over(Panel, 0.14f)` and `Panel` is `#1B222A`, a dark
 * value, in every theme. The light theme therefore drew warning cards as **dark
 * olive boxes on a white page**, and success cards as near-black green ones.
 *
 * The function survives — deriving a container from its semantic is what keeps
 * the pair from drifting when a token changes, which was the whole point — but
 * it is now `internal` and takes the ground from the CALLER, and the only
 * callers are [ScanColorScheme]'s container properties, which are resolved
 * against the current theme's card colour at composition time. There is no
 * longer a way to spell "composite over the dark panel" by accident.
 */
internal fun Color.over(ground: Color, alpha: Float): Color = Color(
    red = red * alpha + ground.red * (1f - alpha),
    green = green * alpha + ground.green * (1f - alpha),
    blue = blue * alpha + ground.blue * (1f - alpha),
)

/**
 * ROUND 28 item 144 — **the sensor identities moved into the scheme.**
 *
 * The block that stood here explained why the D6, the Mid-360 and the STL-27L
 * are "fixed regardless of theme so the badge stays recognisable at a glance
 * across projects", and round 25 item 119's reasoning for a third colour — sand
 * rather than a second teal or a second blue, because two 2-D serial lidars
 * sharing one pill make the badge stop answering the only question it is asked
 * — is still exactly right and is preserved in [ScanColorScheme.sensor].
 *
 * What was wrong was the word *fixed*. Fixing the hex is not what makes a badge
 * recognisable; fixing the **hue** is. `#3EC4B0` on white is 2.16:1, so on the
 * owner's phone the recognisable teal was a pale smudge. The light column keeps
 * the hue and moves the lightness, so a D6 badge is the same teal in both
 * themes and readable in both.
 *
 * @see ScanColorScheme.sensor
 * @see sensorBadgeColor
 */
// ── light-theme companions (ROUND 28 items 144/146) ────────────────────────
//
// **The light theme was an unfinished port, and it was the single largest
// defect in the app.** This file used to define thirty dark tokens and six
// light ones, and the comment above `sensorBadgeColor` explained the missing
// twenty-four as a principle: the semantics and the sensor identities are
// "deliberately theme-invariant so a Float badge looks the same on every
// screen and in every theme".
//
// The instinct was right and the execution inverted it. A badge that looks the
// same in both themes is the goal; a badge painted with the same *hex* in both
// themes is how you lose it, because #E5B93C measures 8.86:1 on the dark panel
// and **1.71:1** on white. On the owner's phone — which runs light — the amber
// that means "degraded", the green that means "fixed", both sensor identities
// and the ember used as text were all somewhere between invisible and merely
// illegible. `CoverageAmber` at 1.68:1 was painting *"No scanner found. Plug it
// in, then Retry."*
//
// So each semantic gets a light column in which **hue and saturation are
// preserved and only lightness moves**, tuned to land at 4.5:1 on the light
// page. A FAIR badge is recognisably the same amber in both themes and legible
// in both, which is what "theme-invariant" was always trying to buy.
//
// Ratios in the comments are against the theme's own page colour.

/** §C.3 `page` — the light ground. Was `#F4F6F8`; see [Ground] for why it moved. */
val GroundLight = Color(0xFFECEFF3)

/** §C.3 `card` — white, unchanged. It is the ground that moved, not the card. */
val PanelLight = Color(0xFFFFFFFF)

/** §C.3 `trough` — segmented-control tracks and tiles inside a card. */
val PanelAltLight = Color(0xFFE2E7EC)

/** §C.3 `line` — the 1 dp hairline. 1.41:1 → **1.58:1** on white, which is what
 * actually makes a card read as a card once the shadows are gone. */
val LineLight = Color(0xFFC6CFD8)

/**
 * §C.3 `viewport`, light.
 *
 * ROUND 28 item S2: `ViewportGround`'s near-black had no light counterpart, so
 * the 3D view was a `#0B0E12` slab with square corners inside a white page —
 * the review's words are "it reads as a rendering failure". A point cloud still
 * needs a dark ground to carry (that part of the dark-only design was correct),
 * so the light viewport is the dark theme's CARD colour rather than the light
 * theme's: dark enough for the cloud, close enough to the page's family that it
 * reads as a panel and not as a hole.
 */
val ViewportGroundLight = Color(0xFF1B222A)

val InkLight = Color(0xFF12161B)

/** 5.3:1 on [GroundLight] — a real body-text token, not a dark-theme leftover. */
val InkMuteLight = Color(0xFF5C6873)

/**
 * 3.5:1 — **UI only, never text.** Disabled labels, inert chevrons, tick marks.
 * Named `faint` rather than given to text so the one rule that keeps this
 * theme legible ("every string is ≥4.5:1") has no exception to argue about.
 */
val InkFaintLight = Color(0xFF78838E)

/**
 * ROUND 28 item 144 — **orange that may be text.**
 *
 * The brand fill does not move: `#F26A1B` stays exactly as round 22 item 93 set
 * it, on filled buttons, the active tab, the record FAB and progress. But the
 * same value used as *ink* measures **3.06:1** on the light page, and the app
 * uses it as ink in a dozen places (links, section labels, active-chip text).
 * Splitting the role rather than darkening the brand is what lets both be
 * right: `primary` is the fill, `primaryInk` is the text and icon colour, and
 * in the dark theme they are the same token because there they already pass.
 */
val EmberInkLight = Color(0xFFBF4D0B)

// ── the light semantic column (§C.3) ───────────────────────────────────────

/** 4.51:1 — RTK fixed / healthy / done. */
val SemGoodLight = Color(0xFF218147)

/** 4.54:1 — float / degraded / warning. */
val SemWarnLight = Color(0xFF8C6C13)

/** 4.51:1 — the coverage amber. See [CoverageAmber]; this is its light half. */
val CoverageAmberLight = Color(0xFFA16300)

/** 4.51:1 — single / fault / failed. */
val SemBadLight = Color(0xFFD92929)

/** 4.50:1 — the COIN-D6 identity, and the height ramp's low end. */
val ScanTealLight = Color(0xFF267E71)

/** 4.55:1 — the Mid-360 identity, and the trajectory colour. */
val PoseBlueLight = Color(0xFF1F71C9)

/** 4.51:1 — the STL-27L identity, and the height ramp's high end. */
val ScanSandLight = Color(0xFF8B6D18)
