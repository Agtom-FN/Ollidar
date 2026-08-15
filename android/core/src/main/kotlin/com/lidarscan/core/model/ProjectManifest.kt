package com.lidarscan.core.model

import com.lidarscan.core.calib.MountCalibration
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
) {
    companion object {
        /** Bump when a field is added/removed/renamed in a way old readers can't tolerate. */
        const val CURRENT_SCHEMA_VERSION = 1
    }
}
