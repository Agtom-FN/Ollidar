package com.lidarscan.app.ar

import android.app.Activity
import android.content.Context
import com.google.ar.core.ArCoreApk
import com.google.ar.core.exceptions.UnavailableDeviceNotCompatibleException
import com.google.ar.core.exceptions.UnavailableUserDeclinedInstallationException

/**
 * ARCore's availability + install flow, as a small state machine the UI can
 * render without touching `ArCoreApk` itself.
 *
 * The install flow's shape is dictated by ARCore, not chosen here:
 * `requestInstall()` may launch Google Play's ARCore installer, which pauses
 * this activity; when it returns, the app must call `requestInstall()` AGAIN
 * to find out whether the user actually completed it. Missing that second
 * call is the classic ARCore integration bug — the app comes back to the
 * foreground and creates a `Session` that throws
 * `UnavailableArcoreNotInstalledException`. [ArInstaller] carries the
 * `userRequestedInstall` latch that makes the second call behave.
 */
enum class ArAvailability {
    /** Checking (ARCore's own query is asynchronous on a cold start). */
    CHECKING,

    /** ARCore is installed and up to date — a session can be created. */
    READY,

    /** Supported, but the ARCore APK must be installed or updated first. */
    NEEDS_INSTALL,

    /** This device cannot run ARCore at all. */
    UNSUPPORTED,

    /** ARCore could not tell us (transient error, or the Play Store is unreachable). */
    UNKNOWN,
    ;

    val canRunAr: Boolean get() = this == READY

    companion object {
        fun from(availability: ArCoreApk.Availability): ArAvailability = when (availability) {
            ArCoreApk.Availability.SUPPORTED_INSTALLED -> READY
            ArCoreApk.Availability.SUPPORTED_NOT_INSTALLED,
            ArCoreApk.Availability.SUPPORTED_APK_TOO_OLD,
            -> NEEDS_INSTALL
            ArCoreApk.Availability.UNSUPPORTED_DEVICE_NOT_CAPABLE -> UNSUPPORTED
            ArCoreApk.Availability.UNKNOWN_CHECKING -> CHECKING
            ArCoreApk.Availability.UNKNOWN_ERROR,
            ArCoreApk.Availability.UNKNOWN_TIMED_OUT,
            -> UNKNOWN
        }
    }
}

/**
 * User-facing copy for each state. Kept beside the enum so the Capture screen
 * and the calibration wizard say the same thing about the same condition —
 * the alternative (two screens inventing their own wording for
 * `UNSUPPORTED_DEVICE_NOT_CAPABLE`) is how a user ends up being told two
 * different stories about one device.
 */
fun ArAvailability.message(): String = when (this) {
    ArAvailability.CHECKING -> "Checking phone tracking support…"
    ArAvailability.READY -> "Phone tracking ready"
    ArAvailability.NEEDS_INSTALL -> "Google Play Services for AR needs to be installed or updated — it is what supplies the phone's motion tracking"
    ArAvailability.UNSUPPORTED -> "This phone cannot track its own motion (no ARCore), so a COIN-D6 cannot be scanned in 3D on it"
    ArAvailability.UNKNOWN -> "Could not check phone-tracking support — check the network and try again"
}

class ArInstaller {

    /**
     * False on the first `requestInstall()` of a session and true on every
     * subsequent one, per ARCore's documented contract: the first call may
     * *start* an install (returning INSTALL_REQUESTED and pausing us), and the
     * call after resuming is what reports whether it finished.
     */
    private var userRequestedInstall = false

    fun availability(context: Context): ArAvailability =
        ArAvailability.from(ArCoreApk.getInstance().checkAvailability(context))

    /**
     * Returns true when ARCore is installed and a session may be created.
     * Returns false when an install was requested — the activity will be
     * paused and resumed, and the caller should ask again from `onResume`.
     * Throws nothing: every ARCore exception is mapped to a [Result] so a
     * caller cannot forget one.
     */
    fun requestInstall(activity: Activity): Result<Boolean> = try {
        when (ArCoreApk.getInstance().requestInstall(activity, !userRequestedInstall)) {
            ArCoreApk.InstallStatus.INSTALLED -> {
                userRequestedInstall = false
                Result.success(true)
            }
            ArCoreApk.InstallStatus.INSTALL_REQUESTED -> {
                userRequestedInstall = true
                Result.success(false)
            }
        }
    } catch (e: UnavailableUserDeclinedInstallationException) {
        userRequestedInstall = false
        Result.failure(e)
    } catch (e: UnavailableDeviceNotCompatibleException) {
        Result.failure(e)
    } catch (e: Exception) {
        Result.failure(e)
    }

    fun reset() {
        userRequestedInstall = false
    }
}
