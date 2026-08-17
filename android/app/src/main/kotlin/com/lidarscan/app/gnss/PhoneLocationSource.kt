package com.lidarscan.app.gnss

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.core.content.ContextCompat
import com.lidarscan.core.gnss.PhoneFix
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow

/**
 * ROUND 5.2: the phone's own location, at ~1 Hz, as [PhoneFix]es.
 *
 * ### Why `LocationManager` and not the fused Play-services client
 *
 * `FusedLocationProviderClient` lives in `play-services-location`, which this
 * project does **not** depend on (and cannot be added here — the build runs
 * offline against a pinned dependency set). The platform's own
 * `LocationManager` covers the requirement without it, and since **API 31** the
 * platform itself exposes the fused engine as
 * [LocationManager.FUSED_PROVIDER] — so on a modern phone this *is* the fused
 * provider, just reached through the framework rather than through Play services.
 * Below 31 it falls back to GPS, which is the right choice for a survey app:
 * `NETWORK_PROVIDER` is hundreds of metres and would be worse than recording no
 * georeference at all.
 *
 * ### Honesty about accuracy
 *
 * Every emitted fix carries `Location.getAccuracy()` verbatim as its 1-sigma
 * horizontal radius, and `getVerticalAccuracyMeters()` when the platform reports
 * one. A fix with **no** accuracy at all is dropped rather than emitted with a
 * zero: downstream, zero sigma means "perfect", which is the one thing a phone
 * fix never is. Altitude is Android's WGS-84 ellipsoidal height (documented on
 * `Location.getAltitude()`), which is what the engine's `crs.h` wants.
 */
class PhoneLocationSource(private val context: Context) {

    fun hasPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    /**
     * Emits fixes until the collector is cancelled. Cold: the provider is
     * registered on collection and unregistered on cancellation, so nothing
     * touches the GPS chip outside a capture.
     *
     * Silently completes (rather than throwing) when the permission is missing or
     * no provider exists — round 5.2 requires denial to degrade the capture, not
     * end it.
     */
    fun fixes(intervalMs: Long = DEFAULT_INTERVAL_MS): Flow<PhoneFix> = callbackFlow {
        if (!hasPermission()) {
            close()
            return@callbackFlow
        }
        val manager = context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager
        if (manager == null) {
            close()
            return@callbackFlow
        }
        val provider = preferredProvider(manager)
        if (provider == null) {
            close()
            return@callbackFlow
        }

        val listener = LocationListener { location -> toFix(location)?.let { trySend(it) } }

        try {
            // minDistance 0: a stationary scan still needs fixes — the whole point
            // is a georeference for the session, not a track of movement.
            manager.requestLocationUpdates(provider, intervalMs, 0f, listener, Looper.getMainLooper())
        } catch (e: SecurityException) {
            // Permission revoked between the check above and the call.
            Log.w(TAG, "location permission revoked mid-request", e)
            close()
            return@callbackFlow
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "provider $provider unavailable", e)
            close()
            return@callbackFlow
        }

        // A last known fix immediately, so the chip shows a real accuracy in the
        // first second instead of "waiting" for a whole provider cycle.
        runCatching { manager.getLastKnownLocation(provider) }
            .getOrNull()
            ?.takeIf { SystemClock.elapsedRealtime() - it.elapsedRealtimeMillis < LAST_KNOWN_MAX_AGE_MS }
            ?.let { toFix(it) }
            ?.let { trySend(it) }

        awaitClose { runCatching { manager.removeUpdates(listener) } }
    }

    /**
     * `FUSED_PROVIDER` on API 31+, else GPS. Deliberately no `NETWORK_PROVIDER`
     * fallback — see the class doc.
     */
    private fun preferredProvider(manager: LocationManager): String? {
        val candidates = buildList {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) add(LocationManager.FUSED_PROVIDER)
            add(LocationManager.GPS_PROVIDER)
        }
        return candidates.firstOrNull { runCatching { manager.isProviderEnabled(it) }.getOrDefault(false) }
    }

    private fun toFix(location: Location): PhoneFix? {
        if (!location.hasAccuracy()) return null
        val accuracy = location.accuracy
        if (!accuracy.isFinite() || accuracy <= 0f) return null
        return PhoneFix(
            latDeg = location.latitude,
            lonDeg = location.longitude,
            altitudeM = if (location.hasAltitude()) location.altitude else 0.0,
            accuracyM = accuracy,
            verticalAccuracyM = if (location.hasVerticalAccuracy()) location.verticalAccuracyMeters else null,
            utcMillis = location.time,
            speedMps = if (location.hasSpeed()) location.speed else null,
            bearingDeg = if (location.hasBearing()) location.bearing else null,
            // Satellite count: NOT available from a Location. `GnssStatus` has it,
            // behind a second callback and its own registration; left null so the
            // synthesized GGA leaves the field empty rather than inventing a count.
            satellites = null,
            // The engine's own clock domain (CLOCK_BOOTTIME), which is what A4
            // wants for the (t_device, t_arrival) pair — `Location` carries it
            // directly, so no conversion and no guess.
            elapsedRealtimeNanos = location.elapsedRealtimeNanos,
        )
    }

    private companion object {
        const val TAG = "PhoneLocationSource"

        /** ~1 Hz, per the owner's brief. Matches a rover's own epoch rate. */
        const val DEFAULT_INTERVAL_MS = 1_000L

        /** A last-known fix older than this is history, not a georeference. */
        const val LAST_KNOWN_MAX_AGE_MS = 30_000L
    }
}
