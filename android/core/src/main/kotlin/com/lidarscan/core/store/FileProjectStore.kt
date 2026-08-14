package com.lidarscan.core.store

import com.lidarscan.core.model.ProjectManifest
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
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
 * @param appVersion stamped into every manifest's `appVersion` field; passed
 *   in rather than read here because `:core` has no `BuildConfig` of its own.
 */
class FileProjectStore(
    private val rootDir: File,
    private val appVersion: String,
    private val clock: () -> Long = System::currentTimeMillis,
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

        val manifest = ProjectManifest(
            name = name,
            sensor = sensor,
            profile = profile,
            createdAtEpochMillis = clock(),
            appVersion = appVersion,
        )
        writeManifest(dir, manifest)
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

    private fun readProject(dir: File): Project? {
        val manifestFile = File(dir, MANIFEST_FILE_NAME)
        if (!manifestFile.isFile) return null
        val manifest = runCatching { json.decodeFromString(ProjectManifest.serializer(), manifestFile.readText()) }
            .getOrNull() ?: return null
        return Project(id = dir.name, directory = dir, manifest = manifest)
    }

    private fun writeManifest(dir: File, manifest: ProjectManifest) {
        File(dir, MANIFEST_FILE_NAME).writeText(json.encodeToString(ProjectManifest.serializer(), manifest))
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
        const val MANIFEST_FILE_NAME = "manifest.json"
        private const val PROJECT_SUFFIX = ".lscan"
        private const val HEX_CHARS = "0123456789abcdef"
    }
}
