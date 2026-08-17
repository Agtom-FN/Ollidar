package com.lidarscan.app.ui.capture

/**
 * ROUND 8, owner item 28 — **the live scan view gets the screen.**
 *
 * > "the capture layout … the live scan view must keep the majority of the
 * > screen"
 *
 * ## What it was
 *
 * On the Capture tab, pre-capture and connected, everything below stacked
 * *above* or *below* the viewport in one `Column`, and the viewport was
 * whatever was left (`weight(1f)`, floor 140 dp):
 *
 * | band | measured height |
 * | --- | --- |
 * | app bar (`BackBar`) | 56 dp |
 * | RTK / georeference chip strip | ~32 dp |
 * | `PERFORMANCE` label + Light/Optimal/Full chips | ~70 dp |
 * | pre-capture strip (name field, auto-detect line, D6 mount paragraph, Set-mount button, its explanation, tracking chip) | capped at **46 % of the screen** |
 * | four-cell `StatPanel` + its spacers | ~80 dp |
 * | transport row | ~80 dp |
 * | tab-bar clearance | 86 dp |
 *
 * On an 800 dp portrait phone that is ~370 dp of fixed chrome plus a strip
 * allowed to take another 368 dp — leaving the *live 3D view*, which is the
 * only thing on this screen that can tell an operator whether the scan is any
 * good, with as little as 60 dp. The owner's words for the result were that the
 * capture screen is settings with a sliver of scan in it.
 *
 * ## The rule
 *
 * [MIN_VIEWPORT_FRACTION] of the screen, in the **normal** state: a sensor
 * connected, no manual-entry fallback open, no loud banner up. Everything else
 * either collapses into one always-visible chrome row or moves behind a sheet
 * chip, following the Display/Diagnostics pattern the screen already had.
 *
 * The guarantee is **arithmetic, not aspiration**: the viewport is the only
 * weighted child of the capture column, so it receives exactly
 * `screenHeight − fixedChromeDp()`. Keeping that above 60 % is therefore a
 * property of [fixedChromeDp] and nothing else, which is why this object is
 * plain Kotlin with no Compose in it — the guarantee is checkable on a bare JVM
 * (`CaptureLayoutTest`) rather than only on a booted emulator.
 *
 * ## Why these bands and not others
 *
 * Four things had to go to make the number work, and each is a judgement:
 *
 *  * **The app bar, on the tab only.** The Capture tab is a *tab*: its back
 *    arrow went to Projects, which the floating tab bar already does. A
 *    project-scoped or replay entry keeps its `BackBar`, because there it is a
 *    real parent. Worth 56 dp.
 *  * **The four-cell `StatPanel`.** Same four numbers, one mono line, inside
 *    the transport row's left column where the "display only · recording
 *    unaffected" caption used to be. Worth ~80 dp and reads better mid-walk.
 *  * **The preset chips.** Behind a chip that names the current preset, so the
 *    row still *reports* Light/Optimal/Full without spending 70 dp on three
 *    buttons that are pressed once a session.
 *  * **The pre-capture strip.** Behind the Scan chip — with the exception of
 *    the mount state, which ROUND 8 item 30c requires be visible without
 *    opening anything, and which is why [MOUNT_ROW_DP] exists.
 */
object CaptureLayout {

    /**
     * The share of the screen the live viewport keeps in the normal state.
     *
     * 0.60 is the owner's number. It is a floor, not a target: on a 800 dp
     * phone the arithmetic below actually gives the viewport ~0.68.
     */
    const val MIN_VIEWPORT_FRACTION = 0.60f

    /**
     * ROUND 8 item 30c: the always-visible mount-state row —
     * `MOUNT SET · 132.8° · 2 min ago` plus its Set / Re-zero pill.
     *
     * Only present for a phone-tracked D6 (the sensor whose extrinsic the trim
     * is *about*); a Mid-360 session does not pay for it.
     */
    const val MOUNT_ROW_DP = 46f

    /** The one chip row: scan name, preset, display, diagnostics. */
    const val CHIP_ROW_DP = 46f

    /** Live-view switch + the four capture numbers + pause + record. */
    const val TRANSPORT_ROW_DP = 80f

    /** `ScanDims.TabBarClearance` — the room the floating capsule tab bar needs. */
    const val TAB_BAR_CLEARANCE_DP = 86f

    /**
     * The ceiling on the quiet-hint band, in dp.
     *
     * Six advisories can be live at once — georeference fallback, refresh
     * downshift, moving-too-fast, AR degraded, section break, live page store
     * full — and each is two or three lines. Unbounded, that is ~180 dp of a
     * capture screen spent on notes *about* a scan the operator can no longer
     * see, which is the failure mode item 28 is about, one level down. Capped
     * and scrollable: the first is always readable and the rest are a flick
     * away.
     *
     * Deliberately **not** part of [fixedChromeDp]. A hint band is a transient
     * state, not the normal one, and even fully occupied it leaves the viewport
     * above 50 % on every screen this runs on. Counting it into the budget
     * would mean permanently reserving 44 dp for lines that are usually absent.
     */
    const val HINT_BAND_MAX_DP = 44f

    /**
     * The shortest portrait screen on which [MIN_VIEWPORT_FRACTION] is
     * achievable at all, in dp.
     *
     * Below this the fixed chrome — a transport row you must be able to hit
     * one-handed while walking, and a floating tab bar that is not this
     * screen's to shrink — is more than 40 % of the display on its own, and the
     * viewport gets everything that is left rather than a guaranteed 60 %
     * (≈57 % at 640 dp). Stated rather than hidden: 660 dp is a small phone,
     * and every device this app has been run on is 730–900 dp.
     */
    const val REFERENCE_SCREEN_HEIGHT_DP = 660f

    /**
     * Everything above and below the viewport in the normal state, in dp.
     *
     * @param mountRow true for a phone-tracked COIN-D6 — see [MOUNT_ROW_DP].
     * @param appBar true for the project-scoped / replay entries, which keep
     *   their `BackBar` because they have a real parent to go back to.
     */
    fun fixedChromeDp(mountRow: Boolean = true, appBar: Boolean = false): Float =
        (if (appBar) APP_BAR_DP else 0f) +
            (if (mountRow) MOUNT_ROW_DP else 0f) +
            CHIP_ROW_DP +
            TRANSPORT_ROW_DP +
            TAB_BAR_CLEARANCE_DP

    /** `BackBar`, for the entries that still carry one. */
    const val APP_BAR_DP = 56f

    /**
     * The height the viewport is guaranteed in the normal state, in dp.
     *
     * `min` of the two, not just the fraction: over-constraining a `Column`
     * child is how the record button ends up measured to zero height on a small
     * screen, and a capture screen you cannot stop is worse than one whose
     * viewport is 57 %.
     */
    fun viewportMinHeightDp(
        screenHeightDp: Float,
        mountRow: Boolean = true,
        appBar: Boolean = false,
    ): Float = minOf(
        screenHeightDp * MIN_VIEWPORT_FRACTION,
        screenHeightDp - fixedChromeDp(mountRow, appBar),
    ).coerceAtLeast(VIEWPORT_FLOOR_DP)

    /** The share of the screen the viewport actually gets — what the test asserts. */
    fun viewportFraction(
        screenHeightDp: Float,
        mountRow: Boolean = true,
        appBar: Boolean = false,
    ): Float = viewportMinHeightDp(screenHeightDp, mountRow, appBar) / screenHeightDp

    /**
     * The absolute floor, inherited from ROUND 5. A viewport this small is not
     * useful, but it is still the proof that a sensor is streaming, so it never
     * goes to zero.
     */
    const val VIEWPORT_FLOOR_DP = 140f

    /**
     * True when the screen may drop to the compact chrome this object budgets
     * for.
     *
     * Deliberately **not** simply "always". With nothing connected, the Capture
     * tab's job is the connect flow — the auto-detect status line, and the
     * inline manual fallback that opens itself when detection fails (ROUND 5's
     * owner addition 1). That flow needs the room, there is no live view to
     * protect (the viewport says "Connect a sensor…"), and it is what
     * `ReplayCaptureSmokeTest.captureTabIsANewScanWithAutoDetectAndAnInlineManualFallback`
     * walks on a bare emulator. So the compact layout is what a *connected*
     * capture screen looks like, which is exactly the state item 28 is about.
     */
    fun useCompactChrome(connected: Boolean, manualEntryOpen: Boolean): Boolean =
        connected && !manualEntryOpen
}
