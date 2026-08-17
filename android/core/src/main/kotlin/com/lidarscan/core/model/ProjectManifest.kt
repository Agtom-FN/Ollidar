package com.lidarscan.core.model

import com.lidarscan.core.calib.MountCalibration
import com.lidarscan.core.gnss.GeorefRecord
import com.lidarscan.core.net.Mid360Settings
import com.lidarscan.core.render.DisplayParams
import kotlinx.serialization.Serializable

/**
 * Persisted as `<project>.lscan/manifest.json` (Tech Spec §3.11).
 *
 * Only the fields B1 can actually populate are non-null: [sensor], [profile],
 * [createdAtEpochMillis], [appVersion], [schemaVersion]. The spec's container
 * diagram also lists "mount calib, CRS" on the manifest — those arrive with
 * B7 (mount-calibration wizard) and A10 (GNSS/RTK + CRS); the nullable fields
 * below reserve their place so adding them later is not a schema-breaking
 * change. [pointCountEstimate] is the "point-count placeholder from
 * manifest" the Projects list reads: B1 leaves it null (no capture pipeline
 * yet); B4/B6 are expected to fill it in as capture/processing progress.
 */
@Serializable
data class ProjectManifest(
    val schemaVersion: Int = CURRENT_SCHEMA_VERSION,
    val name: String,
    val sensor: SensorType,
    val profile: WorkflowProfile,
    val createdAtEpochMillis: Long,
    val appVersion: String,
    val pointCountEstimate: Long? = null,
    val mountCalibrationId: String? = null,
    val crsEpsg: Int? = null,
    /**
     * B7: the full mount calibration used for this project, not just its id.
     * WIZARD.md §3 wants "the extrinsic, its split-half gate value, the
     * estimated time offset, target size, pose count, sensor serial, bracket
     * ID, timestamp, and app version" *in the manifest* — because a `.lscan`
     * opened on a desktop that has never seen this phone still has to be
     * colorizable, and a bare id pointing into a device-local store would not
     * survive the trip. [mountCalibrationId] stays as the cross-reference
     * back into that device-level store
     * ([com.lidarscan.core.calib.MountCalibrationStore]); this is the copy.
     */
    val mountCalibration: MountCalibration? = null,
    /**
     * B3: the Mid-360's network configuration for this project (Tech Spec
     * §3.1's "Save per project"). Null for a D6 project, and null for a
     * Mid-360 project whose wizard has not been run yet.
     *
     * In the manifest rather than a device-level store, and the split from
     * [mountCalibration] is deliberate: a mount calibration belongs to the
     * *bracket* (WIZARD.md §3) and follows the phone, but a lidar IP / host
     * IP pair belongs to the **site** — the same phone on a different job
     * with a different switch needs different addresses, and the same
     * `.lscan` re-opened on that site should offer the ones that worked. It
     * is also the record of what a capture was actually taken with, which is
     * the first thing anyone asks when a `.lscan` turns out to be empty.
     */
    val mid360: Mid360Settings? = null,
    /**
     * B5: the capture-side defaults this project was **created** with (Tech
     * Spec §3.9's "profiles set defaults").
     *
     * Stored on the project rather than re-derived from [profile] on every read
     * for one reason that matters: a profile's defaults are a *starting point*
     * the operator may then change per project, and re-deriving would silently
     * throw those changes away. It also means that if a later app version
     * retunes what "Survey" means, an existing capture keeps the settings it was
     * actually taken with — which is the same argument [mid360] makes for
     * addresses and [mountCalibration] makes for the extrinsic.
     *
     * Null only for a project created before B5; readers should fall back to
     * `CaptureDefaults.forProfile(profile)`, which
     * [effectiveCaptureDefaults] does.
     */
    val captureDefaults: CaptureDefaults? = null,
    /**
     * B10: §3.9's "settings persist per project". Seeded from the profile's
     * [CaptureDefaults.displayProfile] at creation; every later edit in the
     * display bottom-sheet writes back here.
     */
    val displayParams: DisplayParams? = null,
    /**
     * B9/B12: A10's georeferencing solution snapshotted at capture stop.
     *
     * A10 §9.6 asks for exactly this ("a periodic GeorefSolution + origin
     * snapshot in the manifest so a replay does not have to re-derive the
     * alignment"). B12's auto-merge is the consumer: `merge/session.h` needs
     * each session's `global_from_local` **and the ENU frame it is expressed
     * in**, and neither survives the end of a capture any other way.
     */
    val georef: GeorefRecord? = null,
    /**
     * ROUND 6 (owner item 23): the session's **mount trim** — the one-tap
     * re-zero of how the D6 actually sat on the phone for THIS scan, composed
     * on top of [com.lidarscan.core.calib.BracketNominals.cadNominal].
     *
     * On the project rather than the device-level calibration store for the
     * same reason [mid360] is: the bracket's geometry belongs to the bracket,
     * but "how it was clamped on this morning" belongs to the capture. Post-
     * processing has to use the same trim the live pushbroom used or the two
     * disagree, so it travels in the `.lscan`.
     */
    val mountTrim: com.lidarscan.core.calib.MountTrim? = null,
    /**
     * ROUND 6 (owner item 20): true when this manifest was **rebuilt** by
     * [com.lidarscan.core.store.FileProjectStore] from a capture whose app-side
     * metadata had been destroyed by the pre-0.3.0 `manifest.json` filename
     * collision with the engine's own container manifest.
     *
     * The streams are intact; the name/sensor/profile are the store's best
     * honest reconstruction. Surfaced in the UI rather than hidden, because a
     * recovered project's *metadata* deserves less trust than its points.
     */
    val recovered: Boolean = false,
) {
    /** The project's own capture defaults, or the profile's if it predates B5. */
    fun effectiveCaptureDefaults(): CaptureDefaults =
        captureDefaults ?: CaptureDefaults.forProfile(profile)

    /** The project's own display parameters, or the profile's A14 preset if it predates B10. */
    fun effectiveDisplayParams(): DisplayParams =
        displayParams ?: com.lidarscan.core.render.profileDefaults(effectiveCaptureDefaults().displayProfile)

    companion object {
        /**
         * Bump when a field is added/removed/renamed in a way old readers can't
         * tolerate. B5–B12's additions are all **nullable and additive**, and
         * `ignoreUnknownKeys = true` is set on the decoder, so a manifest
         * written by either side still reads on the other — which is why this
         * stays 1.
         */
        const val CURRENT_SCHEMA_VERSION = 1
    }
}
