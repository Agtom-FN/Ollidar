package com.lidarscan.app.rtk

import android.content.Context
import com.lidarscan.app.engine.NativeGnssLayout
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.gnss.GeorefRecord
import com.lidarscan.core.gnss.GnssFixSnapshot
import com.lidarscan.core.gnss.GnssStatsSnapshot
import com.lidarscan.core.gnss.NtripSettings
import com.lidarscan.core.gnss.NtripState
import com.lidarscan.core.gnss.NtripStatsSnapshot
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * B9 — one place that owns the rover link, the NTRIP client, and the polled
 * fix/stats the §3.4 status strip and capture gate read.
 *
 * **Engine-lifetime, not session-lifetime**, deliberately mirroring how A10
 * wires GNSS inside the engine (`core/engine.h`: "One GnssSource, one
 * TcpNtripClient and one GeorefFusion per Engine, all alive for the Engine's
 * whole lifetime: an operator pairs the rover, joins a caster and waits for RTK
 * Fixed BEFORE pressing record, and §3.4's capture gate is that pre-session
 * decision."). Held by `AppContainer`, so navigating away from the RTK screen
 * does not drop the rover.
 *
 * ### Polling, not events
 *
 * `SCAN_EVENT_GNSS_FIX` and `SCAN_EVENT_NTRIP_STATE` exist and carry real
 * payloads at ABI 3+, but B2's event pump marshals a *fixed* `(i0..i4, d0)`
 * tuple across to Kotlin, which cannot carry a fix's ten fields. Widening that
 * pump signature would touch every existing event consumer for a 1 Hz stream, so
 * this polls `scan_engine_last_fix`/`scan_engine_gnss_stats` at 1 Hz instead —
 * the same cadence the data arrives at. Noted in NOTES.md as the one place an
 * event would be cheaper than a poll if the pump ever grows a richer shape.
 */
class RtkManager(context: Context, private val scope: CoroutineScope) {

    val rover = RtkRoverConnection(context)

    private var ntripHandle: Long = 0L
    private var pollJob: Job? = null

    private val _fix = MutableStateFlow(GnssFixSnapshot())
    val fix: StateFlow<GnssFixSnapshot> = _fix.asStateFlow()

    private val _stats = MutableStateFlow(GnssStatsSnapshot())
    val stats: StateFlow<GnssStatsSnapshot> = _stats.asStateFlow()

    private val _ntrip = MutableStateFlow(NtripStatsSnapshot())
    val ntrip: StateFlow<NtripStatsSnapshot> = _ntrip.asStateFlow()

    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    private val _message = MutableStateFlow<String?>(null)
    val message: StateFlow<String?> = _message.asStateFlow()

    /**
     * The ENU origin the engine anchored, remembered from the first fix at or
     * above the origin gate.
     *
     * **The C ABI has no accessor for the ENU frame** — `scan_gnss_stats` has
     * `has_origin`/`origin_lat_deg`/`origin_lon_deg`/`origin_height_m` but they
     * are not in the flat array this app decodes today, and
     * `scan_georef_solution` carries the transform without the frame it maps
     * into. A13 needs the frame (`merge/session.h`: "THE ENU FRAME IS NOT
     * OPTIONAL AND IS NOT SHARED"), so B9 records the first qualifying fix
     * itself, using the same rule `GnssSourceConfig::min_fix_for_origin` uses.
     * See NOTES.md's rebind list.
     */
    private var originLat = 0.0
    private var originLon = 0.0
    private var originHeight = 0.0
    private var haveOrigin = false

    suspend fun connectRover(address: String, engineHandle: Long): Result<Unit> = withContext(Dispatchers.IO) {
        val r = rover.connect(address, engineHandle)
        _connected.value = r.isSuccess && rover.isConnected
        _message.value = r.exceptionOrNull()?.message
        if (r.isSuccess) startPolling(engineHandle)
        r
    }

    fun disconnectRover() {
        rover.disconnect()
        _connected.value = false
        pollJob?.cancel()
        pollJob = null
    }

    private fun startPolling(engineHandle: Long) {
        pollJob?.cancel()
        pollJob = scope.launch(Dispatchers.Default) {
            while (true) {
                _fix.value = NativeGnssLayout.decodeFix(ScanEngineNative.nativeLastFix(engineHandle))
                _stats.value = NativeGnssLayout.decodeStats(ScanEngineNative.nativeGnssStats(engineHandle))
                if (ntripHandle != 0L) {
                    _ntrip.value = NativeGnssLayout.decodeNtrip(ScanEngineNative.nativeNtripStats(ntripHandle))
                }
                _connected.value = rover.isConnected
                rover.lastError?.let { _message.value = it }
                captureOrigin()
                // 1 Hz: the rate a rover publishes epochs at. Polling faster
                // would re-read the same closed epoch.
                delay(1000)
            }
        }
    }

    private fun captureOrigin() {
        if (haveOrigin) return
        val f = _fix.value
        // The same gate GnssSourceConfig::min_fix_for_origin applies: anchor on
        // a fix good enough to be worth anchoring to, not on the first one that
        // arrives.
        if (f.hasFix && f.fix.atLeast(FixType.RTK_FLOAT)) {
            originLat = f.latDeg
            originLon = f.lonDeg
            originHeight = f.heightEllipsoidM
            haveOrigin = true
        }
    }

    // --- NTRIP ----------------------------------------------------------------

    /**
     * Connects to the caster. **The first handshake is synchronous inside the
     * engine**, so a wrong password comes back as `PERMISSION_DENIED` and an
     * unknown mountpoint as `NOT_FOUND` from this call — not as an endless
     * "connecting…". Both are treated as permanently fatal by the engine's own
     * reconnect loop, because retrying a rejected password forever is how an
     * account gets banned from a public caster.
     */
    suspend fun connectNtrip(settings: NtripSettings, engineHandle: Long): Result<Unit> =
        withContext(Dispatchers.IO) {
            if (engineHandle == 0L) {
                return@withContext Result.failure(IllegalStateException("No capture engine yet."))
            }
            disconnectNtrip()
            val h = ScanEngineNative.nativeNtripCreate(engineHandle)
            if (h == 0L) {
                return@withContext Result.failure(IllegalStateException("Could not create the NTRIP client: ${ScanEngineNative.nativeLastError()}"))
            }
            ntripHandle = h
            // Install the rover sink BEFORE connecting so the very first frames
            // off the caster reach the rover rather than being dropped on the
            // floor during the handshake window.
            ScanEngineNative.nativeNtripSetRtcmSink(h, rover.rtcmSink())
            val rc = ScanEngineNative.nativeNtripConnect(
                h,
                settings.host,
                settings.port,
                settings.mountpoint,
                settings.username,
                settings.password,
                settings.ntripVersion,
                settings.allowV1Fallback,
                settings.ggaIntervalMs,
                settings.autoReconnect,
            )
            if (rc != ScanEngineNative.ErrorCode.OK) {
                val why = when (rc) {
                    ScanEngineNative.ErrorCode.PERMISSION_DENIED ->
                        "The caster rejected the credentials. Check the username and password — the engine will not " +
                            "retry a rejected password, on purpose."
                    ScanEngineNative.ErrorCode.NOT_FOUND ->
                        "The caster does not have mountpoint \"${settings.mountpoint}\". Fetch the source table and pick one."
                    ScanEngineNative.ErrorCode.TIMEOUT, ScanEngineNative.ErrorCode.NETWORK ->
                        "Could not reach ${settings.host}:${settings.port}. Note that a phone bound to a direct " +
                            "lidar Ethernet link still needs a route to the internet for corrections."
                    else -> ScanEngineNative.nativeErrorStr(rc)
                }
                ScanEngineNative.nativeNtripDestroy(h)
                ntripHandle = 0L
                return@withContext Result.failure(IllegalStateException(why))
            }
            _ntrip.value = NativeGnssLayout.decodeNtrip(ScanEngineNative.nativeNtripStats(h))
            Result.success(Unit)
        }

    fun disconnectNtrip() {
        val h = ntripHandle
        if (h == 0L) return
        ScanEngineNative.nativeNtripSetRtcmSink(h, null)
        ScanEngineNative.nativeNtripDisconnect(h)
        ScanEngineNative.nativeNtripDestroy(h)
        ntripHandle = 0L
        _ntrip.value = NtripStatsSnapshot()
    }

    data class Mountpoint(
        val mountpoint: String,
        val identifier: String,
        val format: String,
        val country: String,
        val latDeg: Double,
        val lonDeg: Double,
        val needsGga: Boolean,
        val fee: Boolean,
    )

    /**
     * Fetches the caster's source table over a short-lived separate connection,
     * so it works before connect() and while streaming.
     *
     * Sorted by distance from the current fix when there is one: baseline
     * length is the dominant term in Fixed-vs-Float (A10 §8), so "nearest
     * first" is the only ordering that helps.
     */
    suspend fun fetchSourcetable(settings: NtripSettings): Result<List<Mountpoint>> =
        withContext(Dispatchers.IO) {
            val text = ScanEngineNative.nativeNtripSourcetableText(
                settings.host, settings.port, settings.username, settings.password, 256,
            ) ?: return@withContext Result.failure(
                IllegalStateException("Could not fetch the source table from ${settings.host}: ${ScanEngineNative.nativeLastError()}"),
            )
            val numbers = ScanEngineNative.nativeNtripSourcetableNumbers(
                settings.host, settings.port, settings.username, settings.password, 256,
            ) ?: DoubleArray(0)
            val n = text.size / 4
            val out = (0 until n).map { i ->
                Mountpoint(
                    mountpoint = text[i * 4],
                    identifier = text[i * 4 + 1],
                    format = text[i * 4 + 2],
                    country = text[i * 4 + 3],
                    latDeg = numbers.getOrElse(i * 6) { 0.0 },
                    lonDeg = numbers.getOrElse(i * 6 + 1) { 0.0 },
                    needsGga = numbers.getOrElse(i * 6 + 2) { 0.0 } != 0.0,
                    fee = numbers.getOrElse(i * 6 + 3) { 0.0 } != 0.0,
                )
            }
            val f = _fix.value
            val sorted = if (f.hasFix) {
                out.sortedBy { m ->
                    val dLat = m.latDeg - f.latDeg
                    val dLon = (m.lonDeg - f.lonDeg) * Math.cos(Math.toRadians(f.latDeg))
                    dLat * dLat + dLon * dLon
                }
            } else {
                out
            }
            Result.success(sorted)
        }

    val ntripState: NtripState get() = _ntrip.value.state

    /**
     * A10's solution snapshotted for the manifest. Null when the engine has
     * nothing (no rover attached at all).
     */
    fun georefRecord(engineHandle: Long): GeorefRecord? {
        if (engineHandle == 0L) return null
        return NativeGnssLayout.decodeGeoref(
            ScanEngineNative.nativeGeorefSolution(engineHandle),
            ScanEngineNative.nativeGeorefBlocker(engineHandle),
            originLat,
            originLon,
            originHeight,
        )
    }

    /** A9's CRS seam — **empty until the georef transform converges**, which is the point. */
    fun crsWkt(engineHandle: Long): String =
        if (engineHandle == 0L) "" else ScanEngineNative.nativeCrsWkt(engineHandle)

    fun crsEpsg(engineHandle: Long): String =
        if (engineHandle == 0L) "" else ScanEngineNative.nativeCrsEpsg(engineHandle)
}
