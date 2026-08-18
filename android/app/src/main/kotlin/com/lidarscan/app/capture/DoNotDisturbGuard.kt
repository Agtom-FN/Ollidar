package com.lidarscan.app.capture

import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.provider.Settings
import com.lidarscan.core.capture.CaptureFocus
import com.lidarscan.core.capture.DndState

/**
 * ROUND 13 (owner item 47). The Android half of "Do Not Disturb during
 * capture" — every decision lives in [CaptureFocus] in `:core` and is unit
 * tested there; this file only talks to the framework.
 *
 * ## The two rules this class exists to keep
 *
 * 1. **A capture never blocks on it.** Policy access is a Settings screen the
 *    user has to visit, and refusing to Start because they have not is
 *    ROUND 10 item 38 arriving by a third road. [engage] always returns, and
 *    always returns a state the caller can log.
 * 2. **The filter is always put back.** [release] is idempotent and safe to
 *    call from any exit path — Stop, a failed seal, `onCleared`, or the next
 *    Start finding a filter it left behind after a process death. Because a
 *    crash cannot run code, the *last* line of defence is that [engage] on the
 *    next capture reads a filter it did not set, sees it is not
 *    `INTERRUPTION_FILTER_ALL`, and declines to engage — so the worst outcome
 *    of a crash mid-capture is a phone left quiet, which is visible in the
 *    status bar and one tap to undo, never a phone left noisy while scanning.
 *    [restoreOrphaned] makes that recovery explicit rather than incidental.
 *
 * The compile-time constant check below is the whole reason `:core` may hold
 * plain Ints for framework values.
 */
class DoNotDisturbGuard(private val appContext: Context) {

    init {
        check(CaptureFocus.DESIRED_FILTER == NotificationManager.INTERRUPTION_FILTER_PRIORITY) {
            "CaptureFocus.DESIRED_FILTER drifted from the framework constant"
        }
        check(CaptureFocus.FILTER_ALL == NotificationManager.INTERRUPTION_FILTER_ALL)
    }

    private val nm: NotificationManager? =
        appContext.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager

    /** What the phone's filter was when this capture engaged; null when it did not. */
    @Volatile
    var previousFilter: Int? = null
        private set

    @Volatile
    var state: DndState = DndState.DISABLED
        private set

    val hasPolicyAccess: Boolean
        get() = nm?.isNotificationPolicyAccessGranted == true

    /**
     * The Settings screen where policy access is granted. The caller shows this
     * once, from a control the operator chose to press — never automatically
     * mid-capture, which would throw the user out of the app they are scanning
     * with.
     */
    fun policyAccessIntent(): Intent =
        Intent(Settings.ACTION_NOTIFICATION_POLICY_ACCESS_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    /**
     * Silence notifications for the duration of a capture.
     *
     * @param enabled the operator's Settings switch. False short-circuits to
     *   [DndState.DISABLED] without touching anything.
     */
    fun engage(enabled: Boolean): DndState {
        previousFilter = null
        val manager = nm
        if (!enabled) {
            state = DndState.DISABLED
            return state
        }
        if (manager == null || !manager.isNotificationPolicyAccessGranted) {
            state = DndState.NO_PERMISSION
            return state
        }
        state = try {
            val current = manager.currentInterruptionFilter
            if (!CaptureFocus.shouldEngage(current)) {
                DndState.ALREADY_QUIET
            } else {
                manager.setInterruptionFilter(CaptureFocus.DESIRED_FILTER)
                previousFilter = current
                DndState.PROTECTED
            }
        } catch (t: Throwable) {
            // A device policy (managed profiles do this) can revoke the grant
            // between the check and the call, and setInterruptionFilter throws
            // SecurityException. A scan must not die for it.
            previousFilter = null
            DndState.FAILED
        }
        return state
    }

    /**
     * Put the filter back. Idempotent: calling it twice, or on a capture that
     * never engaged, does nothing.
     */
    fun release(): Boolean {
        val manager = nm ?: return false
        val prev = previousFilter ?: return false
        previousFilter = null
        state = DndState.DISABLED
        return try {
            if (!manager.isNotificationPolicyAccessGranted) return false
            val restore = CaptureFocus.filterToRestore(prev, manager.currentInterruptionFilter)
                ?: return false
            manager.setInterruptionFilter(restore)
            true
        } catch (t: Throwable) {
            false
        }
    }

    /**
     * Crash recovery. A process death mid-capture leaves the phone quiet with
     * nobody holding [previousFilter], so this is called once at app start:
     * if we are the reason the filter is PRIORITY and no capture is running,
     * put it back to ALL.
     *
     * `wasCapturing` comes from the app's own persisted "a capture was in
     * flight" marker, which is the only evidence available after the fact —
     * the framework cannot say who set the filter.
     */
    fun restoreOrphaned(wasCapturing: Boolean): Boolean {
        if (!wasCapturing) return false
        val manager = nm ?: return false
        return try {
            if (!manager.isNotificationPolicyAccessGranted) return false
            if (manager.currentInterruptionFilter != CaptureFocus.DESIRED_FILTER) return false
            manager.setInterruptionFilter(CaptureFocus.FILTER_ALL)
            true
        } catch (t: Throwable) {
            false
        }
    }
}
