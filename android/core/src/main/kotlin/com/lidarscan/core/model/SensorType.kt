package com.lidarscan.core.model

import kotlinx.serialization.Serializable

/**
 * The sensors LidarScan supports — Tech Spec §2's two, plus ROUND 25 item
 * 119's LDROBOT STL-27L.
 *
 * ## The name is the wire format
 *
 * kotlinx.serialization writes an enum by its **declared name**, not its
 * ordinal, and this enum is persisted verbatim in `manifest.json` (`"sensor":
 * "COIN_D6"`). So *renaming* an entry — or reordering, which is harmless, or
 * deleting one, which is not — is a schema-breaking change: bump
 * [ProjectManifest.CURRENT_SCHEMA_VERSION] and add a migration if that is ever
 * needed. Nothing here may be renamed for cosmetic reasons; [displayName] and
 * [badgeLabel] exist precisely so the screen text can change without the
 * stored value moving.
 *
 * ## Why ADDING [STL27L] did NOT bump the schema version
 *
 * Stated here rather than assumed, because "we added a sensor, surely that is
 * a schema change" is the obvious wrong answer:
 *
 *  * **Old manifest, new reader** — the only direction the app actually
 *    performs. Every manifest ever written contains `COIN_D6` or `MID360`, and
 *    both still decode to the same constants. Adding a third value cannot make
 *    an existing document unreadable, because a decoder matches the string it
 *    finds; it does not enumerate what it could have found.
 *  * **New manifest, old reader** — a scan sealed by 0.9.10 opened by an
 *    earlier build. That one *does* fail (an unknown enum value throws;
 *    `ignoreUnknownKeys = true` forgives unknown *keys*, never unknown enum
 *    *values*). But bumping `CURRENT_SCHEMA_VERSION` would not rescue it:
 *    nothing in this codebase reads `schemaVersion` before decoding — the
 *    field is decoded as part of the same document, and the only consumer is
 *    a read-only row on the project detail screen. The old build would throw
 *    on the sensor value either way, and the bump would additionally mark
 *    every already-written manifest as stale for no benefit.
 *
 * So the version stays 1, which is the same rule [ProjectManifest]'s companion
 * already states for its nullable additive fields: bump when an old reader
 * genuinely cannot cope AND the number would tell it so.
 */
@Serializable
enum class SensorType(val displayName: String, val badgeLabel: String) {
    COIN_D6(displayName = "COIN-D6", badgeLabel = "D6"),
    MID360(displayName = "Livox Mid-360", badgeLabel = "Mid-360"),

    /**
     * ROUND 25 item 119. A 360° DTOF serial lidar (UART 921600 8N1, LD-series
     * 47-byte packets) reached over the SAME USB-serial path the COIN-D6 uses.
     *
     * It is a **2-D pushbroom sensor exactly like the D6**: no IMU of its own,
     * so the phone's ARCore pose IS the trajectory, the same mount trim
     * applies, and the same hold-still flow gates the start. Wherever this
     * codebase asks "what does this sensor need", the honest answer for the
     * STL-27L is almost always "whatever the D6 needs" — and the branches that
     * say so are written out explicitly rather than left to an `else`, so the
     * next sensor still breaks the build instead of silently inheriting.
     *
     * UNVERIFIED: no STL-27L hardware exists on this project. Everything the
     * app does with this value is protocol-derived from the public LD-series
     * references, not observed.
     */
    STL27L(displayName = "LDROBOT STL-27L", badgeLabel = "STL-27L"),
    ;

    /**
     * ROUND 25 item 119 — **"this sensor scans in one plane and has no IMU, so
     * the phone's ARCore pose IS the trajectory."**
     *
     * Added because adding [STL27L] exposed a whole class of code the compiler
     * could NOT flag. An added enum value breaks every `when` — the compiler is
     * relentless about those — but it says nothing at all about the dozen
     * places written as `sensor == SensorType.COIN_D6`, and *most of those were
     * never about the D6*. They were about a 2-D pushbroom lidar that needs the
     * pose pump running, needs the hold-still stage before Start, needs the
     * mount trim applied, and cannot run live SLAM. Left alone, an STL-27L
     * capture would have compiled cleanly and then recorded a flat fan of
     * points with no trajectory under it — the worst possible failure, because
     * it looks like it worked until somebody opens the scan.
     *
     * So the question those sites are really asking gets a name, and the sites
     * ask it by name. The ones that genuinely mean *the COIN-D6 specifically*
     * — the offline `reprocessD6` pipeline, which the engine keys to
     * `SCAN_STREAM_LIDAR_D6` — deliberately still say `== COIN_D6`, and that
     * difference is now visible in the source instead of being a coincidence.
     */
    val isPhoneTrackedPushbroom: Boolean
        get() = when (this) {
            COIN_D6 -> true
            STL27L -> true
            // The Mid-360 is 3-D and carries its own IMU: it runs LIO and poses
            // are an optional improvement, not the trajectory.
            MID360 -> false
        }

    /**
     * ROUND 25 item 119 — **which of the three identity tints this sensor
     * wears**, as an index rather than a colour.
     *
     * The colours themselves are `:app`'s (`SensorD6Badge` and friends in
     * `ui/theme/Color.kt`); `:core` has no Compose and must not grow one. What
     * `:core` CAN own is the exhaustiveness — and that is the half that was
     * actually broken. Four separate draw sites spell the tint as
     * `if (sensor == MID360) PoseBlue else ScanTeal`, and an `else` over an
     * enum is exactly the construct that let a third sensor arrive and be
     * silently painted as a COIN-D6: the label read "STL-27L" in the D6's
     * teal, which is worse than either being right, because a colour is what
     * the badge is scanned by at arm's length.
     *
     * Returning an ordinal-like index keeps the decision in one exhaustive
     * `when` that a new sensor breaks the build on, while leaving the palette
     * where the palette lives.
     */
    val badgeTint: BadgeTint
        get() = when (this) {
            COIN_D6 -> BadgeTint.D6
            MID360 -> BadgeTint.MID360
            STL27L -> BadgeTint.STL27L
        }

    /** The three sensor identity tints, named so `:app` can map them to colours. */
    enum class BadgeTint { D6, MID360, STL27L }
}
