package com.lidarscan.core.gnss

/**
 * Where this capture's georeference is coming from, right now.
 *
 * ROUND 5.2: a capture with no RTK rover attached is no longer un-georeferenced
 * by default — the phone's own location is used instead, at its own honest
 * accuracy. The two sources are strictly ranked rather than blended: an RTK
 * epoch is centimetres from a survey-grade receiver, a phone fix is metres from a
 * consumer chip, and averaging them would produce a number that describes
 * neither.
 */
enum class GeorefSource {
    /** Nothing usable yet — no rover, and no phone fix (or no permission). */
    NONE,

    /** A connected RTK rover is publishing fixes. Always preferred when present. */
    RTK_ROVER,

    /** Fallback: the phone's fused/GPS location, recorded at `Location.getAccuracy()`. */
    PHONE_GPS,
}

/**
 * What the capture screen's inline georeference chip shows, and whether the phone
 * fallback should be running.
 *
 * [chipLabel] is deliberately built here rather than in the composable: the same
 * words appear in the capture chip strip and in the diagnostics sheet, and the
 * rule "never quote an accuracy better than the source measured" is a policy, not
 * a formatting detail.
 */
data class GeorefSourceState(
    val source: GeorefSource = GeorefSource.NONE,
    val chipLabel: String = "NO GEOREF",
    /** 1-sigma horizontal metres for whichever source is active, or null when there is none. */
    val accuracyM: Float? = null,
    /** True when fine-location permission was asked for and refused. */
    val permissionDenied: Boolean = false,
    /** True when a capture session is running, which is the only time the fallback is armed. */
    val sessionActive: Boolean = false,
) {
    val isRtk: Boolean get() = source == GeorefSource.RTK_ROVER
    val isPhoneFallback: Boolean get() = source == GeorefSource.PHONE_GPS
}

/**
 * The ranking rule, as one pure function so the three paths the owner asked to be
 * tested (rover present / phone fix / permission denied) are testable without a
 * device.
 */
object GeorefSourcePolicy {

    /**
     * True when the phone-location fallback should be streaming: a capture is
     * running, no rover fix is present, and the permission has not been refused.
     *
     * Note what this does **not** say: it does not require the permission to be
     * granted *yet* — the caller uses this to decide whether to ask, and asking
     * only ever happens once a capture actually starts (round 5.2: "only requested
     * when a capture actually starts and no RTK is present").
     */
    fun shouldRunPhoneFallback(
        rtkFix: GnssFixSnapshot?,
        sessionActive: Boolean,
        permissionDenied: Boolean,
    ): Boolean = sessionActive && !permissionDenied && !hasRtkFix(rtkFix)

    /**
     * Resolves the active source and its chip.
     *
     * A rover that connects mid-session simply wins on the next resolve — the
     * chip upgrades from `PHONE GPS ±4 m` to `RTK FIXED ±2 cm` and the fallback
     * stops being armed. That is the whole of the "switching" behaviour, on
     * purpose: no hysteresis, no hand-over state machine, nothing to get stuck in.
     */
    fun resolve(
        rtkFix: GnssFixSnapshot?,
        phoneFix: PhoneFix?,
        sessionActive: Boolean,
        permissionDenied: Boolean,
    ): GeorefSourceState {
        if (hasRtkFix(rtkFix)) {
            val fix = rtkFix!!
            val sigma = fix.sigmaHorizontalM.takeIf { it > 0f }
            return GeorefSourceState(
                source = GeorefSource.RTK_ROVER,
                chipLabel = buildString {
                    append(fix.fix.label.uppercase())
                    if (sigma != null) append(" ±").append(formatAccuracy(sigma))
                },
                accuracyM = sigma,
                permissionDenied = permissionDenied,
                sessionActive = sessionActive,
            )
        }

        if (phoneFix != null) {
            val sigma = phoneFix.accuracyM.takeIf { it.isFinite() && it > 0f }
            return GeorefSourceState(
                source = GeorefSource.PHONE_GPS,
                // "PHONE GPS", never "GNSS" or a bare fix label: the operator has
                // to be able to tell at a glance that this scan is metre-accurate
                // and not centimetre-accurate.
                chipLabel = buildString {
                    append("PHONE GPS")
                    if (sigma != null) append(" ±").append(formatAccuracy(sigma)) else append(" · accuracy unknown")
                },
                accuracyM = sigma,
                permissionDenied = permissionDenied,
                sessionActive = sessionActive,
            )
        }

        val label = when {
            permissionDenied -> "NO GEOREF · location off"
            sessionActive -> "WAITING FOR GPS"
            else -> "NO GEOREF"
        }
        return GeorefSourceState(
            source = GeorefSource.NONE,
            chipLabel = label,
            accuracyM = null,
            permissionDenied = permissionDenied,
            sessionActive = sessionActive,
        )
    }

    /**
     * The quiet note under a denied permission. Round 5.2 is explicit that denial
     * must never block a capture — the scan proceeds, it is simply not
     * georeferenced, and it says so once instead of asking again.
     */
    const val PERMISSION_DENIED_NOTE: String =
        "Recording without a georeference — location permission is off, so this scan keeps its own local frame. " +
            "Grant location in Android settings if you want phone-GPS georeferencing."

    private fun hasRtkFix(fix: GnssFixSnapshot?): Boolean =
        fix != null && fix.hasFix && fix.fix != FixType.NONE

    /** Centimetres under a metre, metres above it — the same rule the capture chip already used. */
    fun formatAccuracy(sigmaM: Float): String =
        if (sigmaM < 1f) "%.0f cm".format(sigmaM * 100) else "%.1f m".format(sigmaM)
}
