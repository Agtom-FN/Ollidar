package com.lidarscan.app.ui.welcome

import android.content.Context
import android.provider.Settings
import java.util.concurrent.atomic.AtomicBoolean

/**
 * ROUND 32 item 177 — **"cold LAUNCH only (process start)".**
 *
 * The gate is process-wide and one-shot, and it is deliberately *not* held in
 * composable state, a `rememberSaveable`, or the Activity. Each of those
 * answers a different question from the one the owner asked:
 *
 *  * composable state replays the film on every tab switch;
 *  * `rememberSaveable` replays it after process death but also survives a
 *    rotation, so it says nothing useful either way;
 *  * the Activity is recreated by a rotation, a theme change and a locale
 *    change, none of which is a launch.
 *
 * A process is created once and is destroyed once, which is exactly the event
 * "cold launch" names. A rotation mid-film therefore ends the film — the
 * animation is not worth restarting and definitely not worth resuming, and
 * "not rotations" is what the item says.
 */
object WelcomeLaunchGate {

    private val claimed = AtomicBoolean(false)

    /**
     * True exactly once per process, for the first caller. Everything after
     * that — including a second Activity — gets false.
     *
     * Atomic rather than a plain `Boolean` because two Activities can be
     * created concurrently (a launcher tap racing a USB-attach intent is the
     * real case in this app) and a torn read would show the film twice.
     */
    fun claimFirstLaunch(): Boolean = claimed.compareAndSet(false, true)

    /** Whether the one claim has been taken, without taking it. */
    val claimedInThisProcess: Boolean get() = claimed.get()

    /** Instrumentation only: a test process outlives the launch it wants to test. */
    fun resetForTest() {
        claimed.set(false)
    }
}

/**
 * ROUND 32 item 177 — the `prefers-reduced-motion` equivalent, on Android.
 *
 * Android has no single "reduce motion" flag to read. What the accessibility
 * **Remove animations** switch actually does is write zero into the three
 * global animation scales, and those are the same knobs the developer-options
 * sliders write — which is correct for us either way: a person who has turned
 * animations off, by whichever road, has said what they want.
 *
 * Read at launch and not observed: this decides one thing, once, and a
 * preference that changes while the film is playing is not a case worth having
 * code for.
 */
object WelcomeReducedMotion {

    /**
     * True when the platform says animations are off.
     *
     * Both scales are consulted because they are set independently — a device
     * can have `animator_duration_scale` at zero from developer options while
     * transitions still run, and the film is an animator, not a transition, so
     * either zero is a "no". A missing or unreadable setting reads as 1 (on),
     * which is the safe direction: the film plays and can be tapped away, and
     * it can be switched off in Settings.
     */
    fun isOn(context: Context): Boolean {
        val resolver = context.contentResolver
        fun scale(key: String): Float =
            runCatching { Settings.Global.getFloat(resolver, key, 1f) }.getOrDefault(1f)
        return scale(Settings.Global.ANIMATOR_DURATION_SCALE) == 0f ||
            scale(Settings.Global.TRANSITION_ANIMATION_SCALE) == 0f
    }
}
