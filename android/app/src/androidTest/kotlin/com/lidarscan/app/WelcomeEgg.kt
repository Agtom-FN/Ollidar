package com.lidarscan.app

import android.os.ParcelFileDescriptor
import android.view.accessibility.AccessibilityNodeInfo
import androidx.test.platform.app.InstrumentationRegistry

/**
 * ROUND 34 items 181 + 183(a) — **the easter egg, seen from outside Compose.**
 *
 * Everything in here deliberately avoids the Compose test API, and the reason is
 * the trap this suite fell into on its first run: `onNodeWithTag`, `waitUntil`
 * and `performClick` all **synchronise** — they wait for the composition to go
 * idle, and a three-second animation is not idle. So the first version of the
 * egg test asked "is the overlay up?" and Compose answered by waiting until the
 * film had finished, at which point the honest answer was no. The film cannot
 * be observed by the machinery that waits for it to be over.
 *
 * Round 32's own suite dodged this with `mainClock.autoAdvance = false`, which
 * works when the test owns the composition. This one does not: the film is
 * played by the real `MainActivity` in response to a real settings write, which
 * is the whole thing being tested.
 *
 * So the film is observed through the **accessibility tree** — where Compose
 * publishes the overlay's `contentDescription` — and touched through **`input
 * tap`**, both of which are outside Compose's clock. No new dependency: this is
 * `UiAutomation`, which the runner already provides.
 */

/** What `WelcomeOverlay` calls itself, per variant. Semantics, not a test tag. */
const val EGG_DESCRIPTION = "Welcome animation, developer"
const val LAUNCH_FILM_DESCRIPTION = "Welcome animation"

private fun automation() = InstrumentationRegistry.getInstrumentation().uiAutomation

private fun shell(command: String): String {
    val fd = automation().executeShellCommand(command)
    return ParcelFileDescriptor.AutoCloseInputStream(fd).use {
        it.readBytes().toString(Charsets.UTF_8).trim()
    }
}

/** A shell tap, which the overlay receives exactly as a finger's. */
fun tapScreen(x: Int, y: Int) {
    shell("input tap $x $y")
}

/**
 * The three global animation scales — **which this suite runs with set to
 * zero**, and that is not incidental.
 *
 * `android.testOptions.animationsDisabled = true` in `app/build.gradle.kts`
 * writes zero into all three for the duration of an instrumentation run, so
 * that Espresso is not racing view animations. Zero into those scales is
 * *precisely* what Android's accessibility **Remove animations** switch does,
 * and it is what `WelcomeReducedMotion` reads — so during a connected run the
 * app is, correctly, a phone with animations turned off, and **no welcome film
 * plays at all**.
 *
 * The first version of round 34's egg test spent two runs failing on that, and
 * the finding is worth more than the time it cost: the reduced-motion gate is
 * proved, on a device, by the test harness's own configuration. A test that
 * wants to see the film therefore has to hand the animations back first, and
 * put them where it found them afterwards.
 */
private val ANIMATION_SCALES = listOf(
    "animator_duration_scale",
    "transition_animation_scale",
    "window_animation_scale",
)

/** What the three scales say right now, for a failure message. */
fun animationScaleReport(): String =
    ANIMATION_SCALES.joinToString(" ") { it.take(3) + "=" + shell("settings get global $it") }

/** True when the platform says animations are off — the app's own reading. */
fun animationsAreDisabled(): Boolean =
    shell("settings get global animator_duration_scale").let { it == "0" || it == "0.0" }

/** Runs [body] on a device whose animations are on, and restores them after. */
fun <T> withAnimationsOn(body: () -> T): T {
    val saved = ANIMATION_SCALES.map { shell("settings get global $it") }
    ANIMATION_SCALES.forEach { shell("settings put global $it 1") }
    try {
        return body()
    } finally {
        ANIMATION_SCALES.zip(saved).forEach { (key, value) ->
            shell("settings put global $key ${value.ifBlank { "1" }}")
        }
    }
}

private fun walk(node: AccessibilityNodeInfo?, found: (AccessibilityNodeInfo) -> Boolean): Boolean {
    if (node == null) return false
    if (found(node)) return true
    for (i in 0 until node.childCount) {
        if (walk(node.getChild(i), found)) return true
    }
    return false
}

/** True when a node describing itself as [description] is on screen right now. */
fun screenDescribes(description: String): Boolean =
    walk(automation().rootInActiveWindow) { it.contentDescription?.toString() == description }

/** Polls [screenDescribes] until it holds, or the time runs out. */
fun awaitDescribed(description: String, timeoutMillis: Long = 8_000): Boolean {
    val end = System.currentTimeMillis() + timeoutMillis
    while (System.currentTimeMillis() < end) {
        if (screenDescribes(description)) return true
        Thread.sleep(80)
    }
    return false
}

/**
 * The on-screen rectangle of the first node whose text contains [substring],
 * or null. The Settings rows merge their semantics, so the version footer
 * arrives here as one node reading `Version 0.9.19 (919)`.
 */
fun nodeBoundsContaining(substring: String): android.graphics.Rect? {
    var hit: android.graphics.Rect? = null
    walk(automation().rootInActiveWindow) { node ->
        val text = (node.text?.toString() ?: "") + " " + (node.contentDescription?.toString() ?: "")
        if (text.contains(substring)) {
            hit = android.graphics.Rect().also { node.getBoundsInScreen(it) }
            true
        } else {
            false
        }
    }
    return hit
}

/** True when some node on screen carries [substring] in its text or description. */
fun screenContains(substring: String): Boolean = nodeBoundsContaining(substring) != null

/** Polls [screenContains]. */
fun awaitText(substring: String, timeoutMillis: Long = 20_000): Boolean {
    val end = System.currentTimeMillis() + timeoutMillis
    while (System.currentTimeMillis() < end) {
        if (screenContains(substring)) return true
        Thread.sleep(120)
    }
    return false
}

/** …and the other way round, for text. */
fun awaitTextGone(substring: String, timeoutMillis: Long = 20_000): Boolean {
    val end = System.currentTimeMillis() + timeoutMillis
    while (System.currentTimeMillis() < end) {
        if (!screenContains(substring)) return true
        Thread.sleep(120)
    }
    return false
}

/** A swipe, for scrolling a page nothing else is driving. */
fun swipe(x1: Int, y1: Int, x2: Int, y2: Int, ms: Int = 200) {
    shell("input swipe $x1 $y1 $x2 $y2 $ms")
}

/** Every content description on screen — for saying what WAS there when a wait fails. */
fun screenDescriptions(): List<String> {
    val out = mutableListOf<String>()
    walk(automation().rootInActiveWindow) { node ->
        node.contentDescription?.toString()?.takeIf { it.isNotBlank() }?.let { out += it }
        node.text?.toString()?.takeIf { it.isNotBlank() }?.let { out += "text:" + it }
        false
    }
    return out
}

/** …and the other way round. */
fun awaitGone(description: String, timeoutMillis: Long = 5_000): Boolean {
    val end = System.currentTimeMillis() + timeoutMillis
    while (System.currentTimeMillis() < end) {
        if (!screenDescribes(description)) return true
        Thread.sleep(80)
    }
    return false
}

/**
 * Skip the egg if it is playing, exactly as a person does.
 *
 * Three suites unlock developer mode as part of their own preconditions and
 * then keep tapping. The film swallows the next touch — round 32's argument for
 * consuming the event is that a lid which leaks is worse than no lid — so
 * without this the first tap after an unlock goes nowhere and a seven-tap count
 * comes up one short.
 *
 * Tolerant of the film not arriving: reduced motion is a device setting, and a
 * suite that failed on a correctly-configured device would be asserting the
 * wrong thing.
 */
fun dismissWelcomeEgg(centreX: Int = 540, centreY: Int = 1200) {
    // Nothing to skip on a device that plays nothing, and every connected run
    // is such a device unless a test has explicitly handed the animations back.
    if (animationsAreDisabled()) return
    if (!awaitDescribed(EGG_DESCRIPTION, timeoutMillis = 4_000)) return
    tapScreen(centreX, centreY)
    awaitGone(EGG_DESCRIPTION)
}
