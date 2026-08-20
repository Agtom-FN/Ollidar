package com.lidarscan.core.calib

import com.lidarscan.core.model.SensorType
import kotlinx.serialization.Serializable

/**
 * The wizard's verdict bands — WIZARD.md screen 4, and the same numbers the
 * engine mirrors as `SCAN_CALIB_GATE_*`.
 *
 * The gate metric is **split-half agreement**, never the solver's covariance.
 * A8 §4.3 re-measured why: over 21 sessions differing only in their noise
 * realisation, the true rotation error's coefficient of variation was 1.83
 * while the reported sigma's was 0.13 — "the covariance is essentially
 * constant while the thing it claims to describe varies by a factor of nearly
 * two either way. It cannot rank sessions, which is the entire job of a gate."
 */
@Serializable
enum class CalibrationGate(val engineValue: Int) {
    UNKNOWN(0),
    GOOD(1),
    USABLE(2),
    REJECT(3),
    ;

    companion object {
        fun fromEngine(value: Int): CalibrationGate =
            entries.firstOrNull { it.engineValue == value } ?: UNKNOWN
    }
}

/**
 * The gate rendered the way WIZARD.md insists it is shown: "in physical
 * terms, never in pixels".
 *
 * The conversion is the wizard's own: the split-half number is quoted at
 * `gate_range_m` (3 m), so `mm = px * 1000 * range / focal_px`. The wizard's
 * example — "±5 mm at 3 m — Good" against a ~12 px band — implies a focal
 * length near 1460 px at 1280x720, which is what a modern phone main camera
 * actually is; that is used as the fallback when the session's own intrinsics
 * are not to hand, and the real `fx` is used when they are.
 */
data class GateReadout(val gate: CalibrationGate, val splitHalfPx: Double, val millimetresAt3m: Double) {

    val headline: String
        get() = when (gate) {
            CalibrationGate.GOOD -> "Mount alignment: ±%.0f mm at 3 m — Good".format(millimetresAt3m)
            CalibrationGate.USABLE -> "Mount alignment: ±%.0f mm at 3 m — Usable".format(millimetresAt3m)
            CalibrationGate.REJECT -> "Not accurate enough"
            CalibrationGate.UNKNOWN -> "Not enough poses to judge"
        }

    val detail: String
        get() = when (gate) {
            CalibrationGate.GOOD -> "Colours will land within about a fingernail's width at room distance."
            CalibrationGate.USABLE -> "Usable, but colours may smear on edges."
            CalibrationGate.REJECT -> "Redo the capture — the poses were not varied enough, or the lidar could not see the board."
            CalibrationGate.UNKNOWN -> "Capture at least five usable poses."
        }

    companion object {
        const val DEFAULT_FOCAL_PX = 1460.0
        const val GATE_RANGE_M = 3.0

        fun of(gate: CalibrationGate, splitHalfPx: Double, focalPx: Double = DEFAULT_FOCAL_PX): GateReadout {
            val mm = if (splitHalfPx < 0 || focalPx <= 0) {
                Double.NaN
            } else {
                splitHalfPx * 1000.0 * GATE_RANGE_M / focalPx
            }
            return GateReadout(gate, splitHalfPx, mm)
        }
    }
}

/**
 * A solved mount calibration, as persisted.
 *
 * WIZARD.md §3 lists exactly what to store — "the extrinsic, its split-half
 * gate value, the estimated time offset, target size, pose count, sensor
 * serial, bracket ID, timestamp, and app version" — and adds the rule that
 * matters more than the field list: **"Calibration belongs to the bracket,
 * not the project."** So this record is written in two places: into the
 * project's `manifest.json` (so a `.lscan` is self-describing wherever it is
 * opened) and into a device-level store keyed by [bracketId] + [sensorSerial]
 * (so the user calibrates once, not once per scan). See
 * [com.lidarscan.core.calib.MountCalibrationStore].
 *
 * `cameraFromLidar` is ROW-major, 16 doubles — the layout
 * `scan_engine_set_mount_extrinsics` requires and the only layout stored
 * anywhere in this app.
 */
@Serializable
data class MountCalibration(
    val id: String,
    val sensor: SensorType,
    val bracketId: String,
    val sensorSerial: String? = null,
    val cameraFromLidar: DoubleArray,
    val splitHalfPx: Double,
    val gate: CalibrationGate,
    val gateRangeM: Double = GateReadout.GATE_RANGE_M,
    val poseCount: Int,
    val squareSizeM: Double,
    val boardCols: Int,
    val boardRows: Int,
    /** The constant camera<->lidar clock offset from WIZARD.md screen 3, nanoseconds; null when the sweep was skipped. */
    val clockOffsetNs: Long? = null,
    val rmsResidualM: Double = 0.0,
    /** Diagnostics only — NEVER gate on these (A8 §4.3). Stored so a bench pass can look at them later. */
    val sigmaRotDeg: Double = 0.0,
    val sigmaTransMm: Double = 0.0,
    val conditionNumber: Double = 0.0,
    val createdAtEpochMillis: Long,
    val appVersion: String,
    val phoneModel: String,
) {
    init {
        require(cameraFromLidar.size == 16) { "cameraFromLidar must be a 16-element row-major 4x4" }
    }

    val matrix: Mat4 get() = Mat4(cameraFromLidar.copyOf())

    fun readout(focalPx: Double = GateReadout.DEFAULT_FOCAL_PX): GateReadout =
        GateReadout.of(gate, splitHalfPx, focalPx)

    /** The device-level store's key: WIZARD.md §3's "(phone model, bracket ID, lidar serial)". */
    fun storeKey(): String = storeKey(phoneModel, bracketId, sensorSerial)

    override fun equals(other: Any?): Boolean =
        other is MountCalibration && id == other.id &&
            cameraFromLidar.contentEquals(other.cameraFromLidar)

    override fun hashCode(): Int = 31 * id.hashCode() + cameraFromLidar.contentHashCode()

    companion object {
        fun storeKey(phoneModel: String, bracketId: String, sensorSerial: String?): String =
            listOf(phoneModel, bracketId, sensorSerial ?: "-")
                .joinToString("|") { it.replace('|', '/') }
    }
}

/**
 * The bracket's CAD nominal — the initial guess every solve starts from, on
 * both halves of the split-half gate ("both seeded from the same CAD nominal,
 * because in the field that is the only initial guess there is", A8 §4.3).
 *
 * ## The COIN-D6 fan frame (ROUND 9, owner item 34)
 *
 * The authority for this is `engine/include/scanengine/drivers/d6/d6_fan.h`,
 * which derives it in full. In short, the fan frame is **right-handed** and
 * pinned to the physical unit as:
 *
 *  * **+y** — the 0-degree beam direction, i.e. the zero mark on the housing.
 *  * **+z** — the spin axis, pointing out of the **BASE** of the unit, away
 *    from the cap / optical-window end.
 *  * **+x** — `y × z`, which completes the right-handed triple.
 *
 * and a return at vendor angle `θ`, range `d`, lands at
 *
 * ```
 * p_lidar = (-d·sin θ,  d·cos θ,  0)
 * ```
 *
 * **The `x` sign is negative, and that is the ROUND 9 fix.** This KDoc used to
 * say the axes were "aligned with the sensor-frame convention A8 §3.1 fixes
 * (`x = d·sinθ, y = d·cosθ, z = 0`)". That formula is **wrong**: the vendor
 * datasheet states its angle convention in a *left-handed* coordinate system
 * ("left-hand coordinate system … rotation angle increases clockwise", quoted
 * in `docs/bench/BENCH_SETUP.md` §3.1), and the engine transcribed the
 * datasheet's `(x, y)` verbatim into a right-handed frame. A left-handed
 * triple read as right-handed silently reverses the sweep direction about the
 * spin axis, so the resolved cloud came out **left-right mirrored** — the
 * owner's "the output is left right reversed".
 *
 * ## Why identity is the right rotation for the owner's mount
 *
 * The owner's authoritative rig: the D6 rides on the **back of the phone**;
 * the **0-degree beam points UP** (phone held portrait); the **cap/top of the
 * lidar faces FORWARD** along the walk direction. Under the frame above, and
 * with ARCore's camera frame (+X right, +Y up, looking along −Z):
 *
 * ```
 * lidar +y (0-deg beam, up)   = camera +Y  (up)
 * lidar +z (out of the BASE)  = camera +Z  (backward — so the CAP faces forward)
 * lidar +x                    = camera +X  (the operator's right)
 * ```
 *
 * Axis for axis, so `phone_from_lidar` carries an **IDENTITY rotation**, which
 * is exactly what [cadNominal] has always had. The CAD nominal's rotation was
 * never the wrong part; formula (1) was. It is therefore no longer a
 * placeholder — it is a derived, owner-confirmed fact, and a change to it needs
 * a changed physical mount, not a tweak. `D6ChiralityTest` pins it.
 *
 * ## What IS still a placeholder
 *
 * Only the **TRANSLATION**. No physical bracket exists yet (Tech Spec's
 * hardware-absent addendum), so the lever arms below encode only the intended
 * geometry — the lidar sitting a few centimetres above and behind the phone's
 * camera. A real bracket ships its own numbers here; the wizard's whole job is
 * to recover the difference.
 */
object BracketNominals {

    const val DEFAULT_BRACKET_ID = "reference-v1"

    /** `phone_from_lidar` (== `camera_from_lidar`), ROW-major. */
    fun cadNominal(sensor: SensorType): Mat4 = when (sensor) {
        // D6: pushbroom bracket, scanner directly above the camera, its scan
        // plane vertical (so the sweep is across the walk direction).
        // Rotation: IDENTITY, derived above from the owner's mount and the
        // right-handed fan frame of d6_fan.h — do not "fix" it. Translation:
        // still a CAD placeholder (6 cm up, 3.5 cm behind the camera).
        SensorType.COIN_D6 -> Mat4(
            doubleArrayOf(
                1.0, 0.0, 0.0, 0.000,
                0.0, 1.0, 0.0, -0.060,
                0.0, 0.0, 1.0, -0.035,
                0.0, 0.0, 0.0, 1.0,
            ),
        )
        // ROUND 25 item 119 — STL-27L: the SAME nominal as the D6, on purpose.
        //
        // The CAD nominal encodes the BRACKET, not the sensor's internals: the
        // reference-v1 mount has one lidar seat, directly above the camera,
        // scan plane vertical. An STL-27L bolted into that seat therefore has
        // the D6's rotation (identity, derived above and pinned by
        // `D6ChiralityTest` — the LD-series fan is right-handed about the same
        // spin axis) and the D6's lever arm. Inventing a different translation
        // here would be inventing a bracket that does not exist; the two
        // sensors' bodies differ by millimetres and the wizard's whole job is
        // to recover that difference from measurement rather than from a guess
        // typed into this file.
        //
        // UNVERIFIED, like the D6's translation beside it: no physical bracket
        // and no STL-27L hardware exist. Kept as a separate branch rather than
        // folded in with the D6 so that a real STL-27L seat, when one is cut,
        // has an obvious place to land.
        SensorType.STL27L -> Mat4(
            doubleArrayOf(
                1.0, 0.0, 0.0, 0.000,
                0.0, 1.0, 0.0, -0.060,
                0.0, 0.0, 1.0, -0.035,
                0.0, 0.0, 0.0, 1.0,
            ),
        )
        // Mid-360: heavier, sits further back on the handle.
        SensorType.MID360 -> Mat4(
            doubleArrayOf(
                1.0, 0.0, 0.0, 0.000,
                0.0, 1.0, 0.0, -0.090,
                0.0, 0.0, 1.0, -0.070,
                0.0, 0.0, 0.0, 1.0,
            ),
        )
    }
}
