package com.lidarscan.app.engine

import com.lidarscan.core.gnss.FixType
import com.lidarscan.core.gnss.GeorefRecord
import com.lidarscan.core.gnss.GnssFixSnapshot
import com.lidarscan.core.gnss.GnssStatsSnapshot
import com.lidarscan.core.gnss.NtripState
import com.lidarscan.core.gnss.NtripStatsSnapshot

/**
 * B9: the index layout of the flat `double[]`s `gnss_jni.cpp` hands back, and
 * the decoders that turn them into `:core` value types.
 *
 * **Why flat arrays and not marshalling classes.** `scan_gnss_fix`,
 * `scan_gnss_stats`, `scan_ntrip_stats` and `scan_georef_solution` are four
 * wide structs of plain numbers with no strings in them (the one exception,
 * `blocker`, is fetched separately). Giving each a Kotlin class would mean four
 * more hand-typed JNI constructor descriptors, and B2/B4/B7 all flagged that as
 * the one bug class that compiles cleanly on both sides and only fails at
 * `JNI_OnLoad`. A flat array has no descriptor: a layout mistake is a wrong
 * number, which the unit tests below this file catch, not a load-time abort.
 *
 * A `double` carries every field exactly — the widest are `uint64` counters and
 * a double is exact to 2^53, which at 1 MB/s of NMEA is 285 years of
 * `bytes_in`. The single field that does not fit is `utc_unix_ns`, so it
 * crosses as **milliseconds** and is named accordingly.
 */
object NativeGnssLayout {

    // --- scan_gnss_fix: 22 doubles ------------------------------------------
    const val FIX_LEN = 22
    const val FIX_TYPE = 0
    const val FIX_SATELLITES = 1
    const val FIX_QUALITY_RAW = 2
    const val FIX_DIMENSION = 3
    const val FIX_STATION_ID = 4
    const val FIX_LAT_DEG = 5
    const val FIX_LON_DEG = 6

    /** ORTHOMETRIC (MSL), exactly as GGA reports it — not the ellipsoidal height. */
    const val FIX_ALT_M = 7
    const val FIX_GEOID_SEP_M = 8
    const val FIX_HEIGHT_ELLIPSOID_M = 9
    const val FIX_HAS_GEOID_SEP = 10
    const val FIX_HDOP = 11
    const val FIX_PDOP = 12
    const val FIX_VDOP = 13
    const val FIX_CORRECTION_AGE_S = 14
    const val FIX_SIGMA_H_M = 15
    const val FIX_SIGMA_FROM_GST = 16
    const val FIX_SPEED_MPS = 17
    const val FIX_COURSE_DEG = 18
    const val FIX_HAS_COURSE = 19
    const val FIX_UTC_UNIX_MILLIS = 20
    const val FIX_HAS_FIX = 21

    // --- scan_gnss_stats: 19 doubles ----------------------------------------
    const val STATS_LEN = 19
    const val STATS_BYTES_IN = 0
    const val STATS_SENTENCES_OK = 1
    const val STATS_CHECKSUM_FAILED = 2
    const val STATS_MALFORMED = 3
    const val STATS_CHECKSUM_PASS_RATE = 4
    const val STATS_EPOCHS = 5
    const val STATS_FIXES_PUBLISHED = 6
    const val STATS_POSES_PUBLISHED = 7
    const val STATS_EPOCHS_NO_POSITION = 8
    const val STATS_EPOCHS_BELOW_GATE = 9
    const val STATS_GST_EPOCHS = 10

    /** `by_fix[0..4]`, indexed by [FixType.code] — the §3.4 fix-quality timeline. */
    const val STATS_BY_FIX_0 = 11
    const val STATS_TIME_CONVERGED = 16
    const val STATS_HAS_ORIGIN = 17
    const val STATS_TIME_UNCERTAINTY_NS = 18

    // --- scan_ntrip_stats: 18 doubles ---------------------------------------
    const val NTRIP_LEN = 18
    const val NTRIP_STATE = 0
    const val NTRIP_RECEIVING = 1
    const val NTRIP_CORRECTION_AGE_S = 2
    const val NTRIP_BYTES_RECEIVED = 3
    const val NTRIP_FRAMES_OK = 4
    const val NTRIP_FRAMES_CRC_FAILED = 5
    const val NTRIP_RTCM_BYTES = 6
    const val NTRIP_GGA_SENT = 7
    const val NTRIP_CONNECT_ATTEMPTS = 8
    const val NTRIP_CONNECTS_OK = 9
    const val NTRIP_DISCONNECTS = 10
    const val NTRIP_RECONNECTS = 11
    const val NTRIP_STALLS = 12
    const val NTRIP_HANDSHAKE_FAILURES = 13
    const val NTRIP_BACKOFF_MS = 14
    const val NTRIP_HTTP_STATUS = 15
    const val NTRIP_VERSION_USED = 16
    const val NTRIP_LAST_ERROR = 17

    // --- scan_georef_solution: 27 doubles ------------------------------------
    const val GEOREF_LEN = 27
    const val GEOREF_CONVERGED = 0
    const val GEOREF_EPSG = 1
    const val GEOREF_YAW_DEG = 2

    /** 16 doubles, ROW-MAJOR — the same convention every matrix crossing this JNI uses. */
    const val GEOREF_MATRIX_0 = 3
    const val GEOREF_HORIZONTAL_SIGMA_M = 19
    const val GEOREF_VERTICAL_SIGMA_M = 20
    const val GEOREF_CEP95_M = 21
    const val GEOREF_SAMPLES = 22
    const val GEOREF_INLIERS = 23
    const val GEOREF_RESIDUAL_RMS_M = 24
    const val GEOREF_SPAN_M = 25
    const val GEOREF_DOMINANT_FIX = 26

    fun decodeFix(a: DoubleArray?): GnssFixSnapshot {
        if (a == null || a.size < FIX_LEN) return GnssFixSnapshot()
        return GnssFixSnapshot(
            fix = FixType.fromCode(a[FIX_TYPE].toInt()),
            satellites = a[FIX_SATELLITES].toInt(),
            qualityRaw = a[FIX_QUALITY_RAW].toInt(),
            stationId = a[FIX_STATION_ID].toInt(),
            latDeg = a[FIX_LAT_DEG],
            lonDeg = a[FIX_LON_DEG],
            altM = a[FIX_ALT_M],
            heightEllipsoidM = a[FIX_HEIGHT_ELLIPSOID_M],
            hdop = a[FIX_HDOP].toFloat(),
            correctionAgeS = a[FIX_CORRECTION_AGE_S].toFloat(),
            sigmaHorizontalM = a[FIX_SIGMA_H_M].toFloat(),
            sigmaFromGst = a[FIX_SIGMA_FROM_GST] != 0.0,
            speedMps = a[FIX_SPEED_MPS].toFloat(),
            utcUnixNs = (a[FIX_UTC_UNIX_MILLIS] * 1_000_000.0).toLong(),
            hasFix = a[FIX_HAS_FIX] != 0.0,
        )
    }

    fun decodeStats(a: DoubleArray?): GnssStatsSnapshot {
        if (a == null || a.size < STATS_LEN) return GnssStatsSnapshot()
        return GnssStatsSnapshot(
            bytesIn = a[STATS_BYTES_IN].toLong(),
            sentencesOk = a[STATS_SENTENCES_OK].toLong(),
            checksumFailed = a[STATS_CHECKSUM_FAILED].toLong(),
            malformed = a[STATS_MALFORMED].toLong(),
            checksumPassRate = a[STATS_CHECKSUM_PASS_RATE],
            epochs = a[STATS_EPOCHS].toLong(),
            fixesPublished = a[STATS_FIXES_PUBLISHED].toLong(),
            epochsNoPosition = a[STATS_EPOCHS_NO_POSITION].toLong(),
            epochsBelowGate = a[STATS_EPOCHS_BELOW_GATE].toLong(),
            gstEpochs = a[STATS_GST_EPOCHS].toLong(),
            byFix = LongArray(5) { a[STATS_BY_FIX_0 + it].toLong() },
            timeConverged = a[STATS_TIME_CONVERGED] != 0.0,
            hasOrigin = a[STATS_HAS_ORIGIN] != 0.0,
        )
    }

    fun decodeNtrip(a: DoubleArray?): NtripStatsSnapshot {
        if (a == null || a.size < NTRIP_LEN) return NtripStatsSnapshot()
        return NtripStatsSnapshot(
            state = NtripState.fromCode(a[NTRIP_STATE].toInt()),
            receiving = a[NTRIP_RECEIVING] != 0.0,
            bytesReceived = a[NTRIP_BYTES_RECEIVED].toLong(),
            framesOk = a[NTRIP_FRAMES_OK].toLong(),
            framesCrcFailed = a[NTRIP_FRAMES_CRC_FAILED].toLong(),
            rtcmBytes = a[NTRIP_RTCM_BYTES].toLong(),
            ggaSent = a[NTRIP_GGA_SENT].toLong(),
            reconnects = a[NTRIP_RECONNECTS].toLong(),
            stalls = a[NTRIP_STALLS].toLong(),
            correctionAgeS = a[NTRIP_CORRECTION_AGE_S].toFloat(),
            backoffMs = a[NTRIP_BACKOFF_MS].toInt(),
            httpStatus = a[NTRIP_HTTP_STATUS].toInt(),
            ntripVersionUsed = a[NTRIP_VERSION_USED].toInt(),
            lastError = a[NTRIP_LAST_ERROR].toInt(),
        )
    }

    /**
     * @param originLatDeg/@param originLonDeg/@param originHeightM the GnssSource's
     *   anchored ENU origin. **Not in `scan_georef_solution`** — the C ABI has no
     *   accessor for the frame at all (see android/NOTES.md's rebind list) — so
     *   B9 takes it from the first fix at or above the origin gate, which is the
     *   same rule `GnssSourceConfig::min_fix_for_origin` uses to anchor it.
     */
    fun decodeGeoref(
        a: DoubleArray?,
        blocker: String,
        originLatDeg: Double,
        originLonDeg: Double,
        originHeightM: Double,
    ): GeorefRecord? {
        if (a == null || a.size < GEOREF_LEN) return null
        return GeorefRecord(
            converged = a[GEOREF_CONVERGED] != 0.0,
            epsg = a[GEOREF_EPSG].toInt(),
            yawDeg = a[GEOREF_YAW_DEG],
            globalFromLocal = DoubleArray(16) { a[GEOREF_MATRIX_0 + it] },
            enuOriginLatDeg = originLatDeg,
            enuOriginLonDeg = originLonDeg,
            enuOriginHeightM = originHeightM,
            horizontalSigmaM = a[GEOREF_HORIZONTAL_SIGMA_M],
            verticalSigmaM = a[GEOREF_VERTICAL_SIGMA_M],
            cep95M = a[GEOREF_CEP95_M],
            samples = a[GEOREF_SAMPLES].toInt(),
            inliers = a[GEOREF_INLIERS].toInt(),
            residualRmsM = a[GEOREF_RESIDUAL_RMS_M],
            spanM = a[GEOREF_SPAN_M],
            dominantFix = FixType.fromCode(a[GEOREF_DOMINANT_FIX].toInt()),
            blocker = blocker,
        )
    }
}
