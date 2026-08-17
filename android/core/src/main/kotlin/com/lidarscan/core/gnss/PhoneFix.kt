package com.lidarscan.core.gnss

import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.sqrt

/**
 * ROUND 5.2: one fix from the **phone's own** location provider.
 *
 * Deliberately a plain value type in `:core` with no `android.location.Location`
 * in sight, so the NMEA synthesis below and the source policy next door are
 * JVM-testable without an emulator. [accuracyM] is Android's own
 * `Location.getAccuracy()` — documented as the 68 % horizontal radius, i.e.
 * already a 1-sigma number — and it is carried through **verbatim**:
 * `A10`'s fusion weights fixes by sigma, and inventing a smaller one here would
 * make a phone fix outvote a real RTK epoch.
 */
data class PhoneFix(
    val latDeg: Double,
    val lonDeg: Double,
    /** Metres above the WGS-84 ellipsoid, as Android reports it. */
    val altitudeM: Double,
    /** `Location.getAccuracy()`: 1-sigma horizontal radius in metres. Never fabricated. */
    val accuracyM: Float,
    /** `Location.getVerticalAccuracyMeters()` when the platform reports one, else null. */
    val verticalAccuracyM: Float? = null,
    /** UTC millis from the fix itself (`Location.getTime()`), not from when we read it. */
    val utcMillis: Long,
    /** Ground speed, m/s, when reported. */
    val speedMps: Float? = null,
    /** Course over ground, degrees true, when reported. */
    val bearingDeg: Float? = null,
    /** Satellite count when the provider exposes one (`GnssStatus`); null otherwise. */
    val satellites: Int? = null,
    /** Monotonic engine-domain timestamp (`SystemClock.elapsedRealtimeNanos`) for A4. */
    val elapsedRealtimeNanos: Long = 0L,
)

/**
 * Turns a [PhoneFix] into the NMEA burst the engine already knows how to ingest.
 *
 * **Why NMEA and not a fix-shaped API.** `engine/capi/scanengine_c.h` exposes
 * exactly one GNSS *ingest* call — `scan_engine_push_nmea(handle, device_id,
 * bytes…)` — and nothing that takes a decoded position (checked; the rest of the
 * GNSS surface is read-only: `scan_engine_last_fix`, `_gnss_stats`,
 * `_georef_solution`). Pushing bytes is therefore not a workaround, it is the
 * only door, and it is the *better* door for two reasons: the bytes land in the
 * `.lscan` as `kGnssNmea` chunks under the engine's record-always guarantee
 * before anything parses them, and A10's whole epoch/sigma/fusion path then
 * treats a phone fix exactly like a rover fix — including publishing the honest
 * sigma, because [gst] carries `Location.getAccuracy()` into the same GST fields
 * a real receiver would.
 *
 * The burst is **GGA → GST → RMC**, in that order and with one shared UTC:
 * `gnss_source.h` closes an epoch when a sentence with a *different* UTC arrives,
 * so all three must agree, and GST must be inside the same epoch as the GGA it
 * describes or the fusion falls back to its HDOP-scaled guess. RMC is included
 * for its **date** — the engine carries the date forward from RMC, and a GGA-only
 * stream has no year at all.
 */
object PhoneFixNmea {

    /**
     * `1` — GPS/SPS fix, GGA field 6. A phone's fused location is a single-point
     * solution: not DGPS (2), not RTK (4/5). Saying 4 here would make the engine
     * (and the UI, and A10's convergence gate) believe centimetres exist.
     */
    const val GGA_QUALITY_SINGLE = 1

    /** The three sentences for one fix, in epoch order, each `\r\n`-terminated. */
    fun burst(fix: PhoneFix, talker: String = "GP"): String =
        gga(fix, talker) + gst(fix, talker) + rmc(fix, talker)

    fun gga(fix: PhoneFix, talker: String = "GP"): String {
        val t = UtcParts.of(fix.utcMillis)
        val body = buildString {
            append(talker).append("GGA,")
            append(t.hhmmssSs).append(',')
            append(latField(fix.latDeg)).append(',')
            append(nsField(fix.latDeg)).append(',')
            append(lonField(fix.lonDeg)).append(',')
            append(ewField(fix.lonDeg)).append(',')
            append(GGA_QUALITY_SINGLE).append(',')
            // Satellites in use: only when the platform actually told us. An
            // invented "12" is the kind of number that gets read as a quality
            // signal later.
            append(fix.satellites?.let { "%02d".format(it.coerceIn(0, 99)) }.orEmpty()).append(',')
            // HDOP: derived from the reported accuracy so the engine's no-GST
            // fallback lands in the right order of magnitude — but GST below is
            // what it will actually use.
            append("%.1f".format(hdopFromAccuracy(fix.accuracyM))).append(',')
            append("%.2f".format(fix.altitudeM)).append(",M,")
            // Geoid separation: EMPTY, not 0.0. Android reports ellipsoidal
            // height (WGS-84) already, and writing 0.0 would assert "the geoid
            // is exactly the ellipsoid here", which is wrong by tens of metres
            // almost everywhere. crs.h's own note says ellipsoidal = alt + this.
            append(",M,,")
        }
        return sentence(body)
    }

    /**
     * GST — the honest sigma. Fields: UTC, RMS, semi-major, semi-minor,
     * orientation, σlat, σlon, σalt.
     *
     * Android gives one horizontal number, so the error ellipse is modelled as a
     * **circle**: semi-major = semi-minor = accuracy, and σlat = σlon =
     * accuracy / √2 so that √(σlat² + σlon²) is the reported accuracy rather
     * than 1.41× it. Vertical sigma is the platform's own when it has one; when
     * it does not, it is left **empty** rather than guessed — `decode_gst` only
     * sets `has_sigmas` when all three parse, so an empty σalt makes the engine
     * fall back to the ellipse fields for horizontal accuracy instead of
     * accepting a fabricated vertical one.
     */
    fun gst(fix: PhoneFix, talker: String = "GP"): String {
        val t = UtcParts.of(fix.utcMillis)
        val acc = if (fix.accuracyM.isFinite() && fix.accuracyM > 0f) fix.accuracyM.toDouble() else 0.0
        val axis = acc / sqrt(2.0)
        val body = buildString {
            append(talker).append("GST,")
            append(t.hhmmssSs).append(',')
            append("%.2f".format(acc)).append(',') // RMS of the residuals, best available
            append("%.2f".format(acc)).append(',') // semi-major
            append("%.2f".format(acc)).append(',') // semi-minor
            append("0.0").append(',') // orientation: a circle has none
            append("%.2f".format(axis)).append(',')
            append("%.2f".format(axis)).append(',')
            append(fix.verticalAccuracyM?.takeIf { it.isFinite() && it > 0f }?.let { "%.2f".format(it) }.orEmpty())
        }
        return sentence(body)
    }

    fun rmc(fix: PhoneFix, talker: String = "GP"): String {
        val t = UtcParts.of(fix.utcMillis)
        val body = buildString {
            append(talker).append("RMC,")
            append(t.hhmmssSs).append(',')
            append('A').append(',')
            append(latField(fix.latDeg)).append(',')
            append(nsField(fix.latDeg)).append(',')
            append(lonField(fix.lonDeg)).append(',')
            append(ewField(fix.lonDeg)).append(',')
            // Speed in knots and course, only when reported — a stationary
            // phone's bearing is noise, and gnss_source.h already refuses to
            // derive heading below its own speed gate.
            append(fix.speedMps?.takeIf { it.isFinite() }?.let { "%.2f".format(it * KNOTS_PER_MPS) }.orEmpty())
            append(',')
            append(fix.bearingDeg?.takeIf { it.isFinite() }?.let { "%.1f".format(it) }.orEmpty()).append(',')
            append(t.ddmmyy).append(',')
            append(",,A")
        }
        return sentence(body)
    }

    /** `$<body>*<CS>\r\n` with NMEA's XOR checksum over the body. */
    private fun sentence(body: String): String {
        var cs = 0
        for (c in body) cs = cs xor c.code
        return "$" + body + "*" + "%02X".format(cs) + "\r\n"
    }

    private const val KNOTS_PER_MPS = 1.943844492

    /** `ddmm.mmmm` for a latitude. */
    internal fun latField(latDeg: Double): String = degreesMinutes(abs(latDeg), 2)

    /** `dddmm.mmmm` for a longitude. */
    internal fun lonField(lonDeg: Double): String = degreesMinutes(abs(lonDeg), 3)

    private fun degreesMinutes(absDeg: Double, degreeDigits: Int): String {
        val d = floor(absDeg).toInt()
        val minutes = (absDeg - d) * 60.0
        return "%0${degreeDigits}d%07.4f".format(d, minutes)
    }

    internal fun nsField(latDeg: Double): Char = if (latDeg < 0) 'S' else 'N'
    internal fun ewField(lonDeg: Double): Char = if (lonDeg < 0) 'W' else 'E'

    /**
     * A crude accuracy → HDOP mapping (1 HDOP ≈ 5 m of horizontal accuracy for a
     * consumer receiver), clamped to NMEA's sane range. Only ever used as the
     * engine's *fallback* when GST is unavailable; GST is always present here, so
     * this exists to avoid writing an empty field that some parser might read as
     * "perfect".
     */
    internal fun hdopFromAccuracy(accuracyM: Float): Double =
        if (!accuracyM.isFinite() || accuracyM <= 0f) 99.9 else (accuracyM / 5.0).coerceIn(0.5, 99.9)

    /** UTC fields NMEA wants, split out so both sentence builders agree to the millisecond. */
    internal data class UtcParts(val hhmmssSs: String, val ddmmyy: String) {
        companion object {
            fun of(utcMillis: Long): UtcParts {
                val t = java.time.Instant.ofEpochMilli(utcMillis).atZone(java.time.ZoneOffset.UTC)
                val secondsWithFraction = t.second + t.nano / 1_000_000_000.0
                return UtcParts(
                    hhmmssSs = "%02d%02d%05.2f".format(t.hour, t.minute, secondsWithFraction),
                    ddmmyy = "%02d%02d%02d".format(t.dayOfMonth, t.monthValue, t.year % 100),
                )
            }
        }
    }
}
