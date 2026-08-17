package com.lidarscan.core.store

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * ROUND 6 (owner item 20 — "the capture not saved to the phone, it just gone
 * and the app project not see any saved"): the ENGINE's `.lscan` manifest, as a
 * read-only shape.
 *
 * ### Why this type has to exist
 *
 * `engine/src/record/lscan.cpp`'s `FileRecordWriter::open()` writes its own
 * `manifest.json` into the capture directory the moment a session starts, and
 * writes it again (with `"sealed": true`) at close. Tech Spec §3.11 is on the
 * engine's side here — `manifest.json` inside a `.lscan` is *the container's*
 * manifest ("sensor, profile, mount calib, CRS, versions"), and the engine owns
 * that file.
 *
 * The Android app had been writing a **completely different schema** to the
 * same path ([com.lidarscan.core.model.ProjectManifest]) since B1. Nothing
 * caught it, because the two hardware-free paths this app can test on never
 * collide: `ReplayEngineBridge` documents that it ignores the project directory
 * entirely, and `FakeEngineBridge` writes no files at all. The FIRST time the
 * two writers meet is a real capture on real hardware — at which point the
 * engine's manifest replaces the app's, `FileProjectStore.readProject()` can no
 * longer decode it (the app schema's `name`, `sensor`, `createdAtEpochMillis`
 * and `appVersion` have no defaults, and `profile` is an enum where the engine
 * writes `"quickscan"`), `list()` skips the directory, and every later
 * `updateManifest()` silently returns null. The capture's bytes are on disk and
 * the project is invisible: exactly the field report.
 *
 * The fix is that the app stops squatting on the engine's filename — see
 * [FileProjectStore.APP_MANIFEST_FILE_NAME] — and this type is what lets an
 * already-clobbered project be **recovered** instead of staying lost.
 *
 * Every field is optional with a default: this parses whatever the engine wrote,
 * including future versions of it, and never throws.
 */
@Serializable
data class EngineLscanManifest(
    val schemaVersion: Int = 0,
    val formatVersion: Int = 0,
    val engineVersion: String = "",
    /** Wall-clock nanoseconds at `FileRecordWriter::open()`. */
    val createdAtUtcNs: Long = 0,
    val sealed: Boolean = false,
    val sealedAtUtcNs: Long = 0,
    /** `survey | floorplan | research | quickscan` — the engine session profile string. */
    val profile: String = "",
    val sensors: List<EngineSensorEntry> = emptyList(),
    /** Present as a JSON object; only its key set is interesting here. */
    @SerialName("streams") val streamFiles: Map<String, EngineStreamEntry> = emptyMap(),
) {
    /**
     * True for a document that is plausibly the engine's rather than a
     * truncated/foreign file. `createdAtUtcNs` is written unconditionally by
     * `write_manifest()` and `formatVersion` is a compile-time constant there,
     * so requiring one of them keeps an empty `{}` from being "recovered" into
     * a project that never existed.
     */
    val looksLikeEngineManifest: Boolean
        get() = formatVersion > 0 || createdAtUtcNs > 0L || engineVersion.isNotEmpty()
}

@Serializable
data class EngineSensorEntry(
    val id: String = "",
    val kind: String = "",
    val model: String = "",
)

@Serializable
data class EngineStreamEntry(val stream: Int = 0)
