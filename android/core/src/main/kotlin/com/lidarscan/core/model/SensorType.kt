package com.lidarscan.core.model

import kotlinx.serialization.Serializable

/**
 * The two sensors LidarScan supports (Tech Spec §2). Persisted verbatim in
 * `manifest.json`, so renaming an entry is a schema-breaking change — bump
 * [ProjectManifest.CURRENT_SCHEMA_VERSION] and add a migration if that's
 * ever needed.
 */
@Serializable
enum class SensorType(val displayName: String, val badgeLabel: String) {
    COIN_D6(displayName = "COIN-D6", badgeLabel = "D6"),
    MID360(displayName = "Livox Mid-360", badgeLabel = "Mid-360"),
}
