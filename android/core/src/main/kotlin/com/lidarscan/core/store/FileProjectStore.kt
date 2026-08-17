package com.lidarscan.core.store

import com.lidarscan.core.model.CaptureDefaults
import com.lidarscan.core.model.ProjectManifest
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.render.profileDefaults
import java.io.File
import kotlin.random.Random
import kotlinx.serialization.json.Json

/**
 * Disk-backed [ProjectStore]. Deliberately takes a plain [rootDir] rather
 * than an Android `Context` so it runs under a bare-JVM unit test (see
 * `core/src/test/.../FileProjectStoreTest.kt`) with no emulator/Robolectric
 * — the `:app` DI-lite container is responsible for resolving that root to
 * an actual app-storage path on device.
 *
 * ## ROUND 6 (owner item 20) — the app no longer squats on `manifest.json`
 *
 * **This is the data-loss bug.** From B1 until 0.2.1 this class persisted the
 * app's [ProjectManifest] as `<project>.lscan/manifest.json`. That is the
 * ENGINE's file: `engine/src/record/lscan.cpp`'s `FileRecordWriter::open()`
 * writes its own, differently-shaped `manifest.json` there the moment
 * `scan_engine_start()` is called, and rewrites it (`"sealed": true`) at
 * `scan_engine_stop()`. So the first real capture into a project **destroyed
 * that project's app-side metadata**: [readProject] could no longer decode it
 * (the app schema requires `name`, `sensor`, `createdAtEpochMillis` and
 * `appVersion`, none of which the engine writes, and its `profile` is an enum
 * where the engine writes `"quickscan"`), so [list] skipped the directory and
 * every later [updateManifest] silently returned null. The capture's bytes were
 * on disk and the project was invisible in the app — the exact field report.
 *
 * Neither hardware-free test path could ever reproduce it: `ReplayEngineBridge`
 * documents that it ignores the project directory, and `FakeEngineBridge`
 * writes nothing to disk at all. The two writers only meet on a real device.
 *
 * The fix has three parts, all inside the Android app (the engine is read-only):
 *
 *  1. **The app's record moves to [APP_MANIFEST_FILE_NAME] (`project.json`)**,
 *     a filename the engine has no concept of. `manifest.json` goes back to
 *     being the engine's, exactly as Tech Spec §3.11 has it.
 *  2. **Legacy migration.** A project whose `manifest.json` still parses as the
 *     app schema (created before this change and never recorded into) is read
 *     as before and its metadata is copied to `project.json` on first read, so
 *     the next capture cannot destroy it.
 *  3. **Recovery of already-lost projects.** A project whose `manifest.json` is
 *     the ENGINE's — i.e. one of the owner's field captures that vanished — is
 *     rebuilt into a listable project from what the engine manifest and the
 *     directory name actually contain, and flagged
 *     [ProjectManifest.recovered] so the UI can say the metadata was
 *     reconstructed rather than pretending it was never lost.
 *
 * @param appVersion stamped into every manifest's `appVersion` field; passed
 *   in rather than read here because `:core` has no `BuildConfig` of its own.
 * @param onDiagnostic optional sink for one-line save/recovery events. Wired in
 *   `:app` to ROUND 6's on-device rolling capture log, so the NEXT field
 *   failure leaves evidence instead of a shrug.
 */
class FileProjectStore(
    private val rootDir: File,
    private val appVersion: String,
    private val clock: () -> Long = System::currentTimeMillis,
    private val onDiagnostic: (String) -> Unit = {},
) : ProjectStore {

    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
    }

    init {
        rootDir.mkdirs()
    }

    override fun list(): List<Project> {
        val dirs = rootDir.listFiles { file -> file.isDirectory && file.name.endsWith(PROJECT_SUFFIX) }
            ?: emptyArray()
        return dirs.mapNotNull(::readProject).sortedByDescending { it.manifest.createdAtEpochMillis }
    }

    override fun create(name: String, sensor: SensorType, profile: WorkflowProfile): Project {
        val dirName = uniqueDirName(name)
        val dir = File(rootDir, dirName)
        check(dir.mkdirs()) { "Could not create project directory: ${dir.absolutePath}" }

        File(dir, "streams").mkdirs()
        File(dir, "streams/frames").mkdirs()
        File(dir, "processed").mkdirs()
        File(dir, "merged").mkdirs()
        File(dir, "exports").mkdirs()

        // B5: the profile stops being a label here and becomes settings. Both
        // are written at creation and belong to the project from then on — see
        // ProjectManifest.captureDefaults for why they are not re-derived.
        val captureDefaults = CaptureDefaults.forProfile(profile)
        val manifest = ProjectManifest(
            name = name,
            sensor = sensor,
            profile = profile,
            createdAtEpochMillis = clock(),
            appVersion = appVersion,
            captureDefaults = captureDefaults,
            displayParams = profileDefaults(captureDefaults.displayProfile),
        )
        writeAppManifest(dir, manifest)
        onDiagnostic("project created: id=$dirName name=\"$name\" sensor=$sensor dir=${dir.absolutePath}")
        return Project(id = dirName, directory = dir, manifest = manifest)
    }

    override fun open(id: String): Project? {
        val dir = File(rootDir, id)
        if (!dir.isDirectory) return null
        return readProject(dir)
    }

    override fun delete(id: String): Boolean {
        val dir = File(rootDir, id)
        if (!dir.isDirectory) return false
        return dir.deleteRecursively()
    }

    override fun updateManifest(id: String, transform: (ProjectManifest) -> ProjectManifest): Project? {
        val dir = File(rootDir, id)
        if (!dir.isDirectory) {
            onDiagnostic("updateManifest FAILED: no such project directory id=$id")
            return null
        }
        val current = readProject(dir)
        if (current == null) {
            onDiagnostic("updateManifest FAILED: manifest unreadable for id=$id (dir=${dir.absolutePath})")
            return null
        }
        val updated = transform(current.manifest)
        return if (writeAppManifest(dir, updated)) {
            Project(id = id, directory = dir, manifest = updated)
        } else {
            onDiagnostic("updateManifest FAILED: could not write ${APP_MANIFEST_FILE_NAME} for id=$id")
            null
        }
    }

    /**
     * ROUND 6: the app-side manifest as it stands on disk RIGHT NOW, without
     * any of [readProject]'s migration/recovery. Used by the capture path's
     * post-seal verification, which has to be able to say "the sealed project
     * is genuinely readable back off the phone" rather than "a recovery
     * synthesised something listable".
     */
    fun readAppManifestOnly(id: String): ProjectManifest? {
        val dir = File(rootDir, id)
        if (!dir.isDirectory) return null
        return decodeAppManifest(File(dir, APP_MANIFEST_FILE_NAME))
    }

    // ── reading ────────────────────────────────────────────────────────────

    private fun readProject(dir: File): Project? {
        // 1. The app's own file. The normal path, and the only one the engine
        //    can never touch.
        decodeAppManifest(File(dir, APP_MANIFEST_FILE_NAME))?.let {
            return Project(id = dir.name, directory = dir, manifest = it)
        }

        val engineManifestFile = File(dir, ENGINE_MANIFEST_FILE_NAME)
        if (!engineManifestFile.isFile) return null
        val raw = runCatching { engineManifestFile.readText() }.getOrNull() ?: return null

        // 2. Legacy: a pre-ROUND-6 project whose manifest.json is still the
        //    app's, because no capture has ever been recorded into it. Read it,
        //    and migrate it out of the engine's way immediately — the next
        //    Start would otherwise overwrite it.
        decodeAppManifest(raw)?.let { legacy ->
            if (writeAppManifest(dir, legacy)) {
                onDiagnostic("migrated legacy manifest.json -> $APP_MANIFEST_FILE_NAME for ${dir.name}")
            }
            return Project(id = dir.name, directory = dir, manifest = legacy)
        }

        // 3. Recovery: manifest.json is the ENGINE's, so this is a capture that
        //    the pre-ROUND-6 filename collision made invisible. Rebuild what is
        //    genuinely knowable and say so.
        val recovered = recoverFromEngineManifest(dir, raw) ?: return null
        if (writeAppManifest(dir, recovered)) {
            onDiagnostic(
                "RECOVERED project ${dir.name} from an engine-written manifest.json " +
                    "(pre-0.3.0 filename collision) — name=\"${recovered.name}\"",
            )
        }
        return Project(id = dir.name, directory = dir, manifest = recovered)
    }

    private fun decodeAppManifest(file: File): ProjectManifest? {
        if (!file.isFile) return null
        val text = runCatching { file.readText() }.getOrNull() ?: return null
        return decodeAppManifest(text)
    }

    private fun decodeAppManifest(text: String): ProjectManifest? =
        runCatching { json.decodeFromString(ProjectManifest.serializer(), text) }.getOrNull()

    /**
     * Rebuilds an app manifest from an engine-written `manifest.json`.
     *
     * Every value below is either read out of the engine's own document or
     * derived from the directory name the app itself chose at creation time —
     * nothing is invented. What genuinely cannot be recovered (the sensor: the
     * engine's `sensors` array is never populated, `add_sensor()` has no caller)
     * is inferred from which stream files the capture actually wrote, and
     * failing that left at the phone-only sensor, with
     * [ProjectManifest.recovered] set so the UI never claims more than it knows.
     */
    private fun recoverFromEngineManifest(dir: File, raw: String): ProjectManifest? {
        val engine = runCatching {
            json.decodeFromString(EngineLscanManifest.serializer(), raw)
        }.getOrNull() ?: return null
        if (!engine.looksLikeEngineManifest) return null

        val createdMillis = if (engine.createdAtUtcNs > 0L) {
            engine.createdAtUtcNs / 1_000_000L
        } else {
            runCatching { dir.lastModified() }.getOrDefault(0L).takeIf { it > 0L } ?: clock()
        }

        return ProjectManifest(
            name = nameFromDirectory(dir.name),
            sensor = sensorFor(dir, engine),
            profile = profileFromEngineString(engine.profile),
            createdAtEpochMillis = createdMillis,
            appVersion = engine.engineVersion.ifEmpty { appVersion },
            recovered = true,
        )
    }

    /**
     * `scan-014-2026-08-17-1932-a1b2c3.lscan` → `Scan-014-2026-08-17-1932`.
     *
     * [uniqueDirName] built the directory name from the project name by
     * lower-casing, slugifying and appending a 6-hex uniquifier, so stripping
     * the uniquifier and restoring the leading capital gets the auto-named
     * `Scan-<series>-<date>-<time>` back verbatim. A hand-typed name comes back
     * lower-cased with spaces as hyphens, which is lossy but recognisable — and
     * the alternative is calling a real capture "Untitled".
     */
    private fun nameFromDirectory(dirName: String): String {
        val withoutSuffix = dirName.removeSuffix(PROJECT_SUFFIX)
        val stripped = withoutSuffix.substringBeforeLast('-', missingDelimiterValue = withoutSuffix)
            .ifEmpty { withoutSuffix }
        return stripped.replaceFirstChar { it.uppercaseChar() }
    }

    /**
     * The engine records D6 and Mid-360 returns into the same
     * `streams/lidar.bin` (`stream_file_of()` maps both), so the stream files
     * cannot tell the two apart. What CAN: a Mid-360 is the only source of an
     * IMU stream (`streams/imu.bin`), because the D6 has no IMU at all — which
     * is the very fact ROUND 5 item 11 is built on.
     */
    private fun sensorFor(dir: File, engine: EngineLscanManifest): SensorType {
        val streams = File(dir, "streams")
        val hasImu = File(streams, "imu.bin").isFile ||
            engine.streamFiles.keys.any { it.equals("imu.bin", ignoreCase = true) }
        return if (hasImu) SensorType.MID360 else SensorType.COIN_D6
    }

    private fun profileFromEngineString(profile: String): WorkflowProfile =
        WorkflowProfile.entries.firstOrNull {
            CaptureDefaults.engineProfileString(it).equals(profile, ignoreCase = true)
        } ?: WorkflowProfile.QUICK_SCAN

    // ── writing ────────────────────────────────────────────────────────────

    /**
     * Temp-file + rename, and it returns whether it worked.
     *
     * A manifest truncated by a kill mid-write would make the whole project
     * unreadable, which is a much worse outcome than losing the last edit. The
     * `Boolean` (rather than the old silent best-effort) is ROUND 6's other
     * half: a capture's seal has to be able to say out loud that it failed.
     */
    private fun writeAppManifest(dir: File, manifest: ProjectManifest): Boolean {
        val target = File(dir, APP_MANIFEST_FILE_NAME)
        val tmp = File(dir, "$APP_MANIFEST_FILE_NAME.tmp")
        return runCatching {
            tmp.writeText(json.encodeToString(ProjectManifest.serializer(), manifest))
            if (!tmp.renameTo(target)) {
                target.delete()
                check(tmp.renameTo(target)) { "rename ${tmp.name} -> ${target.name} failed" }
            }
            true
        }.getOrElse { e ->
            onDiagnostic("manifest write FAILED for ${dir.name}: ${e.javaClass.simpleName}: ${e.message}")
            tmp.delete()
            false
        }
    }

    private fun uniqueDirName(name: String): String {
        val slug = slugify(name)
        while (true) {
            val candidate = "$slug-${randomSuffix()}$PROJECT_SUFFIX"
            if (!File(rootDir, candidate).exists()) return candidate
        }
    }

    private fun randomSuffix(): String = buildString {
        repeat(6) { append(HEX_CHARS[Random.nextInt(HEX_CHARS.length)]) }
    }

    private fun slugify(name: String): String {
        val slug = name.lowercase()
            .map { c -> if (c.isLetterOrDigit()) c else '-' }
            .joinToString("")
            .trim('-')
            .replace(Regex("-{2,}"), "-")
        return slug.ifEmpty { "project" }
    }

    companion object {
        /**
         * The ENGINE's file (Tech Spec §3.11, `engine/include/scanengine/record/lscan.h`'s
         * `kManifestFile`). The app reads it for legacy/recovery only and never
         * writes it — see this class's header for the bug that rule exists to
         * close.
         */
        const val ENGINE_MANIFEST_FILE_NAME = "manifest.json"

        /** The app's own project record. Not a name the engine has any concept of. */
        const val APP_MANIFEST_FILE_NAME = "project.json"

        @Deprecated(
            "Ambiguous since ROUND 6 — say which manifest you mean.",
            ReplaceWith("APP_MANIFEST_FILE_NAME"),
        )
        const val MANIFEST_FILE_NAME = APP_MANIFEST_FILE_NAME

        private const val PROJECT_SUFFIX = ".lscan"
        private const val HEX_CHARS = "0123456789abcdef"
    }
}
