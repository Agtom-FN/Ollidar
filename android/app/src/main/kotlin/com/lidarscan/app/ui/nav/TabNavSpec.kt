package com.lidarscan.app.ui.nav

/**
 * ROUND 22 item 88 — **the one-line navigation defect behind four symptoms.**
 *
 * `LidarScanApp.goTab()` used to navigate like this:
 *
 * ```
 * popUpTo(Routes.PROJECTS) { inclusive = route == Routes.PROJECTS }
 * launchSingleTop = true
 * ```
 *
 * No `saveState`. No `restoreState`. In Navigation Compose that is not a
 * cosmetic omission: popping a destination off the back stack **destroys its
 * `NavBackStackEntry`, and with it that entry's `ViewModelStore`**. Since every
 * seal ends with `goTab(Routes.PROJECTS)` (ROUND 8 item 31, deliberately — a
 * finished capture lands where the "is it saved?" question is answered), every
 * single Stop destroyed the `CAPTURE_NEW` entry, and every tap back into the
 * Scan tab constructed a brand-new [com.lidarscan.app.ui.capture.CaptureViewModel].
 *
 * The owner's 2026-08-20 log shows **four `CaptureViewModel` inits in 37
 * seconds** — the "mount trim restored" line each `init` prints is the
 * observable, and four of them in half a minute is not four captures.
 *
 * ### The four symptoms that were all this
 *
 *  1. **Auto-process reported a failure that never happened** (item 90). It ran
 *     in `viewModelScope`; the navigation that started it also cancelled it.
 *  2. **Doubled operator cues.** Two live ViewModels, two sets of listeners on
 *     the shared controller, every cue played twice — and ROUND 13 measured
 *     that a cue buzz can itself cause a section break, so this was not merely
 *     annoying.
 *  3. **"Tracking lost until I restart the app"** (item 89). Each rebuild
 *     mounts a fresh `ArPosePumpView` while the outgoing one is still composed
 *     through the transition, which is exactly the claim/release race that
 *     item's token fixes. Without this churn the race almost never armed.
 *  4. **scan-068: 194,067 points decoded, 0 recorded** (item 89 again). The
 *     outgoing ViewModel's `onCleared` zeroed the shared controller's engine
 *     handle after the incoming capture had armed it.
 *
 * ### The fix, and why it is a data class
 *
 * `popUpTo(startDestination) { saveState = true }` + `launchSingleTop = true` +
 * `restoreState = true` is the standard bottom-tab contract: each tab's stack
 * (and each entry's `ViewModelStore`) is **saved** on the way out and
 * **restored** on the way back, so the Scan tab's ViewModel survives a trip to
 * Projects and back instead of being rebuilt.
 *
 * `NavOptionsBuilder` is an Android type and cannot be constructed on a bare
 * JVM, so the *decision* is separated from the *call*: this pure data class is
 * what a unit test can pin, and `goTab` does nothing but copy it into the
 * builder. The alternative — asserting on a real `NavController` — needs an
 * emulator, and this fix must be regression-tested by the suite that runs on
 * every build. The emulator test that counts actual ViewModel constructions
 * across tab switches (`TabSwitchKeepsCaptureViewModelTest`) is the other half,
 * and it is the half that proves the property end to end.
 *
 * `inclusive` is now **always false**, including for the Projects tab itself.
 * Popping the start destination inclusively and re-adding it is what made
 * Projects a fresh entry every time too; with `launchSingleTop` the
 * non-inclusive pop lands on the existing start entry, which is what a tab bar
 * means by "go to Projects".
 */
internal data class TabNavSpec(
    val popUpToRoute: String,
    val popUpToInclusive: Boolean,
    val saveState: Boolean,
    val launchSingleTop: Boolean,
    val restoreState: Boolean,
)

/**
 * The navigation options every tab tap uses. [startRoute] is the graph's start
 * destination ([Routes.PROJECTS]); [target] is the tab being opened.
 *
 * Deliberately independent of [target] except in name: a tab bar has one
 * behaviour, and the pre-round-22 special case for the Projects tab
 * (`inclusive = route == Routes.PROJECTS`) is exactly the special case that was
 * wrong.
 */
internal fun tabNavSpec(startRoute: String, target: String): TabNavSpec {
    require(target.isNotBlank()) { "a tab must have a route" }
    return TabNavSpec(
        popUpToRoute = startRoute,
        popUpToInclusive = false,
        saveState = true,
        launchSingleTop = true,
        // ── ROUND 22 item 88, second cut: NOT for the start destination ─────
        //
        // `restoreState` restores the back stack saved for the destination
        // being navigated TO. For every tab except the start destination that
        // is exactly what is wanted — it is how the Scan tab's saved entry
        // (and its ViewModelStore) comes back instead of being rebuilt.
        //
        // Navigating to the START destination is the degenerate case, and it
        // is degenerate because this graph's tabs are FLAT destinations rather
        // than nested graphs: the `popUpTo(startRoute) { saveState = true }`
        // in the same call has just saved whatever was above it, and asking to
        // restore in the same breath puts it straight back. The emulator
        // caught it exactly: from the replay-capture destination, tapping
        // Projects popped the capture entry, immediately restored it, and the
        // screen never changed — `recordButton` was still on screen after the
        // tap, and both `CaptureFlowNoStrayTest` cases timed out waiting for
        // the Projects hero.
        //
        // So the start destination pops and lands, and does not restore. The
        // item-88 property is untouched: the Scan entry is still SAVED on the
        // way out (which is what keeps its ViewModel alive) and still RESTORED
        // when the Scan tab is tapped, which is the trip that was rebuilding
        // it four times in 37 seconds.
        restoreState = target != startRoute,
    )
}
