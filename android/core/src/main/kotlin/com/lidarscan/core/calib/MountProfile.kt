package com.lidarscan.core.calib

import kotlinx.serialization.Serializable

/**
 * ROUND 20 (item 82) — **the per-device mount profile: no hard-coded geometry
 * for a public app.**
 *
 * The owner's mandate, verbatim intent: his D6 sits toward the MIDDLE of the
 * phone back — not near the top edge the CAD placeholder assumed — and every
 * public user's mount position and orientation may differ. So nothing about
 * the mount is hard-coded any more:
 *
 *  * **rotation** — measured per scan by the Start hold (items 78/79), stored
 *    as the gravity-referenced [MountTrim];
 *  * **lever arm** — this class: three centimetre offsets the user can edit in
 *    Settings, describing where the lidar's optical centre sits RELATIVE TO
 *    THE REAR CAMERA. Defaults are the previous placeholder values, so an
 *    existing rig sees no change until its owner types better numbers;
 *  * **convention** — the one thing that stays documented rather than
 *    measured: the 0-degree mark points up and the cap faces the walk
 *    direction (owner-confirmed, round 9). A static hold cannot observe it
 *    and the Settings schematic states it instead of pretending to know.
 *
 * ## The frame the three numbers live in
 *
 * `phone_from_lidar`'s translation is the lidar origin expressed in the
 * ARCore camera frame (+X right in the image, +Y up in the image, −Z the look
 * direction). The Settings fields are deliberately in RIG words, not axis
 * words, and map as:
 *
 * ```
 *   up      cm above the rear camera (toward the phone's top edge) -> −Y · up
 *   behind  cm behind the camera (away from the scene)             -> −Z · behind
 *   right   cm toward the operator's right when scanning           -> +X · right
 * ```
 *
 * The sign of `up` is worth a sentence: in a portrait scanning hold the
 * camera's image-up (+Y) points along gravity-down on this sensor layout, so
 * "above the camera" in hand is −Y in the image frame — which is exactly why
 * the round-9 placeholder was `t = (0, −0.060, −0.035)`: 6 cm up, 3.5 cm
 * behind. [DEFAULT] reproduces that placeholder to the millimetre.
 *
 * Rotation-round accuracy note (why centimetre fields are enough): round 20
 * measured the lever arm's whole contribution on the owner's walking-pace
 * scans at ≤ 1.5 mm of map change — rotation is the error that matters, and
 * it is measured, not typed.
 */
@Serializable
data class MountLeverArm(
    val upCm: Double = DEFAULT_UP_CM,
    val behindCm: Double = DEFAULT_BEHIND_CM,
    val rightCm: Double = DEFAULT_RIGHT_CM,
    /**
     * Where these numbers came from — `"default"`, `"user"` — shown beside the
     * fields so a typed value is never mistaken for a measured one.
     */
    val provenance: String = "default",
    val updatedAtEpochMillis: Long = 0L,
) {
    /** The `phone_from_lidar` translation these offsets encode, metres. */
    val translation: Vec3
        get() = Vec3(rightCm / 100.0, -upCm / 100.0, -behindCm / 100.0)

    /** True when these are exactly the shipped defaults. */
    val isDefault: Boolean
        get() = upCm == DEFAULT_UP_CM && behindCm == DEFAULT_BEHIND_CM && rightCm == DEFAULT_RIGHT_CM

    /**
     * [nominal] with its translation replaced by this lever arm. The rotation
     * block is untouched — rotation belongs to the trim (items 78/79).
     */
    fun appliedTo(nominal: Mat4): Mat4 {
        val m = nominal.m.copyOf()
        val t = translation
        m[3] = t.x
        m[7] = t.y
        m[11] = t.z
        return Mat4(m)
    }

    /** One `key=value` run for the capture log, same shape as every other. */
    val logSuffix: String
        get() = "leverUpCm=%.1f leverBehindCm=%.1f leverRightCm=%.1f leverSource=%s"
            .format(upCm, behindCm, rightCm, provenance)

    companion object {
        // The previous CAD placeholder, now a DEFAULT rather than a truth:
        // BracketNominals.cadNominal(COIN_D6) carried (0, -0.060, -0.035).
        const val DEFAULT_UP_CM = 6.0
        const val DEFAULT_BEHIND_CM = 3.5
        const val DEFAULT_RIGHT_CM = 0.0

        /** Settings clamps to this — half a phone plus a hand of bracket. */
        const val MAX_ABS_CM = 30.0

        val DEFAULT = MountLeverArm()

        fun clamped(upCm: Double, behindCm: Double, rightCm: Double, nowMillis: Long) =
            MountLeverArm(
                upCm = upCm.coerceIn(-MAX_ABS_CM, MAX_ABS_CM),
                behindCm = behindCm.coerceIn(-MAX_ABS_CM, MAX_ABS_CM),
                rightCm = rightCm.coerceIn(-MAX_ABS_CM, MAX_ABS_CM),
                provenance = "user",
                updatedAtEpochMillis = nowMillis,
            )
    }
}

// The per-device MOUNT PROFILE, as a whole, is the trio Settings shows and
// the capture reads: the persisted [StoredMountTrim] (rotation, measured at
// every Start by the hold-steady stage), this [MountLeverArm] (translation,
// typed), and the auto-level suggestion string (item 80's channel, persisted
// beside them with provenance). Deliberately NOT wrapped in one more data
// class: the three have three different writers and three different
// lifetimes, and a wrapper would only exist to be unwrapped.
