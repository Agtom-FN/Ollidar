package com.lidarscan.core.gnss

import kotlinx.serialization.Serializable
import kotlin.math.abs

/**
 * B9 — the GNSS/RTK value types the fix-status strip, the NTRIP screen and the
 * §3.4 capture gate are written against. Plain Kotlin in `:core` so all of it
 * is JVM-unit-testable with no device: the JNI layer (`gnss_jni.cpp`) marshals
 * `scan_gnss_fix` / `scan_gnss_stats` / `scan_ntrip_stats` straight into these.
 */

/**
 * Mirror of `SCAN_FIX_*` (`engine/capi/scanengine_c.h`).
 *
 * **The ordinal order is load-bearing.** `scanengine_c.h` says so in as many
 * words: Tech Spec §3.4's capture gate is "at or above", so a UI compares these
 * numerically. This is deliberately **not** the GGA quality digit — GGA numbers
 * RTK-fixed 4 and RTK-float 5, i.e. the wrong way round for a ladder — which is
 * why `scan_gnss_fix` keeps that separately as `quality_raw`.
 */
@Serializable
enum class FixType(val code: Int, val label: String, val shortLabel: String) {
    NONE(0, "No fix", "—"),
    SINGLE(1, "Single", "SPS"),
    DGPS(2, "DGPS", "DGPS"),
    RTK_FLOAT(3, "RTK Float", "FLOAT"),
    RTK_FIXED(4, "RTK Fixed", "FIXED"),
    ;

    /** §3.4's "at or above" comparison, in one place so no screen re-invents it. */
    fun atLeast(other: FixType): Boolean = code >= other.code

    companion object {
        fun fromCode(code: Int): FixType = entries.firstOrNull { it.code == code } ?: NONE
    }
}

/** Mirror of `SCAN_NTRIP_*`. */
@Serializable
enum class NtripState(val code: Int, val label: String) {
    IDLE(0, "Idle"),
    CONNECTING(1, "Connecting"),
    STREAMING(2, "Streaming"),

    /** Socket open, but no RTCM for `stall_timeout_ms` — A10 §8: 30 s of silence is a black hole, not a quiet caster. */
    STALLED(3, "Stalled"),
    RECONNECTING(4, "Reconnecting"),

    /** Terminal: auth, bad mountpoint, or attempts exhausted. A10 §8 treats 401/404 as permanently fatal on purpose. */
    FAILED(5, "Failed"),
    ;

    companion object {
        fun fromCode(code: Int): NtripState = entries.firstOrNull { it.code == code } ?: IDLE
    }
}

/**
 * One closed NMEA epoch — the subset of `scan_gnss_fix` the UI shows.
 *
 * [altM] is **orthometric** (MSL), exactly as GGA reports it, and
 * [heightEllipsoidM] is what the geodesy uses. A10 §6 calls mixing them "the
 * classic 30-metre georeferencing bug", so both are carried with the difference
 * named in the field names rather than a single ambiguous "altitude".
 */
data class GnssFixSnapshot(
    val fix: FixType = FixType.NONE,
    val satellites: Int = 0,
    /** GGA field 6 verbatim (3 = PPS, 8 = simulator, …). Shown only in the diagnostics expander. */
    val qualityRaw: Int = 0,
    val stationId: Int = 0,
    val latDeg: Double = 0.0,
    val lonDeg: Double = 0.0,
    val altM: Double = 0.0,
    val heightEllipsoidM: Double = 0.0,
    val hdop: Float = 0f,
    /** GGA field 13 — the **rover's** corrections age, not the engine's. See [NtripStatsSnapshot.correctionAgeS]. */
    val correctionAgeS: Float = -1f,
    val sigmaHorizontalM: Float = 0f,
    /** True when the sigma came from a GST sentence; false means A10's fix-quality fallback table. */
    val sigmaFromGst: Boolean = false,
    val speedMps: Float = 0f,
    val utcUnixNs: Long = 0L,
    val hasFix: Boolean = false,
) {
    /**
     * The accuracy line the status strip prints.
     *
     * A10 §7 is explicit that a UI quoting an accuracy should say where it came
     * from, because a GST-derived sigma is a measurement and the fallback is a
     * table lookup keyed on the fix type — the same number means two very
     * different things.
     */
    fun accuracyText(): String = when {
        !hasFix || fix == FixType.NONE -> "No accuracy — no fix"
        sigmaHorizontalM <= 0f -> "Accuracy unknown"
        sigmaFromGst -> "±${formatMetres(sigmaHorizontalM.toDouble())} horizontal (measured, GST)"
        else -> "±${formatMetres(sigmaHorizontalM.toDouble())} horizontal (estimated from fix type)"
    }

    fun positionText(): String =
        if (!hasFix) "—" else "%.7f, %.7f".format(latDeg, lonDeg)
}

/** The §3.4 fix-quality timeline plus the NMEA link health behind it (`scan_gnss_stats`). */
data class GnssStatsSnapshot(
    val bytesIn: Long = 0,
    val sentencesOk: Long = 0,
    val checksumFailed: Long = 0,
    val malformed: Long = 0,
    val checksumPassRate: Double = 0.0,
    val epochs: Long = 0,
    val fixesPublished: Long = 0,
    val epochsNoPosition: Long = 0,
    val epochsBelowGate: Long = 0,
    val gstEpochs: Long = 0,
    /** Epoch counts indexed by [FixType.code] — the §3.4 timeline itself. */
    val byFix: LongArray = LongArray(5),
    /** A4: false for roughly the first 16 s of a 1 Hz stream. Not an error, and the UI says so. */
    val timeConverged: Boolean = false,
    val hasOrigin: Boolean = false,
) {
    /** Fraction of epochs at or above [gate]. The one number that answers "was this capture usable?". */
    fun fractionAtLeast(gate: FixType): Double {
        val total = byFix.sum()
        if (total == 0L) return 0.0
        var good = 0L
        for (t in FixType.entries) if (t.atLeast(gate)) good += byFix[t.code]
        return good.toDouble() / total.toDouble()
    }

    /** Equals/hashCode are hand-written because of the [LongArray] field. */
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is GnssStatsSnapshot) return false
        return bytesIn == other.bytesIn && sentencesOk == other.sentencesOk &&
            checksumFailed == other.checksumFailed && malformed == other.malformed &&
            checksumPassRate == other.checksumPassRate && epochs == other.epochs &&
            fixesPublished == other.fixesPublished && epochsNoPosition == other.epochsNoPosition &&
            epochsBelowGate == other.epochsBelowGate && gstEpochs == other.gstEpochs &&
            byFix.contentEquals(other.byFix) && timeConverged == other.timeConverged &&
            hasOrigin == other.hasOrigin
    }

    override fun hashCode(): Int = 31 * epochs.hashCode() + byFix.contentHashCode()
}

/** `scan_ntrip_stats`, trimmed to what the corrections card shows. */
data class NtripStatsSnapshot(
    val state: NtripState = NtripState.IDLE,
    /** A CRC-valid frame arrived on the **current** connection. A10 §8: "connected" and "receiving" are different claims. */
    val receiving: Boolean = false,
    val bytesReceived: Long = 0,
    val framesOk: Long = 0,
    /** What separates "bad corrections" from "bad sky" — corruption on the Bluetooth hop leaves the rover silently in Float. */
    val framesCrcFailed: Long = 0,
    val rtcmBytes: Long = 0,
    val ggaSent: Long = 0,
    val reconnects: Long = 0,
    val stalls: Long = 0,
    /** The **engine's** corrections age (time since the last CRC-valid frame off the caster). −1 = no frame yet. */
    val correctionAgeS: Float = -1f,
    val backoffMs: Int = 0,
    val httpStatus: Int = 0,
    val ntripVersionUsed: Int = 0,
    val lastError: Int = 0,
) {
    fun ageText(): String = when {
        correctionAgeS < 0f -> "no corrections yet"
        else -> "%.1f s old".format(correctionAgeS)
    }
}

/**
 * NTRIP caster configuration, persisted in DataStore.
 *
 * Validated in `:core` for the same reason `Mid360Settings` is (B3): the
 * failure mode of a wrong value here is a connection that either never
 * completes or completes and silently delivers nothing, and a field that says
 * so beats an eight-second wait that ends in "failed".
 */
@Serializable
data class NtripSettings(
    val host: String = "",
    val port: Int = 2101,
    val mountpoint: String = "",
    val username: String = "",
    val password: String = "",
    /** `scan_ntrip_config.ntrip_version`; 0 = the client's default (2, with v1 fallback). */
    val ntripVersion: Int = 0,
    val allowV1Fallback: Boolean = true,
    /** `gga_interval_ms`; the NTRIP 2.0 suggestion and u-center's default is 10 s. */
    val ggaIntervalMs: Int = 10_000,
    val autoReconnect: Boolean = true,
) {
    fun validate(): List<SettingsIssue> {
        val issues = mutableListOf<SettingsIssue>()
        if (host.isBlank()) {
            issues += SettingsIssue.fatal("Caster host", "Required — the address of the NTRIP caster (a hostname or an IP).")
        } else if (host.contains("://") || host.contains("/")) {
            issues += SettingsIssue.fatal(
                "Caster host",
                "Host only — no scheme and no path. \"$host\" looks like a URL; the mountpoint goes in its own field.",
            )
        }
        if (port !in 1..65535) {
            issues += SettingsIssue.fatal("Port", "Must be 1–65535. 2101 is the NTRIP default.")
        }
        if (mountpoint.isBlank()) {
            issues += SettingsIssue.fatal(
                "Mountpoint",
                "Required to connect. Fetch the caster's source table to pick one — the nearest base is normally the right answer, " +
                    "because baseline length is the dominant term in Fixed-vs-Float.",
            )
        }
        if (username.isBlank() && password.isNotBlank()) {
            issues += SettingsIssue.warning("Credentials", "A password with no username is unusual; most casters want both or neither.")
        }
        if (ggaIntervalMs in 1..999) {
            issues += SettingsIssue.warning(
                "GGA interval",
                "Below 1 s. A VRS caster only needs your position occasionally; uploading it faster wastes the link.",
            )
        }
        return issues
    }

    val isConnectable: Boolean get() = validate().none { it.severity == IssueSeverity.FATAL }
}

enum class IssueSeverity { FATAL, WARNING, NOTE }

data class SettingsIssue(val severity: IssueSeverity, val field: String, val message: String) {
    companion object {
        fun fatal(field: String, message: String) = SettingsIssue(IssueSeverity.FATAL, field, message)
        fun warning(field: String, message: String) = SettingsIssue(IssueSeverity.WARNING, field, message)
        fun note(field: String, message: String) = SettingsIssue(IssueSeverity.NOTE, field, message)
    }
}

/**
 * The §3.4 capture gate: "D6 outdoor mode: RTK **is** the trajectory source;
 * capture UX warns/blocks below fix-quality threshold."
 *
 * Three verdicts rather than a boolean, because "warn" and "block" are
 * different products of the same comparison and the profile decides which one
 * applies ([com.lidarscan.core.model.CaptureDefaults.requireRtkFixForCapture]).
 */
enum class CaptureGateVerdict { OK, WARN, BLOCK }

data class CaptureGate(
    val verdict: CaptureGateVerdict,
    val headline: String,
    val detail: String,
) {
    val blocksCapture: Boolean get() = verdict == CaptureGateVerdict.BLOCK
}

/**
 * Evaluates the §3.4 gate.
 *
 * @param required the profile's [com.lidarscan.core.model.CaptureDefaults.minFixForCapture]
 * @param enforce the profile's `requireRtkFixForCapture` — true blocks, false warns
 * @param rtkIsTrajectorySource true when there is no other trajectory to fall back on
 *   (a D6 outdoors with no ARCore). This is the case §3.4 singles out, and it is
 *   what turns a missing fix from "the cloud will not be georeferenced" into
 *   "there will be no cloud": with no pose stream the pushbroom assembler resolves
 *   nothing and every point lands in `dropped_no_pose`.
 */
fun evaluateCaptureGate(
    fix: FixType,
    required: FixType,
    enforce: Boolean,
    rtkIsTrajectorySource: Boolean,
): CaptureGate {
    if (required == FixType.NONE && !rtkIsTrajectorySource) {
        return CaptureGate(CaptureGateVerdict.OK, "RTK not required", "This profile does not gate capture on GNSS.")
    }
    if (fix.atLeast(required) && !(rtkIsTrajectorySource && fix == FixType.NONE)) {
        return CaptureGate(
            CaptureGateVerdict.OK,
            "${fix.label} — ready to capture",
            "At or above the ${required.label} threshold this profile asks for.",
        )
    }
    val why = if (rtkIsTrajectorySource) {
        "RTK is the trajectory source for this capture (D6 outdoors, no ARCore), so without a fix there is no pose to " +
            "assemble profiles against — every point would be dropped as \"no pose\", not merely left ungeoreferenced."
    } else {
        "Points captured below ${required.label} will not be georeferenced to the accuracy this profile assumes."
    }
    return if (enforce || (rtkIsTrajectorySource && fix == FixType.NONE)) {
        CaptureGate(CaptureGateVerdict.BLOCK, "${fix.label} — below the ${required.label} gate", why)
    } else {
        CaptureGate(CaptureGateVerdict.WARN, "${fix.label} — below the ${required.label} gate", why)
    }
}

/**
 * A10's `GeorefSolution`, snapshotted into the project manifest at capture stop.
 *
 * A10 §9.6 asks for exactly this: "a periodic `GeorefSolution` + origin snapshot
 * in the manifest so a replay does not have to re-derive the alignment". B12's
 * auto-merge is the consumer that makes it load-bearing — a merge needs each
 * session's `global_from_local` **and the ENU frame it is expressed in**
 * (`merge/session.h`: "THE ENU FRAME IS NOT OPTIONAL AND IS NOT SHARED"), and
 * neither survives the end of a capture session any other way.
 *
 * [horizontalSigmaM] is A10's deliberately conservative number (§5.1: the
 * fixes' own accuracy is included at full strength because a session's fixes
 * share a base-station error that does not average out). It is reported as-is;
 * the app does not recombine it.
 */
@Serializable
data class GeorefRecord(
    val converged: Boolean,
    val epsg: Int,
    val yawDeg: Double,
    /** ROW-MAJOR 4×4, `se3.h` convention — the same convention every matrix crossing the JNI uses. */
    val globalFromLocal: DoubleArray,
    /** The ENU tangent frame [globalFromLocal] maps into: lat/lon/height of its origin. */
    val enuOriginLatDeg: Double,
    val enuOriginLonDeg: Double,
    val enuOriginHeightM: Double,
    val horizontalSigmaM: Double,
    val verticalSigmaM: Double,
    val cep95M: Double,
    val samples: Int,
    val inliers: Int,
    val residualRmsM: Double,
    val spanM: Double,
    val dominantFix: FixType,
    /** Empty when converged; A10's stable reason string otherwise ("trajectory too short to observe heading", …). */
    val blocker: String,
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is GeorefRecord) return false
        return converged == other.converged && epsg == other.epsg &&
            abs(yawDeg - other.yawDeg) == 0.0 &&
            globalFromLocal.contentEquals(other.globalFromLocal) &&
            enuOriginLatDeg == other.enuOriginLatDeg && enuOriginLonDeg == other.enuOriginLonDeg &&
            enuOriginHeightM == other.enuOriginHeightM && samples == other.samples &&
            blocker == other.blocker
    }

    override fun hashCode(): Int = 31 * epsg + globalFromLocal.contentHashCode()

    val epsgText: String get() = if (epsg != 0) "EPSG:$epsg" else "local frame (no CRS)"
}

/** Formats a distance in metres at a sensible precision for a status readout. */
internal fun formatMetres(m: Double): String = when {
    abs(m) < 1.0 -> "%.0f mm".format(m * 1000.0)
    abs(m) < 10.0 -> "%.2f m".format(m)
    else -> "%.1f m".format(m)
}
